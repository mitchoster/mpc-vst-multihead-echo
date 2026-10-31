#!/usr/bin/env bash
# Offline x86 host test (ASan/UBSan) for this port; must print PASSED. No device needed.
#   vst/test.sh
# Uses the host gcc/g++ when present, else the gcc:12 Docker image. The Docker daemon runs on the
# host and mounts only host-visible paths, so translate workspace paths to the host alias first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MV="${MPC_VST_PLUGINS:?set MPC_VST_PLUGINS to the mpc-vst-plugins checkout}"

host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host

MPC_VST_PLUGINS="$(host "$MV")" "$(host "$MV")/tools/test_port.sh" "$(host "$HERE")/vst.json" "$@"

echo "== ui switch (T9b) =="
"$HERE/tests/ui_switch.sh"

echo "== option parameters round-trip (labels with digits included) =="
"$HERE/tests/options.sh"

echo "== RE-202 Paket 1 (saturation, tape age, wet only, state) =="
"$HERE/tests/re202.sh"

echo "== RE-202 Paket 2 (sync, drift, warp, twist) =="
"$HERE/tests/t14.sh"

echo "== companion feed (HTTP /state + the page) =="
"$HERE/tests/companion.sh"

echo "== companion lifetime across dlclose (the 2026-11-01 device crash) =="
"$HERE/tests/dlclose.sh"

echo "== companion page logic (headless) =="
"$HERE/tests/page.sh"
