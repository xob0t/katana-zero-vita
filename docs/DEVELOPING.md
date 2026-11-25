# Developing

A map of the loader for contributors: which file does what, how to switch each change off, the debug files on the
Vita, the rules for hooking the runner, and how to build and check a change. Settings are in
[settings.md](settings.md), the player-facing changes in [changes.md](changes.md).

The port-owned code is in `loader/loader/`. Each port file starts with a comment that explains the change, what it
was measured at and how to turn it off; read it before changing the file. Upstream YoYo Loader files are `main.c`
(mixed with port code), `so_util.c`, `so_elf.c`, `dialog.c`, `gamepad.c`, `ioapi.c`, `unzip.c`, `zip.c`, `sha1.c`,
`net.c`, `trophies.c`, `videoplayer_patch.c`, `audio_player.c`, `profiler.c`, `ctype_patch.c`, `openal_patch.c`,
`qoi_decoder.c` (selector build only) and `sbrk.c`.

## Exact and visible changes

Most changes are exact: the game computes the same values, draws the same things and draws the same random numbers as
the original. They fall in two kinds:

- Natives: a GML event or script rewritten in C from the runner's ARM code. These are compiled with
  `-ffp-contract=off` (no fused multiply-add, so doubles round as in the runner) and listed in
  `loader/exact_sources.cmake`. Each one checks what it relies on at install and on every call, and runs the original
  event for anything it doesn't expect.
- Skips: work whose result is thrown away is not done (an event at rest, an off-screen light, a puff that would be
  destroyed before it is seen).

Visible changes each have a `yyl.cfg` key that brings back the original, listed in [changes.md](changes.md).
A few older ones in `katana.c` have no key, only an `exp_*` switch or none: they are marked below.

## Experiment switches (exp_*)

`exp_flag("name")` (`so_util.c`) returns 1 when the file `ux0:data/gms/shared/exp_name` exists. Its content doesn't
matter, except for `exp_spikes`, `exp_explokeep`, `exp_explospeed` and `exp_exploshot`, which hold a number. Until the
runner's Startup returns, the answer comes from one listing of the folder taken at the first call (the card is busy
with the apk prefetch then); after that each call stats the memory card. Flags are read when a feature installs, almost
all once at boot: create the file, then start the game. Exceptions: `exp_roomlighting1` is read at every Room Start, `exp_exploshot` at the first explosion,
`exp_hangdump` when the freeze locator starts.

Kinds of switch:

- `exp_<feature>0`: the off-switch. The game runs the original code.
- `exp_*verify`: a differential check while the game runs. The native runs next to the original (or the original is
  run after a skip) and mismatches are counted in the profiling report. They must stay 0. Some come with negative
  controls (`exp_smokeverifyfault1`, `exp_smokeverifyfault2`, `exp_mcdiffbreak`) that must produce mismatches.
- `exp_*ab`: in-run A/B. The native is switched off in every other report window, so one boot measures both.
- `exp_*prof`, `exp_*time`, `exp_*sample`: extra timing or sampling lines in the report.

Verify, A/B and profiling output goes to the profiling report in `yyl.log`, which needs `debugMode=1`. Some of these
switches also check `debugMode` themselves.

## Feature map

Files are in `loader/loader/`. "Exact" means bit-identical game state and draws, as described above.

### Smoke, fog and particles

| Feature | Files | yyl.cfg | Off-switch | Other switches | Kind |
|---|---|---|---|---|---|
| Smoke cloud Draw native | `smokedraw_native.c` | `smokeClouds` | `exp_smokedraw0` (also restores the full cloud count) | `exp_smokeverify`, `exp_smokelookverify`, `exp_smokeverifyfault1/2`, `exp_smokeab`, `exp_smokelookab`, `exp_smokeprof` | Exact native; `smokeClouds` is visible |
| Settled smoke and corpse embers every 4th tick | `fx_rate.c` | none | `exp_fx0`; `exp_fxembers0` for embers only | `exp_fx2`, `exp_fx3` (other rates) | Visible |
| Prison fog: one puff in N | `katana.c` (`fog_install`), `fog_skip.c` (discarded puffs are never created) | `fogScale` | `exp_fogskip0` (create and destroy instead) | `exp_fogskipverify` | Visible; `fog_skip.c` exact |
| Roll dust without light tint | `fog_trail.c` | `trailLights`, `fogTrailLights` | none | `exp_fogtrailab`, `exp_fogtrailsample` | Visible |
| obj_particle draw event native | `particle_native.c` | none | `exp_particlenative0` | `exp_particleverify`, `exp_particleab` | Exact native |
| Particles past the first 20 per frame drawn on alternate frames | `katana.c` (`particle_draw`) | none | none | | Visible, not in changes.md |
| Explosion particles skip `update_light_array` | `particle_lights.c` | none | `exp_particlelights0` | `exp_particlelightsverify` | Exact skip |

