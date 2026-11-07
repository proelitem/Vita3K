package org.vita3k.emulator;

import android.app.Activity;
import android.view.Surface;

/** The OpenXR session of the Quest: the game draws into this Surface, without any SurfaceView. */
public final class NativeXr {
    public static final int SCREEN_WIDTH = 1920;
    public static final int SCREEN_HEIGHT = 1088;

    private NativeXr() {}

    /** Blocks until the session exists; null when OpenXR cannot start. padMap: Tremu's Touch mapping, or null. */
    public static native Surface start(Activity activity, int[] padMap);
}
