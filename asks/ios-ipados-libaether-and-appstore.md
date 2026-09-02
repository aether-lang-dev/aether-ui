<!-- STATUS 2026-09-01: NEW. All items OPEN. Raised by the aether-ui iOS/iPadOS
     backend work (backend/aether_ui_uikit.m, ci.sh Phase 1e). These are asks on
     the aether TOOLCHAIN/RUNTIME side — the UI backend itself is done and green.
     Do not delete until §1 (libaether iOS builds) and §2 (PCRE2/JIT) close. -->

# iOS / iPadOS: official libaether cross-builds + App Store compliance (esp. PCRE2 / JIT)

**From:** the aether-ui iOS/iPadOS backend line (2026-09-01). The UIKit backend
(`backend/aether_ui_uikit.m`, 202/287 ABI functions real) compiles, links, and
**renders real vg apps** (rubiks_cube, analog_clock) end-to-end. To get there I
had to hand-build a libaether for Mac Catalyst FROM SOURCE
(`/usr/local/share/aether/{runtime,std}/**/*.c`), and hit two real toolchain
gaps that are the aether side's to own. §2 is an **App Store blocker**.

## 1. Official libaether cross-builds for the Apple targets

There is no libaether for any iOS triple — only the host `x86_64-apple-macos`
`libaether.a`, which cannot link into an iOS/simulator/Catalyst binary
(platform mismatch). The UI backend needs libaether built for:

- `arm64-apple-ios`               (device)
- `arm64-apple-ios-simulator`     (Simulator; + `x86_64-…-simulator` for Intel hosts)
- `<arch>-apple-ios<ver>-macabi`  (Mac Catalyst — how we run/spec it on the Mac today)

**Recipe that worked** (Catalyst, 127/131 files): compile every
`runtime/**/*.c` + `std/**/*.c` with `-O2 -target <triple> -isysroot <sdk>
-I<include roots>`, then `libtool -static`. Excludes that are correct to drop:
`std/audio/aether_audio.c` (it's Objective-C — build as `.m`, or omit where no
audio), `std/regex/pcre2/pcre2_jit_match.c` + `pcre2_jit_misc.c` (JIT internals,
`#include`d by `pcre2_jit_compile.c` — do not compile standalone), and
`runtime/libaether_sandbox_preload.c` (a DYLD preload `.dylib` shim, N/A to a
static iOS link).

**Ask:** ship these as first-class `ae`/`aeb` cross-targets (or a documented
`make libaether TARGET=ios-…`), so apps don't hand-roll the archive.

## 2. PCRE2 build defines + JIT — a correctness bug AND an App Store blocker

Two things, both about `std/regex/aether_regex.c`'s PCRE2 guards:

**(a) The defines are load-bearing and easy to miss.** `aether_regex.c` compiles
to a **stub that always errors** (`"regex: built without libpcre2-8"`) unless
built with `-DAETHER_HAS_PCRE2 -DAETHER_VENDOR_PCRE2` (and
`-I…/std/regex` so `#include "pcre2/pcre2.h"` resolves). The `pcre2/*.c` objects
themselves build fine without any define, so the archive *links* — but every
`regex.compile` returns the stub error. This silently broke ALL regex-based code
downstream: `vg/svg/normalizer.parse_path` is regex-based, so **no `<path>`
rendered** — rubiks_cube drew only its background until I found this. Whatever
builds libaether for a new target MUST set these defines; a build that omits
them should ideally FAIL loudly, not stub.

**(b) PCRE2 JIT must be OFF on iOS — App Store 2.5.2 / no-W^X.** PCRE2 JIT
(`SUPPORT_JIT`, via `pcre2_jit_compile.c`) generates executable code at runtime
(`mmap` `PROT_EXEC`). iOS forbids writable-executable memory for non-WebKit apps
— an app that JIT-compiles a regex will be **rejected or crash on device**. The
iOS libaether must be built with **`SUPPORT_JIT` disabled** (interpreted PCRE2 is
correct and sufficient — it's what renders the cube today). Please make JIT a
per-target switch, default-off for all Apple mobile triples.

## 3. Other App Store / runtime asks (verify on the aether side)

- **No executable memory anywhere in the runtime.** Beyond PCRE2 JIT, confirm the
  scheduler/actors/codegen never `mmap` `PROT_WRITE|PROT_EXEC` (no trampolines).
  A single W^X allocation fails App Review's static checks.
- **Required-Reason API audit + privacy manifest.** `std.fs`/`std.os` (and any
  `UserDefaults`-like store) likely call Apple "required-reason" APIs
  (file-timestamp, disk-space, system-boot-time, `NSUserDefaults`). These need a
  declared reason in `PrivacyInfo.xcprivacy` or the app is rejected. Please
  publish which required-reason APIs libaether touches, per subsystem.
- **A release switch to compile OUT networking / the driver.** The AetherUIDriver
  test server (`backend/aether_ui_test_server.c`) is a localhost HTTP control
  server — it must be absent from App Store builds (security + completeness). The
  UI side can `#ifdef` it, but a runtime-level "no listening sockets in release"
  posture helps.
- **Export compliance:** libaether bundles AES/crypto — apps must answer
  `ITSAppUsesNonExemptEncryption`. A one-line statement of what crypto ships (and
  whether it's HTTPS-exempt) would let apps fill the App Store form correctly.

## 3b. ABI shapes that don't fit iOS (UI-side, but cross-backend)

The UIKit backend implements all 287 UI ABI functions, but three ABI *shapes*
assume desktop semantics iOS can't provide synchronously:

- **File pickers are synchronous** (`char* aether_ui_file_open/save/pick_folder`).
  iOS has only the ASYNC `UIDocumentPickerViewController` (delegate callback) —
  there is no `runModal`. The backend returns an empty selection today. The ABI
  needs an async/callback variant for iOS (and it's the better shape on every
  platform).
- **Hardware-keyboard shortcuts** register into a combo→closure registry and are
  driver-drivable, but live delivery on iPad needs `UIKeyCommand` on the
  responder chain — a different wiring than the desktop key hook.
- **Menu bar** (`menu_bar_*`) has no live iOS analogue (an iPad menu bar is built
  once at launch via `UIMenuBuilder`), and the **system tray** family is simply
  N/A on iOS.

These are notes for whoever unifies the ABI, not blockers — the backend degrades
honestly (documented no-ops / empty returns) in each case.

## 4. Minor / FYI

- Generated app C declares `int64_t lrint(double);` while the SDK has
  `long int lrint(double)`. Benign on LP64/LLP64-where-long-is-64 (macabi/ios),
  but it emits a `-W'…'` conflicting-types note on every app build. Consider
  matching the platform prototype (or `#include <math.h>`).

## What the aether-ui side already did (no action needed)
- 4th backend `backend/aether_ui_uikit.m` (UIKit), gated by `ci.sh` Phase 1e
  (compile + link + **render**, pixel-checked via Mac Catalyst).
- `ui_backend` in `build_support/aetherui/module.ae` has an `AETHER_UI_TARGET=ios`
  arm (frameworks: UIKit/Foundation/QuartzCore/CoreGraphics/CoreText/ImageIO).
- Full end-to-end proof: rubiks_cube's own `.ae` source → C → linked with a
  from-source Catalyst libaether + this backend → renders the 3D cube.
