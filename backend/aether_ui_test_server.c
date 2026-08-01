// AetherUIDriver — shared HTTP test server.
//
// Platform-neutral HTTP + routing + JSON for the AetherUIDriver. The
// three native backends (GTK4, AppKit, Win32) provide a small hook table
// (see aether_ui_test_server.h) and call aether_ui_test_server_start().
//
// Before this extraction, each backend had its own ~500 LOC copy of the
// socket accept loop, HTTP parser, URL router, JSON emitter, and sealed-
// widget bookkeeping. This file consolidates all of that to a single
// source of truth so feature additions (new endpoints, new filters)
// land on every backend at once.
//
// Socket layer:
//   _WIN32   → winsock2 (WSAStartup, closesocket, ioctlsocket)
//   POSIX    → <sys/socket.h> + <unistd.h>
//
// Threading:
//   _WIN32   → CreateThread
//   POSIX    → pthread_create + pthread_detach

#include "aether_ui_test_server.h"
#include "aether_ui_backend.h"
#include "aether_ui_system_extras.h"

#include <stdio.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET aether_sock_t;
#define AETHER_SOCK_INVALID INVALID_SOCKET
#define aether_close_socket closesocket
#define aether_socket_recv(s, buf, len) recv((s), (buf), (len), 0)
#define aether_socket_send(s, buf, len) send((s), (buf), (len), 0)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
typedef int aether_sock_t;
#define AETHER_SOCK_INVALID (-1)
#define aether_close_socket close
#define aether_socket_recv(s, buf, len) read((s), (buf), (len))
#define aether_socket_send(s, buf, len) write((s), (buf), (len))
#endif

// Reactive state externs from the backend-neutral state layer (each
// backend defines these; used directly for /state/{id} endpoints).
extern double aether_ui_state_get(int handle);

// Backend ABI functions this server calls straight from the HTTP thread.
// They are pure reads over backend-side tables (no widget-tree traversal),
// which is the same latitude GTK4's embedded server takes for /text_extent
// and /state. Anything that touches the widget tree goes through
// dispatch_action instead.
extern double aether_ui_text_measure(double size, const char* text);
extern double aether_ui_font_ascent(double size);
extern double aether_ui_font_descent(double size);
extern double aether_ui_font_height(double size);
extern int    aether_ui_overlay_count_impl(void);
extern int    aether_ui_overlay_is_live_impl(int overlay_handle);
extern int    aether_ui_overlay_is_modal_impl(int overlay_handle);
extern int    aether_ui_overlay_is_exiting_impl(int overlay_handle);
extern void   aether_ui_overlay_close_impl(int overlay_handle);
extern int    aether_ui_split_position_impl(int handle);
extern int    aether_ui_picker_get_selected(int handle);
extern int    aether_ui_tabs_selected(int handle);
extern int    aether_ui_tabs_count(int handle);
extern void   aether_ui_tabs_select(int handle, int index);

// ---------------------------------------------------------------------------
// Sealed widget list + banner handle.
// ---------------------------------------------------------------------------
static int* sealed_widgets = NULL;
static int  sealed_count = 0;
static int  sealed_capacity = 0;
static int  banner_handle = 0;

void aether_ui_test_server_set_banner(int handle) {
    banner_handle = handle;
}

int aether_ui_test_server_banner_handle(void) {
    return banner_handle;
}

void aether_ui_test_server_seal_widget(int handle) {
    // Ignore duplicates so repeated seals are idempotent.
    for (int i = 0; i < sealed_count; i++) {
        if (sealed_widgets[i] == handle) return;
    }
    if (sealed_count >= sealed_capacity) {
        sealed_capacity = sealed_capacity == 0 ? 32 : sealed_capacity * 2;
        sealed_widgets = (int*)realloc(sealed_widgets,
                                       sizeof(int) * sealed_capacity);
    }
    sealed_widgets[sealed_count++] = handle;
}

