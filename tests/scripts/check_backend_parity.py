import re, io, sys

DEF_START = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b(aether_ui_[a-z0-9_]+)\s*\(", re.M)

def defined_in(path):
    """Names DEFINED (not merely declared) in a C/ObjC source.

    A definition starts at column 0 and its parameter list is followed by `{`;
    a declaration ends in `;`. Scanning forward from the name to whichever
    comes first is what distinguishes them, and unlike a single regex it
    cannot have one match swallow a later one.
    """
    try:
        s = io.open(path, encoding="utf-8").read()
    except OSError:
        return set()
    out = set()
    for m in DEF_START.finditer(s):
        depth, i, n = 0, m.end() - 1, len(s)
        while i < n:
            ch = s[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        j = i + 1
        while j < n and s[j] in " \t\r\n":
            j += 1
        if j < n and s[j] == "{":
            out.add(m.group(1))
    return out

def declared_in(path):
    s = io.open(path, encoding="utf-8").read()
    return set(re.findall(r"\b(aether_ui_[a-z0-9_]+)\s*\(", s))

BACKENDS = {
    "appkit": "backend/aether_ui_macos.m",
    "gtk4":   "backend/aether_ui_gtk4.c",
    "win32":  "backend/aether_ui_win32.c",
    "uikit":  "backend/aether_ui_uikit.m",
    "android": "backend/aether_ui_android.c",
}
SHARED = ("backend/aether_ui_system_extras.c",
          "backend/aether_ui_test_server.c",
          "backend/aether_ui_sni.c")

# Entry points that are the backend's OWN platform work and must not be
# satisfied by a shared source: the frame clock's native display clock (GTK4's
# frame clock, CADisplayLink / CVDisplayLink, Choreographer, DwmFlush). The
# shared layer (system extras) owns the subscribers and the timer fallback, so
# a shared definition of one of these would compile, link and quietly turn
# every backend's frames into the fallback timer.
NATIVE_ONLY = (
    "aether_ui_frame_clock_start_impl",
    "aether_ui_frame_clock_stop_impl",
    "aether_ui_frame_clock_name_impl",
    "aether_ui_frame_clock_hz_impl",
)

def main():
    decl = declared_in("backend/aether_ui_backend.h")
    shared = set()
    for f in SHARED:
        shared |= defined_in(f)
    have = {b: defined_in(p) for b, p in BACKENDS.items()}
    gaps = []
    for name in sorted(decl):
        if name in shared:
            continue
        who = sorted(b for b, d in have.items() if name in d)
        if who and len(who) != len(BACKENDS):
            gaps.append((name, who))
    # Declared and defined NOWHERE: the check above passes it (no backend has
    # it, so none is "missing" it), and the first app to call it fails to
    # link on every platform.
    for name in sorted(decl):
        if name in shared or any(name in d for d in have.values()):
            continue
        gaps.append((name, []))
    native_errs = []
    for name in NATIVE_ONLY:
        if name not in decl:
            native_errs.append("  %s: not declared in aether_ui_backend.h" % name)
        if name in shared:
            native_errs.append("  %s: defined in a shared source; it must be "
                               "each backend's own native implementation" % name)
        missing = sorted(b for b, d in have.items() if name not in d)
        if missing and name not in [g[0] for g in gaps]:
            gaps.append((name, sorted(set(BACKENDS) - set(missing))))
    for line in native_errs:
        print(line)
    for name, who in gaps:
        missing = sorted(set(BACKENDS) - set(who))
        print("  %s: implemented on %s, MISSING on %s"
              % (name, ", ".join(who) or "no backend", ", ".join(missing)))
    if native_errs and not gaps:
        print()
        print("backend parity: a native-only entry point is declared wrong or "
              "defined in a shared source.")
        return 1
    if gaps:
        print()
        print("backend parity: %d function(s) implemented on some backends but not "
              "all. A backend ABI entry point must exist on every backend, or an "
              "app that calls it fails to link on the ones that skipped it. A "
              "platform that cannot do the thing implements a documented no-op."
              % len(gaps))
        return 1
    print("backend parity: every ABI entry point is on all %d backends "
          "(or in the shared sources)" % len(BACKENDS))
    return 0

sys.exit(main())
