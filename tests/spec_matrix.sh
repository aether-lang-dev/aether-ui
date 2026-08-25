#!/usr/bin/env bash
# spec_matrix.sh — run every Aeocha suite against its app and tabulate.
#
# The platform-parity baseline in one command. The Windows equivalent of this
# lived in a session scratchpad and was never committed (see roadmap.md), which
# meant the next person had to reconstruct it from ci.sh. This is that script,
# committed, and portable across all three backends.
#
#   ./tests/spec_matrix.sh              # every suite
#   ./tests/spec_matrix.sh split table  # just these
#   ./tests/spec_matrix.sh --rebuild    # rebuild each app first (see below)
#
# Binaries come from `aeb .all.ae` (target/build/...). Run that first.
#
# Counting: the number printed is std.spec's own "N passing" — one per it()
# block. Failures are counted from its "N failing" tail. Those two numbers
# come straight from the tool, so the matrix is reproducible rather than
# hand-tallied (the roadmap's Windows row mixes it-counts and assertion-counts
# and is confusing as a result).

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT=9222
# HEADLESS only where a hidden window still LAYS OUT: win32 (SW_HIDE children
# get real rects) and macOS (Auto Layout runs unmapped). On GTK4, headless
# realizes-but-never-presents, so NO allocation pass runs — every geometry
# read is 0, picks miss, canvas coords are dead. Discovered on this script's
# first-ever Linux run: 8 suites red purely from this flag; ci.sh (the Linux
# harness) never sets it and the same suites are green there. Linux runs
# mapped-under-Xvfb instead.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Darwin) export AETHER_UI_HEADLESS=1 ;;
esac
export AETHER_UI_NO_ANIMATION=1

# Per-OS launch wrapper + the exe suffix.
#
# FreeBSD (GhostBSD) has no display in a plain SSH session, so — like Linux CI
# — we spin up our OWN private Xvfb (pkg: xorg-vfbserver) at 3200x2000 with the
# pointer parked off-window, and point launches at it via DISPLAY. This runs
# UNPRIVILEGED (no sudo, no lightdm :0), and gives the same mapped-but-hidden
# framebuffer Linux uses, so pointer-parking and HEADLESS behave identically.
# Override the display via $DISPLAY (any pre-set DISPLAY is respected and Xvfb
# is skipped — e.g. to use a real :0). See ~/aether-ui-freebsd-notes.md on .204.
EXE=""; FREEBSD=0; XVFB_PID=""
case "$(uname -s)" in
    FreeBSD) FREEBSD=1 ;;
    MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

if [ "$FREEBSD" -eq 1 ] && [ -z "${DISPLAY:-}" ] && command -v Xvfb >/dev/null 2>&1; then
    # A private display; :99 by convention unless taken.
    XV_DISP=":99"
    Xvfb "$XV_DISP" -screen 0 3200x2000x24 -ac >/tmp/aui_xvfb.log 2>&1 &
    XVFB_PID=$!
    export DISPLAY="$XV_DISP"
    for _ in $(seq 1 20); do
        xdpyinfo -display "$XV_DISP" >/dev/null 2>&1 && break
        sleep 0.2
    done
    trap 'kill "$XVFB_PID" 2>/dev/null' EXIT
fi

# Launch `$bin` with the given inline `VAR=val` env pairs — a plain env exec on
# every platform now (the display is set up above). Backgrounded by the caller.
aui_launch() {   # aui_launch VAR=val ... -- <bin>
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    env "${envs[@]}" "$@"
}

