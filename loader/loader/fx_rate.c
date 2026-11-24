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

/*
 * smokePlayerStill (yyl.cfg, default 1; 0 = every player contact runs the Step on its tick): a skipped settled cloud
 * whose only contact is a player who isn't moving is treated as having none. The Step's player branch is
 *     if (place_meeting(x, y, obj_player)) { obj = instance_place(x, y, obj_player);
 *         if (obj.hsp != 0) { die = true; hspeed = obj.hsp / 10; }  if (obj.vsp != 0) { ...vspeed... }
 *         prev_speed = speed; }
 *     speed = prev_speed;   // after the contact branches, contact or not
 *     ... prev_speed = speed; speed *= fr;   // prev_speed is not read in between
 * With hsp and vsp exactly 0 (plain reals; the original's != 0 is epsilon-compared, so this is stricter) its only
 * writes are obj (the player's id) and prev_speed = speed, so it differs from no contact only in what speed is set to:
 * SV_Speed takes (float) of its value, the field's own value with contact, (float)prev_speed without. So it is used
 * only when (float)prev_speed equals speed, obj already holds the player's id, there is one obj_player
 * instance, and no enemy or slash touches the cloud. Such a cloud stays on fx_rate's 1-in-N schedule instead of
 * running its whole Step every tick (a player standing in a smoke vial's burst: ~30 clouds per tick).
 * exp_smokeplayerverify (debugMode): instead of skipping, the tick's Step runs twice from the same state, with the
 * contact and with place_meeting answering false, and the cloud's variables, fields 0x7c..0x15c, the random state and
 * the collision lists must come out the same ([FXPLAYER]). exp_smokeplayerverifyfault (negative control): the second
 * run starts from prev_speed + 0.25.
 */
int smoke_player_still = 1;
static int fx_ps_verify, fx_ps_fault, fx_ps_ab, fx_ps_off, *var_obj, *var_hsp, *var_vsp, *var_prev_speed;
static uint32_t fx_ps_tick = (uint32_t)-1;
static void *fx_ps_player;
static int fx_ps_player_id;
unsigned fx_ps_skip, fx_ps_moving, fx_ps_speed, fx_ps_obj, fx_ps_other, fx_ps_ok, fx_ps_bad, fx_ps_unchecked;

// An instance id from a real or a reference (low word); -5 if it is neither
static int fx_id_of(const gml_rvalue *v) {
	uint32_t kind = v->kind & 0xffffff;
	if (kind == 0)
		return (int)v->val;
	if (kind == 15) {
		int32_t lo;
		memcpy(&lo, &v->val, 4);
		return lo;
	}
	return -5;
}
// The one obj_player instance (instance_find; NULL if none, several, or an answer it doesn't know), once per tick
static void *fx_find_player(void *self) {
	if (fx_ps_tick == *frame_count)
		return fx_ps_player;
	fx_ps_tick = *frame_count;
	fx_ps_player = NULL;
	if (!fx_fn_instance_find || !fx_id_map)
		return NULL;
	runner_function *f = *fx_functions + fx_fn_instance_find[1], *saved = *fx_current_function;
	int ids[2];
	for (int i = 0; i < 2; i++) {
		gml_rvalue args[2] = {{0, 0, 15}, {(double)i, 0, 0}}, out = {0, 0, 5};
		uint32_t ref[2] = {(uint32_t)fx_contact_objects[1], 0x01000000};
		memcpy(&args[0].val, ref, 8);
		*fx_current_function = f;
		((builtin_fn)f->func)(&out, self, self, 2, args);
		*fx_current_function = saved;
		if ((ids[i] = fx_id_of(&out)) == -5)
			return NULL;
	}
	if (ids[0] < 0 || ids[1] >= 0)
		return NULL;
	fx_ps_player_id = ids[0];
	return fx_ps_player = fx_instance(ids[0]);
}
static int fx_player_still(void *self) {
	void *p = fx_find_player(self);
	if (!p) {
		fx_ps_other++;
		return 0;
	}
	gml_rvalue *h = var_hsp ? gml_vget(p, var_hsp[1]) : NULL, *v = var_vsp ? gml_vget(p, var_vsp[1]) : NULL;
	if ((!h || !v || h->kind != 0 || v->kind != 0 || h->val != 0 || v->val != 0)) {
		fx_ps_moving++;
		return 0;
	}
	gml_rvalue *ps = var_prev_speed ? gml_vget(self, var_prev_speed[1]) : NULL;
	float a = ps && ps->kind == 0 ? (float)ps->val : 0, b = (float)builtin_get(gv_speed, self);
	if (!ps || ps->kind != 0 || memcmp(&a, &b, 4)) {
		fx_ps_speed++;
		return 0;
	}
	gml_rvalue *o = var_obj ? gml_vget(self, var_obj[1]) : NULL;
	if (!o || fx_id_of(o) != fx_ps_player_id) {
		fx_ps_obj++;
		return 0;
	}
	fx_ps_skip++;
	return 1;
}

