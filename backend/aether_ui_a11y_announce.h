/* aether_ui_a11y_announce.h -- the last thing each widget had a screen
 * reader say (a11y_announce), shared by every backend (each includes it once,
 * from its single translation unit). a11y_announce hands the platform the
 * announcement (GTK's gtk_accessible_announce, AppKit's
 * NSAccessibilityAnnouncementRequestedNotification, UIKit's
 * UIAccessibilityAnnouncementNotification, Android's
 * View.announceForAccessibility, UI Automation's notification event) and
 * records it here; the driver reads it back (/widget/{id}/a11y "announced",
 * and a count, so a spec can tell a repeat from nothing).
 *
 * The store is written on the UI thread and read on the driver's, so a small
 * spin lock guards it. */
#ifndef AETHER_UI_A11Y_ANNOUNCE_H
#define AETHER_UI_A11Y_ANNOUNCE_H

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

typedef struct { char* text; int count; } AeuiAnnounced;

static AeuiAnnounced* aeui_an = NULL;
static int aeui_an_len = 0;
static atomic_flag aeui_an_lock = ATOMIC_FLAG_INIT;

static void aeui_an_take(void) { while (atomic_flag_test_and_set_explicit(&aeui_an_lock, memory_order_acquire)) { } }
static void aeui_an_give(void) { atomic_flag_clear_explicit(&aeui_an_lock, memory_order_release); }

/* Record `text` as `handle`'s latest announcement. */
static void aeui_an_store(int handle, const char* text) {
    if (handle < 1) return;
    char* copy = strdup(text ? text : "");
    if (!copy) return;
    aeui_an_take();
    if (handle > aeui_an_len) {
        int want = handle + 64;
        AeuiAnnounced* a = (AeuiAnnounced*)realloc(aeui_an, sizeof(AeuiAnnounced) * (size_t)want);
        if (!a) { aeui_an_give(); free(copy); return; }
        memset(a + aeui_an_len, 0, sizeof(AeuiAnnounced) * (size_t)(want - aeui_an_len));
        aeui_an = a;
        aeui_an_len = want;
    }
    free(aeui_an[handle - 1].text);
    aeui_an[handle - 1].text = copy;
    aeui_an[handle - 1].count++;
    aeui_an_give();
}

/* The latest announcement into buf ("" for none); how many there have been. */
static int aeui_an_get(int handle, char* buf, int bufsz) {
    int n = 0;
    if (buf && bufsz > 0) buf[0] = '\0';
    aeui_an_take();
    if (handle >= 1 && handle <= aeui_an_len && aeui_an[handle - 1].text) {
        if (buf && bufsz > 0) {
            strncpy(buf, aeui_an[handle - 1].text, (size_t)bufsz - 1);
            buf[bufsz - 1] = '\0';
        }
        n = aeui_an[handle - 1].count;
    }
    aeui_an_give();
    return n;
}

#endif
