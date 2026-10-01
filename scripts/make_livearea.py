"""Generates the LiveArea images for the Katana ZERO app.

Without arguments: original placeholder art in livearea/ (committed).
With --apk: the game's own title art (title background, fence and the neon KATANA ZERO logo), extracted from
your APK into livearea/game/ (not committed; the build uses it when present).

Vita LiveArea images must be 8-bit indexed PNGs of fixed sizes (icon 128x128, background 840x500,
startup gate 280x158).
Usage: python scripts/make_livearea.py [--apk <Katana ZERO APK, XAPK or game.droid>]
"""
import os
import sys
from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFont

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


def additive(base, layer):
    """Adds a glow layer onto base the way the title screen draws its neon frames (additive blending)."""
    glow = Image.composite(layer.convert("RGB"), Image.new("RGB", layer.size), layer.split()[3])
    out = ImageChops.add(base.convert("RGB"), glow).convert("RGBA")
    out.putalpha(ImageChops.lighter(base.split()[3], layer.split()[3]))
    return out


def game_logo(game):
    """The title logo: dim ZERO outline + lit ZER and Ø frames, KATANA above."""
    big = game.frames("spr_titlegraphic_big")
    zero = additive(additive(big[0], big[1]), big[2])
    zero = zero.crop(zero.getbbox())
    katana = game.frames("spr_titlegraphic_big2")[0]
    katana = katana.crop(katana.getbbox())
    kw = int(zero.width * 0.58)
    katana = katana.resize((kw, int(katana.height * kw / katana.width)), Image.LANCZOS)
    top = int(katana.height * 0.62)
    logo = Image.new("RGBA", (zero.width, zero.height + top), (0, 0, 0, 0))
    logo.alpha_composite(zero, (0, top))
    logo.alpha_composite(katana, (int(zero.width * 0.24), 0))
    return logo


def game_background(game, w, h):
    """Title background plate with the chain-link fence, cropped to fill w x h."""
    plate = game.frames("spr_title_background")[0].crop((0, 100, 640, 720))  # skip the black top band
    scale = max(w / plate.width, h / plate.height)
    size = (int(plate.width * scale) + 1, int(plate.height * scale) + 1)
    plate = plate.resize(size, Image.LANCZOS)
    fence = game.frames("spr_title_fence")[0].crop((0, 100, 640, 720)).resize(size, Image.LANCZOS)
    fence.putalpha(fence.split()[3].point(lambda a: a // 2))
    plate.alpha_composite(fence)
    x, y = (plate.width - w) // 2, (plate.height - h) // 2
    plate = plate.crop((x, y, x + w, y + h))
    return ImageEnhance.Brightness(plate.convert("RGB")).enhance(0.85).convert("RGBA")


def place(canvas, img, cx, cy, width):
    img = img.resize((width, int(img.height * width / img.width)), Image.LANCZOS)
    canvas.alpha_composite(img, (int(cx - img.width / 2), int(cy - img.height / 2)))


def game_art(apk):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from extract_art import GameData, load_game_data

    game = GameData(load_game_data(apk))
    logo = game_logo(game)
    out = os.path.join(OUT, "game")
    os.makedirs(out, exist_ok=True)
    for name, (w, h), (cx, cy, width) in (
        ("icon0.png", (128, 128), (64, 66, 112)),
        ("bg0.png", (840, 500), (420, 230, 560)),
        ("startup.png", (280, 158), (140, 79, 210)),
    ):
        img = game_background(game, w, h)
        place(img, logo, cx, cy, width)
        img.convert("RGB").convert("P", palette=Image.ADAPTIVE, colors=256).save(os.path.join(out, name), optimize=True)
    print(f"game art written to {out}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--apk":
        game_art(sys.argv[2])
        return
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
