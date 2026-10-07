// AetherWrap — the wrap container: children left to right, onto the next
// line when the width runs out (GtkFlowBox on GTK4, a flow-layout UIView on
// UIKit). The platform has no flow layout in android.widget, and a layout is
// a ViewGroup that measures and places its children, so this is one; the
// gaps are dp converted by native code.
package dev.aether.ui;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;

final class AetherWrap extends ViewGroup {
    private final int hgap, vgap;   // px

    AetherWrap(Context context, int hgap, int vgap) {
        super(context);
        this.hgap = hgap;
        this.vgap = vgap;
    }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int mode = MeasureSpec.getMode(widthSpec);
        int maxw = mode == MeasureSpec.UNSPECIFIED ? Integer.MAX_VALUE
                 : MeasureSpec.getSize(widthSpec) - getPaddingLeft() - getPaddingRight();
        int x = 0, y = 0, rowh = 0, widest = 0;
        for (int i = 0; i < getChildCount(); i++) {
            View c = getChildAt(i);
            if (c.getVisibility() == GONE) continue;
            measureChildWithMargins(c, widthSpec, 0, heightSpec, 0);
            MarginLayoutParams lp = (MarginLayoutParams) c.getLayoutParams();
            int cw = c.getMeasuredWidth() + lp.leftMargin + lp.rightMargin;
            int ch = c.getMeasuredHeight() + lp.topMargin + lp.bottomMargin;
            if (x > 0 && x + cw > maxw) { y += rowh + vgap; x = 0; rowh = 0; }
            x += cw;
            widest = Math.max(widest, x);
            x += hgap;
            rowh = Math.max(rowh, ch);
        }
        int w = widest + getPaddingLeft() + getPaddingRight();
        int h = y + rowh + getPaddingTop() + getPaddingBottom();
        setMeasuredDimension(resolveSize(Math.max(w, getSuggestedMinimumWidth()), widthSpec),
                             resolveSize(Math.max(h, getSuggestedMinimumHeight()), heightSpec));
    }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        int rtl = getLayoutDirection() == LAYOUT_DIRECTION_RTL ? 1 : 0;
        int maxw = r - l - getPaddingLeft() - getPaddingRight();
        int x = 0, y = 0, rowh = 0;
        for (int i = 0; i < getChildCount(); i++) {
            View c = getChildAt(i);
            if (c.getVisibility() == GONE) continue;
            MarginLayoutParams lp = (MarginLayoutParams) c.getLayoutParams();
            int cw = c.getMeasuredWidth(), ch = c.getMeasuredHeight();
            int fw = cw + lp.leftMargin + lp.rightMargin;
            if (x > 0 && x + fw > maxw) { y += rowh + vgap; x = 0; rowh = 0; }
            int left = getPaddingLeft() + (rtl == 1 ? maxw - x - fw : x) + lp.leftMargin;
            int top = getPaddingTop() + y + lp.topMargin;
            c.layout(left, top, left + cw, top + ch);
            x += fw + hgap;
            rowh = Math.max(rowh, ch + lp.topMargin + lp.bottomMargin);
        }
    }

    @Override protected LayoutParams generateDefaultLayoutParams() {
        return new MarginLayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
    }
    @Override public LayoutParams generateLayoutParams(android.util.AttributeSet a) {
        return new MarginLayoutParams(getContext(), a);
    }
    @Override protected LayoutParams generateLayoutParams(LayoutParams p) {
        return new MarginLayoutParams(p);
    }
    @Override protected boolean checkLayoutParams(LayoutParams p) {
        return p instanceof MarginLayoutParams;
    }
}
