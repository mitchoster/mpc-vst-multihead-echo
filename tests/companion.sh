#!/usr/bin/env bash
# Companion feed (Phase 2): the plugin's own HTTP listener and the page it serves. No device.
#   vst/tests/companion.sh
# Two runs: one with the listener on a test port (all endpoint checks), one with MHEC_HTTP=0 to
# prove the switch really closes the socket (never leave a listener behind unintentionally).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"   # .../vst
host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host

PORT="${MHEC_TEST_PORT:-18099}"
OFF_PORT=$((PORT + 1))

# The page is generated into a .so string: a stale header would serve an old panel.
python3 "$HERE/web/embed.py" --check || exit 1

docker run --rm -e "MHEC_HTTP_PORT=$PORT" -e "MHEC_OFF_PORT=$OFF_PORT" \
  -v "$HERE":/w -w /w gcc:12 bash -lc '
set -e
c++ -std=gnu++17 -O2 -Wall -Wextra -I. -Isrc -Isrc/ported/shared-dpf/dsp -o /tmp/cp_test \
  tests/companion_test.cpp src/dsp/airtape_module.cpp src/dsp/companion.cpp src/dsp/qrcodegen.c src/dsp/preamp.c \
  src/dsp/chorus.c src/dsp/tape_imperfection.c src/dsp/spread.c src/ported/core/TapeEchoDSP.cpp \
  -lm -lpthread 2>&1 | grep -E "error|Error" && exit 1 || true
/tmp/cp_test

# The switch: with MHEC_HTTP=0 nothing may listen, even though an instance exists.
MHEC_HTTP=0 MHEC_HTTP_PORT=$MHEC_OFF_PORT /tmp/cp_test --instance-only >/dev/null 2>&1 &
pid=$!
sleep 0.5
if /tmp/cp_test --probe $MHEC_OFF_PORT; then
  echo "PASS: MHEC_HTTP=0 opens no listener"
else
  echo "FAIL: MHEC_HTTP=0 still opened a listener"
  kill $pid 2>/dev/null || true
  exit 1
fi
kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
'