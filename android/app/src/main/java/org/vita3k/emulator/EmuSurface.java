package org.vita3k.emulator;

import android.content.Context;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;

import org.libsdl.app.SDLSurface;
import org.vita3k.emulator.overlay.InputOverlay;

public class EmuSurface extends SDLSurface {

    private InputOverlay mOverlay;
    private android.view.Surface xrSurface;

    public InputOverlay getmOverlay() {
        return mOverlay;
    }

    public EmuSurface(Context context){
        super(context);
        mOverlay = new InputOverlay(context);
    }

    /** Quest: SDL draws into the OpenXR session's Surface; the view's own one is never shown. */
    public void useXrSurface(android.view.Surface surface) {
        xrSurface = surface;
        org.libsdl.app.SDLActivity.onNativeSurfaceCreated();
        org.libsdl.app.SDLActivity.nativeSetScreenResolution(NativeXr.SCREEN_WIDTH, NativeXr.SCREEN_HEIGHT, NativeXr.SCREEN_WIDTH, NativeXr.SCREEN_HEIGHT, 1.0f, 72.0f);
        org.libsdl.app.SDLActivity.onNativeResize();
        mIsSurfaceReady = true;
        setSurfaceStatus(true);
        org.libsdl.app.SDLActivity.onNativeSurfaceChanged();
    }

    @Override
    public android.view.Surface getNativeSurface() {
        return xrSurface != null ? xrSurface : super.getNativeSurface();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (xrSurface == null)
            super.surfaceChanged(holder, format, width, height);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (xrSurface != null)
            return;
        setSurfaceStatus(true);
        super.surfaceCreated(holder);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (xrSurface != null)
            return;
        setSurfaceStatus(false);
        super.surfaceDestroyed(holder);
    }

    @Override
    public boolean onTouch(View v, MotionEvent event) {
        if(mOverlay.onTouch(v, event)){
            // priority is given to the overlay
            return true;
        }

        return super.onTouch(v, event);
    }

    public native void setSurfaceStatus(boolean surface_present);
}
