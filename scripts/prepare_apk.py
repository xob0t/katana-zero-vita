"""Turns a Katana ZERO Netflix Android APK into the game.apk the Vita loader expects.

- keeps only what the loader needs: AndroidManifest.xml, the armeabi-v7a runner and assets
- moves the Netflix build's assets/assets/* up to assets/*, where the runner looks for them
- deflates the two entries the loader reads first (DEFLATED below) and stores everything else. The Vita's memory card
  reads only ~11 MB/s, and the loader inflates these while it reads them, on other cores (loader/loader/runner_inflate.c,
  apk_prefetch.c): the runner (80 MB -> ~26 MB) loads in ~2.9 s instead of ~6.3 s, game.droid shrinks from 75 to
  ~51 MB. The audio groups, OGG songs and the rest barely compress and stay stored
- recompresses game.droid texture pages from bzip2 to LZ4 (bzip2 decoding on the Vita CPU makes
  startup very slow). Pages stay in the runner's "2zoq" container with an "LZ4H"-marked payload,
  which the loader decodes by replacing BZ2_bzBuffToBuffDecompress (loader/loader/katana.c). Plain QOI
  would decode fastest but keeps ~25 MB more of game.droid resident in RAM. Needs lz4 (pyproject.toml).
- redraws the Xbox button glyphs as PS Vita buttons (scripts/vita_glyphs.py; --xbox-glyphs keeps them)
- rewrites the runner's PLT calls into direct branches (scripts/patch_plt_calls.py)

Usage: python scripts/prepare_apk.py [--xbox-glyphs] <input.apk> <output.apk>
"""
import hashlib
import io
import os
import struct
import sys
import tempfile
import zipfile
from collections import Counter

import lz4.block

import patch_plt_calls
from extract_art import FLAT_GAME, NESTED_GAME, game_data_member, unpack_gm_texture, validate_gm_qoi
from vita_glyphs import encode_gm_qoi, patched_pages

KEEP_LIB = "lib/armeabi-v7a/libyoyo.so"
DEFLATED = {KEEP_LIB, FLAT_GAME}
MANIFEST = "AndroidManifest.xml"
REQUIRED_OUTPUT = {MANIFEST, FLAT_GAME, KEEP_LIB}
TEXTURE_ALIGN = 128
TESTED_RUNNERS = {
    "ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701": "1.1.10 (Netflix)",
}
UNKNOWN_RUNNER = ("This APK's runner is not the one the port was tested with (Netflix release 1.1.10). "
                  "The VPK may not start.")


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def parse_chunks(data):
    if len(data) < 8 or data[:4] != b"FORM" or u32(data, 4) != len(data) - 8:
        raise ValueError("not a GameMaker data file")
    chunks, off = [], 8
    while off < len(data):
        if off + 8 > len(data):
            raise ValueError(f"truncated GameMaker chunk header at {off:#x}")
        name = data[off:off + 4].decode("ascii", errors="replace")
        size = u32(data, off + 4)
        if off + 8 + size > len(data):
            raise ValueError(f"{name} chunk extends past the end of game.droid")
        chunks.append((name, off, size))
        off += 8 + size
    return chunks


