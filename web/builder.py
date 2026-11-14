"""Builds KatanaZero.vpk from the user's APK, in the browser.

web/worker.js runs this under Pyodide with the scripts from scripts/ next to it, so the web build makes exactly
what `prepare_apk.py` and `make_livearea.py` make on the command line. The VPK holds the prebuilt loader
(eboot.bin, param.sfo, LiveArea template), the LiveArea art drawn from the APK and game.apk itself, which the loader
reads from app0: when it is there.
"""
import hashlib
import io
import os
import sys
import zipfile

import make_livearea
import prepare_apk

RUNNER = prepare_apk.KEEP_LIB
# Runners the port was tested with (sha256 of lib/armeabi-v7a/libyoyo.so). The loader hooks this exact build.
TESTED_RUNNERS = {
    "ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701": "1.1.10 (Netflix)",
}
VPK_FILES = (  # base file -> path in the VPK
    ("eboot.bin", "eboot.bin"),
    ("param.sfo", "sce_sys/param.sfo"),
    ("template.xml", "sce_sys/livearea/contents/template.xml"),
)
LIVEAREA = (
    ("icon0.png", "sce_sys/icon0.png"),
    ("bg0.png", "sce_sys/livearea/contents/bg0.png"),
    ("startup.png", "sce_sys/livearea/contents/startup.png"),
)


def inspect(path):
    """Checks that `path` is an APK this port can use. Returns a dict for the page."""
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        return {"ok": False, "error": "This file is not an APK."}
    problem = prepare_apk.apk_problem(set(z.namelist()))
    if problem:
        return {"ok": False, "error": problem}
    digest = hashlib.sha256()
    with z.open(RUNNER) as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    version = TESTED_RUNNERS.get(digest.hexdigest())
    result = {"ok": True, "version": version}
    if not version:
        result["warning"] = ("This APK's runner is not the one the port was tested with (Netflix release 1.1.10). "
                             "The VPK may not start.")
    return result


class _Stages(io.TextIOBase):
    """Forwards the scripts' output to the page and moves the progress bar when a step finishes."""

    def __init__(self, log, progress, stages):
        self.log, self.progress, self.stages, self.buf = log, progress, stages, ""

    def write(self, s):
        self.buf += s
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            self.log(line)
            for prefix, value, label in self.stages:
                if line.startswith(prefix):
                    self.progress(value, label)
        return len(s)


def build(apk, out, base, glyphs=True, log=print, progress=lambda value, label: None):
    """Writes the VPK to `out` from the APK `apk` and the prebuilt loader files in the folder `base`."""
    work = os.path.join(os.path.dirname(out) or ".", "work")
    os.makedirs(work, exist_ok=True)
    game_apk = os.path.join(work, "game.apk")
    stdout = sys.stdout
    sys.stdout = _Stages(log, progress, (
        ("game.droid:", 0.45, "Patching the game runner"),
        ("libyoyo.so:", 0.8, "Writing game.apk"),
        ("wrote ", 0.85, "Drawing the LiveArea art"),
        ("LiveArea", 0.9, "Packing the VPK"),
    ))
    try:
        progress(0.02, "Recompressing the textures")
        prepare_apk.prepare(apk, game_apk, glyphs)
        make_livearea.game_art(apk, work)
        files = [(os.path.join(base, src), dst) for src, dst in VPK_FILES]
        files += [(os.path.join(work, src), dst) for src, dst in LIVEAREA]
        files.append((game_apk, "game.apk"))
        with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as vpk:
            for src, dst in files:
                info = zipfile.ZipInfo(dst, (2020, 1, 1, 0, 0, 0))
                with open(src, "rb") as fi, vpk.open(info, "w", force_zip64=False) as fo:
                    for chunk in iter(lambda: fi.read(1 << 22), b""):
                        fo.write(chunk)
        log(f"{os.path.basename(out)}: {os.path.getsize(out) / 1e6:.0f} MB")
    finally:
        sys.stdout = stdout
        for name in os.listdir(work):
            os.remove(os.path.join(work, name))
        os.rmdir(work)
    progress(1.0, "Done")
