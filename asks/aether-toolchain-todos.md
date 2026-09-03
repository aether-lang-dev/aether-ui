<!-- STATUS 2026-09-02: NEW consolidated hand-off. All items OPEN unless marked.
     This is the single "everything the toolchain/runtime side needs to do for
     aether-ui" list. Deep detail + working recipes for the iOS/PCRE items live
     in asks/ios-ipados-libaether-and-appstore.md (keep both). -->

# To the aether toolchain / runtime maintainers — everything aether-ui needs

**From:** the aether-ui line (2026-09-02). The UI side is done and green: the
iOS/iPadOS UIKit backend implements all 287 ABI functions, renders real vg apps
(rubiks_cube, analog_clock) end-to-end, is gated in CI (ci.sh Phase 1e:
compile + link + pixel-checked render via Mac Catalyst), and the AetherUIDriver
control server is now OUT of release builds by default. What's left to actually
**ship an aether-ui app on the App Store is almost entirely on your side** —
libaether builds + runtime posture. This is that list, prioritised.

Priorities: **P0 = nothing ships without it** · **P1 = App Review will reject
without it** · **P2 = quality / nice-to-have**.

---

## P0 — libaether for the Apple targets (the hard blocker)

**1. Official libaether cross-builds.** There is no libaether for any iOS
triple today — only the host `x86_64-apple-macos` `.a`, which cannot link into
an iOS/simulator/Catalyst binary (platform mismatch). Ship first-class
`ae`/`aeb` cross-targets (or a documented `make` recipe) for:
- `arm64-apple-ios` (device)
- `arm64-apple-ios-simulator` (+ `x86_64-…-simulator` for Intel hosts)
- `<arch>-apple-ios<ver>-macabi` (Mac Catalyst — how aether-ui runs/specs it today)

*Working recipe (proved on Catalyst, 127/131 files): see
asks/ios-ipados-libaether-and-appstore.md §1. Excludes: `std/audio/aether_audio.c`
(compile as `.m` or omit), `pcre2_jit_match.c` + `pcre2_jit_misc.c` (JIT internals
`#include`d by `pcre2_jit_compile.c`), `runtime/libaether_sandbox_preload.c`
(DYLD shim).*

**2. PCRE2 defines are load-bearing — fail loudly if missing.** `aether_regex.c`
compiles to a **stub that always errors** unless built with
`-DAETHER_HAS_PCRE2 -DAETHER_VENDOR_PCRE2 -I…/std/regex`. The pcre2 objects link
fine without them, so nothing errors at build — but every `regex.compile`
returns the stub error, silently breaking ALL regex-based code (e.g.
`vg/svg/normalizer.parse_path` → **rubiks_cube drew only its background** until we
found this). Any libaether build for a new target MUST set these; a build that
omits them should FAIL, not stub.

**Acceptance:** an aether-ui app links against the target libaether and renders a
vg scene (path fills present), on device and simulator.

---

## P1 — App Store compliance (runtime posture)

**3. PCRE2 JIT OFF on iOS.** *Critical.* PCRE2 JIT (`SUPPORT_JIT`, via
`pcre2_jit_compile.c`) generates executable code at runtime (`mmap` `PROT_EXEC`).
iOS forbids writable-executable memory for non-WebKit apps → **auto-reject or
crash on device**. Build the iOS/simulator/device libaether with JIT disabled
(interpreted PCRE2 is correct and sufficient — it's what renders the cube). Make
JIT a per-target switch, default-off for every Apple mobile triple.

**4. No writable-executable memory anywhere in the runtime.** Beyond PCRE2 JIT,
confirm the scheduler / actors / any codegen never `mmap` `PROT_WRITE|PROT_EXEC`
and use no runtime trampolines. A single W^X allocation fails App Review's static
checks.

**5. Required-Reason API list → privacy manifest.** `std.fs`/`std.os` (and any
`UserDefaults`-like store) almost certainly call Apple "required-reason" APIs
(file-timestamp, disk-space, system-boot-time, `NSUserDefaults`). These need a
declared reason in the app's `PrivacyInfo.xcprivacy` or the app is rejected.
Publish which required-reason APIs libaether touches, per subsystem, so app
authors can fill the manifest.

**6. Export-compliance statement for the bundled crypto.** libaether ships
AES/crypto; apps must answer `ITSAppUsesNonExemptEncryption`. State what crypto
ships and whether it's HTTPS-exempt, so authors can answer the App Store form.

**7. (Belt-and-braces) a runtime "no listening sockets in release" posture.** The
UI side already compiles the AetherUIDriver control server OUT of release builds
by default (commit dd3c0cc1). A libaether-level assertion that no server binds in
a release build would make the whole toolchain provably socket-free when shipped.

**Acceptance:** a release iOS build passes App Review's static checks (no
executable memory, declared required-reason APIs, export answered) — verifiable
before submission with the App Store validation pass.

---

## P1 — packaging / toolchain support

