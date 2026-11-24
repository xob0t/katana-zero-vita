# Batch C: bosses, special enemies, level scripts (PC Steam YYC x86 vs Android 1.1.10 YYC ARM)

## How this was done

I worked through out/const_review.txt, out/vardiff.txt, out/call_review.txt and out/linediff.txt for the 778
candidates. For every lead I read the PC disassembly and the Android annotated ARM.

Scratch tools in `scratch_C/`:
- `pcr.py`: annotated PC disassembly by GML line or by address range.
- `anr.sh`: condensed Android annotate output by GML line.
- `asset.py`: asset name for an index on either side.
- `objev.py`: OBJT event tables from data.win and game.droid.
- `winmatch.py`, `cntdiff.py`, `numdiff.py`: extra constant scans. They were too noisy because the ARM build keeps
  constants in registers and the PC line markers are out of order. Ignore their output.

Two helper agents did part of the reading. Their findings are marked [bike] and [hazards]:
- [bike]: bike chase objects. Its tools and per-function results are in `scratch_C/bike/`; `cfgcmp.py` lines up the
  two builds line by line.
- [hazards]: level hazards and level scripts. Its listings are in `scratch_C/hazards/`.

Not in scope: Draw events, unless they change gameplay. `global.max_time` (1.5x, already known).

## 1. Real gameplay differences

| # | Object / event | GML line PC / AN | PC (addr, logic) | Android (addr, logic) | Effect | Conf. / how verified |
|---|---|---|---|---|---|---|
| C1 | obj_headhunter Step_0 + Create_0 | Step PC L274-277 / AN L275-278; Create AN L18 (Android only) | 0x119d5be: `if shoot_timer > 15` (addsd -15.0 at 0x1b59420) | Create 0x40e935c: `sec_attack_timer = 6`. Step 0x40f5e3c: `if shoot_timer > 15 + sec_attack_timer` (YYCompareVal == 1). Reset to 0 at L278, `shoot_count -= 1` at L280. | Headhunter's rifle state (state 5, aim laser) fires one shot every ~22 frames instead of every ~16 (`shoot_timer += 1` per step at L274/L275), so her bursts are about 35% slower. | High. Both sides read; Create value read on ARM. |
| C2 | obj_bikeplayer Step_0 [bike] | PC L219-220 / AN L220, 224, 226 | 0x177718f: local deadzone = `global.controller_threshold`. 0x1777288: compares right stick axes 32785/32786 against it. | 0x359a18c: same assignment. 0x359a1a8 (L224): overwritten with 0.001. 0x359a294 (L226): compared with 0.001. | In the bike chase, right-stick sword aiming ignores the deadzone setting; any deflection above 0.001 aims. Gamepad only. | High (agent read both sides). The PC default of `controller_threshold` was not looked up. |
| C3 | obj_wall_cam Step_0 + Create [hazards] | Android only: Step L10-19 and L74; Create L50 | PC 0x13f8980: no such code. Detection always runs: L59 0x13f9868 `turret_line_of_sight`, then L61 `following_player = 1`. | 0xd0a178: `if room == room_club_2 && obj_broken_pipe.dead` then `override_off = true` (0xd0a398), and `following_player` / `following_timeout` are cleared (0xd0a44c). 0xd0b7f4 (L74) skips detection L76-79 while `override_off` is set. | In club_2, once the broken pipe is destroyed, wall cams stop tracking for good. On PC, the smoke only blinds them while it sits between camera and player. | High for the code. Room contents not confirmed. |
| C4 | obj_heli_mortar_explosion Create_0 [hazards] (outside the batch list, found by the agent) | PC L8-11 / AN L8-16 | 0x180b3a4: scale x0.85 only if `speedrun && speedrun_hardmode`. | 0x415783c: x0.85 if `speedrun` alone; otherwise x0.95 at 0x41579d0 / 0x4157a98. | The mortar blast hits the bike player through place_meeting (PC 0x178106c / AN 0x35a7978). On Android the blast is 5% smaller per axis in normal play and 15% smaller in a non-hard-mode speedrun. | High for the code. Assumes the mask scales with image scale. |
| C5 | obj_smoke_spawner Step_0 + Create [hazards] (outside the batch list) | PC L3-7 / AN L0-27 | Spawns every tick while there are at most 149 obj_smokecloud (0x148798e). | Spawns when `timer >= global.smokeFreq` (0x48f7740). Cap is `smokeMax * 0.7` when `fps_real < 100` (0x48f783c). Defaults are smokeMax 150 and smokeFreq 1 (opto_master Other_4, 0x456b9dc). | Below an uncapped 100 fps, smoke vents top out at 105 clouds instead of 150, so there is less cover. An Android quality system (`opto_master` Other_15) can cut this further during frame-rate dips. Likely active on the Vita. | High for the code. Medium for the effect: Other_15 not decoded. |
| C6 | obj_smokescreen_master Step_0 [hazards] | Android only: L20-26 | After smoke breaks pursuit: `following_player = 0`, `state` set. | 0x49077f4: `falling_smoke_timer = 3` if `vsp != 0`, else 0; then `state = 0`. | Enemies that were airborne when smoke broke their pursuit get a 3-tick timer. Its consumer is AI code, which was not read. | Medium |
| C7 | obj_floor_turret Step_0: bullet `parent` | PC L110 / AN L111 | 0x13e4158: `bb.parent = -1` (hi word 0xbff00000) | 0x3f552e4: `bb.parent = -4` (noone) | The turret's bullet stores self (-1, GMS 2.2 `self`) on PC and noone on Android. obj_bullet Step L3 (PC 0xf5f083) only copies `parent.player_slot` when `parent != noone && instance_exists(parent)`. Other uses of `parent` in obj_bullet were not traced, so the effect is unknown and probably small. | Low (effect). Assignment verified. obj_bullet itself has the same pattern (PC L47 / L145 store -1; AN L48 stores `id`, AN L146 stores -4): see batch A. |

