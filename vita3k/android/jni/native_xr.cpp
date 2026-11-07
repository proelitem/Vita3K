// Quest: the game as a big screen in an OpenXR session, which is also the only way to read the Touch controllers.
// Same design as our Cemu's NativeXr.cpp: Vulkan draws into a Surface owned by a swapchain of the compositor.
#include "android_state.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include <emuenv/state.h>
#include <touch/functions.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define XR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "Vita3KXr", __VA_ARGS__)

namespace {

constexpr int SCREEN_WIDTH = 1920; // twice the Vita's 960x544
constexpr int SCREEN_HEIGHT = 1088;
constexpr float SCREEN_WIDTH_METERS = 3.2f;
constexpr float SCREEN_DISTANCE_METERS = 2.2f;
constexpr float POINTER_METERS = 0.04f;
constexpr auto QUIT_HOLD = std::chrono::milliseconds(1500);

struct XrButton {
    const char *name;
    const char *path;
    XrAction action = XR_NULL_HANDLE;
};

XrButton s_buttons[] = {
    { "a", "/user/hand/right/input/a/click" },
    { "b", "/user/hand/right/input/b/click" },
    { "x", "/user/hand/left/input/x/click" },
    { "y", "/user/hand/left/input/y/click" },
    { "stick_l", "/user/hand/left/input/thumbstick/click" },
    { "stick_r", "/user/hand/right/input/thumbstick/click" },
};

// Tremu's "console controls" page (padmap.h): the Touch inputs in its order, each set to a RetroPad
// button (RETRO_DEVICE_ID_JOYPAD_*), or to one of these.
enum { TI_A, TI_B, TI_X, TI_Y, TI_LGRIP, TI_RGRIP, TI_LTRIGGER, TI_RTRIGGER, TI_RS_UP, TI_RS_DOWN, TI_RS_LEFT, TI_RS_RIGHT, TI_LCLICK, TI_RCLICK, TI_COUNT };
enum { PAD_NOTHING = -1, PAD_RIGHT_ANALOG = 100, PAD_TOUCH_FRONT = 101, PAD_TOUCH_REAR = 102 };
enum { RP_B, RP_Y, RP_SELECT, RP_START, RP_UP, RP_DOWN, RP_LEFT, RP_RIGHT, RP_A, RP_X, RP_L, RP_R, RP_COUNT = 12 };
// The same defaults as Tremu's table, for a launch without settings.
int s_map[TI_COUNT] = { RP_B, RP_A, RP_Y, RP_X, PAD_TOUCH_REAR, PAD_TOUCH_FRONT, RP_L, RP_R, PAD_RIGHT_ANALOG, PAD_RIGHT_ANALOG, PAD_RIGHT_ANALOG, PAD_RIGHT_ANALOG, PAD_TOUCH_REAR, RP_START };
const int RETROPAD_TO_SDL[RP_COUNT] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
};

XrInstance s_instance = XR_NULL_HANDLE;
XrSession s_session = XR_NULL_HANDLE;
XrSpace s_space = XR_NULL_HANDLE, s_aimSpace = XR_NULL_HANDLE;
XrSwapchain s_swapchain = XR_NULL_HANDLE, s_pointerChain = XR_NULL_HANDLE;
XrActionSet s_actionSet = XR_NULL_HANDLE;
XrAction s_triggerL, s_triggerR, s_gripL, s_gripR, s_stickL, s_stickR, s_menu, s_aim;
XrSessionState s_state = XR_SESSION_STATE_UNKNOWN;
bool s_running = false;
JavaVM *s_vm = nullptr;
jobject s_activity = nullptr;
jobject s_surface = nullptr;
bool s_startFinished = false;
std::mutex s_startMutex;
std::condition_variable s_startDone;
bool s_fingerDown = false;
XrVector3f s_pointer{};
bool s_pointerOnScreen = false;

XrPath Path(const char *text) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(s_instance, text, &path);
    return path;
}

