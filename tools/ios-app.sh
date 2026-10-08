#!/usr/bin/env bash
# tools/ios-app.sh — build an aether-ui app for a REAL iPhone/iPad, signed, and
# (optionally) install + launch it. The device counterpart of ci.sh Phase 1e2
# (which runs listbox_demo in the simulator) and the iOS sibling of
# tools/android-apk.sh.
#
#   tools/ios-app.sh apps/rubiks_cube/rubiks_cube.ae
#       -> target/ios/rubiks_cube/build/Release-iphoneos/rubiks_cube.app (signed)
#   IOS_DEVICE=<udid> tools/ios-app.sh apps/rubiks_cube/rubiks_cube.ae
#       ... then installs it on that device and launches it.
#
# Steps:
#   1. The Aether runtime for arm64 iOS: `ae build --target=aarch64-ios
#      --emit=staticlib tests/ios/runtime_seed.ae` (the seed program exists only
#      to make ae compile the runtime; its main() is kept as aether_main and
#      never called). Cached in target/ios/libaether_ios.a.
#   2. The app's portable C (aetherc, exactly as build.sh / Phase 1e2 emit it;
#      sibling modules such as apps/rubiks_cube/cube_engine.ae resolve from the
#      source's directory), compiled for arm64-apple-ios with the UIKit backend,
#      the shared system extras and the driver (AETHER_UI_WITH_DRIVER=1) or the
#      no-control stub. The app's own main() stays main(): the backend's
#      ui.app_run calls UIApplicationMain, as in the simulator run.
#   3. A generated, throwaway Xcode project (target/ios/<app>/<App>.xcodeproj)
#      whose one target compiles a one-line stub and LINKS the objects from 2
#      and the runtime from 1. Its only job is what nothing but Xcode can do on
#      a free/personal team: create the provisioning profile (automatic
#      signing, -allowProvisioningUpdates, the device registered to the team)
#      and sign the bundle with it.
#      The bundle carries the app's files (everything beside its source that is
#      not .ae, plus the toolkit's fonts/, plus whatever .aeui-files names) under
#      files/ at checkout-relative paths; the UIKit backend chdir()s there at
#      start-up so vg's fonts/DejaVuSans-Bold.ttf and other relative paths
#      resolve as they do from a desktop checkout (cf. the Android APK assets).
#   4. With IOS_DEVICE set: `xcrun devicectl device install app` and
#      `... process launch` (AETHER_UI_TEST_PORT passed through when the driver
#      is linked).
#
# A free (personal) team's profile lasts 7 days: re-run to re-sign. The first
# install from a new team needs, ON THE PHONE: Settings > General > VPN &
# Device Management > (Developer App) Apple Development: <you> > Trust.
#
# Environment:
#   AE, AETHERC     the tools to build with (default: ae, aetherc on PATH)
#   IOS_TEAM        Apple team id (default: the OU of the one "Apple Development"
#                   identity in the keychain)
#   IOS_BUNDLE_ID   (default: dev.aether.ui.<team>.<app, alphanumerics only>)
#   IOS_MIN         deployment target (default 17.0)
#   IOS_DEVICE      UDID / name to install on and launch (optional)
#   IOS_NO_LAUNCH   non-empty: install only
#   AETHER_UI_WITH_DRIVER  non-empty: link the AetherUIDriver (listens on
#                   127.0.0.1 on the phone); default the no-control stub.
#   AETHER_UI_TEST_PORT    passed to the launched app when the driver is linked.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:?usage: tools/ios-app.sh <app.ae>}"
[ -f "$SRC" ] || { echo "error: no such source: $SRC" >&2; exit 1; }
SRC="$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")"
APP="$(basename "$SRC" .ae)"
APP_DIR="$(dirname "$SRC")"
AE="${AE:-ae}"
AETHERC="${AETHERC:-aetherc}"
IOS_MIN="${IOS_MIN:-17.0}"

if [ -z "${IOS_TEAM:-}" ]; then
    IOS_TEAM="$(security find-certificate -a -c "Apple Development" -p 2>/dev/null \
        | openssl x509 -noout -subject 2>/dev/null \
        | sed -nE 's/.*OU ?= ?([A-Z0-9]+).*/\1/p' | head -1)"