# suite | binary | spec | extra env
SUITES=(
  "calculator|examples/calculator|calculator/spec_calculator|"
  "text_metrics|examples/calculator|text_metrics/spec_text_metrics|"
  "testable|examples/testable|testable/spec_testable|"
  "context_menu|examples/context_menu|context_menu/spec_context_menu|"
  "overlay|examples/overlay_demo|overlay_demo/spec_overlay_demo|"
  "auto_hide|examples/auto_hide_demo|auto_hide_demo/spec_auto_hide_demo|"
  "vg_tooltip|examples/vg_tooltip|vg_tooltip/spec_vg_tooltip|AETHER_UI_TOOLTIP=drawn"
  "picker|examples/picker|picker/spec_picker|AETHER_UI_PICKER=drawn"
  "each|examples/each_demo|each_demo/spec_each_demo|"
  "clearchildren|examples/rebuild_demo|rebuild_demo/spec_rebuild_demo|"
  "fileicon|examples/fileicon_demo|fileicon_demo/spec_fileicon_demo|"
  "scrollbg|examples/scrollbg_demo|scrollbg_demo/spec_scrollbg_demo|"
  "listbox|examples/listbox_demo|listbox_demo/spec_listbox_demo|"
  "navstack|examples/navstackdemo|navstackdemo/spec_navstackdemo|"
  "table|examples/table_demo|table_demo/spec_table_demo|"
  "split|examples/split_demo|split_demo/spec_split_demo|"
  "bindings|examples/bindings_demo|bindings_demo/spec_bindings_demo|"
  "tabs|examples/tabs_demo|tabs_demo/spec_tabs_demo|"
  "menu|examples/menu|menu/spec_menu|"
  "rbind|examples/rbind_demo|rbind_demo/spec_rbind_demo|"
  "typo|examples/typo_demo|typo_demo/spec_typo_demo|"
  "multiselect|examples/multiselect_demo|multiselect_demo/spec_multiselect_demo|"
  "dblclick|examples/dblclick_demo|dblclick_demo/spec_dblclick_demo|"
  "tree|examples/tree_demo|tree_demo/spec_tree_demo|"
  "tabledeleg|examples/tabledeleg_demo|tabledeleg_demo/spec_tabledeleg_demo|"
  "weightclamp|examples/weightclamp_demo|weightclamp_demo/spec_weightclamp_demo|"
  "shortcut|examples/shortcut_demo|shortcut_demo/spec_shortcut_demo|"
  "polish|examples/polish_demo|polish_demo/spec_polish_demo|"
  "vlist|examples/vlist_demo|vlist_demo/spec_vlist_demo|"
  "wshortcut|examples/wshortcut_demo|wshortcut_demo/spec_wshortcut_demo|"
  "multiwindow|examples/multiwindow_demo|multiwindow_demo/spec_multiwindow_demo|"
  "winmenu|examples/winmenu_demo|winmenu_demo/spec_winmenu_demo|"
  "reorder|examples/reorder_demo|reorder_demo/spec_reorder_demo|"
  "overlaytr|examples/overlaytr_demo|overlaytr_demo/spec_overlaytr_demo|"
  "a11y|examples/a11y_demo|a11y_demo/spec_a11y_demo|"
  "material|examples/material_demo|material_demo/spec_material_demo|"
  "themes|examples/themes_demo|themes_demo/spec_themes_demo|"
  "roles|examples/roles_demo|roles_demo/spec_roles_demo|"
  "command|examples/command_demo|command_demo/spec_command_demo|"
  "csssem|examples/csssem_demo|csssem_demo/spec_csssem_demo|"
  "zen|examples/zen_demo|zen_demo/spec_zen_demo|"
  "states|examples/states_demo|states_demo/spec_states_demo|"
  "undo|examples/undo_demo|undo_demo/spec_undo_demo|"
  "textpath|examples/textpath_demo|textpath_demo/spec_textpath_demo|"
  "pills|examples/pills_demo|pills_demo/spec_pills_demo|"
  "icons|examples/icons_demo|icons_demo/spec_icons_demo|"
  "disclosure|examples/disclosure_demo|disclosure_demo/spec_disclosure_demo|"
  "stroker|examples/stroker_demo|stroker_demo/spec_stroker_demo|"
  "vg3d|examples/vg3d_demo|vg3d_demo/spec_vg3d_demo|"
  "golden|examples/golden_gallery|golden_gallery/spec_golden_gallery|"
  # Contract test, not a feature test: AetherUIDriver must expose the same
  # routes on every backend. It is implemented TWICE (aether_ui_test_server.c
  # on win32/macOS, woven into aether_ui_gtk4.c on linux/freebsd) and has
  # drifted before — see docs/design/one-driver-not-two.md. Any app with a
  # canvas and the driver enabled is a valid subject; cmdkind_coverage is used
  # because it has both.
  "routeparity|examples/cmdkind_coverage|route_parity/spec_route_parity|"
  # Behavioural twin of routeparity: asserts each action verb has its NAMED
  # effect, not merely that its route answers 200. Guards the GTK4 driver
  # migration, where private action numbers collide with the shared enum
  # (docs/design/one-driver-not-two.md).
  "driveractions|examples/testable|driver_actions/spec_driver_actions|"
  "frames|apps/frames_demo|frames_demo/spec_frames_demo|"
  "sketchpad|apps/sketchpad|sketchpad/spec_sketchpad|"
  # video_frame decodes in-process via contrib.avcodec, so it needs FFmpeg's
  # dev libraries to LINK. The suite SKIPs where they are absent (see the
  # optional-dependency check in the run loop) rather than failing, since the
  # app demonstrates the region path and is not a portability claim.
  "video_frame|apps/video_frame|video_frame/spec_video_frame|"
  "maerkdown|apps/maerkdown|maerkdown/spec_maerkdown|"
  "falling_blocks|apps/falling_blocks|falling_blocks/spec_falling_blocks|"
  "svg_tetris|apps/svg_tetris|svg_tetris/spec_svg_tetris|"
  "rubiks_cube|apps/rubiks_cube|rubiks_cube/spec_rubiks_cube|"
  "tumbling_cube|apps/tumbling_cube|tumbling_cube/spec_tumbling_cube|AEVG_FREEZE=1"
  "font_picker|apps/font_picker|font_picker/spec_font_picker|"
  "lismusic|apps/LisMusic|LisMusic/spec_lismusic|LIS_OFFLINE=1"
  "turtle|apps/turtle|turtle/spec_turtle|"
)

