// tests/ios/link_stub.c — minimal libaether runtime stub for the iOS backend
// LINK gate in ci.sh (Phase 1e), mirroring tests/win32's role for Win32.
//
// The UIKit backend (backend/aether_ui_uikit.m) + the shared driver/system
// sources reference little from libaether by NAME: boxed closures are invoked
// through a function pointer in the _AeClosure struct, not a named runtime
// entry. What they do name is resolved here so the link proves the UIKit
// frameworks are all present, without needing a full iOS build of libaether:
// the std float-array accessors the canvas clip/gradient code reads, and the
// closure-env reclaim the undo store uses.
//
// A NEW UNDEFINED SYMBOL HERE is this gate working: the backend started
// calling into the runtime somewhere new. Add it with a comment naming the
// call site, exactly as tests/win32/win32_runtime_test.c says for its half.
//
// The real link happens too: ci.sh Phase 1e2 builds the Aether runtime for
// the simulator (`ae build --target=<arch>-ios-simulator --emit=staticlib`,
// see tests/ios/runtime_seed.ae) and links an example against it, installs it
// in a booted simulator and drives it. This stub stays for Phase 1e's own
// check, which links the backend against the frameworks in seconds, before
// the runtime is built, and names the runtime symbol a backend change starts
// needing.

#include <stddef.h>
#include <stdlib.h>

double floatarr_get_raw(void* arr, int i) { (void)arr; (void)i; return 0.0; }
double floatarr_get_unchecked(void* arr, int i) { (void)arr; (void)i; return 0.0; }

// The undo/redo edit store reclaims a dropped edit's boxed closures through
// the env's own destructor (aether_ui_system_extras.c: undo_edit_release).
// That is the runtime's aether_closure_env_free; the real thing runs the
// `_dtor` codegen puts first in every env struct, and this mirrors it so the
// gate links without a build of libaether.
typedef struct { void (*dtor)(void*); } AeEnvHeaderStub;
void aether_closure_env_free(void* env);
void aether_closure_env_free(void* env) {
    if (!env) return;
    AeEnvHeaderStub* h = (AeEnvHeaderStub*)env;
    if (h->dtor) { h->dtor(env); return; }
    free(env);
}

// aether_ui_uikit.m installs std.worker's main-thread poster at app_create
// (aether_ui_worker_poster_install_impl) and delivers posted jobs from the
// main dispatch queue. Both live in libaether; the poster struct is the
// compiler's _AeClosure ({fn, env}) by value, mirrored in
// aether_ui_backend.h as AetherUiWorkerClosure. The gate runs no job, so
// these record nothing -- a backend that starts calling a THIRD worker
// symbol fails to link here first.
typedef struct { void (*fn)(void); void* env; } AeWorkerClosureStub;
void aether_worker_set_main_poster(AeWorkerClosureStub poster);
void aether_worker_set_main_poster(AeWorkerClosureStub poster) { (void)poster; }
void aether_worker_deliver(void* job);
void aether_worker_deliver(void* job) { (void)job; }

// UIApplicationMain is never called by the gate (it runs nothing), but a
// hosted executable needs an entry point to link.
int main(int argc, char** argv) { (void)argc; (void)argv; return 0; }
