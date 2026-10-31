#!/usr/bin/env bash
# Build this port with the upstream mpc-vst-plugins toolchain.
#   vst/build.sh [extra args]
# Needs Docker (with QEMU for arm32v7). Set MPC_VST_PLUGINS to the framework checkout if it is
# not the default sibling.
#
# The Docker daemon runs on the host and mounts only host-visible paths, so translate this
# alias (<framework>) before calling the tools.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MV="${MPC_VST_PLUGINS:?set MPC_VST_PLUGINS to the mpc-vst-plugins checkout}"

host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host

"$(host "$MV")/tools/build_port.sh" "$(host "$HERE")/vst.json" "$@"
