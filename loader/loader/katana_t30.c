/*
 * katana_t30.c
 *
 * 30 fps mode (fpsMode in yyl.cfg, or the FPS MODE menu), and half-rate enemies at 60 ticks/s (enemyHalfRate, or
 * the ENEMY AI menu).
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <kubridge.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "lz4/lz4.h"
#include "dialog.h"

#include "katana_internal.h"

/*
 * 30 fps mode (fpsMode in yyl.cfg, old name fps30: 0 off (default), 1 in rooms that can't hold 60 fps, 2 everywhere). GameMaker games run one
 * logic tick per frame, so a CPU-bound room at 31 fps plays at half speed. In this mode such a room runs 30 ticks a
 * second (paced by the display: two vblanks per frame) and each tick advances the game twice as far, so it plays at
 * full speed at 30 fps. Not exact: input is read 30 times a second, movement takes steps twice as long, and anything
 * the game counts in ticks without its time factor runs at half speed (some effect lifetimes). What is converted:
 * - The game's time factor global.fr (slow motion; 0 during hit-stop) is doubled right after obj_master and
 *   obj_cutscene_master user event 0 set it each tick. Before those run, a value we doubled is put back, so a
 *   controller that keeps the previous value (pause) does not double it again.
 * - The hit-stop counter (freeze_timer of those controllers) counts two ticks per tick.
 * - The player's animation runs with fr = 1 (player_animation overrides it around animation_code): 2 in this mode.
 * - The runner's built-in clocks run twice per tick: sprite animation (Instance_Animate, image_speed) and alarms
 *   (HandleAlarm). Built-in motion (speed/hspeed/vspeed, HandleInstanceMovement) too with exp_t30move: off by default,
 *   since much of the game's motion already multiplies its own speeds by fr.
 * - The loader's every-other-tick work (lighting refresh, cloth physics, rain and particle drawing) runs every tick,
 *   and dynamic resolution doesn't count the wait for the second vblank as GPU load.
 * - Dialogue letters don't start on frozen ticks (t30_letter_step).
 * - Enemies' search_timeout gets the tick it missed (t30_enemy_search), and a passive textbox (a line spoken while
 *   you play) types twice per tick while its line is incomplete (t30_passive_textbox_user0). Its hold and fade once
 *   complete use global.fr (die_timer += fr, fade_alpha -= 0.1 * fr), so they need nothing.
 * - Slow motion's ramp and meter run twice per tick (t30_bullettime_check).
 * Scripted sequences (the game drives the player: the first level's tutorial, cutscenes) count ticks exactly and break
 * at 30 Hz (the tutorial's slow motion came too late and the player was shot), so they always run at 60 ticks/s.
 * In mode 1 a room switches to 30 once T30_SLOW_FRAMES of a T30_WINDOW-frame window took over T30_SLOW_US (from
 * T30_SETTLE_FRAMES after the room starts), and back to 60 once T30_FAST_FRAMES of a window took under T30_FAST_US of
 * CPU work per tick (the frame interval says nothing at 30: it is always two vblanks). Each switch holds for at least
 * T30_DWELL_US, so a room doesn't flap between the two. A room that switched to 30 starts at 30 the next time (death
 * restarts keep the room) until it goes back to 60 once; the rooms in t30_known_heavy always start at 30 (after a
 * starting cutscene). So a momentary slowdown costs a few seconds at 30, not the rest of the room.
 * exp_t30anim0, exp_t30alarm0, exp_t30freeze0, exp_t30search0 and exp_t30text0 turn the respective conversion off.
 * Debug: exp_t30trace logs the player's position, exp_t30never keeps 60 ticks/s with the hooks installed (a baseline
 * for traces), exp_t30watchdog crashes the game (for a core dump) when ticks stop for 3 s in 30 fps mode, exp_t30diag
 * logs which objects run scripted sequences, enemies' search_timeout changes and passive textbox timers.
 * The hooks are installed whatever fps30 says, since the game's GRAPHICS menu can choose the mode at runtime (see
 * fps_menu_install); with fps30 0 every hook is gated on t30_active and changes nothing.
 */
int fps30 = 0; // NATIVE by default
#define T30_SETTLE_FRAMES 45
#define T30_WINDOW 90           // ~2.6 s while slow at 60, 3 s at 30
#define T30_SLOW_FRAMES 80      // frames over T30_SLOW_US (under 50 fps) to drop to 30
#define T30_SLOW_US 20000
#define T30_FAST_FRAMES 85      // ticks under T30_FAST_US of work to go back to 60
// A tick at 30 does at least the work of a tick at 60 (it runs the every-other-tick work every time), so one under
// 16 ms fits a 60 Hz frame with the swap. Measured at 500 MHz, the same state at either rate: room_motel_6 16.6 ms per
// tick (58 fps at 60), room_motel_3 13.9 ms at 30 and 13.5 ms at 60 (60 fps). It was 14 ms: the Hotel's last room,
// once cleared, took ~14.1 ms per tick at 30 and stayed at 30 until the exit.
#define T30_FAST_US 16000
#define T30_DWELL_US 5000000    // minimum time between two switches
#define T30_MAX_ROOMS 1024
static int t30_anim, t30_alarm, t30_move, t30_freeze;
static int *var_fr, *var_freeze_timer;
static int t30_fr_scaled; // global.fr currently holds t30_scaled_fr, our doubling of the game's t30_game_fr
static double t30_game_fr, t30_scaled_fr;
static unsigned char t30_heavy_room[T30_MAX_ROOMS]; // 1 switched to 30 last time, 2 in t30_known_heavy
// Rooms measured under 50 fps at 500 MHz: they start at 30 ticks/s instead of playing ~3 s of slow motion first
static const char *const t30_known_heavy[] = {
	"room_bunker2_3", "room_club_4_2", "room_dragon_4", "room_factory2_4", "room_haunted_alien2", "room_haunted_cart2",
	"room_haunted_silent1", "room_mansion_2_2", "room_mansion_3", "room_prison_4_return",
};
static event_func orig_master_user0, orig_cutscene_master_user0;
static gml_script_fn orig_player_animation, orig_animation_code;
static void (*orig_instance_animate)(void *inst);
static void (*orig_handle_alarm)(void);
static void (*orig_handle_movement)(int);
static void (*update_active_lists)(void);
static int in_player_animation;
unsigned t30_switches;

