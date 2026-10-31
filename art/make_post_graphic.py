#!/usr/bin/env python3
"""Compose the Facebook image for the companion-screen post.

    python3 art/make_post_graphic.py [--out companion-post.png]

Two real renders of the page (the wide panel and the phone-sized one) on a dark plate, with a
short caption baked in. Nothing is illustrated or invented: both screens are screenshots of the
page the plugin actually serves, captured against the device's own /state.
"""
import argparse
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.abspath(os.path.join(HERE, "..", "..", "docs"))
W, H = 1200, 675
BG = (8, 10, 7)
GOLD = (201, 162, 39)
CREAM = (233, 228, 210)
DIM = (154, 160, 143)
GREEN = (47, 191, 99)


def font(size, mono=True, bold=True):
    name = "DejaVuSansMono-Bold.ttf" if mono and bold else (
        "DejaVuSansMono.ttf" if mono else ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"))
    path = os.path.join("/usr/share/fonts/truetype/dejavu", name)
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


def rounded(img, radius):
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, img.size[0] - 1, img.size[1] - 1], radius, fill=255)
    out = Image.new("RGBA", img.size)
    out.paste(img, (0, 0), mask)
    return out, mask


def plate(base, path, box_w, x, y, radius=14, border=1):
    shot = Image.open(path).convert("RGB")
    ratio = box_w / shot.width
    shot = shot.resize((box_w, int(shot.height * ratio)), Image.LANCZOS)
    card, mask = rounded(shot, radius)

    # shadow
    sh = Image.new("RGBA", (card.width + 60, card.height + 60), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([30, 34, card.width + 30, card.height + 34], radius,
                                         fill=(0, 0, 0, 150))
    sh = sh.filter(ImageFilter.GaussianBlur(18))
    base.alpha_composite(sh, (x - 30, y - 30))

    base.paste(card, (x, y), mask)
    d = ImageDraw.Draw(base)
    d.rounded_rectangle([x, y, x + card.width - 1, y + card.height - 1], radius,
                        outline=GOLD + (90,), width=border)
    return card.size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(DOCS, "companion-post.png"))
    a = ap.parse_args()

    base = Image.new("RGBA", (W, H), BG + (255,))

    # a soft green glow behind the machine, the same colour the panel throws
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([W * 0.28, H * 0.18, W * 0.98, H * 1.02], fill=GREEN + (26,))
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    base.alpha_composite(glow)

    # caption
    d = ImageDraw.Draw(base)
    d.text((54, 40), "MPC MULTIHEAD ECHO", font=font(26), fill=GOLD)
    d.text((54, 76), "the screen runs in your phone", font=font(20, bold=False), fill=DIM)
    d.line([54, 112, W - 54, 112], fill=GOLD + (70,), width=1)

    # the two real renders
    wide = os.path.join(DOCS, "companion-panel-echo.png")
    tall = os.path.join(DOCS, "companion-panel-portrait.png")
    wsize = plate(base, wide, 700, 54, 150)
    tsize = plate(base, tall, 250, W - 54 - 250, 150, radius=26)

    # one line under them
    d = ImageDraw.Draw(base)
    d.text((54, 150 + wsize[1] + 26), "three pages  ·  swipe  ·  drag the knobs  ·  hold the switches",
           font=font(17, bold=False), fill=DIM)
    d.text((54, H - 34), "free  ·  GPL-3.0  ·  github.com/mitchoster/mpc-vst-multihead-echo",
           font=font(15, bold=False), fill=(93, 101, 85))

    out = base.convert("RGB")
    out.save(a.out, optimize=True)
    print("wrote", a.out, out.size)


if __name__ == "__main__":
    main()