# grand_perspective: one spec per component, each against a FRESH app scanning
# a FRESH fixture — the fileops spec really trashes a file, so isolation is what
# makes these order-independent. Fixture lives under $HOME: `gio trash` refuses
# /tmp on some systems.
GP_SPECS=(scan_and_list map_nav legend fileops hover_and_resize)

# --rebuild: build every selected suite's app before running it, and treat a
# still-stale binary as a hard error rather than a warning.
#
# OFF by default because the inner loop wants speed: `spec_matrix.sh frames`
# in 20s beats a 100s rebuild while iterating. ON for any run whose NUMBER
# you intend to believe -- CI, a release check, a four-platform sweep.
#
# The warning this complements has caught real phantom failures (a 4-day-old
# frames_demo reporting 11 bogus failures on Windows; 12 on FreeBSD), but a
# warning only tells you the run was worthless AFTER it finishes. This
# prevents it. Worth doing now that aeb's [miss] genuinely means "rebuilt"
# (aeb 63bcb14) -- before that fix, rebuilding could silently no-op.
REBUILD=0
ARGS=()
for a in "$@"; do
    case "$a" in
        --rebuild) REBUILD=1 ;;
        *) ARGS+=("$a") ;;
    esac
done
# ${ARGS[@]} on an EMPTY array is an unbound-variable error under `set -u`
# in bash 3.2, which is what macOS ships (3.2.57). Bash 4+ tolerates it, so
# this passed on Linux and killed the entire matrix on macOS at line 1 --
# "ARGS[@]: unbound variable", zero suites run. The +x guard is the portable
# form: expand only when the array is actually set.
WANT=(${ARGS[@]+"${ARGS[@]}"})

