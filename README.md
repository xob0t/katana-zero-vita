# katana-zero-vita

PS Vita port of Katana ZERO, built from the Netflix Android release (1.1.10). The game's Android runner
(`libyoyo.so`) runs on the Vita through [YoYo Loader Vita](https://github.com/Rinnegatamante/yoyoloader_vita),
vendored in `loader/` from upstream commit `e32f68f`. No game files are included: you need your own APK.

## Current state

The game is playable at full speed.

- **Light rooms run at 60 fps**, the first level included.
- **Heavy rooms run at 30 fps.** These rooms can't hold 60 on the Vita, and GameMaker slows the whole game down
  instead of dropping frames, so they used to play in slow motion. The minecart ride ran at 31 fps, half speed. Now a
  room that stays under 50 fps for a second and a half switches to 30 frames a second with the game's clock doubled.
  The heaviest rooms hold 27-30 fps, so they play at 90-100% speed.
- **The CPU is the limit**, not the GPU. The game logic runs on one core and can't be split across more. The cheap
  wins are done. What's left would mean weeks of work for about 10% more speed.
- **Overclocking helps a little.** The loader runs the CPU at 444 MHz. A 500 MHz profile in PSVshell adds a few fps
  in heavy rooms.
- **Loading takes a while.** The first launch spends about 3 minutes compiling shaders, later launches about 1
  minute. A room load can pause for 1-1.5 s when texture memory is cleared.

Getting here cost some effects and some accuracy. See [compromises](#compromises).

## Compromises

Each one is a setting in `ux0:data/gms/katanazero/yyl.cfg`. The value after the description brings back the original.

- **30 fps in heavy rooms.** Less smooth, and input is read 30 times a second. Some of the game's timers count frames
  instead of time, so they run differently at 30. The slow motion ramp takes 0.3 s instead of 0.16 s, and hit-stops,
  enemy reactions and dialogue typing are slightly off. Cutscenes and scripted sequences, like the tutorial, always
  run at 60 because they break at 30. `fps30=0` brings back slow motion. `fps30=2` runs 30 everywhere.
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
- **Lighting updates 30 times a second** in rooms that run at 60. This one has no setting. The `exp_lighting60`
  test flag turns it off, see [performance](#performance).

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

## Installing on a Vita

1. Install kubridge, FdFix and `libshacccg.suprx`.
2. Install `build/app/KatanaZero.vpk` (title ID `KTNZ00001`). It contains only the loader.
3. Build the game APK with `python scripts/prepare_apk.py <Katana ZERO 1.1.10 APK> game.apk`
   (needs `pip install lz4 pyelftools`) and copy it to `ux0:data/gms/katanazero/game.apk`. This also redraws the
   Xbox button prompts as Vita buttons (`--xbox-glyphs` keeps the originals).
4. The first launch compiles every shader, which takes about 3 minutes. Shaders are cached in
   `ux0:data/shader_cache/KTNZ00001`, so later launches reach the title screen in about a minute.

Settings are in `ux0:data/gms/katanazero/yyl.cfg`, see [compromises](#compromises). With `debugMode=1` the log
goes to `ux0:data/gms/shared/yyl.log`.

### Controls

The game's own controller layout. L and R act as the triggers: L is slow motion, R is roll. The rear touch
panel is unused.

## Changes to the loader

- Imports needed by runners built with NDK r26+: fortified libc, `*_l` locale functions, `pthread_rwlock_*`,
  bionic `readdir`, time functions.
- JNI object arrays hold real element pointers, so extension functions can take several strings.
- `loader/loader/netflix.c` replaces the `NetflixIntegration` extension offline. Cloud save slots are stored in
  `ux0:data/gms/<game>/assets/netflix/<slot>.sav`; leaderboards and telemetry do nothing.

## Performance

The game runs CPU-bound on the Vita, and GameMaker games slow down below 60 fps rather than skip frames. The
first level runs at ~59.5 fps. Heavy combat rooms run at roughly 32-44 at 60 Hz, which is why they switch to 30 fps
mode. The source comments have the measurements.

- The game thread has CPU core 1 to itself; on core 0 it loses ~2 ms per frame to system work.
- Texture pages are LZ4-compressed inside the runner's QOI container (`prepare_apk.py`) and decoded natively.
- The application surface is the output size (960x540) instead of 1280x720. Lighting refreshes at 30 Hz,
  frames are paced by vblank, and large texture pages get their own VRAM region.
- When VRAM runs out, all texture pages are flushed and reloaded. The game never unloads its 16 MB pages, so
  over a session new pages either fail to load (sprites draw black) or land in main RAM, where the GPU reads
  them much more slowly. The flush costs a 1-1.5 s pause at a room load.
- Off-screen clotheslines are paused; the rope solver and bounding boxes run as native code.
- Hot GML is replaced with exact native versions (same results, RNG use and collision queries, with the original
  code as fallback): controller input, minecart and rails, ceiling lasers, dialogue letters.
- Render-only work is cut: flashlight masks outside the screen are skipped, and enemy surface captures are
  replayed directly instead of drawn into a 70x70 surface each (small resampling differences possible).

To compare against the original, create an empty file `ux0:data/gms/shared/exp_<name>`:

| Flag | Turns off |
| --- | --- |
| `gamecore0` | game thread on core 1 (uses core 0) |
| `vcapture0`, `vcapexact` | replayed enemy captures (`vcapexact`: pixel-exact replay) |
| `inputnative0`, `lasernative0`, `letternative0`, `minecartnative0` | native input, lasers, dialogue letters, minecart and rails |
| `nativebbox0`, `ropefast0` | native bounding boxes, native rope solver |
| `clubbernative0`, `dustnative0` | native dancer glow / cathedral dust when those are turned on |
| `flashmask0`, `clothall` | flashlight mask culling, off-screen clothesline pause |
| `lighting60`, `lightmapbig` | 30 Hz lighting, the light map size fix |
| `texflush0` | texture page flush |
| `appsurf1280`, `appsurf640` | output-size application surface (`appsurf640`: force 640x360) |
| `parambuf16`, `novramsplit`, `sleeppace` | 8 MB GPU parameter buffer, VRAM split, vblank pacing |
| `pool3k` | 2 MB vitaGL vertex pool (uses upstream's 3 KB) |
| `touchui` | hidden touch controls (shows them again) |

## Benchmarking and profiling

`scripts/bench.py` installs `build/app/eboot.bin`, launches the game, continues into a room and reports fps:

```bash
python scripts/bench.py --label test --save work/testsaves/bunker2_3 --room room_bunker2_3 --window
```

- `--save DIR` installs a test save first, after backing up the saves on the Vita to `work/save_backups/<time>/`.
  Put them back with `python scripts/vita_saves.py restore <dir>`.
- `--window` prints a `[WINDOW]` line: the average frame time over frames 300-899 after entering the room, so
  runs compare the same game frames. Prefer A/B/B/A runs with it to single averages.
- `--profile` turns on the heavy profilers (needs `debugMode=1`); `--exp NAME` creates `exp_NAME`.
- `scripts/fast_sweep.py` jumps between rooms in one session (`shared/goto_room`) for quick triage; confirm
  results with fresh boots.

Log lines in `yyl.log`: `[PROF]` fps, frame time and game-thread time; `[EVENT]` time per event; `[STAGE]` time
per runner stage; `[GLPROF]` GL calls; `[SCENES]` GXM scenes per frame; `[MEM]`, `[VRAM]`, `[VGLMEM]` memory;
`[TEXFLUSH]` texture page flushes.

Opt-in profilers, as flag files in `ux0:data/gms/shared/` (all need `debugMode=1`; `exp_scriptprof`,
`exp_userprof` and `callprof` also need `--profile`):

- `exp_scriptprof`: times the scripts or runner functions listed in `scriptprof.txt` (at most 12 argument words).
- `exp_userprof`: times the user events listed in `userprof.txt`.
- `exp_builtinprof`: times the builtins listed in `builtinprof.txt`.
- `exp_allocprof`: counts the runner's malloc family.
- `exp_spikes`: logs every frame over 20 ms.
- `callprof`: counts calls through each PLT slot (slow).
- `stub_events`: object events listed one per line (`obj_name_Draw_0`) return at once, to measure their cost.