bool CreateAction(XrAction &action, const char *name, XrActionType type) {
    XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
    info.actionType = type;
    snprintf(info.actionName, sizeof(info.actionName), "%s", name);
    snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", name);
    return XR_SUCCEEDED(xrCreateAction(s_actionSet, &info, &action));
}

bool InitActions() {
    XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(setInfo.actionSetName, "vita");
    strcpy(setInfo.localizedActionSetName, "Vita");
    if (XR_FAILED(xrCreateActionSet(s_instance, &setInfo, &s_actionSet)))
        return false;

    std::vector<XrActionSuggestedBinding> bindings;
    for (auto &button : s_buttons) {
        if (!CreateAction(button.action, button.name, XR_ACTION_TYPE_BOOLEAN_INPUT))
            return false;
        bindings.push_back({ button.action, Path(button.path) });
    }
    if (!CreateAction(s_triggerL, "trigger_l", XR_ACTION_TYPE_FLOAT_INPUT) || !CreateAction(s_triggerR, "trigger_r", XR_ACTION_TYPE_FLOAT_INPUT)
        || !CreateAction(s_gripL, "grip_l", XR_ACTION_TYPE_FLOAT_INPUT) || !CreateAction(s_gripR, "grip_r", XR_ACTION_TYPE_FLOAT_INPUT)
        || !CreateAction(s_stickL, "stick_axis_l", XR_ACTION_TYPE_VECTOR2F_INPUT) || !CreateAction(s_stickR, "stick_axis_r", XR_ACTION_TYPE_VECTOR2F_INPUT)
        || !CreateAction(s_menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT) || !CreateAction(s_aim, "aim", XR_ACTION_TYPE_POSE_INPUT))
        return false;
    bindings.push_back({ s_triggerL, Path("/user/hand/left/input/trigger/value") });
    bindings.push_back({ s_triggerR, Path("/user/hand/right/input/trigger/value") });
    bindings.push_back({ s_gripL, Path("/user/hand/left/input/squeeze/value") });
    bindings.push_back({ s_gripR, Path("/user/hand/right/input/squeeze/value") });
    bindings.push_back({ s_stickL, Path("/user/hand/left/input/thumbstick") });
    bindings.push_back({ s_stickR, Path("/user/hand/right/input/thumbstick") });
    bindings.push_back({ s_menu, Path("/user/hand/left/input/menu/click") });
    bindings.push_back({ s_aim, Path("/user/hand/right/input/aim/pose") });

    XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggested.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
    suggested.suggestedBindings = bindings.data();
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    if (XR_FAILED(xrSuggestInteractionProfileBindings(s_instance, &suggested)))
        return false;

    XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &s_actionSet;
    if (XR_FAILED(xrAttachSessionActionSets(s_session, &attach)))
        return false;

    XrActionSpaceCreateInfo aim{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    aim.action = s_aim;
    aim.poseInActionSpace.orientation.w = 1;
    return XR_SUCCEEDED(xrCreateActionSpace(s_session, &aim, &s_aimSpace));
}

// The dot that shows where the right controller points: a small swapchain painted once.
bool InitPointer() {
    XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = GL_RGBA8;
    info.sampleCount = 1;
    info.width = info.height = 4;
    info.faceCount = info.arraySize = info.mipCount = 1;
    info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    if (XR_FAILED(xrCreateSwapchain(s_session, &info, &s_pointerChain)))
        return false;
    uint32_t count = 0;
    xrEnumerateSwapchainImages(s_pointerChain, 0, &count, nullptr);
    std::vector<XrSwapchainImageOpenGLESKHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
    xrEnumerateSwapchainImages(s_pointerChain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
    uint32_t index = 0;
    xrAcquireSwapchainImage(s_pointerChain, nullptr, &index);
    XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s_pointerChain, &wait);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[index].image, 0);
    glClearColor(1.0f, 0.25f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    xrReleaseSwapchainImage(s_pointerChain, nullptr);
    return true;
}

bool InitXr(JNIEnv *env) {
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction *>(&initLoader));
    if (initLoader == nullptr)
        return false;
    XrLoaderInitInfoAndroidKHR loaderInfo{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    loaderInfo.applicationVM = s_vm;
    loaderInfo.applicationContext = s_activity;
    initLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR *>(&loaderInfo));

    const char *extensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
        // A Vulkan image in that swapchain reaches the compositor upside down.
        XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME,
        XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME,
    };
    XrInstanceCreateInfoAndroidKHR androidInfo{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    androidInfo.applicationVM = s_vm;
    androidInfo.applicationActivity = s_activity;
    XrInstanceCreateInfo instanceInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.next = &androidInfo;
    strcpy(instanceInfo.applicationInfo.applicationName, "Vita3K");
    instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    instanceInfo.enabledExtensionCount = 5;
    instanceInfo.enabledExtensionNames = extensions;
    XrResult result = xrCreateInstance(&instanceInfo, &s_instance);
    if (XR_FAILED(result)) {
        XR_LOG("xrCreateInstance failed: %d", result);
        return false;
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system;
    if (XR_FAILED(xrGetSystem(s_instance, &systemInfo, &system)))
        return false;

    // The session needs a GL context even though Vulkan draws everything through the Surface.
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, nullptr, nullptr);
    const EGLint configAttribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig config;
    EGLint configCount = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &configCount) || configCount < 1)
        return false;
    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    const EGLint surfaceAttribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface pbuffer = eglCreatePbufferSurface(display, config, surfaceAttribs);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, pbuffer, pbuffer, context))
        return false;

    PFN_xrGetOpenGLESGraphicsRequirementsKHR glesRequirements = nullptr;
    xrGetInstanceProcAddr(s_instance, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&glesRequirements));
    XrGraphicsRequirementsOpenGLESKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    glesRequirements(s_instance, system, &requirements);

    XrGraphicsBindingOpenGLESAndroidKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    binding.display = display;
    binding.config = config;
    binding.context = context;
    XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &binding;
    sessionInfo.systemId = system;
    result = xrCreateSession(s_instance, &sessionInfo, &s_session);
    if (XR_FAILED(result)) {
        XR_LOG("xrCreateSession failed: %d", result);
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    if (XR_FAILED(xrCreateReferenceSpace(s_session, &spaceInfo, &s_space)) || !InitActions() || !InitPointer())
        return false;

    PFN_xrCreateSwapchainAndroidSurfaceKHR createSurface = nullptr;
    xrGetInstanceProcAddr(s_instance, "xrCreateSwapchainAndroidSurfaceKHR", reinterpret_cast<PFN_xrVoidFunction *>(&createSurface));
    if (createSurface == nullptr)
        return false;
    // Quest rejects any format, usage or count here (tested with Cemu): the Surface's producer decides them.
    XrSwapchainCreateInfo chainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    chainInfo.width = SCREEN_WIDTH;
    chainInfo.height = SCREEN_HEIGHT;
    jobject surface = nullptr;
    result = createSurface(s_session, &chainInfo, &s_swapchain, &surface);
    if (XR_FAILED(result) || surface == nullptr) {
        XR_LOG("android surface swapchain failed: %d", result);
        return false;
    }
    s_surface = env->NewGlobalRef(surface);
    return true;
}

