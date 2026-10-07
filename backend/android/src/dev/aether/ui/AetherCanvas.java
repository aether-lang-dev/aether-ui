// AetherCanvas — the canvas widget's View.
//
// The canvas's drawing is a command buffer kept in native code
// (backend/aether_ui_android.c), replayed through android.graphics.Canvas
// into a Bitmap the size of this View -- the retained paint surface, the
// counterpart of GTK4's -- which onDraw then shows. This class only carries
// things across: a paint (onDraw), a size change, and the raw input -- the
// touch stream (with up to two pointers, for the gesture probe), the mouse
// hover and wheel, and hardware keys. What any of it MEANS (which closure
// runs, in which units, with which key name) is decided in C, like every
// other event of this backend's.
package dev.aether.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

final class AetherCanvas extends View {
    // Event kinds; the same numbers are AEUI_CV_* in aether_ui_android.c.
    static final int DOWN = 1, MOVE = 2, UP = 3, CANCEL = 4,
                     POINTER_DOWN = 5, POINTER_UP = 6,
                     HOVER = 7,        // a pointer moving with no button held
                     SCROLL = 8,       // x0 = AXIS_HSCROLL, y0 = AXIS_VSCROLL
                     SIZE = 9,         // x0, y0 = the new size in pixels
                     KEY_DOWN = 10, KEY_UP = 11;   // count = key code, x0 = unicode char

    private final int handle;

    AetherCanvas(Context context, int handle) {
        super(context);
        this.handle = handle;
    }

    // Natives, bound in JNI_OnLoad.
    static native void nativeCanvasDraw(int handle, Canvas canvas, int width, int height);
    // true = a handler took the event.
    static native boolean nativeCanvasEvent(int handle, int kind, int count,
                                            float x0, float y0, float x1, float y1, int meta);

    // The size asked for at creation is a floor the layout measures against,
    // not "all the space offered" (View's default answer to WRAP_CONTENT).
    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(resolveSize(getSuggestedMinimumWidth(), widthSpec),
                             resolveSize(getSuggestedMinimumHeight(), heightSpec));
    }

    @Override protected void onDraw(Canvas canvas) {
        nativeCanvasDraw(handle, canvas, getWidth(), getHeight());
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        nativeCanvasEvent(handle, SIZE, 0, w, h, 0, 0, 0);
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int kind;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: kind = DOWN; break;
            case MotionEvent.ACTION_MOVE: kind = MOVE; break;
            case MotionEvent.ACTION_UP: kind = UP; break;
            case MotionEvent.ACTION_CANCEL: kind = CANCEL; break;
            case MotionEvent.ACTION_POINTER_DOWN: kind = POINTER_DOWN; break;
            case MotionEvent.ACTION_POINTER_UP: kind = POINTER_UP; break;
            default: return false;
        }
        int n = e.getPointerCount();
        float x1 = n > 1 ? e.getX(1) : 0, y1 = n > 1 ? e.getY(1) : 0;
        boolean took = nativeCanvasEvent(handle, kind, n, e.getX(0), e.getY(0), x1, y1, e.getMetaState());
        if (took && kind == DOWN && isFocusable()) requestFocus();
        return took;
    }

    @Override public boolean onHoverEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_HOVER_MOVE)
            nativeCanvasEvent(handle, HOVER, 1, e.getX(), e.getY(), 0, 0, e.getMetaState());
        return super.onHoverEvent(e);
    }

    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_SCROLL
                && nativeCanvasEvent(handle, SCROLL, 1, e.getAxisValue(MotionEvent.AXIS_HSCROLL),
                                     e.getAxisValue(MotionEvent.AXIS_VSCROLL), 0, 0, e.getMetaState()))
            return true;
        return super.onGenericMotionEvent(e);
    }

    @Override public boolean onKeyDown(int code, KeyEvent e) {
        if (nativeCanvasEvent(handle, KEY_DOWN, code, e.getUnicodeChar(), 0, 0, 0, e.getMetaState()))
            return true;
        return super.onKeyDown(code, e);
    }

    @Override public boolean onKeyUp(int code, KeyEvent e) {
        if (nativeCanvasEvent(handle, KEY_UP, code, e.getUnicodeChar(), 0, 0, 0, e.getMetaState()))
            return true;
        return super.onKeyUp(code, e);
    }
}
