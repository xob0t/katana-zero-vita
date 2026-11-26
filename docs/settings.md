# Settings

Every key the loader reads from `yyl.cfg`, with its default and what it does. [changes.md](changes.md) is the
player-facing list of what the port changes; this page covers all keys, including the debug and upstream ones.

## The file

The app reads `ux0:data/gms/katanazero/yyl.cfg` once at startup (`loadConfig` in `loader/loader/main.c`). A change
takes effect at the next launch. The file is optional: without it every key keeps its default. Nothing writes it.

Format (`yyl_read_setting` in `loader/loader/yyl_config.h`):

- One `key=value` per line. Spaces and tabs around the key, the `=` and the value are allowed.
- The value is a decimal integer, optionally signed. `on`, `true` or `1.5` are not numbers, so the line is skipped.
- Any line that doesn't parse is skipped without a message: comments (`# ...`), blank lines, a comment after the
  value (`rain=1 # note`), lines of 127 characters or more, keys of 64 characters or more.
- Unknown keys are ignored without a message. Check the spelling: keys are case-sensitive.
- A key that appears twice takes the last value.

`maximizeNewlib` is the exception: `sbrk.c` reads it before `main()` by searching the raw file for the text
`maximizeNewlib=1`. That search also matches inside a comment, and allows no space around `=`.

The drop-in loader build (`loader2.bin`, no `KATANA_APP`) reads `ux0:data/gms/<game>/yyl.cfg` for the game named in
`ux0:data/gms/launch.txt`. A `STANDALONE` build reads `app0:yyl.cfg`.

## Port settings

Defaults come from the variable definitions in the file listed. "Nonzero" means any value other than 0.

