#!/usr/bin/env bash
# ci.sh's run_server_test passes the port; see stress_focus_reads.py.
exec python3 -I "$(cd "$(dirname "$0")" && pwd)/stress_focus_reads.py" "${1:-9222}" "${DRIVER_THREADS_MOVES:-4000}"
