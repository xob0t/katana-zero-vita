"""Generates the LiveArea images for the Katana ZERO app (original placeholder art).

Vita LiveArea images must be 8-bit indexed PNGs of fixed sizes.
Usage: python scripts/make_livearea.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "livearea")

TOP = (24, 8, 48)
BOTTOM = (120, 20, 90)
SLASH = (255, 90, 200)
TEXT = (240, 235, 255)


def font(size):
    for name in ("arialbd.ttf", "Arial Bold.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def gradient(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        c = tuple(int(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def slash(draw, w, h, width):
    draw.line([(w * 0.08, h * 0.92), (w * 0.92, h * 0.08)], fill=SLASH, width=width)


def centered(draw, w, y, text, size):
    f = font(size)
    box = draw.textbbox((0, 0), text, font=f)
    draw.text(((w - (box[2] - box[0])) / 2, y), text, font=f, fill=TEXT)


def save(img, name):
    img.convert("P", palette=Image.ADAPTIVE, colors=256).save(os.path.join(OUT, name), optimize=True)


def main():
    os.makedirs(OUT, exist_ok=True)

    icon = gradient(128, 128)
    d = ImageDraw.Draw(icon)
    slash(d, 128, 128, 6)
    centered(d, 128, 38, "KZ", 48)
    save(icon, "icon0.png")

    bg = gradient(840, 500)
    d = ImageDraw.Draw(bg)
    slash(d, 840, 500, 10)
    centered(d, 840, 190, "KATANA ZERO", 72)
    centered(d, 840, 290, "PS Vita", 32)
    save(bg, "bg0.png")

    gate = gradient(280, 158)
    d = ImageDraw.Draw(gate)
    slash(d, 280, 158, 5)
    centered(d, 280, 55, "KATANA ZERO", 30)
    save(gate, "startup.png")

    with open(os.path.join(OUT, "template.xml"), "w", encoding="utf-8") as f:
        f.write(
            '<?xml version="1.0" encoding="utf-8"?>\n\n'
            '<livearea style="a1" format-ver="01.00" content-rev="1">\n'
            "\t<livearea-background>\n\t\t<image>bg0.png</image>\n\t</livearea-background>\n\n"
            "\t<gate>\n\t\t<startup-image>startup.png</startup-image>\n\t</gate>\n"
            "</livearea>\n"
        )


if __name__ == "__main__":
    main()
