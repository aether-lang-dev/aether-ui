#!/usr/bin/env bash
# tests/run_spec.sh — launcher glue for ALL the Aeocha driver specs.
# The specs are Aether programs (tests/<app>/spec_*.ae) built on the shared
# tests/lib/uidriver.ae client; this wrapper only wires the module search
# path (aeocha + tests/lib) and runs ae from the spec's directory (same-dir
# modules like gp_driver.ae resolve, and the ae module cache is cwd-keyed).
#
# Which spec: $UI_SPEC as "<app-dir>/<spec-name>" (ci.sh sets it per
# iteration), e.g. UI_SPEC=calculator/spec_calculator — or $1. ci.sh's
# run_server_test passes the port as the first arg; the driver's port is
# fixed at 9222, so a numeric $1 is ignored.
#
# NB: the module-search env var is AETHER_LIB_DIR (aether #413),
# multi-entry with the platform path separator.
#
#   AEOCHA_DIR   where aeocha.ae lives (default ~/scm/aeocha)
set -e
SPEC="${UI_SPEC:-$1}"
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
# Default to a flat ~/scm/aeocha, else the sibling checkout beside aether-ui
# ($TESTS_DIR/../../aeocha — the AetherThings/ grouping). Explicit $AEOCHA_DIR
# (e.g. exported by ci.sh) wins over both.
if [ -z "${AEOCHA_DIR:-}" ]; then
    if [ -f "$HOME/scm/aeocha/aeocha.ae" ]; then
        AEOCHA_DIR="$HOME/scm/aeocha"
    else
        AEOCHA_DIR="$TESTS_DIR/../../aeocha"
    fi
fi
if [ ! -f "$AEOCHA_DIR/aeocha.ae" ]; then
    echo "  FAIL: aeocha not found at $AEOCHA_DIR (set AEOCHA_DIR, or clone github.com/aether-lang-org/aeocha)"
    exit 1
fi
cd "$TESTS_DIR/$(dirname "$SPEC")"
# AETHER_LIB_DIR is multi-entry with the PLATFORM path separator — ";" on
# Windows (MSYS), ":" elsewhere (aether #413).
SEP=":"
LIBA="$AEOCHA_DIR"
LIBT="$TESTS_DIR/lib"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        SEP=";"
        # ae.exe is a NATIVE Windows binary: it cannot resolve MSYS mount
        # paths like /home/paul/aeocha, only real Windows paths. That works
        # by accident whenever a checkout sits under /c/Users/... (which IS
        # a Windows path spelled MSYS-style) and breaks the moment one lives
        # under the MSYS root, where /home/paul is really
        # C:/msys64/home/paul. Convert both entries so either layout works.
        command -v cygpath >/dev/null 2>&1 && {
            LIBA="$(cygpath -m "$LIBA")"
            LIBT="$(cygpath -m "$LIBT")"
        }
        ;;
esac
exec env AETHER_LIB_DIR="${LIBA}${SEP}${LIBT}" ae run "$(basename "$SPEC").ae"
