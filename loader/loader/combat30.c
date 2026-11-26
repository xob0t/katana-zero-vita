/*
 * combat30.c
 *
 * 30 FPS IN COMBAT (GRAPHICS menu row, katana_menu.c; combat30 in yyl.cfg until the row is used; off by default).
 *
 * GameMaker games run one logic tick per frame, so a fight that can't hold 60 fps plays in slow motion. With this on,
 * combat rooms run 30 ticks a second, paced by the display (two vblanks per frame), and each tick advances the game
 * twice as far, so they play at full speed at a steady 30 fps. Every other room stays at 60.
 *
 * Combat rooms. The first tick of the room's time controller (obj_cutscene_master, placed in every level room, or
 * obj_master) decides: a room is a combat room when an enemy (obj_enemy or a child) or a boss exists then. The
 * decision holds until the next room or a new controller instance (a death restart recreates the room's instances), so
 * a room never switches to 30 in the middle of play.
 * Bosses: obj_headhunter (Headhunter), obj_kissyface (Kissyface), obj_cutscene_studio_finale and obj_V_fight (V),
 * obj_cutscene_psych_11_battle and obj_psychboss_master (the psychiatrist), obj_cutscene_mansion_bikechase (the bike
 * chase). The game data (game.droid ROOM instances) places obj_enemy children in 70 level rooms, from room_factory_0
 * to room_bunker2_3; room_psychiatrist counts only when the secret fight starts there. The decision and its reason are logged on every room entry.
 * A change of the setting applies at the next room. Each tick of a scripted sequence (the game drives the player
 * through override_control_init: the first level's tutorial, cutscenes, talking) runs at 60 whatever the room:
 * those count ticks with exact comparisons (control_timer == 10) and broke at 30 (the tutorial's slow motion came
 * too late and the player was shot). The room goes back to 30 three ticks after the sequence ends.
 *
 * What a 30 Hz tick converts (the same as the removed Nov 2026 30 fps mode, which played the whole game this way):
 * - The game's time factor global.fr (slow motion; 0 during hit-stop) is doubled right after obj_master and
 *   obj_cutscene_master user event 0 set it each tick. Before those run, a value we doubled is put back, so a
 *   controller that keeps the previous value (pause) does not double it again.
 * - The hit-stop counter (freeze_timer of those controllers) counts two ticks per tick, and so do the level timer
 *   (global.time -= 1) and global.wait in obj_master's user event 0 (count_twice).
 * - The player's animation runs with fr = 1 (player_animation overrides it around animation_code): 2 here (the entry
 *   hook on animation_code is fx_rate.c's when it has one, which calls combat30_animation_code).
 * - The runner's built-in clocks run twice per tick: sprite animation (Instance_Animate: runner_fast.c's native one
 *   runs twice, or this file's hook without it) and alarms (HandleAlarm). Built-in motion (speed, hspeed) is not
 *   doubled: the game's own motion multiplies its speeds by fr.
 * - Enemies' search_timeout gets the tick it missed (enemy_half_rate.c, which owns the enemy Step wrappers), and
 *   half-rate enemies are off: enemies think every tick, as at 60 ticks/s.
 * - A passive textbox (a line spoken while you play) types twice per tick while its line is incomplete: its typing
 *   counts ticks. Its hold and fade once complete use global.fr.
 * - Slow motion's ramp and meter (player_bullettime_check, ticks) run twice per tick; the second pass sees no new key
 *   press (controller_check_pressed false), since the press belongs to the first.
 * - The loader's every-other-tick or 1-in-N work keeps its rate in real time: lighting refresh, cloth, rain and
 *   particle drawing run every tick, the smoke and ember rate (fx_rate.c) and lamp sway (light_fast_inline.c) use
 *   half the period, and dynamic resolution doesn't count the wait for the second vblank as GPU load. Texture page
 *   eviction (pages unused for 120 frames) keeps counting frames: 4 s instead of 2 at 30, which only evicts less.
 * Not converted (known differences): input is read 30 times a second; built-in motion (lamps sway at half speed);
 * anything else the game counts in ticks without fr (the death slow-mo ramp of obj_master, the pause menu and the
 * rewind effects) runs at half speed in real time. The pause menu runs at 30 in a combat room.
 *
 * Debug: exp_c30all puts every room with a time controller at 30 (testing), exp_c30trace logs global.slowdown each
 * tick it moves ([C30SLOW]) and global.time every 30 ticks ([C30TIME]), to compare in ticks with a 60 run.
 * exp_c30anim0, exp_c30alarm0, exp_c30freeze0, exp_c30timer0, exp_c30text0 and exp_c30slowmo0 turn one conversion
 * off; exp_c30menu0 leaves the menu row out.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "asyncsave.h"
#include "yyl_config.h"

#include "katana_internal.h"

int combat30_cfg = 0;  // combat30 in yyl.cfg: 1 on (the menu's choice, when saved, overrides it)
int combat30_on;       // the setting now: the menu's saved choice, else yyl.cfg
int combat30_menu = -1; // the menu's saved choice (combat30.cfg in the save folder), -1 none
int c30_active;        // this tick runs at 30 ticks/s with the time doubled
int c30_anim = 1;      // Instance_Animate twice per tick (runner_fast.c reads it)
int c30_installed;     // everything the conversion needs is hooked
static int c30_all, c30_alarm, c30_freeze, c30_text2, c30_trace, c30_timer;
static int room_combat;          // the current room has enemies or a boss
static int room_on;              // and runs at 30 (the setting as it was at the room's first tick)
static int decided_room = -2;    // the room it was made for
static uint32_t decided_id;      // and the time controller (instance id) that made it
static int *var_fr, *var_freeze_timer, *var_text_complete, *var_slowdown, *var_time, *var_wait;
static int t30_fr_scaled;        // global.fr holds t30_scaled_fr, our doubling of the game's t30_game_fr
static double t30_game_fr, t30_scaled_fr;
static event_func orig_master_user0, orig_cutscene_master_user0, orig_passive_textbox_user0;
static gml_script_fn orig_player_animation, orig_animation_code, orig_bullettime_check, orig_controller_check_pressed;
static void (*orig_instance_animate)(void *inst);
static void (*orig_handle_alarm)(void);
static void (*update_active_lists)(void);
static int in_player_animation;
static int (*object_find)(const char *name);
static const char *(*room_name)(int index);
static runner_function **functions, **current_function;
static const int *fn_instance_exists;
unsigned c30_switches, c30_textbox_doubled, c30_bullettime_doubled, c30_alarm_doubled;

// Rooms count as combat rooms with any of these at their first tick (obj_enemy covers its children)
static const char *const combat_objects[] = {
	"obj_enemy", "obj_headhunter", "obj_kissyface", "obj_cutscene_studio_finale", "obj_V_fight",
	"obj_cutscene_psych_11_battle", "obj_psychboss_master", "obj_cutscene_mansion_bikechase",
};
#define N_COMBAT_OBJECTS (int)(sizeof(combat_objects) / sizeof(*combat_objects))
static int combat_index[N_COMBAT_OBJECTS];
static int objects_resolved;

extern uint32_t scripted_tick; // enemy_half_rate.c: the last tick a scripted sequence drove the player
extern int scripted_hooked;

static const char *room_label(int room) {
	const char *n = room_name && room >= 0 ? room_name(room) : NULL;
	return n ? n : "?";
}

static int scripted_now(void) {
	return scripted_tick && *frame_count - scripted_tick < 3;
}

static void c30_set(int on) {
	if (on == c30_active)
		return;
	c30_active = on;
	eglSwapInterval(NULL, on ? 2 : 1);
	c30_switches++;
}

// The menu's saved choice: a key=value file next to the saves, so it is backed up and restored with them
static void menu_path(char *out, size_t size) {
	extern char data_path[];
	snprintf(out, size, "%scombat30.cfg", data_path);
}

static void menu_load(void) {
	char path[300], key[YYL_KEY_SIZE];
	int value;
	menu_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	while (yyl_read_setting(f, key, &value))
		if (!strcmp(key, "combat30"))
			combat30_menu = value ? 1 : 0;
	fclose(f);
}

// From the GRAPHICS page's SAVE (katana_menu.c): takes effect at the next room
void combat30_menu_save(int on) {
	on = on ? 1 : 0;
	if (on == combat30_menu)
		return;
	char path[300], text[32];
	menu_path(path, sizeof(path));
	int len = snprintf(text, sizeof(text), "combat30=%d\n", on);
	int r = asyncsave_commit_file(path, text, (size_t)len);
	combat30_menu = on;
	combat30_on = on;
	debugPrintf("[COMBAT30] menu: 30 FPS IN COMBAT %s (from the next room)%s\n", on ? "ON" : "OFF",
		r < 0 ? ", NOT saved (write failed)" : "");
}

static int builtin_true(const int *desc, void *self, int argc, gml_rvalue *args) {
	runner_function *f = *functions + desc[1], *saved = *current_function;
	gml_rvalue out = {0, 0, 5};
	*current_function = f;
	((builtin_fn)f->func)(&out, self, self, argc, args);
	*current_function = saved;
	return (out.kind == 0 || out.kind == 13) && out.val != 0;
}

// The first combat object that exists, or NULL
static const char *combat_object_present(void *self) {
	if (!objects_resolved) {
		objects_resolved = 1;
		for (int i = 0; i < N_COMBAT_OBJECTS; i++)
			combat_index[i] = object_find(combat_objects[i]);
	}
	for (int i = 0; i < N_COMBAT_OBJECTS; i++) {
		if (combat_index[i] < 0)
			continue;
		gml_rvalue obj = {0, 0, 15}; // an object reference, as YYC passes obj_* constants
		uint32_t b[2] = {(uint32_t)combat_index[i], 0x01000000};
		memcpy(&obj.val, b, 8);
		if (builtin_true(fn_instance_exists, self, 1, &obj))
			return combat_objects[i];
	}
	return NULL;
}

// The room's first tick: is it a combat room? self is the time controller running its user event
static void decide_room(void *self) {
	int again = decided_room == *current_room;
	decided_room = *current_room;
	const char *why = self ? combat_object_present(self) : NULL;
	room_combat = why || (c30_all && self);
	int on = room_on = room_combat && combat30_on;
	debugPrintf("[COMBAT30] room %s (%d)%s: %s -> %s\n", room_label(decided_room), decided_room, again ? " restart" : "",
		why ? why : room_combat ? "exp_c30all" : self ? "no enemy or boss" : "no time controller",
		on ? "30 ticks/s" : room_combat ? "60 ticks/s (30 FPS IN COMBAT off)" : "60 ticks/s");
	c30_set(on && !scripted_now());
}

// Once per presented frame (katana_after_swap): scripted sequences at 60, a room without a time controller at 60
void combat30_update(void) {
	if (!c30_installed || !current_room)
		return;
	if (*current_room != decided_room) {
		// No time controller ran in this room yet: 60 until one does. A title, menu or results room has none: logged
		// once, after 30 frames
		static int waiting_room = -2, frames;
		c30_set(0);
		if (*current_room != waiting_room) {
			waiting_room = *current_room;
			frames = 0;
		}
		if (++frames == 30)
			debugPrintf("[COMBAT30] room %s (%d): no time controller -> 60 ticks/s\n", room_label(*current_room),
				*current_room);
		return;
	}
	c30_set(room_on && !scripted_now());
}

// obj_master user event 0 counts the level timer and a delay in ticks, not with fr: L62 global.time -= 1 (when the
// level is timed), L67 if (global.wait > 0) global.wait -= 1, L69-71 clamped at 0 (L75-77: time <= 0 ends the level).
// At 30 ticks/s each step of exactly 1 is taken twice, so the time limit lasts as long in real time. The second step
// stops at 0 (a time that reaches 0 this way ends the level on the next tick, 33 ms later); a value already below 0
// (rooms without a time limit count on below 0) keeps counting.
static void count_twice(int *var, double before) {
	gml_rvalue *v = global_var(var);
	if (!v || v->kind != 0 || before - v->val != 1.0)
		return;
	double next = v->val - 1.0;
	v->val = v->val >= 0.0 && next < 0.0 ? 0.0 : next;
}

static double global_or(int *var, double fallback) {
	gml_rvalue *v = global_var(var);
	return v && v->kind == 0 ? v->val : fallback;
}

static void time_controller(event_func orig, void *self, void *other) {
	static int depth;
	if (depth) { // an inherited user event inside: the outer call converts
		orig(self, other);
		return;
	}
	// A new room, or a new controller in it (a death restart, or a controller created later): decide again
	uint32_t id = *(uint32_t *)((char *)self + 0x80);
	if (*current_room != decided_room || id > decided_id) {
		decided_id = id;
		decide_room(self);
	}
	gml_rvalue *fr = global_var(var_fr);
	if (t30_fr_scaled && fr && fr->kind == 0 && fr->val == t30_scaled_fr)
		fr->val = t30_game_fr;
	t30_fr_scaled = 0;
	gml_rvalue *freeze;
	if (c30_active && c30_freeze && (freeze = instance_var(self, var_freeze_timer)) && freeze->val >= 1)
		freeze->val -= 1;
	double time_before = global_or(var_time, -1e9), wait_before = global_or(var_wait, -1e9);
	depth++;
	orig(self, other);
	depth--;
	if (c30_active && c30_timer) {
		count_twice(var_time, time_before);
		count_twice(var_wait, wait_before);
	}
	if (c30_active && (fr = global_var(var_fr)) && fr->kind == 0) {
		t30_game_fr = fr->val;
		t30_scaled_fr = fr->val *= 2;
		t30_fr_scaled = 1;
	}
	if (c30_trace) {
		static uint32_t last;
		gml_rvalue *t = global_var(var_time);
		if (t && t->kind == 0 && *frame_count - last >= (c30_active ? 15u : 30u)) {
			last = *frame_count;
			debugPrintf("[C30TIME] tick %u %s time %.3f\n", *frame_count, c30_active ? "30" : "60", t->val);
		}
	}
}

static void master_user0(void *self, void *other) {
	time_controller(orig_master_user0, self, other);
}

static void cutscene_master_user0(void *self, void *other) {
	time_controller(orig_cutscene_master_user0, self, other);
}

static void *player_animation(void *self, void *other, void *result, int argc, void **args) {
	in_player_animation++;
	void *r = orig_player_animation(self, other, result, argc, args);
	in_player_animation--;
	return r;
}

// animation_code inside player_animation, which runs it with fr = 1: fr = 2 at 30 ticks/s. orig is the script (or
// whatever an entry hook on it calls through)
void *combat30_animation_code(gml_script_fn orig, void *self, void *other, void *result, int argc, void **args) {
	gml_rvalue *fr;
	if (!c30_active || !in_player_animation || !(fr = global_var(var_fr)) || fr->kind != 0 || fr->val != 1.0)
		return orig(self, other, result, argc, args);
	fr->val = 2.0;
	void *r = orig(self, other, result, argc, args);
	if ((fr = global_var(var_fr)) && fr->kind == 0 && fr->val == 2.0)
		fr->val = 1.0;
	return r;
}

static void *animation_code(void *self, void *other, void *result, int argc, void **args) {
	return combat30_animation_code(orig_animation_code, self, other, result, argc, args);
}

// Slow motion (player_bullettime_check, once per tick from player_input) counts in ticks, not with global.fr: while
// the key is held the ramp speeds up (slow_sp -= 0.02) and global.slowdown moves by slow_sp, the meter drains
// (bullettime_timer -= 1, bullettime_max -= 0.1); released, slow_sp += 0.005 and the meter refills by 0.5. At 30
// ticks/s all of it would run at half speed. The script runs twice per tick, as two 60 Hz ticks would: the second
// pass sees no new key press (controller_check_pressed is false, held keys stay held).
static int bullettime_second;
static void *controller_check_pressed(void *self, void *other, void *result, int argc, void **args) {
	if (!bullettime_second)
		return orig_controller_check_pressed(self, other, result, argc, args);
	gml_rvalue *r = result;
	r->val = 0;
	r->flags = 0;
	r->kind = 0;
	return result;
}

static void *bullettime_check(void *self, void *other, void *result, int argc, void **args) {
	void *r = orig_bullettime_check(self, other, result, argc, args);
	if (c30_active && orig_controller_check_pressed) {
		gml_rvalue scratch = {0, 0, 5}; // the script returns nothing (undefined); keep the first pass's result
		bullettime_second = 1;
		orig_bullettime_check(self, other, &scratch, argc, args);
		bullettime_second = 0;
		c30_bullettime_doubled++;
	}
	if (c30_trace) { // exp_c30trace: global.slowdown each tick while it moves
		static double last = 1.0;
		gml_rvalue *s = global_var(var_slowdown);
		if (s && s->kind == 0 && (s->val != last || s->val != 1.0))
			debugPrintf("[C30SLOW] tick %u %s slowdown %.4f\n", *frame_count, c30_active ? "30" : "60", s->val);
		if (s && s->kind == 0)
			last = s->val;
	}
	return r;
}

// A passive textbox types at most one character per user event 0 (text_timer counts up to typeCharSpd), once per
// tick: twice per tick while the line is still typing. Not after it (text_complete): the finished line's hold and
// fade count with global.fr, already doubled.
static double vnum(void *self, int *var) {
	gml_rvalue *v = var ? gml_vget(self, var[1]) : NULL;
	return v && (v->kind == 0 || v->kind == 13) ? v->val : -999;
}

static void passive_textbox_user0(void *self, void *other) {
	orig_passive_textbox_user0(self, other);
	if (c30_active && c30_text2 && vnum(self, var_text_complete) == 0) {
		c30_textbox_doubled++;
		orig_passive_textbox_user0(self, other);
	}
}

static void instance_animate(void *inst) {
	orig_instance_animate(inst);
	// Not again for an instance its animation end event destroyed or deactivated (flags +0x7c)
	if (c30_active && c30_anim && !(*(uint32_t *)((char *)inst + 0x7c) & 3))
		orig_instance_animate(inst);
}

static void handle_alarm(void) {
	orig_handle_alarm();
	if (c30_active && c30_alarm) {
		update_active_lists();
		orig_handle_alarm();
		c30_alarm_doubled++;
	}
}

void combat30_report(int frames) {
	static unsigned last[4];
	unsigned now[4] = { c30_switches, c30_textbox_doubled, c30_bullettime_doubled, c30_alarm_doubled };
	if (debugMode && frames > 0 && memcmp(now, last, sizeof(now)) && c30_installed)
		debugPrintf("[COMBAT30] %s ticks/s | switches %u, passive textbox x2 %u, slow-mo x2 %u, alarm passes x2 %u\n",
			c30_active ? "30" : "60", now[0], now[1] - last[1], now[2] - last[2], now[3] - last[3]);
	memcpy(last, now, sizeof(last));
}

void combat30_install(gml_func_entry *table) {
	menu_load();
	combat30_on = combat30_menu >= 0 ? combat30_menu : combat30_cfg != 0;
	var_fr = (int *)so_symbol(&yoyoloader_mod, "g_VAR_fr");
	var_freeze_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_freeze_timer");
	var_text_complete = (int *)so_symbol(&yoyoloader_mod, "g_VAR_text_complete");
	var_slowdown = (int *)so_symbol(&yoyoloader_mod, "g_VAR_slowdown");
	var_time = (int *)so_symbol(&yoyoloader_mod, "g_VAR_time");
	var_wait = (int *)so_symbol(&yoyoloader_mod, "g_VAR_wait");
	update_active_lists = (void *)so_symbol(&yoyoloader_mod, "_Z17UpdateActiveListsv");
	object_find = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	room_name = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_Namei");
	functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	current_function = (runner_function **)so_symbol(&yoyoloader_mod, "g_pFunction");
	fn_instance_exists = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_instance_exists");
	if (!var_fr || !var_freeze_timer || !update_active_lists || !object_find || !functions || !current_function ||
			!fn_instance_exists || !global_obj_ptr || !current_room || !frame_count || !scripted_hooked) {
		debugPrintf("30 fps in combat: NOT installed (symbols%s)\n", scripted_hooked ? "" : ", scripted-sequence hook");
		return;
	}
	c30_all = exp_flag("c30all");
	c30_trace = debugMode && exp_flag("c30trace");
	c30_freeze = !exp_flag("c30freeze0");
	c30_timer = var_time && var_wait && !exp_flag("c30timer0");
	c30_alarm = !exp_flag("c30alarm0");
	c30_anim = !exp_flag("c30anim0");
	c30_text2 = var_text_complete && !exp_flag("c30text0");
	// The time controllers last (outermost), after pc_rules.c's obj_master wrapper
	orig_master_user0 = gml_replace(table, "gml_Object_obj_master_Other_10", master_user0);
	orig_cutscene_master_user0 = gml_replace(table, "gml_Object_obj_cutscene_master_Other_10", cutscene_master_user0);
	orig_passive_textbox_user0 = gml_replace(table, "gml_Object_obj_passive_textbox_Other_10", passive_textbox_user0);
	if (!orig_passive_textbox_user0)
		c30_text2 = 0;
	orig_player_animation = entry_hook(so_symbol(&yoyoloader_mod,
		"_Z27gml_Script_player_animationP9CInstanceS0_R8YYRValueiPPS1_"), player_animation);
	// fx_rate.c's entry hook on animation_code calls combat30_animation_code; without one, this file's
	extern int fx_animation_code_hooked;
	if (orig_player_animation && !fx_animation_code_hooked)
		orig_animation_code = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z25gml_Script_animation_codeP9CInstanceS0_R8YYRValueiPPS1_"), animation_code);
	int anim_fr = orig_player_animation && (fx_animation_code_hooked || orig_animation_code);
	if (!exp_flag("c30slowmo0"))
		orig_controller_check_pressed = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z35gml_Script_controller_check_pressedP9CInstanceS0_R8YYRValueiPPS1_"), controller_check_pressed);
	if (orig_controller_check_pressed || c30_trace)
		orig_bullettime_check = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z34gml_Script_player_bullettime_checkP9CInstanceS0_R8YYRValueiPPS1_"), bullettime_check);
	orig_handle_alarm = entry_hook(so_symbol(&yoyoloader_mod, "_Z11HandleAlarmv"), handle_alarm);
	if (!orig_handle_alarm)
		c30_alarm = 0;
	// 30 ticks/s needs both time controllers (global.fr doubled): without them it never activates
	c30_installed = orig_master_user0 && orig_cutscene_master_user0;
	debugPrintf("30 fps in combat: %s, %s (menu %d, yyl.cfg combat30 %d); master %s, cutscene master %s, player "
		"animation %s, alarms %s, hit-stop %s, level timer %s, passive textboxes %s, slow motion %s\n",
		c30_installed ? "installed" : "NOT installed", combat30_on ? "ON" : "OFF", combat30_menu, combat30_cfg,
		orig_master_user0 ? "hooked" : "NOT hooked", orig_cutscene_master_user0 ? "hooked" : "NOT hooked",
		anim_fr ? "fr x2" : "NOT hooked", c30_alarm ? "x2" : "x1", c30_freeze ? "x2" : "x1", c30_timer ? "x2" : "x1",
		c30_text2 ? "x2" : "x1",
		orig_bullettime_check && orig_controller_check_pressed ? "x2" : "x1");
}

// After runner_fast_install (katana_late_install): its native Instance_Animate runs twice at 30 by itself; without it
// this file hooks the runner's
void combat30_late_install(void) {
	extern int runner_fast_anim_on;
	if (!c30_installed || !c30_anim || runner_fast_anim_on)
		return;
	orig_instance_animate = entry_hook(so_symbol(&yoyoloader_mod, "_Z16Instance_AnimateP9CInstance"), instance_animate);
	if (!orig_instance_animate)
		c30_anim = 0;
	debugPrintf("30 fps in combat: sprite animation %s\n", orig_instance_animate ? "x2 (runner's Instance_Animate)" :
		"x1 (NOT hooked)");
}
