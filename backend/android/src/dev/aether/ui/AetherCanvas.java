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
import android.text.InputType;
import android.view.GestureDetector;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import java.nio.charset.StandardCharsets;

final class AetherCanvas extends View {
    // Event kinds; the same numbers are AEUI_CV_* in aether_ui_android.c.
    static final int DOWN = 1, MOVE = 2, UP = 3, CANCEL = 4,
                     POINTER_DOWN = 5, POINTER_UP = 6,
                     HOVER = 7,        // a pointer moving with no button held
                     SCROLL = 8,       // x0 = AXIS_HSCROLL, y0 = AXIS_VSCROLL
                     SIZE = 9,         // x0, y0 = the new size in pixels
                     KEY_DOWN = 10, KEY_UP = 11,   // count = key code, x0 = unicode char
                     DOUBLE_TAP = 12,  // the second tap of a double tap (x0, y0)
                     SECONDARY = 13;   // a right click: a mouse's secondary button
                                       // released, or a long press (x0, y0)

    private final int handle;
    // The platform's own double-tap and long-press timing (ViewConfiguration),
    // fed from onTouchEvent. It only REPORTS: the touch stream still goes to
    // native code untouched, so press/move/release keep working.
    private final GestureDetector gestures;

    AetherCanvas(Context context, int handle) {
        super(context);
        this.handle = handle;
        gestures = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent e) { return true; }
            @Override public boolean onDoubleTap(MotionEvent e) {
                return nativeCanvasEvent(AetherCanvas.this.handle, DOUBLE_TAP, 1,
                                         e.getX(), e.getY(), 0, 0, e.getMetaState());
            }
            @Override public void onLongPress(MotionEvent e) {
                nativeCanvasEvent(AetherCanvas.this.handle, SECONDARY, 1,
                                  e.getX(), e.getY(), 0, 0, e.getMetaState());
            }
        });
    }

    // A mouse's secondary button, on its release (the context-menu
    // convention). Android delivers it as ACTION_BUTTON_RELEASE, through
    // onTouchEvent while the button is down and onGenericMotionEvent
    // otherwise, so both look for it.
    private boolean secondaryRelease(MotionEvent e) {
        if (e.getActionMasked() != MotionEvent.ACTION_BUTTON_RELEASE) return false;
        if (e.getActionButton() != MotionEvent.BUTTON_SECONDARY) return false;
        return nativeCanvasEvent(handle, SECONDARY, 1, e.getX(), e.getY(), 0, 0, e.getMetaState());
    }

    // Natives, bound in JNI_OnLoad.
    static native void nativeCanvasDraw(int handle, Canvas canvas, int width, int height);
    // true = a handler took the event.
    static native boolean nativeCanvasEvent(int handle, int kind, int count,
                                            float x0, float y0, float x1, float y1, int meta);
    // Text committed to the canvas (canvas_on_text), as UTF-8: a JNI string
    // would carry an astral character as two surrogates (modified UTF-8).
    static native void nativeCanvasText(int handle, byte[] utf8);

    // Text input (canvas_on_text, #214). While the canvas takes text it is a
    // text editor to the input method -- the software keyboard types into
    // it, through commitText -- and a hardware key's character (with a dead
    // key composed in) is its text too. Control characters stay keys.
    private boolean takesText;
    private int deadAccent;
    public void setTakesText(boolean on) { takesText = on; }

    private void text(String s) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); ) {
            int c = s.codePointAt(i);
            i += Character.charCount(c);
            if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) continue;
            b.appendCodePoint(c);
        }
        if (b.length() > 0) nativeCanvasText(handle, b.toString().getBytes(StandardCharsets.UTF_8));
    }

    // A hardware key's text: what it types with the meta state, a dead key
    // (COMBINING_ACCENT) held over to compose with the next. true = text.
    private boolean typed(KeyEvent e) {
        int u = e.getUnicodeChar();
        if (u == 0) return false;
        if ((u & KeyCharacterMap.COMBINING_ACCENT) != 0) {
            deadAccent = u & KeyCharacterMap.COMBINING_ACCENT_MASK;
            return true;
        }
        if (deadAccent != 0) {
            int c = KeyCharacterMap.getDeadChar(deadAccent, u);
            deadAccent = 0;
            if (c != 0) u = c;
        }
        if (u < 0x20 || u == 0x7F) return false;
        text(new String(Character.toChars(u)));
        return true;
    }

    @Override public boolean onCheckIsTextEditor() { return takesText; }

    @Override public InputConnection onCreateInputConnection(EditorInfo out) {
        if (!takesText) return null;
        out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        out.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        return new BaseInputConnection(this, false) {
            @Override public boolean commitText(CharSequence t, int newCursorPosition) {
                if (t != null && t.length() > 0) text(t.toString());
                return true;
            }
            // A soft keyboard's delete: the BackSpace key, as a hardware one.
            @Override public boolean deleteSurroundingText(int before, int after) {
                for (int i = 0; i < before; i++) {
                    sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL));
                    sendKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL));
                }
                return true;
            }
        };
    }

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

    // click_through: the touch goes to the view beneath, as a container's does.
    private boolean passThrough;
    public void setPassThrough(boolean on) { passThrough = on; }

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (passThrough) return false;
        if (secondaryRelease(e)) return true;
        // A secondary-button press is a right click, not a touch: keep it out
        // of the press/long-press stream.
        if ((e.getButtonState() & MotionEvent.BUTTON_SECONDARY) == 0) gestures.onTouchEvent(e);
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
        if (secondaryRelease(e)) return true;
        if (e.getActionMasked() == MotionEvent.ACTION_SCROLL
                && nativeCanvasEvent(handle, SCROLL, 1, e.getAxisValue(MotionEvent.AXIS_HSCROLL),
                                     e.getAxisValue(MotionEvent.AXIS_VSCROLL), 0, 0, e.getMetaState()))
            return true;
        return super.onGenericMotionEvent(e);
    }

    @Override public boolean onKeyDown(int code, KeyEvent e) {
        boolean took = nativeCanvasEvent(handle, KEY_DOWN, code, e.getUnicodeChar(), 0, 0, 0, e.getMetaState());
        if (takesText && typed(e)) took = true;
        if (took) return true;
        return super.onKeyDown(code, e);
    }

    @Override public boolean onKeyUp(int code, KeyEvent e) {
        if (nativeCanvasEvent(handle, KEY_UP, code, e.getUnicodeChar(), 0, 0, 0, e.getMetaState()))
            return true;
        return super.onKeyUp(code, e);
    }
}
