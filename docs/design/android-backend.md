# An Android backend

Status: **stage 2, pass B done** (2026-10-07); pass C (canvas, GPU view)
next. The fifth
native implementation of the backend ABI (`backend/aether_ui_backend.h`),
beside GTK4, AppKit, Win32 and UIKit.

## Status

Stage 1 ran `examples/counter` on a Pixel 6a (Android 16, API 36) and
`tests/counter/spec_counter.ae` passed 4/4 against it through
`adb forward tcp:9222 tcp:9222`. The whole chain is real: `--emit=lib` for
`aarch64-linux-android`, JNI, the ALooper bridge, packaging without Gradle,
and the shared driver.

Stage 2 pass A brought the widget set most apps are made of, and the spec
matrix's example suites that use only those now pass on the emulator (AVD
API 36 arm64, `aelane android`): counter, calculator, text_metrics, bindings,
rbind, placeholder, imagefill, scrollbg, each, clearchildren (rebuild_demo),
picker, typo, a11y, disclosure, hoverpaint, timer, background, auto_hide,
tabledeleg, routeparity and native_view. Every other example suite fails only
where it uses a pass-B/C stub (canvas, menus, overlays, sheets, tabs,
navstack, split, shortcuts and key handling, CSS classes, seal, fire_*,
multi-window, file pickers), which the backend's log names.

- `backend/aether_ui_android.c` — 168 ABI functions real: the stage-1 core
  plus textfield/securefield/textarea (`EditText`), toggle (`CheckBox`, a
  radio button in a group), slider (`SeekBar`), picker (`Spinner`),
  progress bar, scroll view, grid (`GridLayout`), form and section, image
  (`ImageView` over `BitmapFactory`, all four fill modes, tint), text
  wrap/anchor/truncation and metrics, every `set_*` styler and sizer, the
  a11y role/name/description, on_click/double/hover/layout, focus,
  enabled/hidden and the property bindings. The other **159 are stubs**
  that log once through liblog and return a neutral value; the full list is
  the STATUS comment at the top of the file (and the STUBS section at its
  end).
- **Layout** is real Android layouts (`LinearLayout`, `GridLayout`,
  `ScrollView`). A child's `LayoutParams` are derived in one place from what
  the DSL asked of it and of its parent (fixed size, expansion, spacing,
  alignment, distribution, grid cell), and re-derived whenever one of those
  changes, since the DSL assembles top-down. Expansion follows GTK4's
  propagation (a scroll area makes every container above it take the slack).
- **Styling** keeps every background input (colour, gradient, border,
  radius, hover and pressed colours, flatness) on the widget record and
  rebuilds the background `Drawable` from all of them; hover and pressed
  colours are a `StateListDrawable`, so the platform's own state machinery
  shows them.
- **Units are dp** everywhere the ABI has a length: sizes the app sets,
  geometry the driver reports, `get_width`, `on_layout`, font sizes and text
  metrics. That is what points are on AppKit and UIKit, so a spec's numbers
  mean the same on all of them.
