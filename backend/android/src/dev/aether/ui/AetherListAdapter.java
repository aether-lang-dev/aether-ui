// AetherListAdapter — the rows of a native list (vlist), realized on demand.
//
// A ListView asks its adapter for the rows its viewport needs and drops the
// rest, so virtualisation is the platform's job (NSTableView's on AppKit,
// GtkListView's on GTK4). ListView needs an Adapter OBJECT to ask, which is
// why this class exists; what a row is stays in C: getView asks native code
// to build row `position` (a registered container the app's row closure
// fills) and returns its View, and a row the list scraps is handed back so
// native code can retire it from the widget registry -- without that, every
// row ever scrolled past would stay registered and "virtualised" would be a
// claim the driver could see was false.
//
// convertView is never reused: a row is a widget tree the app built for one
// index, and re-binding it to another index is the app's render closure's
// job, which runs again for the new row.
package dev.aether.ui;

import android.view.View;
import android.view.ViewGroup;
import android.widget.AbsListView;
import android.widget.BaseAdapter;

final class AetherListAdapter extends BaseAdapter implements AbsListView.RecyclerListener {
    static native View nativeListRow(int handle, int position);
    static native void nativeListScrap(int handle, View row);

    private final int handle;
    private int count;

    AetherListAdapter(int handle) { this.handle = handle; }

    void setCount(int n) {
        count = Math.max(0, n);
        notifyDataSetChanged();
    }

    @Override public int getCount() { return count; }
    @Override public Object getItem(int position) { return null; }
    @Override public long getItemId(int position) { return position; }
    @Override public boolean hasStableIds() { return false; }

    @Override public View getView(int position, View convertView, ViewGroup parent) {
        View v = nativeListRow(handle, position);
        return v != null ? v : new View(parent.getContext());
    }

    @Override public void onMovedToScrapHeap(View view) {
        nativeListScrap(handle, view);
    }
}
