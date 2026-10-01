# katana-vita

PS Vita port of Katana ZERO (Netflix Android build), based on
[YoYo Loader Vita](https://github.com/Rinnegatamante/yoyoloader_vita) (vendored in `loader/`,
upstream commit `e32f68f`). No game files are included; you need your own APK.

## Changes to the loader

- Imports needed by runners built with NDK r26+ (fortified libc, `*_l` locale functions,
  `pthread_rwlock_*`, bionic `readdir`, time functions).
- JNI object arrays hold real element pointers, so extension functions can take multiple strings.
- `loader/netflix.c`: offline replacement for the `NetflixIntegration` extension. Cloud save
  slots go to `ux0:data/gms/<game>/assets/netflix/<slot>.sav`; leaderboards/telemetry are no-ops.

## Building (Windows, Git Bash, no WSL)

Requires `gh`, CMake and MinGW `mingw32-make` on PATH.

```bash
bash scripts/setup-sdk.sh    # softfp VitaSDK into C:/vitasdk-softfp
bash scripts/setup-deps.sh   # vitaGL, openal-soft, vitaShaRK, SceShaccCgExt, opensles
python scripts/make_livearea.py --apk <Katana ZERO APK>   # optional: game art for the bubble/LiveArea
bash scripts/build.sh        # -> build/app/KatanaZero.vpk (+ build/loader/loader2.bin)
```

`make_livearea.py --apk` builds the icon, LiveArea background and startup gate from the game's own title
screen art (extracted from your APK with `scripts/extract_art.py`) into `livearea/game/`, which is not committed.
Without it the VPK uses the placeholder art in `livearea/`.

## Installing on a Vita

1. Install kubridge, FdFix and `libshacccg.suprx`.
2. Install `build/app/KatanaZero.vpk` (title ID `KTNZ00001`). It contains only the loader.
3. Build the game APK with `python scripts/prepare_apk.py <Katana ZERO 1.1.10 APK> game.apk`
   (needs `pip install lz4 pyelftools`) and put it at `ux0:data/gms/katanazero/game.apk`.
   Settings: `ux0:data/gms/katanazero/yyl.cfg`.
4. With `debugMode=1` in `yyl.cfg`, logs go to `ux0:data/gms/shared/yyl.log`.
5. The first launch compiles all shaders and takes ~3 minutes; they are cached in
   `ux0:data/shader_cache/KTNZ00001`, so later launches reach the title in about a minute.

### Controls

The game's controller layout, with the Vita's L and R acting as the triggers: L is slow-mo, R is roll. The rear
touch panel is not used.

### Settings (`yyl.cfg`)

- `renderRes=960|720|640|480`: render resolution; the Vita scales it to the screen (640 recommended).
- `replays=0|1`: replay recording (end-of-room VHS replay and the death rewind). Off by default (levels end
  through the game's own skip-replay path and deaths restart without the rewind):
  it runs the Vita out of memory after a few minutes in one room and costs ~2 fps in heavy rooms.
  With `replays=1` the game's own option in the graphics menu decides.
- `rain=0|1|2`: rain drops none (default, ~20% faster in rainy levels), half, or all.
- `ropeLights=0|1`: coloured light tint on clotheslines. Off by default (~7 fps faster near them).
- `frameSkip=1`: experimental frame skipping (known to break some scenes).
- `skipNetflixLogo=1|0`: skip the Netflix logo at launch (default 1). The Netflix SDK screen still runs its
  checks, without the logo and its fades.
- `memOverlay=1`: YoYo Loader's on-screen memory overlay (needs `debugMode=1`; off by default).
- `stickDeadzone=20`: analog stick deadzone in percent (default 20). Vita sticks rest slightly off centre; without
  a deadzone the camera drifts while standing still. `0` disables it.

## Performance

The Vita runs the Android YYC runner CPU-bound. Below 60 fps GameMaker games slow down rather than drop frames,
so higher fps also means closer to full game speed. Heavy combat rooms run at roughly 32-44 fps, the first level
at ~59.5. What the loader does (details and measurements in the source comments):

- Game thread on CPU core 1; core 0 loses ~2 ms/frame to system work.
- Texture pages stored as LZ4 inside the runner's QOI container (`prepare_apk.py`), decoded natively.
- 640x360 application surface, lighting refresh at 30 Hz, vblank-paced frames, larger vitaGL render-target
  scene budgets, VRAM split for large allocations.
- Off-screen clothesline simulation paused; clothesline/rope solver and bbox computation in native code.
- Exact native replacements for hot GML (same results, RNG use and collision queries; fallback to the original
  code for unusual inputs): `controller_check_pressed_ext`, minecart rails/master, ceiling lasers, dialogue
  letters.
- Render-only work removed: flashlight mask draws outside the screen, and enemy surface captures replayed
  directly instead of rendering each enemy into a 70x70 surface (small resampling differences possible).

Each default can be turned off for comparison by creating an empty flag file
`ux0:data/gms/shared/exp_<name>`: `gamecore0` (core 0), `vcapture0` (real enemy captures; `vcapexact` for the
pixel-exact mode), `inputnative0`, `flashmask0`, `lasernative0`, `minecartnative0` (rails only; plus `railnative0`
for none), `letternative0`.

## Benchmarking and profiling

`scripts/bench.py` installs `build/app/eboot.bin`, launches the game, continues into a room and reports fps:

```bash
python scripts/bench.py --label test --save work/testsaves/bunker2_3 --room room_bunker2_3 --window
```

- `--save DIR` installs a test save first (`scripts/vita_saves.py`; back up your own saves before).
- `--window` prints the `[WINDOW]` line: average frame time over frames 300-899 after entering the room, so A/B
  runs compare the same game frames. Prefer A/B/B/A runs with it over single wall-clock averages.
- `--profile` enables the heavy profilers (needs `debugMode=1`); `--exp NAME` creates `exp_NAME`.

Log lines (`ux0:data/gms/shared/yyl.log`): `[PROF]` fps / frame time / game-thread run time, `[EVENT]` per-event
time, `[STAGE]`, `[GLPROF]` GL calls, `[SCENES]` GXM scenes per frame and their CPU cost, `[MEM]`/`[VGLMEM]`
memory. Opt-in profilers via flag files: `exp_scriptprof` times the scripts or runner functions listed in
`shared/scriptprof.txt` (entry hooks; functions with at most 12 argument words), `exp_userprof` times the
user events listed in `shared/userprof.txt`, `exp_builtinprof` times builtins from `shared/builtinprof.txt`,
`exp_allocprof` counts the runner's malloc family.