int t30_installed; // t30_install found its symbols: the time conversion is in place for any mode
static int t30_never; // exp_t30never: hooks (and exp_t30trace) installed, 30 ticks/s never used: a baseline for traces
static uint32_t t30_scripted_tick; // last tick a cutscene script drove the player
static volatile int t30_stage; // debug: where the 30 fps hooks were last (the exp_t30watchdog report)
static gml_script_fn orig_override_control_init;

static int t30_diag; // exp_t30diag: which objects run scripted sequences, and how dialogue timers move
static void *t30_override_control_init(void *self, void *other, void *result, int argc, void **args) {
	if (t30_diag) {
		static int last_obj = -1, last_room = -1, lines;
		int obj = self ? *(int *)((char *)self + 0x84) : -1;
		if ((obj != last_obj || *current_room != last_room) && lines++ < 60) {
			const char *(*object_name)(int) = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_Namei");
			debugPrintf("[T30DIAG] room %d tick %u: override_control_init from %s\n", *current_room, *frame_count,
				object_name && obj >= 0 ? object_name(obj) : "?");
		}
		last_obj = obj;
		last_room = *current_room;
	}
	t30_scripted_tick = *frame_count ? *frame_count : 1;
	return orig_override_control_init(self, other, result, argc, args);
}

// The dossier burn's Steps (dossier_native.c) mark its ticks
static uint32_t t30_heavy_tick;
void t30_dossier_burning(void) {
	if (frame_count)
		t30_heavy_tick = *frame_count ? *frame_count : 1;
}

// obj_gamepaused exists while the pause menu is open; its Begin Step marks the paused ticks
static event_func orig_gamepaused_step1;
static uint32_t t30_paused_tick;
static void t30_gamepaused_step1(void *self, void *other) {
	t30_paused_tick = *frame_count ? *frame_count : 1;
	orig_gamepaused_step1(self, other);
}

static void eh_setting_check(void);

static void t30_set(int on) {
	if (on == t30_active)
		return;
	t30_active = on;
	eglSwapInterval(NULL, on ? 2 : 1);
	t30_switches++;
	debugPrintf("[FPS30] room %d: %s\n", current_room ? *current_room : -1, on ? "30 ticks/s, double time step" : "60 ticks/s");
}


