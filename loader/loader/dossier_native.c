/*
 * Dossier burn (room_folder). APK 1.10 libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * GML lifts: work/decomp/out/gml_Object_obj_burncircle*.gml, gml_Object_obj_folder_cutscene_*.gml.
 *
 * The burn spreads through obj_burncircle_spawner cells; each new cell makes a burn circle, which makes three flames
 * and six decorative child circles, and the cutscene stamps every live circle into its burn mask each frame. At the
 * peak ~330 cells, ~720 circles and ~200 flames are alive and a tick took ~55 ms (14-17 fps, work/t30/burn_dyn); with
 * the changes below ~32 ms (30 fps in the 30 fps modes, burn_new; the original behaviour: burn_orig).
 *
 * 1. Native Steps (exact). Most Step calls take one short branch:
 *    obj_burncircle, once `spawn` is true (a main circle after its first Step):
 *        if (grow_spd > 0.5) grow_spd -= 0.1; size += grow_spd * obj_folder_cutscene.fire_spd;
 *        if (size > max_size) instance_destroy();
 *    obj_burncircle_spawner: a burned-out barrier does nothing; a spreading cell does
 *        timer += 1 * obj_folder_cutscene.fire_spd and only spreads once timer > timer_max.
 *    These run natively with the runner's BOOL_RValue and YYCompareVal (g_GMLMathEpsilon, as the compiled events) in
 *    the same double arithmetic; anything else runs the original event. obj_folder_cutscene.fire_spd is read from the
 *    cutscene instance directly (see fire_spd).
 * 2. Burned-out cells skip their cleanup search. Every 31 Steps (and 5 Steps in a row once it succeeds) a burned-out
 *    cell enlarges its mask to 100x100 and runs 8 place_meeting + instance_place queries to delete itself when
 *    surrounded by burned-out cells: ~10 ms per tick at the peak. Cells are invisible, burned-out cells never spread
 *    again, and the cutscene ends when every cell is burned out. With the search skipped the spread is the same: the
 *    same 1107 burn circles and 108029 circle Steps in all (burn_skip against burn_nat3). exp_dossiercleanup keeps it.
 * 3. Fewer decorative circles (dossierCircles, see create_depth).
 * exp_dossiernative0 turns all three off.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <kubridge.h>
#include "main.h"
#include "so_util.h"

typedef struct {
	double val;
	uint32_t flags;
	uint32_t kind;
} rv;

#define KIND_REAL 0
#define OBJ_FOLDER_CUTSCENE 458 // object indices in Katana ZERO 1.1.10 (0x1ca in the compiled Step)
#define OBJ_BURNCIRCLE 92

typedef void (*event_fn)(void *self, void *other);

static event_fn orig_circle_step, orig_spawner_step, orig_cutscene_create, orig_cutscene_cleanup;
static int (*yy_bool)(const rv *v);
static int (*yy_compare)(const rv *a, const rv *b, double eps, int);
static void (*yy_get_value)(const rv *inst, int var, int index, rv *out, int, int);
static void (*yy_destroy)(void *self, void *other, int argc, void **args);
static const double *math_epsilon;
static int *var_spawn, *var_grow_spd, *var_size, *var_max_size, *var_fire_spd;
static int *var_make_fire, *var_burnt_out, *var_barrier, *var_timer, *var_kill_timer, *var_timer_max;
static int *current_room;
static int skip_cleanup; // 2. in the header; exp_dossiercleanup keeps the original search

unsigned dossier_circle_fast, dossier_circle_slow, dossier_spawner_fast, dossier_spawner_slow;

static rv *vget(void *self, int *var) {
	rv *(*getter)(void *, int) = (*(void ***)self)[2];
	return getter(self, var[1]);
}

static int compare(const rv *a, double b) {
	rv c = {b, 0, KIND_REAL};
	return yy_compare(a, &c, *math_epsilon, 1);
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
		rv *v = vget(live, var_fire_spd);
		if (v && v->kind == KIND_REAL) {
			dossier_spd_cached++;
			*out = v->val;
			return 1;
		}
	}
	rv obj = {0}, spd = {0};
	uint32_t ref[2] = {OBJ_FOLDER_CUTSCENE, 0x01000000};
	memcpy(&obj.val, ref, sizeof(ref));
	obj.kind = 15;
	yy_get_value(&obj, var_fire_spd[1], (int)0x80000000, &spd, 0, 0);
	dossier_spd_looked_up++;
	*out = spd.val;
	return spd.kind == KIND_REAL; // nothing to free: only strings, arrays and structs are counted
}

// Decorative circles (dossierCircles in yyl.cfg: keep one in N, 0 = none, 1 = all). A main circle's first Step makes
// six children around itself:
//     repeat (6) { r = random(1); bb = instance_create_depth(x + lengthdir_x(r * 3, r * 360), ..., obj_burncircle);
//                  bb.max_size = r * 25 + 5; bb.spawn = true; }
// They sit within 3 px of the parent, never spread or make flames, and only add their stamp to the burn mask, where
// the parent's own (radius 30, the largest) covers all but a 3 px rim. Creating one costs ~80 us (creation and its
// Create event), ~40 of them per frame at the burn peak, plus their Steps and mask stamps. A dropped child is never
// created: instance_create_depth (wrapped in the function registry while a circle's first Step runs) draws the
// child's random(1.5) from its Create, so the random sequence, and with it the spread and the cutscene's length, is
// unchanged, and returns the parent's id. The two writes through bb then name the parent, which the same Step tests
// next (size > max_size); the two YYGML_Variable_SetValue calls of those lines go through child_set_value (veneers
// on their bl), which drops a write aimed at the parent while its first Step runs: a kept child has its own id, and
// the parent's own writes (spawn = true after the loop) are not bb.* lines.
int dossier_circles = 0;
unsigned dossier_children_dropped, dossier_children_kept, dossier_child_writes_dropped;
static void *first_step_parent;
static void (*orig_create_depth)(rv *result, void *self, void *other, int argc, rv *args);
static double (*yy_random)(double range);
static void (*gv_id)(void *inst, int index, rv *out);

static void create_depth(rv *result, void *self, void *other, int argc, rv *args) {
	static unsigned children;
	if (first_step_parent && self == first_step_parent && argc == 4) {
		uint32_t ref[2];
		memcpy(ref, &args[3].val, sizeof(ref));
		int object = args[3].kind == 15 ? (int)ref[0] : args[3].kind == KIND_REAL ? (int)args[3].val : -1; // reference or index
		if (object == OBJ_BURNCIRCLE && (dossier_circles <= 0 || children++ % dossier_circles)) {
			yy_random(1.5); // the child's Create: grow_spd = random(1.5) + 0.5
			gv_id(self, (int)0x80000000, result);
			dossier_children_dropped++;
			return;
		}
		if (object == OBJ_BURNCIRCLE)
			dossier_children_kept++;
	}
	orig_create_depth(result, self, other, argc, args);
}

// bb.max_size = ... and bb.spawn = true, the YYGML_Variable_SetValue(const YYRValue &inst, var, index, RValue *)
// calls of the children's loop. bb is the id create_depth returned: a number or an instance reference (kind 15).
static void (*yy_set_value)(const rv *inst, int var, int index, rv *value);
static void child_set_value(const rv *inst, int var, int index, rv *value) {
	if (first_step_parent && inst) {
		uint32_t w[2];
		memcpy(w, &inst->val, sizeof(w));
		uint32_t id = inst->kind == KIND_REAL ? (uint32_t)inst->val : inst->kind == 15 ? w[0] : 0xffffffffu;
		if (id == *(uint32_t *)((char *)first_step_parent + 0x80)) {
			dossier_child_writes_dropped++;
			return; // a dropped child's write
		}
	}
	yy_set_value(inst, var, index, value);
}

static int child_writes_patched;

static void circle_first_step(void *self, void *other) {
	if (!orig_create_depth || !child_writes_patched || dossier_circles == 1) {
		orig_circle_step(self, other);
		return;
	}
	void *outer = first_step_parent;
	first_step_parent = self;
	orig_circle_step(self, other);
	first_step_parent = outer;
}

// The two bl sites (APK 1.1.10 ELF addresses), each to the PLT entry or, in a patch_plt_calls.py build, straight to
// the function: an 8-byte veneer near each (ldr pc, [pc, #-4]; .word child_set_value) and the bl turned to it.
// Both are checked before either is written.
static int patch_child_writes(uintptr_t circle) {
	enum { EVENT = 0x364f9c0, MAX_SIZE_SITE = 0x3651264, SPAWN_SITE = 0x3651334, PLT_SET_VALUE = 0x4ab1140 };
	uintptr_t sites[2] = {circle + MAX_SIZE_SITE - EVENT, circle + SPAWN_SITE - EVENT};
	uintptr_t plt = circle + PLT_SET_VALUE - EVENT, direct = (uintptr_t)yy_set_value;
	uint32_t *veneers[2];
	for (int i = 0; i < 2; i++) {
		uint32_t ins = *(uint32_t *)sites[i];
		uintptr_t target = sites[i] + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
		if ((ins >> 24) != 0xeb || (target != plt && target != direct))
			return 0;
		veneers[i] = (uint32_t *)so_alloc_near(&yoyoloader_mod, sites[i], 8);
		int32_t off = veneers[i] ? (int32_t)((uintptr_t)veneers[i] - (sites[i] + 8)) : 0;
		if (!veneers[i] || off < -(1 << 25) || off >= (1 << 25))
			return 0;
	}
	for (int i = 0; i < 2; i++) {
		uint32_t code[2] = {0xe51ff004, (uint32_t)(uintptr_t)child_set_value};
		kuKernelCpuUnrestrictedMemcpy(veneers[i], code, sizeof(code));
		kuKernelFlushCaches(veneers[i], sizeof(code));
		int32_t off = (int32_t)((uintptr_t)veneers[i] - (sites[i] + 8));
		uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
		kuKernelCpuUnrestrictedMemcpy((void *)sites[i], &bl, 4);
		kuKernelFlushCaches((void *)sites[i], 4);
	}
	return 1;
}

static void circle_step(void *self, void *other) {
	rv *spawn = vget(self, var_spawn);
	if (!spawn || !yy_bool(spawn))
		goto slow;
	rv *grow = vget(self, var_grow_spd), *size = vget(self, var_size), *max = vget(self, var_max_size);
	if (!grow || !size || !max || grow->kind != KIND_REAL || size->kind != KIND_REAL || max->kind != KIND_REAL)
		goto slow;
	double spd;
	if (!fire_spd(&spd))
		goto slow;
	if (compare(grow, 0.5) >= 1)
		grow->val -= 0.1;
	double step = grow->val * spd;
	size->val += step;
	dossier_circle_fast++;
	if (compare(size, max->val) >= 1)
		yy_destroy(self, other, 0, NULL);
	return;
slow:
	dossier_circle_slow++;
	circle_first_step(self, other);
}

static void spawner_step(void *self, void *other) {
	extern void t30_dossier_burning(void);
	t30_dossier_burning(); // the 30 fps modes switch to 30 ticks/s during the burn

	rv *make_fire = vget(self, var_make_fire), *burnt_out = vget(self, var_burnt_out);
	if (!make_fire || !burnt_out || yy_bool(make_fire))
		goto slow;
	if (!yy_bool(burnt_out)) {
		// Spreading cell: timer += 1 * fire_spd; it spreads (the original runs) once timer > timer_max
		rv *timer = vget(self, var_timer), *timer_max = vget(self, var_timer_max);
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
		return;
	}
	rv *barrier = vget(self, var_barrier);
	if (!barrier)
		goto slow;
	if (yy_bool(barrier)) {
		dossier_spawner_fast++;
		return;
	}
	if (skip_cleanup) {
		dossier_spawner_fast++;
		return;
	}
	// Burned-out cell: timer += 1; the cleanup search (the original) runs once timer > 30 or while kill_timer > 0
	rv *timer = vget(self, var_timer), *kill = vget(self, var_kill_timer);
	if (!timer || !kill || timer->kind != KIND_REAL)
		goto slow;
	rv next = *timer;
	next.val += 1.0;
	if (compare(&next, 30.0) > 0 || compare(kill, 0.0) >= 1)
		goto slow; // the cleanup search; the original also does the increment
	timer->val = next.val;
	dossier_spawner_fast++;
	return;
slow:
	dossier_spawner_slow++;
	orig_spawner_step(self, other);
}

int dossier_native_install(void *(*entry_hook)(uintptr_t entry, void *replacement)) {
#define SYM(name) so_symbol(&yoyoloader_mod, name)
	yy_bool = (void *)SYM("_Z11BOOL_RValuePK6RValue");
	yy_compare = (void *)SYM("_Z12YYCompareValRK6RValueS1_db");
	yy_get_value = (void *)SYM("_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	yy_destroy = (void *)SYM("_Z22YYGML_instance_destroyP9CInstanceS0_iPP8YYRValue");
	math_epsilon = (const double *)SYM("g_GMLMathEpsilon");
	var_spawn = (int *)SYM("g_VAR_spawn");
	var_grow_spd = (int *)SYM("g_VAR_grow_spd");
	var_size = (int *)SYM("g_VAR_size");
	var_max_size = (int *)SYM("g_VAR_max_size");
	var_fire_spd = (int *)SYM("g_VAR_fire_spd");
	var_make_fire = (int *)SYM("g_VAR_make_fire");
	var_burnt_out = (int *)SYM("g_VAR_burnt_out");
	var_barrier = (int *)SYM("g_VAR_barrier");
	var_timer = (int *)SYM("g_VAR_timer");
	var_kill_timer = (int *)SYM("g_VAR_kill_timer");
	var_timer_max = (int *)SYM("g_VAR_timer_max");
	current_room = (int *)SYM("Current_Room");
	id_hash = (const uint32_t *)SYM("_ZN9CInstance14ms_ID2InstanceE");
	yy_set_value = (void *)SYM("_Z23YYGML_Variable_SetValueRK8YYRValueiiP6RValue");
	uintptr_t create = SYM("_Z39gml_Object_obj_folder_cutscene_Create_0P9CInstanceS0_");
	uintptr_t cleanup = SYM("_Z40gml_Object_obj_folder_cutscene_CleanUp_0P9CInstanceS0_");
	uintptr_t circle = SYM("_Z32gml_Object_obj_burncircle_Step_0P9CInstanceS0_");
	uintptr_t spawner = SYM("_Z40gml_Object_obj_burncircle_spawner_Step_0P9CInstanceS0_");
#undef SYM
	if (!yy_bool || !yy_compare || !yy_get_value || !yy_destroy || !math_epsilon || !circle || !spawner || !create ||
			!cleanup || !current_room || !id_hash)
		return 0;
	if (!var_spawn || !var_grow_spd || !var_size || !var_max_size || !var_fire_spd || !var_make_fire ||
			!var_burnt_out || !var_barrier || !var_timer || !var_kill_timer || !var_timer_max)
		return 0;
	skip_cleanup = !exp_flag("dossiercleanup");
	yy_random = (void *)so_symbol(&yoyoloader_mod, "_Z12YYGML_randomd");
	gv_id = (void *)so_symbol(&yoyoloader_mod, "_Z5GV_IdP9CInstanceiP6RValue");
	typedef struct { const char *name; void *func; int argc; } runner_function;
	runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	// Children are dropped only with their writes dropped too (the sites are patched before the prologue hook)
	child_writes_patched = dossier_circles != 1 && yy_random && gv_id && yy_set_value && patch_child_writes(circle);
	for (int i = 0; child_writes_patched && functions && *functions && count && i < *count; i++) {
		runner_function *f = &(*functions)[i];
		if (f->name && !strcmp(f->name, "instance_create_depth")) {
			orig_create_depth = f->func;
			f->func = create_depth;
			break;
		}
	}
	if (!(orig_cutscene_cleanup = entry_hook(cleanup, cutscene_cleanup)) ||
			!(orig_cutscene_create = entry_hook(create, cutscene_create)))
		return 0;
	orig_circle_step = entry_hook(circle, circle_step);
	orig_spawner_step = entry_hook(spawner, spawner_step);
	return orig_circle_step && orig_spawner_step;
}

// Debug (profiling report): the cutscene's fire_spd, when the cutscene instance is known and alive
int dossier_fire_spd(double *out) {
	return live_cutscene() && cutscene_room >= 0 && fire_spd(out);
}
