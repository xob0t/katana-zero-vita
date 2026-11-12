"""Generates the LiveArea images for the Katana ZERO app from the game's own title art (title background and
fence; the neon KATANA ZERO logo on the icon and start gate), extracted from your APK into livearea/. The images
are game art, so they are not committed; the app build needs them.

Vita LiveArea images must be 8-bit indexed PNGs of fixed sizes (icon 128x128, background 840x500,
startup gate 280x158).
Usage: python scripts/make_livearea.py <Katana ZERO APK, XAPK or game.droid>
"""
import os
import sys
from PIL import Image, ImageChops, ImageEnhance

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "livearea")


def additive(base, layer):
    """Adds a glow layer onto base the way the title screen draws its neon frames (additive blending)."""
    glow = Image.composite(layer.convert("RGB"), Image.new("RGB", layer.size), layer.split()[3])
    out = ImageChops.add(base.convert("RGB"), glow).convert("RGBA")
    out.putalpha(ImageChops.lighter(base.split()[3], layer.split()[3]))
    return out


# Letter colours of ZERO as the title screen draws it (sampled from the running game): Z, E, R, Ø
ZERO_COLORS = ((0, 225, 225), (160, 20, 218), (225, 69, 197), (235, 0, 138))


def letter_bounds(alpha):
    """Column bounds of the four ZERO letters: the three widest interior gaps of empty columns."""
    w, h = alpha.size
    empty = [alpha.crop((x, 0, x + 1, h)).getbbox() is None for x in range(w)]
    gaps, x = [], 0
    while x < w:
        if empty[x]:
            start = x
            while x < w and empty[x]:
                x += 1
            if start > 0 and x < w:
                gaps.append((x - start, (start + x) // 2))
        else:
            x += 1
    return [0] + sorted(mid for _, mid in sorted(gaps, reverse=True)[:3]) + [w]


def tint_letters(img, bounds):
    """Colours each letter of the lit ZERO by its brightness, keeping the white-hot neon cores."""
    lum = img.convert("RGB").convert("L")
    alpha = img.split()[3]
    out = Image.new("RGBA", img.size)
    for i, color in enumerate(ZERO_COLORS):
        box = (bounds[i], 0, bounds[i + 1], img.height)
        part = lum.crop(box)
        channels = [part.point(lambda v, k=k: min(255, k * v // 160 + max(0, v - 190) * 5 // 2)) for k in color]
        out.paste(Image.merge("RGBA", channels + [alpha.crop(box)]), box[:2])
    return out


def game_logo(game):
    """The title logo as the game shows it: ZERO from the dim outline + lit frames, coloured per letter,
    with KATANA overlapping its top like on the title screen."""
    big = game.frames("spr_titlegraphic_big")
    lit = additive(additive(big[0], big[1]), big[2])
    box = lit.getbbox()
    outline = big[0].getbbox()
    bounds = [b + outline[0] - box[0] for b in letter_bounds(big[0].crop(outline).split()[3])]
    bounds[0], bounds[-1] = 0, box[2] - box[0]
    zero = tint_letters(lit.crop(box), bounds)
    katana = game.frames("spr_titlegraphic_big2")[0]
    katana = katana.crop(katana.getbbox())
    kw = int(zero.width * 0.60)
    katana = katana.resize((kw, int(katana.height * kw / katana.width)), Image.LANCZOS)
    top = int(katana.height * 0.45)
    logo = Image.new("RGBA", (zero.width, max(zero.height + top, katana.height)), (0, 0, 0, 0))
    logo.alpha_composite(zero, (0, top))
    logo.alpha_composite(katana, (int(zero.width * 0.20), 0))
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
    # The LiveArea background is the bare title graphic; the bubble and the start gate carry the logo
    for name, (w, h), logo_at in (
        ("icon0.png", (128, 128), (64, 64, 116)),
        ("bg0.png", (840, 500), None),
        ("startup.png", (280, 158), (140, 79, 210)),
    ):
        img = game_background(game, w, h)
        if logo_at:
            place(img, logo, *logo_at)
        img.convert("RGB").convert("P", palette=Image.ADAPTIVE, colors=256).save(os.path.join(OUT, name), optimize=True)
    print(f"LiveArea art written to {os.path.normpath(OUT)}")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: python scripts/make_livearea.py <Katana ZERO APK, XAPK or game.droid>")
    game_art(sys.argv[1])


if __name__ == "__main__":
    main()
