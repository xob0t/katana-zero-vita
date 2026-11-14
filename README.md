# katana-zero-vita

PS Vita port of Katana ZERO, built from the Netflix Android release (1.1.10). The game's Android runner
(`libyoyo.so`) runs on the Vita through [YoYo Loader Vita](https://github.com/Rinnegatamante/yoyoloader_vita),
vendored in `loader/` from upstream commit `e32f68f`. No game files are included. You need your own APK.

**To play, build the VPK from your APK with the [web builder](https://xob0t.github.io/katana-zero-vita/web/).**

The game is playable at full speed. Light rooms run at 60 fps, heavy rooms at 30. The CPU is the limit: the game
logic runs on one core and can't be split across more. A 500 MHz profile in PSVshell adds a few fps in heavy rooms
(the loader runs at 444 MHz).

## Compromises

Most items below are a setting in `ux0:data/gms/katanazero/yyl.cfg`, and the value at the end of the item brings
back the original.

- **30 fps in heavy rooms.** GameMaker slows the whole game down instead of dropping frames, so rooms that can't hold
  60 used to play in slow motion. The minecart ride ran at 31 fps, half speed. Now a room that stays under 50 fps for
  a second and a half switches to 30 frames a second with the game's clock doubled, and the heaviest rooms hold
  27-30 fps. The cost is smoothness, and input is read 30 times a second. Some of the game's timers count frames
  instead of time, so the slow motion ramp takes 0.3 s instead of 0.16 s, and hit-stops, enemy reactions and dialogue
  typing are slightly off. Cutscenes and scripted sequences, like the tutorial, always run at 60 because they break
  at 30. `fps30=0` brings back slow motion. `fps30=2` runs 30 everywhere.
- **No rain.** Rainy levels run about 20% faster without it. `rain=2`, or `rain=1` for half the drops.
- **No replays.** No end-of-level VHS replay and no death rewind. With them on, memory runs out after a few minutes
  in one room and heavy rooms lose about 2 fps. `replays=1` leaves it to the game's own graphics option.
- **No coloured light tint on clotheslines.** It cost about 7 fps near them. `ropeLights=1`.
- **No rim glow on club dancers.** It's about 285 extra draws per frame, 44 fps with it and 52-56 without. The light
  show stays. `dancerGlow=1`.
- **No dust in the cathedral's window light.** 41 fps with it, 59 without. `cathedralDust=1`.
- **Lower resolution in the cathedral.** A room that keeps the GPU waiting at 960x540, which so far is only the
  cathedral's window light, drops to 640x360 until the next room. `dynamicRes=0`.
- **Coarser laser deaths.** The body is cut into 4 times fewer, 4 times thicker slices. `dissolveChunkScale=1`.
- **Thinner prison fog.** One puff in 4 is kept and drawn denser. `fogScale=1`.
- **Sound limits.** At most 3 copies of one sound and 32 voices at once. Laser deaths start dozens of identical
  sounds and stalled the game mixing them. `sameSoundLimit=0` and `maxVoices=0`.
- **Lighting updates 30 times a second** in rooms that run at 60. No setting.
- **Pauses at some room loads.** The game never unloads its texture pages, so over a session it runs out of video
  memory. The loader then clears them all, which costs 1-1.5 s at the next room load. No setting.

## Installing on a Vita

1. Install kubridge, FdFix and `libshacccg.suprx` on the Vita.
2. Open the [web builder](https://xob0t.github.io/katana-zero-vita/web/) and drop in the Netflix Android release of
   Katana ZERO, version 1.1.10. It builds `KatanaZero.vpk` in your browser in about a minute, with the game inside.
   Your APK isn't uploaded anywhere.
3. Copy the VPK to `ux0:VPK/` and install it with VitaShell.

The first launch compiles every shader, which takes about 3 minutes. Shaders are cached in
`ux0:data/shader_cache/KTNZ00001`, so later launches reach the title screen in about a minute. To update, build a
new VPK and install it over the old one. Saves are kept.

With `debugMode=1` in `yyl.cfg` the log goes to `ux0:data/gms/shared/yyl.log`.

### Controls

The game's own controller layout. L and R act as the triggers: L is slow motion, R is roll. The rear touch
panel is unused.

## Building (Windows, Git Bash, no WSL)

Requires `gh`, CMake, MinGW `mingw32-make` and Python with Pillow on PATH.

```bash
bash scripts/setup-sdk.sh    # softfp VitaSDK into C:/vitasdk-softfp
bash scripts/setup-deps.sh   # vitaGL (+ patches/vitagl.patch), openal-soft, vitaShaRK, SceShaccCgExt, opensles
python scripts/make_livearea.py <Katana ZERO APK>   # bubble icon, LiveArea and start gate art
bash scripts/build.sh        # build/app/KatanaZero.vpk and build/loader/loader2.bin
```

`make_livearea.py` builds the LiveArea images from the game's title screen art in your APK, into `livearea/`.
They are game art, so they are not committed, and the app build stops if they are missing.

To install a source build without the web builder:

1. Install `build/app/KatanaZero.vpk` (title ID `KTNZ00001`). It contains only the loader.
2. Build the game APK with `python scripts/prepare_apk.py <Katana ZERO 1.1.10 APK> game.apk`
   (needs `pip install lz4 pyelftools`) and copy it to `ux0:data/gms/katanazero/game.apk`. This also redraws the
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
- `loader/loader/katana.c` holds the game-specific speedups and the 30 fps mode. Its comments have the
  measurements and the `exp_*` flags that turn each change off for comparison.

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