// Would this tick's Step take one of its contact branches? -1 if it can't tell (the Step runs then); 2: only the still
// player (smokePlayerStill verify)
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
			// smokePlayerStill: the player alone, standing still
			if (i == 1 && !filtered && smoke_player_still && !fx_ps_off && !fx_place_meeting(self, x, y, fx_contact_objects[2]) &&
					fx_player_still(self))
				return fx_ps_verify ? 2 : 0;
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

// exp_smokeplayerverify: this tick's Step with the still-player contact (A) and with place_meeting answering false (B),
// from the same state; the game goes on from B
#define PS_SLOTS 192
#define PS_FROM 0x7c
#define PS_TO 0x15c
typedef struct { gml_rvalue *slot; gml_rvalue value; } ps_slot;
static uint32_t *fx_rng_state, *fx_rng_index;
static int ps_take(void *self, ps_slot *out) {
	const uint8_t *ht = (const uint8_t *)(uintptr_t)fx_word(self, 0x24);
	if (!ht)
		return -1;
	uint32_t n = fx_word(ht, 8) + 1;
	const uint8_t *entries = (const uint8_t *)(uintptr_t)fx_word(ht, 16);
	int c = 0;
	for (uint32_t i = 0; i < n; i++) {
		gml_rvalue *slot = (gml_rvalue *)(uintptr_t)fx_word(entries + 12 * i, 0);
		if (!fx_word(entries + 12 * i, 8) || !slot)
			continue;
		if (c == PS_SLOTS)
			return -1;
		out[c].slot = slot;
		out[c++].value = *slot;
	}
	return c;
}
typedef struct { int n; ps_slot vars[PS_SLOTS]; uint8_t fields[PS_TO - PS_FROM]; uint32_t rng[17], lists[2], links[6]; } ps_state;
static int ps_snap(void *self, ps_state *s) {
	if ((s->n = ps_take(self, s->vars)) < 0 || !fx_rng_state || !fx_rng_index)
		return 0;
	memcpy(s->fields, (uint8_t *)self + PS_FROM, sizeof(s->fields));
	memcpy(s->rng, fx_rng_state, 64);
	s->rng[16] = *fx_rng_index;
	s->lists[0] = fx_dirty_list[0];
	s->lists[1] = fx_dirty_list[1];
	memcpy(s->links, (uint8_t *)self + 0x15c, sizeof(s->links));
	return 1;
}
static int ps_same(const ps_state *a, const ps_state *b) {
	if (a->n != b->n || memcmp(a->fields, b->fields, sizeof(a->fields)) || memcmp(a->rng, b->rng, sizeof(a->rng)) ||
			memcmp(a->lists, b->lists, sizeof(a->lists)) || memcmp(a->links, b->links, sizeof(a->links)))
		return 0;
	for (int i = 0; i < a->n; i++)
		if (a->vars[i].slot != b->vars[i].slot || memcmp(&a->vars[i].value, &b->vars[i].value, sizeof(gml_rvalue)))
			return 0;
	return 1;
}
static void fx_player_verify(void *self, void *other) {
	static ps_state before, after_a, after_b;
	if (!ps_snap(self, &before)) {
		fx_ps_unchecked++;
		orig_smoke_step(self, other);
		return;
	}
	orig_smoke_step(self, other);
	// A run that destroyed the cloud, moved the collision lists or made a value that owns something can't be repeated
	if ((fx_word(self, 0x7c) & 3) || !ps_snap(self, &after_a) || memcmp(after_a.lists, before.lists, 8) ||
			memcmp(after_a.links, before.links, sizeof(before.links))) {
		fx_ps_unchecked++;
		return;
	}
	// Values that own something (close_light_array) are not put back: the A run must have left them as they were
	for (int i = 0; i < before.n; i++) {
		uint32_t kind = before.vars[i].value.kind & 0xffffff;
		if (kind == 1 || kind == 2 || kind == 6) {
			if (memcmp(before.vars[i].slot, &before.vars[i].value, sizeof(gml_rvalue))) {
				fx_ps_unchecked++;
				return;
			}
			continue;
		}
	}
	for (int i = 0; i < before.n; i++) {
		uint32_t kind = before.vars[i].value.kind & 0xffffff, now = before.vars[i].slot->kind & 0xffffff;
		if (kind == 1 || kind == 2 || kind == 6 || now == 1 || now == 2 || now == 6)
			continue;
		*before.vars[i].slot = before.vars[i].value;
	}
	memcpy((uint8_t *)self + PS_FROM, before.fields, sizeof(before.fields));
	memcpy(fx_rng_state, before.rng, 64);
	*fx_rng_index = before.rng[16];
	if (fx_ps_fault) { // negative control: run B starts from a prev_speed off by a quarter
		gml_rvalue *ps = gml_vget(self, var_prev_speed[1]);
		if (ps && ps->kind == 0)
			ps->val += 0.25;
	}
	runner_function *pm = *fx_functions + fx_fn_place_meeting[1];
	fx_place_meeting_real = pm->func;
	pm->func = (void *)fx_place_meeting_false;
	orig_smoke_step(self, other);
	pm->func = (void *)fx_place_meeting_real;
	if (ps_snap(self, &after_b) && ps_same(&after_a, &after_b)) {
		fx_ps_ok++;
		return;
	}
	if (fx_ps_bad++ < 8)
		debugPrintf("[FXPLAYER] mismatch: cloud %u (vars %d/%d)\n", fx_word(self, 0x80), after_a.n, after_b.n);
}

