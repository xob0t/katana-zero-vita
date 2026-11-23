/*
 * fx_rate.c
 *
 * Heavy visual effects update on every 4th tick: settled smoke clouds (obj_smokecloud) and burning-corpse embers
 * (obj_corpsefire). club_4's broken-pipe smoke (84 clouds) cost 3.4 ms of Step per tick; at 1/4 it's 1.15 ms
 * (process 27.4 -> 25.2 ms, 35.7 -> 38.8 fps, NATIVE, 500 MHz), and the smoke looked the same in play.
 * exp_fx0 turns it off, exp_fx2 / exp_fx3 pick another N (testing).
 *
 * Instances are staggered by id: an instance updates when (id + tick) % N == 0, so each tick carries 1/N of them. On
 * its update tick the Step runs with global.fr multiplied by the ticks it covers (those since its last run, N
 * normally), then fr is put back. What the scaled fr gets wrong is corrected around the call:
 * - obj_smokecloud Step ends with `prev_speed = speed; speed *= fr`, and the runner applies built-in speed on every
 *   tick, Step or not: speed is divided by the ticks covered afterwards, so the cloud drifts the per-tick distance on
 *   each of them. animation_code (the cloud's frame counter) drops the excess of its accumulator, so fr x N would not
 *   be N ticks of animation (dissolve would last up to 50% longer at N=3, and clouds block enemy sight until they are
 *   gone): while the wrapped Step runs, animation_code runs once per tick covered with the real fr instead
 *   (fx_animation_code, called from the 30 fps mode's hook on it). Clouds that are still flying to their place
 *   (in_place false: a new cloud and its first Steps) run every tick, so they appear and spread smoothly.
 * - Contacts stay at full rate. A settled cloud on screen (in_screen, set by its Draw) tests the original Step's three
 *   gates every tick: place_meeting(x, y, obj_enemy) (the touched enemy gets first_attack_timer >= 20 and
 *   whip_timer = 30, and a moving one dissolves and pushes the cloud), obj_player (the same push) and obj_slash
 *   (alpha -= 0.02 per tick of overlap, no fr). A skipped cloud with any of them true runs its Step on that tick,
 *   covering the ticks since its last run (smoke_last), so each of those writes happens on the tick and at the place
 *   it would have, and the fade counts every overlapping tick once.
 * - obj_corpsefire (burning-corpse embers) Step sets image_speed from fr, and the runner animates every tick (its
 *   Animation End destroys the ember): image_speed is divided by N. Its own motion (x += hsp * fr) would move in
 *   jumps every N ticks; the jump is spread over the N ticks with built-in hspeed/vspeed, which the runner applies
 *   after the Step (the ember doesn't use built-in motion otherwise).
 * In 30 fps mode a tick already covers two 60 Hz ticks (fr doubled), so the period there is N/2 rounded up: the same
 * update rate in real time (exp_fxkeep30 keeps N, fr up to 2N).
 * Debug counters print with the profiler as [FXRATE].
 */

#include <vitasdk.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

typedef int (*builtin_var_fn)(void *inst, int index, gml_rvalue *value);

int fx_rate_n;           // N: 4, or from exp_fx0 / exp_fx2 / exp_fx3
static int fx_keep30;    // exp_fxkeep30: N also at 30 ticks/s
static int *var_fr, *var_in_place, *var_animation_finished;
static builtin_var_fn gv_speed, sv_speed, gv_image_speed, sv_image_speed, gv_x, gv_y, sv_x, sv_y, sv_hspeed, sv_vspeed;
static event_func orig_smoke_step, orig_corpsefire_step;

// animation_code repeat request (fx_animation_code): times, and the real fr to run it with
int fx_anim_repeat;
static double fx_anim_fr;

unsigned fx_smoke_full, fx_smoke_update, fx_smoke_skip, fx_smoke_contacts, fx_anim_calls, fx_ember_update, fx_ember_skip;

static int fx_period(void) {
	int n = fx_rate_n;
	if (t30_active && !fx_keep30)
		n = (n + 1) / 2;
	return n;
}

static int fx_due(void *self, int n) {
	uint32_t id = *(uint32_t *)((char *)self + 0x80);
	return (id + *frame_count) % (unsigned)n == 0;
}