fi
[ -n "$IOS_TEAM" ] || { echo "error: no Apple Development identity found; set IOS_TEAM" >&2; exit 1; }
BUNDLE_ID="${IOS_BUNDLE_ID:-dev.aether.ui.$IOS_TEAM.$(echo "$APP" | tr -cd 'a-zA-Z0-9')}"

if [ -n "${AETHER_UI_WITH_DRIVER:-}" ]; then
    CONTROL="aether_ui_test_server.c"
else
    CONTROL="aether_ui_no_control.c"
fi

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CLANG="$(xcrun --sdk iphoneos -f clang)"
TGT="arm64-apple-ios$IOS_MIN"
OUT="$ROOT/target/ios/$APP"
rm -rf "$OUT"
mkdir -p "$OUT/obj" "$OUT/proj"

# --- 1. the runtime ---------------------------------------------------------
RUNTIME="$ROOT/target/ios/libaether_ios.a"
if [ ! -f "$RUNTIME" ] || [ "$(command -v "$AE")" -nt "$RUNTIME" ]; then
    echo "[1/4] runtime  ($AE build --target=aarch64-ios --emit=staticlib)"
    AETHER_IOS_MIN="$IOS_MIN" "$AE" build --target=aarch64-ios --emit=staticlib \
        "$ROOT/tests/ios/runtime_seed.ae" -o "$RUNTIME"
else
    echo "[1/4] runtime  (cached: $RUNTIME)"
fi

# --- 2. the app + backend objects -------------------------------------------
echo "[2/4] $APP for $TGT (UIKit backend, $CONTROL)"
( cd "$ROOT" && "$AETHERC" "$SRC" "$OUT/obj/$APP.c" > "$OUT/aetherc.log" 2>&1 ) \
    || { tail -20 "$OUT/aetherc.log" >&2; exit 1; }
INCS="$("$AE" cflags | tr ' ' '\n' | grep -E '^-I' | tr '\n' ' ')"
OBJS=()
for s in "$OUT/obj/$APP.c" \
         "$ROOT/backend/aether_ui_uikit.m" \
         "$ROOT/backend/$CONTROL" \
         "$ROOT/backend/aether_ui_system_extras.c"; do
    o="$OUT/obj/$(basename "${s%.*}").o"
    # shellcheck disable=SC2086
    "$CLANG" -c -fobjc-arc -O2 -g -target "$TGT" -isysroot "$SDK" $INCS -I"$ROOT/backend" \
        -Wno-incompatible-library-redeclaration "$s" -o "$o" 2> "$o.log" \
        || { grep -E "error" "$o.log" | head -20 >&2; exit 1; }
    OBJS+=("$o")
done

# --- 3. bundle: Xcode project for signing -----------------------------------
echo "[3/4] signed bundle  ($BUNDLE_ID, team $IOS_TEAM)"
P="$OUT/proj"
echo '/* The app (main included) is in the prebuilt objects tools/ios-app.sh links. */' \
    > "$P/aeui_ios_stub.c"
echo 'int aeui_ios_stub_unused;' >> "$P/aeui_ios_stub.c"

# The app's files, at checkout-relative paths, under files/ (see the header).
FILES="$P/files"
mkdir -p "$FILES"
REL_BASE="$ROOT"
case "$APP_DIR/" in "$ROOT"/*) ;; *) REL_BASE="$(dirname "$APP_DIR")" ;; esac
add_file() { local rel="$2"; mkdir -p "$FILES/$(dirname "$rel")"; cp "$1" "$FILES/$rel"; }
while IFS= read -r f; do add_file "$f" "${f#"$REL_BASE"/}"; done \
    < <(find "$APP_DIR" -type f ! -name '*.ae' ! -name '.*' ! -path '*/.*' -size -8M | sort)
add_tree() {
    local src="$ROOT/$1"
    [ -e "$src" ] || { echo "  warning: $1 (named for the app) does not exist" >&2; return 0; }
    while IFS= read -r f; do add_file "$f" "${f#"$ROOT"/}"; done \
        < <(find "$src" -type f ! -name '.*' -size -8M | sort)
}
[ -d "$ROOT/fonts" ] && add_tree fonts
if [ -f "$APP_DIR/.aeui-files" ]; then
    while IFS= read -r line; do
        line="${line%%#*}"; line="$(echo "$line" | tr -d '[:space:]')"
        [ -n "$line" ] && add_tree "$line"
    done < "$APP_DIR/.aeui-files"