int aether_ui_test_server_is_sealed(int handle) {
    for (int i = 0; i < sealed_count; i++) {
        if (sealed_widgets[i] == handle) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// HTTP parsing helpers.
// ---------------------------------------------------------------------------
static int parse_http_request(const char* req, char* path, int pathsize) {
    int method = 0;
    if (strncmp(req, "POST", 4) == 0) method = 1;
    const char* p = strchr(req, ' ');
    if (!p) return -1;
    p++;
    const char* end = strchr(p, ' ');
    if (!end) end = strchr(p, '\r');
    if (!end) end = p + strlen(p);
    int len = (int)(end - p);
    if (len >= pathsize) len = pathsize - 1;
    memcpy(path, p, len);
    path[len] = '\0';
    return method;
}

static int extract_id_from_path(const char* path, const char* prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return -1;
    return atoi(path + plen);
}

static const char* extract_query_param(const char* path, const char* key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char* p = strstr(path, needle);
    if (!p) return NULL;
    return p + strlen(needle);
}

// URL-decode s in place (tolerant: passes through malformed escapes).
static void url_decode(char* s) {
    char* out = s;
    for (char* in = s; *in; in++) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = in[1], lo = in[2];
            hi = hi >= 'a' ? hi - 'a' + 10 : hi >= 'A' ? hi - 'A' + 10 : hi - '0';
            lo = lo >= 'a' ? lo - 'a' + 10 : lo >= 'A' ? lo - 'A' + 10 : lo - '0';
            *out++ = (char)(hi * 16 + lo);
            in += 2;
        } else if (*in == '+') {
            *out++ = ' ';
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

// As url_decode, but '+' stays a literal '+' instead of becoming a space.
// Accelerator combos are the one place where that matters: "Ctrl+B" must
// survive the round trip, and form-style decoding would hand the backend
// "Ctrl B" and silently never match.
static void url_decode_keep_plus(char* s) {
    char* out = s;
    for (char* in = s; *in; in++) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = in[1], lo = in[2];
            hi = hi >= 'a' ? hi - 'a' + 10 : hi >= 'A' ? hi - 'A' + 10 : hi - '0';
            lo = lo >= 'a' ? lo - 'a' + 10 : lo >= 'A' ? lo - 'A' + 10 : lo - '0';
            *out++ = (char)(hi * 16 + lo);
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

// ---------------------------------------------------------------------------
// JSON emission + HTTP response send.
// ---------------------------------------------------------------------------
// Write the WHOLE buffer, looping over partial sends. A single send()/write()
// may transfer fewer bytes than asked — especially on Windows loopback for a
// large /widgets body — silently truncating the JSON so the spec's json.parse
// sees a cut-off array and widget_id_by_text misses everything past the cut.
// (The GTK4 embedded server learned this the hard way with its own write_all;
// the shared server behind win32/macOS never had the loop.)
static void send_all(aether_sock_t fd, const char* buf, int len) {
    int off = 0;
    while (off < len) {
        int n = (int)aether_socket_send(fd, buf + off, len - off);
        if (n <= 0) break;   // peer closed / fatal error — nothing more to do
        off += n;
    }
}

static void send_http(aether_sock_t fd, int status, const char* status_text,
                      const char* content_type, const char* body) {
    char header[512];
    int bodylen = body ? (int)strlen(body) : 0;
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, bodylen);
    send_all(fd, header, hlen);
    if (body && bodylen > 0) send_all(fd, body, bodylen);
}

static int widget_to_json(const AetherDriverHooks* h, int handle,
                          char* buf, int bufsize) {
    const char* type = h->widget_type(handle);
    if (!type || strcmp(type, "null") == 0) {
        return snprintf(buf, bufsize, "{\"id\":%d,\"type\":\"null\"}", handle);
    }

    char text[1024];
    h->widget_text_into(handle, text, sizeof(text));

    // Escape for JSON. Beyond " \ \n \r \t, ANY control char (U+0000–U+001F)
    // must be emitted as \uXXXX — a raw control byte in a widget's text (e.g. a
    // stray U+0001 in a search-result title) otherwise produces invalid JSON
    // that breaks every client-side json.parse (found via LisMusic on win32).
    // A \uXXXX expansion is 6 bytes; leave that much headroom.
    char esc[2560];
    int ei = 0;
    for (int i = 0; text[i] && ei < (int)sizeof(esc) - 8; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '"' || ch == '\\') { esc[ei++] = '\\'; esc[ei++] = (char)ch; }
        else if (ch == '\n') { esc[ei++] = '\\'; esc[ei++] = 'n'; }
        else if (ch == '\r') { esc[ei++] = '\\'; esc[ei++] = 'r'; }
        else if (ch == '\t') { esc[ei++] = '\\'; esc[ei++] = 't'; }
        else if (ch < 0x20) {
            // Any other control char → \u00XX.
            static const char hex[] = "0123456789abcdef";
            esc[ei++] = '\\'; esc[ei++] = 'u';
            esc[ei++] = '0'; esc[ei++] = '0';
            esc[ei++] = hex[(ch >> 4) & 0xF];
            esc[ei++] = hex[ch & 0xF];
        }
        else esc[ei++] = (char)ch;
    }
    esc[ei] = '\0';

    int visible   = h->widget_visible(handle);
    int sealed    = aether_ui_test_server_is_sealed(handle);
    int is_banner = (handle == banner_handle) ? 1 : 0;
    int parent    = h->widget_parent(handle);

    int n = snprintf(buf, bufsize,
        "{\"id\":%d,\"type\":\"%s\",\"text\":\"%s\",\"visible\":%s,"
        "\"sealed\":%s,\"banner\":%s,\"parent\":%d,\"window\":%d",
        handle, type, esc,
        visible ? "true" : "false",
        sealed  ? "true" : "false",
        is_banner ? "true" : "false",
        parent,
        aether_ui_widget_window_impl(handle));

    if (h->widget_enabled) {
        n += snprintf(buf + n, bufsize - n, ",\"enabled\":%s",
                      h->widget_enabled(handle) ? "true" : "false");
    }
    if (h->widget_rect) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (h->widget_rect(handle, &rx, &ry, &rw, &rh) == 0) {
            // A HIDDEN widget occupies no space, whatever the platform left
            // in its last-measured rect. GTK4 zeroes it naturally (unmapped
            // widgets have no allocation); win32 keeps the stale size, which
            // made "collapsed" indistinguishable from "just laid out" to a
            // spec. Normalise here so geometry means the same thing on every
            // backend.
            if (!visible) { rw = 0; rh = 0; }
            n += snprintf(buf + n, bufsize - n,
                          ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d", rx, ry, rw, rh);
        }
    }
    if (h->widget_classes_into) {
        char cls[256] = "";
        h->widget_classes_into(handle, cls, sizeof(cls));
        if (cls[0]) {
            n += snprintf(buf + n, bufsize - n, ",\"classes\":\"%s\"", cls);
        }
    }

    // Explicitly-styled colors (stylesheet layer): emitted only when set, so a
    // spec can prove a theme swap restyled the backend, not just the model.
    {
        int bg = aether_ui_styled_bg_impl(handle);
        int fg = aether_ui_styled_fg_impl(handle);
        if (bg >= 0) n += snprintf(buf + n, bufsize - n, ",\"bg\":\"#%06x\"", bg);
        if (fg >= 0) n += snprintf(buf + n, bufsize - n, ",\"fg\":\"#%06x\"", fg);
        const char* fam = aether_ui_styled_font_family_impl(handle);
        const char* wt  = aether_ui_styled_weight_impl(handle);
        if (fam[0]) n += snprintf(buf + n, bufsize - n, ",\"fontFamily\":\"%s\"", fam);
        if (wt[0])  n += snprintf(buf + n, bufsize - n, ",\"fontWeight\":\"%s\"", wt);
        int op = aether_ui_styled_opacity_impl(handle);
        if (op >= 0) n += snprintf(buf + n, bufsize - n, ",\"opacity\":%.2f", op / 100.0);
        if (h->widget_hovered && h->widget_hovered(handle))
            n += snprintf(buf + n, bufsize - n, ",\"hovered\":true");
        if (h->widget_pressed && h->widget_pressed(handle))
            n += snprintf(buf + n, bufsize - n, ",\"pressed\":true");
        int bd = aether_ui_styled_border_impl(handle);
        if (bd >= 0) {
            n += snprintf(buf + n, bufsize - n,
                          ",\"borderWidth\":%d,\"borderColor\":\"#%06x\"",
                          (bd >> 24) & 0x3F, bd & 0xFFFFFF);
        }
        int hv = aether_ui_state_style_impl(handle, 0);
        int ac = aether_ui_state_style_impl(handle, 1);
        if (hv >= 0) n += snprintf(buf + n, bufsize - n, ",\"hoverBg\":\"#%06x\"", hv);
        if (ac >= 0) n += snprintf(buf + n, bufsize - n, ",\"activeBg\":\"#%06x\"", ac);
    }

    // Accessibility: the widget's effective role + accessible name (auto when
    // unset). Emitted only when present so existing specs are unaffected. The
    // name reuses the same JSON escaping the text field needs.
    if (h->widget_a11y) {
        char role[64] = "", name[512] = "", desc[512] = "";
        h->widget_a11y(handle, role, sizeof(role), name, sizeof(name),
                       desc, sizeof(desc));
        if (role[0]) n += snprintf(buf + n, bufsize - n, ",\"role\":\"%s\"", role);
        if (name[0]) {
            char ne[1040]; int j = 0;
            for (int i = 0; name[i] && j < (int)sizeof(ne) - 8; i++) {
                unsigned char ch = (unsigned char)name[i];
                if (ch == '"' || ch == '\\') { ne[j++] = '\\'; ne[j++] = (char)ch; }
                else if (ch < 0x20) { ne[j++] = ' '; }
                else ne[j++] = (char)ch;
            }
            ne[j] = '\0';
            n += snprintf(buf + n, bufsize - n, ",\"a11y_name\":\"%s\"", ne);
        }
    }

    if (strcmp(type, "text") == 0) {
        static const char* an[] = {"start", "middle", "end"};
        int a = aether_ui_text_get_anchor(handle);
        if (a < 0 || a > 2) a = 0;
        n += snprintf(buf + n, bufsize - n, ",\"wrap\":%s,\"anchor\":\"%s\"",
                      aether_ui_text_get_wrap(handle) ? "true" : "false", an[a]);
    } else if (strcmp(type, "toggle") == 0) {
        n += snprintf(buf + n, bufsize - n, ",\"active\":%s",
                      h->toggle_active(handle) ? "true" : "false");
    } else if (strcmp(type, "slider") == 0) {
        n += snprintf(buf + n, bufsize - n, ",\"value\":%.2f",
                      h->slider_value(handle));
    } else if (strcmp(type, "progressbar") == 0) {
        n += snprintf(buf + n, bufsize - n, ",\"value\":%.2f",
                      h->progressbar_fraction(handle));
    } else if (strcmp(type, "picker") == 0) {
        n += snprintf(buf + n, bufsize - n, ",\"selected\":%d",
                      aether_ui_picker_get_selected(handle));
    } else if (strcmp(type, "splitview") == 0) {
        // Both of these are plain ABI reads, which is why they can be answered
        // here rather than through a hook. They were previously emitted ONLY by
        // GTK4's embedded server — which is why every split and picker spec was
        // red on win32 and macOS regardless of how good the backend was.
        n += snprintf(buf + n, bufsize - n, ",\"splitPosition\":%d",
                      aether_ui_split_position_impl(handle));
    } else if (strcmp(type, "tabs") == 0) {
        n += snprintf(buf + n, bufsize - n,
                      ",\"tabSelected\":%d,\"tabCount\":%d",
                      aether_ui_tabs_selected(handle),
                      aether_ui_tabs_count(handle));
    }
    n += snprintf(buf + n, bufsize - n, "}");
    return n;
}

// ---------------------------------------------------------------------------
// Request dispatch.
// ---------------------------------------------------------------------------
static void dispatch_and_reply(aether_sock_t client_fd,
                                const AetherDriverHooks* h,
                                AetherDriverActionCtx* ctx,
                                const char* ok_msg) {
    (void)ok_msg;  // responses are now a uniform JSON envelope (see below)
    h->dispatch_action(ctx);
    // Reply in the same JSON shape as the GTK driver (gtk4.c) so the shared
    // AetherUIDriver harness (test_automation.sh) passes identically on both
    // backends: success → {"ok":true}; seal/banner/not-found → {"error":...}.
    switch (ctx->result) {
        case 0:
            send_http(client_fd, 200, "OK", "application/json",
                      "{\"ok\":true}");
            break;
        case 1:
            send_http(client_fd, 403, "Forbidden", "application/json",
                      "{\"error\":\"widget is sealed\"}");
            break;
        case 2:
            send_http(client_fd, 403, "Forbidden", "application/json",
                      "{\"error\":\"banner cannot be manipulated\"}");
            break;
        default:
            send_http(client_fd, 404, "Not Found", "application/json",
                      "{\"error\":\"widget not found\"}");
            break;
    }
}

static void handle_request(aether_sock_t client_fd, const AetherDriverHooks* h) {
    char req[4096];
    int n = (int)aether_socket_recv(client_fd, req, sizeof(req) - 1);
    if (n <= 0) { aether_close_socket(client_fd); return; }
    req[n] = '\0';

    char path[1024];
    int method = parse_http_request(req, path, sizeof(path));
    if (method < 0) {
        send_http(client_fd, 400, "Bad Request", "text/plain", "bad request");
        aether_close_socket(client_fd);
        return;
    }

    // GET /widgets[?type=X][&text=Y]
    if (method == 0 && strncmp(path, "/widgets", 8) == 0
        && (path[8] == '\0' || path[8] == '?')) {
        const char* filter_type = extract_query_param(path, "type");
        const char* filter_text = extract_query_param(path, "text");
        char ft[128] = "", fx[128] = "";
        if (filter_type) {
            strncpy(ft, filter_type, sizeof(ft) - 1);
            char* amp = strchr(ft, '&'); if (amp) *amp = '\0';
            url_decode(ft);
        }
        if (filter_text) {
            strncpy(fx, filter_text, sizeof(fx) - 1);
            char* amp = strchr(fx, '&'); if (amp) *amp = '\0';
            url_decode(fx);
        }

        int total = h->widget_count();
        // Per-widget JSON caps at ~512 bytes; allocate a generous buffer.
        char* body = (char*)malloc((size_t)total * 4096 + 64);
        int pos = 0, first = 1;
        pos += sprintf(body + pos, "[");
        for (int i = 1; i <= total; i++) {
            const char* type = h->widget_type(i);
            if (!type || strcmp(type, "null") == 0) continue;
            if (ft[0] && strcmp(type, ft) != 0) continue;
            if (fx[0]) {
                char txt[1024];
                h->widget_text_into(i, txt, sizeof(txt));
                if (strcmp(txt, fx) != 0) continue;
            }
            if (!first) pos += sprintf(body + pos, ",");
            first = 0;
            pos += widget_to_json(h, i, body + pos, 4096);
        }
        pos += sprintf(body + pos, "]");
        send_http(client_fd, 200, "OK", "application/json", body);
        free(body);
    } else if (method == 0 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/children")) {
        int id = extract_id_from_path(path, "/widget/");
        if (!h->widget_children) {
            send_http(client_fd, 501, "Not Implemented", "text/plain",
                      "/children not supported by this backend");
        } else {
            int kids[256];
            int n = h->widget_children(id, kids, 256);
            if (n < 0) {
                send_http(client_fd, 404, "Not Found", "text/plain",
                          "widget not found");
            } else {
                char* body = (char*)malloc((size_t)n * 4096 + 64);
                int pos = 0;
                pos += sprintf(body + pos, "[");
                for (int i = 0; i < n; i++) {
                    if (i > 0) pos += sprintf(body + pos, ",");
                    pos += widget_to_json(h, kids[i], body + pos, 4096);
                }
                pos += sprintf(body + pos, "]");
                send_http(client_fd, 200, "OK", "application/json", body);
                free(body);
            }
        }
    } else if (method == 0 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/a11y")) {
        // GET /widget/{id}/a11y — the widget's effective accessible
        // role/name/description (auto when unset). Must precede the generic
        // /widget/{id} GET, whose match is a prefix of this path.
        int id = extract_id_from_path(path, "/widget/");
        if (!h->widget_a11y) {
            send_http(client_fd, 501, "Not Implemented", "text/plain",
                      "/a11y not supported by this backend");
        } else {
            char role[64] = "", name[512] = "", desc[512] = "";
            h->widget_a11y(id, role, sizeof(role), name, sizeof(name),
                           desc, sizeof(desc));
            // Escape name/desc minimally (quotes/backslash/control).
            char ne[1040], de[1040];
            for (int p = 0; p < 2; p++) {
                const char* src = p == 0 ? name : desc;
                char* out = p == 0 ? ne : de;
                int j = 0;
                for (int i = 0; src[i] && j < (int)sizeof(ne) - 8; i++) {
                    unsigned char ch = (unsigned char)src[i];
                    if (ch == '"' || ch == '\\') { out[j++] = '\\'; out[j++] = (char)ch; }
                    else if (ch < 0x20) { out[j++] = ' '; }
                    else out[j++] = (char)ch;
                }
                out[j] = '\0';
            }
            char body[2200];
            snprintf(body, sizeof(body),
                     "{\"role\":\"%s\",\"name\":\"%s\",\"description\":\"%s\"}",
                     role, ne, de);
            send_http(client_fd, 200, "OK", "application/json", body);
        }
    } else if (method == 0 && strcmp(path, "/screenshot") == 0) {
        if (!h->screenshot_png) {
            send_http(client_fd, 501, "Not Implemented", "text/plain",
                      "/screenshot not supported by this backend");
        } else {
            unsigned char* data = NULL;
            size_t len = 0;
            if (h->screenshot_png(&data, &len) == 0 && data && len > 0) {
                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: image/png\r\n"
                    "Content-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n", len);
                send_all(client_fd, header, hlen);
                send_all(client_fd, (const char*)data, (int)len);  // PNG can be large
                free(data);
            } else {
                send_http(client_fd, 500, "Error", "text/plain",
                          "screenshot capture failed");
            }
        }
    } else if (method == 0 && strncmp(path, "/widget/", 8) == 0) {
        int id = extract_id_from_path(path, "/widget/");
        if (id > 0) {
            char body[4096];
            widget_to_json(h, id, body, sizeof(body));
            send_http(client_fd, 200, "OK", "application/json", body);
        } else {
            send_http(client_fd, 404, "Not Found", "text/plain",
                      "widget not found");
        }
    // NB every widget action below is anchored on the /widget/ prefix, not on
    // strstr(verb) alone. Without the anchor, "/canvas/1/click" matches the
    // "/click" arm, gets read as widget id 0, and answers 404 "widget not
    // found" — a canvas click that looks like a missing widget.
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/double_click")) {
        // Fire the widget's double-click closure directly (a plain ABI call,
        // like tabs_select) — headless-safe, no synthetic gesture. Must precede
        // the /click arm, whose strstr would also match "/double_click".
        int id = extract_id_from_path(path, "/widget/");
        int fired = aether_ui_fire_double_click(id);
        char body[64];
        snprintf(body, sizeof(body), "{\"fired\":%s}", fired ? "true" : "false");
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/drop")) {
        // POST /widget/{id}/drop?src=N — drop row N onto this row (row
        // drag-reorder). Fires the row's on_drop(N) closure headlessly.
        int id = extract_id_from_path(path, "/widget/");
        const char* s = extract_query_param(path, "src");
        int fired = aether_ui_fire_row_drop(id, s ? atoi(s) : -1);
        char body[64];
        snprintf(body, sizeof(body), "{\"fired\":%s}", fired ? "true" : "false");
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 1 && (strcmp(path, "/undo") == 0
                               || strcmp(path, "/redo") == 0)) {
        // Step the edit stack via the backend's marshalled fire.
        int did = (path[1] == 'u') ? aether_ui_fire_undo() : aether_ui_fire_redo();
        char body[128];
        snprintf(body, sizeof(body),
                 "{\"did\":%d,\"undo_depth\":%d,\"redo_depth\":%d}",
                 did, aether_ui_undo_depth_impl(), aether_ui_redo_depth_impl());
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 0 && strcmp(path, "/undo_state") == 0) {
        char body[192];
        snprintf(body, sizeof(body),
                 "{\"undo_depth\":%d,\"redo_depth\":%d,\"label\":\"%s\"}",
                 aether_ui_undo_depth_impl(), aether_ui_redo_depth_impl(),
                 aether_ui_undo_label_impl());
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 1 && strncmp(path, "/appearance", 11) == 0) {
        // POST /appearance?dark=N — steer the OS-appearance override
        // headlessly (AeCS styles_for_mode re-theme; see the GTK4 twin).
        const char* v = extract_query_param(path, "dark");
        int fired = aether_ui_fire_appearance(v ? atoi(v) : 0);
        char body[64];
        snprintf(body, sizeof(body), "{\"fired\":%s}", fired ? "true" : "false");
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/scroll")) {
        // POST /widget/{id}/scroll?dy=N — scroll a vlist container by N rows
        // (dy>0 toward the end). Fires the container's on_scroll(N) headlessly.
        int id = extract_id_from_path(path, "/widget/");
        const char* s = extract_query_param(path, "dy");
        int fired = aether_ui_fire_scroll(id, s ? atoi(s) : 0);
        char body[64];
        snprintf(body, sizeof(body), "{\"fired\":%s}", fired ? "true" : "false");
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/click")) {
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_CLICK;
        ctx.handle = extract_id_from_path(path, "/widget/");
        dispatch_and_reply(client_fd, h, &ctx, "clicked");
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/hover")) {
        // POST /widget/{id}/hover — put the pointer over a widget, so a spec
        // can assert :hover styling, hover-revealed affordances and
        // tooltips. POST /widget/0/hover leaves everything.
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_HOVER;
        ctx.handle = extract_id_from_path(path, "/widget/");
        dispatch_and_reply(client_fd, h, &ctx, "hovered");
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/press")) {
        // POST /widget/{id}/press — hold the button down (no release), so
        // the ACTIVE/pressed visual state can be asserted.
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_PRESS;
        ctx.handle = extract_id_from_path(path, "/widget/");
        dispatch_and_reply(client_fd, h, &ctx, "pressed");
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/unpress")) {
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_RELEASE;
        ctx.handle = extract_id_from_path(path, "/widget/");
        dispatch_and_reply(client_fd, h, &ctx, "released");
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/set_text")) {
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_SET_TEXT;
        ctx.handle = extract_id_from_path(path, "/widget/");
        const char* v = extract_query_param(path, "v");
        if (v) {
            strncpy(ctx.sval, v, sizeof(ctx.sval) - 1);
            char* amp = strchr(ctx.sval, '&'); if (amp) *amp = '\0';
            url_decode(ctx.sval);
        }
        dispatch_and_reply(client_fd, h, &ctx, "set");
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/toggle")) {
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_TOGGLE;
        ctx.handle = extract_id_from_path(path, "/widget/");
        dispatch_and_reply(client_fd, h, &ctx, "toggled");
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/set_value")) {
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_SET_VALUE;
        ctx.handle = extract_id_from_path(path, "/widget/");
        const char* v = extract_query_param(path, "v");
        if (v) ctx.dval = atof(v);
        dispatch_and_reply(client_fd, h, &ctx, "set");
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/context_menu")) {
        // POST /widget/{id}/context_menu          → {"mapped":1}
        // POST /widget/{id}/context_menu/{idx}    → {"mapped":1,"activated":idx}
        //
        // Must be tested BEFORE the generic /widget/{id}/<action> arm, whose
        // else-branch 400s. Activating drives the same closure a real
        // right-click fires — there is no test-only path.
        int id = extract_id_from_path(path, "/widget/");
        const char* tail = strstr(path, "/context_menu");
        const char* idx_s = tail ? strchr(tail + 1, '/') : NULL;

        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_CTX_MENU;
        ctx.handle = id;
        h->dispatch_action(&ctx);
        int mapped = ctx.retval;

        if (!idx_s) {
            char body[64];
            snprintf(body, sizeof(body), "{\"mapped\":%d}", mapped);
            send_http(client_fd, 200, "OK", "application/json", body);
        } else {
            int idx = atoi(idx_s + 1);
            AetherDriverActionCtx act = {0};
            act.action = AETHER_DRV_CTX_ACTIVATE;
            act.handle = id;
            act.ival = idx;
            h->dispatch_action(&act);
            if (!act.retval) {
                send_http(client_fd, 404, "Not Found", "application/json",
                          "{\"error\":\"no such context menu item\"}");
            } else {
                char body[96];
                snprintf(body, sizeof(body),
                         "{\"mapped\":%d,\"activated\":%d}", mapped, idx);
                send_http(client_fd, 200, "OK", "application/json", body);
            }
        }
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/split_position")) {
        // POST /widget/{id}/split_position?px=N — omit px (or pass <0) to
        // read the divider without moving it. Answers the resulting
        // position, or -1 when the handle is not a splitview.
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_SPLIT_POS;
        ctx.handle = extract_id_from_path(path, "/widget/");
        const char* px = extract_query_param(path, "px");
        ctx.ival = px ? atoi(px) : -1;
        h->dispatch_action(&ctx);
        char body[64];
        snprintf(body, sizeof(body), "{\"ok\":true,\"position\":%d}", ctx.retval);
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 1 && strncmp(path, "/widget/", 8) == 0
               && strstr(path, "/tab_select")) {
        // POST /widget/{id}/tab_select?i=N — switch the active tab. Marshaled
        // to the UI thread (GtkStack/NSTabView aren't thread-safe). On a stub
        // backend tabs_selected stays -1, which is how a spec learns the tab
        // strip is unwired.
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_TAB_SELECT;
        ctx.handle = extract_id_from_path(path, "/widget/");
        const char* i = extract_query_param(path, "i");
        ctx.ival = i ? atoi(i) : -1;
        h->dispatch_action(&ctx);
        char body[64];
        snprintf(body, sizeof(body), "{\"ok\":true,\"tabSelected\":%d}", ctx.retval);
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 0 && strncmp(path, "/focus", 6) == 0) {
        // GET /focus — who has keyboard focus (parity with the GTK server).
        if (h->focused_widget) {
            int fh = h->focused_widget();
            char body[128];
            snprintf(body, sizeof(body), "{\"handle\":%d,\"type\":\"%s\"}",
                     fh, fh > 0 ? h->widget_type(fh) : "none");
            send_http(client_fd, 200, "OK", "application/json", body);
        } else {
            send_http(client_fd, 501, "Not Implemented", "application/json",
                      "{\"error\":\"focus introspection not wired on this backend\"}");
        }
    } else if (method == 1 && strstr(path, "/widget/") && strstr(path, "/focus")) {
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_FOCUS;
        ctx.handle = extract_id_from_path(path, "/widget/");
        dispatch_and_reply(client_fd, h, &ctx, "focused");
    } else if (method == 1 && strncmp(path, "/window/resize", 14) == 0) {
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_WIN_RESIZE;
        const char* ws = extract_query_param(path, "w");
        const char* hs = extract_query_param(path, "h");
        ctx.ival = ws ? atoi(ws) : 0;
        ctx.ival2 = hs ? atoi(hs) : 0;
        if (ctx.ival > 0 && ctx.ival2 > 0) {
            h->dispatch_action(&ctx);
            send_http(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
        } else {
            send_http(client_fd, 400, "Bad Request", "application/json",
                      "{\"error\":\"need w>0, h>0\"}");
        }
    } else if (method == 1 && strncmp(path, "/window/key", 11) == 0) {
        // Fire a combo through the backend's key dispatch. Honesty rule:
        // backends WITHOUT real accelerator wiring answer fired:false for
        // combos (win32 today) — Tab/Shift+Tab focus moves are real.
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_WIN_KEY;
        const char* combo = extract_query_param(path, "combo");
        if (combo) {
            strncpy(ctx.sval, combo, sizeof(ctx.sval) - 1);
            char* amp = strchr(ctx.sval, '&');
            if (amp) *amp = '\0';
            url_decode_keep_plus(ctx.sval);
        }
        h->dispatch_action(&ctx);
        char body[64];
        snprintf(body, sizeof(body), "{\"fired\":%s}",
                 ctx.retval ? "true" : "false");
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 0 && strncmp(path, "/window/pick", 12) == 0) {
        // GET /window/pick?x=&y= — a REAL hit-test at window-local coords.
        // This is what proves a modal scrim blocks input by z-order rather
        // than by an honour system: pick the button under the scrim and the
        // scrim must answer, not the button.
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_PICK;
        const char* xs = extract_query_param(path, "x");
        const char* ys = extract_query_param(path, "y");
        ctx.ival = xs ? atoi(xs) : 0;
        ctx.ival2 = ys ? atoi(ys) : 0;
        h->dispatch_action(&ctx);
        if (ctx.result == 3) {
            send_http(client_fd, 501, "Not Implemented", "application/json",
                      "{\"error\":\"window pick not wired on this backend\"}");
        } else {
            char body[160];
            snprintf(body, sizeof(body),
                     "{\"handle\":%d,\"type\":\"%s\",\"on_scrim\":%d}",
                     ctx.retval,
                     ctx.ival2 ? "scrim"
                               : (ctx.retval > 0 ? h->widget_type(ctx.retval) : "none"),
                     ctx.ival2 ? 1 : 0);
            send_http(client_fd, 200, "OK", "application/json", body);
        }
    } else if (method == 0 && strncmp(path, "/text_extent", 12) == 0) {
        // GET /text_extent?size=&s= — the metrics vg.text_extent() reports.
        // Measured with the SAME font the canvas draws with, or centring
        // math built on it would drift from what the user sees.
        const char* sz = extract_query_param(path, "size");
        const char* s  = extract_query_param(path, "s");
        double size = sz ? atof(sz) : 16.0;
        if (size <= 0) size = 16.0;
        char text[512] = "";
        if (s) {
            strncpy(text, s, sizeof(text) - 1);
            char* amp = strchr(text, '&');
            if (amp) *amp = '\0';
            url_decode(text);
        }
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"width\":%.3f,\"ascent\":%.3f,\"descent\":%.3f,\"height\":%.3f}",
                 aether_ui_text_measure(size, text),
                 aether_ui_font_ascent(size),
                 aether_ui_font_descent(size),
                 aether_ui_font_height(size));
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 0 && strcmp(path, "/overlays") == 0) {
        // Overlay handles are 1-based and monotonic; the table is never
        // compacted, so a closed overlay stays listed with "live":0. Specs
        // rely on that (a toast must be observably dead, not merely absent).
        int n = aether_ui_overlay_count_impl();
        char body[4096];
        int pos = snprintf(body, sizeof(body), "{\"count\":%d,\"overlays\":[", n);
        for (int i = 1; i <= n && pos < (int)sizeof(body) - 96; i++) {
            pos += snprintf(body + pos, sizeof(body) - pos,
                "%s{\"handle\":%d,\"modal\":%d,\"live\":%d,\"exiting\":%d,\"material\":\"%s\"}",
                            i > 1 ? "," : "", i,
                            aether_ui_overlay_is_modal_impl(i),
                            aether_ui_overlay_is_live_impl(i),
                            aether_ui_overlay_is_exiting_impl(i),
                            aether_ui_overlay_material_effective_impl(i));
        }
        snprintf(body + pos, sizeof(body) - pos, "]}");
        send_http(client_fd, 200, "OK", "application/json", body);
    } else if (method == 1 && strncmp(path, "/overlay/", 9) == 0
               && strstr(path, "/dismiss")) {
        int id = extract_id_from_path(path, "/overlay/");
        aether_ui_overlay_close_impl(id);
        send_http(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
    } else if (method == 1 && strncmp(path, "/canvas/", 8) == 0) {
        // POST /canvas/{id}/click|move|release|key — drive the canvas's
        // registered hit-test closures. A backend with no handler answers 404,
        // which is how a spec learns the difference between "missed" and
        // "unwired". release completes a press→drag→release gesture; without it
        // no driver on any backend could exercise a drag-to-swipe app.
        AetherDriverActionCtx ctx = {0};
        ctx.handle = extract_id_from_path(path, "/canvas/");
        const char* what = "canvas event";
        if (strstr(path, "/click")) {
            ctx.action = AETHER_DRV_CANVAS_CLICK;
            what = "no canvas click handler";
        } else if (strstr(path, "/release")) {
            ctx.action = AETHER_DRV_CANVAS_RELEASE;
            what = "no canvas release handler";
        } else if (strstr(path, "/move")) {
            ctx.action = AETHER_DRV_CANVAS_MOVE;
            what = "no canvas move handler";
        } else if (strstr(path, "/key")) {
            ctx.action = AETHER_DRV_CANVAS_KEY;
            what = "no canvas key handler";
        } else {
            send_http(client_fd, 400, "Bad Request", "application/json",
                      "{\"error\":\"unknown canvas action\"}");
            return;
        }
        const char* xs = extract_query_param(path, "x");
        const char* ys = extract_query_param(path, "y");
        const char* nm = extract_query_param(path, "name");
        if (xs) ctx.dval  = atof(xs);
        if (ys) ctx.dval2 = atof(ys);
        if (nm) {
            strncpy(ctx.sval, nm, sizeof(ctx.sval) - 1);
            char* amp = strchr(ctx.sval, '&');
            if (amp) *amp = '\0';
            url_decode(ctx.sval);
        }
        h->dispatch_action(&ctx);
        if (ctx.result == 3) {
            char err[96];
            snprintf(err, sizeof(err), "{\"error\":\"%s\"}", what);
            send_http(client_fd, 404, "Not Found", "application/json", err);
        } else {
            send_http(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
        }
    } else if (method == 1 && strcmp(path, "/shutdown") == 0) {
        // Reply BEFORE closing: the app exits by the same path a user-close
        // takes, so the port is released cleanly. Signal-killing instead
        // leaves a zombie holding 9222 and the next suite in the matrix
        // interrogates the previous app — a whole family of impossible
        // failures.
        send_http(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_SHUTDOWN;
        h->dispatch_action(&ctx);
        return;
    } else if (method == 0 && strncmp(path, "/state/", 7) == 0) {
        int id = extract_id_from_path(path, "/state/");
        if (id > 0) {
            // JSON envelope matching the GTK driver: float cells keep the
            // original {"id":N,"value":F} byte shape; typed cells add
            // "type" (docs/design/reactivity-unification.md §3).
            char body[512];
            int st = aether_ui_state_type(id);
            if (st == 1) {
                snprintf(body, sizeof(body), "{\"id\":%d,\"type\":\"int\",\"value\":%d}",
                         id, aether_ui_state_get_i(id));
            } else if (st == 2) {
                snprintf(body, sizeof(body), "{\"id\":%d,\"type\":\"bool\",\"value\":%s}",
                         id, aether_ui_state_get_b(id) ? "true" : "false");
            } else if (st == 3) {
                const char* sv = aether_ui_state_get_s(id);
                snprintf(body, sizeof(body), "{\"id\":%d,\"type\":\"string\",\"value\":\"%s\"}",
                         id, sv);
                free((void*)sv);
            } else if (st == 4) {
                snprintf(body, sizeof(body), "{\"id\":%d,\"type\":\"list\",\"rev\":%d}",
                         id, aether_ui_state_list_rev(id));
            } else {
                snprintf(body, sizeof(body), "{\"id\":%d,\"value\":%.6f}",
                         id, aether_ui_state_get(id));
            }
            send_http(client_fd, 200, "OK", "application/json", body);
        } else {
            send_http(client_fd, 404, "Not Found", "application/json",
                      "{\"error\":\"state not found\"}");
        }
    } else if (method == 1 && strstr(path, "/state/") && strstr(path, "/set")) {
        AetherDriverActionCtx ctx = {0};
        ctx.action = AETHER_DRV_SET_STATE;
        ctx.handle = extract_id_from_path(path, "/state/");
        const char* v = extract_query_param(path, "v");
        if (v) {
            ctx.dval = atof(v);
            strncpy(ctx.sval, v, sizeof(ctx.sval) - 1);
        }
        h->dispatch_action(&ctx);
        send_http(client_fd, 200, "OK", "application/json", "{\"ok\":true}");

    // --- /menus ---
    // GET  /menus                          → every menu with its item labels
    // POST /menu/{handle}/activate?label=X → fire that item's closure (the
    //   SAME closure the native GTK4 GAction / macOS-NSMenu / win32 path runs)
    // The menu-item side-store is shared across all three backends, so this
    // proves item activation everywhere; the app-visible effect (a counter
    // bump) is what the spec asserts, confirming the closure genuinely fired.
    } else if (method == 0 && strcmp(path, "/windows") == 0) {
        // Every top-level window: 1=primary, 2..=extras.
        int nw = aether_ui_window_count_impl();
        char* body = (char*)malloc(64 + nw * 256);
        int pos = sprintf(body, "[");
        for (int i = 1; i <= nw; i++) {
            if (i > 1) pos += sprintf(body + pos, ",");
            pos += snprintf(body + pos, 256,
                "{\"id\":%d,\"title\":\"%s\",\"live\":%s}",
                i, aether_ui_window_title_impl(i),
                aether_ui_window_is_open_impl(i) ? "true" : "false");
        }
        sprintf(body + pos, "]");
        send_http(client_fd, 200, "OK", "application/json", body);
        free(body);
    } else if (method == 1 && strncmp(path, "/window/", 8) == 0
               && strstr(path, "/close")) {
        int id = extract_id_from_path(path, "/window/");
        aether_ui_close_window_by_handle_impl(id);
        send_http(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
    } else if (method == 0 && strcmp(path, "/menus") == 0) {
        int handles[64];
        int nh = aether_ui_menu_handles(handles, 64);
        char* body = (char*)malloc(64 + nh * 2048);
        int pos = sprintf(body, "[");
        for (int i = 0; i < nh; i++) {
            if (i > 0) pos += sprintf(body + pos, ",");
            int h = handles[i];
            pos += sprintf(body + pos, "{\"handle\":%d,\"items\":[", h);
            int ic = aether_ui_menu_item_count_for(h);
            for (int k = 0; k < ic; k++) {
                if (k > 0) pos += sprintf(body + pos, ",");
                pos += snprintf(body + pos, 256, "\"%s\"",
                                aether_ui_menu_item_label_at(h, k));
            }
            pos += sprintf(body + pos, "]}");
        }
        sprintf(body + pos, "]");
        send_http(client_fd, 200, "OK", "application/json", body);
        free(body);
    } else if (method == 1 && strncmp(path, "/menu/", 6) == 0
               && strstr(path, "/activate")) {
        int handle = extract_id_from_path(path, "/menu/");
        const char* label = extract_query_param(path, "label");
        int r = aether_ui_menu_item_invoke(handle, label ? label : "");
        if (r == 0) send_http(client_fd, 200, "OK", "application/json",
                              "{\"ok\":true}");
        else if (r == 4) send_http(client_fd, 200, "OK", "application/json",
                              "{\"ok\":true,\"noClosure\":true}");
        else send_http(client_fd, 404, "Not Found", "text/plain",
                       "no such menu item");

    // --- /tray ---
    // GET  /tray              → list of all registered tray records
    // GET  /tray/{id}         → single tray record (tooltip, menu_handle,
    //                            current_icon, sealed)
    // GET  /tray/{id}/icon    → current icon path (resolves state cell)
    // POST /tray/{id}/click   → fire the left-click closure
    // POST /tray/{id}/menu/activate?label=Foo → invoke item by label
    // POST /tray/{id}/set_tooltip?v=Text     → update tooltip
    } else if (method == 0 && strcmp(path, "/tray") == 0) {
        int n = aether_ui_tray_count();
        char* body = (char*)malloc((size_t)n * 4096 + 64);
        int pos = sprintf(body, "[");
        for (int i = 1; i <= n; i++) {
            if (i > 1) pos += sprintf(body + pos, ",");
            pos += snprintf(body + pos, 512,
                "{\"id\":%d,\"name\":\"%s\",\"tooltip\":\"%s\","
                "\"menu_handle\":%d,\"icon\":\"%s\",\"template\":%s,"
                "\"sealed\":%s}",
                i, aether_ui_tray_name(i), aether_ui_tray_tooltip(i),
                aether_ui_tray_menu_handle(i),
                aether_ui_tray_current_icon(i),
                aether_ui_tray_is_template(i) ? "true" : "false",
                aether_ui_tray_is_sealed(i)   ? "true" : "false");
        }
        sprintf(body + pos, "]");
        send_http(client_fd, 200, "OK", "application/json", body);
        free(body);
    } else if (method == 0 && strncmp(path, "/tray/", 6) == 0
               && strstr(path, "/icon")) {
        int id = extract_id_from_path(path, "/tray/");
        const char* icon = aether_ui_tray_current_icon(id);
        send_http(client_fd, 200, "OK", "text/plain", icon);
    } else if (method == 0 && strncmp(path, "/tray/", 6) == 0) {
        int id = extract_id_from_path(path, "/tray/");
        if (id < 1 || id > aether_ui_tray_count()) {
            send_http(client_fd, 404, "Not Found", "text/plain", "tray not found");
        } else {
            char body[1024];
            snprintf(body, sizeof(body),
                "{\"id\":%d,\"name\":\"%s\",\"tooltip\":\"%s\","
                "\"menu_handle\":%d,\"icon\":\"%s\",\"template\":%s,"
                "\"sealed\":%s}",
                id, aether_ui_tray_name(id), aether_ui_tray_tooltip(id),
                aether_ui_tray_menu_handle(id),
                aether_ui_tray_current_icon(id),
                aether_ui_tray_is_template(id) ? "true" : "false",
                aether_ui_tray_is_sealed(id)   ? "true" : "false");
            send_http(client_fd, 200, "OK", "application/json", body);
        }
    } else if (method == 1 && strncmp(path, "/tray/", 6) == 0
               && strstr(path, "/click")) {
        int id = extract_id_from_path(path, "/tray/");
        int r = aether_ui_tray_emit_click(id);
        if (r == 0) send_http(client_fd, 200, "OK", "text/plain", "clicked");
        else if (r == 1) send_http(client_fd, 403, "Forbidden", "text/plain",
                                    "tray is sealed");
        else if (r == 4) send_http(client_fd, 204, "No Content", "text/plain", "");
        else send_http(client_fd, 404, "Not Found", "text/plain", "tray not found");
    } else if (method == 1 && strncmp(path, "/tray/", 6) == 0
               && strstr(path, "/menu/activate")) {
        int id = extract_id_from_path(path, "/tray/");
        const char* v = extract_query_param(path, "label");
        char label[256] = "";
        if (v) {
            strncpy(label, v, sizeof(label) - 1);
            char* amp = strchr(label, '&'); if (amp) *amp = '\0';
            url_decode(label);
        }
        int r = aether_ui_tray_menu_activate(id, label);
        if (r == 0) send_http(client_fd, 200, "OK", "text/plain", "activated");
        else if (r == 1) send_http(client_fd, 403, "Forbidden", "text/plain",
                                    "tray is sealed");
        else if (r == 4) send_http(client_fd, 204, "No Content", "text/plain", "");
        else send_http(client_fd, 404, "Not Found", "text/plain", "item not found");
    } else if (method == 1 && strncmp(path, "/tray/", 6) == 0
               && strstr(path, "/set_tooltip")) {
        int id = extract_id_from_path(path, "/tray/");
        const char* v = extract_query_param(path, "v");
        char text[256] = "";
        if (v) {
            strncpy(text, v, sizeof(text) - 1);
            char* amp = strchr(text, '&'); if (amp) *amp = '\0';
            url_decode(text);
        }
        aether_ui_tray_set_tooltip_reg(id, text);
        send_http(client_fd, 200, "OK", "text/plain", "set");

    // --- /notifications ---
    } else if (method == 0 && strcmp(path, "/notifications") == 0) {
        int n = aether_ui_notif_count();
        char* body = (char*)malloc((size_t)n * 1536 + 64);
        int pos = sprintf(body, "[");
        for (int i = 1; i <= n; i++) {
            if (i > 1) pos += sprintf(body + pos, ",");
            pos += snprintf(body + pos, 1536,
                "{\"id\":%d,\"title\":\"%s\",\"body\":\"%s\","
                "\"icon\":\"%s\",\"tag\":\"%s\",\"dismissed\":%s}",
                i, aether_ui_notif_title(i), aether_ui_notif_body(i),
                aether_ui_notif_icon(i), aether_ui_notif_tag(i),
                aether_ui_notif_dismissed(i) ? "true" : "false");
        }
        sprintf(body + pos, "]");
        send_http(client_fd, 200, "OK", "application/json", body);
        free(body);
    } else if (method == 1 && strncmp(path, "/notifications/", 15) == 0
               && strstr(path, "/click")) {
        int id = extract_id_from_path(path, "/notifications/");
        int r = aether_ui_notif_emit_click(id);
        if (r == 0) send_http(client_fd, 200, "OK", "text/plain", "clicked");
        else if (r == 4) send_http(client_fd, 204, "No Content", "text/plain", "");
        else send_http(client_fd, 404, "Not Found", "text/plain", "notification not found");
    } else if (method == 1 && strncmp(path, "/notifications/", 15) == 0
               && strstr(path, "/dismiss")) {
        int id = extract_id_from_path(path, "/notifications/");
        int r = aether_ui_notif_mark_dismissed(id);
        if (r == 0) send_http(client_fd, 200, "OK", "text/plain", "dismissed");
        else send_http(client_fd, 404, "Not Found", "text/plain", "notification not found");

    } else {
        send_http(client_fd, 404, "Not Found", "text/plain",
                  "unknown endpoint");
    }

    aether_close_socket(client_fd);
}

// ---------------------------------------------------------------------------
// Accept-loop thread.
// ---------------------------------------------------------------------------
typedef struct {
    int port;
    const AetherDriverHooks* hooks;
} ServerArgs;

#ifdef _WIN32
static DWORD WINAPI server_thread(LPVOID arg) {
#else
static void* server_thread(void* arg) {
#endif
    ServerArgs* args = (ServerArgs*)arg;
    int port = args->port;
    const AetherDriverHooks* hooks = args->hooks;
#ifndef _WIN32
    // A timed-out client closing its socket mid-response must not SIGPIPE
    // the app to death (no SIGPIPE on Windows; sends there just fail).
    signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    aether_sock_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == AETHER_SOCK_INVALID) {
        fprintf(stderr, "AetherUIDriver: socket() failed\n");
#ifdef _WIN32
        WSACleanup();
        return 1;
#else
        return NULL;
#endif
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "AetherUIDriver: bind to port %d failed\n", port);
        aether_close_socket(server_fd);
#ifdef _WIN32
        WSACleanup();
        return 1;
#else
        return NULL;
#endif
    }
    if (listen(server_fd, 8) < 0) {
        aether_close_socket(server_fd);
#ifdef _WIN32
        WSACleanup();
        return 1;
#else
        return NULL;
#endif
    }

    fprintf(stderr, "AetherUIDriver: listening on http://127.0.0.1:%d\n", port);

    for (;;) {
        struct sockaddr_in caddr;
#ifdef _WIN32
        int caddr_len = sizeof(caddr);
#else
        socklen_t caddr_len = sizeof(caddr);
#endif
        aether_sock_t client_fd = accept(server_fd,
            (struct sockaddr*)&caddr, &caddr_len);
        if (client_fd == AETHER_SOCK_INVALID) break;
        handle_request(client_fd, hooks);
    }

    aether_close_socket(server_fd);
#ifdef _WIN32
    WSACleanup();
    return 0;
#else
    return NULL;
#endif
}

void aether_ui_test_server_start(int port, const AetherDriverHooks* hooks) {
    ServerArgs* args = (ServerArgs*)malloc(sizeof(ServerArgs));
    args->port = port;
    args->hooks = hooks;

#ifdef _WIN32
    CreateThread(NULL, 0, server_thread, args, 0, NULL);
#else
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, args);
    pthread_detach(tid);
#endif
}
