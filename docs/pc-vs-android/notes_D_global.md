# Batch D (global timing, slow-mo, restart, rooms), reviewed by the coordinating agent

## Real differences
| Where | PC | Android | Effect | Conf. |
|---|---|---|---|---|
| `global.max_time` default, obj_init Create L171 (PC 0xa25beb..0xa25c1b) / Android `mobile_reset_game_data` (and `xbox_reset_game_data`), the Android init path | `max_time = 6000` | `max_time = 9000` | Level time limit 1.5x | high (PC var slot resolved to `max_time`; Android constant set 6000 -> 9000, others same) |
| obj_master Create L78 PC 0xe0d863 (`max_time = 4000`, then `time = max_time`) / Android L83 0x4365748 | 4000 | 6000 | 1.5x | high |
| Every level controller (`obj_cutscene_<level>_Create_0` / `_Step_0`, 89 functions) | 6000 / 7000 / 8000 / 8100 / 8300 / 8600 / 9000 | 9000 / 10500 / 12000 / 11850 / 12450 / 12900 / 13500 | Each level's time limit is 1.5x (club_0: 8100 -> 11850 = 1.46x). Examples: chinatown_2 Create L26 PC 0xcc5e7c -> L27 0x38bd674 (`global.max_time = 10500`, verified in the ARM: r8 = global max_time). Full list: out/const_review.txt / tools/funcdiff.py | high |
| obj_master Step (Android obj_master Other_10) L110-115 PC 0xe100cf..0xe10279 / Android L116-121 0x4369e5c..0x4369fc0 | `if gameover_slowdown > 0.2: gameover_slowdown -= 0.1`; `gsp += 0.0001` | `-= 0.12`; `gsp += 0.00015` | The slow-motion ramp while the player is in the dying/invincible game-over state (obj_player.invincible and gameover_type == -1, after gameover_timer > 5) falls 20% faster and gsp recovers 50% faster. Cosmetic-ish timing of the death slow-mo | medium (constants verified both sides; meaning of `gsp` not traced) |
| obj_cutscene_master Create | no `gameover_slowdown` init | `gameover_slowdown` initialised | Bug-fix of an uninitialised variable in cutscene rooms | medium |
| Room instance placement (tools/rooms_diff.py): room_prison_0_return / prison_1_return / prison_3_return | obj_respawn_point at x 3040 / 2080 / 2256; obj_temp_block (2160,272), (2368,384); obj_end_block x 16 | x 2976 / 2048 / 2272; temp_block (2144,288), (2336,384); end_block x 32 | Respawn (checkpoint) positions in the prison "return" levels moved by 16..64 px; blockers moved | high (data), effect low |
| room_motel_4 creation code | 6 instances with creation code `moving = 0` | 5 | One instance no longer gets `moving = 0` from room creation code | medium (which instance not traced), low impact |

## Structural / no gameplay change found
- obj_timeMaster Step (Android-only, 0xc0a158): unconditionally runs, in this order, user events of obj_master (u0),
  obj_lightmaster, obj_speedrun_master, obj_musicmaster, obj_lightmaster, obj_opto_master, ..., obj_replay_controller,
  obj_camera, obj_continuegame (u5) - the former Step events. Only the position in the step order changes
  (masters now run where obj_timeMaster's Step runs). No frame-rate or delta logic inside.
- obj_master Step otherwise matches (restart key, slowdown/bullettime glue, dragon dash state) after the +3/+6 line offset.
- Level timer countdown (obj_cutscene_master Step / Android Other_10): no constant or logic difference found.
- Room speed (60) and room sizes: identical in all 193 shared rooms. 12659 vs 12654 instances; the only count
  difference is room_title (menu). Other moved instances are 1-px geometry nudges (obj_block / obj_floor /
  obj_jumpthrough / obj_level_end in bunker2_1, bunker2_2, chinatown_4, chinatown_7_old, chinatown_9,
  factory2_3, prison_4, prison_4_return) and one light.
- Room creation code (2095 matched by content, tools/roomcc.py): 2014 identical; 81 differ only by encoding
  (colour constants, `static` renamed `obStatic` in 2.3, `x += n` as int operand, sprite refs).

## Platform-only seen in this batch
- obj_camera: Android-only roomTopBorder/roomBotBorder/roomHBorder (400/64/240) used in the look-around block
  where PC uses the mouse (center_x, mouse_dir, controller_check); mouse look replaced.
- obj_gamepaused Step_1 / obj_pausemenu Step: menu layout (menuPosX..), Netflix button, speedrun overrides,
  `setup_speedrun_vars` on quit; PC `stage == 12` test at L151 replaced by an instance_exists test (not traced).
- obj_savegame / obj_continuegame / load_* / save_*: Netflix cloud saves, conflict UI, hash checks, xbox flash saves.
- obj_init: Android moved global initialisation into `mobile_reset_game_data`.
