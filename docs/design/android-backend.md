# An Android backend

Status: **stage 1 (the skeleton) done** (2026-10-06); stage 2 next. The fifth
native implementation of the backend ABI (`backend/aether_ui_backend.h`),
beside GTK4, AppKit, Win32 and UIKit.

## Status

Stage 1 runs `examples/counter` on a Pixel 6a (Android 16, API 36) and
`tests/counter/spec_counter.ae` passes 4/4 against it through
`adb forward tcp:9222 tcp:9222` (as does `spec_route_parity`, 7/7). The whole
chain is real: `--emit=lib` for `aarch64-linux-android`, JNI, the ALooper
bridge, packaging without Gradle, and the shared driver.

- `backend/aether_ui_android.c` — 66 ABI functions real (registry,
  lifecycle/surfaces, vstack/hstack/text/button/spacer/divider, the state
  subsystem, the per-widget driver readbacks, the worker poster and timerfd
  timers), plus the driver hooks (enumeration, children, geometry, click,
  set_text, state, `GET /screenshot`). The other **261 are stubs** that log
  once through liblog and return a neutral value; the full list is the
  STATUS comment at the top of the file (and the STUBS section at its end).
- `backend/android/` — the Java shim (`AetherActivity`,
  `AetherClickListener`) and the manifest template. Natives are bound with
  `RegisterNatives`, so the shim's package is fixed (`dev.aether.ui`) and an
  activity-alias gives every app `<package>/.AetherActivity`. String extras
  named `AETHER_*` become environment variables before `main()` runs
  (`am start ... -e AETHER_UI_TEST_PORT 9222` arms the driver).
- `tools/android-apk.sh` builds the APK; `tools/android-install.sh` installs
  it in 256 KB pieces (for hosts whose adb dies on large transfers).
- Selection: `AETHER_UI_TARGET=android` in `build_support/aetherui` picks the
  sources (dormant, like the iOS arm); `ci.sh` Phase 1e3 compiles the backend
  `-Wall -Werror` for bionic and builds the counter's `libapp.so` when
  `AETHER_ANDROID_SYSROOT` is set; Phase 4a runs the counter spec on the
  desktop; `check_backend_parity.py` counts five backends.

Found on the way: `--emit=lib` drops a program's `main()`, so the packaging
script compiles a copy with `main()` renamed to `aeui_app_main()` (exported as
`aether_aeui_app_main`) — `asks/aether-emit-lib-keeps-main.md`. And an
app-as-library needs `--with=fs,net,os`, since `--emit=lib` is
capability-empty by default.

Run it by hand:

```sh
export AETHER_SYSROOT=<bases/aarch64-android29> ANDROID_HOME=<sdk> JAVA_HOME=<jdk>
AETHER_UI_WITH_DRIVER=1 tools/android-apk.sh examples/counter/counter.ae
tools/android-install.sh target/android/counter/counter.apk
adb forward tcp:9222 tcp:9222
adb shell am start -n dev.aether.ui.counter/.AetherActivity -e AETHER_UI_TEST_PORT 9222
UI_SPEC=counter/spec_counter tests/run_spec.sh
```

## The decision: native Views, not a drawn surface

Android games skip the platform toolkit: a `NativeActivity` or GameActivity
hands native code a surface, and the engine draws everything with Vulkan or
GLES. That route is simpler to start, and toolkit-envy F3.1 once leaned that
way. It is the wrong one for aether-ui:

- **Every other backend wraps real platform widgets.** Accessibility, input
  methods, text selection and the system look come from the platform, and
  `semantics-belong-above-the-abi.md` keeps it that way.
- **A drawn surface signs up for G1 and G9 on one platform only**: its own
  accessibility tree and its own IME protocol, the subsystems GPUI had to
  build (toolkit-envy Round 6).
- **The UIKit backend settled the mobile question.** It exists, has no stubs,
  and runs in the simulator; Android is its sibling, not a new architecture.

So: `android.widget` Views (`LinearLayout`, `TextView`, `Button`,
`EditText`, `ScrollView`, `CheckBox`/`Switch`, `SeekBar`, …), driven from C
through JNI. Apps that want a GPU surface keep `vg` and the GPU-surface work,
hosted in a `SurfaceView`.

## Shape

### Native: `backend/aether_ui_android.c`

