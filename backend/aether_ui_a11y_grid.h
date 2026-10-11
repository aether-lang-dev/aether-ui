/* aether_ui_a11y_grid.h -- the accessible grid position side-store, shared
 * by every backend (each includes it once, from its single translation
 * unit). a11y_grid / a11y_grid_row / a11y_grid_cell / a11y_grid_header record
 * here what they hand the platform (GtkAccessible relations, NSAccessibility
 * index ranges, UIAccessibilityContainerDataTable, Android CollectionInfo,
 * UI Automation's Grid/GridItem patterns), and the driver reads it back.
 *
 * kind: 0 grid (rows x cols), 1 row (row), 2 cell (row, col, header), 3 a
 * column header (row 0, col). Rows and columns count from 0 here; a backend
 * whose platform counts from 1 (GTK, after ARIA) converts.
 */
#ifndef AETHER_UI_A11Y_GRID_H
#define AETHER_UI_A11Y_GRID_H

#include <stdlib.h>
#include <string.h>

typedef struct {
    int set, kind, row, col, rows, cols, header;
} AeuiGridA11y;

static AeuiGridA11y* aeui_ga = NULL;
static int aeui_ga_len = 0;

static AeuiGridA11y* aeui_ga_at(int handle, int grow) {
    if (handle < 1) return NULL;
    if (handle > aeui_ga_len) {
        if (!grow) return NULL;
        int want = handle + 64;
        AeuiGridA11y* g = (AeuiGridA11y*)realloc(aeui_ga, sizeof(AeuiGridA11y) * (size_t)want);
        if (!g) return NULL;
        memset(g + aeui_ga_len, 0, sizeof(AeuiGridA11y) * (size_t)(want - aeui_ga_len));
        aeui_ga = g;
        aeui_ga_len = want;
    }
    return &aeui_ga[handle - 1];
}

/* Record; 1 when anything changed (a backend may skip the platform call). */
static int aeui_ga_store(int handle, int kind, int row, int col, int rows, int cols, int header) {
    AeuiGridA11y* e = aeui_ga_at(handle, 1);
    if (!e) return 0;
    if (e->set && e->kind == kind && e->row == row && e->col == col
        && e->rows == rows && e->cols == cols && e->header == header) return 0;
    e->set = 1; e->kind = kind; e->row = row; e->col = col;
    e->rows = rows; e->cols = cols; e->header = header;
    return 1;
}

/* The record, as set. Each backend defines aether_ui_a11y_grid_get_impl
   itself, starting from this and reporting what the platform actually
   holds (GTK's test AT context, AppKit's accessors, UIKit's data-table
   protocol, Android's node info, the Win32 UIA provider), so the driver
   reads the screen reader's view, not just the record. */
static int aeui_ga_get(int handle, int* kind, int* row, int* col,
                       int* rows, int* cols, int* header) {
    AeuiGridA11y* e = aeui_ga_at(handle, 0);
    if (!e || !e->set) return 0;
    *kind = e->kind; *row = e->row; *col = e->col;
    *rows = e->rows; *cols = e->cols; *header = e->header;
    return 1;
}


#endif