The known `20 + sec_attack_timer` enemy change also makes bunker7's scripted volleys slower on Android. Its own
controller is identical. [hazards]

## 2. Platform-only differences

- obj_nightmare_boy Other_24 AN L118-119 (0x44535cc): `with (obj_onScreenKeyboard) vKeyList[..].signalButton = 30`.
  Touch prompt during the ball-throw minigame.
- obj_cutscene_apartment_postnightmare Step AN L1 (0x383633c): reads `obj_onScreenKeyboard.shortCutscene`. Touch UI.
- Interact highlights: `scr_highlightInteract` calls in obj_search_leviathan Step and obj_conveyer_lever Step. Touch.
- obj_search_leviathan Draw/Step: `gamepadMode` and `game_language` reads (UI).
- Telemetry: `scrTelemetryControl` in the factory_3 cutscenes and others. [hazards]
- Sniper dialogue boxes moved down 120 px. [hazards]
- `static` was renamed to `fxStatic` / `obStatic`. [hazards]
- [bike] items:
  - obj_bikebullet Other_10 gets crash guards (saves `obj_bikeplayer.id` only if the player exists; extra
    `instance_exists(parent)` checks).
  - The bullet whiz sound goes through an audio emitter.
  - obj_bikecorpse prints debug messages.
- Enemy hair is chosen through `hairNb` + `asset_get_index` in several Creates (enemybike, flyinghead_*, rollinghead,
  space*, cutscene_mansion_bikechase Step_2). Cosmetic; the sprite sets were not compared.
- Visual only:
  - Halogen alpha: 1.0 on PC, `0.9 + random(0.1)` on Android (PC L39 0x16eac44 / 0x16f0ee4).
  - flamethrower_explosion alpha is clamped to 0.0001 (AN 0x3f28dbc).
  - haunted_light1 fog is denser (spacing / 3).
  - Wall cam uses a red sprite while following. [hazards]
