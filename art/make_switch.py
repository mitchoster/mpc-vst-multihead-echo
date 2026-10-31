#!/usr/bin/env python3
"""Turn one rendered footswitch into the two button images the skin uses.

    python3 art/make_switch.py raw_render.png       # -> switch_off.png, switch_on.png

The render is a chrome footswitch on a dark background. It is keyed out by
luminance (the part is bright, the background is not), so the hexagonal nut keeps
its corners instead of being clipped by a circle, then normalised to a square.

Two states, because a switch that only changes colour does not read as a switch:

  switch_off.png  the part as rendered — cap raised
  switch_on.png   pressed: the whole part drawn slightly smaller and darker, with
                  the cap's highlight collapsed and a recess shadow around it, so
                  it reads as "pushed in", plus a thin warm rim as the active cue
"""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageEnhance, ImageChops

OUT = 192          # 2x the layout's 96 px button, downscaled by MPC/preview


def keyed(img):
    """Luminance key: alpha follows brightness with a smooth ramp, black -> clear."""
    img = img.convert("RGB")
    w, h = img.size
    s = min(w, h)
    img = img.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))
    img = img.resize((OUT * 2, OUT * 2), Image.LANCZOS)
    luma = img.convert("L")
    # Adapt the threshold to the render: the corner median is the background level.
    px = sorted(luma.getdata())
    bg = px[int(len(px) * 0.15)]
    lo, hi = bg + 15, bg + 42
    alpha = luma.point(lambda v: 0 if v <= lo else (255 if v >= hi else int((v - lo) * 255 / max(1, hi - lo))))
    alpha = alpha.filter(ImageFilter.GaussianBlur(0.8))
    out = img.convert("RGBA")
    out.putalpha(alpha)
    # trim the empty border so the part fills the square
    bbox = alpha.getbbox()
    if bbox:
        out = out.crop(bbox)
        side = max(out.size)
        sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        sq.paste(out, ((side - out.size[0]) // 2, (side - out.size[1]) // 2), out)
        out = sq
    return out.resize((OUT * 2, OUT * 2), Image.LANCZOS)


def drop_shadow(base, spread=0.030, alpha=110, dy=0.008):
    w, h = base.size
    sh = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    a = base.getchannel("A").point(lambda v: int(v * alpha / 255))
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    shadow.putalpha(a)
    sh.paste(shadow, (0, int(h * dy)), shadow)
    sh = sh.filter(ImageFilter.GaussianBlur(w * spread))
    return Image.alpha_composite(sh, base)


def pressed(off):
    """Pressed: the part sits a touch lower and darker, with a shadow under the cap rim.

    No glow and no ring: the recess has to read as geometry, not as a light. The cap is
    what moves, so the shadow is placed on the upper part of the cap and fades downward.
    """
    w, h = off.size
    small = off.resize((int(w * 0.975), int(h * 0.975)), Image.LANCZOS)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    canvas.paste(small, ((w - small.size[0]) // 2, (h - small.size[1]) // 2 + int(h * 0.015)), small)

    rgb = canvas.convert("RGB")
    rgb = ImageEnhance.Brightness(rgb).enhance(0.80)
    rgb = ImageEnhance.Contrast(rgb).enhance(1.05)
    out = rgb.convert("RGBA")
    out.putalpha(canvas.getchannel("A"))

    # recess shadow: dark inside the top edge of the cap, fading toward its middle
    sh = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(sh)
    d.ellipse([w * 0.22, h * 0.10, w * 0.78, h * 0.62], fill=(0, 0, 0, 130))
    sh = sh.filter(ImageFilter.GaussianBlur(w * 0.07))
    sh.putalpha(ImageChops.multiply(sh.getchannel("A"), out.getchannel("A")))
    return Image.alpha_composite(out, sh)


def main(src):
    part = keyed(Image.open(src))
    off = drop_shadow(part)
    on = drop_shadow(pressed(part), spread=0.035, alpha=130, dy=0.012)
    off.resize((OUT, OUT), Image.LANCZOS).save("switch_off.png")
    on.resize((OUT, OUT), Image.LANCZOS).save("switch_on.png")
    print("wrote switch_off.png switch_on.png (%dx%d)" % (OUT, OUT))


if __name__ == "__main__":
    main(sys.argv[1])
