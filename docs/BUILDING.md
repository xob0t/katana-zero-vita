# Building from source (Windows, no WSL)

Requires [uv](https://docs.astral.sh/uv/), Git and MinGW `mingw32-make` on PATH. uv provides Python, CMake and the
Python packages (`pyproject.toml`). From the repository root, in any shell:

```
uv run build.py "<Katana ZERO APK>"
```

This builds `build/app/KatanaZero.vpk` and `build/loader/loader2.bin` (the YoYo Loader drop-in loader). The first run
also:

- installs the softfp VitaSDK into `C:/vitasdk-softfp` (pinned downloads, checked by sha256)
- builds vitaGL (with `patches/vitagl.patch`), openal-soft, vitaShaRK, SceShaccCgExt and opensles in `C:/vitasdk-build`
- draws the bubble icon, LiveArea and start gate art from the title screen art in your APK into `livearea/`. It is
  game art, so it is not committed.

Later runs skip finished steps and rebuild only what changed. The dependencies are rebuilt when a patch, a pin or a
dependency checkout changes. If only the installed libraries were replaced (another vitaGL build, say), the build
stops and asks for `--deps` or `--no-deps`. Once the art exists, the APK argument is optional.

`--game-apk` also makes `build/game.apk` (step 2 under "Installing a source build"). `--sdk` and `--deps-dir` choose other locations. `--deps`
forces a dependency rebuild, `--no-deps` leaves the installed ones alone (e.g. an experimental vitaGL), and `--check`
only reports what a run would do. On Linux, set `VITASDK` (or `--sdk`) to a softfp VitaSDK, such as the
`atamanenko/vitasdk-softfp` container CI uses, and have `make` instead of `mingw32-make`. In Git Bash,
`. scripts/env.sh` sets up the same environment to run `cmake --build build/app` or the toolchain by hand.

## Installing a source build

To install a source build without the web builder:

1. Install `build/app/KatanaZero.vpk` (title ID `KTNZ00001`). It contains only the loader.
2. Build the game APK with `uv run build.py --game-apk "<Katana ZERO 1.1.10 APK>"` and copy `build/game.apk` to
   `ux0:data/gms/katanazero/game.apk`. This also redraws the
   Xbox button prompts as Vita buttons (`--xbox-glyphs` keeps the originals). A `game.apk` inside the app, as the web
   builder makes, takes precedence over this one.

## The web builder

The web builder is `web/`. It runs `web/builder.py` and the scripts in `scripts/` under [Pyodide](https://pyodide.org),
so it makes the same `game.apk` as the command line. Pushing a version tag (`v*`) runs
`.github/workflows/web-builder.yml`, which builds the loader and publishes the page with it to GitHub Pages; pull
requests only build it, as an artifact to test first. The app version the Vita shows (`APP_VER` in `param.sfo`) comes
from the latest `v*` tag: v1.3.0 becomes 01.30 (`loader/CMakeLists.txt`). To try the page locally, copy `build/app/eboot.bin` and
`build/app/KatanaZero.vpk_param.sfo` (as `param.sfo`) to `web/base/` and serve the repository root
(`python -m http.server`), then open `/web/`.