// Once per presented frame (katana_after_swap); work_us is the CPU time of the tick that was just presented
void t30_update(uint32_t work_us) {
	static int room = -2, frames, window, count, mode = -1;
	static uint64_t last, switched;
	uint64_t now = sceKernelGetProcessTimeWide();
	uint64_t interval = last ? now - last : 0;
	last = now;
	fps_mode_sync();
	eh_setting_check();
	// The mode can change at runtime (GRAPHICS menu): leave 30 ticks/s when it turns off
	if (!fps30 || t30_never || !current_room) {
		t30_set(0);
		return;
	}
	// A new mode evaluates the room afresh (Dynamic: settle, then measure; heavy rooms start at 30)
	if (fps30 != mode) {
		mode = fps30;
		room = -2;
	}
	static int known_resolved;
	if (!known_resolved && *current_room >= 0) {
		known_resolved = 1;
		int (*room_find)(const char *) = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
		int found = 0;
		for (unsigned i = 0; room_find && i < sizeof(t30_known_heavy) / sizeof(*t30_known_heavy); i++) {
			int id = room_find(t30_known_heavy[i]);
			if (id >= 0 && id < T30_MAX_ROOMS) {
				t30_heavy_room[id] = 2;
				found++;
			}
		}
		debugPrintf("[FPS30] %d of %u known heavy rooms start at 30 ticks/s\n", found,
			(unsigned)(sizeof(t30_known_heavy) / sizeof(*t30_known_heavy)));
	}
	if (*current_room != room) {
		room = *current_room;
		frames = window = count = 0;
		switched = 0;
	}
	// Scripted sequences (the game drives the player through override_control_init every tick) count ticks with
	// exact comparisons: keep them at 60 ticks/s
	if (t30_scripted_tick && *frame_count - t30_scripted_tick < 3) {
		t30_set(0);
		frames = window = count = 0;
		return;
	}
	// The pause menu draws ~21 ms of VHS effects over the frozen room (chinatown_3: 26 ms frames, ~38 fps). It runs
	// at a steady 30 and doesn't count as the room's load: closing it returns to the mode the room had, and the
	// measurement starts over.
	static int pause_saved = -1;
	if (t30_paused_tick && *frame_count - t30_paused_tick < 3) {
		if (pause_saved < 0)
			pause_saved = t30_active;
		t30_set(1);
		window = count = 0;
		return;
	}
	if (pause_saved >= 0) {
		t30_set(fps30 >= 2 || pause_saved);
		pause_saved = -1;
		window = count = 0;
	}
	if (fps30 >= 2) {
		t30_set(1);
		return;
	}
	unsigned char *heavy = room >= 0 && room < T30_MAX_ROOMS ? &t30_heavy_room[room] : NULL;
	if (frames++ == 0) {
		// A room starts (or resumes after a cutscene) at 30 if it was heavy last time
		int on = heavy && *heavy;
		if (on != t30_active)
			switched = now;
		t30_set(on);
	}
	// The dossier burn (its Steps mark the ticks) counts Steps, so it plays in slow motion once a tick takes longer
	// than a frame: its first second is light, its peak ~30 ms per tick. It drops to 30 after 10 slow frames of 15
	// and stays there until it ends. The folder's animations before it also count Steps, so room_folder is never
	// remembered as heavy (they would play at half speed the next time).
	if (t30_heavy_tick && *frame_count - t30_heavy_tick < 3) {
		if (!t30_active) {
			count += interval > T30_SLOW_US;
			if (++window >= 15) {
				if (count >= 10) {
					debugPrintf("[FPS30] room %d: dossier burn, %d of %d frames over %.0f ms\n", room, count, window,
						T30_SLOW_US / 1000.0f);
					t30_set(1);
					switched = now;
				}
				window = count = 0;
			}
		}
		return;
	}
	if (frames <= T30_SETTLE_FRAMES || (switched && now - switched < T30_DWELL_US)) {
		window = count = 0;
		return;
	}
	count += t30_active ? work_us < T30_FAST_US : interval > T30_SLOW_US;
	if (++window < T30_WINDOW)
		return;
	if (!t30_active && count >= T30_SLOW_FRAMES) {
		if (heavy && !*heavy)
			*heavy = 1;
		debugPrintf("[FPS30] room %d: %d of %d frames over %.0f ms\n", room, count, T30_WINDOW, T30_SLOW_US / 1000.0f);
		t30_set(1);
		switched = now;
	} else if (t30_active && count >= T30_FAST_FRAMES) {
		if (heavy && *heavy == 1)
			*heavy = 0;
		debugPrintf("[FPS30] room %d: %d of %d ticks under %.0f ms of work\n", room, count, T30_WINDOW, T30_FAST_US / 1000.0f);
		t30_set(0);
		switched = now;
	}
	window = count = 0;
}

gml_rvalue *instance_var(void *self, int *var) {
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)self)[3];
	gml_rvalue *v = var ? vwrite(self, var[1]) : NULL;
	return v && v->kind == 0 ? v : NULL;
}

static void t30_time_controller(event_func orig, void *self, void *other) {
	t30_stage = 30;
	gml_rvalue *fr = global_var(var_fr);
	if (t30_fr_scaled && fr && fr->kind == 0 && fr->val == t30_scaled_fr)
		fr->val = t30_game_fr;
	t30_fr_scaled = 0;
	gml_rvalue *freeze;
	if (t30_active && t30_freeze && (freeze = instance_var(self, var_freeze_timer)) && freeze->val >= 1)
		freeze->val -= 1;
	orig(self, other);
	if (t30_active && (fr = global_var(var_fr)) && fr->kind == 0) {
		t30_game_fr = fr->val;
		t30_scaled_fr = fr->val *= 2;
		t30_fr_scaled = 1;
	}
	t30_stage = 0;
}

static void t30_master_user0(void *self, void *other) {
	t30_time_controller(orig_master_user0, self, other);
}

static void t30_cutscene_master_user0(void *self, void *other) {
	t30_time_controller(orig_cutscene_master_user0, self, other);
}

// exp_t30trace (debugMode): the player's position every 6th tick, to compare movement over real time between modes
static int t30_trace;

static void *t30_player_animation(void *self, void *other, void *result, int argc, void **args) {
	if (t30_trace) {
		static uint32_t last_tick;
		if (*frame_count - last_tick >= (t30_active ? 3u : 6u)) {
			last_tick = *frame_count;
			float x, y;
			memcpy(&x, (char *)self + 0xb8, 4);
			memcpy(&y, (char *)self + 0xbc, 4);
			gml_rvalue *fr = global_var(var_fr);
			debugPrintf("[T30POS] %llu us tick %u x %.2f y %.2f fr %.3f\n", sceKernelGetProcessTimeWide(), *frame_count, x, y,
				fr && fr->kind == 0 ? fr->val : -1.0);
		}
	}
	in_player_animation++;
	void *r = orig_player_animation(self, other, result, argc, args);
	in_player_animation--;
	return r;
}

