#!/usr/bin/env python3
"""Draws the faceplate for MPC Multihead Echo (1280x628, plugin area only).

Text-free, no vendor artwork — geometry and colour only, because MPC draws every
label and value on top. One look (2026-11-01: Rack and Classic removed):

  roots   the pedal plate: black chassis, green centre panel, gold rules,
          three metal footswitches, one drawn head-tick row

Run in the build container (needs Pillow):
  docker run --rm -v <framework>:/repo \
    -w /repo/AKAI_MPC/VST/AirTapeEcho/vst python:3.11-slim \
    sh -c 'pip install -q pillow && python3 art/make_faceplate.py'
"""
import os

from PIL import Image, ImageDraw, ImageFilter

W, H = 1280, 628


def brushed(img, base, spread=5):
    d = ImageDraw.Draw(img)
    for x in range(0, W, 3):
        s = base + (x * 7 % spread)
        d.line([(x, 0), (x, H)], fill=(s, s + 1, s + 3), width=1)
    return img.filter(ImageFilter.GaussianBlur(1.1))


def screws(d, points, fill, edge):
    for cx, cy in points:
        d.ellipse([cx - 7, cy - 7, cx + 7, cy + 7], fill=fill, outline=edge)
        d.line([(cx - 4, cy), (cx + 4, cy)], fill=edge, width=1)


def viz_strip(out="viz_strip.png", w=420, h=34, frames=41, accent=(63, 174, 111)):
    """Echo-level meter: one frame per fill step, from empty to full.

    A meter is the only honest animation this platform can do smoothly (pre-rendered
    frames, one parameter, ~20 Hz redraw). It answers "how strongly does it repeat"
    without costing a redraw storm.
    """
    img = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for f in range(frames):
        y0 = f * h
        # a bar inside the VU window: segmented, with a peak line
        d.rectangle([0, y0, w - 1, y0 + h - 1], fill=(10, 11, 13, 0))
        fillw = int((w - 6) * f / (frames - 1))
        seg = 9
        x = 3
        while x < 3 + fillw:
            d.rectangle([x, y0 + 4, min(x + seg - 3, 3 + fillw), y0 + h - 5],
                        fill=(accent[0], accent[1], accent[2], 235))
            x += seg
        if f:
            px = 3 + fillw
            d.line([(px, y0 + 2), (px, y0 + h - 3)], fill=(240, 240, 235, 255), width=2)
    img.save(out)


def main():
    """One interface only (2026-11-01): Roots. Rack and Classic were removed — they were
    never held against a real reference and the owner's verdict was "terrible"."""
    labels = (("WARP", 250), ("JOG", 640), ("TWIST", 1030))
    roots(labels=labels)
    roots(labels=labels, out="bg_roots_char.png")
    viz_strip()                                                   # 420x34, the PEDAL meter
    print("wrote viz_strip.png + bg_roots.png + bg_roots_char.png", (W, H))



# --------------------------------------------------------------------------
# roots — the pedal plate (reference: a wide echo pedal, reggae palette).
#
# Black chassis, green centre panel, gold rules, red status lights, three large
# metal footswitches across the bottom. The renderer draws every knob, segment and
# value on top; this image only carries geometry, colour and the metal.
# --------------------------------------------------------------------------
def _label(d, cx, cy, text, fill):
    try:
        from PIL import ImageFont
        f = ImageFont.load_default(size=15)
    except Exception:
        from PIL import ImageFont
        f = ImageFont.load_default()
    w = d.textlength(text, font=f)
    d.text((cx - w / 2, cy), text, font=f, fill=fill)


def roots(labels=(("WARP", 250), ("JOG", 640), ("TWIST", 1030)), out="bg_roots.png"):
    TICK = (26, 120, 62)
    GOLD = (201, 162, 39)
    GOLD_DIM = (120, 96, 26)
    GREEN = (15, 107, 53)
    GREEN_DARK = (10, 44, 26)
    PANEL = (16, 17, 18)
    BASE = (10, 10, 11)
    RED = (214, 40, 40)

    img = brushed(Image.new("RGB", (W, H), BASE), 8, spread=6)
    d = ImageDraw.Draw(img)

    # chassis rim
    d.rounded_rectangle([8, 8, W - 9, H - 9], radius=16, outline=GOLD, width=3)
    d.rounded_rectangle([18, 18, W - 19, H - 19], radius=12, outline=GOLD_DIM, width=1)
    screws(d, [(34, 36), (W - 35, 36), (34, H - 37), (W - 35, H - 37)],
           (30, 31, 33), GOLD_DIM)

    # left block: LEDs, switch rows, saturation / wow / mix
    d.rounded_rectangle([26, 60, 330, 452], radius=12, fill=PANEL, outline=GOLD_DIM)

    # centre block: the mode panel
    d.rounded_rectangle([348, 60, 806, 452], radius=12, fill=GREEN, outline=GOLD, width=2)
    d.rounded_rectangle([362, 76, 792, 436], radius=8, fill=GREEN_DARK, outline=GOLD_DIM)
    # four head ticks, as on the pedal, under the mode grid
    for i in range(4):
        cx = 470 + i * 90
        d.ellipse([cx - 9, 436 - 9, cx + 9, 436 + 9], fill=BASE, outline=GOLD_DIM)
        d.ellipse([cx - 3, 436 - 3, cx + 3, 436 + 3], fill=TICK)

    # right block: six knobs in two rows of three
    d.rounded_rectangle([824, 60, W - 26, 452], radius=12, fill=PANEL, outline=GOLD_DIM)

    # footswitch row. With a rendered switch (switch_off.png from art/make_switch.py) the
    # disc comes from the photo, so only the status light is drawn here — the button widget
    # carries the switch itself and its pressed state.
    photo_switch = os.path.exists("switch_off.png")
    for cx in (250, 640, 1030):
        if not photo_switch:
            # fallback: a drawn bezel with a recessed cap where the button sits
            for r, col in ((58, (44, 45, 48)), (53, (70, 72, 76)), (44, (86, 89, 94)),
                           (38, (58, 60, 64))):
                d.ellipse([cx - r, 520 - r, cx + r, 520 + r], fill=col)
            d.ellipse([cx - 52, 520 - 52, cx + 52, 520 + 52], outline=GOLD, width=2)

    for text, cx in labels:
        _label(d, cx, 576, text, (232, 230, 220))

    img.save(out)