- Porting fixes that keep PC behaviour [hazards]:
  - factory2_6 Step re-registers the stompers.
  - factory_3 adds an `alarm[0]` and a new Alarm_0 event that repeat the skip actions one frame later.
  - factory_3 initialises `lever_grunt` / `death_grunt`.

## 3. Inspected and judged identical or noise

Headhunter:
- headhunter Step, all other PC-only doubles (892-936): sprite ids (spr_headhunter_*), stored as doubles on PC. Not a
  difference.
- headhunter Step L33/L69 vs AN L34: sweep start `!created_sweep && shoot_timer >= 1` on both. Different scheduling.
- headhunter_grenade_bounce Create, AN 40: `explode_timer_max = irandom(40) + 40` on both. PC adds the int 0x28 after
  irandom at 0x120d7fa; AN 0x40c2e14.
- headhunter_energy_bullet_sweep Draw, AN 15: `draw_lightning(..., irandom(10) + 15)` on both (PC 0x121e250 /
  AN 0x40b2c10). Same pattern in energy_bullet and energy_bullet2 Draw.
- headhunter Other_15 / Other_24, V, V_limo, V_fight Step, kissyface Step, psychboss_giant Step, tracking_hand Step:
  no non-builtin variable differs and no number is on one side only. The other differences are builtin accessor form
  and sprite ids (animation-frame ints) on PC.

V:
- V_fight Other_22, AN 400: `dest_x = irandom(680) + 400` on both (PC 0x116b80c / AN 0xc8dde8). The `A7` PC-only
  variable is a misresolved immediate.
- V_Step and V_limo_Step 230 (spr_vwalk); V_death Create 289 / 1750 (spr_v_death_onground, spr_nothing);
  V_limo Other_24 736 (spr_richhostage_punched): sprite ids.

Psychiatrist boss:
- psychboss_master Step, open_timer: `-= irandom(20) + 20` on both (PC 0x126772a / AN 0x47266d0).
- psychboss_master Other_15: attack picks are `choose(5 args)` and `choose(9 args)` on both (PC 0x19013c0 with
  const RValues 1..8; AN YYGML_choose r1=5/9). Values of the ARM argument helpers were not decoded.
  The track constants 45 are music.
- psychiatrist Other_24 35, create_psych_nightmare 40, destroy_psych_nightmare 20, nightmare_scientist Step 23,
  cutscene_mansion_bikechase Step_0 21: `track = N` music indices. On Android this is an int64 with the same value
  (checked in create_psych_nightmare: `track = 40`, kind 10).
- psychboss_eye Other_25 70, psychboss_background_nightmare Draw 14, psychoffice_shake_sides Draw 16: rendering only.

Other bosses:
- cutscene_dragon_1 Step, AN 15: `with (obj_enemy) if shoot_timer < -200 shoot_timer = irandom(10) + 15` on both
  (PC 0xce5a60 / AN 0x392fc40).
- dragon Step 885 (spr_dragon_casual_walk); SNOW Step and ending_snow 319 (spr_snow_walk); electrohead Create 664;
  kissyface Other_24 697; coffinhead Create 1232; penthouse_mill 577: sprite ids.
- dialogue_club_electrohead PC-only 2.2: Android keeps 2.2 as a static const RValue (0x1692788). The dialogue switch is
  compiled differently.

Nightmare levels:
- nightmare_girl Step, Android-only L64-130 (250, 450, 930, 32; tag_count, dest_x, step): the whole block sits under
  `if (playing && false)`. At 0x44563cc-0x4456410 a kind-13 false constant is tested and the code branches to L137
  at 0x4458d44. The block is dead code; PC has no code for those lines. Not a difference.
- choose_nightmare_cutscene Create: PC uses a jump table on `stage - 2` (0x1b71db4); Android uses compares.
  Same cases (2, 3, 4, 5, 10), mapping to nightmare_2/3/4/5/10.
