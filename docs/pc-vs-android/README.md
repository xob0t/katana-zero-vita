# Katana ZERO: PC (Steam) vs Netflix Android 1.1.10, gameplay differences (static audit, Nov 24 2026)

Report-only. Both builds are YoYo Compiler native code: the PC build is `Katana ZERO.exe`, x86, GMS 2.2-era YYC;
the Android build is `libyoyo.so`, ARMv7, GMS 2.3 YYC. Nothing was run: not the PC game, not the Vita.

PC addresses are VAs in `Katana ZERO.exe`. Android addresses are offsets in `apk110/lib/armeabi-v7a/libyoyo.so`.
"L" is the GML source line from the YYC line markers; Android lines are usually PC +1..+3 (2.3 wrapper lines and
edits). A "tick" is one game step at `global.fr = 1` (60 per second; slow-mo scales the counters by `fr`).


> **In this repository:** the per-area notes are next to this file. The tools are in
> [tools/pc-vs-android/](../../tools/pc-vs-android/) (with the earlier `bulletfreeze/` helpers), and their README
> explains how to run them. Their generated `out/` lists and the game files they read are not committed; paths in the
> tools still point at the untracked `work/` folders where they were developed.
>
> **What the port does with it:**
> - Gun timing (items 3-5) is PC by default since #94 (`enemyTiming`, **GAME → ENEMY TIMING**).
> - Melee and slash rules, plus the level timer and slow-mo meter, are being switched to PC under their own options.
> - The rest stays as in the Android release.

## 1. Real gameplay differences, by player impact

