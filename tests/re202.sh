#!/usr/bin/env bash
# RE-202 Paket-1 regression test: saturation, tape age, wet only, state chunk.
# Needs no device and no MPC — only Docker with the gcc:12 image.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"   # .../vst
host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host

grep -q '"saturation"' "$HERE/src/airtape_module.json" || {
  echo "FAIL: saturation missing from the parameter contract" >&2; exit 1; }

docker run --rm -v "$HERE":/w -w /w gcc:12 bash -lc '
set -e
c++ -std=gnu++17 -O2 -Wall -Wextra -I. -Isrc -Isrc/ported/shared-dpf/dsp -o /tmp/re202_test \
  tests/re202_test.cpp src/dsp/airtape_module.cpp src/dsp/companion.cpp src/dsp/qrcodegen.c src/dsp/preamp.c src/dsp/chorus.c \
  src/dsp/tape_imperfection.c src/dsp/spread.c src/ported/core/TapeEchoDSP.cpp -lm -lpthread 2>&1 \
  | grep -E "error" && exit 1 || true
/tmp/re202_test'