static double builtin_get(builtin_var_fn get, void *self) {
	gml_rvalue v = {0, 0, 0};
	get(self, (int)0x80000000, &v);
	return v.kind == 0 ? v.val : 0;
}

static void builtin_set(builtin_var_fn set, void *self, double x) {
	gml_rvalue v = {x, 0, 0};
	set(self, (int)0x80000000, &v);
}

// A real or boolean instance variable, NULL otherwise
static gml_rvalue *fx_var(void *self, int *var) {
	gml_rvalue *v = var ? gml_vget(self, var[1]) : NULL;
	return v && (v->kind == 0 || v->kind == 13) ? v : NULL;
}

// global.fr times n for the call; returns the real fr (or -1 if fr isn't a number)
static double fx_scale_fr(int n) {
	gml_rvalue *fr = global_var(var_fr);
	if (!fr || fr->kind != 0)
		return -1;
	double real = fr->val;
	fr->val = real * n;
	return real;
}

static void fx_restore_fr(double real, int n) {
	gml_rvalue *fr = global_var(var_fr);
	if (fr && fr->kind == 0 && fr->val == real * n)
		fr->val = real;
}

// From the 30 fps mode's animation_code hook while fx_anim_repeat > 1: the script runs fx_anim_repeat times with the
// real fr, and animation_finished is true if any run finished
void *fx_animation_code(gml_script_fn orig, void *self, void *other, void *result, int argc, void **args) {
	int n = fx_anim_repeat;
	fx_anim_repeat = 0; // nothing inside runs a wrapped Step, but don't recurse either way
	gml_rvalue *fr = global_var(var_fr);
	double scaled = fr && fr->kind == 0 ? fr->val : 0;
	if (fr && fr->kind == 0)
		fr->val = fx_anim_fr;
	int finished = 0;
	void *ret = result;
	for (int i = 0; i < n; i++) {
		gml_rvalue scratch = {0, 0, 5}; // the script returns nothing; keep the first run's result
		void *r = orig(self, other, i == 0 ? result : &scratch, argc, args);
		if (i == 0)
			ret = r;
		gml_rvalue *f = fx_var(self, var_animation_finished);
		finished |= f && f->val != 0;
		fx_anim_calls++;
	}
	gml_rvalue *f = fx_var(self, var_animation_finished);
	if (finished && f)
		f->val = 1.0;
	if ((fr = global_var(var_fr)) && fr->kind == 0 && fr->val == fx_anim_fr)
		fr->val = scaled;
	fx_anim_repeat = n;
	return ret;
}

// The smoke Step's contact gates: place_meeting through the registry, with g_pFunction set as the GML call sites do
static runner_function **fx_functions, **fx_current_function;
static const int *fx_fn_place_meeting;
static int (*fx_object_find)(const char *name);
static int *var_in_screen;
static int fx_contact_objects[3] = {-2, -2, -2}; // obj_enemy, obj_player, obj_slash; -2 not looked up yet

static int fx_place_meeting(void *self, float x, float y, int object) {
	gml_rvalue args[3] = {{x, 0, 0}, {y, 0, 0}, {0, 0, 15}};
	uint32_t ref[2] = {(uint32_t)object, 0x01000000}; // an object reference, as YYC passes obj_* constants
	memcpy(&args[2].val, ref, 8);
	runner_function *f = *fx_functions + fx_fn_place_meeting[1], *saved = *fx_current_function;
	gml_rvalue out = {0, 0, 5};
	*fx_current_function = f;
	((builtin_fn)f->func)(&out, self, self, 3, args);
	*fx_current_function = saved;
	return (out.kind == 0 || out.kind == 13) && out.val != 0;
}

// Would this tick's Step take one of its contact branches? -1 if it can't tell (the Step runs then)
static int fx_smoke_contact(void *self) {
	gml_rvalue *v = fx_var(self, var_in_screen);
	if (!v)
		return -1;
	if (v->val == 0)
		return 0; // the contacts are inside if (in_screen)
	static const char *const names[3] = {"obj_enemy", "obj_player", "obj_slash"};
	float x, y;
	memcpy(&x, (char *)self + 0xb8, 4);
	memcpy(&y, (char *)self + 0xbc, 4);
	for (int i = 0; i < 3; i++) {
		if (fx_contact_objects[i] == -2)
			fx_contact_objects[i] = fx_object_find(names[i]);
		if (fx_contact_objects[i] < 0)
			return -1;
		if (fx_place_meeting(self, x, y, fx_contact_objects[i]))
			return 1;
	}
	return 0;
}