// Slow motion (player_bullettime_check, once per tick from player_input) counts in ticks, not with global.fr: while
// the key is held the ramp speeds up (slow_sp -= 0.02) and global.slowdown moves by slow_sp, the meter drains
// (bullettime_timer -= 1, bullettime_max -= 0.1); released, slow_sp += 0.005 and the meter refills by 0.5. At 30
// ticks/s all of it ran at half speed: the ramp into slow motion took 0.3 s instead of 0.16 s, and the meter lasted
// twice as long and refilled half as fast. The script runs twice per tick, as two 60 Hz ticks would: the second pass
// sees no new key press (controller_check_pressed is false, held keys stay held), since the press belongs to the
// first. exp_t30slowmo0 turns it off.
static gml_script_fn orig_bullettime_check, orig_controller_check_pressed;
static int t30_bullettime_second;
unsigned t30_bullettime_doubled;
static void *t30_controller_check_pressed(void *self, void *other, void *result, int argc, void **args) {
	if (!t30_bullettime_second)
		return orig_controller_check_pressed(self, other, result, argc, args);
	gml_rvalue *r = result;
	r->val = 0;
	r->flags = 0;
	r->kind = 0;
	return result;
}
static int *var_slowdown;
static void *t30_bullettime_check(void *self, void *other, void *result, int argc, void **args) {
	void *r = orig_bullettime_check(self, other, result, argc, args);
	if (t30_active && orig_controller_check_pressed) {
		gml_rvalue scratch = {0, 0, 5}; // the script returns nothing (undefined); keep the first pass's result
		t30_bullettime_second = 1;
		orig_bullettime_check(self, other, &scratch, argc, args);
		t30_bullettime_second = 0;
		t30_bullettime_doubled++;
	}
	if (t30_trace) { // exp_t30trace: global.slowdown each tick while it moves
		static double last = 1.0;
		gml_rvalue *s = global_var(var_slowdown);
		if (s && s->kind == 0 && (s->val != last || s->val != 1.0))
			debugPrintf("[T30SLOW] %llu us tick %u %s slowdown %.4f\n", sceKernelGetProcessTimeWide(), *frame_count,
				t30_active ? "30" : "60", s->val);
		if (s && s->kind == 0)
			last = s->val;
	}
	return r;
}

// Also carries the reduced-rate effects' animation repeat (fx_rate.c)
int t30_animation_code_hooked;
static void *t30_animation_code(void *self, void *other, void *result, int argc, void **args) {
	extern int fx_anim_repeat;
	extern void *fx_animation_code(gml_script_fn orig, void *self, void *other, void *result, int argc, void **args);
	if (fx_anim_repeat > 1)
		return fx_animation_code(orig_animation_code, self, other, result, argc, args);
	gml_rvalue *fr;
	if (!t30_active || !in_player_animation || !(fr = global_var(var_fr)) || fr->kind != 0 || fr->val != 1.0)
		return orig_animation_code(self, other, result, argc, args);
	fr->val = 2.0;
	void *r = orig_animation_code(self, other, result, argc, args);
	if ((fr = global_var(var_fr)) && fr->kind == 0 && fr->val == 2.0)
		fr->val = 1.0;
	return r;
}

// obj_letter Step (dialogue letters) first runs `while (oscillate_delay > 0) { ...; oscillate_delay -= 1 * fr; }`:
// endless while time is frozen (fr = 0, hit-stop), and the club's sign letters start with delays in the thousands.
// The original never steps a new letter on a frozen tick; with the shifted timing of 30 fps mode it can (the game
// hung in room_club_4_2 after a death). Such a letter skips its Step until time moves again.
static event_func orig_letter_step;
static int *var_oscillate_delay;
unsigned t30_letter_skips;

static void t30_letter_step(void *self, void *other) {
	gml_rvalue *fr, *delay;
	if (t30_active && (fr = global_var(var_fr)) && fr->kind == 0 && fr->val < 0.001 && (delay = gml_vget(self, var_oscillate_delay[1])) &&
			delay->kind == 0 && delay->val > 0) {
		t30_letter_skips++;
		return;
	}
	orig_letter_step(self, other);
}

// Enemy Steps count `search_timeout += 1` per tick and give up searching once it passes a limit (a `>` test, so a
// step of two can't skip it). At 30 ticks/s that took twice as long: each Step that advanced it gets one more.
static int *var_search_timeout;
unsigned t30_search_ticks, t30_enemy_steps;
static int t30_enemy_depth, t30_search;

/*
 * Half-rate enemies (enemyHalfRate in yyl.cfg: 1 on (default), 0 off; or the ENEMY AI menu, see fps_menu_install).
 * Only while the game runs 60 ticks/s: in 30 fps mode every enemy Step runs as above. Enemy Steps (AI, AI_physics,
 * enemy_hurt: 3-8 ms per tick in combat rooms) run every other tick, which saves 2.3-3.5 ms per tick in heavy rooms.
 * - A Step that runs covers the ticks since the enemy's last one: global.fr is doubled for it (put back after,
 *   unless the Step set fr itself, as enemy_hurt does for hit-stop) and its search_timeout gets the tick it missed,
 *   as in 30 fps mode.
 * - Half of the enemies run on even ticks and half on odd ones (instance id parity), so every tick carries half of
 *   the AI.
 * - Enemies find their own hits: enemy_hurt, inside their Step, polls place_meeting with obj_slash, obj_projectile
 *   (bullets, throwables), obj_explosion and obj_flamethrower_explosion. On a skipped tick the same overlaps are
 *   checked here, and an enemy touching one runs this tick (fr as it is), so hits land on the tick they would have.
 * - Enemies run every tick during scripted sequences, and a Step that didn't run for longer than a skip (pause, 30 fps
 *   mode, deactivation, the setting just turned on) covers one tick.
 * Caveats: enemy movement, aim and reactions update at 30 Hz (sprite animation, alarms and built-in motion stay at
 * 60), and anything an enemy counts in ticks without fr runs at half speed, as in 30 fps mode. Playtesting found no
 * difference in enemy behaviour.
 * Debug: the profile report has an [FPS30] half-rate line (steps run and skipped per tick), and enemy deaths
 * (create_corpse) are counted by the kind of Step they happened in.
 *
 * Per enemy (instance id at +0x80) the table holds the tick its Step last ran. A room change or a change of the
 * setting clears it, and entries not seen for EH_STALE ticks are reused (death restarts keep the room, with new ids).
 */