fi

cat > "$P/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>\$(PRODUCT_BUNDLE_IDENTIFIER)</string>
    <key>CFBundleExecutable</key><string>\$(EXECUTABLE_NAME)</string>
    <key>CFBundleName</key><string>$APP</string>
    <key>CFBundleDisplayName</key><string>$APP</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSRequiresIPhoneOS</key><true/>
    <!-- Scene manifest: puts the backend on its UIWindowScene path
         (AetherSceneDelegate is supplied in code by the app delegate). -->
    <key>UIApplicationSceneManifest</key>
    <dict><key>UIApplicationSupportsMultipleScenes</key><false/></dict>
    <key>UILaunchScreen</key><dict/>
</dict>
</plist>
PLIST

LDFLAGS=""
for o in "${OBJS[@]}"; do LDFLAGS="$LDFLAGS \"$o\""; done
LDFLAGS="$LDFLAGS \"$RUNTIME\" -framework UIKit -framework Foundation -framework QuartzCore -framework CoreGraphics -framework CoreText -framework ImageIO -framework UserNotifications -lm"
LDFLAGS_ESC="$(printf '%s' "$LDFLAGS" | sed 's/\\/\\\\/g; s/"/\\"/g')"

XP="$P/$APP.xcodeproj"
mkdir -p "$XP/xcshareddata/xcschemes"
cat > "$XP/project.pbxproj" <<PBX
// !\$*UTF8*\$!
{
	archiveVersion = 1;
	classes = {};
	objectVersion = 56;
	objects = {
		A0000000000000000000000B /* stub.c in Sources */ = {isa = PBXBuildFile; fileRef = A0000000000000000000000F; };
		A0000000000000000000001B /* files in Resources */ = {isa = PBXBuildFile; fileRef = A0000000000000000000001F; };
		A0000000000000000000000F = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = aeui_ios_stub.c; sourceTree = "<group>"; };
		A0000000000000000000001F = {isa = PBXFileReference; lastKnownFileType = folder; path = files; sourceTree = "<group>"; };
		A0000000000000000000002F = {isa = PBXFileReference; lastKnownFileType = text.plist.xml; path = Info.plist; sourceTree = "<group>"; };
		A0000000000000000000003F = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = "$APP.app"; sourceTree = BUILT_PRODUCTS_DIR; };
		A0000000000000000000004A = {isa = PBXGroup; children = (A0000000000000000000000F, A0000000000000000000001F, A0000000000000000000002F, A0000000000000000000004B); sourceTree = "<group>"; };
		A0000000000000000000004B = {isa = PBXGroup; children = (A0000000000000000000003F); name = Products; sourceTree = "<group>"; };
		A0000000000000000000005A = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (A0000000000000000000000B); runOnlyForDeploymentPostprocessing = 0; };
		A0000000000000000000005B = {isa = PBXResourcesBuildPhase; buildActionMask = 2147483647; files = (A0000000000000000000001B); runOnlyForDeploymentPostprocessing = 0; };
		A0000000000000000000006A = {isa = PBXNativeTarget; buildConfigurationList = A0000000000000000000008B; buildPhases = (A0000000000000000000005A, A0000000000000000000005B); buildRules = (); dependencies = (); name = "$APP"; productName = "$APP"; productReference = A0000000000000000000003F; productType = "com.apple.product-type.application"; };
		A0000000000000000000007A = {isa = PBXProject; attributes = {LastUpgradeCheck = 1600; TargetAttributes = {A0000000000000000000006A = {DevelopmentTeam = $IOS_TEAM; ProvisioningStyle = Automatic; }; }; }; buildConfigurationList = A0000000000000000000008A; compatibilityVersion = "Xcode 14.0"; developmentRegion = en; hasScannedForEncodings = 0; knownRegions = (en, Base); mainGroup = A0000000000000000000004A; productRefGroup = A0000000000000000000004B; projectDirPath = ""; projectRoot = ""; targets = (A0000000000000000000006A); };
		A0000000000000000000009A = {isa = XCBuildConfiguration; buildSettings = {SDKROOT = iphoneos; IPHONEOS_DEPLOYMENT_TARGET = $IOS_MIN; ARCHS = arm64; ONLY_ACTIVE_ARCH = NO; }; name = Release; };
		A0000000000000000000009B = {isa = XCBuildConfiguration; buildSettings = {
			PRODUCT_NAME = "$APP";
			PRODUCT_BUNDLE_IDENTIFIER = "$BUNDLE_ID";
			INFOPLIST_FILE = Info.plist;
			GENERATE_INFOPLIST_FILE = NO;
			CODE_SIGN_STYLE = Automatic;
			CODE_SIGN_IDENTITY = "Apple Development";
			DEVELOPMENT_TEAM = $IOS_TEAM;
			TARGETED_DEVICE_FAMILY = "1,2";
			SDKROOT = iphoneos;
			SUPPORTED_PLATFORMS = iphoneos;
			IPHONEOS_DEPLOYMENT_TARGET = $IOS_MIN;
			ENABLE_BITCODE = NO;
			ENABLE_USER_SCRIPT_SANDBOXING = NO;
			OTHER_LDFLAGS = "$LDFLAGS_ESC";
		}; name = Release; };
		A0000000000000000000008A = {isa = XCConfigurationList; buildConfigurations = (A0000000000000000000009A); defaultConfigurationIsVisible = 0; defaultConfigurationName = Release; };
		A0000000000000000000008B = {isa = XCConfigurationList; buildConfigurations = (A0000000000000000000009B); defaultConfigurationIsVisible = 0; defaultConfigurationName = Release; };
	};
	rootObject = A0000000000000000000007A;
}
PBX

