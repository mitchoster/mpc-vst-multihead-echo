#!/usr/bin/env bash
# Build the interface into ONE skin folder (skin set) and stage a deployable plugin folder.
#
#   vst/make_skinset.sh                        -> build/multihead/ (skin set + .so + version.xml)
#   DEVICE=<ip> vst/make_skinset.sh --deploy    -> ... and copy it to /sdcard/Synths/
#
# Why a set: MPC binds a skin at insert and has no reload call, but it re-reads
# "Plugin Skins/TUI.json" on every insert. The interface lives as a subfolder and the plugin
# copies two small JSON files + ui.txt (src/dsp/airtape_module.cpp, ui_swap). No image is copied.
# One interface only since 2026-11-01 (Rack and Classic removed); the mechanism stays, so a
# stale root TUI.json heals on the next insert.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MV="${MPC_VST_PLUGINS:?set MPC_VST_PLUGINS to the mpc-vst-plugins checkout}"
FOLDER="LarryDaimond - VST - MPC Multihead Echo"
SO="mpc_multihead_echo.so"
IFACES="roots"
ACTIVE="roots"

host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host
set_layout() { python3 - "$HERE" "$1" <<'PY'
import json, sys
here, name = sys.argv[1], sys.argv[2]
cfg = json.load(open(f"{here}/vst.json"))
cfg["layout"] = f"layout_{name}.conf"
json.dump(cfg, open(f"{here}/vst.json", "w"), indent=2)
PY
}

cp "$HERE/vst.json" "$HERE/build.vst.json.bak"
trap 'mv -f "$HERE/build.vst.json.bak" "$HERE/vst.json"' EXIT

echo "== 1/4 build .so + the active skin ($ACTIVE) =="
set_layout "$ACTIVE"
"$(host "$MV")/tools/build_port.sh" "$(host "$HERE")/vst.json" >/dev/null

STAGE="$HERE/build/multihead"
rm -rf "$STAGE" "$HERE/build/skins"
mkdir -p "$STAGE/$FOLDER/Plugin Skins" "$HERE/build/skins"
cp -a "$HERE/build/skin/$FOLDER/Plugin Skins" "$HERE/build/skins/$ACTIVE"
cp -a "$HERE/build/skin/$FOLDER/version.xml" "$STAGE/$FOLDER/version.xml" 2>/dev/null || true

echo "== 2/4 other interfaces (none since 2026-11-01) =="
for i in $IFACES; do
  [ "$i" = "$ACTIVE" ] && continue
  set_layout "$i"
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$HERE":/w -v "$(host "$MV")":/mv:ro \
    -w "/w" mpc-vst-html-art \
    python3 /mv/tools/gen_vst.py vst.json >/dev/null
  cp -a "$HERE/build/skin/$FOLDER/Plugin Skins" "$HERE/build/skins/$i"
done
set_layout "$ACTIVE"

echo "== 3/4 assemble the skin set (paths rewritten into each subfolder) =="
python3 - "$HERE" "$STAGE" "$FOLDER" "$ACTIVE" $IFACES <<'PY'
import os, shutil, sys
here, stage, folder, active = sys.argv[1:5]
ifaces = sys.argv[5:]
root = os.path.join(stage, folder, "Plugin Skins")
for i in ifaces:
    src = os.path.join(here, "build", "skins", i)
    dst = os.path.join(root, i)
    shutil.copytree(src, dst, dirs_exist_ok=True)
    tui = os.path.join(dst, "TUI.json")
    if os.path.isfile(tui):
        text = open(tui, encoding="utf-8").read()
        for name in sorted(os.listdir(dst)):
            if name.lower().endswith(".png"):
                text = text.replace('"%s"' % name, '"%s/%s"' % (i, name))
        open(tui, "w", encoding="utf-8").write(text)
shutil.copyfile(os.path.join(root, active, "TUI.json"), os.path.join(root, "TUI.json"))
ql = os.path.join(root, active, "Q-Links.json")
if os.path.isfile(ql):
    shutil.copyfile(ql, os.path.join(root, "Q-Links.json"))
open(os.path.join(root, "ui.txt"), "w").write(active + "\n")
print("skin set:", sorted(os.listdir(root)))
PY

cp "$HERE/build/$SO" "$STAGE/$FOLDER/$SO"
echo "staged: $STAGE/$FOLDER"

if [ "${1:-}" = "--deploy" ]; then
  : "${DEVICE:?set DEVICE to the MPC IP}"
  echo "== 4/4 deploy to $DEVICE =="
  tar -C "$STAGE" -cf - "$FOLDER" | ssh -o StrictHostKeyChecking=no "root@$DEVICE" "tar -C /sdcard/Synths -xf -"
  # An insert EFFECT's presets are read from 'MPC Documents/Plugin Presets/Insert Effects/<skin>/',
  # NOT from the skin's own Presets/ (that is the instruments path). Copied in without clearing the
  # folder, so presets the user saved stay.
  PRESET_STAGE="$(mktemp -d)"
  python3 "$HERE/gen_presets.py" --out-dir "$PRESET_STAGE" --so-path "/sdcard/Synths/$FOLDER/$SO" >/dev/null
  PUSER="/sdcard/MPC Documents/Plugin Presets/Insert Effects/$FOLDER"
  tar -C "$PRESET_STAGE" -cf - . | ssh -o StrictHostKeyChecking=no "root@$DEVICE" \
    "mkdir -p '$PUSER' && tar -C '$PUSER' -xf -"
  rm -rf "$PRESET_STAGE"
  echo -n "device md5: "; ssh -o StrictHostKeyChecking=no "root@$DEVICE" "md5sum '/sdcard/Synths/$FOLDER/$SO' | cut -d' ' -f1"
  echo -n "device presets: "; ssh -o StrictHostKeyChecking=no "root@$DEVICE" "ls '$PUSER'/*.xpl | wc -l"
  echo -n "local  md5: "; md5sum "$STAGE/$FOLDER/$SO" | cut -d' ' -f1
fi