int enemy_half_rate = 1;
int eh_installed; // t30_install found everything half-rate enemies need
#define EH_SLOTS 512
#define EH_STALE 120
unsigned enemyhalf_runs, enemyhalf_doubled, enemyhalf_skips, enemyhalf_forced; // debug counters
static struct { uint32_t key, last; } eh_table[EH_SLOTS]; // key = id + 1, 0 = empty
static int eh_room = -2;
static runner_function **eh_functions, **eh_current_function;
static const int *eh_fn_place_meeting, *eh_fn_instance_exists;
static const char *const eh_source_names[] = { "obj_slash", "obj_projectile", "obj_explosion", "obj_flamethrower_explosion" };
#define EH_SOURCES (int)(sizeof(eh_source_names) / sizeof(*eh_source_names))
static int eh_sources[EH_SOURCES], eh_sources_found;
static int (*eh_object_find)(const char *name);

// Once per frame: a change of the setting (GRAPHICS menu) starts a fresh table, so no enemy covers more than two ticks
static void eh_setting_check(void) {
	static int seen = -1;
	if (enemy_half_rate == seen)
		return;
	seen = enemy_half_rate;
	eh_room = -2;
	if (eh_installed)
		debugPrintf("Half-rate enemies %s\n", enemy_half_rate ? "on" : "off");
}

// Once, at the first enemy Step: the object indices of the hurt sources. Without all of them half-rate stays off.
static int eh_resolve_sources(void) {
	if (eh_sources_found)
		return 1;
	int found = 0;
	for (int i = 0; i < EH_SOURCES; i++)
		found += (eh_sources[i] = eh_object_find(eh_source_names[i])) >= 0;
	debugPrintf("Half-rate enemies: hurt sources %d/%d (%s %d, %s %d, %s %d, %s %d)\n", found, EH_SOURCES,
		eh_source_names[0], eh_sources[0], eh_source_names[1], eh_sources[1], eh_source_names[2], eh_sources[2],
		eh_source_names[3], eh_sources[3]);
	if (found != EH_SOURCES) {
		eh_installed = 0;
		return 0;
	}
	return eh_sources_found = 1;
}

static uint32_t eh_word(const void *p, size_t off) {
	uint32_t v;
	memcpy(&v, (const char *)p + off, 4);
	return v;
}

// A runner builtin through its registry entry, with g_pFunction set as the GML call sites do
static int eh_builtin(const int *desc, void *self, int argc, gml_rvalue *args) {
	runner_function *f = *eh_functions + desc[1], *saved = *eh_current_function;
	gml_rvalue out = {0, 0, 5};
	*eh_current_function = f;
	((builtin_fn)f->func)(&out, self, self, argc, args);
	*eh_current_function = saved;
	return (out.kind == 0 || out.kind == 13) && out.val != 0;
}

static gml_rvalue eh_object(int index) {
	gml_rvalue r = {0, 0, 15}; // an object reference, as YYC passes obj_* constants
	uint32_t b[2] = {(uint32_t)index, 0x01000000};
	memcpy(&r.val, b, 8);
	return r;
}

// Does the enemy touch something enemy_hurt reacts to? Object existence is checked once per tick.
static int eh_hurt_overlap(void *self, uint32_t tick) {
	static uint32_t present_tick = ~0u;
	static unsigned present;
	if (present_tick != tick) {
		present_tick = tick;
		present = 0;
		for (int i = 0; i < EH_SOURCES; i++) {
			gml_rvalue obj = eh_object(eh_sources[i]);
			if (eh_sources[i] >= 0 && eh_builtin(eh_fn_instance_exists, self, 1, &obj))
				present |= 1u << i;
		}
	}
	if (!present)
		return 0;
	float x, y;
	memcpy(&x, (char *)self + 0xb8, 4);
	memcpy(&y, (char *)self + 0xbc, 4);
	for (int i = 0; i < EH_SOURCES; i++) {
		if (!(present & (1u << i)))
			continue;
		gml_rvalue args[3] = {{x, 0, 0}, {y, 0, 0}, eh_object(eh_sources[i])};
		if (eh_builtin(eh_fn_place_meeting, self, 3, args))
			return 1;
	}
	return 0;
}

// How many ticks this Step covers: 0 skip, 1 or 2
static int eh_decide(void *self) {
	uint32_t tick = *frame_count;
	if (*current_room != eh_room) {
		eh_room = *current_room;
		memset(eh_table, 0, sizeof(eh_table));
	}
	uint32_t key = eh_word(self, 0x80) + 1, i = (key * 2654435761u) % EH_SLOTS;
	int found = -1, free_slot = -1;
	for (int n = 0; n < EH_SLOTS; n++, i = (i + 1) % EH_SLOTS) {
		if (eh_table[i].key == key) {
			found = i;
			break;
		}
		if (!eh_table[i].key) {
			if (free_slot < 0)
				free_slot = i;
			break;
		}
		if (free_slot < 0 && tick - eh_table[i].last > EH_STALE)
			free_slot = i;
	}
	if (found < 0) {
		if (free_slot < 0) // table full: run every tick
			return 1;
		eh_table[free_slot].key = key;
		// A new enemy runs now (one tick) or next tick (covering this one too), by id parity
		eh_table[free_slot].last = (key ^ tick) & 1 ? tick - 3 : tick - 1;
		found = free_slot;
	}
	uint32_t elapsed = tick - eh_table[found].last;
	// Scripted sequences count ticks exactly: every tick. A Step that didn't run for longer than a skip (pause, the
	// 30 fps mode, deactivation) covers one tick, as the original would.
	if ((t30_scripted_tick && tick - t30_scripted_tick < 3) || elapsed > 2 || elapsed == 0) {
		eh_table[found].last = tick;
		return 1;
	}
	if (elapsed == 2) {
		eh_table[found].last = tick;
		return 2;
	}
	if (eh_hurt_overlap(self, tick)) {
		eh_table[found].last = tick;
		enemyhalf_forced++;
		return 1;
	}
	return 0;
}