### Lighting

| Feature | Files | yyl.cfg | Off-switch | Other switches | Kind |
|---|---|---|---|---|---|
| Lighting at 30 Hz | `katana_display.c` | none | `exp_lighting60` | | Visible |
| No lamp glow in club_0 and haunted_cart2 | `room_lighting.c` | `roomLighting` | none | `exp_roomlighting1` (A/B, read at Room Start) | Visible |
| obj_orangelight and obj_lightback Step natives | `light_fast_inline.c` (default), `light_fast.c` (`exp_lightfast`) | none | `exp_lightfast0` | | Exact native |
| Off-screen obj_lightback user event 0 | `lightback_native.c` | none | `exp_lightbacknative0` | `exp_lbverify`, `exp_lbab` | Exact native |
| `update_light_array` loop | `light_array_native.c` | none | `exp_lightarray0` | `exp_lightarrayverify` | Exact native |
| `draw_sprite_recolored` light loop | `recolor_lights.c` | none | `exp_recolorlights0` | `exp_recolorlightsverify`, `exp_recolorlightsab`, `exp_recolorlightsprof` | Exact native |
| Afterimage trails: far lights skipped, tint native | `trail_lights.c` | `trailLights` (afterimage tint) | `exp_traillights0`; `exp_traillightsnative0` keeps only the skipping | `exp_traillightscheck`, `exp_traillightsverify` | Exact; the tint off is visible |
| `update_region` native | `region_native.c` | none | `exp_regionnative0` | `exp_regionverify` | Exact native |
| Club dancers' rim glow, cheap by default | `dancer_glow.c` (cheap, 2), `katana.c` (stub for 0), `clubber_native.c` (native for 1) | `dancerGlow` | `exp_clubbernative0` (GML for 1) | `exp_dglowab`, `exp_dglowprof`, `exp_dglowcompare` | Visible (cheap: outline corners); native exact |
| Clotheslines without light tint | `katana.c` (`cloth_instance_exists`) | `ropeLights` | none | | Visible |
| Lighting tilemaps sized in tiles (game bug fix) | `katana.c` (`layer_tilemap_create_fixed`) | none | `exp_lightmapbig` | | Fix |

### Rain

| Feature | Files | yyl.cfg | Off-switch | Other switches | Kind |
|---|---|---|---|---|---|
| Cheap rain, or none | `rain_cheap.c`, `katana.c` (`rain=0`) | `rain` | `rain=2` | `exp_rainkeep2` to `exp_rainkeep8`, `exp_rainnosplash`, `exp_rainprof`, `exp_rainab` | Visible |

### Enemies, bodies, blood

| Feature | Files | yyl.cfg | Off-switch | Other switches | Kind |
|---|---|---|---|---|---|
| Enemy AI every other tick | `enemy_half_rate.c`, `katana_menu.c` (ENEMY AI row) | `enemyHalfRate` | the setting or the menu | | Gameplay timing change |
| Hidden blood drops skip their Step | `blood_drops.c` | none | `exp_blooddrops0` | | Exact for game state |
| Enemy wobble springs at rest skip their Step | `wobble_rest.c` | none | `exp_wobblerest0` | `exp_wobbleverify` | Exact skip |
| Enemy surface captures without render-target switches | `virtual_capture.c` | none | `exp_vcapture0` | `exp_vcapexact` (exact geometry instead of the default relaxed mode) | See the file |

### Rooms and effects