// The tick each settled cloud's Step last ran (key id + 1, 0 empty); a room change clears it, and entries not seen
// for SMOKE_STALE ticks are reused
#define SMOKE_SLOTS 1024
#define SMOKE_STALE 240
static struct { uint32_t key, last; } smoke_table[SMOKE_SLOTS];
static int smoke_room = -2;

static uint32_t *smoke_last(void *self, uint32_t tick) {
	if (*current_room != smoke_room) {
		smoke_room = *current_room;
		memset(smoke_table, 0, sizeof(smoke_table));
	}
	uint32_t key = *(uint32_t *)((char *)self + 0x80) + 1, i = (key * 2654435761u) % SMOKE_SLOTS;
	int free_slot = -1;
	for (int n = 0; n < SMOKE_SLOTS; n++, i = (i + 1) % SMOKE_SLOTS) {
		if (smoke_table[i].key == key)
			return &smoke_table[i].last;
		if (!smoke_table[i].key) {
			if (free_slot < 0)
				free_slot = i;
			break;
		}
		if (free_slot < 0 && tick - smoke_table[i].last > SMOKE_STALE)
			free_slot = i;
	}
	if (free_slot < 0)
		return NULL; // full: this cloud runs every tick
	smoke_table[free_slot].key = key;
	smoke_table[free_slot].last = tick - 1; // first seen settled: it ran last tick (in_place false runs every tick)
	return &smoke_table[free_slot].last;
}

static void smoke_step(void *self, void *other) {
	int n = fx_period();
	uint32_t tick = *frame_count, *last = NULL;
	gml_rvalue *v;
	if (n <= 1 || !(v = fx_var(self, var_in_place)) || v->val == 0 || !(last = smoke_last(self, tick))) {
		fx_smoke_full += n > 1;
		orig_smoke_step(self, other);
		return;
	}
	// The ticks this Step covers: those since its last run; one after a longer gap (deactivated, paused), as the
	// original would
	uint32_t k = tick - *last;
	if (k == 0 || k > (uint32_t)n) {
		k = 1;
	} else if (k < (uint32_t)n && !fx_due(self, n)) {
		int contact = fx_smoke_contact(self);
		if (!contact) {
			fx_smoke_skip++;
			return;
		}
		fx_smoke_contacts += contact > 0;
	}
	*last = tick;
	if (k == 1) {
		fx_smoke_full++;
		orig_smoke_step(self, other);
		return;
	}
	fx_smoke_update++;
	double fr = fx_scale_fr((int)k);
	if (fr < 0) {
		orig_smoke_step(self, other);
		return;
	}
	fx_anim_fr = fr;
	fx_anim_repeat = (int)k;
	orig_smoke_step(self, other);
	fx_anim_repeat = 0;
	fx_restore_fr(fr, (int)k);
	double speed = builtin_get(gv_speed, self);
	if (speed != 0)
		builtin_set(sv_speed, self, speed / k);
}

static void corpsefire_step(void *self, void *other) {
	int n = fx_period();
	if (n <= 1) {
		orig_corpsefire_step(self, other);
		return;
	}
	if (!fx_due(self, n)) {
		fx_ember_skip++;
		return;
	}
	fx_ember_update++;
	double spd = builtin_get(gv_image_speed, self);
	double x = builtin_get(gv_x, self), y = builtin_get(gv_y, self);
	double fr = fx_scale_fr(n);
	orig_corpsefire_step(self, other);
	if (fr < 0)
		return;
	fx_restore_fr(fr, n);
	double after = builtin_get(gv_image_speed, self);
	if (after != spd)
		builtin_set(sv_image_speed, self, after / n);
	double dx = builtin_get(gv_x, self) - x, dy = builtin_get(gv_y, self) - y;
	builtin_set(sv_hspeed, self, dx / n);
	builtin_set(sv_vspeed, self, dy / n);
	if (dx != 0)
		builtin_set(sv_x, self, x);
	if (dy != 0)
		builtin_set(sv_y, self, y);
}

