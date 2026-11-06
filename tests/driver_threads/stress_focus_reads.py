#!/usr/bin/env python3
"""The driver's reads race the UI thread unless they run on it.

sae-driver died in objc_release on 2026-11-05 (spraedsheet's spec_sheet, its
Tab rounds): AppKit's hooks read -[NSTextField stringValue] on the driver's
HTTP thread while the main thread put a field editor away, and the HTTP
thread's autorelease pool released an object the main thread had already
freed. A sequential client almost never sees it -- the read has to land
while the UI thread is still busy with the Tab before it -- so this keeps
two readers on the fields' text while a third client moves the focus
through them, thousands of times.

Run against focus_order_demo (two text fields among buttons and toggles).
ci.sh launches the app under NSZombieEnabled on macOS, and with the Main
Thread Checker (MTC_CRASH_ON_REPORT=1) when Xcode's is installed: without
the fix the checker aborts the app on the first request and the zombies
trap within about a second (-[__NSFrozenDictionaryM release]: message sent
to deallocated instance); with it, neither does. On GTK4 every request was
already on the UI thread; this holds it there.

    stress_focus_reads.py <port> [moves]
"""
import json
import sys
import threading
import urllib.parse
import urllib.request

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9222
MOVES = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
BASE = "http://127.0.0.1:%d" % PORT


def req(path, post=False):
    r = urllib.request.Request(BASE + path, method="POST" if post else "GET",
                               data=b"" if post else None)
    with urllib.request.urlopen(r, timeout=20) as f:
        return f.read()


failures = []
stop = threading.Event()


def fail(msg):
    failures.append(msg)
    stop.set()


def main():
    widgets = json.loads(req("/widgets"))
    fields = [w["id"] for w in widgets if w["type"] == "textfield"]
    if len(fields) < 2:
        print("FAIL: expected two text fields, found %r" % fields)
        return 1
    want = {}
    for i, f in enumerate(fields):
        text = "field %d text" % (i + 1)
        req("/widget/%d/set_text?v=%s" % (f, urllib.parse.quote(text)), post=True)
        want[f] = text

    reads = [0]

    def reader():
        try:
            while not stop.is_set():
                for f in fields:
                    w = json.loads(req("/widget/%d" % f))
                    if w.get("text") != want[f]:
                        fail("widget %d read %r, not %r" % (f, w.get("text"), want[f]))
                        return
                    reads[0] += 1
        except Exception as e:  # the app died, or answered garbage
            fail("reader: %r after %d reads" % (e, reads[0]))

    readers = [threading.Thread(target=reader) for _ in range(2)]
    for t in readers:
        t.start()
    landed = 0
    try:
        for i in range(MOVES):
            if stop.is_set():
                break
            r = json.loads(req("/focus/move", post=True))
            if not r.get("moved"):
                fail("focus move %d did not move: %r" % (i, r))
                break
            if r.get("handle") in want:
                landed += 1
    except Exception as e:
        fail("mover: %r after %d moves" % (e, i))
    stop.set()
    for t in readers:
        t.join()
    if not failures and landed == 0:
        fail("the focus never landed in a text field")
    if not failures:
        try:
            req("/widgets")
        except Exception as e:
            fail("the app stopped answering: %r" % e)
    if failures:
        for f in failures:
            print("  FAIL: " + f)
        return 1
    print("  OK   %d focus moves (%d into a text field) beside %d text reads"
          % (MOVES, landed, reads[0]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # the app died before the stress began
        print("  FAIL: %r (did the app abort? see its log)" % e)
        sys.exit(1)