# rebuild_app <appdir> <base> — build one app with whatever this platform has.
# Mirrors the per-OS fallbacks in the binary-resolution block below: aeb where
# its fan-out works, build.sh where it does not (Windows, FreeBSD).
rebuild_app() {
    local appdir="$1" base="$2" log="/tmp/smx_rebuild_${2}.log"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*|FreeBSD)
            ./build.sh "$appdir/$base.ae" "$base" > "$log" 2>&1
            ;;
        *)
            if [ -f "$appdir/.build.ae" ] && command -v aeb >/dev/null 2>&1; then
                aeb "$appdir/.build.ae" > "$log" 2>&1
            else
                ./build.sh "$appdir/$base.ae" "$base" > "$log" 2>&1
            fi
            ;;
    esac
}
want_suite() {
    [ ${#WANT[@]} -eq 0 ] && return 0
    for w in "${WANT[@]}"; do [ "$w" = "$1" ] && return 0; done
    return 1
}

# Wait for the port to stop answering — a lingering app would let the NEXT
# suite interrogate the PREVIOUS app's widget tree, which produces a whole
# family of impossible failures.
# App startup budget: $STARTUP_TRIES * 0.25s. 10s is plenty on Linux, but a
# macOS app on a VM guest can spend ~5s in CoreText font enumeration BEFORE
# the driver binds, then needs the first window render — apps were reported
# "DID NOT START" when they were merely slow. 30s everywhere; the loop still
# exits early the moment /widgets answers, so this costs nothing when fast.
STARTUP_TRIES=${STARTUP_TRIES:-120}
PORT_FREE_TRIES=${PORT_FREE_TRIES:-60}   # 6s at 0.1s: a shutdown we already signalled

# Is $PORT free? Polls until the driver stops answering.
#
# BUDGET: this used to reuse $STARTUP_TRIES (120 = 30s), which is a LAUNCH
# budget and far too generous for "has the app we just told to quit finished
# quitting". Measured 2026-08-15: one suite took 96s wall against 0.4s of
# actual work, and the trace put 30.2s of it in this loop's `sleep 0.25`.
# Teardown signals the app before calling here, so a second is plenty; the
# rare genuine straggler still gets caught by the caller's kill path.
port_free() {
    for _ in $(seq 1 $PORT_FREE_TRIES); do
        # Ask the KERNEL whether anything is listening, rather than making an
        # HTTP request to find out. curl against a dead port still costs a
        # connect() + teardown per poll, and the old loop ran it 75 times per
        # suite; ss reads the socket table. Falls back to curl where ss is
        # absent (macOS/FreeBSD have no ss).
        if command -v ss >/dev/null 2>&1; then
            # LISTEN only. `ss -tln` still lists TIME-WAIT sockets on this
            # kernel, and those linger ~60s after every HTTP request the spec
            # made -- matching them made port_free spin its whole budget on a
            # port with nothing listening (18.3s per suite, measured).
            ss -tln state listening 2>/dev/null | grep -q ":$PORT " || return 0
        else
            curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets" || return 0
        fi
        sleep 0.1
    done
    return 1
}

teardown() {
    local pid="$1" bin="$2"
    curl -s -o /dev/null --max-time 2 -X POST "http://127.0.0.1:$PORT/shutdown" || true
    port_free || { kill "$pid" 2>/dev/null; sleep 0.5; kill -9 "$pid" 2>/dev/null; }
    # Belt-and-suspenders EVERYWHERE, not just FreeBSD: killing $pid only
    # reaps the direct child. A demo that holds a native modal (winmenu) or
    # re-execs outlives it, keeps $PORT bound, and every LATER suite then
    # aborts with "port still busy" — silently truncating the run. Match the
    # basename exactly (-x, not -f) so this never matches our own shell.
    local base; base="$(basename "$bin")"
    # LINUX TRUNCATES THE PROCESS NAME TO 15 CHARS, and `pkill -x` matches
    # against that truncated comm — so an exact match on a 16+ char binary
    # NEVER fires. Eleven binaries in this tree are over the limit
    # (cmdkind_coverage, transitions_demo, multiwindow_demo,
    # grand_perspective, svg_transpile_*, ...), which is precisely the class
    # that leaked: cmdkind_coverage held $PORT after its suite and truncated
    # a whole matrix run with "port still busy". Try the exact name first,
    # then the 15-char prefix the kernel actually stores.
    port_free || pkill -x "$base" 2>/dev/null
    port_free || {
        local short="${base:0:15}"
        [ "$short" != "$base" ] && pkill -x "$short" 2>/dev/null
    }
    wait "$pid" 2>/dev/null
    # pkill only SENDS the signal. Returning here while the process is still
    # dying lets the NEXT suite launch a binary with the same name into the
    # window where a late SIGTERM is still in flight — the new app binds the
    # port, prints "listening", then dies, and the run reports "APP DID NOT
    # START". Observed ~4-in-5 on the macOS VM. Wait for the name to actually
    # clear before handing the port on.
    #
    # SIGNAL, THEN WAIT -- in that order. `port_free` above is checked straight
    # after POST /shutdown, before the app has actually exited, so it often
    # reports free and the kill is skipped; the loop below would then wait its
    # full budget for a process nobody had signalled. That is invisible while
    # pkill -x cannot match a 16-char name (the loop breaks instantly and the
    # matrix charges on), and becomes ~10s of dead time PER SUITE the moment
    # the match is fixed -- apps sitting on screen doing nothing. So make sure
    # the thing we are waiting on has been told to go.
    local watch="${base:0:15}"    # same truncation as above; pgrep -x sees comm
    if pgrep -x "$base" >/dev/null 2>&1 || pgrep -x "$watch" >/dev/null 2>&1; then
        kill "$pid" 2>/dev/null
        pkill -x "$base" 2>/dev/null
        [ "$watch" != "$base" ] && pkill -x "$watch" 2>/dev/null
    fi
    for _ in $(seq 1 40); do
        pgrep -x "$base" >/dev/null 2>&1 || pgrep -x "$watch" >/dev/null 2>&1 || break
        sleep 0.25
    done
}

printf "%-14s %6s %6s   %s\n" SUITE PASS FAIL RESULT
printf -- "------------------------------------------------------------\n"

TOTAL_PASS=0 TOTAL_FAIL=0 SUITES_RED=0

for row in "${SUITES[@]}"; do
    IFS='|' read -r name appdir spec extra <<< "$row"
    want_suite "$name" || continue

    base="$(basename "$appdir")"

    # OPTIONAL SYSTEM DEPENDENCIES. A suite whose app needs one that is not
    # installed here SKIPs -- a provisioning gap is not a test failure, the
    # same rule contrib_build.sh applies to the shim itself. Without this
    # video_frame reports NO BINARY and goes RED on macOS and Windows (0/5
    # FFmpeg dev libraries as of 2026-08-10) purely for lacking a package.
    #
    # Deliberately narrow: it skips only where the named pkg-config modules
    # are absent, so a box that HAS them still runs the suite and a genuine
    # build break is still red.
    case "$name" in
        video_frame) _need="libavcodec libavformat libavutil libswscale libswresample" ;;
        *)           _need="" ;;
    esac
    if [ -n "$_need" ]; then
        if ! pkg-config --exists $_need 2>/dev/null; then
            printf "%-14s %6s %6s   %s\n" "$name" - - "SKIP (no ffmpeg dev libs)"
            continue
        fi
    fi

    # video_frame needs a clip with an AUDIO track: without one it falls back
    # to the scene clock and presents nothing, so its held/clock assertions
    # fail. The suite must not depend on a file that happens to be lying
    # around -- it did, and passed here while failing on macOS purely because
    # /tmp/vidtest/clip.mp4 existed on one box. Generate it per run.
    #
    # Under target/ rather than /tmp: gitignored, per-checkout, alongside the
    # other build output, and NOT swept mid-session the way /tmp is on macOS
    # (periodic cleaner) and FreeBSD (clear_tmp_enable).
    if [ "$name" = "video_frame" ]; then
        # Find ffmpeg without depending on the login shell's PATH: Homebrew
        # puts it in /usr/local/bin (Intel) or /opt/homebrew/bin (Apple
        # silicon), and a non-login ssh session on macOS has neither. This
        # is the same PATH trap that bites on winbaz.
        _FFM=""
        for _c in ffmpeg /usr/local/bin/ffmpeg /opt/homebrew/bin/ffmpeg; do
            command -v "$_c" >/dev/null 2>&1 && { _FFM="$_c"; break; }
        done
        export VF_CLIP="$ROOT/target/fixtures/clip.mp4"
        if [ -n "$_FFM" ] && [ ! -f "$VF_CLIP" ]; then
            mkdir -p "$ROOT/target/fixtures"
            "$_FFM" -y -f lavfi -i testsrc=size=320x240:rate=15:duration=6 \
                   -f lavfi -i "sine=frequency=440:duration=6" \
                   -pix_fmt yuv420p -c:v libx264 -c:a aac -shortest \
                   "$VF_CLIP" >/dev/null 2>&1 || true
        fi
    fi

    if [ "$REBUILD" = "1" ]; then rebuild_app "$appdir" "$base"; fi
    bin="target/build/$appdir/bin/$base"
    # .build.contrib.ae nodes emit under target/build.contrib/ — the node
    # TYPE routes the output tree (filename-is-the-route), so the contrib
    # apps' binaries never appear under target/build/. Cost a "silent build
    # failure" diagnosis on macvm that was really this lookup reading the
    # wrong tree while the binary sat built in the right one.
    if [ ! -x "$bin" ] && [ -f "$ROOT/$appdir/.build.contrib.ae" ]; then
        cbin="target/build.contrib/$appdir/bin/$base"
        [ -x "$cbin" ] && bin="$cbin"
    fi
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            # PREFER the aeb fan-out artifact, fall back to build.sh.
            #
            # This used to read build/$base.exe unconditionally, because aeb's
            # fan-out could not build UI apps on MSYS. It can now — so the
            # unconditional read meant the matrix ran whatever build.sh last
            # left behind while aeb wrote somewhere else entirely. Measured
            # 2026-08-17: `golden` was red with build/golden_gallery.exe from
            # Aug 10 while target/build/'s Aug 14 binary passed ALL-GREEN
            # against the SAME goldens. That is a false red, and the
            # neighbouring staleness warning could not see it because it
            # checks whichever path this line picked.
            bin="target/build/$appdir/bin/$base.exe"
            # Same contrib-tree fallback as the default branch: this
            # unconditional reset used to skip it, so winbaz measured a
            # build.sh-era exe so stale it predated the /pixel route.
            if [ ! -x "$bin" ] && [ -f "$ROOT/$appdir/.build.contrib.ae" ]; then
                cbin="target/build.contrib/$appdir/bin/$base.exe"
                [ -x "$cbin" ] && bin="$cbin"
            fi
            if [ ! -x "$bin" ]; then
                bin="build/$base.exe"
                if [ ! -x "$bin" ]; then
                    ./build.sh "$appdir/$base.ae" "$base" \
                        > "/tmp/smx_$base.log" 2>&1 || true
                fi
            fi
            ;;
        FreeBSD)
            # No aeb on the box → prefer the fan-out artifact if present,
            # else build.sh into build/ on demand. Absolutized for safety.
            #
            # Under --rebuild, prefer build/ UNCONDITIONALLY: rebuild_app just
            # wrote there, while target/build/ holds whatever an older aeb
            # fan-out left behind. Without this the freshly built binary is
            # ignored, the stale one fails the post-rebuild check, and every
            # suite reports STALE AFTER REBUILD -- 51 of them, on a run where
            # each rebuild had in fact succeeded ("Built: build/icons_demo").
            if [ "$REBUILD" = "1" ] && [ -x "build/$base" ]; then
                bin="build/$base"
            fi
            if [ ! -x "$bin" ] && [ -f "$ROOT/$appdir/.build.contrib.ae" ]; then
                cbin="target/build.contrib/$appdir/bin/$base"
                [ -x "$cbin" ] && bin="$cbin"
            fi
            if [ ! -x "$bin" ]; then
                bin="build/$base"
                [ -x "$bin" ] || ./build.sh "$appdir/$base.ae" "$base" \
                    > "/tmp/smx_$base.log" 2>&1 || true
            fi
            [ -x "$bin" ] && bin="$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")"
            ;;
    esac
    if [ ! -x "$bin" ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "NO BINARY ($bin) — run: aeb .all.ae (or see /tmp/smx_*.log on Windows)"
        SUITES_RED=$((SUITES_RED + 1))
        continue
    fi

    # STALENESS WARNING. This harness runs whatever binary it finds; it does
    # NOT rebuild. That has produced false greens repeatedly: four days of
    # them on FreeBSD (matrix read target/build/ while the real artifacts sat
    # in build/), a stale aeb build that masked a compile error outright, and
    # a frames_demo.exe four days older than its sources reporting 11 bogus
    # failures on Windows.
    #
    # This check MUST come after the platform case above, not before it: that
    # case REASSIGNS $bin (build/$base.exe on Windows, build/$base on
    # FreeBSD), so checking earlier inspects a path those platforms never
    # launch — which is exactly how the Windows staleness went unreported
    # while seven other suites on the same run were correctly flagged.
    #
    # It bites hardest during falsification — edit a source, run the matrix,
    # see the OLD binary stay green, and wrongly conclude the test cannot
    # detect the change. Warn loudly rather than silently measuring the past.
    newer="$(find "$appdir" ui vg -name '*.ae' -newer "$bin" 2>/dev/null | head -3)"
    if [ -n "$newer" ]; then
        if [ "$REBUILD" = "1" ]; then
            # We just built this and it is STILL older than its sources, so
            # the build silently did nothing. Under --rebuild the whole point
            # is that the number can be believed; carrying on would produce
            # exactly the false green the flag exists to prevent.
            printf "%-14s %6s %6s   %s\n" "$name" - - "STALE AFTER REBUILD — see /tmp/smx_rebuild_$base.log"
            printf "       %s\n" $newer >&2
            SUITES_RED=$((SUITES_RED + 1))
            continue
        fi
        printf "  \033[33mWARN\033[0m %s: binary older than sources — measuring a STALE build.\n" "$name" >&2
        printf "       %s\n" $newer >&2
        # Contrib apps (LisMusic, video_frame) carry .build.contrib.ae to stay
        # out of the .all.ae scan; everything else is .build.ae. Name the file
        # that exists — a hint naming a missing node cost a debugging loop.
        node=".build.ae"
        [ -f "$ROOT/$appdir/.build.contrib.ae" ] && node=".build.contrib.ae"
        printf "       rebuild: aeb %s/%s (or pass --rebuild)\n" "$appdir" "$node" >&2
    fi

    port_free || { echo "port $PORT still busy; aborting"; exit 1; }

    # Launch, waiting for the driver to answer. RETRIED, because a launch can
    # fail for reasons that have nothing to do with the app: macOS rate-limits
    # rapid relaunches of the same binary (every failing log carries
    # "NSXPCSharedListener ... Connection interrupted", and the app binds the
    # port, prints "listening", then dies). Measured on the .160 VM: 2-in-4
    # BARE launches fail with no test harness involved, and a 60s pause always
    # clears it. Backing off and retrying turns a spurious red into a real
    # result; a suite that is genuinely broken still fails all attempts.
    ready=0; log=""; pid=""
    for attempt in 1 2 3; do
        log="$(mktemp)"
        # shellcheck disable=SC2086
        aui_launch AETHER_UI_TEST_PORT=$PORT $extra -- "$bin" >"$log" 2>&1 &
        pid=$!
        for _ in $(seq 1 $STARTUP_TRIES); do
            if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.25
        done
        [ "$ready" -eq 1 ] && break
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        pkill -x "$(basename "$bin")" 2>/dev/null
        [ "$attempt" -lt 3 ] && sleep $((attempt * 5))
    done
    if [ "$ready" -ne 1 ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "APP DID NOT START after 3 tries (see $log)"
        SUITES_RED=$((SUITES_RED + 1))
        kill "$pid" 2>/dev/null
        pkill -x "$(basename "$bin")" 2>/dev/null
        continue
    fi

    out="$(UI_SPEC="$spec" tests/run_spec.sh 2>&1)"
    rc=$?
    teardown "$pid" "$bin"

    pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
    fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
    pass=${pass:-0}; fail=${fail:-0}

    retried=""
    if [ "$rc" -ne 0 ] && [ "$pass" = "0" ] && [ "$fail" = "0" ]; then
        # The SPEC PROGRAM died before reporting anything — a launch-layer
        # failure (`ae run` fresh-exe exec killed under MSYS subprocess
        # pressure; seen as "Program crashed (signal 1)"), not an assertion
        # outcome. Retry ONCE with a fresh app. Assertion failures (fail>0)
        # are never retried — only the nothing-ran class is.
        sleep 2
        port_free || { echo "port $PORT still busy; aborting"; exit 1; }
        # shellcheck disable=SC2086
        aui_launch AETHER_UI_TEST_PORT=$PORT $extra -- "$bin" >"$log" 2>&1 &
        pid=$!
        ready=0
        for _ in $(seq 1 $STARTUP_TRIES); do
            if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.25
        done
        if [ "$ready" -eq 1 ]; then
            # Virgin cache for the retry: the first crash may have PINNED a
            # bad exe under ~/.aether/cache (the poisoned-publish shape) —
            # rerunning it would fail deterministically. A throwaway
            # AETHER_CACHE_DIR forces a fresh compile+exec.
            rcache="$(mktemp -d)"
            out="$(AETHER_CACHE_DIR="$rcache" UI_SPEC="$spec" tests/run_spec.sh 2>&1)"
            rc=$?
            rm -rf "$rcache"
            retried=" (retried)"
        fi
        teardown "$pid" "$bin"
        pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
        fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
        pass=${pass:-0}; fail=${fail:-0}
    fi

    if [ "$rc" -ne 0 ] && [ "$pass" = "0" ] && [ "$fail" = "0" ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "SPEC ERROR"
        printf '%s\n' "$out" | tail -4 | sed 's/^/                              | /'
        SUITES_RED=$((SUITES_RED + 1))
        rm -f "$log"
        continue
    fi

    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail))
    if [ "$fail" -gt 0 ]; then
        SUITES_RED=$((SUITES_RED + 1))
        printf "%-14s %6s %6s   %s\n" "$name" "$pass" "$fail" "RED"
        printf '%s\n' "$out" | grep -E '✗|FAIL|not ok' | head -6 \
            | sed 's/^/                              | /'
    else
        printf "%-14s %6s %6s   %s\n" "$name" "$pass" "$fail" "green$retried"
    fi
    rm -f "$log"
done

# --- grand_perspective (the real vg app: canvas hit-testing, resize, fileops) --
GP_BIN="target/build/apps/grand_perspective/bin/grand_perspective"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        GP_BIN="build/grand_perspective.exe"
        if [ ! -x "$GP_BIN" ]; then
            ./build.sh apps/grand_perspective/grand_perspective.ae \
                grand_perspective > /tmp/smx_gp.log 2>&1 || true
        fi
        ;;
    FreeBSD)
        if [ ! -x "$GP_BIN" ]; then
            GP_BIN="build/grand_perspective"
            [ -x "$GP_BIN" ] || ./build.sh apps/grand_perspective/grand_perspective.ae \
                grand_perspective > /tmp/smx_gp.log 2>&1 || true
        fi
        [ -x "$GP_BIN" ] && GP_BIN="$(cd "$(dirname "$GP_BIN")" && pwd)/$(basename "$GP_BIN")"
        ;;
