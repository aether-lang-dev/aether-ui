#!/usr/bin/env bash
# tests/run_spec.sh — launcher glue for ALL the AetherUIDriver specs.
# The specs are Aether programs (tests/<app>/spec_*.ae) built on the shared
# tests/lib/uidriver.ae client; this wrapper only wires the module search
# path (tests/lib) and runs ae from the spec's directory (same-dir modules
# like gp_driver.ae resolve, and the ae module cache is cwd-keyed).
#
# Which spec: $UI_SPEC as "<app-dir>/<spec-name>" (ci.sh sets it per
# iteration), e.g. UI_SPEC=calculator/spec_calculator — or $1. ci.sh's
# run_server_test passes the port as the first arg; the driver's port is
# fixed at 9222, so a numeric $1 is ignored.
#
# NB: the module-search env var is AETHER_LIB_DIR (aether #413),
# multi-entry with the platform path separator.
#
# The specs used to need an aeocha CHECKOUT on the lib path. They no longer
# do: aeocha's core was absorbed into the stdlib as `std.spec` (ae 0.539) and
# its HTTP matchers as `std.http.client.httptest`, both of which ship with the
# toolchain. So there is nothing to clone, nothing to point $AEOCHA_DIR at,
# and no hard failure when a box lacks the repo — one less thing to install
# on every new platform.
set -e
SPEC="${UI_SPEC:-$1}"
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$TESTS_DIR/$(dirname "$SPEC")"
# AETHER_LIB_DIR is multi-entry with the PLATFORM path separator — ";" on
# Windows (MSYS), ":" elsewhere (aether #413). Only one entry now, but the
# platform conversion below still matters for it.
LIBT="$TESTS_DIR/lib"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # ae.exe is a NATIVE Windows binary: it cannot resolve MSYS mount
        # paths like /home/paul/aether-ui/tests/lib, only real Windows paths.
        # That works by accident whenever a checkout sits under /c/Users/...
        # (which IS a Windows path spelled MSYS-style) and breaks the moment
        # one lives under the MSYS root, where /home/paul is really
        # C:/msys64/home/paul. Convert so either layout works.
        command -v cygpath >/dev/null 2>&1 && LIBT="$(cygpath -m "$LIBT")"
        ;;
esac
exec env AETHER_LIB_DIR="${LIBT}" ae run "$(basename "$SPEC").ae"