// Debug: enemy deaths (create_corpse calls) by the kind of Step they happened in:
// 0 outside a half-rate enemy Step, 1 a Step run for a hurt overlap, 2 a Step covering two ticks, 3 a one-tick Step
static gml_script_fn orig_create_corpse;
static int eh_step_kind;
unsigned enemyhalf_kills[4];
static void *eh_create_corpse(void *self, void *other, void *result, int argc, void **args) {
	static int lines;
	enemyhalf_kills[eh_step_kind]++;
	if (lines++ < 40)
		debugPrintf("[ENEMYHALF] create_corpse at tick %u in %s\n", *frame_count, (const char *const[]){"another event",
			"a step run for a hurt overlap", "a step covering 2 ticks", "a one-tick step"}[eh_step_kind]);
	return orig_create_corpse(self, other, result, argc, args);
}

static void eh_enemy_step(event_func orig, void *self, void *other) {
	unsigned forced = enemyhalf_forced;
	int ticks = eh_decide(self);
	if (!ticks) {
		enemyhalf_skips++;
		return;
	}
	enemyhalf_runs++;
	eh_step_kind = forced != enemyhalf_forced ? 1 : ticks == 2 ? 2 : 3;
	gml_rvalue *fr = NULL, *t;
	double game_fr = 0, scaled = 0, before = -1;
	if (ticks == 2 && (fr = global_var(var_fr)) && fr->kind == 0) {
		game_fr = fr->val;
		scaled = fr->val *= 2;
		enemyhalf_doubled++;
	} else {
		fr = NULL;
	}
	if (ticks == 2 && (t = gml_vget(self, var_search_timeout[1])) && t->kind == 0)
		before = t->val;
	t30_enemy_depth++; // an inherited Step runs inside with this decision and fr
	orig(self, other);
	t30_enemy_depth--;
	eh_step_kind = 0;
	// enemy_hurt sets fr itself (0 for hit-stop on a kill): keep the game's value then
	if (fr && (fr = global_var(var_fr)) && fr->kind == 0 && fr->val == scaled)
		fr->val = game_fr;
	if (before >= 0 && (t = instance_var(self, var_search_timeout))) {
		double step = t->val - before;
		if (step == 1 || step == 2) {
			t->val += step;
			t30_search_ticks++;
		}
	}
}

static void t30_enemy_search(event_func orig, void *self, void *other) {
	if (enemy_half_rate && eh_installed && !t30_active && !t30_enemy_depth && eh_resolve_sources()) {
		eh_enemy_step(orig, self, other);
		return;
	}
	gml_rvalue *t;
	double before = -1;
	if (t30_active && t30_search && !t30_enemy_depth && (t = gml_vget(self, var_search_timeout[1])) && t->kind == 0)
		before = t->val;
	t30_enemy_steps += t30_active && !t30_enemy_depth;
	t30_enemy_depth++;  // an inherited Step runs inside: only the outermost one adds
	orig(self, other);
	t30_enemy_depth--;
	if (t30_diag && !t30_enemy_depth && (t = gml_vget(self, var_search_timeout[1])) && t->kind == 0) {
		static double last_seen[1];
		static int lines;
		if (t->val != last_seen[0] && t->val != 0 && lines++ < 80)
			debugPrintf("[T30DIAG] tick %u (%s) enemy search_timeout %.2f\n", *frame_count, t30_active ? "30" : "60", t->val);
		last_seen[0] = t->val;
	}
	if (before >= 0 && (t = instance_var(self, var_search_timeout))) {
		double step = t->val - before;
		if (step == 1 || step == 2) {
			t->val += step;
			t30_search_ticks++;
		}
	}
}

#define T30_ENEMY_STEPS(X) X(enemy) X(enemy_cop) X(enemy_sci) X(enemy_grunt) X(enemy_meele) X(enemy_redpomp) \
	X(enemy_shotgun) X(enemy_shieldcop) X(enemy_machinegun)
#define T30_ENEMY_WRAP(name) static event_func orig_##name##_step; \
	static void t30_##name##_step(void *self, void *other) { t30_enemy_search(orig_##name##_step, self, other); }
T30_ENEMY_STEPS(T30_ENEMY_WRAP)

