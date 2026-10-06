#!/usr/bin/env bash
# tools/android-apk.sh — package an aether-ui app as an Android APK, no Gradle.
#
#   tools/android-apk.sh examples/counter/counter.ae            # -> target/android/counter/counter.apk
#   AETHER_UI_WITH_DRIVER=1 tools/android-apk.sh examples/counter/counter.ae
#
# The Android backend's packaging (docs/design/android-backend.md, "Packaging:
# an APK without Gradle"); the Android case of the aeb `package` verb, until
# that verb exists. Steps:
#
#   1. The app as a shared library: `ae build --target=aarch64-linux-android
#      --emit=lib`, with the Android backend, the shared system extras and the
#      driver (or the no-control stub) as --extra sources, and liblog /
#      libandroid from the sysroot. Two things --emit=lib needs that an
#      executable build does not:
#        * main() is RENAMED to aeui_app_main() in a generated copy of the
#          source. --emit=lib drops a program's main() (aether's codegen:
#          "a future Shape B extension could emit an aether_main() wrapper");
#          a renamed top-level function is exported as aether_aeui_app_main,
#          which the backend calls from AetherActivity.onCreate. See
#          asks/aether-emit-lib-keeps-main.md.
#        * --with=fs,net,os: an --emit=lib library is capability-empty by
#          default, and ui/module.ae imports std.fs and std.os. An app IS
#          the host, so it grants them.
#   2. The Java shim (backend/android/src): javac --release 17 against the
#      platform android.jar, then d8 to classes.dex.
#   3. aapt2 link of the manifest template (backend/android/AndroidManifest.xml)
#      against android.jar -> the base APK.
#   4. classes.dex and lib/arm64-v8a/libapp.so added, zipalign, apksigner with a
#      debug key generated once under target/android/.
#
# Environment:
#   AETHER_SYSROOT   Android sysroot for zig (aether-crossbuild's
#                    fetch-android-sysroot.sh; bases/aarch64-android29). Required.
#   ANDROID_HOME     Android SDK (build-tools + platforms). Required.
#   JAVA_HOME        A JDK (javac, keytool). macOS's /usr/bin/javac is a stub.
#   AE               The ae to build with (default: ae on PATH).
#   ANDROID_BUILD_TOOLS  build-tools version (default: newest installed).
#   ANDROID_PLATFORM     platform to compile against (default: newest, e.g. android-36).
#   ANDROID_MIN_SDK  (29)   ANDROID_TARGET_SDK (platform's API level)
#   ANDROID_PACKAGE  application id (default: dev.aether.ui.<app>)
#   ANDROID_LABEL    launcher label (default: <app>)
#   ANDROID_LIB_ONLY non-empty: stop after libapp.so (no SDK or JDK needed;
#                    ci.sh's build check)
#   AETHER_UI_WITH_DRIVER  non-empty: link the AetherUIDriver and request
#                    INTERNET (the driver listens on 127.0.0.1); else the
#                    no-control stub, and no permission.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:?usage: tools/android-apk.sh <app.ae>}"
[ -f "$SRC" ] || { echo "error: no such source: $SRC" >&2; exit 1; }
SRC="$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")"
APP="$(basename "$SRC" .ae)"
APP_DIR="$(dirname "$SRC")"

: "${AETHER_SYSROOT:?set AETHER_SYSROOT to the Android sysroot (bases/aarch64-android29)}"
AE="${AE:-ae}"
if [ -n "${JAVA_HOME:-}" ]; then export PATH="$JAVA_HOME/bin:$PATH"; fi
MIN_SDK="${ANDROID_MIN_SDK:-29}"
PLATFORM="android-36"
if [ -z "${ANDROID_LIB_ONLY:-}" ]; then
    : "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
    newest() { ls -1 "$1" 2>/dev/null | sort -V | tail -1; }
    BT_VER="${ANDROID_BUILD_TOOLS:-$(newest "$ANDROID_HOME/build-tools")}"
    PLATFORM="${ANDROID_PLATFORM:-$(newest "$ANDROID_HOME/platforms")}"
    BT="$ANDROID_HOME/build-tools/$BT_VER"
    ANDROID_JAR="$ANDROID_HOME/platforms/$PLATFORM/android.jar"
    [ -x "$BT/aapt2" ] || { echo "error: no aapt2 under $BT" >&2; exit 1; }
    [ -f "$ANDROID_JAR" ] || { echo "error: no $ANDROID_JAR" >&2; exit 1; }
fi
TARGET_SDK="${ANDROID_TARGET_SDK:-${PLATFORM#android-}}"
PACKAGE="${ANDROID_PACKAGE:-dev.aether.ui.$(echo "$APP" | tr -c 'a-zA-Z0-9_\n' '_')}"
LABEL="${ANDROID_LABEL:-$APP}"

