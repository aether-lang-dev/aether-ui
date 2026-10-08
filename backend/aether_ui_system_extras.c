// Aether UI — system tray + desktop-notification registry.
// See aether_ui_system_extras.h for the contract + design notes.

#include "aether_ui_system_extras.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <time.h>   // clock_gettime: the frame clock's monotonic time
#endif

// Backend-supplied: read a reactive-state cell. Used by tray_current_icon
// to resolve which of (clean / busy / alert) the tray is on. Every backend
// already defines this — see aether_ui_state_get in the per-backend file.
extern double aether_ui_state_get(int handle);

// Runtime: reclaim a closure env through its own destructor (aether_runtime.h).
// Declared here rather than included so this file stays free of the runtime
// headers, exactly as aether_ui_state_get above is.
extern void aether_closure_env_free(void* env);

// Closure layout: the box_closure() return contract is `{fn, env}`. We
// only need to invoke it, so a thin local mirror works for both 32- and
// 64-bit pointers.
typedef struct { void* fn; void* env; } AeClosureLocal;
static void invoke_closure(void* boxed) {
    if (!boxed) return;
    AeClosureLocal* c = (AeClosureLocal*)boxed;
    if (!c->fn) return;
    ((void(*)(void*))c->fn)(c->env);
}

// ---------------------------------------------------------------------------
// Tray registry
// ---------------------------------------------------------------------------

#define TRAY_MAX 64
#define NOTIF_MAX 256
#define MENU_ITEM_MAX 512

typedef struct {
    int  in_use;
    char name[64];
    char tooltip[256];
    int  menu_handle;
    int  state_handle;
    char icon_clean[256];
    char icon_busy[256];
    char icon_alert[256];
    int  is_template;
    int  sealed;
    void* left_click_boxed;
} TrayRec;

static TrayRec g_tray[TRAY_MAX];
static int     g_tray_count = 0;

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int aether_ui_tray_register(const char* name, void* boxed_left_click) {
    if (g_tray_count >= TRAY_MAX) return 0;
    int idx = g_tray_count++;
    TrayRec* t = &g_tray[idx];
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    copy_str(t->name, sizeof(t->name), name ? name : "");
    t->left_click_boxed = boxed_left_click;
    return idx + 1; // 1-based
}

static TrayRec* tray_lookup(int tray_id) {
    if (tray_id < 1 || tray_id > g_tray_count) return NULL;
    TrayRec* t = &g_tray[tray_id - 1];
    return t->in_use ? t : NULL;
}

void aether_ui_tray_set_tooltip_reg(int tray_id, const char* text) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    copy_str(t->tooltip, sizeof(t->tooltip), text);
}

void aether_ui_tray_set_menu_reg(int tray_id, int menu_handle) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    t->menu_handle = menu_handle;
}

void aether_ui_tray_set_icon_for_state_reg(int tray_id, int state_handle,
                                            const char* icon_clean,
                                            const char* icon_busy,
                                            const char* icon_alert) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    t->state_handle = state_handle;
    copy_str(t->icon_clean, sizeof(t->icon_clean), icon_clean);
    copy_str(t->icon_busy,  sizeof(t->icon_busy),  icon_busy);
    copy_str(t->icon_alert, sizeof(t->icon_alert), icon_alert);
}

void aether_ui_tray_set_icon_template_reg(int tray_id, int is_template) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    t->is_template = is_template ? 1 : 0;
}

void aether_ui_tray_seal_reg(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    t->sealed = 1;
}

int aether_ui_tray_count(void) { return g_tray_count; }

const char* aether_ui_tray_name(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->name : "";
}

const char* aether_ui_tray_tooltip(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->tooltip : "";
}

int aether_ui_tray_menu_handle(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->menu_handle : 0;
}

// Convention from the ask: 0=clean, 1=busy/syncing, 2=conflict/alert.
const char* aether_ui_tray_current_icon(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return "";
    if (t->state_handle <= 0) return t->icon_clean;
    int s = (int)aether_ui_state_get(t->state_handle);
    if (s <= 0) return t->icon_clean;
    if (s == 1) return t->icon_busy;
    return t->icon_alert;
}

int aether_ui_tray_is_template(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->is_template : 0;
}

int aether_ui_tray_is_sealed(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->sealed : 0;
}

int aether_ui_tray_emit_click(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return 3;
    if (t->sealed) return 1;
    if (!t->left_click_boxed) return 4;
    invoke_closure(t->left_click_boxed);
    return 0;
}

int aether_ui_tray_menu_activate(int tray_id, const char* item_label) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return 3;
    if (t->sealed) return 1;
    if (t->menu_handle <= 0) return 3;
    return aether_ui_menu_item_invoke(t->menu_handle, item_label);
}

// ---------------------------------------------------------------------------
// Notification registry
// ---------------------------------------------------------------------------

typedef struct {
    int  in_use;
    char title[256];
    char body[1024];
    char icon[256];
    char tag[128];
    int  dismissed;
    void* click_boxed;
} NotifRec;

static NotifRec g_notif[NOTIF_MAX];
static int      g_notif_count = 0;

// Tag-replace: if a new notification matches an existing record's tag,
// reuse its slot (mirroring libnotify replaces_id / UN identifier / Toast
// tag+group semantics). Empty tag never replaces.
static int notif_slot_for_tag(const char* tag) {
    if (!tag || !*tag) return -1;
    for (int i = 0; i < g_notif_count; i++) {
        if (g_notif[i].in_use && strcmp(g_notif[i].tag, tag) == 0) return i;
    }
    return -1;
}

static int notif_alloc_slot(void) {
    if (g_notif_count >= NOTIF_MAX) return -1;
    return g_notif_count++;
}

