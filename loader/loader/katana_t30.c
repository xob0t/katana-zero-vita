/*
 * katana_t30.c
 *
 * 30 fps mode (fpsMode in yyl.cfg, or the FPS MODE menu).
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
 * 30 fps mode (fpsMode in yyl.cfg, old name fps30: 0 off, 1 in rooms that can't hold 60 fps (default), 2 everywhere). GameMaker games run one
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
int fps30 = 1;
#define T30_SETTLE_FRAMES 45
#define T30_WINDOW 90           // ~2.6 s while slow at 60, 3 s at 30
#define T30_SLOW_FRAMES 80      // frames over T30_SLOW_US (under 50 fps) to drop to 30
#define T30_SLOW_US 20000
#define T30_FAST_FRAMES 85      // ticks under T30_FAST_US of work to go back to 60
#define T30_FAST_US 14000       // leaves room for the every-other-tick work 60 ticks/s adds back
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

static void *t30_animation_code(void *self, void *other, void *result, int argc, void **args) {
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
static int t30_enemy_depth;

static void t30_enemy_search(event_func orig, void *self, void *other) {
	gml_rvalue *t;
	double before = -1;
	if (t30_active && !t30_enemy_depth && (t = gml_vget(self, var_search_timeout[1])) && t->kind == 0)
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
	if (var_search_timeout && !exp_flag("t30search0")) {
#define T30_ENEMY_HOOK(name) enemies += !!(orig_##name##_step = gml_replace(table, "gml_Object_obj_" #name "_Step_0", t30_##name##_step));
		T30_ENEMY_STEPS(T30_ENEMY_HOOK)
	}
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