| # | Area | Object / script, event | PC (address: value or logic) | Android (address: value or logic) | Effect in plain words | Conf. |
|---|---|---|---|---|---|---|
| 1 | Level time limit | Every level controller `obj_cutscene_<level>_Create_0/_Step_0` (89 functions); `obj_master` Create L78; global init (`obj_init` Create L171 → Android `mobile_reset_game_data`) | `global.max_time` = 7000 (most levels), 6000, 8000, 8100, 8300, 8600, 9000; master default 4000; init 6000. E.g. chinatown_2 Create L26 0xcc5e7c `= 7000`; master Create L78 0xe0d863 `= 4000`; init L171 0xa25c1b `= 6000` | ×1.5 everywhere: 10500, 9000, 12000, 11850 (club_0, ×1.46), 12450, 12900, 13500; master 6000 (L83 0x4365748); init 9000. E.g. chinatown_2 Create L27 0x38bd674 `global.max_time = 10500` | Each level's countdown is 50% longer (club_0: 46%). Fewer time-out deaths | high |
| 2 | Slow-mo (chronos) | `obj_player` Other_13 (katana setup, runs on every player spawn via Other_11) | L9 0x1378bde `bullettime_max = 380` | L11 0x462e0e4 `bullettime_max = 760` | Zero's slow-mo meter lasts twice as long, about 12.7 s instead of 6.3 s. Drain (−1/tick, max −0.1) and recharge (+0.5/tick) are unchanged. Dragon after a Zero→dragon cutscene: 880 instead of 500 | high |
| 3 | Enemy gunfire | `AI_init`; Step of obj_enemy, obj_enemy_cop, obj_enemy_sci, obj_enemy_shieldcop, obj_enemy_shotgun | AI_init L80 0x59ae23 `first_attack_timer = 15`; fire gate `shoot_timer > 20` (enemy L65 0x108b969, cop L67 0x10a0b2b) / `> 40` (sci L62 0x10d66b4, shieldcop L207 0x10e8415, shotgun pump L21 0x10f73d3 and shot L67 0x10f8780) | AI_init L82 0x151003c `first_attack_timer = 30`, L83 0x1510098 `sec_attack_timer = 6`; gates `> 20 + sec_attack_timer` (L66 0x3e791fc, cop L68 0x3d99b40) / `> 40 + sec_attack_timer` (sci L63 0x3e18558, shieldcop L208 0x3e2d12c, shotgun L22 0x3e44bd0 / L68 0x3e4dc1c) | Every gun enemy shoots 6 ticks (0.1 s) later after aiming, on every shot; for the shotgun both the pump and the shot. First-encounter grace (`first_attack_timer`) is twice as long. Both sides use strict `>` (correcting the brief's `>=`) | high |
| 4 | Enemy gunfire | `obj_enemy_machinegun` Step | L76 0x110d265 `shoot_timer > 8`, checked before every round of a burst | L77 0x3dd0d18 `> 8 + sec_attack_timer` | Machine-gunner fires one round every 15 ticks instead of 9: about 40% slower bursts | high |
| 5 | Boss gunfire | `obj_headhunter` Create/Step (rifle attack) | Step L275 0x119d5be `if shoot_timer > 15` | Create L18 0x40e935c `sec_attack_timer = 6`; Step L276 0x40f5e3c `> 15 + sec_attack_timer` | Headhunter's rifle attack (state 5, aim laser) fires every ~22 ticks instead of ~16: bursts about 35% slower | high |
| 6 | Enemy melee | `whip_timer` (melee cooldown) after a shot: obj_enemy, cop, machinegun Step; after a melee animation: Other_24 of enemy, cop, machinegun, shieldcop, shotgun, grunt, meele, redpomp | After a shot `whip_timer = 15` (enemy L148 0x108dfbc, cop L192 0x10a448e, mg L151 0x110f58a); after melee 30 / 20 (grunt, meele) / 10, 25, 15 (redpomp) (e.g. enemy Other_24 L128 0x10986d5) | Same value `+ whip_attack_timer` (= 4, AI_init L84 0x15100f0): e.g. enemy Step L149 0x3e7d93c, enemy Other_24 L129 0x3e86e6c, grunt L101 0x3dc0984 | Every enemy melee (pistol-whip, grunt, meele, redpomp) waits 4 ticks longer between attacks; gunners wait 4 ticks longer before meleeing after a shot | high |
| 7 | Player attack | `enemy_hurt` (katana slash vs enemy) | L6/L7 line-of-sight `collision_line(..., obj_block)` checks, no exclusion | L11 0x1993ef4 `with (obj_tempdoor) y -= 100000` before the checks, L30-32 restore | `obj_tempdoor` (a temporary door left by door breaks/kicks/explosions) is a child of obj_block. On PC it blocks a slash kill; on Android the slash kills through it | high (code), medium (frequency) |
| 8 | Player attack | `obj_slash` Collision with obj_door (PC GUID e61f8e37) + Step | L49 0x13857d8 `harmless = 1` immediately | L54 0x48d5318 `trigger_harmless = 2`, counted down in Step (0x48d2318) before `harmless = true` | A slash that opens a door stays deadly 2-3 more frames, so it can kill an enemy right behind the door | high |
| 9 | Player movement | `player_rolling` | L74-L139: a roll pressed during the forced roll extension (stuck under a ceiling/obstacle) is ignored | L78 0x32b9d34 latches `roll_end`; L113 0x32bc1fc `key_roll` pressed sets `queue_roll`; L152-165 0x32bb600 starts the queued roll | Roll input is buffered during roll extensions: rolls chain on Android (gamepad too) | high |
| 10 | Stealth / smoke | `obj_smoke_spawner` Step/Create + Android-only `obj_opto_master` (Other_4 defaults, Other_15 adaptive profile) | L5 0x148798e: one cloud per tick, cap `instance_number(obj_smokecloud) <= 149` | L4 0x48f7740 rate `global.smokeFreq` (default 1); L8 0x48f783c cap `global.smokeMax` (150) ×0.7 when `fps_real < 100`; the opto profile can lower smokeMax/Freq/Scale/Density after repeated FPS dips (Other_15 L37-L110, values not decoded) | Vents and pipes make fewer clouds on slow hardware (105 instead of 150 below 100 fps uncapped, less after dips): less smoke to hide in from enemies, turrets and cameras. Relevant to the Vita | high (spawner), medium (opto values) |
| 11 | Level script | `obj_wall_cam` Create/Step | No such logic; detection (L59 0x13f9868 `turret_line_of_sight`) always runs | Step L10 0xd0a178 `if room == room_club_2 && obj_broken_pipe.dead` → L12 `override_off = 1`; L16 0xd0a44c drop lock; L74 0xd0b7f4 detection only `if !override_off` | In club_2, once the broken pipe is destroyed, the wall cameras stop working for good | high (code) |
| 12 | Hazard | `obj_heli_mortar_explosion` Create (bike chase helicopter) | L8 0x180b3a4 `if speedrun && speedrun_hardmode: scale ×0.85` | L8 0x415783c `if speedrun: ×0.85 else (L15 0x41579d0 / L16 0x4157a98) ×0.95` | Normal play: mortar blasts are 5% smaller per axis (≈10% less area; bikeplayer is hit via place_meeting). Non-hard-mode speedruns: 15% smaller on Android | high (code), medium (hitbox) |
| 13 | Enemy AI | `obj_smokescreen_master` Step + `ai_break_idle` | ai_break_idle L4 0x5e0c25 `vsp = 0` at once for a smoked idle enemy | smokescreen L20/L22 0x4907860 `falling_smoke_timer = 3` if `vsp != 0`; ai_break_idle L6 0x14fb490 zeroes `vsp` only when the timer reaches 0 | An enemy smoked in mid-air keeps falling for 3 ticks instead of freezing in the air (a fix) | high (code) |
| 14 | Aiming | `player_get_throwdirection`, `player_get_attackdirection`, `obj_bikeplayer` Step (right-stick sword aim) | Stick counts as aiming only past `global.controller_threshold` | throw L21 0x32872b0 and bike L224 0x359a1a8: deadzone forced to 0.001; attack L11-24 0x3284c58/0x3284d74: analog slash angle only if `global.gamepadMode`, else 8-way | Tiny stick tilts aim throws and bike slashes at the exact stick angle; on-foot slash aim is analog only in gamepadMode (set on stick use). On Vita the loader's 0.2 radial deadzone hides most of this | high (code), low (impact) |
| 15 | Death slow-mo | `obj_master` Step (Android `obj_master` Other_10) | L112 0xe10146 `gameover_slowdown -= 0.1`; L115 0xe1024d `gsp += 0.0001` | L118 0x4369eec `-= 0.12`; L121 0x4369fb8 `gsp += 0.00015` | The slow-motion ramp after the player dies drops 20% faster and recovers 50% faster | medium |
| 16 | Checkpoints | Room data (ROOM chunk): room_prison_0_return, prison_1_return, prison_3_return | obj_respawn_point at x 3040 / 2080 / 2256; obj_temp_block (2160,272), (2368,384); obj_end_block x 16 | x 2976 / 2048 / 2272; temp_block (2144,288), (2336,384); end_block x 32 | Respawn positions in the prison "return" levels moved by 16-64 px | high (data), low (impact) |
| 17 | Option | `player_moving_ground` (crouch while running on a jump-through platform) | L173 0x42766f always drops through | L211 0x32a193c rolls instead unless `global.moving_dropdown` | With the option on (mobile reset sets it on) it matches PC | medium |
| 18 | Feel | `player_casual_walking` (story walking only) | Acceleration model | L119-223: speed follows the animation frames | Apartment/story walking feel only | low |
| 19 | Level data | room_motel_4 room creation code | 6 instances get `moving = 0` | 5 | One instance no longer starts with `moving = 0` | medium (data), low |

Bosses otherwise: no difference found in Headhunter (other events), V / V_fight / V_limo, the psychiatrist boss
(open_timer `irandom(20)+20`, attack `choose(5)`/`choose(9)` on both), Dragon, Snow, Kissyface, Electrohead, Leviathan,
the nightmare levels (Android-only nightmare_girl Step L64-130 is dead code under `if (playing && false)`), or the bike
chase waves (all 31 `switch(wave)` cases, spawn calls and wave timers 100/150/60/130/30/300 match). Hazards
(fanblade knockback 20, stomper selection, minecart, conveyors, bunker7 volleys apart from item 3) match.
Bullet/turret `parent = -1` (PC 2.2 `self`) vs `-4`/`id` on Android: no effect found (notes_A §2, notes_C C7).

Calibration (enemy fire, item 3): in the tutorial, slow-mo starts with `shoot_timer` ≈16. PC fires at fr 0.59 and the
bullet lands in front of Zero; Android fires at fr 0.09 and the bullet freezes inside the muzzle flash
(work/bulletfreeze/README.md).

### Vita-port issue found on the way (not a PC/Android difference)
Android gates ground crouching with `allowCrouch || obj_screenJoystick.wasNeutral > 0 || control_override > 0`
(player_idle_ground L100 0x32978b4, player_moving_ground L204/L236, player_walking L124, slope_land_check L47).
`allowCrouch` is always false. `wasNeutral` is 0 from Create (0x487c10c) and becomes 20 only in obj_screenJoystick Step
(L105 0x48801ac). The loader's touchControls=0 (`loader/loader/touch_controls.c`, PR #82) skips that Step, so
`wasNeutral` stays 0. The likely result is that idle crouch, crouch-roll, crouch drop-through and slope crouch don't
work on the Vita, while key_roll rolls still do. This comes from reading the code only, so try crouching on the device.
A fix would be to set `wasNeutral = 20` when skipping the Step. Details: notes_B_player.md §0.

## 2. Platform-only changes (brief)
- Input: touch controls (obj_screenJoystick, obj_screenBtn, obj_playerBtn gestures, obj_onScreenKeyboard,
  mobile_control_menu, cursorCircleEffect), `fakeJumpHold`/`launchGo`/`anglePos`/`walking`/`wasNeutral` touch terms in
  the player scripts, `controller_check*` mode 3 (touch + gamepad), key id 9997 renamed 99970, keyboard W binding
  dropped, `obj_global_controller.control = 3`, the mouse look-around in obj_camera replaced by room-border clamps.
- Netflix: SDK objects (obj_netflixSDK*, Nfx* scripts, Page* UI), cloud saves with conflict UI (obj_continuegame,
  obj_savegame, load_*/save_* with hash checks), telemetry (scrTelemetryControl), menu button, Netflix access
  button, title menu (obj_start_title_menu replaces obj_titlemenu in room_title).
- Achievements: Steam `trigger_steam_achievement` calls removed or routed to `trigger_xbox_achievement`; xbox
  flash-save, rich presence and pause objects compiled in (inactive).
- Performance and rendering: obj_opto_master adaptive quality (rim shading, trails, blood timer, particle scaler,
  smoke; its smoke part is gameplay item 10), flashlight `simple = 1` (1 light instead of 3), halogen flicker,
  denser fog in haunted_light1, wall-cam red sprite, hair chosen via `asset_get_index`, audio emitters for bike bullets.
- Menus and options: options menu (mobile controls, cloud sync, moving_dropdown, speedrun overrides), pause menu
  layout and Netflix entries, level select layout and unlocking (`levelsUnlocked`, `sceneUnlocked`, `warpLevel`),
  speedrun results screen, credits, apartment-cutscene shortening (`shortCutscene`), help/debug keys.
- Structure: 14 master objects' Step events run as user events from obj_timeMaster Step (0xc0a158, fixed order, no
  timing logic); global init moved from obj_init Create to `mobile_reset_game_data`; player speeds moved into
  obj_player user event 5 (same values); PC's inline "fix glitched player position" block moved into
  `physics_fix_glitched_position` (same logic); `static` renamed obStatic/fxStatic (2.3 keyword).