- `backend/android/` — the Java shim (pass B added `AetherWrap`,
  `AetherListAdapter`, `AetherHost` and `AetherScroll`, the objects a flow
  layout, a `ListView` and the window's minimum size need,
  and the key, menu, picker and notification hooks on the activity):
  `AetherActivity`, `AetherListener`
  (every View event — click, text change, check change, seek, item
  selection, hover, layout change, double tap — carried to one native
  dispatch with the widget's handle; what the event means is decided in C)
  and `AetherA11y` (the accessibility delegate that reports a role as a
  class name and a description as hint text), and the manifest template.
  Natives are bound with `RegisterNatives`, so the shim's package is fixed
  (`dev.aether.ui`) and an activity-alias gives every app
  `<package>/.AetherActivity`. String extras named `AETHER_*` become
  environment variables before `main()` runs (`am start ... -e
  AETHER_UI_TEST_PORT 9222` arms the driver).
- `tools/android-apk.sh` builds the APK, now with **the app's files**: what
  sits beside the source and is not source goes in as assets at its
  checkout-relative path, the backend copies them out under the app's files
  directory and runs the app there, so a relative path a desktop app opens
  (`examples/imagefill_demo/swatch.png`) opens on the phone.
  `tools/android-install.sh` installs in 256 KB pieces (for hosts whose adb
  dies on large transfers).
- Selection: `AETHER_UI_TARGET=android` in `build_support/aetherui` picks the
  sources (dormant, like the iOS arm); `ci.sh` Phase 1e3 compiles the backend
  `-Wall -Werror` for bionic and builds the counter's `libapp.so` when
  `AETHER_ANDROID_SYSROOT` is set; Phase 4a runs the counter spec on the
  desktop; `check_backend_parity.py` counts five backends.

Stage 2 pass B brought everything around the widgets: 104 more ABI
functions real, plus the tray family as documented no-ops (no status-area
tray on Android, as on iOS). What is left is pass C's: the 40 canvas
functions, the 8 GPU-view functions and `fire_double_click`. The mappings:

- **Containers.** zstack is a `FrameLayout`; wrap is `AetherWrap`, a flow
  layout (the shim's one `ViewGroup`: `android.widget` has none); tabs are a
  strip of buttons over a frame of pages, the unselected ones `GONE` but
  registered; a navstack keeps every pushed page, hides the one below and
  shows it again on pop (a phone's back stack), retiring the popped page's
  widgets; a splitview is `[pane, divider, pane]` with a real drag handle.
- **Windows.** Window 1 is the activity. An extra window is an
  `android.app.Dialog` (a real platform window with a title and its own
  view tree; Back closes it), so `/windows`, per-widget `window` and the
  close path are what they are on the desktop. Sheets are dialogs too.
  `/window/resize` sizes the activity's content (wider than the screen if
  asked, as a freeform window is) or the dialog.
- **Overlays** are drawn in the window's host frame above the body; a
  modal's scrim is a registered widget; "blur" is a real `RenderEffect`
  blur of the body from API 31 (else "tint", and `material_effective` says
  so); exit transitions are `ViewPropertyAnimator` tweens. Toasts and drawn
  tooltips are overlay labels, so the driver finds them.
- **Menus.** Window 1's menu bar is the action bar's options menu (each
  menu a submenu); a dialog window's is a row of menu titles opening
  `PopupMenu`s; `menu_popup` and context menus (long press, or a secondary
  click) are `PopupMenu`s. `/menu/{h}/native_activate` performs the real
  `MenuItem` through the options menu.
- **Keys.** `AetherActivity.dispatchKeyEvent` hands every hardware-keyboard
  key to native code first: chords, then shortcuts (which consume the key),
  then the any-key handlers (which do not), Escape closing the top overlay.
  `Primary` is Ctrl, as on GTK4 and Win32.
- **Pickers** are the Storage Access Framework's, run as a modal (a nested
  `Looper.loop()` until `onActivityResult`), answering a path C can open
  (`/proc/self/fd/N`; a folder is its tree's `content://` URI). Headless,
  the shared `/prompts` queue answers, as everywhere.
- **System.** Alerts are `AlertDialog`s; notifications post on the app's
  own channel, with `POST_NOTIFICATIONS` requested at API 33+ and a tap
  coming back through `onNewIntent`; clipboard, `ACTION_VIEW` URLs and the
  configuration's night bit (watched through configuration changes) are
  the platform's.
- **Window minimum size.** `LinearLayout` squeezes children that overflow a
  full stack to height 0 (and a zero-sized View cannot take focus); a
  desktop window instead grows to what its content needs. `AetherHost`, the
  frame every window's body mounts in, measures the body's natural height
  (`AetherScroll` scroll areas reporting their minimum, as a
  `GtkScrolledWindow` does) and lays it out at that or the window's height,
  whichever is more: overflow is clipped, not collapsed.
- **Lists and views.** A native list is a `ListView` over
  `AetherListAdapter`: rows are built on demand and retired when scrapped.
  A native view is a `SurfaceView` whose handle is its `ANativeWindow*`
  (native-view kind 5).

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
