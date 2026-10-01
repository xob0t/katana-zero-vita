"""Redraws the game's Xbox button glyphs as PS Vita buttons, for prepare_apk.py.

The Android build only has Xbox, Switch and keyboard glyph sprites, and shows the Xbox set for a gamepad. The
face buttons keep their shape and shading, recoloured to a dark grey with the PlayStation symbol in place of the
letter (A -> cross, B -> circle, X -> square, Y -> triangle, matching the Vita's button positions); the shoulder
plates read L and R. Start/Select (generic icons) and the stick clicks (no Vita equivalent) stay as they are.

The patched texture page is re-encoded in GameMaker's QOI variant with only the COLOR and RUN opcodes. A page the
runner can't decode crashes the Vita's GPU, so check changes to the encoder with scripts/qoi_emulate.py, which runs
the runner's own ReadQOIFFile on the result.
"""
import struct

from extract_art import GameData

# PlayStation symbol colours
CROSS, CIRCLE, SQUARE, TRIANGLE = (124, 178, 232), (255, 102, 102), (230, 150, 200), (64, 226, 160)

SYMBOLS = {
    "cross": ("X...X", ".X.X.", "..X..", ".X.X.", "X...X"),
    "circle": (".XXX.", "X...X", "X...X", "X...X", ".XXX."),
    "square": ("XXXXX", "X...X", "X...X", "X...X", "XXXXX"),
    "triangle": ("..X..", ".X.X.", ".X.X.", "X...X", "XXXXX"),
    "L": ("X..", "X..", "X..", "X..", "XXX"),
    "R": ("XX.", "X.X", "XX.", "X.X", "X.X"),
}

# Face buttons: the letter sits on a 5x5 plate at (4, 4) from the top of the button's silhouette
FACE = {"spr_xbox_a": ("cross", CROSS), "spr_xbox_b": ("circle", CIRCLE),
        "spr_xbox_x": ("square", SQUARE), "spr_xbox_y": ("triangle", TRIANGLE)}
# Shoulder plates: letter area (x, y from the silhouette top, width, height)
SHOULDER = {"spr_xbox_lb": ("L", (5, 3, 7, 5)), "spr_xbox_rb": ("R", (5, 3, 7, 5)),
            "spr_xbox_lt": ("L", (3, 5, 6, 5)), "spr_xbox_rt": ("R", (3, 5, 6, 5))}
PLATE_FACE = (156, 156, 156, 255)  # light grey top of the shoulder plates
PLATE_INK = (63, 50, 46, 255)      # their dark letter colour


def stamp(px, x0, y0, rows, color):
    for dy, row in enumerate(rows):
        for dx, c in enumerate(row):
            if c == "X":
                px[x0 + dx, y0 + dy] = color


def silhouette_top(img):
    return img.getbbox()[1]


def face_button(img, symbol, color):
    img = img.copy()
    px = img.load()
    top = silhouette_top(img)

    def luma(p):
        return (299 * p[0] + 587 * p[1] + 114 * p[2]) // 1000

    # Grey shading relative to the top surface, so all four buttons come out alike whatever their original hue
    face_luma = luma(px[4, top + 2])
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a and (r, g, b) != (255, 255, 255):  # body (the white outline stays)
                grey = max(12, min(150, 64 + (luma((r, g, b)) - face_luma) * 3 // 5))
                px[x, y] = (grey, grey, grey + 4, a)
    face = px[4, top + 2]  # flat colour of the button's top surface
    for y in range(top + 4, top + 9):
        for x in range(4, 9):
            px[x, y] = face
    stamp(px, 4, top + 4, SYMBOLS[symbol], color + (255,))
    return img


def shoulder_plate(img, letter, area):
    img = img.copy()
    px = img.load()
    top = silhouette_top(img)
    x0, y0, w, h = area
    for y in range(top + y0, top + y0 + h):
        for x in range(x0, x0 + w):
            px[x, y] = PLATE_FACE
    stamp(px, x0 + (w - 3 + 1) // 2, top + y0, SYMBOLS[letter], PLATE_INK)
    return img


def frame_entries(game, name):
    """Texture page entries (sx, sy, sw, sh, tx, ty, tw, th, bw, bh, page) of a sprite's frames."""
    p = game.sprites[name]
    if game.u32(p + 56) == 0xFFFFFFFF:
        version = game.u32(p + 60)
        tex_list = p + 76 + (4 if version >= 2 else 0) + (4 if version >= 3 else 0)
    else:
        tex_list = p + 56
    return [struct.unpack_from("<11H", game.d, game.u32(tex_list + 4 + 4 * i)) for i in range(game.u32(tex_list))]


def patched_pages(data):
    """Returns {texture page index: RGBA page image} with the Vita glyphs drawn in."""
    game = GameData(data)
    pages = {}
    for name in list(FACE) + list(SHOULDER):
        for entry, frame in zip(frame_entries(game, name), game.frames(name)):
            sx, sy, sw, sh, tx, ty, tw, th, _, _, page = entry
            if (sw, sh) != (tw, th):
                raise ValueError(f"{name}: scaled texture entry")
            new = face_button(frame, *FACE[name]) if name in FACE else shoulder_plate(frame, *SHOULDER[name])
            if page not in pages:
                pages[page] = game.page(page).copy()
            pages[page].paste(new.crop((tx, ty, tx + tw, ty + th)), (sx, sy))
    return pages


def encode_gm_qoi(img):
    """Encodes an RGBA image as GameMaker QOI ("fioq") using only the COLOR and RUN opcodes."""
    w, h = img.size
    px = img.convert("RGBA").tobytes()
    out = bytearray()
    prev = (0, 0, 0, 255)  # decoder start state, in stream channel order (B, G, R, A of the image)
    run = 0

    def flush_run():
        nonlocal run
        while run:
            if run <= 32:  # RUN_8: n + 1 pixels
                out.append(0x40 | (run - 1))
                run = 0
            else:  # RUN_16: n + 33 pixels
                n = min(run, 33 + 0x1FFF)
                out.extend((0x60 | ((n - 33) >> 8), (n - 33) & 0xFF))
                run -= n

    for i in range(0, len(px), 4):
        cur = (px[i + 2], px[i + 1], px[i], px[i + 3])
        if cur == prev:
            run += 1
            continue
        flush_run()
        flags, values = 0, bytearray()
        for bit, c, p in zip((8, 4, 2, 1), cur, prev):
            if c != p:
                flags |= bit
                values.append(c)
        out.append(0xF0 | flags)  # COLOR with the changed channels
        out.extend(values)
        prev = cur
    flush_run()
    return b"fioq" + struct.pack("<HHI", w, h, len(out)) + bytes(out)
