// AetherA11y — the accessible role and description a widget was given.
//
// An Android View announces its role through the class name in its
// AccessibilityNodeInfo, and a description through the node's hint text.
// Neither has a setter on View, so native code (a11y_set_role_impl /
// a11y_set_description_impl) installs one of these as the View's
// AccessibilityDelegate with the values to report. It carries them; the
// role vocabulary is mapped in C.
package dev.aether.ui;

import android.view.View;
import android.view.accessibility.AccessibilityNodeInfo;

final class AetherA11y extends View.AccessibilityDelegate {
    private String className;   // null = the View's own
    private String hint;        // null = none
    // A grid's structure (aether-ui's a11y_grid*): a grid's size, or a
    // cell's place in its grid (heading for a column header). -1 = unset.
    private int collRows = -1, collCols = -1;
    private int itemRow = -1, itemCol = -1;
    private boolean itemHeading;

    void setClassName(String className) { this.className = className; }
    void setHint(String hint) { this.hint = hint; }
    void setCollection(int rows, int cols) { collRows = rows; collCols = cols; }
    void setItem(int row, int col, boolean heading) {
        itemRow = row; itemCol = col; itemHeading = heading;
    }

    @Override
    public void onInitializeAccessibilityNodeInfo(View host, AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfo(host, info);
        if (className != null) info.setClassName(className);
        if (hint != null) info.setHintText(hint);
        if (collRows >= 0) {
            info.setCollectionInfo(AccessibilityNodeInfo.CollectionInfo.obtain(
                collRows, collCols, false));
        }
        if (itemRow >= 0) {
            info.setCollectionItemInfo(AccessibilityNodeInfo.CollectionItemInfo.obtain(
                itemRow, 1, Math.max(itemCol, 0), 1, itemHeading));
        }
    }
}
