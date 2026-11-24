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
 *   (fx_animation_code, an entry hook on it). Clouds that are still flying to their place
 *   (in_place false: a new cloud and its first Steps) run every tick, so they appear and spread smoothly.
 * - Contacts stay at full rate. A settled cloud on screen (in_screen, set by its Draw) tests the original Step's three
 *   gates every tick: place_meeting(x, y, obj_enemy) (the touched enemy gets first_attack_timer >= 20 and
 *   whip_timer = 30, and a moving one dissolves and pushes the cloud), obj_player (the same push) and obj_slash
 *   (alpha -= 0.02 per tick of overlap, no fr). A skipped cloud with any of them true catches up the skipped ticks
 *   (one Step covering them, its contact tests answering false, as they were tested) and then runs this tick's Step
 *   at the real fr (smoke_last keeps each cloud's last run), so each of those writes happens on the tick and at the
 *   place it would have, the push's speed decays per tick, dissolve starts on its first frame, and the fade counts
 *   every overlapping tick once. A contact that starts on an update tick is handled by that update's Step (fr x N):
 *   the enemy writes and the fade are exact, the cloud's own push decay and first dissolve frames are batched as in
 *   any update.
 * - obj_corpsefire (burning-corpse embers) Step sets image_speed from fr, and the runner animates every tick (its
 *   Animation End destroys the ember): image_speed is divided by N. Its own motion (x += hsp * fr) would move in
 *   jumps every N ticks; the jump is spread over the N ticks with built-in hspeed/vspeed, which the runner applies
 *   after the Step (the ember doesn't use built-in motion otherwise).
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
static int *var_fr, *var_in_place, *var_animation_finished;
static builtin_var_fn gv_speed, sv_speed, gv_image_speed, sv_image_speed, gv_x, gv_y, sv_x, sv_y, sv_hspeed, sv_vspeed;
static event_func orig_smoke_step, orig_corpsefire_step;

// animation_code repeat request (fx_animation_code): times, and the real fr to run it with
static int fx_anim_repeat;
static double fx_anim_fr;
static gml_script_fn orig_animation_code;

unsigned fx_smoke_full, fx_smoke_update, fx_smoke_skip, fx_smoke_contacts, fx_anim_calls, fx_ember_update, fx_ember_skip;

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

// Entry hook on animation_code: while fx_anim_repeat > 1 the script runs fx_anim_repeat times with the real fr, and
// animation_finished is true if any run finished
static void *fx_animation_code(void *self, void *other, void *result, int argc, void **args) {
	gml_script_fn orig = orig_animation_code;
	if (fx_anim_repeat <= 1)
		return orig(self, other, result, argc, args);
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

/*
 * smokeContactFilter (yyl.cfg, default 1; 0 = the three place_meeting calls for every skipped cloud): a skipped
 * settled cloud tests its gates only if its box can touch the box of an enemy, the player or a slash. place_meeting
 * at the cloud's own position needs the boxes to overlap, so a cloud whose box (+0xec..+0xf8) overlaps none of them,
 * grown by a pixel, has no contact. It is used only when every box involved is current: the collision dirty list is
 * empty (the first tree query of a tick drains it: until then, and after anything moves, the gates run as before),
 * and neither the cloud nor any target has the box flag (+0x7c bit 3) set. The targets are every instance of the three
 * objects (instance_find, children included), collected once per tick and again when an instance is created
 * (room_maxid). Not covered: an instance activated, or changed into an enemy (instance_change), during a tick after
 * the collection, until the next tick. ~131 clouds of a smoke vial made ~300 place_meeting calls per tick (2.3 ms).
 * exp_smokecontactverify (debugMode): every filtered cloud runs the gates anyway; a contact counts as a miss.
 * exp_smokecontactverifyfault (negative control): no targets, so every contact of a skipped cloud is a miss.
 * exp_smokecontactab: the filter off in every other report window ([FXCONTACT]).
 */
int smoke_contact_filter = 1;
static const int *fx_fn_instance_find;
static uint32_t *fx_id_map, *fx_dirty_list;
static const int *fx_room_maxid;
static int fx_cf_verify, fx_cf_fault, fx_cf_ab, fx_cf_off;
unsigned fx_cf_filtered, fx_cf_tested, fx_cf_unsure, fx_cf_misses, fx_cf_checked;
#define FX_TARGETS 48
static struct { uint32_t tick; int maxid, n, valid; void *inst[FX_TARGETS]; } fx_targets = {.tick = (uint32_t)-1};

static void *fx_instance(int id) {
	if (!fx_id_map[0])
		return NULL;
	const uint32_t *buckets = (const uint32_t *)(uintptr_t)fx_id_map[0]; // {first, last} each, element {prev, next, id, inst}
	const uint32_t *e = (const uint32_t *)(uintptr_t)buckets[(fx_id_map[1] & (uint32_t)id) * 2];
	for (; e; e = (const uint32_t *)(uintptr_t)e[1])
		if ((int)e[2] == id)
			return (void *)(uintptr_t)e[3];
	return NULL;
}
static void fx_collect_targets(void *self) {
	fx_targets.tick = *frame_count;
	fx_targets.maxid = *fx_room_maxid;
	fx_targets.n = 0;
	fx_targets.valid = 0;
	runner_function *f = *fx_functions + fx_fn_instance_find[1], *saved = *fx_current_function;
	for (int k = 0; k < 3; k++) {
		if (fx_cf_fault)
			continue; // exp_smokecontactverifyfault: no targets
		for (int i = 0;; i++) {
			gml_rvalue args[2] = {{0, 0, 15}, {(double)i, 0, 0}}, out = {0, 0, 5};
			uint32_t ref[2] = {(uint32_t)fx_contact_objects[k], 0x01000000};
			memcpy(&args[0].val, ref, 8);
			*fx_current_function = f;
			((builtin_fn)f->func)(&out, self, self, 2, args);
			*fx_current_function = saved;
			uint32_t kind = out.kind & 0xffffff;
			int id;
			if (kind == 0)
				id = (int)out.val;
			else if (kind == 15) {
				int32_t lo;
				memcpy(&lo, &out.val, 4);
				id = lo;
			} else
				return; // an answer it doesn't know: no filter this tick
			if (id < 0)
				break; // noone: the end of this object's instances
			void *inst = fx_instance(id);
			if (!inst || fx_targets.n == FX_TARGETS)
				return;
			fx_targets.inst[fx_targets.n++] = inst;
		}
	}
	fx_targets.valid = 1;
}
static inline uint32_t fx_word(const void *p, size_t o) {
	uint32_t v;
	memcpy(&v, (const uint8_t *)p + o, 4);
	return v;
}
static inline void fx_box(const void *inst, float b[4]) {
	float x0, y0, x1, y1;
	memcpy(&x0, (const char *)inst + 0xec, 4);
	memcpy(&y0, (const char *)inst + 0xf0, 4);
	memcpy(&x1, (const char *)inst + 0xf4, 4);
	memcpy(&y1, (const char *)inst + 0xf8, 4);
	b[0] = x0 < x1 ? x0 : x1;
	b[1] = y0 < y1 ? y0 : y1;
	b[2] = x0 > x1 ? x0 : x1;
	b[3] = y0 > y1 ? y0 : y1;
}
// 1: no target's box comes near the cloud's, so no gate can be true; 0: the gates must run
static int fx_no_contact_possible(void *self) {
	if (!smoke_contact_filter || fx_cf_off || !fx_fn_instance_find || !fx_id_map || !fx_dirty_list || !fx_room_maxid)
		return 0;
	if (fx_dirty_list[0] != (uint32_t)(uintptr_t)fx_dirty_list || (fx_word(self, 0x7c) & 8)) {
		fx_cf_unsure++;
		return 0;
	}
	if (fx_targets.tick != *frame_count || fx_targets.maxid != *fx_room_maxid)
		fx_collect_targets(self);
	if (!fx_targets.valid) {
		fx_cf_unsure++;
		return 0;
	}
	float c[4];
	fx_box(self, c);
	if (!(c[0] <= c[2] && c[1] <= c[3])) { // NaN
		fx_cf_unsure++;
		return 0;
	}
	for (int i = 0; i < fx_targets.n; i++) {
		const void *t = fx_targets.inst[i];
		if (fx_word(t, 0x7c) & 8) {
			fx_cf_unsure++;
			return 0;
		}
		float b[4];
		fx_box(t, b);
		if (!(b[0] - 1 > c[2] || c[0] > b[2] + 1 || b[1] - 1 > c[3] || c[1] > b[3] + 1))
			return 0; // overlap (or NaN): the gates decide
	}
	return 1;
}

// Would this tick's Step take one of its contact branches? -1 if it can't tell (the Step runs then)
static int fx_smoke_contact(void *self) {
	gml_rvalue *v = fx_var(self, var_in_screen);
	if (!v)
		return -1;
	if (v->val == 0)
		return 0; // the contacts are inside if (in_screen)
	static const char *const names[3] = {"obj_enemy", "obj_player", "obj_slash"};
	for (int i = 0; i < 3; i++) {
		if (fx_contact_objects[i] == -2)
			fx_contact_objects[i] = fx_object_find(names[i]);
		if (fx_contact_objects[i] < 0)
			return -1;
	}
	int filtered = fx_no_contact_possible(self);
	if (filtered) {
		fx_cf_filtered++;
		if (!fx_cf_verify)
			return 0;
	} else {
		fx_cf_tested++;
	}
	float x, y;
	memcpy(&x, (char *)self + 0xb8, 4);
	memcpy(&y, (char *)self + 0xbc, 4);
	for (int i = 0; i < 3; i++)
		if (fx_place_meeting(self, x, y, fx_contact_objects[i])) {
			if (filtered) {
				if (fx_cf_misses++ < 8)
					debugPrintf("[FXCONTACT] miss: cloud %u touches object %d\n", fx_word(self, 0x80),
						fx_contact_objects[i]);
				// the original's answer stands (verify mode only)
			}
			return 1;
		}
	if (filtered)
		fx_cf_checked++;
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

// While a catch-up Step runs for ticks whose contacts were all tested false, the Step's own place_meeting calls
// (they are only its three contact gates) answer false too: the registry entry is swapped around the call
static void (*fx_place_meeting_real)(gml_rvalue *, void *, void *, int, gml_rvalue *);
static void fx_place_meeting_false(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	result->val = 0;
	result->flags = 0;
	result->kind = 13; // false, as place_meeting returns
}

// The Step covering k ticks: fr x k, animation_code k times at the real fr, speed back to a per-tick drift
static void smoke_run(void *self, void *other, int k, int no_contacts) {
	runner_function *pm = no_contacts ? *fx_functions + fx_fn_place_meeting[1] : NULL;
	if (pm) {
		fx_place_meeting_real = pm->func;
		pm->func = (void *)fx_place_meeting_false;
	}
	double fr = k > 1 ? fx_scale_fr(k) : -1;
	if (fr >= 0) {
		fx_anim_fr = fr;
		fx_anim_repeat = k;
	}
	orig_smoke_step(self, other);
	if (pm)
		pm->func = (void *)fx_place_meeting_real;
	if (fr < 0)
		return;
	fx_anim_repeat = 0;
	fx_restore_fr(fr, k);
	double speed = builtin_get(gv_speed, self);
	if (speed != 0)
		builtin_set(sv_speed, self, speed / k);
}

static void smoke_step(void *self, void *other) {
	int n = fx_rate_n;
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
		if (contact > 0) {
			// The skipped ticks had no contact: they are caught up first with the Step's contact tests answering
			// false, then this tick's Step runs at the real fr, so the push, its speed decay and the dissolve's first
			// frame happen as on this tick at full rate
			fx_smoke_contacts++;
			*last = tick;
			if (k > 1) {
				fx_smoke_update++;
				smoke_run(self, other, (int)k - 1, 1);
				if (*(uint32_t *)((char *)self + 0x7c) & 3)
					return; // destroyed (or deactivated) by the catch-up: the original would not Step it again
			}
			fx_smoke_full++;
			orig_smoke_step(self, other);
			return;
		}
	}
	*last = tick;
	if (k == 1)
		fx_smoke_full++;
	else
		fx_smoke_update++;
	smoke_run(self, other, (int)k, 0);
}

static void corpsefire_step(void *self, void *other) {
	int n = fx_rate_n;
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
		debugPrintf("[FXRATE] N %d: smoke steps full %.1f updated %.1f skipped %.1f per tick, contact runs %u, "
			"animation_code runs %.1f per tick\n", fx_rate_n, (float)fx_smoke_full / frames,
			(float)fx_smoke_update / frames, (float)fx_smoke_skip / frames, fx_smoke_contacts, (float)fx_anim_calls / frames);
	if (fx_ember_update || fx_ember_skip)
		debugPrintf("[FXRATE] embers updated %.1f skipped %.1f per tick\n", (float)fx_ember_update / frames,
			(float)fx_ember_skip / frames);
	fx_smoke_full = fx_smoke_update = fx_smoke_skip = fx_smoke_contacts = fx_anim_calls = fx_ember_update = fx_ember_skip = 0;
	if (fx_cf_filtered || fx_cf_tested || fx_cf_ab)
		debugPrintf("[FXCONTACT] smokeContactFilter %d: filtered %.1f gates run %.1f (unsure %.1f) per tick, targets %d%s\n",
			smoke_contact_filter, (float)fx_cf_filtered / frames, (float)fx_cf_tested / frames,
			(float)fx_cf_unsure / frames, fx_targets.n, fx_cf_ab ? (fx_cf_off ? " | A/B: off" : " | A/B: on") : "");
	if (fx_cf_verify)
		debugPrintf("[FXCONTACT] verify: %u filtered clouds checked, %u misses\n", fx_cf_checked, fx_cf_misses);
	fx_cf_filtered = fx_cf_tested = fx_cf_unsure = 0;
	if (fx_cf_ab)
		fx_cf_off = !fx_cf_off;
}

void fx_rate_install(gml_func_entry *table) {
	fx_rate_n = exp_flag("fx0") ? 0 : exp_flag("fx2") ? 2 : exp_flag("fx3") ? 3 : 4;
	if (!fx_rate_n)
		return;
	var_fr = (int *)so_symbol(&yoyoloader_mod, "g_VAR_fr");
	var_in_place = (int *)so_symbol(&yoyoloader_mod, "g_VAR_in_place");
	var_in_screen = (int *)so_symbol(&yoyoloader_mod, "g_VAR_in_screen");
	fx_functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	fx_current_function = (runner_function **)so_symbol(&yoyoloader_mod, "g_pFunction");
	fx_fn_place_meeting = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_place_meeting");
	fx_object_find = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	var_animation_finished = (int *)so_symbol(&yoyoloader_mod, "g_VAR_animation_finished");
	fx_fn_instance_find = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_instance_find");
	fx_id_map = (uint32_t *)so_symbol(&yoyoloader_mod, "_ZN9CInstance14ms_ID2InstanceE");
	fx_dirty_list = (uint32_t *)so_symbol(&yoyoloader_mod, "s_dirty_list");
	fx_room_maxid = (const int *)so_symbol(&yoyoloader_mod, "room_maxid");
	fx_cf_verify = debugMode && exp_flag("smokecontactverify");
	fx_cf_ab = debugMode && exp_flag("smokecontactab");
	fx_cf_fault = fx_cf_verify && exp_flag("smokecontactverifyfault");
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
	// The smoke needs the animation_code repeat
	if (var_fr && var_in_place && var_in_screen && var_animation_finished && gv_speed && sv_speed && fx_functions &&
			fx_current_function && fx_fn_place_meeting && fx_object_find && current_room)
		orig_animation_code = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z25gml_Script_animation_codeP9CInstanceS0_R8YYRValueiPPS1_"), fx_animation_code);
	if (orig_animation_code)
		smoke = !!(orig_smoke_step = gml_replace(table, "gml_Object_obj_smokecloud_Step_0", smoke_step));
	if (var_fr && gv_image_speed && sv_image_speed && gv_x && gv_y && sv_x && sv_y && sv_hspeed && sv_vspeed &&
			!exp_flag("fxembers0"))
		embers = !!(orig_corpsefire_step = gml_replace(table, "gml_Object_obj_corpsefire_Step_0", corpsefire_step));
	debugPrintf("effects at 1/%d rate: smoke %s, embers %s\n", fx_rate_n,
		smoke ? "hooked" : "NOT hooked", embers ? "hooked" : "NOT hooked");
}
