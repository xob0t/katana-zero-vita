"""Builds KatanaZero.vpk from the user's APK, in the browser.

web/worker.js runs this under Pyodide with the scripts from scripts/ next to it, so the web build makes exactly
what `prepare_apk.py` and `make_livearea.py` make on the command line. The VPK holds the prebuilt loader
(eboot.bin, param.sfo, LiveArea template), the LiveArea art drawn from the APK and game.apk itself, which the loader
reads from app0: when it is there.
"""
import os
import sys
import tempfile
import zipfile

import make_livearea
import prepare_apk

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


_opened = None  # (path, temporary folder, ApkFiles) of the last APK inspect() accepted, for build()


def _close():
    global _opened
    if _opened:
        _opened[2].close()
        _opened[1].cleanup()
        _opened = None


def _open(path):
    """ApkFiles for `path`; an APK bundle is unpacked once (into the same virtual filesystem) and kept for build()."""
    global _opened
    if _opened and _opened[0] == path:
        return _opened[2]
    _close()
    work = tempfile.TemporaryDirectory()
    try:
        apk = prepare_apk.open_apk(path, work.name, log=lambda line: None)
    except BaseException:
        work.cleanup()
        raise
    _opened = (path, work, apk)
    return apk


def inspect(path):
    """Checks that `path` is an APK or APK bundle (APKM, XAPK, APKS) this port can use. Returns a dict for the page."""
    _close()  # the page mounts each chosen file at the same path, so never reuse an earlier one
    try:
        apk = _open(path)
        problem = prepare_apk.apk_problem(apk.names)
        if problem:
            _close()
            return {"ok": False, "error": problem}
        with apk.open(prepare_apk.KEEP_LIB) as runner:
            version = prepare_apk.runner_version(runner)
        if apk.paths == [path]:
            _close()  # a plain APK is cheap to open again; only an unpacked bundle is worth keeping
    except zipfile.BadZipFile:
        _close()
        return {"ok": False, "error": "This file is not an APK."}
    except ValueError as exc:
        _close()
        return {"ok": False, "error": str(exc)}
    result = {"ok": True, "version": version}
    if not version:
        result["warning"] = prepare_apk.UNKNOWN_RUNNER
    return result


def build(apk, out, base, glyphs=True, log=print, progress=lambda value, label: None):
    """Writes the VPK to `out` from the APK or APK bundle `apk` and the prebuilt loader files in the folder `base`."""
    if log is print:
        stdout = sys.stdout
        log = lambda line: print(line, file=stdout)
    stages = (
        ("game.droid:", 0.45, "Patching the game runner"),
        ("libyoyo.so:", 0.8, "Writing game.apk"),
        ("wrote ", 0.85, "Drawing the LiveArea art"),
        ("LiveArea", 0.9, "Packing the VPK"),
    )

    def stage_log(line):
        log(line)
        for prefix, value, label in stages:
            if line.startswith(prefix):
                progress(value, label)

    try:
        _build(apk, out, base, glyphs, log, progress, stage_log)
    finally:
        _close()  # frees an unpacked bundle
    progress(1.0, "Done")


def _build(apk, out, base, glyphs, log, progress, stage_log):
    # Pyodide's output and stage files live in the same virtual filesystem.
    with tempfile.TemporaryDirectory(dir=os.path.dirname(out) or ".") as work:
        game_apk = os.path.join(work, "game.apk")
        progress(0.02, "Recompressing the textures")
        files_in = _open(apk)
        prepare_apk.prepare(files_in, game_apk, glyphs, log=stage_log)
        # the art reads game.droid from the (inner) APK that holds it
        game = prepare_apk.game_data_member(files_in.names)
        make_livearea.game_art(files_in.path_of(game) if game else apk, work, log=stage_log)
        files = [(os.path.join(base, src), dst) for src, dst in VPK_FILES]
        files += [(os.path.join(work, src), dst) for src, dst in LIVEAREA]
        files.append((game_apk, "game.apk"))
        staged_vpk = os.path.join(work, "output.vpk")
        with zipfile.ZipFile(staged_vpk, "w", zipfile.ZIP_STORED) as vpk:
            for src, dst in files:
                info = zipfile.ZipInfo(dst, (2020, 1, 1, 0, 0, 0))
                with open(src, "rb") as fi, vpk.open(info, "w", force_zip64=False) as fo:
                    for chunk in iter(lambda: fi.read(1 << 22), b""):
                        fo.write(chunk)
        size = os.path.getsize(staged_vpk)
        os.replace(staged_vpk, out)
        log(f"{os.path.basename(out)}: {size / 1e6:.0f} MB")
