#!/usr/bin/env bash
# Does the listener survive the host unloading the .so? (Device crash of 2026-11-01.)
#   vst/tests/dlclose.sh
# MPC dlclose()s a plugin's .so when the last instance is removed. A thread still parked in
# poll() then runs in unmapped memory and kills the app ("OOPS!"). This builds the real
# companion unit into a shared object and does the exact sequence in-process, both ways:
#   A: attach -> detach -> dlclose   must SURVIVE (detach joins the thread)
#   B: attach -> dlclose             must CRASH   (what the bug looked like)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${MHEC_DLCLOSE_PORT:-18199}"
# The Docker daemon runs on the host and only sees host-visible paths.
host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host

docker run --rm -e "MHEC_HTTP_PORT=$PORT" -v "$(host "$HERE")":/w -w /w gcc:12 bash -lc '
set -e
g++ -std=gnu++17 -O2 -fPIC -shared -Wall -Wextra -Isrc -o /tmp/libcp_under_test.so \
  src/dsp/companion.cpp tests/dlclose_shim.cpp -lpthread
g++ -std=gnu++17 -O2 -Wall -Wextra -o /tmp/dlclose_test tests/dlclose_test.cpp -ldl

echo "== A: detach, then unload (must survive)"
/tmp/dlclose_test /tmp/libcp_under_test.so '"$PORT"' > /tmp/a.log 2>&1; rc=$?
cat /tmp/a.log
if [ $rc -ne 0 ]; then echo "FAIL: the fixed code did not survive dlclose (rc=$rc)"; exit 1; fi
echo "PASS: fixed code unloads cleanly"

echo "== B: unload with the listener still running (must crash)"
set +e
/tmp/dlclose_test /tmp/libcp_under_test.so '"$PORT"' --no-detach > /tmp/b.log 2>&1; rc=$?
set -e
cat /tmp/b.log
if [ $rc -lt 128 ]; then echo "FAIL: expected a signal death (rc>=128), got rc=$rc — the test proves nothing"; exit 1; fi
grep -q "listener answering" /tmp/b.log || { echo "FAIL: the listener never came up in case B"; exit 1; }
echo "PASS: unloading a running listener dies (signal $((rc-128))), which is what the device did"
'