| Key | Default | Values | Effect | Defined in |
|---|---|---|---|---|
| `enemyHalfRate` | 1 | 0, nonzero = 1 | Enemy Steps every other tick. The GAME menu's ENEMY AI row overrides this once a player picks FULL RATE or HALF RATE there (the choice is saved in the game options). | `enemy_half_rate.c`, `katana_menu.c` |
| `enemyTiming` | 0 | 0, nonzero | 1 plays the Android release's enemy timing and combat rules: later first attacks and shots, 4 frames longer melee cooldowns, slashes through temporary doors, door-opening slashes deadly 2-3 frames longer, smoked enemies falling 3 more frames, the death slow-mo starting after the hit freeze and ramping faster. 0 the PC version's. The GAME menu's ENEMY TIMING row overrides this once a player picks PC or ANDROID there (saved in the game options). | `enemy_timing.c`, `pc_rules.c`, `katana_menu.c` |
| `combat30` | 0 | 0, nonzero | 1 runs rooms with enemies or a boss at 30 ticks a second with the game's clock doubled (30 FPS IN COMBAT). The GRAPHICS menu's 30 FPS IN COMBAT row overrides this once a player saves it there (the choice is kept in `assets/combat30.cfg`, next to the saves). Unrelated to the removed `fpsMode`/`fps30`, which stay ignored. | `combat30.c`, `katana_menu.c` |
| `lagLatch` | 0 | 0, 1, 2; other values are ignored | The game's own lag mode (obj_opto_master user event 5) turns rim shading and a few other effects off while the frame rate is low, and back on after half a second at speed. Where full rim shading can't hold 60 (chinatown_4) it goes back and forth 4 times, each time with 7-8 frames at ~31 ms, before the game keeps it on for the room. 1 keeps lag mode on for the room after the first failed exit (a dip within 1 s of leaving it). 2 also remembers the room until the app quits: its later entries and restarts start in lag mode with rim shading off. 0 is the game's behaviour. Visible: rim shading stays off where the game would have turned it on between dips. | `lag_latch.c` |
| `timerSlowmo` | 0 | 0, nonzero | 1 keeps the Android release's level time limits (1.5 times the PC ones) and slow-mo meter (twice the PC one). 0 the PC version's. The GAME menu's TIMER & SLOW-MO row overrides this once a player picks PC or ANDROID there (saved in the game options). | `timer_slowmo.c`, `katana_menu.c` |
| `smokeClouds` | 2 | 1 to 3; below 1 counts as 1, above 3 as 3 | One smoke cloud in N is drawn, larger. Needs the native smoke Draw: with `exp_smokedraw0` every cloud is drawn as in the original. | `smokedraw_native.c` |
| `smokeWobble` | 1 | 0, nonzero | 1: a smoke cloud's wobble (its Draw moves it and puts it back every frame) writes x, y and image_angle without the collision system's remove and re-insert. Hit areas stay the same; when several instances overlap the probed spot, a check that returns one of them may return another. 0: the original setters. Needs the native smoke Draw (`exp_smokedraw0` restores the original); off when its runner functions aren't the expected ones. | `smokedraw_native.c` |
| `smokeContactFilter` | 1 | 0, nonzero | 1: a settled cloud that skips its Step this tick (`fx_rate.c`) tests its three contact checks only when its box can touch an enemy, the player or a slash. 0: every skipped cloud makes the three `place_meeting` calls. Exact while the collision boxes are current, which it checks. | `fx_rate.c` |
| `smokePlayerStill` | 1 | 0, nonzero | 1: a settled cloud whose only contact is a player standing still stays on the every-4th-tick schedule instead of running its Step every tick. 0: any player contact runs the Step on its tick. Exact: used only where the Step would write what is already there. | `fx_rate.c` |
| `smokeRecord` | 0 | 0, nonzero | 0: with `replays=0`, a cloud's Begin Step sets its recorded flags without appending it to the replay's smoke arrays (only the replay and the death rewind read them). 1: always recorded, as in the original. With `replays=1` clouds are always recorded. | `fx_rate.c` |
| `collisionRoundTrip` | 1 | 0, nonzero | 1: an instance's collision tree entry isn't removed and inserted again when it already holds the same rectangle (probes such as `place_meeting(x + 1, y, ...)` and code that moves an instance and back). The same instances and rectangles stay in the tree; only their order inside it differs, which decides which of several overlapping instances a first-hit check returns. 0: the original. | `collision_round_trip.c` |
| `lightArrayFast` | 1 | 0, nonzero | 1: the native `update_light_array` loop computes each light's distance inline instead of calling the runner's `point_distance` (exact: the same code, checked at install). 0: through the runner. Needs the native loop (`exp_lightarray0` turns it off). | `light_array_native.c` |
| `lampSway` | 4 | 1 to 4; below 1 counts as 1, above 4 as 4 | Swaying lamps (free-floating obj_orangelight) run their Step on one tick in N and apply the skipped ticks' steering on the next one, with the same random numbers. 1 is the original. | `light_fast_inline.c` |
| `trailLights` | 0 | 0, nonzero | 1 keeps the light tint on roll dust and afterimages everywhere. | `fog_trail.c`, `trail_lights.c` |
| `fogTrailLights` | 0 | 0, nonzero | 1 keeps the roll dust's light tint in the fog rooms only. Afterimages are not affected. | `fog_trail.c` |
| `letterGlow` | 2 | 0 to 2; other values are ignored | The glow behind highlighted dialogue words. 2: the same 12 purple copies of the letter as the game, with one text call and one vertex batch per letter (`letter_glyph.c`); 1: the game's own glow script; 0: no glow. Needs the native letters: with `exp_letternative0` the game's glow is always drawn; with `exp_letterglyph0`, 2 draws the game's glow. | `letter_native.c`, `letter_glyph.c` |
| `waveStrips` | 4 | 1 to 8; below 2 counts as 1, above 8 as 8 | Strip height factor of the wavy dialogue backgrounds. 1 is the original. Needs the native `draw_sprite_wavy` (`exp_wavynative0` restores the original). | `wavy_native.c` |
| `dissolveChunkScale` | 4 | 1 or less = original, N > 1 | Laser deaths cut the body into N times taller slices. | `katana.c` |
| `sameSoundLimit` | 3 | 0 or less = no limit, 1 to 8; above 8 counts as 8 | Most copies of one sound that play at once. | `sound_limit.c` |
| `maxVoices` | 32 | 0 or less = the runner's 128, N > 0 | Audio voice count, set once after startup. | `katana.c` |
| `asyncSave` | 1 | 0, nonzero | 1 writes checkpoints and options on a background thread. 0 writes them on the game thread (as does `exp_asyncsave0`). | `asyncsave.c` |
| `rain` | 1 | 0, 1, 2; other values are ignored | 1 cheap rain drawn by the loader, 0 no rain, 2 the game's rain. If cheap rain can't install it falls back to 0. With 2, `katana.c` still runs each drop's Draw on alternate frames only (half the drops each frame). | `main.c`, `rain_cheap.c`, `katana.c` |
| `replays` | 0 | 0, nonzero | 1 brings back the end-of-level replay and the death rewind. | `main.c`, `katana.c` |
| `pauseStack` | 1 | 0, nonzero | 1 draws the paused room picture's 10 stacked layers (behind the pause and options menus) as one quad with a shader that applies the 10 blends; 0 the game's 10 draws. | `pause_stack.c` |
| `dancerGlow` | 2 | 0 to 2 | The club dancers' rim glow. 2: the loader draws it with the game's lights, distances and alphas, one composited outline per side (`dancer_glow.c`; about 0.6 ms in club_0); 1: the game's own glow (in club_0 it also needs `roomLighting=1`); 0: no glow. | `katana.c`, `dancer_glow.c` |
| `bloomDefault` | 0 | 0, nonzero | 1: BLOOM FX defaults to on, as in the game. 0: off on a fresh install and after the game's option resets. A choice saved in the GRAPHICS menu is kept either way. | `option_defaults.c` |
| `restartFxDefault` | 0 | 0, nonzero | The same for RESTART FX (the wave distortion of the restart transition). | `option_defaults.c` |
| `rewindFxDefault` | 0 | 0, nonzero | The same for REWIND FX (the VHS look of the death rewind; nothing to see unless `replays=1`). | `option_defaults.c` |
| `shadersDefault` | 0 | 0, nonzero | The same for SHADERS (needed by bloom, RESTART FX, REWIND FX and the replay's VHS look). | `option_defaults.c` |
| `bloodSprayDefault` | 0 | 0, nonzero | The same for BLOOD SPRAY (blood drops and splatter). | `option_defaults.c` |
| `roomLighting` | 0 | 0, nonzero | 1 keeps the lamp glow in room_club_0 and room_haunted_cart2. | `room_lighting.c` |
| `hudSparks` | 0 | 0, nonzero | 1 keeps the sparks the HUD's battery meter throws during slow motion. 0 doesn't create them, but draws their random numbers. | `hud_sparks.c` |
| `flameLights` | 0 | 0, nonzero | 1 keeps the light of the player's flamethrower flames (room_mansion_2_2, room_mansion_3). 0 doesn't create those lights, but takes their instance ids. | `flame_lights.c` |
| `psychEffects` | 0 | 0, nonzero | 1 keeps obj_psychboss_effects' screen-copy pass in part 3 of the Psychiatrist fight (the black void). 0 skips it while its effect is the part-3 one; its variable writes stay. | `psych_fix.c` |
| `psychPetals` | 2 | 1 to 4; below 1 counts as 1, above 4 as 4 | One rose petal in N is drawn in the Psychiatrist fight's last part; all of them still exist and fall. 1 draws all. | `psych_petal.c` |
| `ropeLights` | 0 | 0, nonzero | 1 keeps the light tint on clotheslines. | `main.c`, `katana.c` |
| `cathedralDust` | 0 | 0, nonzero | 1 keeps the dust in the cathedral's window light (with an exact native, `exp_dustnative0` for the GML). | `katana.c`, `dustspawner_native.c` |
| `fogScale` | 4 | 1 or less = original, N > 1 | Keeps one prison fog puff in N, drawn denser. | `katana.c`, `fog_skip.c` |
| `dossierCircles` | 0 | 0 or less = none, 1 = all, N > 1 = one in N | Decorative burn circles kept in the dossier burn. Only the native first Step drops them: with `exp_dossiernative0` or `exp_dossierexact0` all are kept. | `dossier_native.c` |
| `coneShadows` | 0 | 0, nonzero | 1 keeps the traffic cone shadows in the bike chase. | `cone_shadow.c` |
| `farRocketLights` | 0 | 0, nonzero | 1 keeps the lighting on the distant rockets in the bike chase. | `far_rocket.c` |
| `fullExplosions` | 0 | 0, nonzero | 1 keeps all 30 particles of each mortar explosion. With `debugMode=1` the lighter-explosion code still installs, for the `exp_exploshot` screenshot trigger, but creates every particle. | `explosion_lite.c` |
| `skipNetflixLogo` | 1 | 0, nonzero | Skips the Netflix logo at launch. Boot runs the same steps without it. | `katana.c` |
| `renderRes` | 960 | 960, 720, 640, 480; other values are ignored | Output resolution: 960x544, 720x408, 640x368 or 480x272, scaled up by the Vita's display hardware. The application surface follows it: 960x540 at 960, 720x405 at 720, the game's 640x360 at 640 and 480. | `main.c`, `katana_display.c` |
| `dynamicRes` | 0 | 0, nonzero | 1 drops a GPU-bound room to a 640x360 application surface until the next room. Only acts when the surface is larger than 640 wide (`renderRes` 960 or 720). | `katana_display.c` |
| `touchControls` | 0 | 0, 1, 2 or more | 0: no touch input, the touch controls' per-frame events stop, and the game starts in its controller mode (`global.gamepadMode`), so prompts show buttons from the first frame; the options menu leaves out CONTROLS, the touch controls' page (`katana_menu.c`). 1: touch works and the controls aren't drawn; the game starts in touch mode as on Android and switches on the first stick move. 2 or more: the controls are also drawn. | `main.c`, `touch_controls.c`, `gamepad_prompts.c`, `katana_menu.c` |
| `stickDeadzone` | 20 | 0 to 90, clamped | Radial stick deadzone in percent of full tilt. 0 turns it off. | `gamepad.c` |
| `audioGrain` | 512 | 64 to 4096, other values use 512; rounded down to a multiple of 64 | Buffer size in sample frames of the OpenSL ES audio thread (`patches/opensles.patch`). Smaller buffers underrun on a busy Vita and crackle. | `patches/opensles.patch` |
| `debugMode` | 0 | 0, nonzero | 1 writes the log to `ux0:data/gms/shared/yyl.log` and turns on the debug request files, the profiling report and the freeze locator ([DEVELOPING.md](DEVELOPING.md#debug-files)). | `main.c` |
| `memOverlay` | 0 | 0, nonzero | 1 shows YoYo Loader's memory overlay. Only with `debugMode=1`. | `main.c` |

Settings marked visible in [changes.md](changes.md) change what you see or hear. `dynamicRes` also changes the
picture when it acts; `renderRes` changes sharpness.

## Upstream YoYo Loader settings

These come from YoYo Loader Vita and mostly matter for other games. All default to 0.

| Key | Values | Effect |
|---|---|---|
| `forceGLES1` | 0, nonzero | Refuses to open GLES2 libraries (`dlopen` of names with `v2` fails), so the runner falls back to GLES1. |
| `forceBilinear` | 0, nonzero | Forces linear texture filtering. |
| `platTarget` | 0, 1, 2; other values are ignored | The platform the game is told it runs on: 0 Android, 1 Windows, 2 PS4. Also changes the gamepad slot layout. |
| `winMode` | 0, nonzero | Old name: nonzero sets `platTarget=1`, 0 sets `platTarget=0`. |
| `debugShaders` | 0, nonzero | Writes every shader source the runner compiles to `ux0:data/gms/shared/glsl/<n>.glsl`. |
| `noSplash` | 0, nonzero | Skips the APK's `assets/splash.png` at boot. |
| `maximizeMem` | 0, nonzero | Initialises vitaGL with `vglInitWithCustomThreshold` instead of `vglInitExtended`, and shows no message dialog for fatal errors (the log still gets them). |
| `maximizeNewlib` | `maximizeNewlib=1` anywhere in the file | Newlib heap of 300 MB instead of 240 MB. Read by `sbrk.c`, see [The file](#the-file). |
| `squeezeMem` | 0, nonzero | vitaGL circular vertex pool of 3 KB instead of 2 MB and a 2 MB parameter buffer. Costs frame time in heavy rooms. |
| `uncachedMem` | 0, nonzero | Turns off `vglUseCachedMem`. |
| `doubleBuffering` | 0, nonzero | Double buffering instead of vitaGL's triple buffering. |
| `netSupport` | 0, nonzero | Starts sceNet and keeps the runner's network imports. With 0 they resolve to dummies. Katana ZERO doesn't need it: `netflix.c` answers the Netflix calls offline. |
| `disableAudio` | 0, nonzero | Starts the runner with audio off. |

## Ignored keys

| Key | What happens |
|---|---|
| `fpsMode`, `fps30` | The old 30 fps mode (heavy rooms or every room) was removed; `combat30` is its successor, with a new key so old configs don't turn it on. The loader logs once that the key is ignored. |
| `frameSkip` | The opt-in frame skipping was removed (Nov 2026). The loader logs once that the key is ignored. |

Any other key is ignored without a message.