- nightmare_girl Other_24 809/812/813, nightmare_killer 791, nightmare_psych 815, nightmare_scientist Create 473,
  cutscene_nightmare_4/5/10 474/475, nightmare_boy Step 745: sprite ids.
- kissyface_grenade_bounce Other_10 0.19999992847 and cutscene_psych_nightmare Draw 2.0000014: float32 noise.
- RoomCC room_psychiatrist_nightmare_7 4235519: a colour.

Empty events:
- PC-only events (V_fight Mouse_55/Other_18/Other_21, flyinghead_* Other_24, rollinghead Other_24,
  stompercloud Other_7/10, psych_projectile_die Other_23, headhunter_grenade_bounce Other_23, nightmare_actor Other_18,
  psychboss_foreground Create, bikeplayer Destroy/Other_11, bikecorpse Alarm_1, flamethrower_spawner Step,
  minecart_lockplayer_x Step, bikechase_wave_master Mouse_51) are all 16-byte empty functions.
- Both event tables still list them (objev.py; only the 2.2 PreCreate type 14 differs), so inheritance is unchanged.

[bike]:
- bikechase_wave Other_10 / Other_15, constants 13..28: `switch(wave)` is a jump table on PC (0x1b9cd78) and a case
  table 0..30 on Android (0x35684f0). All 31 cases and every spawn call match. Wave timers are 100/150/60/130/30/300
  on both.
- bikeplayer Step 0.65: `frict = 0.65` is also on Android (L341/365).
- bikeplayer Create 27: spr_hurtfly_begin; Android-only `casual` and `slow_sp` are initialisation.
- enemybike_bully Other_12 130: PC has it at L37.
- enemybike* Other_12: 805306368 is a flag word.
- enemybike Step 2048/273: float bits and a sound id.
- machinegun / v Create 1616/1613: sprites.
- cutscene_mansion_bikechase: Create 1000 is parallax_depth; Step_2 40 is `irandom(20) + 40`; Draw is a colour.
- About 30 other bike functions show only noise.

[hazards]:
- fanblade 20: knockback `-20 * dir` on both.
- bunker7 20: `irandom(5) - 20`.
- club_3 1000: ceasefire_timer.
- broken_halogen: exactly 0.1.
- dustspawner 450: `-450 - irandom(200)`.
- deadcop 190/140: `irandom(20) + 50`.
- factory2_6: 576/608/1632 clamp and stomper selection match.
- Stomper: Create 1532 is spr_block.
- turret_aim, restore_doors, oil_drum, prison_spotlight, swatvan, sniper_1 aim_time 260, conveyer_lever: identical.

## 4. Not resolved

- C1: whether hard mode or difficulty alters headhunter's sec_attack_timer elsewhere. Only Create and Step reference
  it.
- C2: the PC default of `global.controller_threshold`, which sets how large the deadzone change is. The same 0.001
  override seems to be in `player_get_attackdirection` (AN L13) and `player_get_throwdirection` (AN L21); these
  are batch B.
- C5: the values `opto_master` Other_15 sets during frame-rate dips are not decoded.
- C6: what `falling_smoke_timer` does in the enemy AI.
- C7: how obj_bullet uses `parent` beyond the player_slot copy; GMS 2.2 vs 2.3 `self` semantics. Batch A should check.
- Limits of the method: a constant changed to a value already present elsewhere in the same function, or a comparison
  change (`>` vs `>=`) without a new constant, would be missed. That applies to boss events I did not read line by
  line (V_fight Step, psychboss_master Step/Other_15, kissyface Step, psychboss_giant Step).
- psychboss_master Other_15: the choose() argument values on ARM are not decoded.
- [bike]: two object arguments in bikechase_wave Other_15 (AN L93 / L190) are held in registers; the totals match PC.
  Bike Draw events were not compared.
- [hazards]: only the token comparison was done for stomper Step, minecart Step L105-138, conveyer_belt Step and most
  of factory2_6 Step. Not checked: `check_light_collision*` (PC-only 8192 / 131072) and obj_bullet Step 90/270
  (batch A).