// Dialogue typing (textbox user event 0, once per tick) types at most one character per call: text_timer counts up
// to typeCharSpd (2: 1 -> 2, then a character and back to 1). Conversations are cutscenes and run at 60, but passive
// textboxes (lines while you play) do run at 30 ticks/s: there the event runs twice per tick while the line is still
// typing. Not after it (text_complete): the finished line's hold and fade count with global.fr, already doubled.
static event_func orig_textbox_user0, orig_passive_textbox_user0;
static int *var_text_complete, t30_text2;
unsigned t30_textbox_ticks[2], t30_textbox_doubled;
static void t30_textbox_user0(void *self, void *other) {
	t30_textbox_ticks[0] += t30_active;
	orig_textbox_user0(self, other);
}
static int *var_text_timer, *var_type_spd[3];
// A number or boolean instance variable (kinds 0 real, 13 bool, 7 int32, 10 int64); -999 if absent or not numeric
static double t30_vnum(void *self, int *var) {
	gml_rvalue *v = var ? gml_vget(self, var[1]) : NULL;
	if (!v)
		return -999;
	switch (v->kind & 0xffffff) {
	case 0: case 13: return v->val;
	case 7: { int32_t i; memcpy(&i, &v->val, 4); return i; }
	case 10: { int64_t i; memcpy(&i, &v->val, 8); return (double)i; }
	default: return -999;
	}
}
static void t30_passive_textbox_user0(void *self, void *other) {
	t30_textbox_ticks[1] += t30_active;
	static int lines;
	double before = t30_diag && lines < 160 ? t30_vnum(self, var_text_timer) : 0;
	orig_passive_textbox_user0(self, other);
	int twice = t30_active && t30_text2 && t30_vnum(self, var_text_complete) == 0;
	if (twice) {
		t30_textbox_doubled++;
		orig_passive_textbox_user0(self, other);
	}
	if (t30_diag && lines < 160) {
		lines++;
		debugPrintf("[T30DIAG] passive textbox tick %u (%s%s): text_timer %.2f -> %.2f, complete %.0f, char/space/slash spd %.2f %.2f %.2f\n",
			*frame_count, t30_active ? "30" : "60", twice ? " x2" : "", before, t30_vnum(self, var_text_timer),
			t30_vnum(self, var_text_complete), t30_vnum(self, var_type_spd[0]), t30_vnum(self, var_type_spd[1]),
			t30_vnum(self, var_type_spd[2]));
	}
}

static void t30_instance_animate(void *inst) {
	t30_stage = 10;
	orig_instance_animate(inst);
	if (t30_active && t30_anim) {
		t30_stage = 11;
		orig_instance_animate(inst);
	}
	t30_stage = 0;
}

static void t30_handle_alarm(void) {
	t30_stage = 20;
	orig_handle_alarm();
	if (t30_active && t30_alarm) {
		t30_stage = 21;
		update_active_lists();
		t30_stage = 22;
		orig_handle_alarm();
	}
	t30_stage = 0;
}

static int t30_watchdog(SceSize args, void *argp) {
	uint32_t last = 0, still = 0;
	for (;;) {
		sceKernelDelayThread(500000);
		uint32_t now = *frame_count;
		if (now != last || !t30_active) { // loading screens stop ticks for seconds: armed in 30 fps mode only
			last = now;
			still = 0;
		} else if (++still == 6) {
			gml_rvalue *fr = global_var(var_fr);
			debugPrintf("[WATCHDOG] no tick for 3 s at tick %u: 30 fps stage %d, active %d, scripted tick %u, fr %f kind %u, "
				"game fr %f scaled %f (%d)\n", now, t30_stage, t30_active, t30_scripted_tick, fr ? fr->val : -1.0,
				fr ? fr->kind : 99, t30_game_fr, t30_scaled_fr, t30_fr_scaled);
			// A core dump records where every thread is
			sceKernelDelayThread(500000);
			*(volatile int *)0 = 0;
		}
	}
	return 0;
}

static void t30_handle_movement(int arg) {
	orig_handle_movement(arg);
	if (t30_active && t30_move) {
		update_active_lists();
		orig_handle_movement(arg);
	}
}

