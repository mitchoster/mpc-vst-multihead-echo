#!/usr/bin/env bash
# RE-202 Paket-2 regression test: tempo sync, drift, warp, twist. No device, no MPC.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"   # .../vst
host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host

grep -q '"HAS_LFO_BPM"' "$HERE/vst.json" || {
  echo "FAIL: vst.json does not pass the host tempo (HAS_LFO_BPM)" >&2; exit 1; }

docker run --rm -v "$HERE":/w -w /w gcc:12 bash -lc '
set -e
c++ -std=gnu++17 -O2 -Wall -Wextra -I. -Isrc -Isrc/ported/shared-dpf/dsp -o /tmp/t14_test \
  tests/t14_test.cpp src/dsp/airtape_module.cpp src/dsp/companion.cpp src/dsp/preamp.c src/dsp/chorus.c \
  src/dsp/tape_imperfection.c src/dsp/spread.c src/ported/core/TapeEchoDSP.cpp -lm -lpthread 2>&1 \
  | grep -E "error" && exit 1 || true
/tmp/t14_test'
