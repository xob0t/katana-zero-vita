# katana-zero-vita

PS Vita port of Katana ZERO, built from the Netflix Android release (1.1.10). The game's Android runner
(`libyoyo.so`) runs on the Vita through [YoYo Loader Vita](https://github.com/Rinnegatamante/yoyoloader_vita),
vendored in `loader/` from upstream commit `e32f68f`. No game files are included: you need your own APK.

## Current state

The game is playable, but not at full speed everywhere.

- **Light rooms run at 60 fps**, the first level included.
- **Heavy rooms run at about 30-45 fps.** GameMaker slows the whole game down instead of dropping frames, so these
  rooms play in slow motion: at 30 fps, half speed. The worst is the haunted minecart ride (~31 fps); factory rooms
  run at ~38.
- **The CPU is the limit**, not the GPU. The game logic runs on one core and can't be split across more. The cheap
  wins are done; what's left would mean weeks of work for about 10% more speed.
- **Overclocking helps a little.** The loader runs the CPU at 444 MHz. A 500 MHz profile in PSVshell adds a few fps
  in heavy rooms but doesn't get them to 60.
- **Some effects are off by default for speed**: rain, clothesline light tint, club dancer glow, cathedral dust and
  replays. They can be turned back on in the [settings](#settings-yylcfg).
- **Loading:** the first launch takes about 3 minutes to compile shaders, later launches about 1 minute. A room
  load can pause for 1-1.5 s when texture memory is cleared.

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

Settings are in `ux0:data/gms/katanazero/yyl.cfg`. With `debugMode=1` the log goes to
`ux0:data/gms/shared/yyl.log`.

### Controls

The game's own controller layout. L and R act as the triggers: L is slow motion, R is roll. The rear touch
panel is unused.

## Settings (`yyl.cfg`)

Defaults in brackets.

| Setting | Effect |
| --- | --- |
| `renderRes=960\|720\|640\|480` [960] | Output resolution; lower ones are upscaled. Lower is not faster (the heavy rooms are CPU-bound), and below 960 text loses pixel columns. |
| `dynamicRes=0\|1` [1] | A room that keeps the GPU waiting at 960x540 (room_cathedral's window light) drops to 640x360 until the next room. |
| `replays=0\|1` [0] | Replay recording: the end-of-level VHS replay and the death rewind. With it on, memory runs out after a few minutes in one room and heavy rooms lose ~2 fps. Off, levels end through the game's skip-replay path. With `1` the game's own graphics option decides. |
| `rain=0\|1\|2` [0] | Rain drops: none, half, all. None is ~20% faster in rainy levels. |
| `ropeLights=0\|1` [0] | Coloured light tint on clotheslines (~7 fps near them). |
| `dancerGlow=0\|1` [0] | Club dancers' rim glow from the club lights, ~285 extra draws per frame (44 fps with it, 52-56 without). The light show stays either way. |
| `cathedralDust=0\|1` [0] | Dust motes in room_cathedral's window light (41 fps with it, 59 without). |
| `dissolveChunkScale=N` [4] | Laser deaths cut the body into N times fewer, N times thicker slices. `1` is the original. |
| `fogScale=N` [4] | Prison fog keeps one puff in N, drawn denser. `1` is the original. |
| `sameSoundLimit=N` [3] | At most N copies of one sound at once (`0`: no limit). Laser deaths start dozens of identical sounds and stalled the game mixing them. |
| `maxVoices=N` [32] | Overall voice limit (`0` keeps the runner's 128). |
| `audioGrain=N` [512] | Sound buffer in sample frames, a multiple of 64. The stock 128 crackled. A dropped buffer is logged as `[AUDIO] output late`. |
| `stickDeadzone=N` [20] | Analog stick deadzone in percent. Vita sticks rest slightly off centre. |
| `skipNetflixLogo=0\|1` [1] | Skips the Netflix logo at launch; the Netflix screen's checks still run. |
| `frameSkip=0\|1` [0] | Experimental frame skipping; breaks some scenes. |
| `memOverlay=0\|1` [0] | YoYo Loader's memory overlay (needs `debugMode=1`). |

## Changes to the loader

- Imports needed by runners built with NDK r26+: fortified libc, `*_l` locale functions, `pthread_rwlock_*`,
  bionic `readdir`, time functions.
- JNI object arrays hold real element pointers, so extension functions can take several strings.
- `loader/loader/netflix.c` replaces the `NetflixIntegration` extension offline. Cloud save slots are stored in
  `ux0:data/gms/<game>/assets/netflix/<slot>.sav`; leaderboards and telemetry do nothing.

## Performance

The game runs CPU-bound on the Vita, and GameMaker games slow down below 60 fps rather than skip frames. The
first level runs at ~59.5 fps; heavy combat rooms at roughly 32-44. The source comments have the measurements.

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
