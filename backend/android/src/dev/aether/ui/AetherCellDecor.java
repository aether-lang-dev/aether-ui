package dev.aether.ui;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.LayerDrawable;
import android.text.Layout;
import android.text.StaticLayout;
import android.os.Build;
import android.view.View;
import android.widget.TextView;

/**
 * A text label's cell decorations (aether_ui_text_set_decor), as a
 * spreadsheet draws them. Two of these may be on a view: one under its text,
 * layered over its background, painting a data bar; and one as its
 * foreground, over the text, painting lines on its edges and a triangle in
 * its top end corner. Also the label's underline and line-through, which
 * are paint flags.
 */
public final class AetherCellDecor extends Drawable {
    private final boolean under;
    private final int[] edges;   // top, its colour, right, ..., left, its colour (px, ARGB)
    private final float bar;
    private final int barColor;
    private final int corner;    // ARGB, or 0 for none
    private final float density;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private AetherCellDecor(boolean under, int[] edges, float bar, int barColor, int corner, float density) {
        this.under = under;
        this.edges = edges;
        this.bar = bar;
        this.barColor = barColor;
        this.corner = corner;
        this.density = density;
    }

    /** The background with a bar across `bar` of it (from the end when negative); bg itself for none. */
    public static Drawable under(Drawable bg, float bar, int barColor) {
        if (bar == 0f) return bg;
        Drawable b = new AetherCellDecor(true, null, bar, barColor, 0, 1f);
        if (bg == null) return b;
        return new LayerDrawable(new Drawable[] { bg, b });
    }

    /** Edge lines (widths in dp) and a corner triangle (cornerArgb 0 for none) over the view's content. */
    public static void over(View v, int[] edges, int cornerArgb, float density) {
        boolean any = cornerArgb != 0;
        for (int k = 0; k < 4; k++) if (edges[k * 2] > 0) any = true;
        if (Build.VERSION.SDK_INT >= 23) {
            v.setForeground(any ? new AetherCellDecor(false, edges.clone(), 0f, 0, cornerArgb, density) : null);
        }
    }

    /** A label's underline and line-through, kept with its other paint flags. */
    public static void decorate(TextView tv, boolean underline, boolean strike) {
        int f = tv.getPaintFlags() & ~(Paint.UNDERLINE_TEXT_FLAG | Paint.STRIKE_THRU_TEXT_FLAG);
        if (underline) f |= Paint.UNDERLINE_TEXT_FLAG;
        if (strike) f |= Paint.STRIKE_THRU_TEXT_FLAG;
        tv.setPaintFlags(f);
    }

    /** The height, in px, that text takes in the label's paint wrapped at widthPx (one line for 0). */
    public static int measureHeight(TextView tv, String text, int widthPx) {
        String s = (text == null || text.isEmpty()) ? " " : text;
        int w = widthPx > 0 ? widthPx : 1 << 20;
        StaticLayout l;
        if (Build.VERSION.SDK_INT >= 23) {
            l = StaticLayout.Builder.obtain(s, 0, s.length(), tv.getPaint(), w)
                    .setIncludePad(tv.getIncludeFontPadding()).build();
        } else {
            l = new StaticLayout(s, tv.getPaint(), w, Layout.Alignment.ALIGN_NORMAL, 1f, 0f, tv.getIncludeFontPadding());
        }
        return l.getHeight();
    }

    @Override
    public void draw(Canvas c) {
        Rect b = getBounds();
        boolean rtl = Build.VERSION.SDK_INT >= 23 && getLayoutDirection() == View.LAYOUT_DIRECTION_RTL;
        paint.setStyle(Paint.Style.FILL);
        if (under) {
            float f = Math.abs(bar);
            float w = b.width() * f;
            boolean fromLeft = (bar > 0) != rtl;
            paint.setColor(barColor);
            if (fromLeft) c.drawRect(b.left, b.top, b.left + w, b.bottom, paint);
            else c.drawRect(b.right - w, b.top, b.right, b.bottom, paint);
            return;
        }
        for (int k = 0; k < 4; k++) {
            int wd = edges[k * 2];
            if (wd <= 0) continue;
            float px = Math.max(1f, wd * density);
            paint.setColor(edges[k * 2 + 1]);
            if (k == 0) c.drawRect(b.left, b.top, b.right, b.top + px, paint);
            else if (k == 1) c.drawRect(b.right - px, b.top, b.right, b.bottom, paint);
            else if (k == 2) c.drawRect(b.left, b.bottom - px, b.right, b.bottom, paint);
            else c.drawRect(b.left, b.top, b.left + px, b.bottom, paint);
        }
        if (corner != 0) {
            float t = 6f * density;
            float x = rtl ? b.left : b.right;
            float dx = rtl ? t : -t;
            Path p = new Path();
            p.moveTo(x, b.top);
            p.lineTo(x + dx, b.top);
            p.lineTo(x, b.top + t);
            p.close();
            paint.setColor(corner);
            c.drawPath(p, paint);
        }
    }

    @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); }
    @Override public void setColorFilter(ColorFilter cf) { paint.setColorFilter(cf); }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
}
