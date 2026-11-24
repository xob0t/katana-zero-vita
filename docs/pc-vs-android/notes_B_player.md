# Batch B: player character and player controls (PC Steam vs Android 1.1.10)

Method: Android: Ghidra decompile of 47 functions (read-only project) plus the decomp lifter, run into
`scratch_B/gh*/` and `scratch_B/gml/`, each claim checked against `annotate.py` ARM. PC: `scratch_B/pcann.py`
(pcdis plus line markers, var ids, f64 and builtin slots). Line numbers: PC first, Android second.

## 0. Vita-port finding (not PC vs Android, but it matters most for gamepad players)

**Crouching on the ground is probably blocked on the Vita build with touchControls=0 (PR #82, on main).**
Android adds this gate to every crouch check on the ground:
`controller_check(key_crouch) && (obj_global_controller.allowCrouch || obj_screenJoystick.wasNeutral > 0 ||
obj_global_controller.control_override > 0)`.
- `allowCrouch` is only ever written `false` (obj_global_controller Create L15, 0x4057304; no other function
  references it).
- `wasNeutral` is set to 0 in obj_screenJoystick Create L19 (0x487c10c). obj_screenJoystick Step L105
  (0x48801ac) writes `wasNeutral = 20` on every frame without a stick touch (`!buttonDown || moveAmnt <= 0.15`), and
  counts down only while the touch stick is held. On Android without touch this is 20, so the gate is touch-only and
  gamepad play matches PC.
- `loader/loader/touch_controls.c` (touchControls=0) replaces `gml_Object_obj_screenJoystick_Step_0` with a skip,
  so `wasNeutral` stays at its Create value of 0. Its rest check (exp_touchverify) covers posValueX/Y, moveAmnt,
  walking and buttonDown, not wasNeutral.
- Result (static reading, not tested on the device): idle crouch (player_idle_ground L100 0x32978b4), crouch-roll
  and the crouch drop while running (player_moving_ground L204/L236 0x32a02d8/0x32a04d0), crouch from walking
  (player_walking L124) and crouch/roll on a slope landing (slope_land_check L47 0x335e0a8) all fail unless a
  cutscene override is active. key_roll rolls (idle L127, running L198) still work. Fix idea: write wasNeutral = 20
  (or run the Step's rest path) when skipping the stick Step. Confidence: medium-high (gate and values verified in
  the ARM; the effect is not tested on a Vita).

## 1. Real gameplay differences

| # | Where | PC | Android | Effect | Conf. / how |
|---|---|---|---|---|---|
| 1 | obj_player Other_13 (katana setup) L9 / L11 | 0x1378bde `bullettime_max = 380` (0x4077c000) | 0x462e0e4 `bullettime_max = 760` (0x4087c000); L12 `bullettime_timer = bullettime_max` on both | Zero's slow-mo meter holds twice as long: about 12.7 s instead of 6.3 s of chronos. Other_13 runs from Other_11 (event_user(3)), which runs from Create, so it applies at every spawn. Create itself sets 380 on both sides (PC L80 0x136e517, AN L85 0x461b338) and Other_13 overwrites it. Drain is the same (`bullettime_timer -= 1`, `bullettime_max -= 0.1` per slow-mo frame, PC L81/L88 0x439d78/0x439e18, AN L89/L90 0x3264ac4/0x3264b20) and so is the recharge (+0.5 per frame, PC L117 0x43a2c8, AN L119 0x326504c). The per-katana modifiers (0.6, 0.8, +60) are the same. Dragon: Create 380 then Other_11 `+= 120` gives 500 on both when spawned as the dragon; after a Zero→dragon cutscene it is 880 on Android vs 500 on PC. | high: constants in ARM and x86; drain and recharge compared |
| 2 | player_rolling L74-L139 / L76-L165 | Roll end checked only on frames with `animation_finished && sprite_index == sprite_roll` (0x465179). While overlapping an obstacle/projectile, extend at hsp ±3 up to roll_extend_max (L93-L104). Roll presses during that extension are ignored. | Latches `roll_end = true` (L78 0x32b9d34) and runs the end check every frame until the state leaves 8 (reset L190/191). During the extension, `controller_check_pressed(key_roll)` (L113 0x32bc1fc) or, if player `control == 0` (keyboard), key_crouch pressed (L118-122 0x32bc460) sets `queue_roll`. When the extension ends, a queued roll starts at once (L152-165 0x32bb600: state 8, delay 0, animation reset). | Input buffer: a roll pressed while still stuck in the forced roll extension (under a low ceiling or obstacle) chains into a new roll on Android; on PC it is lost. Gamepad uses key_roll, so it applies to gamepad. Extension speeds and limits are unchanged. | high (Ghidra lift checked against ARM markers and PC x86 L74-L119) |
| 3 | obj_slash Collision with obj_door (PC GUID e61f8e37) L49 / L54; obj_slash Step L53-58 | 0x13857d8 `harmless = 1` as soon as the slash hits a door | 0x48d5318 `trigger_harmless = 2`. Step (0x48d2318-0x48d2458): `if (trigger_harmless > 0 && deadly && global.fr != 0) { trigger_harmless -= 1; if (trigger_harmless <= 0) harmless = true; }`, then the L48 `if (harmless) deadly = false` takes effect the next frame. Create L11 (0x48d080c) sets 0. | A slash that opens a door stays deadly 2-3 more frames on Android (within its deadly frame window). It can kill an enemy right behind the door that PC's slash spares. | high (both sides read) |
| 4 | player_moving_ground: crouch while running on a jump-through platform, L169-L183 / L204-L227 | `if crouch pressed && state==1: state=7; if ((place_meeting(x,y+1,obj_jumpthrough) \|\| place_meeting(x,y+1,obj_cable)) && !block_meeting(x,y+1))` nothing (drop through; L173-174 0x42766f/0x427a3e), else `if control==X` roll when a direction is held (L178-183) | Same structure (L208 0x32a07a8), but on a platform it rolls if `control==X && !global.moving_dropdown` (L211 0x32a193c, reads the global hoisted at 0x329c738). Off a platform it is the same as PC (L222-227). | Option only. With `global.moving_dropdown = true`, crouching while running on a platform drops through, as on PC. With it false, it rolls instead. mobile_reset_game_data L583 (0x33cb194) sets true; the load_options default is not verified. | medium (branch read in ARM; default partly verified) |
| 5 | player_get_throwdirection L17 / L18-L23 | Stick inside `global.controller_threshold`: 8-way digital (facing, up/down) | L18 copies controller_threshold, then L21 (0x32872b0) unconditionally overrides it with 0.001. Digital only when the left stick is exactly centred, otherwise `point_direction` of the stick (L76). | Gamepad: small stick tilts aim throws at the exact stick angle on Android. On Vita the loader's 0.2 radial deadzone (gamepad.c) zeroes resting drift, so the change only shows between 0.2 and controller_threshold. | high on code, low on impact |
| 6 | player_get_attackdirection L11-L76 / L10-L76 | Stick beyond controller_threshold: analog slash angle, else 8-way digital (L17 0x4cd585) | L11-13 (0x3284c58): threshold = 0.001 if `!global.gamepadMode`. L24 (0x3284d74): `if (global.gamepadMode)` gives the PC logic, else always 8-way digital. | If gamepadMode is false, gamepad slashes snap to 8 directions instead of the analog angle. player_analogue_pressed_check L13-17 (0x321139c/0x3211464) sets `global.gamepadMode = true` once a stick passes controller_threshold (tempStopScreenjoystick == 30). The onScreenKeyboard Step L109 (0x44c0b6c) also writes gamepadMode (source not verified). So on Vita it should switch on as soon as the stick is used; d-pad-only play is 8-way on both sides anyway. | medium (code high; Vita runtime value not verified) |
| 7 | player_casual_walking (state 21, non-combat walk) | Acceleration model only (L5-L108: 1.2/0.3/0.2/0.5/1.4) | Adds a non-drunk branch L119-L223: speed `_l68` from a switch on animation_index (frame-synced, constants 2/3/4/20), `hsp = ±_l68` (L187/L210) | Story/apartment walking speed follows the animation frames on Android. No combat effect. | low (lifter partial at the switch; PC structure not fully read) |

## 2. Platform-only differences (touch, debug, cosmetic)

- fakeJumpHold / fakeLeftHold / fakeRightHold: set to 60 only inside `if (obj_onScreenKeyboard.launchGo)` (idle_ground
  L54-71, moving_ground L149-169). Otherwise they only count down or reset (player_air L69-73, L89-106, L154-155).
  launchGo is true for one frame after `triggerLaunch`, which only obj_playerBtn Gesture_3/4 set (touch). The added
  `fakeJumpHold <= 0` term in player_air L74 is always true without touch. Touch-only.
- launchGo / anglePos: idle_ground L54-71, L117-123 (touch jump/roll at the anglePos angle: 150/30/100/80/120/60/
  90/270), moving_ground L149-187 (170/10/100/80/120/60/90), player_air L131, player_rolling L113, player_wallgrab
  L47/L106: all read obj_onScreenKeyboard/obj_playerBtn.launchGo. Touch-only. The "Android-only 150/30/270/90/170/100/
  60/120" constants are these touch angle windows, not stick thresholds; the 60.0 at moving_ground L181/198/204/224
  is the fakeHold value in a register (the token extractor's misattribution).
- walking / walking_timer (obj_screenJoystick): set true only while the touch stick is held at a small tilt; read
  as `controller_check(key_walk) || obj_screenJoystick.walking` (moving_ground L173, player_walking L130,
  slope_land_check L44, physics L457). Touch-only.
- allowCrouch / wasNeutral / control_override gate: touch-only on Android (wasNeutral 20 at rest), but see section 0
  for the Vita port.
- player_secondary_dragon L175 (0x32c2c20) `if (... && control == 3)`: touch dash aim (moveAmnt, angle from the
  stick, 15/360/180 steps L177-180). The player instance's `control` is 1 from Create (L1 0x461916c). No other writer
  found in the player scripts, so it is inactive for gamepad (not fully verified).
- hurtfly_shake (Create L149 0x461d318 = 6; player_physics L353/L428/L520 0x32ad6a8/0x32adea4/0x32aed44): picks
  the screen_shake strength for hurtfly impacts. Cosmetic.
- executionCount / lastPlayerMoveSpd: player_physics L577 (0x32b21cc) `global.lastPlayerMoveSpd = <local>`, L578
  (0x32b227c) `global.executionCount += 1`; obj_control Step L59 (0x375989c) resets it to 0. No other reader:
  diagnostics.
- dragon_dash_hint (Create L151 0x461d428) and motion_alpha_scale (Create L213, Other_11 L5 0x4627868): written,
  never read in player code. Unused or cosmetic.
- obj_player Other_10 (default key map): PC binds keyboard W (87) to key_up[0] (L26) and key_jump[1] (L36); Android
  drops both. Gamepad bindings are the same (32769-32784, 9990-9993). Keyboard only.
- controller_check / _ext / _pressed / _pressed_ext: right-stick-down key id 9997 (PC) renamed 99970 (Android). Mode
  3 adds touch buttons (vKeyList isHeld), the touch stick (0.05 deadzone) and gp_shoulderlb/rb needing
  `gamepad_button_value > 0.5`. Input mapping only.
- player_analogue_pressed_check: same angle windows (120/60) as PC; Android adds
  `obj_global_controller.tempStopScreenjoystick = 30` on stick use (the "30") and the gamepadMode switch above.
- Script_physics_fix_glitched_position (0x31f46b8): this is PC's inline "ATTEMPTING TO FIX GLITCHED PLAYER POSITION"
  block moved into a script. In player_physics, PC L548-L615 (0x49bae7-0x49ecd0) maps to Android call-site
  L571-L574 (condition 0x32b1e7c, call 0x32b21bc) and script L2-L74. The condition is the same on both sides:
  `!casual && !obj_global_controller.control_override && point_distance(...) > max(|hsp|,|vsp|)*2+...` (32 at PC
  L545 / AN L568). Every inner line maps with a constant offset of 541 (debug strings, sprite size resets, 10/360
  spiral steps, "POSITION RESOLVED", final revert at PC L612-615 / script L71-74). Mode local = 0 on both (PC L566
  `[esp+0x10e8]=0`, script L30), so the spiral branch is dead on both. The same holds for player_ragdoll (PC L88-146)
  and ragdoll_physics (PC L109-167). No change to collision, unsticking or teleporting. Confidence: medium-high
  (line mapping and condition checked; the branch-to-callee pairing of the two place_meeting_radius* calls looks
  swapped between the readings, which I put down to shared code and did not resolve).
- obj_player Create speeds (PC L208-224: jump_power -10, max_hsp 7.75, roll_spd 16.5, base/true copies, max_spd 20,
  walljump_spd 16.5) moved to the new obj_player Other_15 (user event 5): same values (lifted Other_15). Android
  calls it from Create L217 (0x461f5ac) and at the start of Other_11 L2 (0x4627854); no other caller on obj_player
  (full event_user(5) scan: other hits are bikechase_wave, club_master, headhunter, psychboss_master on their own
  events). Effect: each Other_11 run resets speeds before the dragon bonuses (jump_power -1, max_hsp/roll_spd +0.75,
  walljump +0.5; PC L85-L92 0x1377765-0x13779d5, AN L92-L99). PC applies the bonuses to the current values.
  Other_11 runs once at Create (is_dragon = false) and once in a dragon cutscene (cutscene_dragon_* Step `set`
  guard, e.g. dragon_basic L7-8 0x3944498), so both builds end at -11 / 8.5 / 17.25 / 17. No practical difference;
  `bullettime_max += 120` still stacks on both.

## 3. Inspected, identical or noise only

- player_bullettime_check: same drain/recharge/slow_sp constants (0.1, 1, 0.5, 0.02, 0.005, 0.15, 12). The room 66 vs
  67 test is the same room (room_tiedup in both asset lists).
- player_air: identical apart from the touch-only fakeHold/launchGo terms (jump release vsp/1.5, fast-fall vsp 8
  with control_override test, air accel 0.7, roll-on-land 20 px, air dodge `roll_delay*2`).
- player_idle_ground / player_moving_ground: identical apart from the touch terms and items 4/section 0 (doorbreak,
  run accel 0.5/0.05, run start 0.5/width 1.3/delay 7, jump, key_roll roll).
- player_walking, slope_land_check: only the touch gate and walking terms.
- player_attack_check (PC 1693 = spr_sword_projectile, asset encoding), player_throw (PC 1673 used as
  audio_is_playing argument vs Android sound ref): asset-index noise. No other constant, variable or call
  differences in either (const_review / vardiff / call_review).
- player_secondary_dragon: only the touch branch (L175-180) and Other_11 calls inside a `with` (slash user event).
- player_hurt, player_attack, player_airdodge, player_walljump, player_wallgrab (launchGo only), player_crouching,
  player_block_check: no one-sided constants, variables or calls. Line diffs are 2.3 noise. Not read line by line.
- obj_player Other_11 sprite assignments: asset renumbering only (Android adds event_user(5), see section 2).
- obj_player Other_13: only 380→760 (item 1); katana branches same constants.
- obj_global_controller Create: Android starts `control = 3` (PC 0), adds allowCrouch=false, gamepad_mode=false
  and the obj_onScreenKeyboard creation. Platform control scheme (3 = mobile combined input; the Vita
  held_native.c handles mode 3).

## 4. Not resolved

- Runtime value of `global.gamepadMode` on Vita (item 6): set by analogue_pressed_check on stick use and written by
  onScreenKeyboard Step L109 from a source I did not trace. If L109 copies a false `obj_global_controller.gamepad_mode`
  every frame, stick-angle slashes could be lost on Vita.
- Whether the obj_player instance `control` ever becomes 0 or 3 on Android/Vita (assumed 1 from Create). It decides the
  key_crouch roll queue (item 2) and the dragon touch-aim branch.
- load_options default for `moving_dropdown` (item 4; mobile reset sets true).
- player_casual_walking new branch (item 7): exact speed table and the PC structure after L108.
- Section 0 is static analysis; it should be confirmed on the device (crouch on flat ground with touchControls=0).
- player_hurt / player_attack / player_airdodge / player_walljump were not read line by line: no flagged constants,
  variables or calls, but a logic-only change (same tokens, different comparisons) would not show up in those lists.
