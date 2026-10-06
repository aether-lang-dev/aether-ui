// AetherListener — carries a widget's events back to native code.
//
// Native code makes one per (View, kind of event) with the widget's registry
// handle and sets it as that View's listener. Whatever fires it -- a tap, a
// keystroke, a drag of the thumb, the driver's performClick() -- calls the
// one native dispatch, AetherActivity.nativeEvent(handle, kind, a, b, s).
// What the event MEANS (which closure runs, with what, whether it runs at
// all) is decided in C (backend/aether_ui_android.c), where the widget's
// state lives; this class only carries the event across and never
// interprets it.
package dev.aether.ui;

import android.content.Context;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.View;
import android.widget.AdapterView;
import android.widget.CompoundButton;
import android.widget.SeekBar;

final class AetherListener implements View.OnClickListener, TextWatcher,
        CompoundButton.OnCheckedChangeListener, SeekBar.OnSeekBarChangeListener,
        AdapterView.OnItemSelectedListener, View.OnHoverListener,
        View.OnLayoutChangeListener, View.OnTouchListener {
    // Event kinds; the same numbers are AEUI_EV_* in aether_ui_android.c.
    static final int CLICK = 1, TEXT = 2, CHECK = 3, SEEK = 4, SELECT = 5,
                     HOVER = 6, LAYOUT = 7, DOUBLE = 8;

    private final int handle;
    private final int kind;
    private final GestureDetector taps;   // DOUBLE only

    AetherListener(int handle, int kind) {
        this.handle = handle;
        this.kind = kind;
        this.taps = null;
    }

    // A double tap: Android has no double-click event of its own, so the
    // platform's GestureDetector recognises it from the touch stream.
    AetherListener(Context context, int handle) {
        this.handle = handle;
        this.kind = DOUBLE;
        this.taps = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDoubleTap(MotionEvent e) {
                AetherActivity.nativeEvent(handle, DOUBLE, 0, 0, null);
                return true;
            }
        });
    }

    @Override public void onClick(View v) {
        AetherActivity.nativeEvent(handle, CLICK, 0, 0, null);
    }

    @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
    @Override public void onTextChanged(CharSequence s, int start, int before, int count) { }
    @Override public void afterTextChanged(Editable s) {
        AetherActivity.nativeEvent(handle, TEXT, 0, 0, s.toString());
    }

    @Override public void onCheckedChanged(CompoundButton b, boolean checked) {
        AetherActivity.nativeEvent(handle, CHECK, checked ? 1 : 0, 0, null);
    }

    // Only a person's drag: a programmatic setProgress is not a change the
    // app asked to hear about (native code fires the closure itself where
    // the contract says a set does).
    @Override public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
        if (fromUser) AetherActivity.nativeEvent(handle, SEEK, progress, 0, null);
    }
    @Override public void onStartTrackingTouch(SeekBar sb) { }
    @Override public void onStopTrackingTouch(SeekBar sb) { }

    @Override public void onItemSelected(AdapterView<?> parent, View v, int position, long id) {
        AetherActivity.nativeEvent(handle, SELECT, position, 0, null);
    }
    @Override public void onNothingSelected(AdapterView<?> parent) { }

    // Enter / exit of a real pointer (mouse, trackpad, stylus). Returns false
    // so the View still updates its own hovered state.
    @Override public boolean onHover(View v, MotionEvent e) {
        int a = e.getActionMasked();
        if (a == MotionEvent.ACTION_HOVER_ENTER) AetherActivity.nativeEvent(handle, HOVER, 1, 0, null);
        else if (a == MotionEvent.ACTION_HOVER_EXIT) AetherActivity.nativeEvent(handle, HOVER, 0, 0, null);
        return false;
    }

    @Override public void onLayoutChange(View v, int l, int t, int r, int b,
                                         int ol, int ot, int or, int ob) {
        AetherActivity.nativeEvent(handle, LAYOUT, r - l, b - t, null);
    }

    // Observes the stream for the double tap and never consumes it, so the
    // View's own click handling still sees every event.
    @Override public boolean onTouch(View v, MotionEvent e) {
        if (taps != null) taps.onTouchEvent(e);
        return false;
    }
}
