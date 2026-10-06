// AetherActivity — the Java half of aether-ui's Android backend.
//
// Views cannot be created without Java objects, so a pure-C NativeActivity
// cannot host real android.widget Views. This shim is the only non-C part of
// the backend (docs/design/android-backend.md): it loads the app's shared
// library, hands native code the activity (the Context every View needs) and
// a host layout to mount the window body in, and forwards the lifecycle.
// Everything else -- building Views, layout, the widget registry, the
// AetherUIDriver -- is C in backend/aether_ui_android.c, over JNI.
//
// The natives are bound with RegisterNatives in JNI_OnLoad rather than by
// Java_<package>_... symbol names, so the shim's package is fixed
// (dev.aether.ui) while each app keeps its own application id.
package dev.aether.ui;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.view.ViewGroup;
import android.widget.FrameLayout;

public class AetherActivity extends Activity {
    // Lifecycle events forwarded to native code (aeui_android_lifecycle).
    static final int EVENT_PAUSE = 1;
    static final int EVENT_RESUME = 2;
    static final int EVENT_CONFIGURATION = 3;
    static final int EVENT_DESTROY = 4;

    static {
        // The app: its Aether code, libaether, the backend and the driver,
        // linked by `ae build --emit=lib` into lib/<abi>/libapp.so.
        System.loadLibrary("app");
    }

    // Runs the app's Aether main() on this (the UI) thread. main builds the
    // widget tree and returns: Android owns this thread's loop, so the app
    // keeps running in it rather than in a loop of its own.
    static native void nativeStart(Activity activity, ViewGroup host, float density);
    static native void nativeLifecycle(int event, boolean finishing);
    // A View's listener firing (AetherListener): the widget's registry
    // handle, the kind of event, and its payload.
    static native void nativeEvent(int handle, int kind, int a, int b, String s);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        exportEnvironment(getIntent());
        // fitsSystemWindows: targetSdk 35+ draws edge to edge, so without it
        // the body would sit under the status and navigation bars.
        FrameLayout host = new FrameLayout(this);
        host.setFitsSystemWindows(true);
        setContentView(host);
        nativeStart(this, host, getResources().getDisplayMetrics().density);
    }

    // Desktop apps read their configuration from the environment
    // (AETHER_UI_TEST_PORT arms the driver, AETHER_UI_HEADLESS, ...). An
    // Android app has no environment of its own, so string extras named
    // AETHER_* become one before main() runs:
    //   adb shell am start -n <pkg>/.AetherActivity -e AETHER_UI_TEST_PORT 9222
    @SuppressWarnings("deprecation")
    private static void exportEnvironment(Intent intent) {
        Bundle extras = intent == null ? null : intent.getExtras();
        if (extras == null) return;
        for (String key : extras.keySet()) {
            if (!key.startsWith("AETHER_")) continue;
            Object value = extras.get(key);
            if (value == null) continue;
            try {
                Os.setenv(key, String.valueOf(value), true);
            } catch (ErrnoException e) {
                android.util.Log.w("aether-ui", "setenv " + key + " failed", e);
            }
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        nativeLifecycle(EVENT_PAUSE, false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        nativeLifecycle(EVENT_RESUME, false);
    }

    // The manifest claims every configuration change, so a rotation reaches
    // here instead of destroying and recreating the activity (and with it
    // the whole widget tree main() built).
    @Override
    public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        nativeLifecycle(EVENT_CONFIGURATION, false);
    }

    @Override
    protected void onDestroy() {
        boolean finishing = isFinishing();
        super.onDestroy();
        nativeLifecycle(EVENT_DESTROY, finishing);
    }
}