**8. An iOS packaging path in `aeb`.** aether-ui already selects the UIKit backend
via `AETHER_UI_TARGET=ios` (dormant — no iOS libaether to link). To produce a
runnable `.app` we need: `aetherc --emit=csrc` (or equivalent) → clang for the
iOS triple → link against the target libaether + backend, then a bundle
(Info.plist, code-signing, `.ipa`/xcarchive). Owning the cross-compile +
bundle steps in `aeb` (a `ui-ios` target) is the clean home; the UI side can
supply the backend source list and framework set.

**9. An ASYNC file-picker ABI.** `char* aether_ui_file_open/save/pick_folder` are
synchronous; iOS only has async `UIDocumentPickerViewController`. The backend
returns empty today. The ABI needs an async/callback variant (better on every
platform). See asks/ios-ipados-libaether-and-appstore.md §3b.

---

## P2 — minor / FYI

**10. `lrint` prototype mismatch.** Generated app C declares
`int64_t lrint(double);` while the SDK has `long int lrint(double)`. Benign on
LP64 (macabi/ios), but it emits a conflicting-types note on every app build.
Match the platform prototype (or `#include <math.h>`).

---

## Not asks — done on the aether-ui side (context)

- 4th backend `backend/aether_ui_uikit.m` — UIKit, all 287 ABI functions real
  (tray/menu-bar are documented iOS-N/A), gated by ci.sh Phase 1e.
- `ui_backend` `AETHER_UI_TARGET=ios` arm (frameworks: UIKit / Foundation /
  QuartzCore / CoreGraphics / CoreText / ImageIO / UserNotifications).
- Control server (AetherUIDriver) OUT of app builds by default —
  `aether_ui_no_control.c` vs `aether_ui_test_server.c`, opt in with
  `AETHER_UI_WITH_DRIVER=1`.
- End-to-end proof: rubiks_cube's own `.ae` → C → linked with a from-source
  Catalyst libaether + this backend → renders the 3D cube (see
  asks/ios-ipados-libaether-and-appstore.md for the debug trail).

---

## Item 10 follow-up (aether-ui line, 2026-09-03) — it IS ours to emit, but we cannot spell it correctly

You asked to be pointed at the emitting file. It is not one file: **30 modules
under `vg/` declare `extern lrint(x: float) -> long`** (vg/module.ae:97,
vg/live.ae:362, vg/font.ae:56, vg/render3d.ae:47, vg/geom/easing.ae:27,
vg/geom/path_builder.ae:50, …). Confirmed in the generated C, e.g.
`target/build/apps/vg_demo/vg_demo.c:3759`:

```c
// Extern C function: lrint
int64_t lrint(double);
```

against libm's real `long lrint(double)`. On LP64 (macOS/iOS/Linux) both are
64-bit so it is a warning, not a miscompile — but it is a warning in every
generated app, and clang emits it as `-Wincompatible-library-redeclaration`.

**Why we cannot just fix the declaration.** `-> long` emits `int64_t` and
`-> int` emits `int` (measured); neither is C `long`, and per
`docs/distinct-types.md` that mapping is fixed. So no spelling of an Aether
extern produces a prototype matching libm. Nor can we drop the call: `as int`
TRUNCATES where `lrint` rounds-to-nearest (measured: `3.7 as int` = 3;
`lrint(3.7)` = 4), and the vg colour paths depend on rounding —
`lrint(v * 255.0)` per channel. `std.math` exposes `math_round(x: float) ->
float`, which rounds correctly but returns float, so it still needs a
truncating cast afterwards; that composition is fine numerically
(`math.round` then `as int`) and is our likely migration, but it is a
30-module change we would rather make once, in the direction you prefer.

**What would help, cheapest first:**
1. `std.math` gains an `lrint`-equivalent returning an integer type
   (`math_lrint(x: float) -> long`), so the prototype lives in your tree
   where it can be declared correctly and we drop 30 externs; or
2. a documented FFI spelling that emits C `long`; or
3. confirmation that `math.round(...) as int` is the blessed idiom, in which
   case we migrate and the warning disappears with the extern.

Not urgent — it is warning noise, not breakage — but it is 30 files of ours
and one function of yours, so it is much cheaper on your side.

### RESOLVED in aether v0.626.0 — `math.lrint(x) -> long` (and one correction)

Option 1 landed. Two things their changelog gets right that this note had
wrong:

- **It is an ERROR on Apple targets, not warning noise.** The redeclaration is
  invisible on Linux, where `int64_t` IS `long`, and a hard error on
  macOS/iOS, where `int64_t` is `long long`. So this was never cosmetic for
  the iOS/Catalyst work — it would have blocked it. The "not urgent" above was
  wrong for the platform that matters most here.
- **`math.round(...) as int` would have been a silent bug.** `math.lrint`
  rounds half-to-EVEN like libm; `math.round` is half-away-from-zero. An 8-bit
  channel computed as `lrint(v * 255.0)` is off by one at exact halves if it
  becomes `round`. Good that option 3 was not taken.

Our side of the migration: drop the 30 `extern lrint(x: float) -> long`
declarations under `vg/` and call `math.lrint` instead. Gated on bumping the
pin from v0.613.0 to v0.626.0.
