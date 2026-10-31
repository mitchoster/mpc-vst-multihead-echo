#!/usr/bin/env bash
# MPC Multihead Echo — build the skin and render every page offline (no device needed).
#   vst/preview.sh  -> docs/ui-preview/page_0..N.png
set -euo pipefail

MV="${MPC_VST_PLUGINS:?set MPC_VST_PLUGINS to the mpc-vst-plugins checkout}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
FOLDER="LarryDaimond - VST - MPC Multihead Echo"

# Docker sees host paths; the workspace is <framework> there.
host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host

echo "== build skin =="
"$(host "$MV")/tools/build_port.sh" "$(host "$HERE")/vst.json" >/dev/null

mkdir -p "$PROJECT/docs/ui-preview"
echo "== render pages -> docs/ui-preview/page_*.png =="
docker run --rm -v "$(host "$MV")":/mv:ro -v "$(host "$PROJECT")":/w -w /w python:3.11-slim sh -c '
  pip install -q --no-warn-script-location Pillow >/dev/null 2>&1
  python3 /mv/tools/studio.py preview "/w/vst/build/skin/'"$FOLDER"'/Plugin Skins" -o "/w/docs/ui-preview/page_%d.png"
'
echo "done."
