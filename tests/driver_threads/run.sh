#!/usr/bin/env bash
# ci.sh's run_server_test passes the port; see stress_focus_reads.py and silent_client.py.
D="$(cd "$(dirname "$0")" && pwd)"
python3 -I "$D/silent_client.py" "${1:-9222}" || exit 1
exec python3 -I "$D/stress_focus_reads.py" "${1:-9222}" "${DRIVER_THREADS_MOVES:-4000}"
