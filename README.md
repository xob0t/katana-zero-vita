# katana-zero-vita

PS Vita port of [Katana ZERO](https://store.steampowered.com/app/460950/Katana_ZERO/), built from the Netflix Android release (1.1.10). The game's Android runner
(`libyoyo.so`) runs on the Vita through [YoYo Loader Vita](https://github.com/Rinnegatamante/yoyoloader_vita),
vendored in `loader/` from upstream commit `e32f68f`. No game files are included. You need your own APK.

**To play, build the VPK from your APK with the [web builder](https://xob0t.github.io/katana-zero-vita/web/).**

Most rooms run at 60 fps. Heavy rooms dip below it, to about 40 in the busiest fights. The CPU is the limit: the game
logic runs on one core and can't be split across more. A 500 MHz profile in PSVshell adds a few fps in heavy rooms
(the loader runs at 444 MHz).

## Compromises

Most items below are a setting in `ux0:data/gms/katanazero/yyl.cfg`, and the value at the end of the item brings
back the original.

- **Optional 30 fps in heavy rooms.** In GameMaker the game's speed follows the frame rate, so a room that can't
  hold 60 fps plays in slow motion. The port can run such rooms at 30 fps with the game's clock doubled. They play at
  full speed, but less smoothly, and input is read 30 times a second. Pick the mode in the game's settings, under
  **GRAPHICS → FPS MODE**.
  - **NATIVE** (default). Never switches to 30, like the original game. Most rooms hold 60, and heavy ones slow down
    instead: 45 fps plays at three-quarters speed. A few of the heaviest scenes, like the dossier burning, play at
    about half speed. This mode has the smoothest picture and the game's exact timing. The slowdown is easy to live
    with, and in busy fights it gives you extra time.
  - **DYNAMIC**. A room drops to 30 after about two seconds under 50 fps and goes back to 60 when it can hold it
    again. The heaviest rooms start at 30 as soon as their opening cutscene ends. You get full speed everywhere and
    60 wherever the Vita can manage it. `fpsMode=1`.
  - **30 FPS**. Every room runs at 30. Pick it if the switching bothers you or you want the same feel and timing in
    every room. `fpsMode=2`.
- **Enemy AI at half rate.** At 60 fps each enemy thinks and moves every other frame, half of them on even frames
  and half on odd ones. Hits still land on the frame they would have. It saves 2-3 ms per frame in fights; in
  testing nobody noticed a difference. **GRAPHICS → ENEMY AI**, or `enemyHalfRate=0`.
- **Smoke updates every 4th frame.** Smoke clouds that have settled update on every 4th frame, a quarter of them on
  each frame, and burning-corpse embers do the same. It looks the same and saves about 2 ms per frame in smoky rooms.
- **No rain.** Rainy levels run about 20% faster without it. `rain=2`, or `rain=1` for half the drops.
- **No replays.** No end-of-level VHS replay and no death rewind. With them on, memory runs out after a few minutes
  in one room and heavy rooms lose about 2 fps. `replays=1` leaves it to the game's own graphics option.
- **No coloured light tint on clotheslines.** It cost about 7 fps near them. `ropeLights=1`.
- **No rim glow on club dancers.** It's about 285 extra draws per frame, 44 fps with it and 52-56 without. The light
  show stays. `dancerGlow=1`.
- **No dust in the cathedral's window light.** 41 fps with it, 59 without. `cathedralDust=1`.
- **No lamp glow in haunted_cart2 and club_0.** The coloured glow lamps and spotlights throw on walls and floors is
  off in those two rooms: 36 -> 39 fps in the minecart room, and the club's dance floor holds 60. `roomLighting=1`.
- **Lower resolution in GPU-heavy rooms.** A room where the GPU can't keep up at 960x540, so frames miss the screen
  refresh while the CPU has time to spare, drops to 640x360 until the next room. `dynamicRes=0`.
- **Coarser laser deaths.** The body is cut into 4 times fewer, 4 times thicker slices. `dissolveChunkScale=1`.
- **Thinner prison fog.** One puff in 4 is kept and drawn denser. `fogScale=1`.
- **Unlit roll dust in fog.** In the foggy rooms (the prison, the neighbour's party) the dust clouds a roll kicks up
  get one fixed pale tint instead of being lit by the lights around them. Rolling there: 57 -> 59 fps. `fogTrailLights=1`.
- **Sound limits.** At most 3 copies of one sound and 32 voices at once. Laser deaths start dozens of identical
  sounds and stalled the game mixing them. `sameSoundLimit=0` and `maxVoices=0`.
- **Lighting updates 30 times a second** in rooms that run at 60. No setting.
- **Checkpoints are saved in the background.** The game no longer waits for the memory card, which stuttered it for
  20-90 ms at every checkpoint. If the Vita crashes or loses power right after a checkpoint, that checkpoint can be
  lost and you continue from the one before; a save is never left half-written. `asyncSave=0`.
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