void t30_install(gml_func_entry *table) {
	var_fr = (int *)so_symbol(&yoyoloader_mod, "g_VAR_fr");
	var_freeze_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_freeze_timer");
	update_active_lists = (void *)so_symbol(&yoyoloader_mod, "_Z17UpdateActiveListsv");
	if (!var_fr || !var_freeze_timer || !update_active_lists || !global_obj_ptr || !current_room) {
		fps30 = 0;
		debugPrintf("30 fps mode: NOT installed (symbols)\n");
		return;
	}
	orig_master_user0 = gml_replace(table, "gml_Object_obj_master_Other_10", t30_master_user0);
	var_oscillate_delay = (int *)so_symbol(&yoyoloader_mod, "g_VAR_oscillate_delay");
	if (var_oscillate_delay)
		orig_letter_step = gml_replace(table, "gml_Object_obj_letter_Step_0", t30_letter_step);
	orig_cutscene_master_user0 = gml_replace(table, "gml_Object_obj_cutscene_master_Other_10", t30_cutscene_master_user0);
	int enemies = 0;
	var_search_timeout = (int *)so_symbol(&yoyoloader_mod, "g_VAR_search_timeout");
	t30_search = !exp_flag("t30search0");
	if (var_search_timeout) {
#define T30_ENEMY_HOOK(name) enemies += !!(orig_##name##_step = gml_replace(table, "gml_Object_obj_" #name "_Step_0", t30_##name##_step));
		T30_ENEMY_STEPS(T30_ENEMY_HOOK)
	}
	// Half-rate enemies. The hurt source objects are looked up by name at the first enemy Step (the object table isn't
	// loaded yet).
	eh_object_find = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	eh_functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	eh_current_function = (runner_function **)so_symbol(&yoyoloader_mod, "g_pFunction");
	eh_fn_place_meeting = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_place_meeting");
	eh_fn_instance_exists = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_instance_exists");
	eh_installed = enemies == 9 && eh_object_find && eh_functions && eh_current_function && eh_fn_place_meeting &&
		eh_fn_instance_exists;
	if (eh_installed && debugMode)
		orig_create_corpse = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z24gml_Script_create_corpseP9CInstanceS0_R8YYRValueiPPS1_"), eh_create_corpse);
	debugPrintf("Half-rate enemies: %s, yyl.cfg enemyHalfRate %d\n", eh_installed ? "installed" : "NOT installed",
		enemy_half_rate);
	t30_diag = debugMode && exp_flag("t30diag");
	var_text_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_text_timer");
	var_text_complete = (int *)so_symbol(&yoyoloader_mod, "g_VAR_text_complete");
	t30_text2 = var_text_complete && !exp_flag("t30text0");
	var_type_spd[0] = (int *)so_symbol(&yoyoloader_mod, "g_VAR_typeCharSpd");
	var_type_spd[1] = (int *)so_symbol(&yoyoloader_mod, "g_VAR_typeSpaceSpd");
	var_type_spd[2] = (int *)so_symbol(&yoyoloader_mod, "g_VAR_typeSlashSpd");
	orig_textbox_user0 = gml_replace(table, "gml_Object_obj_textbox_Other_10", t30_textbox_user0);
	orig_passive_textbox_user0 = gml_replace(table, "gml_Object_obj_passive_textbox_Other_10", t30_passive_textbox_user0);
	orig_player_animation = entry_hook(so_symbol(&yoyoloader_mod, "_Z27gml_Script_player_animationP9CInstanceS0_R8YYRValueiPPS1_"), t30_player_animation);
	orig_animation_code = orig_player_animation ? entry_hook(so_symbol(&yoyoloader_mod, "_Z25gml_Script_animation_codeP9CInstanceS0_R8YYRValueiPPS1_"), t30_animation_code) : NULL;
	t30_animation_code_hooked = orig_animation_code != NULL;
	// The script's hook also carries exp_t30trace's slowdown log, so it's installed with exp_t30slowmo0 too
	var_slowdown = (int *)so_symbol(&yoyoloader_mod, "g_VAR_slowdown");
	if (!exp_flag("t30slowmo0"))
		orig_controller_check_pressed = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z35gml_Script_controller_check_pressedP9CInstanceS0_R8YYRValueiPPS1_"), t30_controller_check_pressed);
	orig_bullettime_check = entry_hook(so_symbol(&yoyoloader_mod,
		"_Z34gml_Script_player_bullettime_checkP9CInstanceS0_R8YYRValueiPPS1_"), t30_bullettime_check);
	orig_override_control_init = entry_hook(so_symbol(&yoyoloader_mod, "_Z32gml_Script_override_control_initP9CInstanceS0_R8YYRValueiPPS1_"), t30_override_control_init);
	orig_gamepaused_step1 = entry_hook(so_symbol(&yoyoloader_mod, "_Z32gml_Object_obj_gamepaused_Step_1P9CInstanceS0_"), t30_gamepaused_step1);
	if (!orig_gamepaused_step1)
		debugPrintf("30 fps mode: pause menu hook NOT installed\n");
	orig_instance_animate = entry_hook(so_symbol(&yoyoloader_mod, "_Z16Instance_AnimateP9CInstance"), t30_instance_animate);
	orig_handle_alarm = entry_hook(so_symbol(&yoyoloader_mod, "_Z11HandleAlarmv"), t30_handle_alarm);
	orig_handle_movement = entry_hook(so_symbol(&yoyoloader_mod, "_Z22HandleInstanceMovementb"), t30_handle_movement);
	t30_anim = orig_instance_animate && !exp_flag("t30anim0");
	t30_alarm = orig_handle_alarm && !exp_flag("t30alarm0");
	t30_move = orig_handle_movement && exp_flag("t30move");
	t30_freeze = !exp_flag("t30freeze0");
	t30_trace = debugMode && exp_flag("t30trace");
	if (debugMode && exp_flag("t30watchdog")) {
		SceUID th = sceKernelCreateThread("t30 watchdog", t30_watchdog, 0x10000100, 0x2000, 0, 0, NULL);
		if (th >= 0)
			sceKernelStartThread(th, 0, NULL);
	}
	t30_never = exp_flag("t30never");
	if (exp_flag("t30force")) // testing: 30 ticks/s everywhere
		fps30 = 2;
	debugPrintf("30 fps mode %d: master %s, cutscene master %s, player animation %s, scripts %s, letters %s, animate %s, alarms %s, motion %s, hit-stop %s, enemy search %d/9, textboxes %s, slow motion %s\n",
		fps30, orig_master_user0 ? "hooked" : "NOT hooked", orig_cutscene_master_user0 ? "hooked" : "NOT hooked",
		orig_animation_code ? "hooked" : "NOT hooked", orig_override_control_init ? "hooked" : "NOT hooked",
		orig_letter_step ? "hooked" : "NOT hooked", t30_anim ? "x2" : "x1", t30_alarm ? "x2" : "x1", t30_move ? "x2" : "x1",
		t30_freeze ? "x2" : "x1", enemies, orig_textbox_user0 && orig_passive_textbox_user0 ? "counted" : "NOT hooked", orig_bullettime_check && orig_controller_check_pressed ? "x2" : "x1");
	t30_installed = 1;
}