def texture_entries(data, start, end):
    if start + 4 > end:
        raise ValueError("truncated TXTR page count")
    count = u32(data, start)
    table_end = start + 4 + 4 * count
    if not count or table_end > end:
        raise ValueError("invalid TXTR page count")
    entries = []
    for n in range(count):
        entry = u32(data, start + 4 + 4 * n)
        if not table_end <= entry <= end - 28:
            raise ValueError(f"texture page {n}: entry outside TXTR")
        length, ptr = u32(data, entry + 8), u32(data, entry + 24)
        if ptr < table_end or length < 12 or ptr + length > end:
            raise ValueError(f"texture page {n}: blob outside TXTR or truncated")
        if data[ptr:ptr + 4] not in (b"2zoq", b"fioq"):
            raise ValueError(f"texture page {n}: expected 2zoq or fioq header")
        entries.append((entry, ptr, length))
    first_blob = min(ptr for _, ptr, _ in entries)
    for n, (entry, _, _) in enumerate(entries):
        if entry + 28 > first_blob:
            raise ValueError(f"texture page {n}: entry overlaps texture data")
    for start_index, length_index in ((0, None), (1, 2)):
        prev_end = 0
        for n, entry in sorted(enumerate(entries), key=lambda item: item[1][start_index]):
            offset = entry[start_index]
            if offset < prev_end:
                raise ValueError(f"texture page {n}: overlapping TXTR ranges")
            prev_end = offset + (28 if length_index is None else entry[length_index])
    return entries


def recompress_textures(data, vita_glyphs=True):
    """Returns game.droid with the bzip2 payload of 2zoq texture pages replaced by LZ4 (and the Vita glyphs)."""
    chunks = parse_chunks(data)
    names = [c[0] for c in chunks]
    if names[-2:] != ["TXTR", "AUDO"]:
        raise ValueError(f"unexpected chunk order: {names[-2:]}")
    if names.count("TXTR") != 1 or names.count("AUDO") != 1:
        raise ValueError("duplicate TXTR or AUDO chunk in game.droid")
    _, txtr_off, _ = chunks[-2]
    _, audo_off, audo_size = chunks[-1]
    entries = texture_entries(data, txtr_off + 8, audo_off)
    if audo_size < 4 or 4 + 4 * u32(data, audo_off + 8) > audo_size:
        raise ValueError("invalid AUDO entry table")
    pages = patched_pages(data) if vita_glyphs else {}
    blobs = []
    for n, (_, ptr, length) in enumerate(entries):
        blob = data[ptr:ptr + length]
        try:
            qoi, dimensions = unpack_gm_texture(blob)
            if n in pages:
                qoi = encode_gm_qoi(pages[n])
                if validate_gm_qoi(qoi) != dimensions:
                    raise ValueError("patched glyphs changed the texture dimensions")
        except (ValueError, OSError, RuntimeError) as exc:
            raise ValueError(f"texture page {n}: {exc}") from exc
        if blob[:4] == b"2zoq":
            # The runner allocates from the uncompressed QOI size in the header.
            # Payload: "LZ4H", u32 block size (the runner passes the whole blob length), LZ4 block
            block = lz4.block.compress(qoi, mode="high_compression", compression=12, store_size=False)
            blob = blob[:8] + struct.pack("<I4sI", len(qoi), b"LZ4H", len(block)) + block
        else:
            blob = qoi
        blobs.append(blob)

    first_blob = min(ptr for _, ptr, _ in entries)
    out = bytearray(data[:first_blob])
    for (entry, _, _), blob in zip(entries, blobs):
        while len(out) % TEXTURE_ALIGN:
            out.append(0)
        struct.pack_into("<I", out, entry + 24, len(out))
        struct.pack_into("<I", out, entry + 8, len(blob))
        out += blob
    old_blob_end = max(ptr + length for _, ptr, length in entries)
    out += data[old_blob_end:audo_off]
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


def patch_runner(payload, log=print):
    """Rewrites the runner's PLT calls (scripts/patch_plt_calls.py) and returns the patched bytes."""
    if runner_version(io.BytesIO(payload)) is None:
        log(UNKNOWN_RUNNER)
    data, lines = patch_plt_calls.patch(payload)
    log("libyoyo.so: " + lines[0])
    return data


def runner_version(runner):
    digest = hashlib.sha256()
    for chunk in iter(lambda: runner.read(1 << 20), b""):
        digest.update(chunk)
    return TESTED_RUNNERS.get(digest.hexdigest())


