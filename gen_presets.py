#!/usr/bin/env python3
"""Ship named MPC presets for MPC Multihead Echo.

MPC reads an insert **effect**'s presets from
`MPC Documents/Plugin Presets/Insert Effects/<vendor> - VST - <name>/`, one `.xpl`
per preset (the skin's own `Presets/` folder is the *instruments* path and is not
shown for an effect). That `.xpl` is `<pluginstate>`: a `<PLUGIN .../>` line, a
`<preset>` name and a `<state>` holding a JUCE VST2 fxb chunk set around the
plugin's own chunk.

Our chunk is the engine's `state` string — `key=value` lines in the order of
`src/airtape_module.json`, exactly what `get_param("state")` writes and
`set_param("state")` reads (wrapper `effGetChunk` / `effSetChunk`).

    python3 vst/gen_presets.py                                  # -> build/presets/
    python3 vst/gen_presets.py --out-dir DIR --so-path ABS_SO    # device effect folder
    python3 vst/gen_presets.py --list                            # names + state only

Add a preset: append one `(name, {key: value, ...})` entry to PRESETS below; keys
not listed keep the module's default. Values must be inside the declared range —
this script refuses anything else instead of silently clamping.
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
MV = os.environ.get("MPC_VST_PLUGINS", "<framework>")


def load_xpl():
    path = os.path.join(MV, "tools", "xpl.py")
    spec = importlib.util.spec_from_file_location("xpl", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_presets():
    """The list lives in vst/presets.json — the same file the wrapper compiles into MPC's
    PRESET menu (vst.json "presets"), so the menu and the .xpl files can never drift apart."""
    with open(os.path.join(HERE, "presets.json"), encoding="utf-8") as f:
        d = json.load(f)
    presets = d["presets"] if isinstance(d, dict) else d
    return [(p["name"], p.get("values", {})) for p in presets]


PRESETS = load_presets()


def load_params():
    with open(os.path.join(HERE, "src", "airtape_module.json"), encoding="utf-8") as f:
        mod = json.load(f)
    return mod["capabilities"]["chain_params"]


def check(plist, name, overrides):
    by_key = {p["key"]: p for p in plist}
    for key, val in overrides.items():
        p = by_key.get(key)
        if p is None:
            raise SystemExit("preset %r: unknown parameter %r" % (name, key))
        if p["type"] == "enum":
            if val not in p["options"]:
                raise SystemExit("preset %r: %r is not one of %s" % (name, val, p["options"]))
        else:
            if not isinstance(val, (int, float)) or val < p["min"] or val > p["max"]:
                raise SystemExit("preset %r: %s=%r outside %s..%s"
                                 % (name, key, val, p["min"], p["max"]))


def state_string(plist, overrides):
    """The engine's `state` text: key=value per line, module.json order."""
    lines = []
    for p in plist:
        key = p["key"]
        val = overrides.get(key, p.get("default"))
        if isinstance(val, float):
            text = ("%.4f" % val).rstrip("0").rstrip(".") or "0"
        else:
            text = str(val)
        lines.append("%s=%s" % (key, text))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", help="where the .xpl files go (default: build/presets/)")
    ap.add_argument("--so-path", help="the <PLUGIN file=...> value; for a device effect "
                                      "preset pass the absolute .so path")
    ap.add_argument("--list", action="store_true", help="print names and state, write nothing")
    a = ap.parse_args()

    with open(os.path.join(HERE, "vst.json"), encoding="utf-8") as f:
        cfg = json.load(f)
    plist = load_params()
    xpl = load_xpl()

    so_path = a.so_path or "%%payload-path%%/%s - VST - %s/%s" % (
        cfg["vendor"], cfg["name"], cfg["so"])
    uid = int.from_bytes(cfg["uid"].encode(), "big")
    version = cfg.get("version", 1000)

    out_dir = a.out_dir or os.path.join(HERE, "build", "presets")
    if not a.list:
        os.makedirs(out_dir, exist_ok=True)

    for name, overrides in PRESETS:
        check(plist, name, overrides)
        state = state_string(plist, overrides)
        if a.list:
            print("== %s\n%s" % (name, state))
            continue
        chunk = state.encode("utf-8") + b"\0"   # the NUL effGetChunk appends
        text = xpl.xpl(cfg["name"], cfg["vendor"], so_path, uid, version, chunk,
                       preset=name, effect=True)
        dst = os.path.join(out_dir, "%s.xpl" % name)
        with open(dst, "w", encoding="utf-8") as f:
            f.write(text)
        print(dst)

    if not a.list:
        print("%d presets -> %s" % (len(PRESETS), out_dir))


if __name__ == "__main__":
    main()
