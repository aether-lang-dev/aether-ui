// aether_ui_no_control.c — the NO-CONTROL implementation of the AetherUIDriver
// control surface.
//
// The backends drive apps for specs/CI through a small "control" API
// (aether_ui_test_server.h): aether_ui_test_server_start() spins a localhost
// HTTP control server that the AetherUIDriver client pokes at. That server is
// exactly what must NOT ship in a release / App Store binary — a listening
// socket in a shipped app is a security and App-Review problem.
//
// So the control surface has TWO implementations, selected at build time:
//   • aether_ui_test_server.c — the real control server (socket/bind/listen/
//     accept + the HTTP routes). Linked only when a build opts in with
//     AETHER_UI_WITH_DRIVER=1 (ci.sh and the spec runs do).
//   • aether_ui_no_control.c  — THIS file: the same API as no-ops, so no
//     control server / socket code ships. The DEFAULT.
//
// The backends are identical either way: they still call
// aether_ui_test_server_start() etc.; here those calls do nothing, so
// aether_ui_enable_test_server_impl() is a no-op and no server ever binds.
// Source selection lives in build_support/aetherui/module.ae (control_source).

#include "aether_ui_test_server.h"

void aether_ui_test_server_start(int port, const AetherDriverHooks* hooks) {
    (void)port; (void)hooks;
}
void aether_ui_test_server_set_banner(int handle) { (void)handle; }
int  aether_ui_test_server_banner_handle(void) { return 0; }
void aether_ui_test_server_seal_widget(int handle) { (void)handle; }
int  aether_ui_test_server_is_sealed(int handle) { (void)handle; return 0; }
