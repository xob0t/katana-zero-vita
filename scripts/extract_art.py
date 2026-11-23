"""Extracts sprites from a GameMaker data file (game.droid / data.win) as PNG images.

Used by make_livearea.py to build the Vita LiveArea art from the user's own APK, so no game art has to be
stored in this repository.

Usage: python scripts/extract_art.py <APK, XAPK or game.droid> <out_dir> <sprite name> [...]
       python scripts/extract_art.py <APK, XAPK or game.droid> --list [substring]
"""
import bz2
import io
import os
import struct
import sys
import zipfile

from PIL import Image

FLAT_GAME = "assets/game.droid"
NESTED_GAME = "assets/assets/game.droid"
MAX_TEXTURE_SIZE = 4096  # vitaGL's GXM_TEX_MAX_SIZE


def game_data_member(names):
    """Pick the APK data member and therefore the layout of the rest of its assets."""
    return next((name for name in (NESTED_GAME, FLAT_GAME) if name in names), None)


def load_game_data(path):
    """Returns the bytes of game.droid from an APK, XAPK (APK inside a ZIP) or a plain data file."""
    if not zipfile.is_zipfile(path):
        with open(path, "rb") as data:
            return data.read()

    def find(z):
        names = z.namelist()
        member = game_data_member(names)
        if member:
            if names.count(member) != 1:
                raise ValueError(f"APK has duplicate entries for {member}")
            return z.read(member)
        if "AndroidManifest.xml" in names:
            return None
        for name in names:  # standalone data files inside a ZIP, or APKs inside an XAPK
            if name.rsplit("/", 1)[-1] in ("game.droid", "data.win"):
                return z.read(name)
        for name in names:
            if name.endswith(".apk"):
                with zipfile.ZipFile(io.BytesIO(z.read(name))) as inner:
                    data = find(inner)
                if data is not None:
                    return data
        return None

    with zipfile.ZipFile(path) as z:
        data = find(z)
    if data is not None:
        return data
    raise ValueError(f"no game.droid in {path}")


def check_texture_dimensions(width, height):
    if not (1 <= width <= MAX_TEXTURE_SIZE and 1 <= height <= MAX_TEXTURE_SIZE):
        raise ValueError(f"invalid texture dimensions {width}x{height} (Vita limit {MAX_TEXTURE_SIZE})")


# Total bytes consumed by each opcode. RUN_16 also reads its second byte to get the pixel count.
QOI_OP_SIZE = (1,) * 0x60 + (2,) * 0x20 + (1,) * 0x40 + (2,) * 0x20 + (3,) * 0x10 + tuple(
    1 + flags.bit_count() for flags in range(16)
)


def validate_gm_qoi(blob):
    """Validate GameMaker QOI and return its dimensions without allocating decoded pixels."""
    if len(blob) < 12 or blob[:4] != b"fioq":
        raise ValueError("invalid GameMaker QOI header (expected fioq)")
    width, height, length = struct.unpack_from("<HHI", blob, 4)
    check_texture_dimensions(width, height)
    end = len(blob)
    if length != end - 12:
        raise ValueError("QOI header length does not match its payload")

    # Every bit pattern is a valid opcode, including every possible index and channel delta.
    # Only byte consumption and run lengths can make the stream invalid.
    expected = width * height
    pixels, pos = 0, 12
    while pos < end:
        op = blob[pos]
        if op & 0xc0 != 0x40:
            pixels += 1
        elif op & 0x20:
            if pos + 1 == end:
                raise ValueError("truncated QOI run opcode")
            pixels += (((op & 0x1f) << 8) | blob[pos + 1]) + 33
        else:
            pixels += (op & 0x1f) + 1
        pos += QOI_OP_SIZE[op]
    if pos > end:
        raise ValueError("truncated QOI opcode")
    if pixels != expected:
        raise ValueError(f"QOI stream decodes to {pixels} pixels, expected {expected}")
    return width, height


def unpack_gm_texture(blob):
    """Return a validated QOI stream and its dimensions from a 2zoq or plain fioq page."""
    if blob[:4] != b"2zoq":
        return blob, validate_gm_qoi(blob)
    if len(blob) < 12:
        raise ValueError("truncated 2zoq header")
    width, height, size = struct.unpack_from("<HHI", blob, 4)
    check_texture_dimensions(width, height)
    if not 12 < size <= 12 + 5 * width * height:
        raise ValueError(f"invalid 2zoq uncompressed size {size}")
    payload = blob[12:]
    if payload.startswith(b"LZ4H"):
        import lz4.block

        if len(payload) < 8 or struct.unpack_from("<I", payload, 4)[0] != len(payload) - 8:
            raise ValueError("invalid 2zoq LZ4 block length")
        qoi = lz4.block.decompress(payload[8:], uncompressed_size=size)
    else:
        dec = bz2.BZ2Decompressor()
        qoi = dec.decompress(payload, max_length=size + 1)
        if not dec.eof or dec.unused_data:
            raise ValueError("invalid or truncated 2zoq bzip2 stream")
    if len(qoi) != size:
        raise ValueError("2zoq uncompressed size does not match its QOI payload")
    dimensions = validate_gm_qoi(qoi)
    if dimensions != (width, height):
        raise ValueError("2zoq dimensions do not match its QOI payload")
    return qoi, dimensions