float GetFloat(XrAction action) {
    XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = action;
    XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
    xrGetActionStateFloat(s_session, &info, &state);
    return state.isActive ? state.currentState : 0.0f;
}

bool GetBool(XrAction action) {
    XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = action;
    XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
    xrGetActionStateBoolean(s_session, &info, &state);
    return state.isActive && state.currentState;
}

XrVector2f GetStick(XrAction action) {
    XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = action;
    XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
    xrGetActionStateVector2f(s_session, &info, &state);
    return state.isActive ? state.currentState : XrVector2f{ 0, 0 };
}

int16_t AxisValue(float value) {
    return static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
}

void RequestQuit(JNIEnv *env) {
    jclass activityClass = env->GetObjectClass(s_activity);
    env->CallVoidMethod(s_activity, env->GetMethodID(activityClass, "onXrQuit", "()V"));
    env->DeleteLocalRef(activityClass);
}

// Where the right controller's ray meets the screen; the Vita's touch screen follows it.
void UpdatePointer(XrTime time) {
    XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
    s_pointerOnScreen = false;
    if (XR_FAILED(xrLocateSpace(s_aimSpace, s_space, time, &location))
        || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return;
    const XrQuaternionf q = location.pose.orientation;
    // The aim pose looks along -Z: that axis rotated by q.
    const XrVector3f forward{ -2 * (q.x * q.z + q.w * q.y), -2 * (q.y * q.z - q.w * q.x), -(1 - 2 * (q.x * q.x + q.y * q.y)) };
    if (forward.z >= -0.01f)
        return;
    const float distance = (-SCREEN_DISTANCE_METERS - location.pose.position.z) / forward.z;
    s_pointer = { location.pose.position.x + forward.x * distance, location.pose.position.y + forward.y * distance, -SCREEN_DISTANCE_METERS };
    const float halfW = SCREEN_WIDTH_METERS / 2, halfH = halfW * SCREEN_HEIGHT / SCREEN_WIDTH;
    s_pointerOnScreen = std::abs(s_pointer.x) <= halfW && std::abs(s_pointer.y) <= halfH;
}

void SendFinger(SDL_EventType type) {
    const float halfW = SCREEN_WIDTH_METERS / 2, halfH = halfW * SCREEN_HEIGHT / SCREEN_WIDTH;
    SDL_Event event{};
    event.type = type;
    event.tfinger.timestamp = SDL_GetTicksNS();
    event.tfinger.touchID = 1;
    event.tfinger.fingerID = 1;
    event.tfinger.x = std::clamp((s_pointer.x + halfW) / (2 * halfW), 0.0f, 1.0f);
    event.tfinger.y = std::clamp((halfH - s_pointer.y) / (2 * halfH), 0.0f, 1.0f);
    event.tfinger.pressure = 1.0f;
    SDL_PushEvent(&event);
}

void UpdateTouch(bool front, bool rear) {
    const bool pressed = front || rear;
    auto *emuenv = get_emuenv();
    if (emuenv && pressed && !s_fingerDown)
        set_rear_touchscreen(emuenv->touch, rear && !front);
    // The rear pad is felt, not aimed at: pointing away from the screen touches its middle.
    if (rear && !front && !s_pointerOnScreen)
        s_pointer = { 0, 0, -SCREEN_DISTANCE_METERS };
    if (pressed && (s_pointerOnScreen || (rear && !front))) {
        SendFinger(s_fingerDown ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_FINGER_DOWN);
        s_fingerDown = true;
    } else if (s_fingerDown) {
        SendFinger(SDL_EVENT_FINGER_UP);
        s_fingerDown = false;
    }
}

// The left menu button is Start, with the left grip held Select, and held down it leaves the game.
void UpdateInput(JNIEnv *env, XrTime time) {
    XrActiveActionSet active{ s_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(s_session, &sync)))
        return;

    UpdatePointer(time);

    auto *controller = get_app_session_controller();
    const bool session = controller && controller->has_active_session() && SDL_WasInit(SDL_INIT_GAMEPAD);
    if (session && !overlay_virtual_controller_attached())
        attach_overlay_virtual_controller();

    const XrVector2f left = GetStick(s_stickL), right = GetStick(s_stickR);
    const bool vertical = std::abs(right.y) > std::abs(right.x);
    const float gripL = GetFloat(s_gripL);
    const bool in[TI_COUNT] = {
        GetBool(s_buttons[0].action), GetBool(s_buttons[1].action), GetBool(s_buttons[2].action), GetBool(s_buttons[3].action),
        gripL > 0.5f, GetFloat(s_gripR) > 0.5f, GetFloat(s_triggerL) > 0.5f, GetFloat(s_triggerR) > 0.5f,
        right.y > 0.5f && vertical, right.y < -0.5f && vertical, right.x < -0.5f && !vertical, right.x > 0.5f && !vertical,
        GetBool(s_buttons[4].action), GetBool(s_buttons[5].action),
    };
    bool pressed[RP_COUNT] = {};
    bool front = false, rear = false;
    for (int i = 0; i < TI_COUNT; i++) {
        if (!in[i])
            continue;
        if (s_map[i] >= 0 && s_map[i] < RP_COUNT)
            pressed[s_map[i]] = true;
        front = front || s_map[i] == PAD_TOUCH_FRONT;
        rear = rear || s_map[i] == PAD_TOUCH_REAR;
    }
    UpdateTouch(front, rear);

    const bool menu = GetBool(s_menu);
    pressed[RP_START] = pressed[RP_START] || (menu && gripL <= 0.5f);
    pressed[RP_SELECT] = pressed[RP_SELECT] || (menu && gripL > 0.5f);
    for (int i = 0; i < RP_COUNT; i++)
        overlay_set_button(RETROPAD_TO_SDL[i], pressed[i]);
    { // Which Touch inputs press which buttons, logged when they change: the first thing to read when a control feels wrong.
        static uint32_t s_inputs_logged;
        uint32_t inputs = 0;
        for (int i = 0; i < TI_COUNT; i++)
            inputs |= in[i] ? 1u << i : 0;
        if (inputs != s_inputs_logged) {
            s_inputs_logged = inputs;
            uint32_t buttons = 0;
            for (int i = 0; i < RP_COUNT; i++)
                buttons |= pressed[i] ? 1u << i : 0;
            XR_LOG("Touch inputs 0x%x -> buttons 0x%x, front %d rear %d, controller attached %d", inputs, buttons, front, rear, overlay_virtual_controller_attached());
        }
    }
    float analogX = 0, analogY = 0;
    if ((right.y > 0 && s_map[TI_RS_UP] == PAD_RIGHT_ANALOG) || (right.y < 0 && s_map[TI_RS_DOWN] == PAD_RIGHT_ANALOG))
        analogY = right.y;
    if ((right.x < 0 && s_map[TI_RS_LEFT] == PAD_RIGHT_ANALOG) || (right.x > 0 && s_map[TI_RS_RIGHT] == PAD_RIGHT_ANALOG))
        analogX = right.x;
    overlay_set_axis(SDL_GAMEPAD_AXIS_LEFTX, AxisValue(left.x));
    overlay_set_axis(SDL_GAMEPAD_AXIS_LEFTY, AxisValue(-left.y));
    overlay_set_axis(SDL_GAMEPAD_AXIS_RIGHTX, AxisValue(analogX));
    overlay_set_axis(SDL_GAMEPAD_AXIS_RIGHTY, AxisValue(-analogY));

    static std::chrono::steady_clock::time_point s_menuDown;
    static bool s_menuWasDown = false;
    static bool s_quitRequested = false;
    if (menu && !s_menuWasDown)
        s_menuDown = std::chrono::steady_clock::now();
    s_menuWasDown = menu;
    if (menu && !s_quitRequested && std::chrono::steady_clock::now() - s_menuDown > QUIT_HOLD) {
        s_quitRequested = true;
        RequestQuit(env);
    }
}

