"""Builds the Katana ZERO Vita app and drop-in loader in one step. Run it with uv from the repository root:

    uv run build.py "<Katana ZERO APK>"

Each step runs only when needed, so running the command again just rebuilds what changed:

1. SDK: installs the softfp VitaSDK into --sdk (default C:/vitasdk-softfp on Windows) if it is not there yet.
   The loader must be built softfp to call into the Android .so. The last softfp Windows toolchain is from 2022, so
   its Vita system headers and syscall stubs are refreshed from a newer VitaSDK (these are float-ABI independent).
2. Dependencies: builds vitaGL (with patches/vitagl.patch), openal-soft, vitaShaRK, SceShaccCgExt and opensles
   (with patches/opensles.patch) in --deps-dir and installs them into the SDK, mirroring
   loader/.github/workflows/compilation.yml. Skipped while the patches, the pins below, the dependency checkouts and
   the installed libraries are unchanged since the last run (--deps forces it, --no-deps skips it).
3. LiveArea: draws the bubble icon, LiveArea and start gate art from the APK's title art into livearea/ (game art,
   so not committed) if it is missing.
4. Builds build/app (KatanaZero.vpk, eboot.bin) and build/loader (loader2.bin, the YoYo Loader drop-in).
5. With --game-apk, also makes the game.apk the loader reads (scripts/prepare_apk.py).

External tools: git and a GNU make (on Windows MinGW's mingw32-make; its recipes run in the sh.exe that comes
with Git for Windows). CMake and the Python packages come from pyproject.toml.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WINDOWS = os.name == "nt"
DEFAULT_WINDOWS_SDK = "C:/vitasdk-softfp"
DEFAULT_WINDOWS_DEPS = "C:/vitasdk-build"


def gh(repo, tag, asset):
    return f"https://github.com/{repo}/releases/download/{tag}/{asset}"


# Pinned SDK downloads: (url, sha256)
SOFTFP_TOOLCHAIN = (gh("vitasdk/buildscripts", "softfp-win-v2.228",
                       "vitasdk-x86_64-w64-mingw32-2022-08-14_17-47-24.tar.bz2"),
                    "a7c3350c520ae6f7d41bf083ea82ee417e93fb9147c14df5f63783567806f758")
# softfp builds of the usual vdpm packages. "Nightly" is a moving tag; its assets have not changed since 2022-08-27.
SOFTFP_INCLUDE = (gh("Rinnegatamante/vitasdk-packages-softfp", "Nightly", "include.tar.gz"),
                  "02bb69e6078427d86527de739b71f44e761d756abec8a1d5ac2adf75b4262e6b")
SOFTFP_LIB = (gh("Rinnegatamante/vitasdk-packages-softfp", "Nightly", "lib.tar.gz"),
              "b268184e835c5f2fa9e1525c55fcf03e3fa85424946abceb197277ba355e61c9")
# Current Vita headers and syscall stubs: the last master-win autobuild
NEW_SDK = (gh("vitasdk/autobuilds", "master-win-v2.540", "vitasdk-x86_64-w64-mingw32-2026-05-24_12-05-17.tar.bz2"),
           "dc2b8a5fb72e162e9ebc3890897d4ab57ab8ed0aff729a6aca6fe94079155b0e")
NEW_SDK_MEMBERS = ("*/arm-vita-eabi/include/psp2*", "*/arm-vita-eabi/include/vitasdk*",
                   "*/arm-vita-eabi/lib/*_stub.a", "*/arm-vita-eabi/lib/*_stub_weak.a")

# Our vitaGL changes (patches/vitagl.patch) are made against this commit
VITAGL_COMMIT = "464876a79cdd00650bb0eb9629b85d81f9908fd8"
VITAGL_FLAGS = ("DEPTH_STENCIL_HACK=1", "SOFTFP_ABI=1", "NO_DEBUG=1", "SHARED_RENDERTARGETS=1", "NO_SPLASHSCREEN=1",
                "ENABLE_LEGACY_PIPELINE=1", "HAVE_SHADER_CACHE=1")
REPOS = {
    "openal-soft": ("https://github.com/Rinnegatamante/openal-soft.git", "vgl"),
    "SceShaccCgExt": ("https://github.com/bythos14/SceShaccCgExt.git", None),
    "vitaShaRK": ("https://github.com/Rinnegatamante/vitaShaRK.git", None),
    "vitaGL": ("https://github.com/Rinnegatamante/vitaGL.git", None),
    "opensles": ("https://github.com/frangarcj/opensles.git", None),
}
# Prebuilt libraries from the repository that the loader links against
REPO_LIBS = ("loader/stubs/libkubridge_stub.a", "loader/stubs/libkubridge_stub_weak.a", "loader/libs/libsndfile.a",
             "loader/libs/libcurl.a", "loader/libs/libssl.a", "loader/libs/libcrypto.a")
# What the dependency step installs (relative to $VITASDK/arm-vita-eabi), checked to decide whether to rerun it
DEPS_INSTALLED = ("lib/libopenal.a", "lib/libSceShaccCgExt.a", "lib/libvitashark.a", "include/vitashark.h",
                  "lib/libvitaGL.a", "include/vitaGL.h", "lib/libOpenSLES.a", "include/SLES/OpenSLES.h")
DEPS_STAMP = ".katana-deps.json"
LIVEAREA = ("icon0.png", "bg0.png", "startup.png")


class BuildError(Exception):
    pass


def log(message):
    print(f"==> {message}", flush=True)


def run(cmd, *, cwd=None, env=None, check=True, quiet=False, capture=None):
    """Runs a command, echoing it. quiet hides stderr (like 2>/dev/null); capture collects stdout lines."""
    print("+ " + subprocess.list2cmdline([str(c) for c in cmd]), flush=True)
    stderr = subprocess.DEVNULL if quiet else None
    if capture is None:
        code = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, stderr=stderr).returncode
    else:
        with subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1) as proc:
            for line in proc.stdout:
                sys.stdout.write(line)
                capture.append(line.rstrip("\n"))
            code = proc.wait()
    if check and code:
        raise BuildError(f"command failed with exit code {code}: {subprocess.list2cmdline([str(c) for c in cmd])}")
    return code == 0


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# --- environment (what scripts/env.sh does in Git Bash) ---------------------------------------------------------

def find_make():
    """Path of a GNU make. MinGW's is called mingw32-make; the Makefiles call plain `make`, so a copy named make
    goes into build/tools (on PATH for the build)."""
    mingw = shutil.which("mingw32-make") if WINDOWS else None
    if mingw:
        tools = ROOT / "build" / "tools"
        shim = tools / "make.exe"
        if not shim.exists() or shim.stat().st_size != Path(mingw).stat().st_size:
            tools.mkdir(parents=True, exist_ok=True)
            shutil.copy2(mingw, shim)
        return shim
    make = shutil.which("make")
    if make:
        return Path(make)
    raise BuildError("no GNU make on PATH: install MinGW's mingw32-make (Windows) or make")


def git_sh_dir():
    """Directory of the sh.exe that comes with Git for Windows: make recipes (cp, mkdir -p, rm -rf) need a POSIX
    shell, and mingw32-make falls back to cmd.exe without one on PATH."""
    if shutil.which("sh"):
        return None
    git = shutil.which("git")
    if git:
        for base in (Path(git).resolve().parent.parent, Path(git).resolve().parent.parent.parent):
            if (base / "usr" / "bin" / "sh.exe").exists():
                return base / "usr" / "bin"
    raise BuildError("no sh.exe for make recipes: install Git for Windows (it comes with sh.exe)")


def build_env(sdk, make):
    env = dict(os.environ)
    env["VITASDK"] = sdk.as_posix()
    paths = [str(sdk / "bin"), str(make.parent)]
    if WINDOWS:
        sh = git_sh_dir()
        if sh:
            paths.append(str(sh))
    env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    env["CMAKE_GENERATOR"] = "Unix Makefiles"
    env["CMAKE_POLICY_VERSION_MINIMUM"] = "3.5"
    return env


def default_sdk():
    if WINDOWS:
        # The softfp SDK, even when VITASDK points at another (hardfp) VitaSDK
        return Path(DEFAULT_WINDOWS_SDK)
    if os.environ.get("VITASDK"):
        return Path(os.environ["VITASDK"])
    raise BuildError("set VITASDK to a softfp VitaSDK (or pass --sdk); the SDK download is for Windows only")


def default_deps_dir():
    if os.environ.get("BUILD_DIR"):
        return Path(os.environ["BUILD_DIR"])
    return Path(DEFAULT_WINDOWS_DEPS) if WINDOWS else ROOT / "build" / "deps"


# --- 1. SDK -----------------------------------------------------------------------------------------------------

def sdk_installed(sdk):
    gcc = sdk / "bin" / ("arm-vita-eabi-gcc.exe" if WINDOWS else "arm-vita-eabi-gcc")
    return (sdk / "share" / "vita.toolchain.cmake").is_file() and gcc.is_file()


def download(item, folder):
    url, digest = item
    dest = folder / url.rsplit("/", 1)[1]
    if dest.exists() and sha256(dest) == digest:
        print(f"  have {dest.name}")
        return dest
    folder.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    print(f"  downloading {url}", flush=True)
    with urllib.request.urlopen(url) as response, open(part, "wb") as out:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        for block in iter(lambda: response.read(1 << 20), b""):
            out.write(block)
            done += len(block)
            if total and sys.stdout.isatty():
                print(f"\r  {done >> 20} / {total >> 20} MB", end="", flush=True)
        print()
    actual = sha256(part)
    if actual != digest:
        part.unlink()
        raise BuildError(f"{url} has sha256 {actual}, expected {digest}: the release changed; update the pin")
    os.replace(part, dest)
    return dest


def extract(archive, dest, strip, patterns=None):
    """tar -x --strip-components=strip [--wildcards patterns]. A `*` in a pattern also matches /, as in GNU tar."""
    with tarfile.open(archive) as tar:
        members = []
        for m in tar:
            if patterns and not any(fnmatch.fnmatchcase(m.name, p) for p in patterns):
                continue
            parts = Path(m.name).parts[strip:]
            if not parts:
                continue
            if not (m.isfile() or m.isdir()):
                raise BuildError(f"{archive.name}: unexpected link or special file {m.name}")
            m.name = "/".join(parts)
            members.append(m)
        tar.extractall(dest, members=members, filter="data")


def setup_sdk(sdk, downloads):
    if sdk.exists() and any(sdk.iterdir()):
        raise BuildError(f"{sdk} exists but holds no VitaSDK; remove it or pass another --sdk")
    log(f"installing the softfp VitaSDK into {sdk}")
    files = [download(item, downloads) for item in (SOFTFP_TOOLCHAIN, SOFTFP_INCLUDE, SOFTFP_LIB, NEW_SDK)]
    staging = sdk.with_name(sdk.name + ".staging")
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    print("  extracting", flush=True)
    extract(files[0], staging, 1)
    extract(files[1], staging, 3)
    extract(files[2], staging, 3)
    new = staging / ".new-sdk"
    extract(files[3], new, 1, NEW_SDK_MEMBERS)
    shutil.copytree(new / "arm-vita-eabi" / "include", staging / "arm-vita-eabi" / "include", dirs_exist_ok=True)
    for stub in (new / "arm-vita-eabi" / "lib").glob("*.a"):
        shutil.copy2(stub, staging / "arm-vita-eabi" / "lib" / stub.name)
    shutil.rmtree(new)
    if not sdk_installed(staging):
        raise BuildError(f"the SDK in {staging} is incomplete")
    if sdk.exists():
        sdk.rmdir()
    os.replace(staging, sdk)
    log(f"SDK installed to {sdk}")


# --- 2. dependencies --------------------------------------------------------------------------------------------

def git(repo, *args, **kw):
    return run(["git", "-C", repo, *args], **kw)


def fetch(build, name):
    url, branch = REPOS[name]
    if not (build / name).is_dir():
        run(["git", "clone", "-q", "--depth", "1", *(["-b", branch] if branch else []), url, build / name])
    return build / name


def apply_patch(repo, patch):
    """Applies patch if it applies cleanly; otherwise it must already be applied."""
    if git(repo, "apply", "--check", patch, check=False, quiet=True):
        git(repo, "apply", patch)
    elif not git(repo, "apply", "--reverse", "--check", patch, check=False):
        raise BuildError(f"Patch does not apply and is not already applied: {patch}")


def vitagl_patch_state(repo, patch):
    """'apply', 'applied' or 'stale' (the tree carries changes other than the current patch)."""
    if git(repo, "apply", "--check", patch, check=False, quiet=True):
        return "apply"
    if git(repo, "apply", "--reverse", "--check", patch, check=False, quiet=True):
        return "applied"
    return "stale"


def stale_vitagl(repo):
    # Usually an older version of the patch: building on would silently leave out the patch's newer changes
    return BuildError(f"{repo.as_posix()} differs from patches/vitagl.patch; reset it with\n"
                      f"  git -C {repo.as_posix()} checkout -- . && uv run build.py")


def deps_inputs(sdk):
    files = ["patches/vitagl.patch", "patches/opensles.patch", *REPO_LIBS]
    return {"sdk": sdk.as_posix(), "vitagl_commit": VITAGL_COMMIT, "vitagl_flags": list(VITAGL_FLAGS),
            "repos": {name: list(v) for name, v in REPOS.items()},
            "files": {f: sha256(ROOT / f) for f in files}}


def deps_sources(build):
    """Commit and uncommitted changes of each dependency checkout: a local edit makes the dependency step run."""
    trees = {}
    for name in REPOS:
        repo = build / name
        if not repo.is_dir():
            trees[name] = None
            continue
        head = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], capture_output=True, text=True)
        diff = subprocess.run(["git", "-C", repo, "diff", "HEAD"], capture_output=True)
        trees[name] = [head.stdout.strip(), hashlib.sha256(diff.stdout).hexdigest()]
    return trees


def deps_outputs(sdk):
    base = sdk / "arm-vita-eabi"
    return {f: sha256(base / f) if (base / f).is_file() else None for f in DEPS_INSTALLED}


def deps_stale_reason(sdk, build):
    """None if the last dependency build used the same inputs and its installed files are unchanged."""
    try:
        stamp = json.loads((build / DEPS_STAMP).read_text())
    except (OSError, ValueError):
        return "no record of a previous dependency build"
    if stamp.get("inputs") != deps_inputs(sdk):
        return "patches, pins or prebuilt libraries changed"
    if stamp.get("sources") != deps_sources(build):
        return "dependency sources changed"
    outputs = deps_outputs(sdk)
    if None in outputs.values():
        return "installed libraries missing"
    if stamp.get("outputs") != outputs:
        return "installed libraries were replaced since the last dependency build"
    return None


def setup_deps(sdk, build, jobs, env, make_exe):
    log(f"building dependencies in {build}")
    build.mkdir(parents=True, exist_ok=True)
    lib = sdk / "arm-vita-eabi" / "lib"
    for f in REPO_LIBS:
        shutil.copy2(ROOT / f, lib)
    vita_cmake = [f"-DCMAKE_TOOLCHAIN_FILE={sdk.as_posix()}/share/vita.toolchain.cmake", "-DCMAKE_BUILD_TYPE=Release"]
    j = ["-j", str(jobs)]

    def cmake(*args):
        run(["cmake", *args], env=env)

    def make(*args):
        run([make_exe, *args], env=env)

    src = fetch(build, "openal-soft")
    cmake("-S", src, "-B", src / "build", *vita_cmake, "-DCMAKE_C_FLAGS=-mfloat-abi=softfp")
    cmake("--build", src / "build", *j, "--target", "install")

    src = fetch(build, "SceShaccCgExt")
    cmake("-S", src, "-B", src / "build")
    cmake("--build", src / "build", *j, "--target", "install")

    src = fetch(build, "vitaShaRK")
    make("-C", src, *j, "install")

    src = fetch(build, "vitaGL")
    head = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True, check=True)
    if head.stdout.strip() != VITAGL_COMMIT:
        git(src, "fetch", "-q", "--depth", "1", "origin", VITAGL_COMMIT)
        git(src, "checkout", "-q", VITAGL_COMMIT)
    patch = ROOT / "patches" / "vitagl.patch"
    state = vitagl_patch_state(src, patch)
    if state == "apply":
        git(src, "apply", patch)
        # The Makefile doesn't track headers: objects built before the patch would keep the old header code
        make("-C", src, "clean")
    elif state == "stale":
        raise stale_vitagl(src)
    make("-C", src, *VITAGL_FLAGS, *j, "install")

    src = fetch(build, "opensles")
    # Larger output grain and a high-priority playback thread: the stock 128-frame port crackles under load
    apply_patch(src, ROOT / "patches" / "opensles.patch")
    make("-C", src / "libopensles", *j, "install")

    (build / DEPS_STAMP).write_text(json.dumps({"inputs": deps_inputs(sdk), "sources": deps_sources(build),
                                                 "outputs": deps_outputs(sdk)}, indent=1))
    log("deps OK")


# --- 3-5. LiveArea, app and loader, game.apk --------------------------------------------------------------------

def missing_livearea():
    return [name for name in LIVEAREA if not (ROOT / "livearea" / name).is_file()]


def build_targets(jobs, env):
    built = []
    for folder, option in (("app", "-DKATANA_APP=1"), ("loader", "-DLOADER=1")):
        log(f"building build/{folder}")
        out = ROOT / "build" / folder
        run(["cmake", "-S", ROOT / "loader", "-B", out, option], env=env)
        lines = []
        run(["cmake", "--build", out, "-j", str(jobs)], env=env, capture=lines)
        built += [f"build/{folder}: {line[line.index('Built target'):]}" for line in lines if "Built target" in line]
    return built


def check(sdk, build, apk):
    """Reports what a build would do, changing nothing."""
    print(f"SDK {sdk}: {'installed' if sdk_installed(sdk) else 'missing, would be installed'}")
    if sdk_installed(sdk):
        reason = deps_stale_reason(sdk, build)
        print(f"dependencies {build}: {'up to date' if not reason else 'would be built: ' + reason}")
        vitagl = build / "vitaGL"
        if vitagl.is_dir():
            state = vitagl_patch_state(vitagl, ROOT / "patches" / "vitagl.patch")
            print(f"  vitaGL patch: {dict(apply='would be applied', applied='applied', stale='STALE TREE')[state]}")
    missing = missing_livearea()
    print("LiveArea art: " + ("present" if not missing else f"missing {', '.join(missing)}, "
                              + ("would be made from the APK" if apk else "NEEDS THE APK")))


def main(argv=None):
    p = argparse.ArgumentParser(description="Builds the Katana ZERO Vita app (build/app/KatanaZero.vpk) and the "
                                "YoYo Loader drop-in loader (build/loader/loader2.bin).")
    p.add_argument("apk", nargs="?", type=Path,
                   help="Katana ZERO APK (or XAPK/game.droid); needed for the LiveArea art and --game-apk")
    p.add_argument("--game-apk", action="store_true",
                   help="also make build/game.apk, the game for ux0:data/gms/katanazero/game.apk")
    p.add_argument("--game-apk-out", type=Path, metavar="PATH", help="--game-apk to another path")
    p.add_argument("--xbox-glyphs", action="store_true", help="with --game-apk: keep the Xbox button prompts")
    p.add_argument("--sdk", type=Path, help=f"softfp VitaSDK (default {DEFAULT_WINDOWS_SDK} on Windows, else $VITASDK)")
    p.add_argument("--deps-dir", type=Path,
                   help=f"dependency sources and builds (default $BUILD_DIR, else {DEFAULT_WINDOWS_DEPS} on Windows)")
    p.add_argument("--download-dir", type=Path, help="SDK downloads (default vitasdk-dl next to the SDK)")
    deps = p.add_mutually_exclusive_group()
    deps.add_argument("--deps", action="store_true", help="rebuild the dependencies even if they look up to date")
    deps.add_argument("--no-deps", action="store_true",
                      help="leave the installed dependencies as they are (e.g. an experimental vitaGL)")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("--check", action="store_true", help="only report which steps would run")
    args = p.parse_args(argv)
    if args.game_apk or args.game_apk_out:
        args.game_apk = (args.game_apk_out or ROOT / "build" / "game.apk").resolve()

    try:
        sdk = (args.sdk or default_sdk()).resolve()
        build = (args.deps_dir or default_deps_dir()).resolve()
        if args.check:
            check(sdk, build, args.apk)
            return 0
        if args.apk and not args.apk.is_file():
            raise BuildError(f"{args.apk} not found")
        missing = missing_livearea()
        if (missing or args.game_apk) and not args.apk:
            raise BuildError(f"livearea/{missing[0]} missing: pass your Katana ZERO APK to make the LiveArea art"
                             if missing else "--game-apk needs your Katana ZERO APK")
        for tool in ("git", "cmake"):
            if not shutil.which(tool):
                raise BuildError(f"{tool} not found on PATH")

        if not sdk_installed(sdk):
            if not WINDOWS:
                raise BuildError(f"no VitaSDK at {sdk}: the SDK download is for Windows; set VITASDK or --sdk")
            setup_sdk(sdk, (args.download_dir or sdk.parent / "vitasdk-dl").resolve())
        else:
            log(f"SDK {sdk}")
        make_exe = find_make()
        env = build_env(sdk, make_exe)

        reason = "--deps" if args.deps else None if args.no_deps else deps_stale_reason(sdk, build)
        if args.no_deps:
            log("dependencies left as they are (--no-deps)")
        elif reason:
            print(f"    ({reason})")
            setup_deps(sdk, build, args.jobs, env, make_exe)
        else:
            log(f"dependencies up to date ({build})")

        if missing:
            log("making the LiveArea art")
            run([sys.executable, ROOT / "scripts" / "make_livearea.py", args.apk])
        built = build_targets(args.jobs, env)

        if args.game_apk:
            log(f"making {args.game_apk}")
            args.game_apk.parent.mkdir(parents=True, exist_ok=True)
            run([sys.executable, ROOT / "scripts" / "prepare_apk.py", *(["--xbox-glyphs"] if args.xbox_glyphs else []),
                 args.apk, args.game_apk])
    except BuildError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print()
    print("\n".join(built))
    outputs = [ROOT / "build/app/KatanaZero.vpk", ROOT / "build/app/eboot.bin", ROOT / "build/loader/loader2.bin"]
    for path in outputs + ([args.game_apk] if args.game_apk else []):
        if not path.is_file() or not path.stat().st_size:
            print(f"error: {path} was not produced", file=sys.stderr)
            return 1
        print(f"{path}  {path.stat().st_size:,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
