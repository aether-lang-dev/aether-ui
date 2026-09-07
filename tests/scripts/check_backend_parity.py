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
}
SHARED = ("backend/aether_ui_system_extras.c",
          "backend/aether_ui_test_server.c",
          "backend/aether_ui_sni.c")

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
    for name, who in gaps:
        missing = sorted(set(BACKENDS) - set(who))
        print("  %s: implemented on %s, MISSING on %s"
              % (name, ", ".join(who), ", ".join(missing)))
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