| Feature | Files | yyl.cfg | Off-switch | Other switches | Kind |
|---|---|---|---|---|---|
| Cloth at 30 Hz, paused far from view | `katana.c` | none | `exp_clothall` (no pausing; 30 Hz stays) | | Visible, not in changes.md |
| Rope solver native | `rope_fast.c` | none | `exp_ropefast0` | | Exact native |
| Bounding-box fast path | `collision_native.c` | none | `exp_nativebbox0` | | Exact native |
| Tile and door glow fades at rest | `glow_rest.c` | none | `exp_glowrest0` | `exp_glowverify` | Exact skip |
| Pause screen and options menu CRT rows (`draw_crt_surface`, `draw_crt_image`) | `crt_native.c` | none | `exp_crtnative0` | `exp_crtverify`, `exp_crtab` | Exact native |
| Paused room's tile layers, covered by the pause background's opaque rectangle, not drawn | `pause_bg.c` (`page_evict.c` keeps their pages) | none | `exp_pausetiles0` | `exp_pausetilesverify`, `exp_pausetilesab` | Exact skip |
| Paused room picture's 10 stacked draws as one shader pass | `pause_stack.c` | `pauseStack` | none | `exp_pausestackverify`, `exp_pausestackab` | Visible (1/255 on a few pixels) |
| Cathedral dust off | `katana.c` (stubs), `dustspawner_native.c` (native when on) | `cathedralDust` | `exp_dustnative0` (GML when on) | | Visible; native exact |
| Dossier burn natives, fewer decorative circles | `dossier_native.c` | `dossierCircles` | `exp_dossiernative0`; `exp_dossierexact0` (keeps part) | `exp_dossiercleanup`, `exp_dossierverify` | Exact; circles visible |
| Wavy dialogue backgrounds | `wavy_native.c` | `waveStrips` | `exp_wavynative0` | `exp_wavyverify`, `exp_waveab`, `exp_waveprof` | Exact native; strips visible |
| Ceiling laser beams | `laser_native.c` | none | `exp_lasernative0`; `exp_laserfast0` (temporary array as before) | `exp_laserab`, `exp_laserprof` | Exact native |
| Laser deaths in fewer slices | `katana.c` (`dissolve_install`) | `dissolveChunkScale` | none | | Visible |
| Minecart master and rails | `minecart_native.c` | none | `exp_minecartnative0` | | Exact native |
| Flashlight mask culling | `flash_mask.c` | none | `exp_flashmask0` | | Exact native |
| `move_contact_solid` (Dragon dash aim) | `move_contact_native.c` | none | `exp_movecontact0` | `exp_mcverify`, `exp_mcdiff`, `exp_mcdiffbreak`, `exp_mcab`, `exp_mcprof` | Exact native |
| No replays or death rewind | `katana.c` | `replays` | none | `exp_bunkerreplay`, `exp_bunkertrace` | Visible |

### Bike chase

| Feature | Files | yyl.cfg | Off-switch | Other switches | Kind |
|---|---|---|---|---|---|
| Actor with-loops step over other actors | `bike_draw.c` | none | `exp_bikedraw0`; `exp_bikepcache0` | `exp_bikedrawverify` | Exact |
| `draw_shadow` native | `shadow_native.c` | none | `exp_shadownative0` | `exp_shadowverify` | Exact native |
| No traffic cone shadows | `cone_shadow.c` | `coneShadows` | none | | Visible |
| Distant rockets unlit | `far_rocket.c` | `farRocketLights` | none | `exp_farrocketverify` | Visible |
| A third of the explosion particles | `explosion_lite.c` | `fullExplosions` | none | `exp_explokeep`, `exp_explospeed`, `exp_exploshot`, `exp_explorng` (debugMode) | Visible |

### Text, input, view tests

