"""Turns a Katana ZERO Netflix Android APK into the game.apk the Vita loader expects.

- keeps only what the loader needs: AndroidManifest.xml, the armeabi-v7a runner and assets
- moves the Netflix build's assets/assets/* up to assets/*, where the runner looks for them
- stores every entry uncompressed so nothing is inflated at load time
- recompresses game.droid texture pages from bzip2 to LZ4 (bzip2 decoding on the Vita CPU makes
  startup very slow). Pages stay in the runner's "2zoq" container with an "LZ4H"-marked payload,
  which the loader decodes by replacing BZ2_bzBuffToBuffDecompress (loader/loader/katana.c). Plain QOI
  would decode fastest but keeps ~25 MB more of game.droid resident in RAM. Needs `pip install lz4`.
- redraws the Xbox button glyphs as PS Vita buttons (scripts/vita_glyphs.py; --xbox-glyphs keeps them)
- rewrites the runner's PLT calls into direct branches (scripts/patch_plt_calls.py)

Usage: python scripts/prepare_apk.py [--xbox-glyphs] <input.apk> <output.apk>
"""
import bz2

import lz4.block
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

from vita_glyphs import encode_gm_qoi, patched_pages

KEEP_LIB = "lib/armeabi-v7a/libyoyo.so"
TEXTURE_ALIGN = 128


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def parse_chunks(data):
    if data[:4] != b"FORM":
        raise ValueError("not a GameMaker data file")
    chunks, off = [], 8
    while off < len(data):
        name = data[off:off + 4].decode()
        size = u32(data, off + 4)
        chunks.append((name, off, size))
        off += 8 + size
    return chunks


def recompress_textures(data, vita_glyphs=True):
    """Returns game.droid with the bzip2 payload of 2zoq texture pages replaced by LZ4 (and the Vita glyphs)."""
    pages = patched_pages(data) if vita_glyphs else {}
    chunks = parse_chunks(data)
    names = [c[0] for c in chunks]
    if names[-2:] != ["TXTR", "AUDO"]:
        raise ValueError(f"unexpected chunk order: {names[-2:]}")
    _, txtr_off, txtr_size = chunks[-2]
    _, audo_off, audo_size = chunks[-1]
    base = txtr_off + 8

    count = u32(data, base)
    entries = [u32(data, base + 4 + 4 * i) for i in range(count)]
    # Texture entry: ..., +8 blob length, ..., +24 blob pointer
    blobs = []
    for n, e in enumerate(entries):
        length, ptr = u32(data, e + 8), u32(data, e + 24)
        blob = data[ptr:ptr + length]
        if n in pages and blob[:4] != b"2zoq":
            raise ValueError(f"texture page {n} is not a 2zoq page")
        if blob[:4] == b"2zoq":
            qoi = bz2.decompress(blob[12:])
            if qoi[:4] != b"fioq" or u32(blob, 8) != len(qoi):
                raise ValueError("bzip2 payload is not a QOI image")
            if n in pages:
                qoi = encode_gm_qoi(pages[n])
                blob = blob[:8] + struct.pack("<I", len(qoi)) + blob[12:]
            # Header (magic, width, height, QOI size) is kept; the runner allocates from the size
            # Payload: "LZ4H", u32 block size (the runner passes the whole blob length), LZ4 block
            block = lz4.block.compress(qoi, mode="high_compression", compression=12, store_size=False)
            blob = blob[:12] + b"LZ4H" + struct.pack("<I", len(block)) + block
        blobs.append(blob)

    first_blob = min(u32(data, e + 24) for e in entries)
    out = bytearray(data[:first_blob])
    for e, blob in zip(entries, blobs):
        while len(out) % TEXTURE_ALIGN:
            out.append(0)
        struct.pack_into("<I", out, e + 24, len(out))
        struct.pack_into("<I", out, e + 8, len(blob))
        out += blob
    txtr_end = txtr_off + 8 + txtr_size
    out += data[len(out):txtr_end] if len(out) < txtr_end else b""
    while len(out) % 4:
        out.append(0)
    struct.pack_into("<I", out, txtr_off + 4, len(out) - (txtr_off + 8))

    # AUDO follows TXTR; its entry table holds absolute offsets that move with it
    new_audo_off = len(out)
    shift = new_audo_off - audo_off
    audo = bytearray(data[audo_off:audo_off + 8 + audo_size])
    audo_count = u32(audo, 8)
    for i in range(audo_count):
        p = 12 + 4 * i
        struct.pack_into("<I", audo, p, u32(audo, p) + shift)
    out += audo
    struct.pack_into("<I", out, 4, len(out) - 8)
    return bytes(out)


def patch_runner(payload):
    """Runs patch_plt_calls.py on the runner and returns the patched bytes."""
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "patch_plt_calls.py")
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, "in.so"), os.path.join(tmp, "out.so")
        open(src, "wb").write(payload)
        out = subprocess.run([sys.executable, script, src, dst], check=True, capture_output=True, text=True).stdout
        print("libyoyo.so: " + out.splitlines()[0])
        return open(dst, "rb").read()


def main():
    args = sys.argv[1:]
    vita_glyphs = "--xbox-glyphs" not in args
    args = [a for a in args if a != "--xbox-glyphs"]
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, dst = args
    with zipfile.ZipFile(src) as zi:
        names = set(zi.namelist())
        nested = any(n.startswith("assets/assets/") for n in names)
        entries = {}
        for info in zi.infolist():
            n = info.filename
            if n == "AndroidManifest.xml" or n == KEEP_LIB:
                entries[n] = n
            elif nested and n.startswith("assets/assets/"):
                entries["assets/" + n[len("assets/assets/"):]] = n
            elif n in ("assets/splash.png", "assets/portrait_splash.png") or (not nested and n.startswith("assets/")):
                entries.setdefault(n, n)
        if KEEP_LIB not in entries or "assets/game.droid" not in entries:
            raise SystemExit("input is missing the armeabi-v7a runner or game.droid")

        with zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED) as zo:
            for out_name, in_name in sorted(entries.items()):
                payload = zi.read(in_name)
                if out_name == KEEP_LIB:
                    payload = patch_runner(payload)
                if out_name == "assets/game.droid":
                    before = len(payload)
                    payload = recompress_textures(payload, vita_glyphs)
                    print(f"game.droid: {before / 1e6:.1f}MB -> {len(payload) / 1e6:.1f}MB (textures recompressed to LZ4"
                          + (", Vita button glyphs)" if vita_glyphs else ")"))
                zo.writestr(zipfile.ZipInfo(out_name, (2020, 1, 1, 0, 0, 0)), payload)
    print(f"wrote {dst} ({len(entries)} entries)")


if __name__ == "__main__":
    main()
