#!/usr/bin/env python3
"""Draw the companion-page QR into a page background.

Why this exists: the QR that matters is the one on the **MPC screen** — you open the insert,
see the code, scan it, and the phone page opens. A QR inside the page is circular, and the
device's time is better spent on the knob panel.

The address depends on the device's DHCP address, so the QR is generated at **deploy** time
(see vst/redeploy.sh) from the IP the deploy already knows, and again whenever
`--companion-url` changes it. Re-inserting the plugin re-reads the skin, so a changed address
needs one remove/insert — no MPC restart.

    python3 vst/art/make_qr_plate.py --url http://192.0.2.7:8102/ \
        --background rack/sh_bg_0.png --out rack/sh_bg_0.png --x 1128 --y 468

The matrix comes from the plugin's own encoder: `qrgen` (tools/qrgen.c, built with the vendored
qrcodegen, MIT/Nayuki) prints exactly the module set that /qr.svg serves, so the plate and the
endpoint can never disagree.
"""
import argparse
import os
import re
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
VST = os.path.dirname(HERE)


def build_qrgen(workdir):
    """Compile the host helper once; it links the same qrcodegen.c the plugin ships."""
    out = os.path.join(workdir, "qrgen")
    if os.path.exists(out):
        return out
    src = os.path.join(VST, "tools", "qrgen.c")
    gen = os.path.join(VST, "src", "dsp", "qrcodegen.c")
    if not os.path.exists(src):
        raise SystemExit(f"missing {src}")
    subprocess.run(["cc", "-O1", "-std=c99", "-I", os.path.dirname(gen), "-o", out, src, gen], check=True)
    return out


def matrix(svg):
    n = int(re.search(r'viewBox="-2 -2 (\d+)', svg).group(1)) - 4
    grid = [[False] * n for _ in range(n)]
    for run in re.finditer(r"M(\d+) (\d+)h(\d+)", svg):
        x, y, w = (int(g) for g in run.groups())
        for i in range(w):
            grid[y][x + i] = True
    return n, grid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--background", required=True, help="page background to draw on")
    ap.add_argument("--out", help="defaults to --background (in place)")
    ap.add_argument("--x", type=int, default=1128)
    ap.add_argument("--y", type=int, default=468)
    ap.add_argument("--module", type=int, default=4)
    ap.add_argument("--label", default="SCAN FOR\nCOMPANION\nPAGE")
    ap.add_argument("--label-x", type=int, default=1004)
    ap.add_argument("--pad", type=int, default=7, help="margin between the code and the plate edge")
    ap.add_argument("--radius", type=int, default=8, help="plate corner radius")
    ap.add_argument("--plate", default="e9e4d6", help="plate fill, hex")
    ap.add_argument("--border", default="", help="hairline around the plate, hex (empty = none)")
    args = ap.parse_args()

    workdir = "/tmp/opencode/qr"
    os.makedirs(workdir, exist_ok=True)
    qrgen = build_qrgen(workdir)
    svg = subprocess.run([qrgen, args.url], capture_output=True, text=True, check=True).stdout
    n, grid = matrix(svg)

    quiet = 2
    side = (n + 2 * quiet) * args.module
    img = Image.open(args.background).convert("RGB")
    d = ImageDraw.Draw(img)

    pad = args.pad
    plate = tuple(int(args.plate[i:i + 2], 16) for i in (0, 2, 4))
    d.rounded_rectangle([args.x, args.y, args.x + side + 2 * pad, args.y + side + 2 * pad],
                        radius=args.radius, fill=plate)
    if args.border:
        pen = tuple(int(args.border[i:i + 2], 16) for i in (0, 2, 4))
        d.rounded_rectangle([args.x, args.y, args.x + side + 2 * pad, args.y + side + 2 * pad],
                            radius=args.radius, outline=pen, width=2)
    for y in range(n):
        for x in range(n):
            if not grid[y][x]:
                continue
            px = args.x + pad + (x + quiet) * args.module
            py = args.y + pad + (y + quiet) * args.module
            d.rectangle([px, py, px + args.module - 1, py + args.module - 1], fill=(10, 12, 9))

    if args.label:
        try:
            font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 14)
        except OSError:
            font = ImageFont.load_default()
        for i, line in enumerate(args.label.split("\n")):
            d.text((args.label_x, args.y + 40 + i * 22), line, font=font, fill=(201, 162, 39))

    out = args.out or args.background
    img.save(out)
    print(f"{out}: {args.url} ({side}x{side} at {args.x},{args.y})")


if __name__ == "__main__":
    main()