| Feature | Files | yyl.cfg | Off-switch | Other switches | Kind |
|---|---|---|---|---|---|
| obj_letter natives, cheap glow on highlighted words | `letter_native.c`, `letter_glyph.c` | `letterGlow` | `exp_letternative0` | `exp_letterglowab` (off / game / cheap per report window), `exp_letterglowprof`, `exp_letterglowverify`, `exp_letterglowverifyfault`, `exp_letterglowcompare`, `exp_lettertime` (read in `katana.c` and `letter_glyph.c`) | Exact native; glow positions within float rounding |
| A new wavy or rainbow letter on a frozen tick (hit-stop, fr = 0) skips its `while (oscillate_delay > 0) oscillate_delay -= fr` setup until time moves, instead of hanging the game (game bug) | `letter_freeze.c` | none | `exp_letterfreeze0` | `exp_letterfreezerepro` (stops time before each new wavy letter's first Step) | Fix |
| `inside_screen`, `inside_screen_point` | `visibility_native.c` | none | `exp_visibilitynative0` | `exp_visverify`, `exp_visab` | Exact native |
| `controller_check_pressed_ext` | `input_native.c` | none | `exp_inputnative0` | | Exact native |
| `controller_check` | `held_native.c` | none | `exp_heldnative0` | `exp_heldverify`, `exp_heldab` | Exact native |
| On-screen touch controls hidden, yes/no prompt preselection | `katana.c`, `katana_menu.c` | none | `exp_touchui`; `exp_btnnodraw0` (buttons drawn with colour writes off instead) | | Port UI |
| Options menu without CONTROLS, the touch controls' page | `katana_menu.c` | `touchControls` (only with 0) | `exp_touchmenu0` | | Port UI |
| Controller prompts from the start (`global.gamepadMode` set after obj_global_controller's Create) | `gamepad_prompts.c` | `touchControls` (only with 0) | `exp_padprompts0` | | Port UI |
| Stick deadzone | `gamepad.c` | `stickDeadzone` | `stickDeadzone=0` | | Input |

### Textures, VRAM, display

| Feature | Files | yyl.cfg | Off-switch | Other switches | Kind |
|---|---|---|---|---|---|
| LZ4 texture pages and the decoder self-test | `katana.c`, `scripts/prepare_apk.py` | none | none: never bypass the self-test | | Required |
| Title pages loaded between frames | `title_pages.c` | none | `exp_titlepages0` | | Exact |
| Drain after the game's texture flush, page recovery | `katana_display.c` | none | `exp_texdrain0`, `exp_texflush0` | | Port fix |
| Heap blocks held for pages outside VRAM (white sprites fix) and for the runner's page decode buffer | `page_reserve.c` (`memalign` wrapped), `main.c` (runner `malloc`/`free`), `page_evict.c` | none | `exp_pagereserve0`, `exp_reservedecode0` (decode block only) | `exp_fragheap` (splits the heap's large holes after Startup, the state of a long session), `exp_fragpools` (also takes vitaGL's pools), `exp_fragdecode` (a 2048x2048 page's decode buffer gets only the decode block, as at the Nov 25 crash) | Port fix |
| Failed page decode never reaches the GPU: evict and retry once, else a blank page retried 1 s later; `glBindTexture` of a name with no vitaGL slot binds a blank texture | `page_evict.c`, `main.c` (GL table) | none | none: never turn it off on the device | `exp_decodeguard0` (test: only the bind guard protects) | Required |
| Application surface size, dynamic resolution | `katana_display.c`, `main.c` | `renderRes`, `dynamicRes` | `exp_appsurf1280` (original size) | `exp_appsurf640`, `exp_dynrestest` | Display |
| vitaGL pools: 2 MB vertex pool, 8 MB parameter buffer, VRAM split | `main.c` | `squeezeMem` | `exp_pool3k`, `exp_parambuf16`, `exp_novramsplit` | | Memory |
| Runner code in physically contiguous memory | `so_util.c` | none | `exp_nophycont` | | Memory |
| NurseryAlloc 8-byte alignment (crash fix) | `katana.c` | none | none | | Fix |
| First-launch status screen | `first_launch.c` | none | none | | Port UI |

### Frame pacing, threads, audio

| Feature | Files | yyl.cfg | Off-switch | Kind |
|---|---|---|---|---|
| vblank pacing; the runner's `Timing_Sleep` skips frame-sized waits | `main.c`, `katana.c` | none | `exp_sleeppace` | Pacing |
| Game thread alone on core 1 | `main.c` | none | `exp_gamecore0` (core 0) | Threads |
| Frame skip (opt-in) | `katana.c` | `frameSkip` | | Visible when on |
| Same-sound limit, voice limit | `sound_limit.c`, `katana.c` | `sameSoundLimit`, `maxVoices` | | Audible |
| OpenSL ES output buffer | `patches/opensles.patch` | `audioGrain` | | Audio |

### Saves, files, boot

| Feature | Files | yyl.cfg | Off-switch | Kind |
|---|---|---|---|---|
| Background save writer | `asyncsave.c`, `netflix.c` | `asyncSave` | `exp_asyncsave0` | A crash right after a checkpoint can lose it |
| Netflix extension offline (cloud slots as files) | `netflix.c` | none | none | Required |
| Thread-safe APK reads | `stdio_mt.c` | none | none | Fix |
| Startup: game.droid and audio groups read on a worker during the CPU-bound setup, handed to the runner's reads | `apk_prefetch.c`, `stdio_mt.c` | none | `exp_noprefetch` | Exact (same bytes) |
| Startup: deflated runner and game.droid inflated while read, CRC checked | `runner_inflate.c`, `apk_prefetch.c`, `scripts/prepare_apk.py` | none | none (stored entries still load) | Exact |
| Startup: game.droid inflated into the block the runner's `MemoryManager::Alloc` returns (its header in front, no copy); the runner's ELF image in the heap's untouched tail while it loads. The newlib heap after Startup is back to ~158 MB before the page blocks (233 MB with the copy) | `apk_prefetch.c`, `main.c`, `sbrk.c` | none | `exp_droidcopy`, `exp_sobufmalloc` (the old layout, tests) | Exact (same bytes) |
| Startup: hashed `Code_Function_Find` during Startup | `fast_find.c` | none | `exp_nofastfind` | Exact; `exp_findverify` |
| Startup: buffered minizip reads, GNU-hash misses, flushing only written code | `ioapi.c`, `so_util.c` | none | none | Exact |
| Netflix logo skipped | `katana.c` | `skipNetflixLogo` | `skipNetflixLogo=0` | Visible |

### Profiling and debugging (debugMode=1)

| Tool | Files | Switch | Output |
|---|---|---|---|
| 2-second profiling report | `main.c` (`frame_profile`), `katana_prof.c`, each file's `*_report()` | `debugMode=1` | `[PROF]` and per-feature lines in `yyl.log` |
| Stage, layer and draw-call profiler | `katana_prof.c` | `shared/profile` file | stage lines in the report |
| Script, event and builtin timing | `katana_prof.c` | `exp_scriptprof` + `scriptprof.txt`, `exp_userprof` + `userprof.txt`, `exp_builtinprof` + `builtinprof.txt` | report |
| Object event profiler | `evprof.c` | `shared/profile` | top events per frame |
| PLT call counter | `callprof.c` | `shared/profile` and `shared/callprof` | most called functions |
| GL call profiler | `glprof.c` | `shared/profile` | GL calls and time |
| Slow-frame log | `main.c` | `exp_spikes` (threshold in ms, default 20) | `[SPIKE]` lines |
| Allocation profilers | `main.c`, `gcalloc.c` | `exp_allocprof`, `exp_gcalloc` | `[GCALLOC]` and allocation lines |
| Freeze locator | `hang_watch.c` | on in debugMode; `exp_hangwatch0` off, `exp_hangdump` core dump after 30 s | `hang.log` |
| Hook registry | `hooks.c` | always | one `[HOOKS]` line at startup |
| Startup timeline | `boot_prof.c` | always in debugMode | `[BOOT]` lines from `main()` on (kept until the log opens) |
| Startup profiler | `boot_prof.c` | `exp_bootprof` | `[BOOTFN]` runner function times, `[BOOTIO]` stdio/zlib counts, `[BOOTSAMPLE]` game-thread samples, `[PREFETCH]` |
| Memory card read benchmark | `boot_prof.c` | `exp_readbench` | `[READBENCH]` MB/s by request size and thread count (reads ~72 MB, slows that boot) |
| Debug requests and screenshots | `screenshot.c`, `katana.c` (`katana_goto_check`) | request files below | |

## Debug files

All in `ux0:data/gms/shared/`. Except `exp_*`, everything here needs `debugMode=1`.

| File | Direction | Format | Read or written | Handled by |
|---|---|---|---|---|
| `yyl.log` | Vita writes | text, `[seconds.ms]` prefix | Buffered, appended every 0.5 s by a writer thread. Never truncated by the loader; `scripts/bench.py` deletes it before a run. | `main.c` |
| `hang.log` | Vita writes | text | Appended when no frame started for 8 s, again every 30 s while frozen | `hang_watch.c` |
| `exp_<name>` | you create | empty, or a number for the four listed above | When the feature installs (see above) | `so_util.c` and the feature |
| `profile` | you create | empty | Once at startup: turns on heavy profiling (stage, event, GL profilers, timing in natives, `[MEM]` lines) | `main.c` |
| `callprof` | you create | empty | At install, only with `profile` | `katana_prof.c`, `callprof.c` |
| `scriptprof.txt` | you create | script names (`draw_shadow`) or raw `_Z` symbols, whitespace separated | At install, with `profile` and `exp_scriptprof` | `katana_prof.c` |
| `userprof.txt` | you create | event function names (`gml_Object_obj_player_Step_0`), whitespace separated | At install, with `profile` and `exp_userprof` | `katana_prof.c` |
| `builtinprof.txt` | you create | GML builtin names (`place_meeting`), whitespace separated | At the first profiling report, with `exp_builtinprof` | `katana_prof.c` |
| `stub_events` | you create | object event names, one per line (`obj_name_Draw_0`) | At install: each listed event returns at once, to measure what it costs | `katana.c` |
| `goto_room` | you create, Vita deletes | room name (`room_club_4_2`) | Polled every 0.5 s by the request thread, acted on by the game thread at the next frame: `room_goto` | `screenshot.c`, `katana.c` |
| `level_complete` | you create, Vita deletes | empty | Same polling: creates obj_level_complete (object 608), as reaching the exit does | same |
| `create_object` | you create, Vita deletes | `<object index>`, `<index> view` (at the view centre), `smoke` or `smoke view` (a smoke vial burst) | Same polling | same |
| `destroy_object` | you create, Vita deletes | object name or index | Same polling: `instance_destroy(object)`, children included, with Destroy events | same |
| `screenshot.req` | you create, Vita deletes | empty | Same polling. The displayed frame goes to `screenshot.raw`: a 12-byte header (width, height, pitch) and ABGR8888 pixels, written as `screenshot.tmp` and renamed. The first-launch screen answers it too. | `screenshot.c`, `first_launch.c`; read with `scripts/vita_screenshot.py` |
| `glsl/<n>.glsl` | Vita writes | shader source | With `debugShaders=1` | `main.c` |

Request files hold at most 63 bytes. A request waits until the game thread takes it; a second copy of the file before
then is read after it.

## Hook rules

The helpers are in `hooks.c` / `hooks.h`, except `hook_addr` and `so_got_slot` in `so_util.c`.

| Helper | What it does | Composes with |
|---|---|---|
| `hook_addr(addr, dst)` | Overwrites the first two words of `addr` with `ldr pc, [pc, #-4]` and `dst`. No trampoline, no check: the original can't be called and an earlier hook on `addr` is silently replaced. | nothing |
| `entry_hook(entry, replacement)` | Copies the two-instruction prologue (`push {..., lr}` then `add fp, sp, #imm` or `mov fp, sp`) into a trampoline from `katana_rx_alloc`, then `hook_addr`. Returns the trampoline (call it to run the original), or NULL: no symbol, another prologue, or the function is already hooked, RX arena full. | GOT-slot interception, `gml_replace` |
| `gml_replace(table, name, wrapper)` | Swaps the compiled event `name` in the runner's `g_GMLFuncs` table. Returns the previous entry (NULL if not found). A second `gml_replace` on the same name chains: it gets the first wrapper as its original. | everything |
| `so_got_slot(mod, symbol)` | Returns the PLT GOT slot of an imported or exported symbol; writing it redirects calls that go through the PLT. The function body is untouched. | entry hooks, natives' prologue checks |
| `stub_event(symbol)` | Writes `bx lr` over the first word: the event returns at once. Used for `dancerGlow=0`, `cathedralDust=0` and `stub_events`. | nothing |
| `katana_rx_alloc(size)` | 64 KB of executable memory for trampolines and veneers, 16-byte aligned, never freed. | |
| Veneers (`so_alloc_near`) | Rewrite one `bl` call site to go through an 8-byte veneer in the code cave or patch block. Only that call site changes. Used by `trail_lights.c`, `bike_draw.c`, `cone_shadow.c`, `explosion_lite.c`, `far_rocket.c` and others, often on `with` iterator calls (`YYGML_NewWithIterator`, `YYGML_WithIteratorNext`). The site must be within `bl` range (32 MB); call sites far from the cave can't be patched. | entry hooks on the callee |

Rules:

1. One entry hook per function. Once a function is entry-hooked its first word is `ldr pc, [pc, #-4]`, so a second
   `entry_hook` returns NULL and that feature doesn't install. `hooks.c` records every `entry_hook` and `stub_event`
   with its call site, logs `[HOOKS] entry_hook NOT installed on <symbol> ... already hooked by ...` when this
   happens, and `hooks_report()` (end of `katana_late_install`) prints one `[HOOKS]` line with the hook count, RX
   arena use and every hook that did not install. Check that line after adding a hook. Hooks a native installs with
   its own `hook_addr` are not in the registry, so the second hook's failure is listed without the owner.
2. A later `hook_addr` or `stub_event` on a hooked function silently replaces the first hook. `stub_event` logs it;
   plain `hook_addr` does not.
3. Prefer GOT-slot interception when the call sites go through the PLT: it composes with an entry hook on the same
   function and leaves the prologue that natives check intact. `scripts/patch_plt_calls.py` rewrites most PLT calls
   of the deployed runner into direct `bl`, so a GOT slot only sees what that script leaves alone (its
   `DEFAULT_EXCLUDE`: `_Z12DoAStep_Drawv`, `BZ2_bzBuffToBuffDecompress`). For anything else, add the symbol to
   `DEFAULT_EXCLUDE` or use an entry hook.
4. Natives check the runner helpers they inline or bypass. `clubber_native.c` and `dustspawner_native.c` compare the
   first two words and the GOT slot of each helper (37 and about 50) at install and on every call;
   `smokedraw_native.c` does it once per frame and after any new hook (`so_hook_generation`); `light_fast.c`,
   `light_fast_inline.c`, `visibility_native.c` and `virtual_capture.c` check the prologues of the entries they
   replace at install. A native that finds a helper hooked declines and the original GML runs. So a verify or
   profiling hook can change which path runs: `exp_fogskipverify` entry-hooks `YYGML_NewWithIterator`, which is in
   the clubber and dust tables; `exp_lbverify` entry-hooks `Variable_SetValue_Direct`, which smokedraw bypasses;
   heavy profiling (`profile`) swaps GOT slots of `draw_self` and `draw_sprite_ext` and entry-hooks stage functions.
   Check the natives' report lines when you profile.
5. Install order matters. Dependencies in the code today:
   - obj_particle user event 15: `katana.c` swaps in `particle_draw`, which calls `fog_trail_install(particle_native_install(orig))`.
     The call order is `particle_draw` (alternate frames), then `fog_trail.c`, then `particle_native.c`, then the original.
   - `room_lighting_install` runs after `blood_drops_install`: both wrap obj_opto_master's Room Start.
   - `light_fast.c` and `light_fast_inline.c` hook the same two entries; `katana.c` installs only one.
   - obj_rain Draw: `katana.c` swaps in `rain_draw` first; cheap rain's Draw wraps it.
   - `DoAStep_Draw` GOT slot: frame skip takes it first (only with `frameSkip=1`), then `katana_display_hook_draw`
     at the end of `patch_performance`. `katana_prof.c` entry-hooks the same function afterwards.
   - `katana_prof_install` runs after `patch_performance`, so its entry hooks fail where a native already hooked the
     function (`inside_screen`, hooked by `visibility_native.c`) and it falls back to the GOT slot.
   - `clubber_native.c` installs after `visibility_native.c`, which hooks `inside_screen`, one of clubber's checked
     helpers. With the default flags the clubber native therefore never installs, and `dancerGlow=1` runs the GML.
   - `fog_skip_install` runs inside `fog_install` (`katana.c`), after the fog Create/Step hooks.
   - `smokedraw_native.c`'s verify recorder hooks its scripts on the first verified event, after every native has
     installed; on an entry another native hooked it puts itself in front.
   - `move_contact_native.c` installs its five entry hooks all or none.
   - `katana_late_install` (once, after the runner's Startup, before the main loop) holds what needs the function
     registry (`the_functions`) or `g_FUNC_*` ids: sound limit, voices, input and held natives, virtual captures,
     the clothesline and tilemap builtin swaps, dossier, wavy and shadow natives.
6. Every install prints `installed` or `NOT installed` in `yyl.log`. A new feature does the same.

## Building

```
uv run build.py "<Katana ZERO APK>"
```

The APK is only needed until the LiveArea art exists in `livearea/`. When another session shares the SDK
(`C:/vitasdk-softfp`) and dependency folder (`C:/vitasdk-build`), build with `--no-deps`, so an experimental vitaGL
someone installed stays. Use `--deps` on purpose only, for example after a `patches/vitagl.patch` change.

Check every build: grep the output case-insensitively for `error` and require the `Built target` lines. `build.py`
deletes the old `eboot.bin` and VPK before building, so a failed build leaves none; a plain `cmake --build` leaves the
old ones in place and every later test runs old code.

Sources: `loader/CMakeLists.txt` compiles every `loader/loader/*.c` (a `file(GLOB ... CONFIGURE_DEPENDS)`), minus
`qoi_decoder.c`, which belongs to the selector build. A new non-exact `.c` file needs no CMake edit: `build.py`
re-runs the configure step, and `CONFIGURE_DEPENDS` re-checks the glob on a plain `cmake --build`. A new exact
native adds one line to `EXACT_SOURCES` in `loader/exact_sources.cmake` (sorted, one file per line). Files verified
with other flags (`dustspawner_native.c`, `smokedraw_native.c`, `light_fast.c`, `light_fast_inline.c`) have their
own lines there; keep their flags as they are.

## Tests and verifiers

| Test | How to run | In CI |
|---|---|---|
| Python unit tests (`tests/test_*.py`: build script, APK tooling, `vita_common.py`) | `uv run python -m unittest discover -s tests` | yes |
| `tests/asyncsave_test.c`: the background save writer on the host (needs glibc) | `gcc -O1 -g -Wall -pthread -Iloader/loader tests/asyncsave_test.c -o asyncsave-test && ./asyncsave-test`; also with `-fsanitize=thread` and `-fsanitize=address,undefined` | yes, plain and under both sanitizers |
| `tests/loader_core.c`: save file replacement, `yyl.cfg` parsing, ELF loading, shared APK reads | see its header; needs `libyoyo.so` from the 1.1.10 APK | no |

On the Vita, the `exp_*verify` switches above are the differential checks: start the game with the switch file and
`debugMode=1`, play the scene, then read the mismatch counts in `yyl.log`. They must be 0.

Offline checks are not in the repository. The Unicorn and differential harnesses that compare natives against the
runner's ARM code (for example `work/smokedraw/smokedraw_verify.py`), the decompiled GML the comments cite
(`work/decomp/`), the test saves (`work/testsaves/`) and the headless Vita3K runner live in the untracked `work/`
directory of the author's checkout. `tools/verify/` holds only the stub headers the host test builds include
(`collision_stub.h`, `smokedraw_test.h`). Until the harnesses are committed, "exact" for a native rests on its
on-device verify switch and the description in its header.

### Scripts

The scripts that talk to the console use vitacompanion (FTP on 1337, commands on 1338). The console address comes
from `VITA_HOST` (default 192.168.1.151) through `scripts/vita_common.py`, which also provides `device_lock`, a lock
on `~/.t3/vita-device.lock` for sessions that share one console. The scripts don't take it themselves; wrap a run:
`python scripts/vita_common.py hold "<who: what>" -- python scripts/bench.py ...`.

| Script | Does |
|---|---|
| `bench.py` | Installs `build/app/eboot.bin`, starts the game, records the `[PROF]` lines in a room (`--room`, `--save`, `--exp`, `--profile`). Needs `debugMode=1`. |
| `vita_saves.py` | Backs up or restores the game's saves over FTP. Back up before installing test saves. |
| `vita_screenshot.py` | Takes a screenshot of the running game through `screenshot.req`. Needs `debugMode=1`. |
| `room_suite.sh` | Runs `bench.py` over a list of heavy rooms with test saves from `work/testsaves/`. |
| `sweep.sh` | Profiles a list of rooms (test saves from `work/testsaves/`, list in `work/sweep_rooms.txt`). |
| `fast_sweep.py` | Boots once and jumps between rooms with `goto_room`. Triage only: some rooms load incomplete. |
| `patch_plt_calls.py` | Rewrites the runner's PLT calls into direct branches (run by `prepare_apk.py`). Upload runner builds only from it. |
| `prepare_apk.py` | Makes the `game.apk` the loader reads: LZ4 texture pages, Vita button glyphs, patched runner. |
| `qoi_emulate.py` | Runs the runner's own QOI decoder in Unicorn on re-encoded pages. |
| `extract_art.py`, `make_livearea.py`, `vita_glyphs.py` | Art from the APK: sprites, LiveArea images, button glyphs. |
| `env.sh` | Sets up the toolchain environment in Git Bash for running cmake by hand. |

The test saves in `work/testsaves/` are not in the repository either.

## Adding a feature

1. Put it in its own file in `loader/loader/` with a header comment: what it changes, the measurement, how it stays
   exact or what it shows differently, its switches.
2. Give it `xxx_install()` (logs `installed` / `NOT installed`) and, if it has counters, `xxx_report(int frames)`
   called from the profiling report.
3. Add an off-switch `exp_xxx0`, checked at install. For a native, add an `exp_xxxverify` that compares against the
   original while the game runs.
4. Call the install from `patch_performance()` (runner loaded) or `katana_late_install()` (needs the function
   registry), and state in a comment at the call any order it depends on.
5. If it is exact, add the file to `loader/exact_sources.cmake`.
6. If it has a `yyl.cfg` key: parse it in `loadConfig`, add a row to [settings.md](settings.md), and if it changes what
   the player sees or hears, a row to [changes.md](changes.md).
7. Use the hook helpers above and check the `[HOOKS]` line: nothing new under "not installed", and no other feature's
   `NOT installed` line in `yyl.log`.
8. In the pull request, give the verifier you ran (the `exp_*verify` switch and the scene, or the offline harness) and
   its result.
