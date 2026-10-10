// AetherVSeekBar — a vertical slider (ui.vslider): a SeekBar laid out, drawn
// and touched a quarter turn clockwise, so its minimum is at the TOP and its
// value grows downward, as a scrollbar's does and as aether-ui's vertical
// slider does on every backend. Android has no vertical SeekBar; this is the
// usual one: measure and size with width and height swapped, rotate the
// canvas to draw, and map a touch's y to progress. Progress, max and the
// change listener are the SeekBar's own, so native code drives it like any
// slider.
package dev.aether.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.view.MotionEvent;
import android.widget.SeekBar;

final class AetherVSeekBar extends SeekBar {
    // setProgress from code reports fromUser = false, and AetherListener only
    // forwards a person's changes: a drag here reports itself to the listener
    // as the user's, once.
    private OnSeekBarChangeListener listener;

    AetherVSeekBar(Context context) { super(context); }

    @Override public void setOnSeekBarChangeListener(OnSeekBarChangeListener l) {
        listener = l;
        super.setOnSeekBarChangeListener(l);
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(h, w, oldh, oldw);
    }

    @Override protected synchronized void onMeasure(int widthSpec, int heightSpec) {
        super.onMeasure(heightSpec, widthSpec);
        setMeasuredDimension(getMeasuredHeight(), getMeasuredWidth());
    }

    @Override protected void onDraw(Canvas c) {
        c.rotate(90);
        c.translate(0, -getWidth());
        super.onDraw(c);
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (!isEnabled()) return false;
        switch (e.getAction()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_UP: {
                int h = getHeight() - getPaddingTop() - getPaddingBottom();
                float y = e.getY() - getPaddingTop();
                if (h <= 0) return true;
                float f = Math.max(0f, Math.min(1f, y / h));
                int p = Math.round(f * getMax());
                if (p != getProgress()) {
                    setProgress(p);
                    if (listener != null) listener.onProgressChanged(this, p, true);
                }
                onSizeChanged(getWidth(), getHeight(), 0, 0);
                if (e.getAction() == MotionEvent.ACTION_UP) performClick();
                return true;
            }
            default:
                return true;
        }
    }

    @Override public boolean performClick() { return super.performClick(); }
}