cat > "$XP/xcshareddata/xcschemes/$APP.xcscheme" <<SCHEME
<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "1600" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForRunning = "YES" buildForArchiving = "YES">
            <BuildableReference BuildableIdentifier = "primary"
               BlueprintIdentifier = "A0000000000000000000006A"
               BuildableName = "$APP.app" BlueprintName = "$APP"
               ReferencedContainer = "container:$APP.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <LaunchAction buildConfiguration = "Release" launchStyle = "0" useCustomWorkingDirectory = "NO">
   </LaunchAction>
</Scheme>
SCHEME

DEST="generic/platform=iOS"
[ -n "${IOS_DEVICE:-}" ] && DEST="id=$IOS_DEVICE"
if ! xcodebuild -project "$XP" -scheme "$APP" -configuration Release \
        -destination "$DEST" -derivedDataPath "$OUT/dd" \
        -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
        SYMROOT="$OUT/build" build > "$OUT/xcodebuild.log" 2>&1; then
    echo "error: xcodebuild failed (log: $OUT/xcodebuild.log):" >&2
    grep -E "error:|requires a provisioning|No Account|No profiles|sign in" "$OUT/xcodebuild.log" \
        | sort -u | head -20 >&2
    exit 1
fi
BUNDLE="$OUT/build/Release-iphoneos/$APP.app"
[ -d "$BUNDLE" ] || BUNDLE="$(find "$OUT" -name "$APP.app" -type d -path '*iphoneos*' | head -1)"
codesign --verify --strict "$BUNDLE"
echo "  signed: $BUNDLE"
codesign -dvv "$BUNDLE" 2>&1 | grep -E "^(Authority|TeamIdentifier)=" | head -2 | sed 's/^/    /'

# --- 4. install + launch ------------------------------------------------------
if [ -z "${IOS_DEVICE:-}" ]; then
    echo "[4/4] (no IOS_DEVICE: not installing)"
    exit 0
fi
echo "[4/4] install + launch on $IOS_DEVICE"
xcrun devicectl device install app --device "$IOS_DEVICE" "$BUNDLE"
[ -n "${IOS_NO_LAUNCH:-}" ] && exit 0
ENV_ARGS=()   # devicectl rejects an empty {} -- pass -e only with something in it
if [ -n "${AETHER_UI_WITH_DRIVER:-}" ]; then
    ENV_ARGS=(--environment-variables "{\"AETHER_UI_TEST_PORT\":\"${AETHER_UI_TEST_PORT:-9222}\"}")
fi
xcrun devicectl device process launch --device "$IOS_DEVICE" \
    --terminate-existing ${ENV_ARGS[@]+"${ENV_ARGS[@]}"} "$BUNDLE_ID"
