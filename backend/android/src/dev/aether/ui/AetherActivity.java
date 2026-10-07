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
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.ViewGroup;

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
    // A key from a hardware (or emulator) keyboard, before the focused View
    // sees it: true = a shortcut consumed it.
    static native boolean nativeKey(int keyCode, int meta, int unicode, int repeat);
    // The action bar's options menu is the window's menu bar: native code
    // fills `menu` from the menu bar the app attached; true = there is one.
    static native boolean nativeOptionsMenu(Menu menu);
    // The app was brought back by a tap on one of its notifications.
    static native void nativeNotificationTap(int id);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        exportEnvironment(getIntent());
        // fitsSystemWindows: targetSdk 35+ draws edge to edge, so without it
        // the body would sit under the status and navigation bars.
        AetherHost host = new AetherHost(this);
        host.setFitsSystemWindows(true);
        setContentView(host);
        nativeStart(this, host, getResources().getDisplayMetrics().density);
        deliverNotificationTap(getIntent());
    }

    // ---- Keyboard ------------------------------------------------------------
    // Android has no window-level accelerator table; a hardware keyboard's
    // keys arrive here first, on their way to the focused View. Native code
    // matches shortcuts and chords and runs the any-key handlers; a key a
    // shortcut consumed goes no further (an accelerator must not also type
    // into the focused field), any other key continues as usual.
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getAction() == KeyEvent.ACTION_DOWN
                && nativeKey(e.getKeyCode(), e.getMetaState(),
                             e.getUnicodeChar(e.getMetaState() & ~(KeyEvent.META_CTRL_MASK
                                 | KeyEvent.META_ALT_MASK | KeyEvent.META_META_MASK)),
                             e.getRepeatCount()))
            return true;
        return super.dispatchKeyEvent(e);
    }

    // ---- The menu bar: the action bar's options menu ----------------------------
    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        return nativeOptionsMenu(menu);
    }

    @Override
    public boolean onPrepareOptionsMenu(Menu menu) {
        menu.clear();
        return nativeOptionsMenu(menu);
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        nativeEvent(0, AetherListener.MENU, item.getItemId(), -1, null);
        return true;
    }

    // ---- Notifications ---------------------------------------------------------
    // launchMode singleTop: a tap on a notification while the app runs
    // arrives here rather than as a second activity.
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        deliverNotificationTap(intent);
    }

    private static void deliverNotificationTap(Intent intent) {
        if (intent == null) return;
        int id = intent.getIntExtra("aeui_notification", 0);
        if (id > 0) {
            intent.removeExtra("aeui_notification");
            nativeNotificationTap(id);
        }
    }

    // ---- Modal pickers -----------------------------------------------------------
    // The ABI's file dialogs are synchronous, as NSOpenPanel.runModal and
    // GtkFileChooser's run are: the call returns the path. Android's pickers
    // are the Storage Access Framework's activities, whose answer arrives
    // later in onActivityResult. A desktop modal is a nested event loop, and
    // so is this: the picker is started and the UI thread's Looper is run
    // again, re-entrantly, until the result has arrived (it is delivered by
    // that very loop), when a posted ModalDone unwinds it. Everything else
    // -- the driver, timers, the app's other events -- keeps running while
    // the picker is up, exactly as under a desktop modal.
    private static final int REQ_PICK = 0xAE01;
    private static final class ModalDone extends RuntimeException {
        ModalDone() { super(null, null, false, false); }
    }
    private Intent pickResult;
    private boolean pickDone;

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQ_PICK) return;
        pickResult = result == RESULT_OK ? data : null;
        pickDone = true;
        new Handler(Looper.getMainLooper()).post(() -> { throw new ModalDone(); });
    }

    // kind: 0 open a file, 1 save a file, 2 choose a folder. The answer is
    // a path the C side can open(): a document is opened through the
    // ContentResolver and handed over as /proc/self/fd/N (the descriptor
    // stays open for the life of the process, so the path stays good); a
    // folder is a document TREE, which has no descriptor, so its content://
    // URI is the answer (with the grant made persistent). "" = cancelled.
    String pick(int kind, String title, String name) {
        Intent i;
        if (kind == 2) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else {
            i = new Intent(kind == 1 ? Intent.ACTION_CREATE_DOCUMENT : Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType(kind == 1 ? guessType(name) : "*/*");
            if (kind == 1 && name != null && !name.isEmpty()) i.putExtra(Intent.EXTRA_TITLE, name);
        }
        pickResult = null;
        pickDone = false;
        try {
            startActivityForResult(i, REQ_PICK);
        } catch (RuntimeException e) {
            android.util.Log.w("aether-ui", "no document picker", e);
            return "";
        }
        try {
            while (!pickDone) Looper.loop();
        } catch (ModalDone done) {
            // the result is in
        }
        Intent data = pickResult;
        pickResult = null;
        Uri uri = data == null ? null : data.getData();
        if (uri == null) return "";
        try {
            if (kind == 2) {
                getContentResolver().takePersistableUriPermission(uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                return uri.toString();
            }
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, kind == 1 ? "rwt" : "r");
            if (pfd == null) return "";
            return "/proc/self/fd/" + pfd.detachFd();
        } catch (Exception e) {
            android.util.Log.w("aether-ui", "picked document " + uri + " cannot be opened", e);
            return "";
        }
    }

    private static String guessType(String name) {
        String ext = name == null ? "" : name.substring(name.lastIndexOf('.') + 1).toLowerCase();
        String t = android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        return t != null ? t : "application/octet-stream";
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