void fx_rate_report(int frames) {
	if (!fx_rate_n)
		return;
	if (fx_smoke_full || fx_smoke_update || fx_smoke_skip)
		debugPrintf("[FXRATE] N %d (now %d): smoke steps full %.1f updated %.1f skipped %.1f per tick, contact runs %u, "
			"animation_code runs %.1f per tick\n", fx_rate_n, fx_period(), (float)fx_smoke_full / frames,
			(float)fx_smoke_update / frames, (float)fx_smoke_skip / frames, fx_smoke_contacts, (float)fx_anim_calls / frames);
	if (fx_ember_update || fx_ember_skip)
		debugPrintf("[FXRATE] embers updated %.1f skipped %.1f per tick\n", (float)fx_ember_update / frames,
			(float)fx_ember_skip / frames);
	fx_smoke_full = fx_smoke_update = fx_smoke_skip = fx_smoke_contacts = fx_anim_calls = fx_ember_update = fx_ember_skip = 0;
}

void fx_rate_install(gml_func_entry *table) {
	extern int t30_animation_code_hooked;
	fx_rate_n = exp_flag("fx0") ? 0 : exp_flag("fx2") ? 2 : exp_flag("fx3") ? 3 : 4;
	if (!fx_rate_n)
		return;
	fx_keep30 = exp_flag("fxkeep30");
	var_fr = (int *)so_symbol(&yoyoloader_mod, "g_VAR_fr");
	var_in_place = (int *)so_symbol(&yoyoloader_mod, "g_VAR_in_place");
	var_in_screen = (int *)so_symbol(&yoyoloader_mod, "g_VAR_in_screen");
	fx_functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	fx_current_function = (runner_function **)so_symbol(&yoyoloader_mod, "g_pFunction");
	fx_fn_place_meeting = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_place_meeting");
	fx_object_find = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	var_animation_finished = (int *)so_symbol(&yoyoloader_mod, "g_VAR_animation_finished");
#define FX_RESOLVE(var, sym) var = (builtin_var_fn)so_symbol(&yoyoloader_mod, sym)
	FX_RESOLVE(gv_speed, "_Z8GV_SpeedP9CInstanceiP6RValue");
	FX_RESOLVE(sv_speed, "_Z8SV_SpeedP9CInstanceiP6RValue");
	FX_RESOLVE(gv_image_speed, "_Z13GV_ImageSpeedP9CInstanceiP6RValue");
	FX_RESOLVE(sv_image_speed, "_Z13SV_ImageSpeedP9CInstanceiP6RValue");
	FX_RESOLVE(gv_x, "_Z4GV_XP9CInstanceiP6RValue");
	FX_RESOLVE(gv_y, "_Z4GV_YP9CInstanceiP6RValue");
	FX_RESOLVE(sv_x, "_Z4SV_XP9CInstanceiP6RValue");
	FX_RESOLVE(sv_y, "_Z4SV_YP9CInstanceiP6RValue");
	FX_RESOLVE(sv_hspeed, "_Z9SV_HSpeedP9CInstanceiP6RValue");
	FX_RESOLVE(sv_vspeed, "_Z9SV_VSpeedP9CInstanceiP6RValue");
#undef FX_RESOLVE
	int smoke = 0, embers = 0;
	// The smoke needs the animation_code repeat (the 30 fps mode's entry hook on it, installed earlier)
	if (var_fr && var_in_place && var_in_screen && var_animation_finished && gv_speed && sv_speed && fx_functions &&
			fx_current_function && fx_fn_place_meeting && fx_object_find && current_room && t30_animation_code_hooked)
		smoke = !!(orig_smoke_step = gml_replace(table, "gml_Object_obj_smokecloud_Step_0", smoke_step));
	if (var_fr && gv_image_speed && sv_image_speed && gv_x && gv_y && sv_x && sv_y && sv_hspeed && sv_vspeed &&
			!exp_flag("fxembers0"))
		embers = !!(orig_corpsefire_step = gml_replace(table, "gml_Object_obj_corpsefire_Step_0", corpsefire_step));
	debugPrintf("effects at 1/%d rate (%s at 30 ticks/s): smoke %s, embers %s\n", fx_rate_n,
		fx_keep30 ? "same" : "half", smoke ? "hooked" : "NOT hooked", embers ? "hooked" : "NOT hooked");
}