SYSLIB="$AETHER_SYSROOT/usr/lib/aarch64-linux-android/$MIN_SDK"
[ -f "$SYSLIB/liblog.so" ] || SYSLIB="$(ls -d "$AETHER_SYSROOT"/usr/lib/aarch64-linux-android/[0-9]* | sort -V | head -1)"

if [ -n "${AETHER_UI_WITH_DRIVER:-}" ]; then
    CONTROL="aether_ui_test_server.c"
    INTERNET='<uses-permission android:name="android.permission.INTERNET" />'
else
    CONTROL="aether_ui_no_control.c"
    INTERNET=''
fi

OUT="$ROOT/target/android/$APP"
STAGE="$OUT/stage"
rm -rf "$OUT"
mkdir -p "$STAGE/lib/arm64-v8a" "$OUT/gen" "$OUT/classes"

# --- 1. the app as libapp.so ------------------------------------------------
GEN="$OUT/gen/$APP.ae"
sed -E 's/^main\(\)/aeui_app_main()/' "$SRC" > "$GEN"
if [ "$(grep -c '^aeui_app_main()' "$GEN")" -ne 1 ]; then
    echo "error: $SRC needs exactly one top-level 'main()' to rename" >&2
    exit 1
fi
echo "[1/4] libapp.so  ($AE build --target=aarch64-linux-android --emit=lib)"
AETHER_SYSROOT="$AETHER_SYSROOT" AETHER_ANDROID_API="$MIN_SDK" \
"$AE" build "$GEN" --target=aarch64-linux-android --emit=lib --with=fs,net,os \
    --lib "$ROOT" --lib "$APP_DIR" \
    --extra "$ROOT/backend/aether_ui_android.c" \
    --extra "$ROOT/backend/$CONTROL" \
    --extra "$ROOT/backend/aether_ui_system_extras.c" \
    --extra "$SYSLIB/liblog.so" \
    --extra "$SYSLIB/libandroid.so" \
    -o "$STAGE/lib/arm64-v8a/libapp.so"

if [ -n "${ANDROID_LIB_ONLY:-}" ]; then
    echo "Built: $STAGE/lib/arm64-v8a/libapp.so (ANDROID_LIB_ONLY: no APK)"
    exit 0
fi

# --- 2. the shim -------------------------------------------------------------
echo "[2/4] classes.dex  (javac --release 17, d8 --min-api $MIN_SDK)"
JAVA_SRCS=()
while IFS= read -r f; do JAVA_SRCS+=("$f"); done \
    < <(find "$ROOT/backend/android/src" -name '*.java' | sort)
javac --release 17 -nowarn -classpath "$ANDROID_JAR" -d "$OUT/classes" "${JAVA_SRCS[@]}"
CLASS_FILES=()
while IFS= read -r f; do CLASS_FILES+=("$f"); done < <(find "$OUT/classes" -name '*.class' | sort)
"$BT/d8" --release --min-api "$MIN_SDK" --lib "$ANDROID_JAR" --output "$STAGE" "${CLASS_FILES[@]}"

# --- 3. manifest -> base APK -------------------------------------------------
echo "[3/4] manifest  (aapt2 link, package $PACKAGE, min $MIN_SDK, target $TARGET_SDK)"
MANIFEST="$OUT/gen/AndroidManifest.xml"
sed -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@LABEL@|$LABEL|g" -e "s|@VERSION@|1.0|g" \
    -e "s|@INTERNET_PERMISSION@|$INTERNET|g" \
    "$ROOT/backend/android/AndroidManifest.xml" > "$MANIFEST"
"$BT/aapt2" link -o "$OUT/base.apk" --manifest "$MANIFEST" -I "$ANDROID_JAR" \
    --min-sdk-version "$MIN_SDK" --target-sdk-version "$TARGET_SDK" --debug-mode

# --- 4. dex + .so in, align, sign --------------------------------------------
echo "[4/4] align + sign"
cp "$OUT/base.apk" "$OUT/unaligned.apk"
( cd "$STAGE" && zip -q -r "$OUT/unaligned.apk" classes.dex lib )
"$BT/zipalign" -f -p 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
KEYSTORE="$ROOT/target/android/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    # The conventional Android debug key: generated locally, never committed.
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" > /dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias androiddebugkey --out "$OUT/$APP.apk" "$OUT/aligned.apk"
"$BT/apksigner" verify "$OUT/$APP.apk"
rm -f "$OUT/base.apk" "$OUT/unaligned.apk" "$OUT/aligned.apk" "$OUT/$APP.apk.idsig"

echo "Built: $OUT/$APP.apk  ($(wc -c < "$OUT/$APP.apk" | tr -d ' ') bytes, package $PACKAGE)"
echo "  install: tools/android-install.sh $OUT/$APP.apk"
echo "  launch:  adb shell am start -n $PACKAGE/.AetherActivity${AETHER_UI_WITH_DRIVER:+ -e AETHER_UI_TEST_PORT 9222}"
