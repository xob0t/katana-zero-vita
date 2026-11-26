#!/usr/bin/env python3
"""Off-device checks of the loader's C code that the compiler alone doesn't make. Needs the Vita toolchain.

    python scripts/check_loader.py [--build build/app] [--only dups,lto,testbuilds]

- dups: no global symbol is defined by two of the app's objects. The link uses --allow-multiple-definition
  (CMakeLists.txt, from upstream), which silently merges such symbols into one: two modules then share a variable
  (crt_ab_off in collision_round_trip.c and crt_native.c did, until Nov 2026). The objects are the ones the link
  uses (CMakeFiles/YoYoLoader.dir/objects1.rsp), so run it after a build. Intended duplicates go in ALLOWED_DUPLICATES
  with the reason; there are none today (the app links without --allow-multiple-definition).
- lto: every loader/loader/*.c (as the CMake glob, minus qoi_decoder.c) is compiled with -flto and the objects are
  partially linked (-r) with -Wlto-type-mismatch, which compares every extern declaration with its definition across
  files: the block-scope `extern int x;` in one file against `double x = ...` in another, say. gamepad.c is compiled
  without -flto (NON_LTO), so its symbols aren't compared. Known mismatches go in ALLOWED_LTO.
- testbuilds: each host test build of a native (TEST_BUILDS, see tools/verify/README.md) compiles (-fsyntax-only).

Exit status 1 if any check fails. The toolchain is $VITASDK/bin (else PATH, else C:/vitasdk-softfp/bin).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "loader" / "loader"
PREFIX = "arm-vita-eabi-"
# Compiled without -flto: main.c declares `extern void *GetPlatformInstance` to take the address of gamepad.c's function
# (an upstream trick) and LTO refuses to merge the two. Leaving gamepad.c out keeps main.c's externs checked.
NON_LTO = "gamepad.c"

# symbol -> why two objects define it on purpose
ALLOWED_DUPLICATES = {}

# symbol -> why its declarations differ on purpose
ALLOWED_LTO = {
    # so_util.c (upstream) calls vglGetProcAddress without a declaration (implicit int); same register on ARM
    "vglGetProcAddress": "implicit declaration in upstream so_util.c",
}

# (define, file, extra flags): the host test builds (tools/verify/README.md)
TEST_BUILDS = [
    ("COLLISION_STUB", "collision_native.c", []),
    ("DUSTSPAWNER_TEST", "dustspawner_native.c", []),
    ("FLASH_MASK_STUB", "flash_mask.c", []),
    ("LASER_TEST", "laser_native.c", []),
    ("LETTER_TEST", "letter_native.c", []),
    ("LIGHT_FAST_STUB", "light_fast_inline.c", []),
    ("PARTICLE_NATIVE_TEST", "particle_native.c", []),
    ("RAIL_TEST", "minecart_native.c", []),
    ("RECOLOR_LIGHTS_TEST", "recolor_lights.c", ["-DBUG(n)=0"]),  # the harness defines BUG(n), its fault injection
    ("ROPE_STUB", "rope_fast.c", []),
    ("SMOKEDRAW_TEST", "smokedraw_native.c", []),
    ("VIRTUAL_CAPTURE_TEST", "virtual_capture.c", []),
]

# The app's compile flags that matter for parsing and types (CMakeLists.txt CMAKE_C_FLAGS, KATANA_APP)
CFLAGS = ["-mfloat-abi=softfp", "-D_GNU_SOURCE", '-DAPP_GAME_NAME="katanazero"', "-Wno-format-overflow"]


def tool(name):
    for d in ([Path(os.environ["VITASDK"]) / "bin"] if os.environ.get("VITASDK") else []) + [None,
                                                                                              Path("C:/vitasdk-softfp/bin")]:
        if d is None:
            found = shutil.which(PREFIX + name)
            if found:
                return found
        else:
            for cand in (d / (PREFIX + name), d / (PREFIX + name + ".exe")):
                if cand.exists():
                    return str(cand)
    sys.exit(f"{PREFIX}{name} not found: set VITASDK")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def check_dups(build):
    rsp = build / "CMakeFiles" / "YoYoLoader.dir" / "objects1.rsp"
    if not rsp.exists():
        return [f"{rsp} missing: build first"]
    objects = [build / o for o in rsp.read_text().split()]
    nm = tool("nm")
    owners = {}
    for obj in objects:
        out = run([nm, "-g", "--defined-only", str(obj)]).stdout
        for line in out.splitlines():
            parts = line.split()
            # strong definitions only: code, data, bss, read-only, common (W/V weak ones may repeat)
            if len(parts) == 3 and parts[1] in "TDBRC":
                owners.setdefault(parts[2], []).append(obj.name)
    problems = [f"{sym} is defined in {', '.join(files)}" for sym, files in sorted(owners.items())
                if len(files) > 1 and sym not in ALLOWED_DUPLICATES]
    print(f"dups: {len(objects)} objects, {len(owners)} global symbols, {len(problems)} defined twice")
    return problems


def sources():
    return sorted(p for p in SRC.glob("*.c") if p.name != "qoi_decoder.c")


def check_lto(jobs):
    gcc = tool("gcc")
    with tempfile.TemporaryDirectory() as d:
        def compile_one(src):
            lto = [] if src.name == NON_LTO else ["-flto"]
            out = Path(d) / (src.name + ".o")
            r = run([gcc, *CFLAGS, "-O1", "-w", *lto, "-c", str(src), "-o", str(out)])
            return src.name, r.returncode, r.stderr, out
        with ThreadPoolExecutor(jobs) as ex:
            results = list(ex.map(compile_one, sources()))
        failed = [f"{name}: compile failed\n{err}" for name, rc, err, _ in results if rc]
        if failed:
            return failed
        r = run([gcc, "-mfloat-abi=softfp", "-flto", "-r", "-nostdlib", "-Wlto-type-mismatch", "-o",
                 str(Path(d) / "all.o"), *[str(o) for *_, o in results]])
        text = re.sub(r"\x1b\[[0-9;]*[mK]", "", r.stderr)
    problems = []
    for m in re.finditer(r"warning: type of '([^']+)' does not match original declaration.*?(?=\n\S|\Z)", text, re.S):
        if m.group(1) not in ALLOWED_LTO:
            problems.append(m.group(0).strip())
    if r.returncode and not problems:
        problems.append("LTO partial link failed:\n" + text[-3000:])
    print(f"lto: {len(results)} files, {len(problems)} extern type mismatches")
    return problems


def check_test_builds(jobs):
    gcc = tool("gcc")

    def one(entry):
        define, name, extra = entry
        r = run([gcc, "-mfloat-abi=softfp", "-fsyntax-only", "-Wall", "-Werror=implicit-function-declaration",
                 "-fdiagnostics-color=never", f"-D{define}", *extra, f"-I{ROOT / 'tools' / 'verify'}", str(SRC / name)])
        return define, name, r.returncode, r.stderr
    with ThreadPoolExecutor(jobs) as ex:
        results = list(ex.map(one, TEST_BUILDS))
    problems = [f"-D{d} {n}: doesn't compile\n{err}" for d, n, rc, err in results if rc]
    print(f"testbuilds: {len(results)} builds, {len(problems)} failed")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", default=str(ROOT / "build" / "app"), help="the app's CMake build directory")
    ap.add_argument("--only", default="dups,lto,testbuilds")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    checks = {"dups": lambda: check_dups(Path(args.build)), "lto": lambda: check_lto(args.jobs),
              "testbuilds": lambda: check_test_builds(args.jobs)}
    problems = []
    for name in args.only.split(","):
        problems += [f"[{name}] {p}" for p in checks[name]()]
    for p in problems:
        print(p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