## 3. Coverage

| | PC | Android |
|---|---|---|
| gml functions found | 6779 (Object 4088, RoomCC 2096, Script 587, Room 5, GlobalScript 3) | 6646 (Object 3072, RoomCC 2095, Script 784, GlobalScript 690, Room 5) |
| matched by name | 5653, including 23 aliases: 18 master Step events → user events, 5 GUID-named collision events | |
| one side only | 1126: 1040 `PreCreate_0` stubs (80 bytes), 83 empty 16-byte events, `init_lines` (split into init_linesA-D on Android), `obj_splash_online_Other_10` (platform), `RoomCC_room_motel_4_11` (`moving = 0`, item 19) | 993: 687 2.3 GlobalScript wrappers, 198 scripts (Netflix/UI/xbox/mobile/save/hash/RNG, `physics_fix_glitched_position`), 108 object events (Netflix, touch, xbox, timeMaster, new user events of moved code, `obj_stomper_invisible`, `obj_cutscene_factory_3_Alarm_0`) |
| variable names | 3843 YYVAR names | 5267 g_VAR names; 1429 not in the PC exe, 724 after removing 2.3 script/method names (out/android_only_vars.txt) |
| room instances | 12659 in 193 rooms | 12654; only room_title differs in count |

Automatic diffing (all 5653 matched pairs; tools/README.md):
- Per GML line, the extractors record variables, doubles (vldr/vmov/movw-movt pairs, x86 qword loads and
  immediate hi/lo stores), int operands, builtin and gml calls, asset refs and strings. Tokens are aligned by line
  order with code-motion cancellation (`linediff.py`); asset indices are mapped by name through both data files.
