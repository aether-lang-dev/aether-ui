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
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.DialogInterface;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.view.DragEvent;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.GestureDetector;
import android.view.InputDevice;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.AdapterView;
import android.widget.TextView;
import android.widget.CompoundButton;
import android.widget.PopupMenu;
import android.widget.SeekBar;

final class AetherListener implements View.OnClickListener, TextWatcher,
        CompoundButton.OnCheckedChangeListener, SeekBar.OnSeekBarChangeListener,
        AdapterView.OnItemSelectedListener, View.OnHoverListener,
        View.OnLayoutChangeListener, View.OnTouchListener,
        View.OnLongClickListener, View.OnContextClickListener,
        PopupMenu.OnMenuItemClickListener, PopupMenu.OnDismissListener,
        DialogInterface.OnDismissListener, DialogInterface.OnClickListener,
        View.OnGenericMotionListener, SurfaceHolder.Callback, View.OnDragListener,
        TextView.OnEditorActionListener {
    // Event kinds; the same numbers are AEUI_EV_* in aether_ui_android.c.
    static final int CLICK = 1, TEXT = 2, CHECK = 3, SEEK = 4, SELECT = 5,
                     HOVER = 6, LAYOUT = 7, DOUBLE = 8,
                     TAB = 9,        // a tab strip button: arg = page index
                     MENU = 10,      // a popup menu item: a = item id
                     CONTEXT = 11,   // long press / secondary click: open the context menu
                     SCRIM = 12,     // a tap on a modal overlay's scrim
                     DISMISS = 13,   // a dialog (window, sheet) went away: arg = what it was
                     DRAG = 14,      // a split divider dragged: a = action, b = raw px
                     WHEEL = 15,     // a wheel / two-finger scroll: a = steps (+ = toward the end)
                     MENU_CLOSED = 16, // a popup menu closed (chosen or not)
                     SURFACE = 17,   // a native view's Surface: a = width px, b = height px (0x0 = gone)
                     MENU_OPEN = 18, // a window's menu-bar title tapped: open that menu
                     ROW_DRAG = 19,  // a reorderable row long-pressed: start its drag
                     ROW_DROP = 20,  // a row dropped on this one: a = source index
                     FILE_DRAG = 21, // a draggable file's widget long-pressed: start the drag
                     FILE_DROP = 22, // files dropped on the window: s = paths, newline-separated
                     SUBMIT = 23;    // Return / the keyboard's Done in a field: s = its text

    private final int handle;
    private final int kind;
    private final int arg;
    private final GestureDetector taps;   // DOUBLE only

    AetherListener(int handle, int kind) {
        this(handle, kind, 0);
    }

    // A listener that carries a payload of its own: the page index a tab
    // button selects, what a dismissed dialog was.
    AetherListener(int handle, int kind, int arg) {
        this.handle = handle;
        this.kind = kind;
        this.arg = arg;
        this.taps = null;
    }

    // A double tap: Android has no double-click event of its own, so the
    // platform's GestureDetector recognises it from the touch stream.
    AetherListener(Context context, int handle) {
        this.handle = handle;
        this.kind = DOUBLE;
        this.arg = 0;
        this.taps = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDoubleTap(MotionEvent e) {
                AetherActivity.nativeEvent(handle, DOUBLE, 0, 0, null);
                return true;
            }
        });
    }

    @Override public void onClick(View v) {
        AetherActivity.nativeEvent(handle, kind == CLICK ? CLICK : kind, arg, 0, null);
    }

    @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
    @Override public void onTextChanged(CharSequence s, int start, int before, int count) { }
    @Override public void afterTextChanged(Editable s) {
        AetherActivity.nativeEvent(handle, TEXT, 0, 0, s.toString());
    }

    // on_submit: the keyboard's action key (Done, Go, Send) or a hardware
    // Enter. The driver calls TextView.onEditorAction(IME_ACTION_DONE),
    // which is what the keyboard's key does, so both arrive here.
    @Override public boolean onEditorAction(TextView v, int actionId, KeyEvent ev) {
        boolean enter = ev != null && ev.getKeyCode() == KeyEvent.KEYCODE_ENTER;
        if (enter && ev.getAction() != KeyEvent.ACTION_DOWN) return true;
        if (enter || actionId == EditorInfo.IME_ACTION_DONE || actionId == EditorInfo.IME_ACTION_GO
                || actionId == EditorInfo.IME_ACTION_SEND) {
            AetherActivity.nativeEvent(handle, SUBMIT, 0, 0, v.getText().toString());
            return true;
        }
        return false;
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
    // View's own click handling still sees every event. A split divider's
    // listener (DRAG) owns its touches: the drag is the divider's whole job.
    @Override public boolean onTouch(View v, MotionEvent e) {
        if (kind == DRAG) {
            int a = e.getActionMasked();
            float raw = arg != 0 ? e.getRawY() : e.getRawX();
            AetherActivity.nativeEvent(handle, DRAG, a, Math.round(raw), null);
            return a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_MOVE
                || a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL;
        }
        if (taps != null) taps.onTouchEvent(e);
        return false;
    }

    // The context menu: a long press on a touch screen, a secondary click
    // under a mouse (View.OnContextClickListener). Native code builds and
    // shows the menu; consumed so the press does not also click.
    // (A reorderable row's or a draggable file's long press starts its drag.)
    @Override public boolean onLongClick(View v) {
        AetherActivity.nativeEvent(handle, kind == ROW_DRAG || kind == FILE_DRAG ? kind : CONTEXT,
                                   0, 0, null);
        return true;
    }
    @Override public boolean onContextClick(View v) {
        AetherActivity.nativeEvent(handle, CONTEXT, 0, 0, null);
        return true;
    }

    // An item of a PopupMenu: the menu's handle and the item's id.
    @Override public boolean onMenuItemClick(MenuItem item) {
        AetherActivity.nativeEvent(handle, MENU, item.getItemId(), arg, null);
        return true;
    }
    @Override public void onDismiss(PopupMenu menu) {
        AetherActivity.nativeEvent(handle, MENU_CLOSED, arg, 0, null);
    }

    // A dialog (an extra window, a sheet, an alert) closed: by its own
    // button, Back, a tap outside, or dismiss() from native code.
    @Override public void onDismiss(DialogInterface d) {
        AetherActivity.nativeEvent(handle, DISMISS, arg, 0, null);
    }
    @Override public void onClick(DialogInterface d, int which) {
        AetherActivity.nativeEvent(handle, CLICK, which, arg, null);
    }

    // A mouse wheel or a trackpad's two-finger scroll over a vlist, in rows:
    // AXIS_VSCROLL is positive away from the user, the DSL's dy is positive
    // toward the end, so the sign flips.
    @Override public boolean onGenericMotion(View v, MotionEvent e) {
        if (kind != WHEEL || e.getActionMasked() != MotionEvent.ACTION_SCROLL
                || !e.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) return false;
        float dy = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
        int steps = dy > 0 ? -(int)Math.ceil(dy) : (int)Math.ceil(-dy);
        if (steps == 0) return false;
        AetherActivity.nativeEvent(handle, WHEEL, steps, 0, null);
        return true;
    }

    // A native view's Surface (SurfaceView): realized with a size, resized,
    // and gone. Native code takes the ANativeWindow from the holder.
    @Override public void surfaceCreated(SurfaceHolder h) { }
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hgt) {
        AetherActivity.nativeEvent(handle, SURFACE, w, hgt, null);
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) {
        AetherActivity.nativeEvent(handle, SURFACE, 0, 0, null);
    }

    // Drops. ROW_DROP: a row of the same app's list, whose index travels as
    // the drag's text (label "aeui-row"). FILE_DROP: anything another app
    // drags in -- each item's URI becomes a path native code can open (a
    // file: URI is its path; a content: URI is opened through the
    // ContentResolver, under the drop's permissions, as /proc/self/fd/N),
    // plain text is passed as it is; the paths go across newline-separated,
    // which is the ABI's file-drop shape.
    @Override public boolean onDrag(View v, DragEvent e) {
        ClipDescription d = e.getClipDescription();
        boolean row = d != null && "aeui-row".contentEquals(d.getLabel());
        if (kind == ROW_DROP) {
            if (!row) return false;
            if (e.getAction() == DragEvent.ACTION_DROP) {
                ClipData c = e.getClipData();
                if (c == null || c.getItemCount() < 1) return false;
                try {
                    int src = Integer.parseInt(String.valueOf(c.getItemAt(0).getText()));
                    AetherActivity.nativeEvent(handle, ROW_DROP, src, 0, null);
                } catch (NumberFormatException ignored) {
                    return false;
                }
            }
            return true;
        }
        if (kind != FILE_DROP || row) return false;
        if (e.getAction() != DragEvent.ACTION_DROP) return true;
        Activity a = (Activity) v.getContext();
        a.requestDragAndDropPermissions(e);
        ClipData c = e.getClipData();
        if (c == null) return false;
        StringBuilder paths = new StringBuilder();
        for (int i = 0; i < c.getItemCount(); i++) {
            ClipData.Item it = c.getItemAt(i);
            String p = null;
            Uri u = it.getUri();
            if (u != null && "file".equals(u.getScheme())) {
                p = u.getPath();
            } else if (u != null) {
                try {
                    ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(u, "r");
                    if (pfd != null) p = "/proc/self/fd/" + pfd.detachFd();
                } catch (Exception ex) {
                    android.util.Log.w("aether-ui", "dropped " + u + " cannot be opened", ex);
                }
            } else if (it.getText() != null) {
                p = it.getText().toString();
            }
            if (p == null || p.isEmpty()) continue;
            if (paths.length() > 0) paths.append('\n');
            paths.append(p);
        }
        if (paths.length() == 0) return false;
        AetherActivity.nativeEvent(handle, FILE_DROP, 0, 0, paths.toString());
        return true;
    }
}