The same ABI, implemented in C over JNI, with `aether_ui_uikit.m` as the
template: it is the most recent sibling port, already mobile-shaped (no tray,
pickers that are asynchronous, a single window per activity), and records
where a mobile platform differs from the desktop ABI. As there, the shared
logic stays above the ABI (`ui/module.ae`, `aether_ui_system_extras.c`, the
shared test server), so the backend translates and never interprets.

- **The widget registry** holds JNI global references to Views; a handle's
  slot self-NULLs when the View is destroyed, as on the other backends.
- **Layout:** the vstack/hstack/grid/scroll family maps to `LinearLayout`,
  `GridLayout` and `ScrollView`; spacing and padding in dp, converted with the
  display density.
- **Canvas and `vg`** render into an `android.graphics.Bitmap` through
  `libjnigraphics` (the CPU path the other backends share), shown in an
  `ImageView`; the GPU surface later goes in a `SurfaceView` through
  `libnativewindow` and Vulkan.

### Java: one small shim, about 200 lines

Views cannot be created without Java objects, so a pure-C `NativeActivity`
cannot host real widgets. The shim is the only non-C part:

- **`AetherActivity`** calls `System.loadLibrary("app")`, then hands native
  code the root layout and a `Context`, and forwards the lifecycle (pause,
  resume, configuration change, destroy).
- **Listener classes** (click, text change, check change, seek, scroll) carry
  the native closure's handle and call one native dispatch function.

It ships as source in `backend/android/` and is compiled when an APK is
packaged; no Gradle, no Kotlin.

### The main loop and threads

Android owns the main thread's loop, so `aether_ui_run` cannot block as it
does on the desktop: the app's `main()` runs from `onCreate`, builds its UI,
and returns into the platform's loop. Work arriving from actors,
`background()` and timers reaches the UI thread through a pipe registered
with the main thread's **`ALooper`** (`ALooper_addFd`), the NDK's counterpart
of GTK's idle sources; all of it in C.

### The app is a shared library

`ae build --target=aarch64-linux-android --emit=lib` with the Android sysroot
(aether-crossbuild's `fetch-android-sysroot.sh`, or the release's
`android-aarch64-sysroot` asset). Everything the backend links is in that
sysroot: `jni.h`, `libandroid` (looper, assets, native window),
`libjnigraphics`, `liblog`, `libnativewindow`, `libvulkan`, `libEGL`.
Android builds are already `-fPIC` (Aether #2436). `--emit=lib` for the
Android target is untested and is the first thing to prove.

### Packaging: an APK without Gradle

`javac` for the shim, `d8` to dex, `aapt2 link` against `android.jar` with a
generated `AndroidManifest.xml`, the `.so` under `lib/arm64-v8a/`, then
`zipalign` and `apksigner` (a debug key by default). It becomes the Android
case of the aeb `package` verb that the desktop platforms need too.

Tools: the Android SDK command-line build-tools and platform `android.jar`
(free downloads under Google's SDK licence: fetched, never committed) and a
JDK.

### Testing: the same driver, the same specs

The AetherUIDriver server is plain sockets, so it runs on Android unchanged.
`adb forward tcp:9222 tcp:9222` puts it where every existing spec expects
it, so the spec matrix that ratchets the other four backends ratchets this
one. Locally that is the Pixel 6a (Android 16); in CI, an x86_64 emulator,
which also needs an `x86_64-linux-android` sysroot.

## Stages

1. **Skeleton.** The shim, the ALooper bridge, the registry, and a window
   with vstack, hstack, text and button; `examples/counter` runs on the
   Pixel and `spec_counter` drives it through `adb forward`. Proves the whole
   chain: `--emit=lib`, JNI, the loop, packaging, the driver.
2. **Widget coverage, pass by pass,** with the spec matrix as the ratchet,
   until no ABI function is a stub (the backend-parity rule).
3. **The aeb `package` verb's Android case,** and an emulator leg in CI.

## Open questions

- **Minimum API level.** The sysroot defaults to 29 (Android 10). Raising it
  buys newer APIs; keeping it reaches more devices.
- **Back button and system bars.** Map Back to a window-close intercept
  (toolkit-envy F2.3) or to `browserContext.back` for sae?
- **Material look.** Plain `android.widget` looks dated next to Material
  Components, which are a library, not part of the platform. Start with the
  platform widgets; decide once the skeleton runs.
