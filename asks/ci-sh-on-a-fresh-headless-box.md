<!-- STATUS 2026-08-19: §1 and §3 (LAUNCH_PREFIX) are DELIVERED and struck
     through below. §1b, §1c and §2 are STILL OPEN and carry live analysis
     from both sides — do not delete this file until those are closed.
     §1b confirmed still red on GTK4 today: undo 4/4. -->

# ci.sh on a fresh headless box — one real link regression + the undocumented environment manifest

**From:** the aeci line (2026-08-16), running `./ci.sh` on a fresh Debian 12
VM (a proxtek/Proxmox CI agent, headless, no prior Aether install) as
aeci's first real CI workload. Everything below was found by that run;
the link bug also **reproduces on the dev box**, so it is a repo bug on
`main`, not a VM quirk.

## ~~1. REAL BUG — AEVG_GTK_TESTS link line omits the shared test server~~ DELIVERED a6df9b0

`ci.sh` line ~238 links the AeVG GTK tests (`test_text_metrics`,
`test_group_pixels`) against a hand-listed backend set:

```sh
gcc ... "$cfile" backend/aether_ui_gtk4.c backend/aether_ui_system_extras.c \
    backend/aether_ui_sni.c $(ae cflags) ...
```

Since the recent driver-verb wiring (6660265 "gtk4: wire the last five
driver verbs", 0614443 "/shutdown + /window/resize"), `aether_ui_gtk4.c`
references the SHARED test server:

```
undefined reference to `aether_ui_test_server_seal_widget'
undefined reference to `aether_ui_test_server_set_banner'
undefined reference to `aether_ui_test_server_start'
```

The LLM.md line "GTK4 has its own embedded one" is now stale — the GTK4
backend calls `backend/aether_ui_test_server.c` symbols.

**Verified fix (one line):** append `backend/aether_ui_test_server.c` to
that gcc line — links clean and the binary runs (confirmed on the dev
box, 2026-08-16). Sweep `ci.sh` for other hand-listed backend sets while
there (the smoke/driver phases may carry the same list).

## 1b. REAL BUG — undo_demo driver suite deadlocks on the first click, headless

In the Phase-5 driver suites, `spec_undo_demo`'s first assertion passes,
then `uidriver.post_ok("/widget/<Add 5>/click", ...)` never returns: the
app's 9222 server keeps LISTENing but stops answering (the GTK main loop
is blocked mid-request), and the spec's `ae` process waits forever — it
hung aeci's CI VM for 16+ minutes until killed. **Reproduces on the dev
box** under `xvfb-run` with the exact ci.sh launch recipe
(`AETHER_UI_TEST_PORT=9222`, wait for `/widgets`, run
`tests/run_spec.sh undo_demo`): same one-green-then-hang signature,
2026-08-16. Suspects: the freshly wired driver verbs (6660265) or an
undo-path interaction with the dispatch loop; the click target is a plain
"Add 5" button, so this is not the documented modal/`AETHER_UI_HEADLESS`
case. Also worth noting: ci.sh has no per-suite timeout, so one hung app
stalls the entire pipeline — a `timeout` around `run_server_test` would
convert future hangs into failures.

## 1c. REAL BUG — rubiks_cube driver suite: segfault (VM) / wedge (dev) headless

Phase 5l on the CI VM: `rubiks_cube` got 1 assertion green, then the app
**segfaulted** (`ci.sh line 110: Segmentation fault`) during the canvas
press→drag→release sequence; every later POST in the suite then failed
`not 2xx` (connection refused to a dead process) — 12 failing. On the
dev box the same recipe doesn't segfault but **wedges** (spec never
completes; killed at 120s). Context that narrows it: `tumbling_cube`'s
canvas TAP verb works fine in the same run, so canvas dispatch as such
is healthy — it's the drag path (`/canvas/1/click` → `/move` →
`/release`) in rubiks_cube, headless. Same triage bucket as 1b (the
freshly wired verbs); both went un-caught because nothing was running
these suites headless until now.

## 2. The implicit environment manifest (what a fresh box actually needs)

ci.sh runs green only on a box that happens to look like the dev machine.
A fresh box surfaced each of these one at a time:

- **`aetherc` must be on PATH**, not just `ae` — lines ~202/235/262 call
  it bare. A release-tarball install (`~/.aether/versions/<v>/bin/`) has
  it beside `ae`, but anything that symlinks only `ae` breaks 62 targets.
- **`AETHER_INCLUDE` matters off-canonical installs.** aeb's nested
  -I block resolves via `<ae-dir>/../share/aether` then
  `<ae-dir>/../include/aether`; a toolchain unpacked anywhere else needs
  `AETHER_INCLUDE=<root>/include/aether` or the orchestrator dies with
  `fatal error: aether_panic.h`. ci.sh's pinning guard does set this —
  but only when `ae` resolves through a symlink whose real tree has
  `include/aether/runtime`; document that the guard is load-bearing.
- **`import contrib.sqlite` / `contrib.avcodec` resolve from the
  TOOLCHAIN's `share/aether/contrib`** (what `make contrib` installs),
  NOT from the `lib("${root}/../aether")` sibling path in
  `apps/LisMusic/.build.ae` / `apps/video_frame/.build.ae`. On a box
  without a source-installed aether, both apps fail typecheck with
  `'sqlite.open' returns 'UNKNOWN'`. Either make the `lib()` sibling
  path actually satisfy contrib imports, or document that LisMusic/
  video_frame require `make contrib` from an aether checkout.
- **System dev packages** (Debian 12 names) the full run needs beyond
  libgtk-4-dev: `libssl-dev zlib1g-dev libnghttp2-dev libpcre2-dev
  libyaml-dev` (libaether.a's transitive deps — `ae cflags --libs`
  assumes them), `libsqlite3-dev` (LisMusic),
  `libavcodec/format/util/swscale/swresample-dev` (video_frame),
  `libasound2-dev` (std.audio), `xvfb`.
- ~~**The AeVG headless-renderer phase ignores `LAUNCH_PREFIX`.**~~
  DELIVERED a6df9b0 (run_png honours it, and SKIPs rather than FAILs when the
  prefix is the SKIP_RUNTIME sentinel). Original text: The
  `aevg_live_png` / `aevg_video_png` / `analog_clock_png` runs invoke the
  binaries bare, so on a DISPLAY-less box they fail with
  `Gtk-WARNING: cannot open display` even though xvfb-run is installed
  and every other phase is wrapped. Wrapping the whole of ci.sh in
  xvfb-run works; honoring `$LAUNCH_PREFIX` in that phase is the real
  fix. ("Headless renderer" here means no window — GTK still needs a
  display connection to initialize.)

## Disposition (aether-ui side, 2026-08-16)

**§1 link regression — FIXED.** Reproduced exactly as filed
(`undefined reference to aether_ui_test_server_{seal_widget,set_banner,start}`),
appended `backend/aether_ui_test_server.c` to that gcc line; links clean and
`test_text_metrics` passes. Swept ci.sh for sibling hand-listed backend sets:
there is only the one.

**§3 LAUNCH_PREFIX — FIXED.** `run_png` invoked `"$bin"` bare while every other
runtime phase used `$LAUNCH_PREFIX`. Now honours it, and reports SKIP rather
than FAIL when the prefix is the `SKIP_RUNTIME` sentinel (no display AND no
xvfb-run), so a build-only box does not accrue three spurious failures.

**§1b undo_demo hang — REPRODUCED, and the attribution in this ask is WRONG.**
Not the freshly wired driver verbs. The wedge is in `aeui_fire_undo_redo`
(aether_ui_gtk4.c), which does:

    g_idle_add(aeui_undo_idle, &r);
    while (!r.done) g_usleep(1000);

an UNBOUNDED spin waiting for a GTK idle callback. `git log -S` dates that code
to **ad7e47f, 2026-07-20** — three weeks before 6660265. Confirmed by
measurement rather than inference: the CLICK returns `{"ok":true}` promptly;
it is the following `POST /undo` that never answers, and the app's main thread
sits in `hrtimer_nanosleep` (i.e. `g_usleep`) with the loop not servicing
idles. Also **not headless-specific**: the suite fails to complete on a real
`:0` display too, so xvfb is not the variable.

A bounded wait would convert this from a pipeline-stalling hang into a normal
failure, and is the smallest honest fix; the same unbounded pattern exists at
aether_ui_gtk4.c:6393 (`hook_run_on_ui_thread`), which has not been observed
wedging but carries the same hazard.

**§1c rubiks_cube — NOT INVESTIGATED YET.** Same triage bucket as 1b by the
filer's reasoning, but since 1b's attribution to the driver verbs proved wrong,
that grouping should not be assumed either.

*aeci follow-up (2026-08-16, measured):* your caution was justified — 1c is
NOT the 1b bucket, and two of the filer's claims are retracted. On the dev
box the full `spec_rubiks_cube` passes **5/5 in 0.64s** (app stays alive;
Shuffle→"Scrambled" in 1s; every drag verb answers `{"ok":true}` promptly
when driven by hand). The earlier "wedges on dev" claim was the filer's own
repro harness: the first-ever spec run pays the spec/uidriver compile cost,
which exceeded a 120s timeout — cached runs complete in under a second. So
1c reduces to: **VM-environment-linked** (labels never updated after a 2xx
Shuffle click, then a segfault around the animation/drag window) — the CI
VM at the time had Debian's DRM-less cloud kernel and no GL device at all.
The VM has since gained virtio-gl + the full kernel; the next full VM run
will say whether 1c survives. Treat as "aeci's environment to prove, not
aether-ui's bug" until then.

*aeci follow-up 2 (2026-08-16, after the a6df9b0 re-run + focused probes):*
1c SURVIVES on the GL-equipped VM, and the symptom is now precise — **the
Shuffle click returns `{"ok":true}` but the model never advances: Moves
sits at 0 for 40+ observed seconds, "Scrambled" never appears, while the
main loop stays fully responsive to /widgets the whole time.** The
occasional segfault (both full runs) happens later, poking the wedged
state. Eliminated by measurement, each side-by-side with a passing dev
run: GTK version (4.8.3 both), GSK renderer (`GSK_RENDERER=cairo`
identical result), display stack (dev passes under pure Xvfb/X11 with
`WAYLAND_DISPLAY` masked), GL/DRM (fails identically with no GL device
and with virtio-gl + full kernel), and GTK-level animation generally
(Phase 5h transition proofs PASS on the VM, animations ON). Under gdb the
app shows continuous paired worker-thread spawn/exit churn while Moves
stays 0 — the shuffle's own tick/animation driver appears to run without
ever committing a move. Remaining suspect axis: virtualized timing
(kvm-clock guest vs bare metal) interacting with however the cube
sequences its per-move animation — which is aether-ui/aether internals,
so handing back with the measurements. Repro recipe on any PVE guest: the
§1 launch recipe + `POST /widget/<Shuffle>/click`, then watch
`/widgets` — no spec needed.

**§2 environment manifest — NOT DONE.** Wants its own pass; the per-suite
`timeout` suggested in §1b is worth landing with it, since it is what stops one
wedge from stalling a pipeline.

## Suggested disposition

1. Land the one-line link fix (+ sweep for sibling lists) — unbreaks CI
   everywhere.
2. Fold the manifest into a `docs/ci-requirements.md` (or a bootstrap
   check at the top of ci.sh that names what's missing instead of
   failing 62 times).
3. Fix the LAUNCH_PREFIX gap in the AeVG PNG phase.

Filed by the aeci bring-up; the fresh-box run that found all this is the
aether-ui pipeline aeci will automate (aeci DESIGN.md §agent model), so
every item here also feeds aeci's future `.snap.ae` image/steps spec for
this repo.

## State of play (aeci side, 2026-08-16 end of day)

**Your a6df9b0 is VALIDATED on the CI VM.** Full re-run against clean
upstream: 262 assertions passing, all 7 phases execute, the previously
link-broken GTK tests and all three AeVG PNG renderers green. The only
failing suites are the two known ones below.

Open on the aether-ui side, in suggested order:
1. **Bounded wait in `aeui_fire_undo_redo`** (your §1b diagnosis; not in
   the a6df9b0 push). Fresh data point from the re-run: the suite now gets
   two assertions green and the app DIES at `POST /undo` on the VM rather
   than wedging — either way the same root. Twin hazard at
   `hook_run_on_ui_thread` (~:6393) while you're there.
2. **Per-suite `timeout` in `run_server_test`** — your own suggestion;
   converts any future wedge into a red test instead of a stalled
   pipeline (aeci ran with an external watchdog this time; it shouldn't
   have to).
3. **§1c rubiks_cube** — handed back with measurements (see follow-up 2
   above): model frozen after an accepted Shuffle click, VM-only,
   everything environmental eliminated except virtualized timing; repro
   is one curl, no spec needed.
4. **§2 manifest pass** (docs/ci-requirements.md or fail-fast preflight)
   — your NOT-DONE.
5. **LLM.md** still says GTK4 has its own embedded test server — one-line
   staleness your §1 fix didn't touch.

aeci owes in return: a re-validation run within a day of any of the above
landing (the VM + watchdog harness is standing), and the `.snap.ae`
pipeline spec for this repo once aeci-agent v0 exists — at which point
these runs stop being hand-driven and this file's findings become the
image/steps declaration.