def apk_entries(names, game):
    nested = game == NESTED_GAME
    entries = {}
    for name in names:
        if name.endswith("/"):
            continue
        if name in (MANIFEST, KEEP_LIB):
            entries[name] = name
        elif nested and name.startswith("assets/assets/"):
            entries["assets/" + name.removeprefix("assets/assets/")] = name
        elif name in ("assets/splash.png", "assets/portrait_splash.png") or (
                not nested and name.startswith("assets/") and not name.startswith("assets/assets/")):
            entries.setdefault(name, name)
    return entries


def apk_problem(names):
    """Returns why an APK with these entry names can't be used, or None. Shared with the web builder."""
    if KEEP_LIB not in names:
        if any(n.endswith(".apk") for n in names):
            return ("This is an APK bundle (XAPK, APKM or APKS), split by device. Use a single APK that includes "
                    "the 32-bit runner, lib/armeabi-v7a/libyoyo.so.")
        if "lib/arm64-v8a/libyoyo.so" in names:
            return ("This APK only has the 64-bit runner (lib/arm64-v8a); APKs copied off a 64-bit phone often do. "
                    "The Vita needs the 32-bit one, lib/armeabi-v7a/libyoyo.so, so use a universal APK.")
        if not any(n.startswith("lib/") for n in names):
            return ("This APK has no runner library. It is probably the base APK of a split install, which keeps "
                    "the runner in a separate APK. Use a single APK that includes lib/armeabi-v7a/libyoyo.so.")
        return "This APK has no 32-bit ARM runner, lib/armeabi-v7a/libyoyo.so."
    game = game_data_member(names)
    if game is None:
        return "This APK has no game data (assets/game.droid). Is it Katana ZERO?"
    entries = apk_entries(names, game)
    missing = REQUIRED_OUTPUT - entries.keys()
    if missing:
        return "This APK is missing required files: " + ", ".join(sorted(missing))
    counts = Counter(names)
    for name in entries.values():
        if counts[name] > 1:
            return f"This APK has duplicate entries for {name}."
    return None


def prepare(src, dst, vita_glyphs=True, *, log=print):
    """Writes the loader's game.apk to `dst` (a path or a writable file) from the Android APK `src`."""
    with zipfile.ZipFile(src) as zi:
        names = zi.namelist()
        problem = apk_problem(names)
        if problem:
            raise ValueError(problem)
        entries = apk_entries(names, game_data_member(names))
        with zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED) as zo:
            for out_name, in_name in sorted(entries.items()):
                payload = zi.read(in_name)
                if out_name == KEEP_LIB:
                    payload = patch_runner(payload, log)
                if out_name == FLAT_GAME:
                    before = len(payload)
                    payload = recompress_textures(payload, vita_glyphs)
                    log(f"game.droid: {before / 1e6:.1f}MB -> {len(payload) / 1e6:.1f}MB (textures recompressed to LZ4"
                        + (", Vita button glyphs)" if vita_glyphs else ")"))
                if out_name in DEFLATED:
                    zo.writestr(zipfile.ZipInfo(out_name, (2020, 1, 1, 0, 0, 0)), payload,
                                compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
                else:
                    zo.writestr(zipfile.ZipInfo(out_name, (2020, 1, 1, 0, 0, 0)), payload)
    log(f"wrote {dst} ({len(entries)} entries)")


def main():
    args = sys.argv[1:]
    vita_glyphs = "--xbox-glyphs" not in args
    args = [a for a in args if a != "--xbox-glyphs"]
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, dst = args
    with tempfile.TemporaryDirectory(dir=os.path.dirname(dst) or ".") as work:
        temporary = os.path.join(work, os.path.basename(dst))
        try:
            prepare(src, temporary, vita_glyphs)
            os.replace(temporary, dst)
        except (ValueError, OSError, zipfile.BadZipFile) as exc:
            raise SystemExit(f"Could not prepare {dst}: {exc}") from exc


if __name__ == "__main__":
    main()