- 2964 pairs have no difference after noise removal. Function-level number differences: 764 pairs, 478 after the
  noise classes below; gml call-set differences: 75; variable-set differences: 316; room creation code: 2095 pairs
  matched by content, 81 differing, all encoding noise (tools/roomcc.py).
- Hand inspection (both disassemblies read at the cited lines): every Android-only gameplay variable (out/varrefs.txt,
  ~75 names), all 478 constant-review entries triaged, all 75 call-review entries, every enemy, player, boss,
  hazard and master function flagged by any list, plus the AI_init/enemy Step/Other_24 families line by line
  (notes_A_enemies.md, notes_B_player.md, notes_C_bosses.md, notes_D_global.md).

Noise classes removed (each verified on examples):
- Asset indices: rooms, sprites, objects and sounds are ordered differently (spr_nothing 1750 on PC, 1196 on Android).
  Mapped by name; Android kind-15 refs (type word `0x0100000T`) as well.
- x86 compiler: `/c` folded to `*(1/c)`, float32-rounded constants (0.19999998807907104), 2^-7/2^31 rounding helpers,
  float32 lengthdir lengths read as doubles (512, 262144, 805306368), constant RValues for clamp bounds.
- ARM compiler: constants kept in a VFP register across statements, constant folding (`7 + round(6)` → 13),
  integer operands (`x += 200` as `operator+=(int)`), switch-case tables (dialogue scripts, bike waves), line markers
  scheduled late.