/*
 * smokeRecord (yyl.cfg, default 0; 1 = the original): with replays off (replays=0, the default) a cloud's Begin Step
 * doesn't record it for the replay. The Begin Step appends the cloud to obj_recordsmoke's eight arrays the first time
 * (and writes its settle and death frames later):
 *     if (!recorded) { with (obj_recordsmoke) { smoke_startx[smoke_counter] = other.x; ...; other.record_id =
 *         smoke_counter; smoke_counter += 1; } recorded = true; }
 *     if (in_place && !recorded_set) { with (obj_recordsmoke) smoke_setframe[other.record_id] = obj_record.frame;
 *         recorded_set = true; }
 *     if (die && !recorded_death) { with (obj_recordsmoke) smoke_deathframe[other.record_id] = ...; recorded_death = true; }
 * Each append grows the arrays by one, so a vial's 131 new clouds cost 18-28 ms in one frame on the Vita (more the more
 * clouds the room has had). Only the replay and the death rewind read those arrays (obj_recordsmoke's Draw while
 * playing back), and with replays off neither runs. Here the three flags are set as the event sets them (true: 1.0,
 * kind 13), under the same conditions (BOOL of plain reals and bools; anything else runs the event), without the
 * recording; record_id is not written (only those with-blocks read it).
 * exp_smokerecordverify (debugMode): the original runs first, then from the cloud's earlier variables this, and the
 * cloud's variables must match except record_id and the flags word of the three flags (the original copies a stack
 * temporary's). exp_smokerecordverifyfault: recorded is not set (a negative control).
 */