int aether_ui_notify_register(const char* title, const char* body) {
    return aether_ui_notify_register_full(title, body, "", "", NULL);
}

int aether_ui_notify_register_full(const char* title, const char* body,
                                    const char* icon_path, const char* tag,
                                    void* boxed_click) {
    int slot = notif_slot_for_tag(tag);
    if (slot < 0) {
        slot = notif_alloc_slot();
        if (slot < 0) return 0;
    }
    NotifRec* n = &g_notif[slot];
    memset(n, 0, sizeof(*n));
    n->in_use = 1;
    copy_str(n->title, sizeof(n->title), title);
    copy_str(n->body,  sizeof(n->body),  body);
    copy_str(n->icon,  sizeof(n->icon),  icon_path);
    copy_str(n->tag,   sizeof(n->tag),   tag);
    n->click_boxed = boxed_click;
    return slot + 1;
}

int aether_ui_notify_request_permission(void) {
    // Linux + Win: always granted. macOS would call
    // UNUserNotificationCenter requestAuthorizationWithOptions here
    // when the real backend lands.
    return 1;
}

int aether_ui_notif_count(void) { return g_notif_count; }

static NotifRec* notif_lookup(int notif_id) {
    if (notif_id < 1 || notif_id > g_notif_count) return NULL;
    NotifRec* n = &g_notif[notif_id - 1];
    return n->in_use ? n : NULL;
}

const char* aether_ui_notif_title(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->title : "";
}
const char* aether_ui_notif_body(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->body : "";
}
const char* aether_ui_notif_icon(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->icon : "";
}
const char* aether_ui_notif_tag(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->tag : "";
}
int aether_ui_notif_dismissed(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->dismissed : 0;
}

int aether_ui_notif_emit_click(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    if (!n) return 3;
    if (!n->click_boxed) return 4;
    invoke_closure(n->click_boxed);
    return 0;
}

