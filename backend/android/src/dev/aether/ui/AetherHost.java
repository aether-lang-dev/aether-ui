// AetherHost — the frame a window's body is mounted in (the activity's, and
// each extra window's / sheet's dialog), with the overlays above it.
//
// A desktop window can never be smaller than its content needs: GTK4 and
// AppKit grow the window (past the screen if they must) rather than squeeze
// what is in it. Android's LinearLayout does the opposite: the children
// past the bottom of a full stack are measured at height 0 -- there, but
// invisible, unfocusable (a zero-sized View cannot take focus) and reported
// as 0 tall. So the body is measured twice: for its NATURAL height (height
// unconstrained; scroll areas report their minimum, see AetherScroll, as a
// GtkScrolledWindow does), and then at the window's height or that natural
// height, whichever is more. Content that fits is laid out exactly as
// before; content that does not overflows the window, clipped, as on the
// desktop, instead of collapsing.
package dev.aether.ui;

import android.content.Context;
import android.view.View;
import android.widget.FrameLayout;

final class AetherHost extends FrameLayout {
    AetherHost(Context context) { super(context); }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        super.onMeasure(widthSpec, heightSpec);
        if (MeasureSpec.getMode(heightSpec) == MeasureSpec.UNSPECIFIED) return;
        int avail = MeasureSpec.getSize(heightSpec) - getPaddingTop() - getPaddingBottom();
        for (int i = 0; i < getChildCount(); i++) {
            View c = getChildAt(i);
            if (c.getVisibility() == GONE) continue;
            LayoutParams lp = (LayoutParams) c.getLayoutParams();
            int floor;
            if (lp.height == LayoutParams.MATCH_PARENT) floor = avail - lp.topMargin - lp.bottomMargin;
            else if (lp.height >= 0) floor = lp.height;
            else continue;   // an overlay's content: its own size
            int w = c.getMeasuredWidth();
            c.measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
                      MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED));
            int natural = c.getMeasuredHeight();
            c.measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
                      MeasureSpec.makeMeasureSpec(Math.max(Math.max(floor, natural), 0),
                                                  MeasureSpec.EXACTLY));
        }
    }
}