int smoke_record = 0;
static event_func orig_smoke_begin;
static int *var_recorded, *var_recorded_set, *var_recorded_death, *var_die, *var_record_id;
static int fx_rec_verify, fx_rec_fault, fx_rec_ab; // exp_smokerecordab: smokeRecord flips every report window
unsigned fx_rec_skipped, fx_rec_run, fx_rec_ok, fx_rec_bad, fx_rec_unchecked;
// BOOL of a variable: 0/1, -1 if missing or not a plain real/bool
static int fx_truth(void *self, int *var) {
	gml_rvalue *v = var ? gml_vget(self, var[1]) : NULL;
	if (!v)
		return -1;
	uint32_t kind = v->kind & 0xffffff;
	if (kind != 0 && kind != 13)
		return -1;
	return v->val > 0.5;
}
static void fx_set_true(void *self, int *var) {
	gml_rvalue *v = gml_vget(self, var[1]);
	v->val = 1.0;
	v->flags = 0;
	v->kind = 13;
}
// The Begin Step without the recording; 0 (nothing done) if it can't tell
static int fx_begin_norecord(void *self) {
	int r = fx_truth(self, var_recorded), p = fx_truth(self, var_in_place), d = fx_truth(self, var_die);
	int rs = fx_truth(self, var_recorded_set), rd = fx_truth(self, var_recorded_death);
	if (r < 0 || p < 0 || d < 0 || rs < 0 || rd < 0)
		return 0;
	if (!r && !fx_rec_fault)
		fx_set_true(self, var_recorded);
	if (p && !rs)
		fx_set_true(self, var_recorded_set);
	if (d && !rd)
		fx_set_true(self, var_recorded_death);
	return 1;
}
static int fx_rec_loose(const gml_rvalue *slot, void *self) {
	return slot == gml_vget(self, var_recorded[1]) || slot == gml_vget(self, var_recorded_set[1]) ||
		slot == gml_vget(self, var_recorded_death[1]);
}
static void fx_rec_verify_event(void *self, void *other) {
	static ps_slot before[PS_SLOTS], after[PS_SLOTS];
	int nb = ps_take(self, before);
	if (nb < 0) {
		fx_rec_unchecked++;
		orig_smoke_begin(self, other);
		return;
	}
	orig_smoke_begin(self, other);
	int na = ps_take(self, after);
	if (na < 0) {
		fx_rec_unchecked++;
		return;
	}
	// Back to the earlier values (plain ones; record_id, if the original made it, stays: it is skipped below)
	for (int i = 0; i < nb; i++) {
		uint32_t k = before[i].value.kind & 0xffffff, now = before[i].slot->kind & 0xffffff;
		if (k == 1 || k == 2 || k == 6 || now == 1 || now == 2 || now == 6)
			continue;
		*before[i].slot = before[i].value;
	}
	if (!fx_begin_norecord(self)) {
		fx_rec_unchecked++;
		orig_smoke_begin(self, other);
		return;
	}
	gml_rvalue *record_id = var_record_id ? gml_vget(self, var_record_id[1]) : NULL;
	int bad = 0;
	for (int i = 0; i < na && !bad; i++) {
		const gml_rvalue *now = after[i].slot;
		if (now == record_id)
			continue;
		const gml_rvalue *a = &after[i].value;
		if (a->kind != now->kind || memcmp(&a->val, &now->val, 8) || (a->flags != now->flags && !fx_rec_loose(now, self)))
			bad = 1;
	}
	if (!bad)
		fx_rec_ok++;
	else if (fx_rec_bad++ < 8)
		debugPrintf("[FXRECORD] mismatch: cloud %u\n", fx_word(self, 0x80));
}
static void smoke_begin(void *self, void *other) {
	extern int replays;
	if (smoke_record || replays) {
		fx_rec_run++;
		orig_smoke_begin(self, other);
		return;
	}
	if (fx_rec_verify) {
		fx_rec_verify_event(self, other);
		return;
	}
	if (fx_begin_norecord(self)) {
		fx_rec_skipped++;
		return;
	}
	fx_rec_run++;
	orig_smoke_begin(self, other);
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
			if (contact == 2)
				fx_player_verify(self, other);
			else
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
	if (fx_rec_skipped || fx_rec_run || fx_rec_verify)
		debugPrintf("[FXRECORD] smokeRecord %d: Begin Steps without recording %.1f, original %.1f per tick%s\n",
			smoke_record, (float)fx_rec_skipped / frames, (float)fx_rec_run / frames,
			fx_rec_ab ? (smoke_record ? " | A/B: off" : " | A/B: on") : "");
	if (fx_rec_verify)
		debugPrintf("[FXRECORD] verify: %u same, %u mismatches, %u unchecked\n", fx_rec_ok, fx_rec_bad, fx_rec_unchecked);
	fx_rec_skipped = fx_rec_run = 0;
	if (fx_rec_ab)
		smoke_record = !smoke_record;
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
	if (fx_ps_skip || fx_ps_moving || fx_ps_speed || fx_ps_obj || fx_ps_other || fx_ps_ab)
		debugPrintf("[FXPLAYER] smokePlayerStill %d: still-player skips %.1f per tick; not: moving %u, speed %u, obj %u, "
			"other %u%s\n", smoke_player_still, (float)fx_ps_skip / frames, fx_ps_moving, fx_ps_speed, fx_ps_obj, fx_ps_other,
			fx_ps_ab ? (fx_ps_off ? " | A/B: off" : " | A/B: on") : "");
	if (fx_ps_verify)
		debugPrintf("[FXPLAYER] verify: %u same, %u mismatches, %u unchecked\n", fx_ps_ok, fx_ps_bad, fx_ps_unchecked);
	fx_ps_skip = fx_ps_moving = fx_ps_speed = fx_ps_obj = fx_ps_other = 0;
	if (fx_ps_ab)
		fx_ps_off = !fx_ps_off;
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
	var_obj = (int *)so_symbol(&yoyoloader_mod, "g_VAR_obj");
	var_hsp = (int *)so_symbol(&yoyoloader_mod, "g_VAR_hsp");
	var_vsp = (int *)so_symbol(&yoyoloader_mod, "g_VAR_vsp");
	var_prev_speed = (int *)so_symbol(&yoyoloader_mod, "g_VAR_prev_speed");
	fx_rng_state = (uint32_t *)so_symbol(&yoyoloader_mod, "state");
	fx_rng_index = (uint32_t *)so_symbol(&yoyoloader_mod, "g_RndIndex");
	if (!var_obj || !var_hsp || !var_vsp || !var_prev_speed || !fx_fn_instance_find || !fx_id_map)
		smoke_player_still = 0;
	fx_ps_verify = debugMode && smoke_player_still && exp_flag("smokeplayerverify");
	fx_ps_fault = fx_ps_verify && exp_flag("smokeplayerverifyfault");
	fx_ps_ab = debugMode && exp_flag("smokeplayerab");
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
	var_recorded = (int *)so_symbol(&yoyoloader_mod, "g_VAR_recorded");
	var_recorded_set = (int *)so_symbol(&yoyoloader_mod, "g_VAR_recorded_set");
	var_recorded_death = (int *)so_symbol(&yoyoloader_mod, "g_VAR_recorded_death");
	var_die = (int *)so_symbol(&yoyoloader_mod, "g_VAR_die");
	var_record_id = (int *)so_symbol(&yoyoloader_mod, "g_VAR_record_id");
	fx_rec_verify = debugMode && exp_flag("smokerecordverify");
	fx_rec_fault = fx_rec_verify && exp_flag("smokerecordverifyfault");
	fx_rec_ab = debugMode && exp_flag("smokerecordab");
	if (var_recorded && var_recorded_set && var_recorded_death && var_die && var_in_place)
		orig_smoke_begin = gml_replace(table, "gml_Object_obj_smokecloud_Step_1", smoke_begin);
	debugPrintf("smoke Begin Step recording: %s\n", !orig_smoke_begin ? "NOT hooked" :
		smoke_record ? "kept (smokeRecord=1)" : "skipped while replays are off");
	debugPrintf("effects at 1/%d rate: smoke %s, embers %s\n", fx_rate_n,
		smoke ? "hooked" : "NOT hooked", embers ? "hooked" : "NOT hooked");
}
