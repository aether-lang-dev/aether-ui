#!/bin/sh
# real_input_x11.sh BIN PORT: datagrid_demo driven by REAL X11 input (xdotool,
# XTEST), as a person's mouse and keyboard reach GTK: a click on a fresh
# grid's R5C4, then "7" typed and Return. The driver reads back what is on
# the screen at each step (the cell's place, the scrollbars, the focus, the
# editor) and /screenshot is saved as target/datagrid-snapshots/x11-*.png.
# Run under an X server (ci.sh wraps it in xvfb-run). Exits 0 on a pass, 1 on
# a fail, 77 when xdotool is missing (skipped).
BIN=$1; PORT=${2:-9388}; U=http://127.0.0.1:$PORT
command -v xdotool > /dev/null 2>&1 || { echo "  SKIP no xdotool"; exit 77; }
DIR=$(cd "$(dirname "$0")" && pwd)/target/datagrid-snapshots
mkdir -p "$DIR"
AETHER_UI_TEST_PORT=$PORT "$BIN" > /tmp/ci_real_input.app.log 2>&1 &
AP=$!
for i in $(seq 1 80); do curl -sf -o /dev/null $U/widgets && break; sleep 0.25; done
sleep 1
WID=$(xdotool search --pid $AP 2>/dev/null | tail -1)
fail=0
say() { echo "  $1"; }
# The cell's "x y w h", the scrollbars' values, the focused handle, the editor's text.
look() {
    curl -s $U/widgets | python3 -c '
import json,sys
W=json.load(sys.stdin)
c=[w for w in W if w.get("text")=="R5C4" or (w.get("classes") or "").find("aui-grid-cell")>=0 and w.get("a11y_row")==6 and w.get("a11y_col")==4]
cell="%d %d %d %d"%(c[0]["x"],c[0]["y"],c[0]["w"],c[0]["h"]) if c else "none"
sl=",".join(str(w.get("value")) for w in W if w["type"]=="scrollbar")
ed=[w["text"] for w in W if w["type"]=="textfield" and not w.get("placeholder")]
print(cell+"|"+sl+"|"+(ed[0] if ed else "-"))'
}
shot() { curl -s -o "$DIR/x11-$1.png" $U/screenshot; }
B=$(look); shot 1-before-click
set -- $(echo "$B" | cut -d'|' -f1)
xdotool windowfocus --sync "$WID" 2>/dev/null
xdotool mousemove --window "$WID" $(( $1 + $3 / 2 )) $(( $2 + $4 / 2 )) click 1
sleep 0.6
A=$(look); shot 2-after-click
[ "$(echo "$A" | cut -d'|' -f1-2)" = "$(echo "$B" | cut -d'|' -f1-2)" ] || { say "FAIL the click moved the grid: $B -> $A"; fail=1; }
F=$(curl -s $U/focused)
xdotool type --delay 40 7
sleep 0.6
T=$(look); shot 3-after-typing
[ "$(echo "$T" | cut -d'|' -f3)" = "7" ] || { say "FAIL typing 7 opened no editor holding it ($T; focus after the click $F)"; fail=1; }
[ "$(echo "$T" | cut -d'|' -f1-2)" = "$(echo "$B" | cut -d'|' -f1-2)" ] || { say "FAIL typing moved the grid: $B -> $T"; fail=1; }
xdotool key Return
sleep 0.6
shot 4-after-return
curl -s $U/widgets | grep -q '"edited: 5,4 = 7"' || { say "FAIL Return stored nothing"; fail=1; }
kill $AP 2>/dev/null
[ $fail -eq 0 ] && say "OK   real click and keys: the grid stayed, an editor took 7, Return stored it"
exit $fail
