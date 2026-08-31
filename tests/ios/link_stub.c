// tests/ios/link_stub.c — minimal libaether runtime stub for the iOS backend
// LINK gate in ci.sh (Phase 1e), mirroring tests/win32's role for Win32.
//
// The UIKit backend (backend/aether_ui_uikit.m) + the shared driver/system
// sources reference almost nothing from libaether by NAME: boxed closures are
// invoked through a function pointer in the _AeClosure struct, not a named
// runtime entry. The only external libaether symbols are the std float-array
// accessors the canvas clip/gradient code reads — declared `extern` in the
// backend and resolved here so the link proves the UIKit frameworks are all
// present, without needing a full iOS build of libaether.
//
// There is no iOS build of libaether on this box; when there is one, link
// against it instead of this stub and delete this file. Until then this is the
// difference between "the backend mirrors the others" and "is known to link
// against the iOS frameworks".

#include <stddef.h>

double floatarr_get_raw(void* arr, int i) { (void)arr; (void)i; return 0.0; }
double floatarr_get_unchecked(void* arr, int i) { (void)arr; (void)i; return 0.0; }

// UIApplicationMain is never called by the gate (it runs nothing), but a
// hosted executable needs an entry point to link.
int main(int argc, char** argv) { (void)argc; (void)argv; return 0; }
