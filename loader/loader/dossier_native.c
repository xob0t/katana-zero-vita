/*
 * Dossier burn (room_folder). APK 1.10 libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * GML lifts: work/decomp/out/gml_Object_obj_burncircle*.gml, gml_Object_obj_folder_cutscene_*.gml; the native code
 * follows the ARM of the compiled events (gml_*.asm next to them, all untracked), not the lifts.
 *
 * The burn spreads through obj_burncircle_spawner cells; each new cell makes a burn circle, whose first Step makes three
 * flames and six decorative child circles, and the cutscene's Draw stamps every live circle into its burn mask each
 * frame. Original: ~1300 instances at the peak, ~55 ms per tick (500 MHz), 108029 circle Steps per burn.
 *
 * 1. Native Steps (exact).
 *    obj_burncircle, every branch the burn uses (circle_native):
 *        if (!spawn) {                                              // the first Step of a main circle
 *            if (!made_fire) { repeat (3) { r = random(1); fire_dir_rand = fire_dir - 45 + r * 45 * 2;
 *                fire_dist = 20 + r * 10; bb = instance_create_depth(x + lengthdir_x(fire_dist, fire_dir_rand),
 *                y + lengthdir_y(...), 0, obj_folder_fireparticle); bb.direction = 80 + r * 10 * 2;
 *                bb.speed = 4 + r * 3; bb.angle = bb.direction; } made_fire = true; }
 *            timer += 1;
 *            if (timer > tts || size + grow_spd * 2 >= 15) { repeat (6) { r = random(1); angle = r * 360;
 *                dist = r * 3; bb = instance_create_depth(x + lengthdir_x(dist, angle), ..., -1000, obj_burncircle);
 *                bb.max_size = r * 25 + 5; bb.spawn = true; } spawn = true; }
 *        }
 *        if (grow_spd > 0.5) grow_spd -= 0.1; size += grow_spd * obj_folder_cutscene.fire_spd;
 *        if (size > max_size) instance_destroy();
 *    The same runner calls (random, lengthdir_x/y, YYGML_CallLegacyFunction(instance_create_depth), the variable
 *    getters and setters, YYCompareVal with g_GMLMathEpsilon, instance_destroy) in the same order, the same double
 *    arithmetic (operator order, float lengthdir arguments), bools written as kind 13. Everything is checked before the
 *    first side effect; anything unexpected runs the original event.
 *    obj_burncircle_spawner: a burned-out barrier does nothing; a spreading cell does
 *        timer += 1 * obj_folder_cutscene.fire_spd and only spreads (the original) once timer > timer_max.
 *    obj_folder_cutscene.fire_spd is read from the cutscene instance directly (see fire_spd).
 * 2. Native burn-mask stamps (exact). The cutscene's Draw runs
 *        with (obj_burncircle) draw_bigcircle(x / other.burn_scale, y / other.burn_scale, size * 2 / other.burn_scale,
 *            c_white);   // draw_sprite_ext(spr_bigcircle, 0, x, y, r / 400 * 2, r / 400 * 2, 0, c, draw_get_alpha())
 *    in GML at ~14 us per circle. Its YYGML_NewWithIterator call goes through a veneer to draw_new_with, which walks the
 *    same iterator list and calls YYGML_draw_sprite_ext with the same values, then reports an empty with, so the
 *    original jumps to its DeleteWithIterator. The compiled body also reads self.burn_surface twice (a dead read that
 *    creates the variable on each circle); the native body makes the first read too.
 * 3. Burned-out cells skip their cleanup search. Every 31 Steps (and 5 Steps in a row once it succeeds) a burned-out
 *    cell enlarges its mask to 100x100 and runs 8 place_meeting + instance_place queries to delete itself when
 *    surrounded by burned-out cells: ~10 ms per tick at the peak. Cells are invisible, burned-out cells never spread
 *    again, and the cutscene ends when every cell is burned out. With the search skipped the spread is the same: the
 *    same 1107 burn circles and 108029 circle Steps in all (burn_skip against burn_nat3). exp_dossiercleanup keeps it.
 * 4. Fewer decorative circles (dossierCircles in yyl.cfg: keep one in N, 0 = none, the default; 1 = all). The six
 *    children sit within 3 px of their parent, never spread or make flames, and only add their stamp to the burn mask,
 *    where the parent's own (radius 30, the largest) covers all but a 3 px rim. The native first Step skips a dropped
 *    child's instance_create_depth but draws its Create's random(1.5) (grow_spd = random(1.5) + 0.5), so the random
 *    sequence, the spread and the cutscene's length are unchanged; its bb.* writes go nowhere. Later instance ids are
 *    lower. Children are only dropped by the native first Step: when it falls back, all six are created. (An earlier
 *    version dropped them in a function-registry wrapper and let the original write bb.max_size into the parent;
 *    then veneers on those writes, which the device never installed: the call sites are ~21 MB from the code cave.)
 *
 * 5. The flames' obj_folder_cutscene.fire_spd reads come from the recorded cutscene (flame_get_value).
 * exp_dossiernative0 turns all of it off; exp_dossierexact0 keeps the circle tail, the spawner and 3 but turns off
 * the native circle first Step (so no children are dropped), 2 and 5.
 * Offline check: dossier_verify.py (untracked) runs the original ARM and this code in Unicorn on the same fixtures.
 * exp_dossierverify (debug): every circle Step runs the original with its instance_create_depth calls recorded, then
 * the native code dry (on a copy of the variables, the creations replayed from the record, the random state checked at
 * each), and compares the random state, the created instances' arguments and written variables, self's variables and
 * the destroy; every mask stamp loop runs the native body dry and compares its draws with the original's
 * YYGML_draw_sprite_ext calls. All children are kept in this mode. [DOSSIER] verify lines; mismatches must stay 0.
 * Debug report ([DOSSIER] burn): ticks and tick intervals from the first spawner Step to the last circle Step, also
 * unprofiled; with profiling, per-tick time in each part.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifdef DOSSIER_TEST
#include "dossier_stub.h" // only for the offline ARM differential test, whose harness is not in the repository
#else
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include "main.h"
#include "so_util.h"
#endif

typedef struct {
	double val;
	uint32_t flags;
	uint32_t kind;
} rv;

#define KIND_REAL 0
#define KIND_UNDEFINED 5
#define KIND_OBJECT 6
#define KIND_BOOL 13
#define KIND_REF 15
#define REFCOUNTED(k) ((1u << ((k) & 0x1f)) & 0x46) // string, array, object: the compiled code's FREE_RValue test
#define NO_INDEX ((int)0x80000000)
#define OBJ_FOLDER_CUTSCENE 458 // object indices in Katana ZERO 1.1.10 (0x1ca in the compiled Step)
#define OBJ_FOLDER_FIREPARTICLE 459
#define OBJ_BURNCIRCLE 92
#define REF_OBJECT 0x01000000 // instance_create_depth's object argument: {index, REF_OBJECT} kind 15
#define REF_SPRITE 0x01000001 // spr_bigcircle in draw_bigcircle: {0x90, REF_SPRITE} kind 15

typedef void (*event_fn)(void *self, void *other);
typedef void (*builtin_fn)(rv *result, void *self, void *other, int argc, rv *args);

enum { V_SPAWN, V_GROW_SPD, V_SIZE, V_MAX_SIZE, V_FIRE_SPD, V_MAKE_FIRE, V_BURNT_OUT, V_BARRIER, V_TIMER, V_KILL_TIMER,
	V_TIMER_MAX, V_MADE_FIRE, V_FIRE_DIR, V_FIRE_DIR_RAND, V_FIRE_DIST, V_BB, V_TTS, V_ANGLE, V_DIST, V_DIRECTION, V_SPEED,
	V_X, V_Y, V_BURN_SURFACE, V_BURN_SCALE, NVARS };
static const char *const var_names[NVARS] = {"g_VAR_spawn", "g_VAR_grow_spd", "g_VAR_size", "g_VAR_max_size",
	"g_VAR_fire_spd", "g_VAR_make_fire", "g_VAR_burnt_out", "g_VAR_barrier", "g_VAR_timer", "g_VAR_kill_timer",
	"g_VAR_timer_max", "g_VAR_made_fire", "g_VAR_fire_dir", "g_VAR_fire_dir_rand", "g_VAR_fire_dist", "g_VAR_bb",
	"g_VAR_tts", "g_VAR_angle", "g_VAR_dist", "g_VAR_direction", "g_VAR_speed", "g_VAR_x", "g_VAR_y",
	"g_VAR_burn_surface", "g_VAR_burn_scale"};
static const int *var_desc[NVARS]; // {name, id}: the runner fills the ids at game start, so read them live
#define VID(n) (var_desc[n][1])

static event_fn orig_circle_step, orig_spawner_step, orig_cutscene_create, orig_cutscene_cleanup;
static int (*yy_bool)(const rv *v);
static int (*yy_compare)(const rv *a, const rv *b, double eps, int);
static void (*yy_get_value)(const rv *inst, int var, int index, rv *out, int, int);
static void (*yy_errcheck_get)(const rv *inst, int var, int index, rv *out, void *self, void *other);
static void (*yy_set_value)(const rv *inst, int var, int index, rv *value);   // YYGML_Variable_SetValue
static void (*yy_set_builtin)(const rv *inst, int var, int index, rv *value); // Variable_SetValue
static void (*yy_destroy)(void *self, void *other, int argc, void **args);
static double (*yy_random)(double range);
static float (*yy_lengthdir_x)(float len, float dir), (*yy_lengthdir_y)(float len, float dir);
static rv *(*yy_call_legacy)(void *self, void *other, rv *result, int argc, int func, rv **args);
static double (*yy_real)(const rv *v);
static int64_t (*yy_int64)(const rv *v);
static const double *math_epsilon;
static const rv *depth_flame, *depth_circle, *colour_white; // the events' constant arguments (gs_constArg*)
static const int *func_create_depth;                       // g_FUNC_instance_create_depth {name, index}
static uint32_t *rng_state, *rng_index;                     // the random generator: 16 words and an index
static int *current_room;
static int skip_cleanup; // 3. in the header; exp_dossiercleanup keeps the original search
static int exact = 1;    // exp_dossierexact0 clears it
static int verify;       // exp_dossierverify

unsigned dossier_circle_fast, dossier_circle_slow, dossier_spawner_fast, dossier_spawner_slow;
unsigned dossier_first_native, dossier_draw_native, dossier_draw_fallback;
int dossier_circles = 0; // dossierCircles in yyl.cfg (4. in the header)
unsigned dossier_children_dropped, dossier_children_kept;

static rv *vget(void *self, int n) {
	rv *(*getter)(void *, int) = (*(void ***)self)[2]; // YYObjectBase::InternalGetYYVarRef (creates a missing one)
	return getter(self, VID(n));
}

static rv *vref(void *self, int n) {
	rv *(*getter)(void *, int) = (*(void ***)self)[3]; // InternalGetYYVarRefL, what the events assign through
	return getter(self, VID(n));
}

static int real(const rv *v) { return v && v->kind == KIND_REAL; }

static int compare(const rv *a, double b) {
	rv c = {b, 0, KIND_REAL};
	return yy_compare(a, &c, *math_epsilon, 1);
}

static rv instance_rv(void *inst) {
	rv r = {0, 0, KIND_OBJECT};
	memcpy(&r.val, &inst, sizeof(inst));
	return r;
}

// ---- Profiling (heavy profiling only): time per part, per report window ----
enum { T_CIRCLE, T_DESTROY, T_FIRST, T_CIRCLE_ORIG, T_SPAWNER, T_SPAWNER_ORIG, T_DRAW, NTIMES };
static uint32_t prof_us[NTIMES], prof_n[NTIMES];
static int timing;
#define TSTART() uint32_t t0_ = timing ? sceKernelGetProcessTimeLow() : 0
#define TEND(k) do { if (timing) { prof_us[k] += sceKernelGetProcessTimeLow() - t0_; prof_n[k]++; } } while (0)

// ---- Burn summary (always): ticks from the first spawner Step to the last circle Step ----
extern uint32_t *frame_count;
static uint32_t burn_frame, burn_ticks, burn_last_circle_tick, burn_over34, burn_over50, burn_max_us, burn_reported;
static uint64_t burn_tick_us, burn_first_us, burn_last_circle_us, burn_sum_us;
static uint32_t burn_ring[30], burn_window_us, burn_worst_window_us;
static int burn_room = -1;

static void burn_tick(int circle) {
	if (!frame_count)
		return;
	uint32_t f = *frame_count;
	if (*current_room != burn_room) { // a new room: a new burn
		burn_room = *current_room;
		burn_ticks = 0;
	}
	if (f != burn_frame || !burn_ticks) {
		uint64_t now = sceKernelGetProcessTimeWide();
		if (!burn_ticks) {
			burn_first_us = now;
			burn_last_circle_us = now;
			burn_sum_us = burn_over34 = burn_over50 = burn_max_us = burn_window_us = burn_worst_window_us = 0;
			burn_last_circle_tick = burn_reported = 0;
			memset(burn_ring, 0, sizeof(burn_ring));
		} else if (f == burn_frame + 1) {
			uint32_t us = (uint32_t)(now - burn_tick_us);
			burn_sum_us += us;
			burn_over34 += us > 34000;
			burn_over50 += us > 50000;
			if (us > burn_max_us)
				burn_max_us = us;
			uint32_t *slot = &burn_ring[burn_ticks % 30];
			burn_window_us += us - *slot;
			*slot = us;
			if (burn_window_us > burn_worst_window_us)
				burn_worst_window_us = burn_window_us;
		}
		burn_frame = f;
		burn_tick_us = now;
		burn_ticks++;
	}
	if (circle) {
		burn_last_circle_tick = burn_ticks;
		burn_last_circle_us = burn_tick_us;
	}
}

// obj_folder_cutscene.fire_spd. The compiled events read it through a reference to the object, which looks up the
// object's first instance by walking the instances (~1300 at the burn peak): 10 us per read. The room has one
// obj_folder_cutscene; its Create records it (pointer and id), its Clean Up (run when it is destroyed, by
// instance_destroy or the room's end, before it is freed) forgets it, and the reference read stays the fallback. The
// instance is bound to the room of its first use (Current_Room still names the previous room while the new room's
// instances are created). Before each use the id is looked up in the runner's id table (CInstance::ms_ID2Instance):
// only a live, active instance that is still the recorded one is read.
static void *cutscene;
static uint32_t cutscene_id;
static int cutscene_room = -1;
static const uint32_t *id_hash;
unsigned dossier_spd_cached, dossier_spd_looked_up;

static void cutscene_create(void *self, void *other) {
	orig_cutscene_create(self, other);
	cutscene = self;
	cutscene_id = *(uint32_t *)((char *)self + 0x80);
	cutscene_room = -1;
}

static void cutscene_cleanup(void *self, void *other) {
	if (self == cutscene)
		cutscene = NULL;
	orig_cutscene_cleanup(self, other);
}

// The instance with this id, if the runner has one (the lookup YYGML_FindInstance makes for ids >= 100000)
static void *instance_by_id(uint32_t id) {
	const uint32_t *buckets = id_hash ? (const uint32_t *)(uintptr_t)id_hash[0] : NULL;
	if (!buckets)
		return NULL;
	const uint32_t *node = (const uint32_t *)(uintptr_t)buckets[2 * (id & id_hash[1])];
	for (unsigned trips = 0; node && trips < 4096; node = (const uint32_t *)(uintptr_t)node[1], trips++)
		if (node[2] == id)
			return (void *)(uintptr_t)node[3];
	return NULL;
}

static void *live_cutscene(void) {
	if (!cutscene || instance_by_id(cutscene_id) != cutscene)
		return NULL;
	// still an obj_folder_cutscene, not marked for destruction or deactivated (instance flags at +0x7c)
	if (*(int *)((char *)cutscene + 0x84) != OBJ_FOLDER_CUTSCENE || (*(uint32_t *)((char *)cutscene + 0x7c) & 0x100003))
		return NULL;
	return cutscene;
}

static int fire_spd(double *out) {
	void *live = live_cutscene();
	if (live && cutscene_room < 0)
		cutscene_room = *current_room;
	if (live && *current_room == cutscene_room) {
		rv *v = vget(live, V_FIRE_SPD);
		if (v && v->kind == KIND_REAL) {
			dossier_spd_cached++;
			*out = v->val;
			return 1;
		}
	}
	rv obj = {0}, spd = {0};
	uint32_t ref[2] = {OBJ_FOLDER_CUTSCENE, REF_OBJECT};
	memcpy(&obj.val, ref, sizeof(ref));
	obj.kind = KIND_REF;
	yy_get_value(&obj, VID(V_FIRE_SPD), NO_INDEX, &spd, 0, 0);
	dossier_spd_looked_up++;
	*out = spd.val;
	return spd.kind == KIND_REAL; // nothing to free: only strings, arrays and structs are counted
}

// ---- Circle Step ----

// Where the native Step reads and writes: the instance (live) or, in verify mode, a copy of its variables (dry), with
// the creations replayed from the original's record and the writes to created instances recorded.
#define MAX_CREATES 12
typedef struct { rv args[4], result; uint32_t rng_before[17], rng_after[17]; } created;
typedef struct { int create, var; rv value; } child_write;
typedef struct {
	int dry, keep_all;
	rv shadow[NVARS];
	uint8_t has[NVARS], written[NVARS];
	int creates, writes, destroy, mismatch;
	child_write write[MAX_CREATES * 3];
} ctx;

static created record[MAX_CREATES];
static int record_n, recording;

static rv *slot_read(ctx *c, void *self, int n) {
	if (!c->dry)
		return vget(self, n);
	if (!c->has[n])
		return NULL;
	return &c->shadow[n];
}

static rv *slot_write(ctx *c, void *self, int n) {
	if (!c->dry)
		return vref(self, n);
	if (!c->has[n]) {
		c->shadow[n] = (rv){0, 0, KIND_UNDEFINED};
		c->has[n] = 1;
	}
	c->written[n] = 1;
	return &c->shadow[n];
}

static void rng_save(uint32_t *to) {
	memcpy(to, rng_state, 64);
	to[16] = *rng_index;
}

static void rng_load(const uint32_t *from) {
	memcpy(rng_state, from, 64);
	*rng_index = from[16];
}

static int same_rv(const rv *a, const rv *b) { return a->kind == b->kind && !memcmp(&a->val, &b->val, 8); }

// instance_create_depth(args) as the compiled event calls it; dry: the original's call with the same index, checked
static void create_instance(ctx *c, void *self, void *other, rv *args, rv *result) {
	if (!c->dry) {
		rv *argv[4] = {&args[0], &args[1], &args[2], &args[3]};
		*result = (rv){0, 0, KIND_UNDEFINED};
		yy_call_legacy(self, other, result, 4, func_create_depth[1], argv);
		return;
	}
	int k = c->creates++;
	if (k >= record_n) {
		c->mismatch |= 1;
		*result = (rv){0, 0, KIND_UNDEFINED};
		return;
	}
	uint32_t now[17];
	rng_save(now);
	for (int i = 0; i < 4; i++)
		if (!same_rv(&args[i], &record[k].args[i]))
			c->mismatch |= 2;
	if (memcmp(now, record[k].rng_before, sizeof(now)))
		c->mismatch |= 4;
	rng_load(record[k].rng_after);
	*result = record[k].result;
}

static void child_set(ctx *c, const rv *bb, int n, rv *value) {
	if (c->dry) {
		if (c->writes < (int)(sizeof(c->write) / sizeof(c->write[0])))
			c->write[c->writes++] = (child_write){c->creates - 1, n, *value};
		return;
	}
	if (n == V_DIRECTION || n == V_SPEED)
		yy_set_builtin(bb, VID(n), NO_INDEX, value);
	else
		yy_set_value(bb, VID(n), NO_INDEX, value);
}

static void store(rv *slot, const rv *value) {
	*slot = *value; // the slot held no counted value (checked before)
}

static int children_counter;

// The obj_burncircle Step natively (see the header). Returns 0, before any side effect, to run the original.
static int circle_native(void *self, void *other, ctx *c) {
	rv *spawn = slot_read(c, self, V_SPAWN);
	if (!spawn || (spawn->kind != KIND_BOOL && spawn->kind != KIND_REAL))
		return 0;
	int first = !yy_bool(spawn);
	rv *grow = slot_read(c, self, V_GROW_SPD), *size = slot_read(c, self, V_SIZE), *max = slot_read(c, self, V_MAX_SIZE);
	if (!real(grow) || !real(size) || !real(max))
		return 0;
	double spd;
	if (!fire_spd(&spd))
		return 0;
	if (first) {
		if (!exact || !func_create_depth || func_create_depth[1] < 0 || !depth_flame || !depth_circle)
			return 0;
		rv *made = slot_read(c, self, V_MADE_FIRE), *timer = slot_read(c, self, V_TIMER), *tts = slot_read(c, self, V_TTS);
		if (!made || (made->kind != KIND_BOOL && made->kind != KIND_REAL) || !real(timer) || !tts)
			return 0;
		int flames = !yy_bool(made);
		rv *fire_dir = NULL;
		if (flames && !real(fire_dir = slot_read(c, self, V_FIRE_DIR)))
			return 0;
		// timer += 1; the children's test (timer > tts || size + grow_spd * 2 >= 15): flames change none of them
		rv t1 = *timer, sum = *size;
		t1.val += 1.0;
		sum.val += grow->val * 2.0;
		int children = yy_compare(&t1, tts, *math_epsilon, 1) > 0 || compare(&sum, 15.0) > -1;
		int keep[6], kept = 0;
		for (int k = 0; k < 6; k++) {
			keep[k] = c->keep_all || dossier_circles == 1 || (dossier_circles > 0 && (children_counter + k) % dossier_circles == 0);
			kept += children && keep[k];
		}
		// The slots written: none may hold a counted value (the original would free it)
		rv *w_made = slot_write(c, self, V_MADE_FIRE), *w_spawn = slot_write(c, self, V_SPAWN);
		rv *w_fdr = NULL, *w_fdist = NULL, *w_angle = NULL, *w_dist = NULL, *w_bb = NULL;
		if (flames) {
			w_fdr = slot_write(c, self, V_FIRE_DIR_RAND);
			w_fdist = slot_write(c, self, V_FIRE_DIST);
		}
		if (children) {
			w_angle = slot_write(c, self, V_ANGLE);
			w_dist = slot_write(c, self, V_DIST);
		}
		if (flames || kept)
			w_bb = slot_write(c, self, V_BB);
		rv *slots[7] = {w_made, w_spawn, w_fdr, w_fdist, w_angle, w_dist, w_bb};
		for (int i = 0; i < 7; i++)
			if ((i < 2 || slots[i]) && (!slots[i] || REFCOUNTED(slots[i]->kind)))
				return 0;
		// From here on the Step runs natively
		rv self_rv = instance_rv(self), xv = {0}, yv = {0};
		if (flames) {
			uint32_t ref[2] = {OBJ_FOLDER_FIREPARTICLE, REF_OBJECT};
			for (int k = 0; k < 3; k++) {
				double r = yy_random(1.0);
				store(w_fdr, &(rv){(fire_dir->val - 45.0) + ((r * 45.0) * 2.0), 0, KIND_REAL});
				store(w_fdist, &(rv){20.0 + (r * 10.0), 0, KIND_REAL});
				rv args[4];
				yy_get_value(&self_rv, VID(V_X), NO_INDEX, &xv, 0, 0);
				yy_get_value(&self_rv, VID(V_Y), NO_INDEX, &yv, 0, 0);
				float lx = yy_lengthdir_x((float)w_fdist->val, (float)w_fdr->val);
				args[0] = xv;
				args[0].val += (double)lx;
				float ly = yy_lengthdir_y((float)w_fdist->val, (float)w_fdr->val);
				args[1] = yv;
				args[1].val += (double)ly;
				args[2] = *depth_flame;
				args[3] = (rv){0, 0, KIND_REF};
				memcpy(&args[3].val, ref, sizeof(ref));
				rv result;
				create_instance(c, self, other, args, &result);
				store(w_bb, &result);
				child_set(c, w_bb, V_DIRECTION, &(rv){80.0 + ((r * 10.0) * 2.0), 0, KIND_REAL});
				child_set(c, w_bb, V_SPEED, &(rv){4.0 + (r * 3.0), 0, KIND_REAL});
				rv dir = {0, 0, KIND_UNDEFINED};
				yy_errcheck_get(w_bb, VID(V_DIRECTION), NO_INDEX, &dir, self, other);
				child_set(c, w_bb, V_ANGLE, &dir);
			}
			store(w_made, &(rv){1.0, 0, KIND_BOOL});
		}
		timer = slot_write(c, self, V_TIMER); // the same slot (live: the ref of the same variable)
		timer->val += 1.0;
		if (children) {
			uint32_t ref[2] = {OBJ_BURNCIRCLE, REF_OBJECT};
			for (int k = 0; k < 6; k++) {
				double r = yy_random(1.0);
				store(w_angle, &(rv){r * 360.0, 0, KIND_REAL});
				store(w_dist, &(rv){r * 3.0, 0, KIND_REAL});
				if (!keep[k]) {
					yy_random(1.5); // the dropped child's Create: grow_spd = random(1.5) + 0.5
					dossier_children_dropped++;
					continue;
				}
				rv args[4];
				yy_get_value(&self_rv, VID(V_X), NO_INDEX, &xv, 0, 0);
				yy_get_value(&self_rv, VID(V_Y), NO_INDEX, &yv, 0, 0);
				float lx = yy_lengthdir_x((float)w_dist->val, (float)w_angle->val);
				args[0] = xv;
				args[0].val += (double)lx;
				float ly = yy_lengthdir_y((float)w_dist->val, (float)w_angle->val);
				args[1] = yv;
				args[1].val += (double)ly;
				args[2] = *depth_circle;
				args[3] = (rv){0, 0, KIND_REF};
				memcpy(&args[3].val, ref, sizeof(ref));
				rv result;
				create_instance(c, self, other, args, &result);
				store(w_bb, &result);
				child_set(c, w_bb, V_MAX_SIZE, &(rv){(r * 25.0) + 5.0, 0, KIND_REAL});
				child_set(c, w_bb, V_SPAWN, &(rv){1.0, 0, KIND_BOOL});
				if (!c->dry)
					dossier_children_kept++;
			}
			if (!c->dry)
				children_counter += 6;
			store(w_spawn, &(rv){1.0, 0, KIND_BOOL});
		}
		if (!c->dry)
			dossier_first_native++;
		// The tail below reads the same slots (grow_spd, size, max_size are not written above)
	}
	if (compare(grow, 0.5) >= 1) {
		grow = slot_write(c, self, V_GROW_SPD);
		grow->val -= 0.1;
	}
	size = slot_write(c, self, V_SIZE);
	size->val += grow->val * spd;
	if (compare(size, max->val) >= 1) {
		if (c->dry) {
			c->destroy = 1;
		} else {
			TSTART();
			yy_destroy(self, other, 0, NULL);
			TEND(T_DESTROY);
		}
	}
	return 1;
}

// ---- Verify mode ----
unsigned dossier_verify_steps, dossier_verify_first, dossier_verify_mismatch, dossier_verify_fallback;
static builtin_fn orig_create_depth;

static void create_depth_record(rv *result, void *self, void *other, int argc, rv *args) {
	if (!recording || argc != 4 || record_n >= MAX_CREATES) {
		if (recording)
			record_n = MAX_CREATES + 1; // overflow: the step is not compared
		orig_create_depth(result, self, other, argc, args);
		return;
	}
	created *r = &record[record_n++];
	memcpy(r->args, args, sizeof(r->args));
	rng_save(r->rng_before);
	orig_create_depth(result, self, other, argc, args);
	r->result = *result;
	rng_save(r->rng_after);
}

static void *created_instance(const rv *id) {
	uint32_t w[2];
	memcpy(w, &id->val, sizeof(w));
	if (id->kind == KIND_REF)
		return instance_by_id(w[0]);
	if (id->kind == KIND_REAL)
		return instance_by_id((uint32_t)id->val);
	return NULL;
}

static void verify_report(const char *what, ctx *c, void *self) {
	if (dossier_verify_mismatch++ < 12)
		debugPrintf("[DOSSIER] verify mismatch (%s) at circle %u: flags %x creates %d/%d writes %d destroy %d\n", what,
			*(uint32_t *)((char *)self + 0x80), c->mismatch, c->creates, record_n, c->writes, c->destroy);
}

static void circle_verify(void *self, void *other) {
	static const int read_set[] = {V_SPAWN, V_GROW_SPD, V_SIZE, V_MAX_SIZE, V_MADE_FIRE, V_TIMER, V_TTS, V_FIRE_DIR};
	static ctx c;
	memset(&c, 0, sizeof(c));
	c.dry = 1;
	c.keep_all = 1;
	for (unsigned i = 0; i < sizeof(read_set) / sizeof(read_set[0]); i++) {
		rv *v = vget(self, read_set[i]); // all set by the circle's Create
		if (v && !REFCOUNTED(v->kind)) {
			c.shadow[read_set[i]] = *v;
			c.has[read_set[i]] = 1;
		}
	}
	uint32_t before[17], after[17], flags0 = *(uint32_t *)((char *)self + 0x7c);
	rng_save(before);
	record_n = 0;
	recording = 1;
	orig_circle_step(self, other);
	recording = 0;
	rng_save(after);
	uint32_t flags1 = *(uint32_t *)((char *)self + 0x7c);
	dossier_verify_steps++;
	if (record_n > MAX_CREATES) {
		dossier_verify_fallback++;
		return;
	}
	rng_load(before);
	int handled = circle_native(self, other, &c);
	uint32_t dry_rng[17];
	rng_save(dry_rng);
	rng_load(after); // the game goes on with the original's state
	if (!handled) {
		dossier_verify_fallback++;
		return;
	}
	if (c.creates)
		dossier_verify_first++;
	if (c.creates != record_n)
		c.mismatch |= 8;
	if (memcmp(dry_rng, after, sizeof(after)))
		c.mismatch |= 16;
	if (c.destroy != (flags0 != flags1))
		c.mismatch |= 32;
	// self's variables the dry run wrote, against the instance after the original
	for (int n = 0; n < NVARS; n++)
		if (c.written[n]) {
			rv *v = vget(self, n);
			// The flags word too, except on bools: the original copies a reused temporary's leftover flags with them
			if (!v || !same_rv(v, &c.shadow[n]) || (v->kind != KIND_BOOL && v->flags != c.shadow[n].flags)) {
				if (!(c.mismatch & 64) && dossier_verify_mismatch < 12)
					debugPrintf("[DOSSIER] verify: self var %d: original %.17g kind %u flags %x, native %.17g kind %u\n", n,
						v ? v->val : -1.0, v ? v->kind : 99u, v ? v->flags : 0u, c.shadow[n].val, c.shadow[n].kind);
				c.mismatch |= 64;
			}
		}
	// the created instances' variables
	for (int i = 0; i < c.writes; i++) {
		child_write *w = &c.write[i];
		void *child = w->create >= 0 && w->create < record_n ? created_instance(&record[w->create].result) : NULL;
		rv got = {0, 0, KIND_UNDEFINED};
		if (child) {
			if (w->var == V_DIRECTION || w->var == V_SPEED) {
				rv inst = instance_rv(child);
				yy_get_value(&inst, VID(w->var), NO_INDEX, &got, 0, 0);
			} else {
				rv *v = vget(child, w->var);
				if (v)
					got = *v;
			}
		}
		// direction and speed are float fields: the instance holds (float)value, which reads back as a double
		if ((w->var == V_DIRECTION || w->var == V_SPEED) && w->value.kind == KIND_REAL)
			w->value.val = (double)(float)w->value.val;
		if (!child || !same_rv(&got, &w->value)) {
			if (!(c.mismatch & 128) && dossier_verify_mismatch < 12)
				debugPrintf("[DOSSIER] verify: child %d var %d: original %.17g kind %u, native %.17g kind %u\n", w->create,
					w->var, got.val, got.kind, w->value.val, w->value.kind);
			c.mismatch |= 128;
		}
	}
	if (c.mismatch)
		verify_report("step", &c, self);
}

static void circle_step(void *self, void *other) {
	burn_tick(1);
	if (verify) {
		circle_verify(self, other);
		return;
	}
	TSTART();
	static ctx live; // only dry and keep_all are read in live mode
	if (circle_native(self, other, &live)) {
		dossier_circle_fast++;
		TEND(T_CIRCLE);
		return;
	}
	dossier_circle_slow++;
	orig_circle_step(self, other);
	TEND(T_CIRCLE_ORIG);
}

// ---- Spawner Step ----

static void spawner_step(void *self, void *other) {
	burn_tick(0);
	TSTART();

	rv *make_fire = vget(self, V_MAKE_FIRE), *burnt_out = vget(self, V_BURNT_OUT);
	if (!make_fire || !burnt_out || yy_bool(make_fire))
		goto slow;
	if (!yy_bool(burnt_out)) {
		// Spreading cell: timer += 1 * fire_spd; it spreads (the original runs) once timer > timer_max
		rv *timer = vget(self, V_TIMER), *timer_max = vget(self, V_TIMER_MAX);
		double spd;
		if (!timer || !timer_max || timer->kind != KIND_REAL || !fire_spd(&spd))
			goto slow;
		double add = 1.0 * spd;
		rv next = *timer;
		next.val += add;
		if (yy_compare(&next, timer_max, *math_epsilon, 1) >= 1)
			goto slow;
		timer->val = next.val;
		dossier_spawner_fast++;
		TEND(T_SPAWNER);
		return;
	}
	rv *barrier = vget(self, V_BARRIER);
	if (!barrier)
		goto slow;
	if (yy_bool(barrier) || skip_cleanup) {
		dossier_spawner_fast++;
		TEND(T_SPAWNER);
		return;
	}
	// Burned-out cell: timer += 1; the cleanup search (the original) runs once timer > 30 or while kill_timer > 0
	rv *timer = vget(self, V_TIMER), *kill = vget(self, V_KILL_TIMER);
	if (!timer || !kill || timer->kind != KIND_REAL)
		goto slow;
	rv next = *timer;
	next.val += 1.0;
	if (compare(&next, 30.0) > 0 || compare(kill, 0.0) >= 1)
		goto slow; // the cleanup search; the original also does the increment
	timer->val = next.val;
	dossier_spawner_fast++;
	TEND(T_SPAWNER);
	return;
slow:
	dossier_spawner_slow++;
	orig_spawner_step(self, other);
	TEND(T_SPAWNER_ORIG);
}

// ---- Burn-mask stamps (cutscene Draw, with (obj_burncircle) draw_bigcircle(...)) ----

typedef struct { void *saved_self, *saved_other, *list, *cursor; } with_iterator;
static int (*new_with)(with_iterator *it, void **self, void **other, const rv *target);
static int (*next_with)(with_iterator *it, void **self, void **other);
static void (*delete_with)(with_iterator *it, void **self, void **other);
static void (*draw_sprite_ext)(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
	int colour, float alpha);
static void (*draw_sprite_ext_orig)(void *self, int sprite, float subimg, float x, float y, float xs, float ys,
	float rot, int colour, float alpha);
static builtin_fn draw_get_alpha;
static int draw_native, stamp_verify;
static uint32_t draw_t0;

typedef struct { void *self; int sprite, colour; float subimg, x, y, xs, ys, rot, alpha; } stamp;
#define MAX_STAMPS 2048
static stamp *dry_stamps, *gml_stamps;
static int dry_n, gml_n, capturing;
unsigned dossier_draw_verified, dossier_draw_mismatch;

static void draw_capture(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
		int colour, float alpha) {
	if (capturing) {
		if (gml_n < MAX_STAMPS)
			gml_stamps[gml_n] = (stamp){self, sprite, colour, subimg, x, y, xs, ys, rot, alpha};
		gml_n++;
	}
	draw_sprite_ext_orig(self, sprite, subimg, x, y, xs, ys, rot, colour, alpha);
}

static int bigcircle_sprite = -1;

// One iteration of the loop (self: the circle, other: the cutscene). dry: check only, or record the draw
static int stamp_one(void *self, void *other, int mode) {
	enum { CHECK, DRY, DRAW };
	if (mode == CHECK) {
		rv *size = vget(self, V_SIZE);
		return real(size);
	}
	vget(self, V_BURN_SURFACE); // the compiled body's dead read, which creates the variable on the circle
	rv *bs = vget(other, V_BURN_SCALE), *size = vget(self, V_SIZE);
	rv inst = instance_rv(self), xv = {0}, yv = {0}, alpha = {0};
	yy_get_value(&inst, VID(V_X), NO_INDEX, &xv, 0, 0);
	yy_get_value(&inst, VID(V_Y), NO_INDEX, &yv, 0, 0);
	double a0 = xv.val / bs->val, a1 = yv.val / bs->val, a2 = (size->val * 2.0) / bs->val;
	// draw_bigcircle: draw_sprite_ext(spr_bigcircle, 0, a0, a1, a2 / 400 * 2, a2 / 400 * 2, 0, a3, draw_get_alpha())
	double scale = (a2 / 400.0) * 2.0;
	int colour = (int)yy_int64(colour_white);
	draw_get_alpha(&alpha, self, other, 0, NULL);
	stamp s = {self, bigcircle_sprite, colour, 0.0f, (float)a0, (float)a1, (float)scale, (float)scale, 0.0f, (float)alpha.val};
	if (mode == DRY) {
		if (dry_n < MAX_STAMPS)
			dry_stamps[dry_n] = s;
		dry_n++;
	} else {
		draw_sprite_ext(s.self, s.sprite, s.subimg, s.x, s.y, s.xs, s.ys, s.rot, s.colour, s.alpha);
	}
	return 1;
}

// spr_bigcircle's index, as draw_bigcircle converts its asset reference: REAL_RValue_Ex, then to an integer
static void stamp_sprite(void) {
	if (bigcircle_sprite >= 0)
		return;
	rv spr = {0, 0, KIND_REF};
	uint32_t ref[2] = {0x90, REF_SPRITE};
	memcpy(&spr.val, ref, sizeof(ref));
	double v = yy_real(&spr);
	if (v >= 0 && v < 100000)
		bigcircle_sprite = (int)v;
}

// The loop's preconditions, for the whole list (no iteration may fall back once the first is drawn)
static int stamps_ok(with_iterator *it, void *first, void *other) {
	if (!other || !first || bigcircle_sprite < 0)
		return 0;
	rv *bs = vget(other, V_BURN_SCALE);
	if (!real(bs) || bs->val == 0 || !colour_white || colour_white->kind != KIND_REAL)
		return 0;
	with_iterator save = *it;
	void *s = first, *o = other;
	int ok = 1, r = 1;
	while (ok && r > 0) {
		ok = stamp_one(s, other, 0);
		r = next_with(it, &s, &o);
	}
	*it = save;
	return ok;
}

static int draw_new_with(with_iterator *it, void **self, void **other, const rv *target) {
	if (timing)
		draw_t0 = sceKernelGetProcessTimeLow();
	int r = new_with(it, self, other, target);
	if (r < 1 || !draw_native)
		return r;
	stamp_sprite();
	if (!stamps_ok(it, *self, *other)) {
		dossier_draw_fallback++;
		return r;
	}
	if (stamp_verify) {
		with_iterator save = *it;
		void *first = *self;
		dry_n = 0;
		for (int more = 1; more > 0; more = next_with(it, self, other))
			stamp_one(*self, *other, 1);
		*it = save;
		*self = first;
		gml_n = 0;
		capturing = 1;
		return r; // the original loop runs, compared at its DeleteWithIterator
	}
	for (int more = 1; more > 0; more = next_with(it, self, other))
		stamp_one(*self, *other, 2);
	dossier_draw_native++;
	return 0; // the original goes straight to DeleteWithIterator
}

static void draw_delete_with(with_iterator *it, void **self, void **other) {
	if (capturing) {
		capturing = 0;
		int n = dry_n < MAX_STAMPS ? dry_n : MAX_STAMPS;
		if (dry_n == gml_n && !memcmp(dry_stamps, gml_stamps, n * sizeof(stamp))) {
			dossier_draw_verified++;
		} else if (dossier_draw_mismatch++ < 8) {
			debugPrintf("[DOSSIER] stamp verify mismatch: draws %d native %d\n", gml_n, dry_n);
			for (int i = 0; i < n && i < gml_n; i++)
				if (memcmp(&dry_stamps[i], &gml_stamps[i], sizeof(stamp))) {
					stamp *a = &gml_stamps[i], *b = &dry_stamps[i];
					debugPrintf("[DOSSIER]   %d: original spr %d col %d %.9g %.9g %.9g %.9g %.9g | native spr %d col %d %.9g %.9g %.9g %.9g %.9g\n", i,
						a->sprite, a->colour, a->x, a->y, a->xs, a->ys, a->alpha, b->sprite, b->colour, b->x, b->y, b->xs, b->ys, b->alpha);
					break;
				}
		}
	}
	delete_with(it, self, other);
	if (timing && draw_t0) {
		prof_us[T_DRAW] += sceKernelGetProcessTimeLow() - draw_t0;
		prof_n[T_DRAW]++;
		draw_t0 = 0;
	}
}

// An 8-byte veneer within bl range of site (ldr pc, [pc, #-4]; .word wrapper), or NULL
static uint32_t *alloc_veneer(uintptr_t site) {
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	int32_t off = veneer ? (int32_t)((uintptr_t)veneer - (site + 8)) : 0;
	return veneer && off >= -(1 << 25) && off < (1 << 25) ? veneer : NULL;
}

static int check_call(uintptr_t site, uintptr_t plt, uintptr_t direct) {
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	return (ins >> 24) == 0xeb && (target == plt || target == direct);
}

static void patch_call(uintptr_t site, uint32_t *veneer, void *wrapper) {
	uint32_t code[2] = {0xe51ff004, (uint32_t)(uintptr_t)wrapper};
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	int32_t off = (int32_t)((uintptr_t)veneer - (site + 8));
	uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
}

// The with loop's New and Delete calls in the cutscene's Draw (APK 1.1.10 ELF addresses). Both sites and the loop's
// target (mov r0, #0x5c: obj_burncircle) are checked, and both veneers allocated, before either is written.
static int patch_stamps(uintptr_t draw) {
	enum { EVENT = 0x3faf478, TARGET = 0x3fbce84, NEW_SITE = 0x3fbcea8, DELETE_SITE = 0x3fbd378, PLT_NEW = 0x4ab0e00,
		PLT_DELETE = 0x4ab0e40 };
#define AT(a) (draw + (a) - EVENT)
	if (*(uint32_t *)AT(TARGET) != 0xe3a0005c || !check_call(AT(NEW_SITE), AT(PLT_NEW), (uintptr_t)new_with) ||
			!check_call(AT(DELETE_SITE), AT(PLT_DELETE), (uintptr_t)delete_with))
		return 0;
	uint32_t *v_new = alloc_veneer(AT(NEW_SITE)), *v_delete = alloc_veneer(AT(DELETE_SITE));
	if (!v_new || !v_delete)
		return 0;
	patch_call(AT(NEW_SITE), v_new, draw_new_with);
	patch_call(AT(DELETE_SITE), v_delete, draw_delete_with);
#undef AT
	return 1;
}

// ---- Flames (obj_folder_fireparticle Step): obj_folder_cutscene.fire_spd ----
// The flame Step reads obj_folder_cutscene.fire_spd four times, each a YYGML_Variable_GetValue on the object reference
// (a lookup of the object's first instance). Those four calls go through veneers to flame_get_value, which copies the
// recorded cutscene's slot (see fire_spd) when the reference, variable and index are the ones of those lines, and
// calls the original otherwise. In verify mode it calls the original too and compares.
unsigned dossier_flame_spd_cached, dossier_flame_spd_original, dossier_flame_spd_mismatch;

static rv *cutscene_fire_spd_slot(void) {
	void *live = live_cutscene();
	if (live && cutscene_room < 0)
		cutscene_room = *current_room;
	if (!live || *current_room != cutscene_room)
		return NULL;
	rv *v = vget(live, V_FIRE_SPD);
	return v && v->kind == KIND_REAL ? v : NULL;
}

static void flame_get_value(const rv *inst, int var, int index, rv *out, int a, int b) {
	uint32_t w[2];
	memcpy(w, &inst->val, sizeof(w));
	rv *slot;
	if (inst->kind == KIND_REF && w[0] == OBJ_FOLDER_CUTSCENE && w[1] == REF_OBJECT && var == VID(V_FIRE_SPD) &&
			index == NO_INDEX && !a && !b && !REFCOUNTED(out->kind) && (slot = cutscene_fire_spd_slot())) {
		if (verify) {
			rv got = {0, 0, KIND_UNDEFINED};
			yy_get_value(inst, var, index, &got, a, b);
			if (memcmp(&got, slot, sizeof(rv)))
				dossier_flame_spd_mismatch++;
		}
		*out = *slot;
		dossier_flame_spd_cached++;
		return;
	}
	dossier_flame_spd_original++;
	yy_get_value(inst, var, index, out, a, b);
}

static int patch_flames(uintptr_t step) {
	enum { EVENT = 0x3fc54e4, PLT_GET = 0x4ab0e70 };
	static const uint32_t sites[4] = {0x3fc59d8, 0x3fc5be0, 0x3fc5df8, 0x3fc605c};
	uint32_t *veneers[4];
	for (int i = 0; i < 4; i++) {
		uintptr_t site = step + sites[i] - EVENT;
		// movw r0, #0x1ca (the reference's object) 0x30 bytes before each call
		if (*(uint32_t *)(site - 0x30) != 0xe30001ca || !check_call(site, step + PLT_GET - EVENT, (uintptr_t)yy_get_value) ||
				!(veneers[i] = alloc_veneer(site)))
			return 0;
	}
	for (int i = 0; i < 4; i++)
		patch_call(step + sites[i] - EVENT, veneers[i], flame_get_value);
	return 1;
}

int dossier_native_install(void *(*entry_hook)(uintptr_t entry, void *replacement)) {
#define SYM(name) so_symbol(&yoyoloader_mod, name)
	yy_bool = (void *)SYM("_Z11BOOL_RValuePK6RValue");
	yy_compare = (void *)SYM("_Z12YYCompareValRK6RValueS1_db");
	yy_get_value = (void *)SYM("_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	yy_errcheck_get = (void *)SYM("_Z32YYGML_ErrCheck_Variable_GetValueRK8YYRValueiiP6RValueP9CInstanceS5_");
	yy_set_value = (void *)SYM("_Z23YYGML_Variable_SetValueRK8YYRValueiiP6RValue");
	yy_set_builtin = (void *)SYM("_Z17Variable_SetValueRK8YYRValueiiP6RValue");
	yy_destroy = (void *)SYM("_Z22YYGML_instance_destroyP9CInstanceS0_iPP8YYRValue");
	yy_random = (void *)SYM("_Z12YYGML_randomd");
	yy_lengthdir_x = (void *)SYM("_Z17YYGML_lengthdir_xff");
	yy_lengthdir_y = (void *)SYM("_Z17YYGML_lengthdir_yff");
	yy_call_legacy = (void *)SYM("_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	yy_real = (void *)SYM("_Z14REAL_RValue_ExPK6RValue");
	yy_int64 = (void *)SYM("_Z12INT64_RValuePK6RValue");
	math_epsilon = (const double *)SYM("g_GMLMathEpsilon");
	depth_flame = (const rv *)SYM("gs_constArg0_74FF3937");
	depth_circle = (const rv *)SYM("gs_constArg1_74FF3937");
	colour_white = (const rv *)SYM("gs_constArg1_B64F29A3");
	func_create_depth = (const int *)SYM("g_FUNC_instance_create_depth");
	rng_state = (uint32_t *)SYM("state");
	rng_index = (uint32_t *)SYM("g_RndIndex");
	current_room = (int *)SYM("Current_Room");
	id_hash = (const uint32_t *)SYM("_ZN9CInstance14ms_ID2InstanceE");
	new_with = (void *)SYM("_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	next_with = (void *)SYM("_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
	delete_with = (void *)SYM("_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_");
	draw_sprite_ext = (void *)SYM("_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
	draw_get_alpha = (builtin_fn)SYM("_Z14F_DrawGetAlphaR6RValueP9CInstanceS2_iPS_");
	uintptr_t create = SYM("_Z39gml_Object_obj_folder_cutscene_Create_0P9CInstanceS0_");
	uintptr_t cleanup = SYM("_Z40gml_Object_obj_folder_cutscene_CleanUp_0P9CInstanceS0_");
	uintptr_t draw = SYM("_Z37gml_Object_obj_folder_cutscene_Draw_0P9CInstanceS0_");
	uintptr_t circle = SYM("_Z32gml_Object_obj_burncircle_Step_0P9CInstanceS0_");
	uintptr_t spawner = SYM("_Z40gml_Object_obj_burncircle_spawner_Step_0P9CInstanceS0_");
	uintptr_t flame = SYM("_Z41gml_Object_obj_folder_fireparticle_Step_0P9CInstanceS0_");
	int vars_ok = 1;
	for (int n = 0; n < NVARS; n++)
		vars_ok = vars_ok && (var_desc[n] = (const int *)SYM(var_names[n]));
#undef SYM
	if (!yy_bool || !yy_compare || !yy_get_value || !yy_destroy || !math_epsilon || !circle || !spawner || !create ||
			!cleanup || !current_room || !id_hash || !vars_ok)
		return 0;
	skip_cleanup = !exp_flag("dossiercleanup");
	extern int heavy_profiling;
	timing = heavy_profiling;
	exact = !exp_flag("dossierexact0") && yy_errcheck_get && yy_set_value && yy_set_builtin && yy_random &&
		yy_lengthdir_x && yy_lengthdir_y && yy_call_legacy && depth_flame && depth_circle && func_create_depth;
	verify = exact && exp_flag("dossierverify") && rng_state && rng_index;
	if (verify) {
		// Record the original's instance_create_depth calls (the function registry is filled by now)
		typedef struct { const char *name; void *func; int argc; } runner_function;
		runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
		int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
		for (int i = 0; functions && *functions && count && i < *count; i++) {
			runner_function *f = &(*functions)[i];
			if (f->name && !strcmp(f->name, "instance_create_depth")) {
				orig_create_depth = f->func;
				f->func = create_depth_record;
				break;
			}
		}
		verify = orig_create_depth != NULL;
	}
	// 2.: the stamps
	draw_native = exact && draw && new_with && next_with && delete_with && draw_sprite_ext && draw_get_alpha && yy_real &&
		yy_int64 && colour_white;
	if (draw_native && verify) {
		static stamp buffers[2][MAX_STAMPS];
		dry_stamps = buffers[0];
		gml_stamps = buffers[1];
		// the original's draws are captured at YYGML_draw_sprite_ext (not hooked when profiling hooks it already)
		draw_sprite_ext_orig = entry_hook((uintptr_t)draw_sprite_ext, draw_capture);
		stamp_verify = draw_sprite_ext_orig != NULL;
		if (!stamp_verify)
			debugPrintf("[DOSSIER] stamp verify off: draw_sprite_ext is hooked\n");
	}
	if (draw && new_with && delete_with && next_with && !patch_stamps(draw))
		draw_native = 0;
	int flames = exact && flame && patch_flames(flame);
	if (!(orig_cutscene_cleanup = entry_hook(cleanup, cutscene_cleanup)) ||
			!(orig_cutscene_create = entry_hook(create, cutscene_create)))
		return 0;
	orig_circle_step = entry_hook(circle, circle_step);
	orig_spawner_step = entry_hook(spawner, spawner_step);
	debugPrintf("[DOSSIER] native first Step %s, stamps %s, flame fire_spd %s%s, decorative circles: keep 1 in %d\n",
		exact ? "on" : "off", draw_native ? "on" : "off", flames ? "on" : "off", verify ? " (verify mode, all children kept)" : "",
		dossier_circles);
	return orig_circle_step && orig_spawner_step;
}

// Debug (profiling report): the cutscene's fire_spd, when the cutscene instance is known and alive
int dossier_fire_spd(double *out) {
	return live_cutscene() && cutscene_room >= 0 && fire_spd(out);
}

// Debug report, every report window (also unprofiled)
void dossier_report(int frames) {
	if (timing && frames > 0 && (prof_n[T_CIRCLE] || prof_n[T_CIRCLE_ORIG] || prof_n[T_SPAWNER_ORIG] || prof_n[T_DRAW])) {
		debugPrintf("[DOSSIER] per tick: circle native %.2fms (%.0f, destroy %.2fms %.1f) original %.2fms (%.1f) | spawner native "
			"%.2fms (%.0f) original %.2fms (%.1f) | stamps %.2fms\n",
			prof_us[T_CIRCLE] / 1000.0f / frames, (float)prof_n[T_CIRCLE] / frames, prof_us[T_DESTROY] / 1000.0f / frames,
			(float)prof_n[T_DESTROY] / frames, prof_us[T_CIRCLE_ORIG] / 1000.0f / frames, (float)prof_n[T_CIRCLE_ORIG] / frames,
			prof_us[T_SPAWNER] / 1000.0f / frames, (float)prof_n[T_SPAWNER] / frames, prof_us[T_SPAWNER_ORIG] / 1000.0f / frames,
			(float)prof_n[T_SPAWNER_ORIG] / frames, prof_us[T_DRAW] / 1000.0f / frames);
	}
	memset(prof_us, 0, sizeof(prof_us));
	memset(prof_n, 0, sizeof(prof_n));
	if (verify && (dossier_verify_steps || dossier_draw_verified || dossier_draw_mismatch))
		debugPrintf("[DOSSIER] verify: %u circle steps (%u first), %u mismatches, %u not compared | stamp loops %u same, %u differ\n",
			dossier_verify_steps, dossier_verify_first, dossier_verify_mismatch, dossier_verify_fallback,
			dossier_draw_verified, dossier_draw_mismatch);
	if (verify && (dossier_flame_spd_cached || dossier_flame_spd_mismatch))
		debugPrintf("[DOSSIER] verify: flame fire_spd reads %u cached, %u differ\n", dossier_flame_spd_cached,
			dossier_flame_spd_mismatch);
	// The burn is over once no circle has stepped for 2 s
	if (burn_ticks && burn_last_circle_tick && !burn_reported && sceKernelGetProcessTimeWide() - burn_last_circle_us > 2000000) {
		burn_reported = 1;
		uint32_t ticks = burn_last_circle_tick;
		double secs = (burn_last_circle_us - burn_first_us) / 1e6;
		debugPrintf("[DOSSIER] burn: %u ticks in %.2f s (%.1f ticks/s) | interval mean %.1f ms, max %.1f ms, worst 30 ticks "
			"%.0f ms (%.1f ticks/s) | over 34 ms %u, over 50 ms %u | first Steps native %u, stamp loops native %u fallback %u\n",
			ticks, secs, secs > 0 ? (ticks - 1) / secs : 0.0, ticks > 1 ? secs * 1000.0 / (ticks - 1) : 0.0,
			burn_max_us / 1000.0, burn_worst_window_us / 1000.0, burn_worst_window_us ? 30e6 / burn_worst_window_us : 0.0,
			burn_over34, burn_over50, dossier_first_native, dossier_draw_native, dossier_draw_fallback);
	}
}