def sx(value, bits):
    """Sign-extends a two's complement bit field."""
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def decode_gm_qoi(blob, dimensions=None):
    """Decodes GameMaker's QOI variant ("fioq": the pre-release QOI opcode set with signed diffs, BGRA order)."""
    width, height = validate_gm_qoi(blob) if dimensions is None else dimensions
    data = blob[12:]
    out = bytearray(width * height * 4)
    index = bytearray(64 * 4)
    r = g = b = 0
    a = 255
    run = pos = 0
    for px in range(0, len(out), 4):
        if run > 0:
            run -= 1
        else:
            b1 = data[pos]
            pos += 1
            if b1 & 0xc0 == 0x00:  # INDEX
                i = (b1 & 0x3f) * 4
                r, g, b, a = index[i], index[i + 1], index[i + 2], index[i + 3]
            elif b1 & 0xe0 == 0x40:  # RUN_8
                run = b1 & 0x1f
            elif b1 & 0xe0 == 0x60:  # RUN_16
                run = (((b1 & 0x1f) << 8) | data[pos]) + 32
                pos += 1
            elif b1 & 0xc0 == 0x80:  # DIFF_8: signed 2-bit r, g, b
                r = (r + sx((b1 >> 4) & 3, 2)) & 0xff
                g = (g + sx((b1 >> 2) & 3, 2)) & 0xff
                b = (b + sx(b1 & 3, 2)) & 0xff
            elif b1 & 0xe0 == 0xc0:  # DIFF_16: signed 5-bit r, 4-bit g, 4-bit b
                b2 = data[pos]
                pos += 1
                r = (r + sx(b1 & 0x1f, 5)) & 0xff
                g = (g + sx(b2 >> 4, 4)) & 0xff
                b = (b + sx(b2 & 0x0f, 4)) & 0xff
            elif b1 & 0xf0 == 0xe0:  # DIFF_24: signed 5-bit r, g, b, a
                merged = (b1 << 16) | (data[pos] << 8) | data[pos + 1]
                pos += 2
                r = (r + sx((merged >> 15) & 0x1f, 5)) & 0xff
                g = (g + sx((merged >> 10) & 0x1f, 5)) & 0xff
                b = (b + sx((merged >> 5) & 0x1f, 5)) & 0xff
                a = (a + sx(merged & 0x1f, 5)) & 0xff
            else:  # COLOR
                if b1 & 8:
                    r = data[pos]
                    pos += 1
                if b1 & 4:
                    g = data[pos]
                    pos += 1
                if b1 & 2:
                    b = data[pos]
                    pos += 1
                if b1 & 1:
                    a = data[pos]
                    pos += 1
            i = ((r ^ g ^ b ^ a) & 0x3f) * 4
            index[i:i + 4] = bytes((r, g, b, a))
        out[px:px + 4] = bytes((b, g, r, a))
    return Image.frombytes("RGBA", (width, height), bytes(out))


class GameData:
    def __init__(self, data):
        self.d = data
        self.chunks = {}
        off = 8
        while off < len(data):
            name = data[off:off + 4].decode()
            size = self.u32(off + 4)
            self.chunks[name] = off + 8
            off += 8 + size
        self.pages = {}
        self.sprites = {}
        base = self.chunks["SPRT"]
        for i in range(self.u32(base)):
            p = self.u32(base + 4 + 4 * i)
            self.sprites[self.cstr(self.u32(p))] = p

    def u32(self, off):
        return struct.unpack_from("<I", self.d, off)[0]

    def cstr(self, off):
        return self.d[off:self.d.index(b"\0", off)].decode("utf-8", "replace")

    def page(self, n):
        if n not in self.pages:
            base = self.chunks["TXTR"]
            entry = self.u32(base + 4 + 4 * n)
            length, ptr = self.u32(entry + 8), self.u32(entry + 24)
            blob = self.d[ptr:ptr + length]
            if blob[:4] in (b"2zoq", b"fioq"):
                try:
                    qoi, dimensions = unpack_gm_texture(blob)
                except (ValueError, OSError, RuntimeError) as exc:
                    raise ValueError(f"texture page {n}: {exc}") from exc
                img = decode_gm_qoi(qoi, dimensions)
            else:
                img = Image.open(io.BytesIO(blob)).convert("RGBA")
            self.pages[n] = img
        return self.pages[n]

    def frames(self, name):
        """Returns the sprite's frames as RGBA images of the sprite's full size."""
        p = self.sprites[name]
        width, height = self.u32(p + 4), self.u32(p + 8)
        if self.u32(p + 56) == 0xFFFFFFFF:  # GMS2 sprite: special version fields before the texture list
            version = self.u32(p + 60)
            tex_list = p + 76 + (4 if version >= 2 else 0) + (4 if version >= 3 else 0)
        else:
            tex_list = p + 56
        frames = []
        for i in range(self.u32(tex_list)):
            t = self.u32(tex_list + 4 + 4 * i)
            sx, sy, sw, sh, tx, ty, tw, th, bw, bh, page = struct.unpack_from("<11H", self.d, t)
            img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
            part = self.page(page).crop((sx, sy, sx + sw, sy + sh))
            if (sw, sh) != (tw, th):
                part = part.resize((tw, th), Image.NEAREST)
            img.paste(part, (tx, ty))
            frames.append(img)
        return frames


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    game = GameData(load_game_data(sys.argv[1]))
    if sys.argv[2] == "--list":
        sub = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for name in sorted(game.sprites):
            if sub in name.lower():
                print(name)
        return
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name in sys.argv[3:]:
        for i, frame in enumerate(game.frames(name)):
            frame.save(os.path.join(out, f"{name}_{i}.png"))
        print(name, len(game.frames(name)), "frames", game.frames(name)[0].size)


if __name__ == "__main__":
    main()