// Without this request Horizon OS leaves the CPU and GPU low (seen: 1382 and 350 MHz, the game at 10 to 20 fps).
void SetPerformanceLevels() {
    PFN_xrPerfSettingsSetPerformanceLevelEXT setLevel = nullptr;
    xrGetInstanceProcAddr(s_instance, "xrPerfSettingsSetPerformanceLevelEXT", reinterpret_cast<PFN_xrVoidFunction *>(&setLevel));
    if (setLevel == nullptr)
        return;
    setLevel(s_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
    setLevel(s_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
}

// Returns false when the session is over for good.
bool PollEvents() {
    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(s_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            s_state = reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state;
            XR_LOG("session state %d", s_state);
            if (s_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                s_running = XR_SUCCEEDED(xrBeginSession(s_session, &begin));
                SetPerformanceLevels();
            } else if (s_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(s_session);
                s_running = false;
            } else if (s_state == XR_SESSION_STATE_EXITING || s_state == XR_SESSION_STATE_LOSS_PENDING) {
                return false;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            return false;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
    return true;
}

void RenderFrame(JNIEnv *env) {
    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
    if (XR_FAILED(xrWaitFrame(s_session, &waitInfo, &frameState)))
        return;
    XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
    xrBeginFrame(s_session, &beginInfo);

    if (s_state == XR_SESSION_STATE_FOCUSED)
        UpdateInput(env, frameState.predictedDisplayTime);

    XrCompositionLayerImageLayoutFB layout{ XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB };
    layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    quad.next = &layout;
    quad.space = s_space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = s_swapchain;
    quad.subImage.imageRect.extent = { SCREEN_WIDTH, SCREEN_HEIGHT };
    quad.pose.orientation.w = 1;
    quad.pose.position = { 0, 0, -SCREEN_DISTANCE_METERS };
    quad.size = { SCREEN_WIDTH_METERS, SCREEN_WIDTH_METERS * SCREEN_HEIGHT / SCREEN_WIDTH };

    XrCompositionLayerQuad dot{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    dot.space = s_space;
    dot.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    dot.subImage.swapchain = s_pointerChain;
    dot.subImage.imageRect.extent = { 4, 4 };
    dot.pose.orientation.w = 1;
    dot.pose.position = s_pointer;
    dot.size = { POINTER_METERS, POINTER_METERS };

    const XrCompositionLayerBaseHeader *layers[] = { reinterpret_cast<XrCompositionLayerBaseHeader *>(&quad),
        reinterpret_cast<XrCompositionLayerBaseHeader *>(&dot) };
    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = !frameState.shouldRender ? 0 : s_pointerOnScreen ? 2 : 1;
    endInfo.layers = layers;
    xrEndFrame(s_session, &endInfo);
}

void XrThread() {
    JNIEnv *env = nullptr;
    s_vm->AttachCurrentThread(&env, nullptr);
    const bool ready = InitXr(env);
    // One OpenXR instance per process: a failed start must not block the next activity.
    if (!ready && s_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(s_instance);
        s_instance = XR_NULL_HANDLE;
    }
    {
        std::lock_guard lock(s_startMutex);
        s_startFinished = true;
    }
    s_startDone.notify_all();
    auto lastSpeedLog = std::chrono::steady_clock::now();
    while (ready && PollEvents()) {
        if (s_running)
            RenderFrame(env);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (auto *emuenv = get_emuenv(); emuenv && std::chrono::steady_clock::now() - lastSpeedLog > std::chrono::seconds(5)) {
            lastSpeedLog = std::chrono::steady_clock::now();
            XR_LOG("game %u fps (avg %u, min %u), session state %d", emuenv->fps, emuenv->avg_fps, emuenv->min_fps, s_state);
        }
    }
    if (ready)
        RequestQuit(env);
    s_vm->DetachCurrentThread();
}

} // namespace

extern "C" JNIEXPORT jobject JNICALL
Java_org_vita3k_emulator_NativeXr_start(JNIEnv *env, jclass, jobject activity, jintArray pad) {
    env->GetJavaVM(&s_vm);
    if (pad && env->GetArrayLength(pad) == TI_COUNT)
        env->GetIntArrayRegion(pad, 0, TI_COUNT, s_map);
    XR_LOG("Touch map: A %d B %d X %d Y %d, grips %d %d, triggers %d %d, clicks %d %d (from Tremu: %d)", s_map[TI_A], s_map[TI_B], s_map[TI_X], s_map[TI_Y],
        s_map[TI_LGRIP], s_map[TI_RGRIP], s_map[TI_LTRIGGER], s_map[TI_RTRIGGER], s_map[TI_LCLICK], s_map[TI_RCLICK], pad != nullptr);
    s_activity = env->NewGlobalRef(activity);
    s_startFinished = false;
    std::thread(XrThread).detach();
    std::unique_lock lock(s_startMutex);
    s_startDone.wait(lock, [] { return s_startFinished; });
    return s_surface;
}