int aether_ui_notif_mark_dismissed(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    if (!n) return 3;
    n->dismissed = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Menu-item side-store
// ---------------------------------------------------------------------------

typedef struct {
    int   in_use;
    int   menu_handle;
    char  label[128];
    void* boxed;
} MenuItemRec;

static MenuItemRec g_menu_items[MENU_ITEM_MAX];
static int         g_menu_item_count = 0;

void aether_ui_menu_item_record(int menu_handle, const char* label,
                                 void* boxed_closure) {
    if (g_menu_item_count >= MENU_ITEM_MAX) return;
    MenuItemRec* m = &g_menu_items[g_menu_item_count++];
    m->in_use = 1;
    m->menu_handle = menu_handle;
    copy_str(m->label, sizeof(m->label), label);
    m->boxed = boxed_closure;
}

int aether_ui_menu_item_invoke(int menu_handle, const char* label) {
    if (!label) return 3;
    for (int i = 0; i < g_menu_item_count; i++) {
        MenuItemRec* m = &g_menu_items[i];
        if (!m->in_use) continue;
        if (m->menu_handle != menu_handle) continue;
        if (strcmp(m->label, label) != 0) continue;
        if (!m->boxed) return 4;
        invoke_closure(m->boxed);
        return 0;
    }
    return 3;
}

int aether_ui_menu_item_relabel(int menu_handle, const char* old_label,
                                const char* new_label) {
    if (!old_label || !new_label) return 0;
    for (int i = 0; i < g_menu_item_count; i++) {
        MenuItemRec* m = &g_menu_items[i];
        if (!m->in_use || m->menu_handle != menu_handle) continue;
        if (strcmp(m->label, old_label) != 0) continue;
        copy_str(m->label, sizeof(m->label), new_label);
        return 1;
    }
    return 0;
}

int aether_ui_menu_item_count_for(int menu_handle) {
    int n = 0;
    for (int i = 0; i < g_menu_item_count; i++) {
        if (g_menu_items[i].in_use && g_menu_items[i].menu_handle == menu_handle)
            n++;
    }
    return n;
}

const char* aether_ui_menu_item_label_at(int menu_handle, int index) {
    int seen = 0;
    for (int i = 0; i < g_menu_item_count; i++) {
        MenuItemRec* m = &g_menu_items[i];
        if (!m->in_use || m->menu_handle != menu_handle) continue;
        if (seen == index) return m->label;
        seen++;
    }
    return "";
}

int aether_ui_menu_handles(int* out, int max) {
    int n = 0;
    for (int i = 0; i < g_menu_item_count; i++) {
        if (!g_menu_items[i].in_use) continue;
        int h = g_menu_items[i].menu_handle;
        int dup = 0;
        for (int j = 0; j < n; j++) { if (out[j] == h) { dup = 1; break; } }
        if (dup) continue;
        if (n >= max) break;
        out[n++] = h;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Headless open_url record (see the header). A small fixed ring would hide
// the order a spec asserts on, so this is a plain capped list: past the cap,
// later URLs are dropped rather than overwriting earlier ones.
// ---------------------------------------------------------------------------
#define OPENED_URL_MAX 64
static char* g_opened_urls[OPENED_URL_MAX];
static int   g_opened_url_count = 0;

void aether_ui_opened_url_record(const char* url) {
    if (!url || g_opened_url_count >= OPENED_URL_MAX) return;
    size_t n = strlen(url);
    char* copy = (char*)malloc(n + 1);
    if (!copy) return;
    memcpy(copy, url, n + 1);
    g_opened_urls[g_opened_url_count++] = copy;
}

int aether_ui_opened_url_count(void) { return g_opened_url_count; }

const char* aether_ui_opened_url_at(int index) {
    if (index < 0 || index >= g_opened_url_count) return "";
    return g_opened_urls[index];
}

// ---------------------------------------------------------------------------
// Headless dialog answers (see the header). The driver thread queues and
// the UI thread consumes, so both sides take one lock. Records are written
// once, complete, before the count that publishes them moves, and never
// change after -- so the accessors can hand out their strings.
// ---------------------------------------------------------------------------
#ifdef _WIN32
static SRWLOCK g_prompt_lock = SRWLOCK_INIT;
#define PROMPT_LOCK()   AcquireSRWLockExclusive(&g_prompt_lock)
#define PROMPT_UNLOCK() ReleaseSRWLockExclusive(&g_prompt_lock)
#else
static pthread_mutex_t g_prompt_lock = PTHREAD_MUTEX_INITIALIZER;
#define PROMPT_LOCK()   pthread_mutex_lock(&g_prompt_lock)
#define PROMPT_UNLOCK() pthread_mutex_unlock(&g_prompt_lock)
#endif

#define PROMPT_KINDS      4
#define PROMPT_QUEUE_MAX  64
#define PROMPT_RECORD_MAX 256

static const char* const g_prompt_kind_names[PROMPT_KINDS] = {
    "open", "save", "folder", "alert"
};

typedef struct {
    int   kind;
    int   scripted;
    char* title;
    char* detail;
    char* answer;
} PromptRec;

// One FIFO per kind: head is the next answer out, count how many wait.
static char* g_prompt_queue[PROMPT_KINDS][PROMPT_QUEUE_MAX];
static int   g_prompt_q_head[PROMPT_KINDS];
static int   g_prompt_q_count[PROMPT_KINDS];
static PromptRec g_prompts[PROMPT_RECORD_MAX];
static int   g_prompt_count = 0;

static char* prompt_dup(const char* s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char* c = (char*)malloc(n + 1);
    if (c) memcpy(c, s, n + 1);
    return c;
}

static int prompt_kind_of(const char* name) {
    if (!name) return -1;
    for (int k = 0; k < PROMPT_KINDS; k++)
        if (strcmp(name, g_prompt_kind_names[k]) == 0) return k;
    return -1;
}

const char* aether_ui_prompt_kind_name(int kind) {
    if (kind < 0 || kind >= PROMPT_KINDS) return "";
    return g_prompt_kind_names[kind];
}

int aether_ui_prompt_queue_answer(const char* kind_name, const char* value) {
    int k = prompt_kind_of(kind_name);
    if (k < 0) return -1;
    if (k == AEUI_PROMPT_ALERT) return -2;
    char* copy = prompt_dup(value);
    if (!copy) return -3;
    PROMPT_LOCK();
    if (g_prompt_q_count[k] >= PROMPT_QUEUE_MAX) {
        PROMPT_UNLOCK();
        free(copy);
        return -3;
    }
    int slot = (g_prompt_q_head[k] + g_prompt_q_count[k]) % PROMPT_QUEUE_MAX;
    g_prompt_queue[k][slot] = copy;
    g_prompt_q_count[k]++;
    PROMPT_UNLOCK();
    return 0;
}

char* aether_ui_prompt_headless(int kind, const char* title, const char* detail) {
    if (kind < 0 || kind >= PROMPT_KINDS) return prompt_dup("");
    char* queued = NULL;
    PROMPT_LOCK();
    if (g_prompt_q_count[kind] > 0) {
        queued = g_prompt_queue[kind][g_prompt_q_head[kind]];
        g_prompt_queue[kind][g_prompt_q_head[kind]] = NULL;
        g_prompt_q_head[kind] = (g_prompt_q_head[kind] + 1) % PROMPT_QUEUE_MAX;
        g_prompt_q_count[kind]--;
    }
    // Past the cap later requests still get their answer; only the record
    // is dropped, so earlier entries a spec asserts on keep their order.
    if (g_prompt_count < PROMPT_RECORD_MAX) {
        PromptRec* r = &g_prompts[g_prompt_count];
        r->kind = kind;
        r->scripted = queued ? 1 : 0;
        r->title = prompt_dup(title);
        r->detail = prompt_dup(detail);
        r->answer = prompt_dup(queued ? queued : "");
        g_prompt_count++;
    }
    PROMPT_UNLOCK();
    if (queued) return queued;   // already a malloc'd copy: hand it over
    return prompt_dup("");
}

int aether_ui_prompt_count(void) {
    PROMPT_LOCK();
    int n = g_prompt_count;
    PROMPT_UNLOCK();
    return n;
}

static const PromptRec* prompt_at(int index) {
    if (index < 0 || index >= aether_ui_prompt_count()) return NULL;
    return &g_prompts[index];
}

int aether_ui_prompt_kind_at(int index) {
    const PromptRec* r = prompt_at(index);
    return r ? r->kind : -1;
}
const char* aether_ui_prompt_title_at(int index) {
    const PromptRec* r = prompt_at(index);
    return r && r->title ? r->title : "";
}
const char* aether_ui_prompt_detail_at(int index) {
    const PromptRec* r = prompt_at(index);
    return r && r->detail ? r->detail : "";
}
const char* aether_ui_prompt_answer_at(int index) {
    const PromptRec* r = prompt_at(index);
    return r && r->answer ? r->answer : "";
}
int aether_ui_prompt_scripted_at(int index) {
    const PromptRec* r = prompt_at(index);
    return r ? r->scripted : 0;
}

int aether_ui_prompt_queued(const char* kind_name) {
    int n = 0;
    int only = (kind_name && *kind_name) ? prompt_kind_of(kind_name) : -2;
    if (only == -1) return 0;
    PROMPT_LOCK();
    for (int k = 0; k < PROMPT_KINDS; k++)
        if (only == -2 || only == k) n += g_prompt_q_count[k];
    PROMPT_UNLOCK();
    return n;
}

// ---------------------------------------------------------------------------
// Cross-backend headless park fallback.
//
// `aether_ui_app_run_headless_impl` lives in each backend file because
// the GTK4 backend wants to run a real GMainLoop (so SNI/DBusMenu
// signals get delivered), while macOS/Win32 need their own equivalents
// when those backends gain native tray support. This sleep loop is the
// shared no-op the backends fall through to when they can't or don't
// want to pump a real loop.
/* #93: an app had no way to stop its own run loop, so any bounded run had to
 * be killed from outside and `timeout 60 ./app` always exited 124 — which
 * cannot tell "finished its work" from "hung", and reports failure on success.
 * aether_ui_app_quit sets this, and every loop that can park checks it. */
static volatile int aeui_quit_requested = 0;

void aether_ui_request_quit(void) { aeui_quit_requested = 1; }
int  aether_ui_quit_requested(void) { return aeui_quit_requested; }

void aether_ui_park_until_killed(void) {
    /* Poll rather than sleep for a minute at a time: this is the path a
     * headless or tray-only app parks on, and app_quit has to be able to end
     * it. A tenth of a second is far below anything a human notices at exit
     * and costs nothing while idle. */
    while (!aeui_quit_requested) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }
}

// ---------------------------------------------------------------------------
// AeCS shared state (styles layer — docs/design/styling.md).
// Lives here because this TU links into every backend build: the "current
// sheet" (use_styles) that widget constructors consult, and the
// appearance-change registry behind styles_for_mode auto re-theming.
// ---------------------------------------------------------------------------

// Opaque Aether-heap sheet ptr; the DSL owns it, C only holds it.
static void* aecs_current_sheet = NULL;
void aether_ui_current_sheet_set_impl(void* sheet) { aecs_current_sheet = sheet; }
void* aether_ui_current_sheet_get_impl(void) { return aecs_current_sheet; }

// The widget scope a `commands() { … }` block was opened in. Held here for the
// same reason the sheet is: Aether has no top-level mutable module state. An
// `action { … }` inside captures this so its on_* verbs know which container
// to attach to -- inside that block the ambient _ctx is the COMMAND, and
// Aether exposes only builder_context() (top of stack). Single slot because
// commands scopes do not nest.
static void* aeui_commands_scope = NULL;
void aether_ui_commands_scope_set_impl(void* scope) { aeui_commands_scope = scope; }
void* aether_ui_commands_scope_get_impl(void) { return aeui_commands_scope; }

// The DSL's state-observer table and the closure that receives every set (see
// aether_ui_backend.h). One copy for all four backends: before this each kept
// its own observer array and fired it synchronously per set, so a computed
// cell over two inputs recomputed twice when both changed, and there was no
// place to coalesce that short of editing four files the same way.
static void* aeui_state_hub = NULL;
static void* aeui_state_notify_boxed = NULL;
void aether_ui_state_hub_set_impl(void* hub, void* boxed_notify) {
    aeui_state_hub = hub;
    aeui_state_notify_boxed = boxed_notify;
}
void* aether_ui_state_hub_get_impl(void) { return aeui_state_hub; }

// background_for's in-flight jobs (aether_ui_backend.h): id + cancel flag.
// A small table of the jobs still running, not one flag per id ever issued,
// so a long-lived app that runs millions of jobs does not grow it. Ids are
// never reused, so a stale id cannot read a later job's flag.
#define BG_JOB_MAX 1024
typedef struct { int id; int cancelled; } BgJob;
static BgJob g_bg_jobs[BG_JOB_MAX];
static int   g_bg_next_id = 0;
#ifdef _WIN32
static SRWLOCK g_bg_lock = SRWLOCK_INIT;
#define BG_LOCK()   AcquireSRWLockExclusive(&g_bg_lock)
#define BG_UNLOCK() ReleaseSRWLockExclusive(&g_bg_lock)
#else
static pthread_mutex_t g_bg_lock = PTHREAD_MUTEX_INITIALIZER;
#define BG_LOCK()   pthread_mutex_lock(&g_bg_lock)
#define BG_UNLOCK() pthread_mutex_unlock(&g_bg_lock)
#endif

int aether_ui_bg_job_begin(void) {
    int id = 0;
    BG_LOCK();
    for (int i = 0; i < BG_JOB_MAX; i++) {
        if (g_bg_jobs[i].id == 0) {
            if (g_bg_next_id == 0x7fffffff) g_bg_next_id = 0;
            id = ++g_bg_next_id;
            g_bg_jobs[i].id = id;
            g_bg_jobs[i].cancelled = 0;
            break;
        }
    }
    BG_UNLOCK();
    return id;
}

static BgJob* bg_job_find(int job) {   // caller holds the lock
    if (job <= 0) return NULL;
    for (int i = 0; i < BG_JOB_MAX; i++)
        if (g_bg_jobs[i].id == job) return &g_bg_jobs[i];
    return NULL;
}

void aether_ui_bg_job_cancel(int job) {
    BG_LOCK();
    BgJob* j = bg_job_find(job);
    if (j) j->cancelled = 1;
    BG_UNLOCK();
}

int aether_ui_bg_job_cancelled(int job) {
    BG_LOCK();
    BgJob* j = bg_job_find(job);
    int c = j ? j->cancelled : 0;
    BG_UNLOCK();
    return c;
}

void aether_ui_bg_job_end(int job) {
    BG_LOCK();
    BgJob* j = bg_job_find(job);
    if (j) { j->id = 0; j->cancelled = 0; }
    BG_UNLOCK();
}
void aether_ui_state_notify(int state_handle) {
    AeClosureLocal* c = (AeClosureLocal*)aeui_state_notify_boxed;
    if (!c || !c->fn) return;   // nothing observes any cell yet
    ((void (*)(void*, int))c->fn)(c->env, state_handle);
}

// A builder _ctx is an opaque void*, but a widget/menu handle is an int -- and
// Aether will not cast between ptr and int. The widget path already does this
// conversion in C (aether_ui_widget_add_child_ctx does `(int)(intptr_t)ctx`);
// this exposes the same one for DSL scopes whose ambient context is a handle
// rather than a struct, e.g. `menu("File") { item("Save") … }`.
int aether_ui_ctx_to_handle_impl(void* ctx) { return (int)(intptr_t)ctx; }

// Single-slot appearance callback (|dark: int| closure) + the driver's
// override (-1 = follow the OS; 0/1 forced via POST /appearance).
static void* appearance_boxed = NULL;
static int appearance_override = -1;
void aether_ui_appearance_register_impl(void* boxed) { appearance_boxed = boxed; }
int aether_ui_appearance_override_get(void) { return appearance_override; }
void aether_ui_appearance_override_set(int dark) { appearance_override = dark; }

// Invoke the registered callback with the new mode. Backends call this from
// their OS event ON THE UI THREAD; the driver's fire goes through the
// per-backend aether_ui_fire_appearance, which owns thread marshalling.
int aether_ui_appearance_invoke(int dark) {
    AeClosureLocal* c = (AeClosureLocal*)appearance_boxed;
    if (!c || !c->fn) return 0;
    ((void (*)(void*, long long))c->fn)(c->env, (long long)dark);
    return 1;
}

// ---------------------------------------------------------------------------
// Undo/redo edit stack (the Swing UndoManager shape; docs: roadmap backlog).
// Edits [0, cursor) are undoable, [cursor, len) redoable. Pushing a new edit
// truncates the redo tail (the classic rule). Closures are zero-arg.
// ---------------------------------------------------------------------------
#define AEUI_UNDO_CAP 128
/* A plain edit carries one undo/redo pair. A GROUP carries the pairs of the
 * edits collapsed into it (see aether_ui_undo_group_end_impl): undoing runs
 * them in reverse, redoing runs them forward, and the whole thing is one step
 * with one label. A drag that records thirty edits becomes one gesture to
 * undo, which is what a user means by "undo that move". */
typedef struct {
    char* label;
    void* undo_boxed;
    void* redo_boxed;
    void** g_undo;      /* group members, oldest first; NULL for a plain edit */
    void** g_redo;
    int    g_count;
} AeUndoEdit;
static AeUndoEdit undo_stack[AEUI_UNDO_CAP];
static int undo_len = 0, undo_cursor = 0;

/* An open group. Only the OUTERMOST begin/end pair collapses, so a helper that
 * groups internally still contributes one step to a caller's larger group
 * rather than fragmenting it. */
static int undo_group_depth = 0;
static int undo_group_start = 0;
static char* undo_group_label = NULL;
/* Counts pushes, so "the group recorded nothing" is exact. Comparing lengths
 * is not: after an undo the stack still holds the redo tail, so an EMPTY group
 * would measure that tail as its own member, relabel it and move the cursor
 * back over it, resurrecting an edit the user had just undone. */
static unsigned long undo_push_seq = 0;
static unsigned long undo_group_seq0 = 0;

/* Reclaim one edit: its label and BOTH closure boxes.
 *
 * An edit owns two boxed closures, and dropping one used to free only the
 * label. Undo-then-edit is the ordinary way to work, and it truncates the redo
 * tail every time, so an editor leaked two boxes per edit for as long as it
 * ran. Each box is freed through the env's own destructor, so the references
 * its captures own are released too, not just the struct (the same contract
 * list_free uses for an owned closure element). env is NULL for a
 * non-capturing closure and aether_closure_env_free is a no-op on it. */
static void undo_box_release(void* boxed) {
    if (!boxed) return;
    aether_closure_env_free(((AeClosureLocal*)boxed)->env);
    free(boxed);
}

static void undo_edit_release(AeUndoEdit* e) {
    free(e->label);
    e->label = 0;
    undo_box_release(e->undo_boxed); e->undo_boxed = 0;
    undo_box_release(e->redo_boxed); e->redo_boxed = 0;
    /* A group owns every member box it absorbed, and the two arrays holding
     * them. Dropping a group without this leaks the whole gesture. */
    for (int i = 0; i < e->g_count; i++) {
        if (e->g_undo) undo_box_release(e->g_undo[i]);
        if (e->g_redo) undo_box_release(e->g_redo[i]);
    }
    free(e->g_undo); e->g_undo = 0;
    free(e->g_redo); e->g_redo = 0;
    e->g_count = 0;
}

void aether_ui_undo_push_impl(const char* label, void* undo_boxed, void* redo_boxed) {
    for (int i = undo_cursor; i < undo_len; i++) undo_edit_release(&undo_stack[i]);
    undo_len = undo_cursor;
    if (undo_len >= AEUI_UNDO_CAP) {          // drop the oldest edit
        undo_edit_release(&undo_stack[0]);
        memmove(undo_stack, undo_stack + 1, sizeof(AeUndoEdit) * (AEUI_UNDO_CAP - 1));
        undo_len--; undo_cursor--;
    }
    undo_stack[undo_len].label = strdup(label ? label : "");
    undo_stack[undo_len].undo_boxed = undo_boxed;
    undo_stack[undo_len].redo_boxed = redo_boxed;
    undo_stack[undo_len].g_undo = 0;
    undo_stack[undo_len].g_redo = 0;
    undo_stack[undo_len].g_count = 0;
    undo_len++; undo_cursor = undo_len;
    undo_push_seq++;
}

// Direct steps — call ON THE UI THREAD (app-side undo()/redo(), or the
// per-backend marshalled aether_ui_fire_undo/redo for driver routes).
int aether_ui_undo_step_impl(void) {
    if (undo_cursor <= 0) return 0;
    undo_cursor--;
    AeUndoEdit* e = &undo_stack[undo_cursor];
    if (e->g_count > 0) {
        /* Reverse order: the members were applied oldest first, so undoing
         * newest first is the only order that restores the starting state. */
        for (int i = e->g_count - 1; i >= 0; i--) invoke_closure(e->g_undo[i]);
    } else {
        invoke_closure(e->undo_boxed);
    }
    return 1;
}
int aether_ui_redo_step_impl(void) {
    if (undo_cursor >= undo_len) return 0;
    AeUndoEdit* e = &undo_stack[undo_cursor];
    if (e->g_count > 0) {
        for (int i = 0; i < e->g_count; i++) invoke_closure(e->g_redo[i]);
    } else {
        invoke_closure(e->redo_boxed);
    }
    undo_cursor++;
    return 1;
}
/* Begin collapsing every edit recorded until the matching end into ONE step.
 *
 * Only the outermost pair collapses: a helper that groups internally then
 * contributes a single step to a caller's larger group rather than fragmenting
 * it, which is the behaviour a caller wants and cannot get by nesting spans. */
void aether_ui_undo_group_begin_impl(const char* label) {
    if (undo_group_depth++ > 0) return;
    undo_group_start = undo_cursor;
    undo_group_seq0 = undo_push_seq;
    free(undo_group_label);
    undo_group_label = strdup(label ? label : "");
}

/* Collapse the edits recorded since the matching begin.
 *
 * The members are MOVED into the group entry, boxes and all, so ownership does
 * not change hands: undo_edit_release frees them with the group. A group of one
 * stays a plain edit wearing the group's label, and a group of none records
 * nothing at all, because a gesture that changed nothing should not cost the
 * user an undo press. */
void aether_ui_undo_group_end_impl(void) {
    if (undo_group_depth == 0) return;
    if (--undo_group_depth > 0) return;

    /* Nothing recorded: leave the stack exactly as it was, redo tail and all. */
    if (undo_push_seq == undo_group_seq0) return;

    int start = undo_group_start;
    if (start < 0) start = 0;
    int n = undo_len - start;
    if (n <= 0) return;

    if (n == 1) {
        free(undo_stack[start].label);
        undo_stack[start].label = strdup(undo_group_label ? undo_group_label : "");
        undo_cursor = undo_len;
        return;
    }

    void** gu = (void**)malloc(sizeof(void*) * (size_t)n);
    void** gr = (void**)malloc(sizeof(void*) * (size_t)n);
    if (!gu || !gr) { free(gu); free(gr); return; }
    for (int i = 0; i < n; i++) {
        gu[i] = undo_stack[start + i].undo_boxed;
        gr[i] = undo_stack[start + i].redo_boxed;
        free(undo_stack[start + i].label);
        undo_stack[start + i].label = 0;
        undo_stack[start + i].undo_boxed = 0;
        undo_stack[start + i].redo_boxed = 0;
    }
    undo_stack[start].label = strdup(undo_group_label ? undo_group_label : "");
    undo_stack[start].g_undo = gu;
    undo_stack[start].g_redo = gr;
    undo_stack[start].g_count = n;
    undo_len = start + 1;
    undo_cursor = undo_len;
}

int aether_ui_undo_group_active_impl(void) { return undo_group_depth > 0; }

int aether_ui_undo_depth_impl(void) { return undo_cursor; }
int aether_ui_redo_depth_impl(void) { return undo_len - undo_cursor; }
const char* aether_ui_undo_label_impl(void) {
    return undo_cursor > 0 ? undo_stack[undo_cursor - 1].label : "";
}

// ─── vg outline-font holder ──────────────────────────────────────────────
// Aether has no module-level mutable globals, so vg/module.ae's text-path
// verbs keep the current outline font (a vg.font TtfFont*) here. Process-
// wide by design: one font serves every scene, use_font() swaps it.
static void* g_aeui_vgfont = 0;
void* aether_ui_vgfont_get(void) { return g_aeui_vgfont; }
void aether_ui_vgfont_set(void* f) { g_aeui_vgfont = f; }

// ─── chrome-drawn face renderer registry ─────────────────────────────────
// vg-drawn controls phase 2: ui.chromed registers a face-renderer closure
// here; ui/module.ae's btn() invokes it when AETHER_UI_CHROME=drawn. The
// cell keeps ui vg-free (apps not opting in never link the vg tree) and
// dodges a transitive-inline compiler trip (see aether asks). Same C-cell
// pattern as the commands scope above — Aether has no module-level state.
static void* g_aeui_chrome_face = 0;   // boxed |handle: int, label: string|
void aether_ui_chrome_face_set_impl(void* boxed) { g_aeui_chrome_face = boxed; }
void* aether_ui_chrome_face_get_impl(void) { return g_aeui_chrome_face; }

// Toggle twin of the face renderer: ui.chromed registers a CREATION-OWNING
// closure |label: string, user_boxed: ptr| -> int here — it must build the
// native toggle itself so it can wrap the user's on_change with a face
// re-render (the knob/track must move on every change, wherever it came
// from: pointer, driver /toggle, or set_toggle).
static void* g_aeui_chrome_toggle = 0;
void aether_ui_chrome_toggle_set_impl(void* boxed) { g_aeui_chrome_toggle = boxed; }
void* aether_ui_chrome_toggle_get_impl(void) { return g_aeui_chrome_toggle; }

// ─── One-shot timer and the frame clock ──────────────────────────────────
// The contract is in aether_ui_backend.h ("One-shot timer and the frame
// clock"). Both are shared so a backend supplies only what is native to it:
// the repeating timer it already has, and its display clock
// (aether_ui_frame_clock_*_impl).
//
// Closure boxes made here: a C box is {fn, env} like the compiler's, and a
// backend releases a timer's box with aether_closure_env_free(env) + free.
// That function reads the env's first field as its destructor, so an env
// made here starts with one (AeuiCEnv), and a box with no env passes NULL.
extern int  aether_ui_timer_create_impl(int interval_ms, void* boxed_closure);
extern void aether_ui_timer_cancel_impl(int timer_id);
extern int  aether_ui_frame_clock_start_impl(void);
extern void aether_ui_frame_clock_stop_impl(void);
extern const char* aether_ui_frame_clock_name_impl(void);
extern int  aether_ui_frame_clock_hz_impl(void);

static void aeui_box_release(void* boxed) {
    if (!boxed) return;
    aether_closure_env_free(((AeClosureLocal*)boxed)->env);
    free(boxed);
}

static void* aeui_c_box(void* fn, void* env) {
    AeClosureLocal* b = (AeClosureLocal*)malloc(sizeof(AeClosureLocal));
    if (!b) return NULL;
    b->fn = fn;
    b->env = env;
    return b;
}

// Milliseconds on the shared monotonic clock.
static double aeui_mono_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER hz;
    LARGE_INTEGER now;
    if (hz.QuadPart == 0) QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)hz.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

// timer_once(ms, fn). The env owns the caller's box; the backend releases the
// wrapper box (and so, through the destructor, the env and the caller's box)
// when the timer is cancelled -- which the first tick does, before it calls
// fn. Every backend releases a cancelled timer's box only after the tick that
// cancelled it has returned (GLib's notify, a deferred main-queue release, the
// Win32 graveyard), so the env outlives the call.
typedef struct {
    void (*dtor)(void*);
    void* user_box;
    int   timer_id;
    int   fired;
} AeuiAfterEnv;

static void aeui_after_env_free(void* env) {
    AeuiAfterEnv* a = (AeuiAfterEnv*)env;
    aeui_box_release(a->user_box);
    free(a);
}

static void aeui_after_tick(void* env) {
    AeuiAfterEnv* a = (AeuiAfterEnv*)env;
    if (!a || a->fired) return;
    a->fired = 1;
    aether_ui_timer_cancel_impl(a->timer_id);   // before fn: exactly once
    invoke_closure(a->user_box);
}

int aether_ui_timer_once_impl(int delay_ms, void* boxed_closure) {
    if (!boxed_closure) return 0;
    if (delay_ms < 1) delay_ms = 1;   // the soonest a timer can fire
    AeuiAfterEnv* a = (AeuiAfterEnv*)calloc(1, sizeof(AeuiAfterEnv));
    if (!a) { aeui_box_release(boxed_closure); return 0; }
    a->dtor = aeui_after_env_free;
    a->user_box = boxed_closure;
    void* box = aeui_c_box((void*)aeui_after_tick, a);
    if (!box) { aeui_after_env_free(a); return 0; }
    int id = aether_ui_timer_create_impl(delay_ms, box);
    if (id <= 0) { aeui_box_release(box); return 0; }
    a->timer_id = id;
    return id;
}

// The frame clock. Subscribers are {id, box}; a cancelled one is marked dead
// and swept when no dispatch is running, so a subscriber may cancel itself
// (or another) from inside its own frame.
typedef struct { int id; void* box; int alive; } AeuiFrameSub;
static AeuiFrameSub* g_frame_subs = NULL;
static int g_frame_n = 0, g_frame_cap = 0, g_frame_next_id = 1, g_frame_live = 0;
static int g_frame_dispatching = 0;
static int g_frame_native = 0;            // a native clock was started
static double g_frame_native_seen = -1e18; // shared-clock time of its last frame
static double g_frame_last_ts = 0.0;      // strictly increasing
static const char* g_frame_source = "none";
static int g_frame_count = 0;
// The fallback timer: WATCH (every 100 ms, is the native clock alive?) or
// DRIVE (at the display rate, deliver frames itself).
#define AEUI_FRAME_WATCH 1
#define AEUI_FRAME_DRIVE 2
#define AEUI_FRAME_STALL_MS 100.0
static int g_frame_timer = 0, g_frame_mode = 0, g_frame_retry = 0;

static void aeui_frame_sweep(void) {
    int w = 0;
    for (int i = 0; i < g_frame_n; i++) {
        if (g_frame_subs[i].alive) g_frame_subs[w++] = g_frame_subs[i];
        else aeui_box_release(g_frame_subs[i].box);
    }
    g_frame_n = w;
}

static void aeui_frame_fallback_set(int mode);

static void aeui_frame_stop_all(void) {
    if (g_frame_native) { aether_ui_frame_clock_stop_impl(); g_frame_native = 0; }
    if (g_frame_timer) { aether_ui_timer_cancel_impl(g_frame_timer); g_frame_timer = 0; }
    g_frame_mode = 0;
}

// Deliver one frame at shared-clock time ts to every live subscriber.
static void aeui_frame_deliver(double ts, const char* source) {
    if (ts <= g_frame_last_ts) ts = g_frame_last_ts + 0.001;
    g_frame_last_ts = ts;
    g_frame_source = source;
    g_frame_count++;
    g_frame_dispatching++;
    int n = g_frame_n;   // one added during this frame waits for the next
    for (int i = 0; i < n; i++) {
        if (!g_frame_subs[i].alive) continue;
        AeClosureLocal* c = (AeClosureLocal*)g_frame_subs[i].box;
        if (c && c->fn) ((void (*)(void*, double))c->fn)(c->env, ts);
    }
    g_frame_dispatching--;
    if (g_frame_dispatching == 0) {
        aeui_frame_sweep();
        if (g_frame_live == 0) aeui_frame_stop_all();
    }
}

void aether_ui_frame_dispatch(double native_ts_ms, double native_now_ms) {
    double now = aeui_mono_ms();
    g_frame_native_seen = now;
    if (g_frame_live == 0) return;
    // The frame's age on the native clock carried onto the shared one. A
    // frame time in the future (a display link's target time) counts as now.
    double age = native_now_ms - native_ts_ms;
    if (age < 0 || age > 1000.0) age = 0;
    aeui_frame_deliver(now - age, aether_ui_frame_clock_name_impl());
}

static double aeui_frame_period_ms(void) {
    int hz = aether_ui_frame_clock_hz_impl();
    if (hz < 1 || hz > 1000) hz = 60;
    return 1000.0 / (double)hz;
}

static void aeui_frame_fallback_tick(void* env) {
    (void)env;
    if (g_frame_live == 0) { aeui_frame_stop_all(); return; }
    double now = aeui_mono_ms();
    double period = aeui_frame_period_ms();
    if (g_frame_mode == AEUI_FRAME_WATCH) {
        if (now - g_frame_native_seen <= AEUI_FRAME_STALL_MS) return;
        aeui_frame_fallback_set(AEUI_FRAME_DRIVE);   // the native clock stalled
        aeui_frame_deliver(now, "timer");
        return;
    }
    // DRIVE. Native frames are back: hand over to them.
    if (g_frame_native && now - g_frame_native_seen < 2.0 * period + 1.0) {
        aeui_frame_fallback_set(AEUI_FRAME_WATCH);
        return;
    }
    // None running (no window yet, no compositor): try again about once a
    // second, so one that becomes possible later takes over.
    if (!g_frame_native && ++g_frame_retry * period >= 1000.0) {
        g_frame_retry = 0;
        g_frame_native = aether_ui_frame_clock_start_impl();
    }
    aeui_frame_deliver(now, "timer");
}

static void aeui_frame_fallback_set(int mode) {
    if (g_frame_mode == mode && g_frame_timer) return;
    if (g_frame_timer) aether_ui_timer_cancel_impl(g_frame_timer);
    g_frame_timer = 0;
    g_frame_mode = mode;
    g_frame_retry = 0;
    int ms = 100;
    if (mode == AEUI_FRAME_DRIVE) {
        ms = (int)(aeui_frame_period_ms() + 0.5);
        if (ms < 1) ms = 1;
    }
    void* box = aeui_c_box((void*)aeui_frame_fallback_tick, NULL);
    if (!box) return;
    g_frame_timer = aether_ui_timer_create_impl(ms, box);
    if (g_frame_timer <= 0) { g_frame_timer = 0; aeui_box_release(box); }
}

int aether_ui_frame_add_impl(void* boxed_closure) {
    if (!boxed_closure) return 0;
    if (g_frame_n >= g_frame_cap) {
        int cap = g_frame_cap ? g_frame_cap * 2 : 8;
        AeuiFrameSub* ns = (AeuiFrameSub*)realloc(g_frame_subs, sizeof(AeuiFrameSub) * (size_t)cap);
        if (!ns) { aeui_box_release(boxed_closure); return 0; }
        g_frame_subs = ns;
        g_frame_cap = cap;
    }
    int id = g_frame_next_id++;
    g_frame_subs[g_frame_n].id = id;
    g_frame_subs[g_frame_n].box = boxed_closure;
    g_frame_subs[g_frame_n].alive = 1;
    g_frame_n++;
    g_frame_live++;
    if (g_frame_live == 1 && !g_frame_dispatching) {
        g_frame_native = aether_ui_frame_clock_start_impl();
        g_frame_native_seen = aeui_mono_ms();   // give it one stall window
        aeui_frame_fallback_set(g_frame_native ? AEUI_FRAME_WATCH : AEUI_FRAME_DRIVE);
    } else if (!g_frame_timer && !g_frame_native) {
        // Added from inside the frame that was about to stop the clock.
        g_frame_native = aether_ui_frame_clock_start_impl();
        g_frame_native_seen = aeui_mono_ms();
        aeui_frame_fallback_set(g_frame_native ? AEUI_FRAME_WATCH : AEUI_FRAME_DRIVE);
    }
    return id;
}

void aether_ui_frame_cancel_impl(int frame_id) {
    for (int i = 0; i < g_frame_n; i++) {
        if (g_frame_subs[i].id != frame_id || !g_frame_subs[i].alive) continue;
        g_frame_subs[i].alive = 0;
        g_frame_live--;
        if (!g_frame_dispatching) {
            aeui_frame_sweep();
            if (g_frame_live == 0) aeui_frame_stop_all();
        }
        return;
    }
}

const char* aether_ui_frame_source_impl(void) { return g_frame_source; }
int aether_ui_frame_count_impl(void) { return g_frame_count; }
int aether_ui_frame_subscribers_impl(void) { return g_frame_live; }
int aether_ui_frame_running_impl(void) { return (g_frame_native || g_frame_timer) ? 1 : 0; }
