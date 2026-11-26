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
4. For the best experience, overclock: install [PSVshell](https://github.com/Electry/PSVshell) and give the game a
   500 MHz CPU profile. Without it the heavy rooms drop more frames (the loader alone sets 444 MHz).

The first launch compiles the graphics shaders into `ux0:data/shader_cache/KTNZ00001` and takes about a minute; a
status screen shows it. Later launches start the game in about 20 seconds. To update, build a new VPK and
install it over the old one. Saves are kept. Older builds kept the shader cache in
`KTNZ00001/v2` (512 folders); current builds compile a new cache once, on the first launch after the update, and
never read that folder again, so it can be deleted.

With `debugMode=1` in `yyl.cfg` the log goes to `ux0:data/gms/shared/yyl.log`.

## About the port

The game's Android runner (`libyoyo.so`) runs on the Vita through
[YoYo Loader Vita](https://github.com/Rinnegatamante/yoyoloader_vita), vendored in `loader/` from upstream `e32f68f`.

Most rooms run at 60 fps. A few of the heaviest dip into the 30s, and as in the original, the game slows down with
the frame rate. The Vita's CPU is the limit. To get this far, the loader patches the game in over 50 places, most of
them with identical results, and fixes vitaGL bugs that caused a freeze and a crash.

**GRAPHICS → 30 FPS IN COMBAT** (off by default) runs every room with enemies or a boss at a steady 30 fps with the
game's clock doubled, so fights play at full speed instead of slowing down; the other rooms stay at 60. Input is read
30 times a second there. The choice is saved and applies from the next room.

Some of the speedups change what you see or play:

- Enemies think every other frame. Hits still land on time. **GAME → ENEMY AI** turns it off.
- No end-of-level replay and no death rewind.
- No touch controls: the front touch screen does nothing, and the options menu has no CONTROLS page for them.
- Bloom, shaders, blood spray and the restart and rewind effects are off by default. With bloom on, some rooms run
  at half the frame rate. They can be turned on in **GRAPHICS**.
- Simpler effects in heavy scenes: less lighting, lighter smoke, fog and explosions.
- No sparks fly off the HUD's battery meter during slow motion.
- The flamethrower's flames don't light up the room.

Each can be undone in `ux0:data/gms/katanazero/yyl.cfg`; [docs/changes.md](docs/changes.md) lists them all.

Enemies attack on the PC version's timing. The Android release delays their first attack and every shot by a few
frames, which also hides the bullet in the first room's tutorial slow-mo. **GAME → ENEMY TIMING: ANDROID**
(or `enemyTiming=1`) brings back the Android timing, along with its longer melee cooldowns and its slash, smoke and
death slow-mo rules.

Level time limits and the slow-mo meter are the PC version's too: the Android release makes the time limits 1.5 times
as long and the meter twice as long. **GAME → TIMER & SLOW-MO: ANDROID** (or `timerSlowmo=1`) brings them back.

`lagLatch=2` in `yyl.cfg` (off by default) removes the stutter at the start of chinatown_4 and after each restart
there: the game stops retrying rim shading that the room can't hold at 60 fps. Visible: rim shading stays off where
the game would have flashed it on for a moment between stutters.

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md): one `uv run build.py` command on Windows (no WSL) or Linux, how to
install a source build, and how the web builder is built.

## Changes to the loader

- Imports needed by runners built with NDK r26+: fortified libc, `*_l` locale functions, `pthread_rwlock_*`,
  bionic `readdir`, time functions.
- JNI object arrays hold real element pointers, so extension functions can take several strings.
- `loader/loader/netflix.c` replaces the `NetflixIntegration` extension offline. Cloud save slots are stored in
  `ux0:data/gms/<game>/assets/netflix/<slot>.sav`. Leaderboards and telemetry do nothing.
- The game-specific speedups are in `loader/loader/`, mostly one file each; [docs/DEVELOPING.md](docs/DEVELOPING.md)
  maps them to their files, settings and the `exp_*` flags that turn each change off for comparison.

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
