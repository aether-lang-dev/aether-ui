// AetherText — aether-ui's label: a TextView with typography beyond its
// font (aether_ui_typo.h): a double underline (two lines under each line of
// text), super- or subscript (a span: two thirds the size, the baseline a
// third up or a fifth down, kept as the text changes), its vertical place in
// its box (gravity, kept as the horizontal gravity changes), an indent
// (leading padding), and a rotation: View.draw paints the background before
// onDraw, so turning the canvas here turns the text alone, about the box's
// centre, clipped to the box.
package dev.aether.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.text.Layout;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.TextPaint;
import android.text.style.MetricAffectingSpan;
import android.view.Gravity;
import android.widget.TextView;

final class AetherText extends TextView {
    private int script, valign, indentPx;
    private boolean doubleUnder;
    private float rotation;
    private int basePadLeft = -1;
    private int baseGravity = -1;   // the label's own, before any typography
    private final Paint line = new Paint();

    AetherText(Context c) { super(c); }

    /** The span a super- or subscript is: the size two thirds, the baseline moved. */
    private static final class ScriptSpan extends MetricAffectingSpan {
        private final boolean sup;
        ScriptSpan(boolean sup) { this.sup = sup; }
        private void apply(TextPaint p) {
            float size = p.getTextSize();
            p.baselineShift += sup ? (int) (-size * 0.33f) : (int) (size * 0.2f);
            p.setTextSize(size * 0.66f);
        }
        @Override public void updateDrawState(TextPaint p) { apply(p); }
        @Override public void updateMeasureState(TextPaint p) { apply(p); }
    }

    /** Native code's call (aeui_typo_apply). */
    void typography(int underline, int script, int valign, int indentPx, float rotation) {
        this.doubleUnder = underline == 2;
        this.script = script;
        this.valign = valign;
        this.rotation = rotation;
        if (basePadLeft < 0) basePadLeft = getPaddingLeft() - this.indentPx;
        if (baseGravity < 0) baseGravity = getGravity();
        this.indentPx = indentPx;
        setPadding(basePadLeft + indentPx, getPaddingTop(), getPaddingRight(), getPaddingBottom());
        // Turned text sits in the middle of its box; otherwise the label's own
        // gravity, its vertical part the placement's (setGravity below).
        setGravity(rotation != 0 ? Gravity.CENTER : baseGravity);
        setText(getText().toString());
        invalidate();
    }

    @Override public void setGravity(int g) {
        if (baseGravity >= 0 && rotation == 0 && g != baseGravity) baseGravity = g;   // the anchor's, as it changes
        if (valign != 0) {
            g &= ~Gravity.VERTICAL_GRAVITY_MASK;
            g |= valign == 1 ? Gravity.TOP : valign == 3 ? Gravity.BOTTOM : Gravity.CENTER_VERTICAL;
        }
        super.setGravity(g);
    }

    @Override public void setText(CharSequence text, BufferType type) {
        if (script != 0 && text != null && !(text instanceof Spanned && ((Spanned) text).getSpans(0, text.length(), ScriptSpan.class).length > 0)) {
            SpannableString s = new SpannableString(text);
            s.setSpan(new ScriptSpan(script == 1), 0, s.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
            super.setText(s, BufferType.SPANNABLE);
            return;
        }
        super.setText(text, type);
    }

    @Override protected void onDraw(Canvas c) {
        if (rotation != 0) {
            c.save();
            // onDraw's canvas is already moved by the view's scroll (a
            // centred single line scrolls): the box is where the scroll is.
            float sx = getScrollX(), sy = getScrollY();
            c.clipRect(sx, sy, sx + getWidth(), sy + getHeight());
            c.rotate(-rotation, sx + getWidth() / 2f, sy + getHeight() / 2f);
            super.onDraw(c);
            c.restore();
            return;
        }
        super.onDraw(c);
        Layout l = getLayout();
        if (!doubleUnder || l == null) return;
        line.setColor(getCurrentTextColor());
        float d = getResources().getDisplayMetrics().density;
        line.setStrokeWidth(Math.max(1f, d));
        float dx = getTotalPaddingLeft() - getScrollX(), dy = getTotalPaddingTop() - getScrollY();
        for (int i = 0; i < l.getLineCount(); i++) {
            float y = dy + l.getLineBaseline(i) + 2 * d;
            float a = dx + l.getLineLeft(i), b = dx + l.getLineRight(i);
            c.drawLine(a, y, b, y, line);
            c.drawLine(a, y + 2 * d, b, y + 2 * d, line);
        }
    }
}
