#!/usr/bin/env python3
"""A client that connects and never sends must not wedge the driver.

The server answers one connection at a time and reads each request before it
hands it to the UI thread, so a silent connection (a port probe, a harness
that died between connect and send) used to hold every later request forever.
The read is bounded now: a second client is answered once the silent one has
timed out (about 5 s).

    silent_client.py <port>
"""
import socket
import sys
import time
import urllib.request

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9222
silent = socket.socket()
silent.connect(("127.0.0.1", port))
t = time.time()
try:
    urllib.request.urlopen("http://127.0.0.1:%d/focus" % port, timeout=20).read()
except Exception as e:
    print("  FAIL: a silent connection wedged the driver: %s (%.1fs)" % (e, time.time() - t))
    sys.exit(1)
finally:
    silent.close()
print("  OK   a silent connection held the driver for %.1fs, then it answered" % (time.time() - t))
