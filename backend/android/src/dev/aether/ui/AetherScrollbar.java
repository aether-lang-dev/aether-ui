// AetherScrollbar — aether-ui's scrollbar (windowing slider). Android has
// no scrollbar control a person can hold by itself (a View's scroll
// indicators belong to its own scrolling and can't be set up apart from
// it), so this one is drawn: a track and a thumb the window's share of it.
// Native code owns the state (aether_ui_scrollbar.h) and hands this view
// the thumb's size and place (set); a drag reports the thumb's new place,
// a tap on the track a page either way, and TalkBack's scroll actions a
// page, all through nativeScrollbar.
package dev.aether.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.accessibility.AccessibilityNodeInfo;

final class AetherScrollbar extends View {
    static final int MOVE_TO = 0, PAGE_BACK = -1, PAGE_FORWARD = 1;

    private final int handle;
    private final boolean vertical;
    private double proportion = 1, fraction = 0;
    private final Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint thumb = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF r = new RectF();
    private boolean dragging;
    private float dragFrom;
    private double dragFraction;

    AetherScrollbar(Context context, int handle, boolean vertical) {
        super(context);
        this.handle = handle;
        this.vertical = vertical;
        track.setColor(0x22808080);
        thumb.setColor(0x99606060);
        setFocusable(true);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
    }

    static native void nativeScrollbar(int handle, int kind, double fraction);

    /** The thumb's share of the track and where it sits in the room it moves in, 0..1. */
    void set(double proportion, double fraction) {
        this.proportion = Math.max(0, Math.min(1, proportion));
        this.fraction = Math.max(0, Math.min(1, fraction));
        setEnabled(this.proportion < 1);
        invalidate();
    }

    private float thick() { return 10 * getResources().getDisplayMetrics().density; }
    private float length() { return vertical ? getHeight() : getWidth(); }
    private float thumbLength() {
        float len = length(), t = (float) (proportion * len), min = thick() * 2;
        return t < min ? Math.min(min, len) : t;
    }
    private float thumbStart() { return (float) (fraction * (length() - thumbLength())); }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int t = Math.round(thick());
        if (vertical) setMeasuredDimension(t, getDefaultSize(t * 6, heightSpec));
        else setMeasuredDimension(getDefaultSize(t * 6, widthSpec), t);
    }

    @Override protected void onDraw(Canvas c) {
        float rad = thick() / 2;
        r.set(0, 0, getWidth(), getHeight());
        c.drawRoundRect(r, rad, rad, track);
        if (proportion >= 1) return;
        float a = thumbStart(), b = a + thumbLength();
        if (vertical) r.set(1, a + 1, getWidth() - 1, b - 1);
        else r.set(a + 1, 1, b - 1, getHeight() - 1);
        c.drawRoundRect(r, rad - 1, rad - 1, thumb);
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (!isEnabled()) return false;
        float at = vertical ? e.getY() : e.getX();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                float a = thumbStart(), b = a + thumbLength();
                if (at >= a && at <= b) {
                    dragging = true;
                    dragFrom = at;
                    dragFraction = fraction;
                    getParent().requestDisallowInterceptTouchEvent(true);
                } else {
                    dragging = false;
                    nativeScrollbar(handle, at < a ? PAGE_BACK : PAGE_FORWARD, 0);
                }
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                if (dragging) {
                    float room = length() - thumbLength();
                    if (room > 0) nativeScrollbar(handle, MOVE_TO,
                        Math.max(0, Math.min(1, dragFraction + (at - dragFrom) / room)));
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (!dragging) performClick();
                dragging = false;
                return true;
            case MotionEvent.ACTION_CANCEL:
                dragging = false;
                return true;
        }
        return super.onTouchEvent(e);
    }

    @Override public boolean performClick() { return super.performClick(); }

    @Override public CharSequence getAccessibilityClassName() { return "android.widget.AbsSeekBar"; }

    @Override public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfo(info);
        info.setRangeInfo(AccessibilityNodeInfo.RangeInfo.obtain(
            AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_PERCENT, 0, 100, (float) (fraction * 100)));
        if (proportion < 1) {
            if (fraction > 0) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD);
            if (fraction < 1) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD);
        }
    }

    @Override public boolean performAccessibilityAction(int action, Bundle args) {
        if (action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD) { nativeScrollbar(handle, PAGE_FORWARD, 0); return true; }
        if (action == AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD) { nativeScrollbar(handle, PAGE_BACK, 0); return true; }
        return super.performAccessibilityAction(action, args);
    }
}
