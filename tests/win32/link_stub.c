// tests/win32/link_stub.c — what the Win32 link check needs and nothing else.
//
// The cross-compile check proves the backend's syntax. It cannot prove that
// the Windows APIs it calls RESOLVE: a function declared in a header whose
// import library is missing compiles perfectly and fails at link. That is a
// real class of mistake here, since the backend reaches into ole32, shell32,
// comctl32, gdiplus and uxtheme, and it is invisible to anyone who cannot
// build on Windows.
//
// So the check links for real. The only things standing between it and a
// binary are the libaether runtime symbols the backend calls, which have no
// Windows build on a Linux or macOS box. They are stubbed here.
//
// IF THIS FILE STOPS BEING ENOUGH, that is the check working: a new undefined
// symbol means the backend started calling into the aether runtime somewhere
// new. Add it below with a comment saying which call site wants it. Do NOT
// disable the link check to make it pass, and do NOT stub a WINDOWS symbol
// here: an undefined Windows API is exactly the bug this exists to catch, and
// the fix for one of those is a missing -l flag, not a stub.

// aether_ui_win32.c: canvas gradient stops and clip rects arrive as a
// std.floatarr from the DSL side.
double floatarr_get_raw(void* arr, int index);
double floatarr_get_unchecked(void* arr, int index);

double floatarr_get_raw(void* arr, int index) { (void)arr; (void)index; return 0.0; }
double floatarr_get_unchecked(void* arr, int index) { (void)arr; (void)index; return 0.0; }

// The link needs an entry point; nothing here is ever run.
int main(void) { return 0; }
