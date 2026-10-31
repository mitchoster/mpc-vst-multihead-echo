#!/usr/bin/env bash
# Interface-swap regression test (T9b): needs no device, no MPC — proves that
# ui.txt is adopted on insert, that a real change writes the skin, and that the
# host re-applying saved values does not undo the user's choice.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"   # .../vst

# The swap below is driven with an explicit dir, so this test alone cannot see the
# one thing that broke on the device (2026-10-29): without "MODULE_SUBDIR" in
# vst.json the wrapper passes an EMPTY dir, the control flips and nothing is
# written. Guard the wiring, not just the code path.
grep -q '"MODULE_SUBDIR"' "$HERE/vst.json" || {
  echo "FAIL: vst.json has no MODULE_SUBDIR — create_instance() gets an empty dir, the ui switch writes nothing" >&2
  exit 1; }
python3 - "$HERE/vst.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert "MODULE_SUBDIR" in d.get("defines", {}), "MODULE_SUBDIR missing from defines"
print("PASS: vst.json defines MODULE_SUBDIR =", d["defines"]["MODULE_SUBDIR"])
PY
host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host
docker run --rm -v "$HERE":/w -w /w gcc:12 bash -lc '
set -e
c++ -std=gnu++17 -O2 -Wall -Wextra -I. -Isrc -Isrc/ported/shared-dpf/dsp -o /tmp/ui_test \
  tests/ui_switch_test.cpp src/dsp/airtape_module.cpp src/dsp/companion.cpp src/dsp/preamp.c src/dsp/chorus.c \
  src/dsp/tape_imperfection.c src/dsp/spread.c src/ported/core/TapeEchoDSP.cpp -lm -lpthread 2>&1 \
  | grep -E "error" && exit 1 || true
/tmp/ui_test'
