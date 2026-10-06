// AetherClickListener — carries a boxed Aether closure for a View's click.
//
// Native code creates one per closure-bearing View (a button, an on_click
// target) with the closure's address, and sets it as the View's
// OnClickListener. A click -- a real tap or the driver's performClick() --
// calls back into native code, which invokes the closure. The listener never
// interprets anything; it only carries the pointer across.
package dev.aether.ui;

import android.view.View;

final class AetherClickListener implements View.OnClickListener {
    private final long closure;

    AetherClickListener(long closure) {
        this.closure = closure;
    }

    @Override
    public void onClick(View view) {
        AetherActivity.nativeClick(closure);
    }
}