- 2.3 runtime: builtin variables as g_VAR, `instance_exists`/`Global`/`Other` calls, error strings, debug messages.

## 4. Gaps
- **Boss events not read line by line**: V_fight Step, psychboss_master Step/Other_15 (the ARM `choose` argument
  values are not decoded), kissyface Step, psychboss_giant Step. `check_light_collision*` (PC-only 8192/131072) was
  not checked.
- **Logic-only edits.** A changed comparison operator or branch that keeps the same variables, numbers and calls
  is invisible to the token diff. It was caught only where the code was read by hand: AI_init, every enemy Step,
  Other_24 and shoot/whip gates, player_rolling, player_bullettime_check, player_air/idle/moving_ground, obj_slash,
  obj_master, the boss functions listed in notes_C. Not read line by line: player_hurt, player_attack,
  player_airdodge, player_walljump, most Draw events, the obj_enemy Step L42-47 aim condition, obj_stomper Step,
  obj_minecart Step L105-138, obj_conveyer_belt Step.
- **Int operands.** They only cancel a same-valued double on the other side and are never reported alone, so a
  change confined to an integer operand (`x += 3` → `x += 4`) is missed unless the code was read.
- **Switch tables.** Case constants are not compared automatically (dialogue_* scripts and similar, about 40
  functions). Bike waves were checked by hand; dialogue flow was not.
- **Variable recall.** x86 caches variable pointers per function and ARM per line, so variable-set differences are
  leads. 316 functions were listed; those outside gameplay objects were not all opened.
- **Data files.** Rooms (instances, sizes, speeds) and room creation code were compared. Object definitions
  (parents, masks, persistence), sprite collision masks, sounds and timelines were not, apart from the
  obj_tempdoor parent chain.
- **Unresolved values.** The exact numbers of the obj_opto_master Other_15 low-FPS profile, the default of
  `global.controller_threshold` and the meaning of `gsp` (item 15). `global.gamepadMode` on Vita: false until the
  first stick move; since `gamepad_prompts.c` it is true from the start with touchControls=0 (item 14 then matches PC).
- **Function extents.** On PC, a function's extent is the code up to the next gml function. The last function,
  `init_lines` (dialogue text), was capped at 1 MB.

## 5. Files
- `tools/pc-vs-android/`: `pcbin.py` (PE + GML table), `pc_extract.py`, `and_extract.py`, `tokens.py`, `match_diff.py`,
  `linediff.py`, `funcdiff.py`, `review_list.py`, `vardiff.py`, `varrefs.py`, `roomcc.py`, `rooms_diff.py`, `show.py`;
  `tools/pc-vs-android/README.md` explains how to run them.
- `out/`: features (`pc_features.json`, `android_features.json`), `match.json`, `linediff.txt`, `funcdiff.txt`,
  `const_review.txt`, `call_review.txt`, `vardiff.txt`, `varrefs.txt`, `roomcc.txt`, `rooms_diff.txt`.
- Per-batch notes with every inspected function: `notes_A_enemies.md`, `notes_B_player.md`, `notes_C_bosses.md`,
  `notes_D_global.md`; scratch tools of the batch reviewers in `scratch_A/`, `scratch_B/`, `scratch_C/`.
