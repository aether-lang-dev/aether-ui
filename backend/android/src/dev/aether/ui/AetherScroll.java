// AetherScroll — the scroll area (scrollview): a ScrollView that, asked for
// its natural height (height unconstrained), answers its minimum rather
// than the whole height of what it scrolls -- what a GtkScrolledWindow and
// an NSScrollView report, and what lets AetherHost ask a body how tall it
// must be without every scroll area in it unrolling. Given a height, it is
// an ordinary ScrollView.
package dev.aether.ui;

import android.content.Context;
import android.widget.ScrollView;

final class AetherScroll extends ScrollView {
    AetherScroll(Context context) { super(context); }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        if (MeasureSpec.getMode(heightSpec) == MeasureSpec.UNSPECIFIED)
            heightSpec = MeasureSpec.makeMeasureSpec(getSuggestedMinimumHeight(), MeasureSpec.EXACTLY);
        super.onMeasure(widthSpec, heightSpec);
    }
}
