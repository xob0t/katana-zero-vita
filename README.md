# katana-zero-vita

PS Vita port of [Katana ZERO](https://store.steampowered.com/app/460950/Katana_ZERO/). It runs the Netflix Android
release (1.1.10). No game files are included: you need your own APK.

## [Build the game for your Vita in the browser →](https://xob0t.github.io/katana-zero-vita/web/)

The [web builder](https://xob0t.github.io/katana-zero-vita/web/) turns your APK into an installable VPK in about a
minute. Nothing to install on your computer, and your APK isn't uploaded anywhere.

1. Install kubridge, FdFix and `libshacccg.suprx` on the Vita.
2. Open the web builder, drop in the Katana ZERO APK (Netflix Android release, version 1.1.10) and save the
   `KatanaZero.vpk` it makes.
3. Copy the VPK to `ux0:VPK/` and install it with VitaShell.

The first launch prepares the graphics cache (`ux0:data/shader_cache/KTNZ00001`) and takes about 3 minutes; a
progress bar shows it. Later launches reach the title screen in about a minute. To update, build a new VPK and
install it over the old one. Saves are kept.

With `debugMode=1` in `yyl.cfg` the log goes to `ux0:data/gms/shared/yyl.log`.

## About the port

The game's Android runner (`libyoyo.so`) runs on the Vita through
[YoYo Loader Vita](https://github.com/Rinnegatamante/yoyoloader_vita), vendored in `loader/` from upstream `e32f68f`.

Most rooms run at 60 fps. A few of the heaviest dip into the 30s, and as in the original, the game slows down with
the frame rate. The Vita's CPU is the limit. To get this far, the loader patches the game in over 50 places, most of
them with identical results, and fixes vitaGL bugs that caused a freeze and a crash. A 500 MHz profile in PSVshell
adds a few fps (the loader runs at 444 MHz).

Some of the speedups change what you see or play:

- Enemies think every other frame. Hits still land on time. **GRAPHICS → ENEMY AI** turns it off.
- No end-of-level replay and no death rewind.
- No touch controls: the front touch screen does nothing.
- Simpler effects in heavy scenes: less lighting, lighter smoke, fog and explosions.

Each can be undone in `ux0:data/gms/katanazero/yyl.cfg`; [docs/compromises.md](docs/compromises.md) lists them all.

Enemies attack on the PC version's timing. The Android release delays their first attack and every shot by a few
frames, which also hides the bullet in the first room's tutorial slow-mo. **GRAPHICS → ENEMY TIMING: ANDROID**
(or `enemyTiming=1`) brings back the Android timing.

## Building (Windows, no WSL)

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

`--game-apk` also makes `build/game.apk` (step 2 below). `--sdk` and `--deps-dir` choose other locations. `--deps`
forces a dependency rebuild, `--no-deps` leaves the installed ones alone (e.g. an experimental vitaGL), and `--check`
only reports what a run would do. On Linux, set `VITASDK` (or `--sdk`) to a softfp VitaSDK, such as the
`atamanenko/vitasdk-softfp` container CI uses, and have `make` instead of `mingw32-make`. In Git Bash,
`. scripts/env.sh` sets up the same environment to run `cmake --build build/app` or the toolchain by hand.

To install a source build without the web builder:

1. Install `build/app/KatanaZero.vpk` (title ID `KTNZ00001`). It contains only the loader.
2. Build the game APK with `uv run build.py --game-apk "<Katana ZERO 1.1.10 APK>"` and copy `build/game.apk` to
   `ux0:data/gms/katanazero/game.apk`. This also redraws the
   Xbox button prompts as Vita buttons (`--xbox-glyphs` keeps the originals). A `game.apk` inside the app, as the web
   builder makes, takes precedence over this one.

The web builder is `web/`. It runs `web/builder.py` and the scripts in `scripts/` under [Pyodide](https://pyodide.org),
so it makes the same `game.apk` as the command line. Pushing a version tag (`v*`) runs
`.github/workflows/web-builder.yml`, which builds the loader and publishes the page with it to GitHub Pages; pull
requests only build it, as an artifact to test first. To try the page locally, copy `build/app/eboot.bin` and
`build/app/KatanaZero.vpk_param.sfo` (as `param.sfo`) to `web/base/` and serve the repository root
(`python -m http.server`), then open `/web/`.

## Changes to the loader

- Imports needed by runners built with NDK r26+: fortified libc, `*_l` locale functions, `pthread_rwlock_*`,
  bionic `readdir`, time functions.
- JNI object arrays hold real element pointers, so extension functions can take several strings.
- `loader/loader/netflix.c` replaces the `NetflixIntegration` extension offline. Cloud save slots are stored in
  `ux0:data/gms/<game>/assets/netflix/<slot>.sav`. Leaderboards and telemetry do nothing.
- `loader/loader/katana.c` holds the game-specific speedups. Its comments have the measurements and the `exp_*`
  flags that turn each change off for comparison.

## Acknowledgements

- Askiisoft and Devolver Digital for Katana ZERO, and Netflix for the Android release this port runs.
- Rinnegatamante for [YoYo Loader Vita](https://github.com/Rinnegatamante/yoyoloader_vita), which this port is
  built on, and for [vitaGL](https://github.com/Rinnegatamante/vitaGL),
  [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK), the Vita port of
  [OpenAL Soft](https://github.com/Rinnegatamante/openal-soft) and the softfp VitaSDK packages.
- TheFloW for the original .so loader that YoYo Loader grew from, and for
  [kubridge](https://github.com/TheOfficialFloW/kubridge), [FdFix](https://github.com/TheOfficialFloW/FdFix) and
  VitaShell.
- JohnnyonFlame for GMLoader, which YoYo Loader uses as a reference.
- bythos14 for [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) and frangarcj for
  [OpenSL ES](https://github.com/frangarcj/opensles) on the Vita.
- The [VitaSDK](https://vitasdk.org) team, and Team Molecule for HENkaku and Ensō.
- [Pyodide](https://pyodide.org), Pillow, python-lz4 and pyelftools, which run the web builder.