esac
for gp in "${GP_SPECS[@]}"; do
    want_suite "gp_$gp" || continue
    if [ ! -x "$GP_BIN" ]; then
        printf "%-14s %6s %6s   %s\n" "gp_$gp" - - "NO BINARY — run: aeb .all.ae"
        SUITES_RED=$((SUITES_RED + 1))
        continue
    fi
    port_free || { echo "port $PORT still busy; aborting"; exit 1; }

    fix="$(mktemp -d "$HOME/.gp-ci-XXXXXX")"
    mkdir -p "$fix/sub"
    head -c 400000 /dev/urandom > "$fix/big.bin"
    head -c 250000 /dev/urandom > "$fix/mid.bin"
    head -c 200000 /dev/urandom > "$fix/sub/inner.bin"
    # The app and the spec are NATIVE binaries — hand them a native path,
    # not an MSYS one (/c/Users/... opens as no-such-dir in the Windows CRT
    # and gp scans nothing).
    fix_app="$fix"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) fix_app="$(cygpath -m "$fix")" ;;
    esac

    log="$(mktemp)"
    aui_launch AETHER_UI_TEST_PORT=$PORT AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" \
        -- "$GP_BIN" >"$log" 2>&1 &
    pid=$!
    ready=0
    for _ in $(seq 1 $STARTUP_TRIES); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.25
    done
    if [ "$ready" -ne 1 ]; then
        printf "%-14s %6s %6s   %s\n" "gp_$gp" - - "APP DID NOT START (see $log)"
        SUITES_RED=$((SUITES_RED + 1))
        kill "$pid" 2>/dev/null
        [ "$FREEBSD" -eq 1 ] && pkill -f "$GP_BIN" 2>/dev/null
        rm -rf "$fix"
        continue
    fi

    out="$(AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" UI_SPEC="grand_perspective/spec_${gp}" \
           tests/run_spec.sh 2>&1)"
    teardown "$pid" "$GP_BIN"

    pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
    fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
    pass=${pass:-0}; fail=${fail:-0}
    retried=""
    if [ "$pass" -eq 0 ] && [ "$fail" -eq 0 ]; then
        # Nothing ran — the spec-launch flake (same class as the widget
        # loop's retry). The spec never touched the fixture, so it's still
        # pristine: relaunch the app against it and retry once.
        sleep 2
        port_free || { echo "port $PORT still busy; aborting"; exit 1; }
        aui_launch AETHER_UI_TEST_PORT=$PORT AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" \
            -- "$GP_BIN" >"$log" 2>&1 &
        pid=$!
        ready=0
        for _ in $(seq 1 $STARTUP_TRIES); do
            if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.25
        done
        if [ "$ready" -eq 1 ]; then
            # Virgin cache — see the widget loop's retry for why.
            rcache="$(mktemp -d)"
            out="$(AETHER_CACHE_DIR="$rcache" AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" \
                   UI_SPEC="grand_perspective/spec_${gp}" tests/run_spec.sh 2>&1)"
            rm -rf "$rcache"
            retried=" (retried)"
        fi
        teardown "$pid" "$GP_BIN"
        pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
        fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
        pass=${pass:-0}; fail=${fail:-0}
    fi
    rm -rf "$fix" "$log"
    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail))
    if [ "$fail" -gt 0 ] || { [ "$pass" -eq 0 ] && [ "$fail" -eq 0 ]; }; then
        SUITES_RED=$((SUITES_RED + 1))
        printf "%-14s %6s %6s   %s\n" "gp_$gp" "$pass" "$fail" "RED"
        printf '%s\n' "$out" | grep -E '✗|FAIL|not ok' | head -6 \
            | sed 's/^/                              | /'
    else
        printf "%-14s %6s %6s   %s\n" "gp_$gp" "$pass" "$fail" "green$retried"
    fi
done

printf -- "------------------------------------------------------------\n"
printf "%-14s %6s %6s   %s\n" TOTAL "$TOTAL_PASS" "$TOTAL_FAIL" \
    "$([ "$SUITES_RED" -eq 0 ] && echo "all green" || echo "$SUITES_RED suite(s) red")"
[ "$SUITES_RED" -eq 0 ]
