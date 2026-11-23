/*
 * region_native.c
 *
 * gml_Script_update_region (APK 1.1.10) looks up which lighting region an instance stands in:
 *
 *     var prev = region;
 *     if (instance_exists(obj_room_seed)) {
 *         with (obj_room_seed) if (room == my_room) {
 *             oth_x = min(other.x, room_width);
 *             oth_y = min(other.y, room_height);
 *             r = ds_grid_get(regionmap, max(0, ceil(oth_x / 32)), max(0, ceil((oth_y - 64) / 32)));
 *             if (r != 0) other.region = r;
 *             else {
 *                 chq = 32;   // then the four neighbours, the first nonzero one wins:
 *                 //  ((oth_x + chq) / 32, (oth_y - 64) / 32), ((oth_x - chq) / 32, (oth_y - 64) / 32),
 *                 //  (oth_x / 32, (oth_y + chq - 64) / 32), (oth_x / 32, (oth_y - chq - 64) / 32)
 *             }
 *         }
 *     } else region = 0;
 *     if (prev != region) update_light_timer = 0;
 *
 * Every particle that moved (and every explosion particle, whose region stays unset) calls it from its draw event:
 * in the bike chase's helicopter volleys 120-270 calls per frame at ~21 us, 2.6-5.6 ms of the frame. The compiled
 * script spends that on RValue plumbing; the native version below makes the same runner calls (instance_exists, the
 * with iterator, the room/room_width/room_height builtins, YYGML_min, ceil, ds_grid_get, YYCompareVal) with the same
 * arguments and in the same order, does the arithmetic in doubles in the same order (this file is built with
 * -ffp-contract=off), and writes the same slots the same way: full RValue copies for oth_x, oth_y and region = r, value
 * and kind only (flags kept) for chq = 32, region = 0 and update_light_timer = 0.
 *
 * It handles one obj_room_seed instance with real values throughout; anything else (more seeds, a non-real value
 * before the first write) runs the original script from the start. Writes made before such a fallback (oth_x, oth_y,
 * chq) are ones the original makes again with the same values before reading them, and region is only written as the
 * last step, so the original still sees the state it would have seen.
 *
 * exp_regionnative0 keeps the original. exp_regionverify runs the native version without writing (recording what it
 * would write), then the original, and compares the five slots it can touch (self.region, self.update_light_timer,
 * the seed's oth_x, oth_y, chq) byte for byte ([REGION] report).
 */
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef void ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);
typedef RV *ABI (*var_fn)(void *instance, int id);
typedef void ABI (*getvalue_fn)(const RV *instance, int id, int index, RV *out, int, int);
typedef int ABI (*builtin_get_fn)(void *instance, int id, int index, RV *out);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double epsilon, int throw_error);
typedef void ABI (*builtin_fn)(RV *result, void *self, void *other, int argc, RV *args);
typedef RV *ABI (*minmax_fn)(RV *result, int argc, RV **args);
typedef RV *ABI (*grid_get_fn)(RV *result, int grid, int x, int y);
typedef int ABI (*int32_fn)(const RV *value);
typedef void ABI (*free_fn)(RV *value);
typedef int ABI (*new_fn)(void *iterator, void **self, void **other, const RV *target);
typedef int ABI (*next_fn)(void *iterator, void **self, void **other);
typedef void ABI (*delete_fn)(void *iterator, void **self, void **other);

static script_fn orig_update_region;
static getvalue_fn variable_get;
static builtin_get_fn builtin_get;
static compare_fn compare;
static builtin_fn instance_exists;
static minmax_fn yy_min;
static grid_get_fn grid_get;
static int32_fn int32_rvalue;
static free_fn free_rvalue;
static new_fn new_with;
static next_fn next_with;
static delete_fn delete_with;
static const double *epsilon;
static int (*object_find)(const char *name);
static const int *v_region, *v_room, *v_my_room, *v_oth_x, *v_oth_y, *v_x, *v_y, *v_room_width, *v_room_height,
	*v_regionmap, *v_chq, *v_update_light_timer;
static int verify, room_seed = -2;

unsigned region_native_calls, region_native_fallback, region_verify_ok, region_verify_mismatch, region_verify_skipped;
unsigned region_fallback_reason[8];
unsigned region_verify_paths[32]; // verify: calls per set of slots written (oth_x 1, oth_y 2, chq 4, region 8, timer 16)

#define KIND(v) ((v)->kind & 0xffffff)
#define NEEDS_FREE(v) ((0x46u >> (KIND(v) & 31)) & 1 && KIND(v) < 32)

static RV *rd(void *inst, const int *desc) { return (*(var_fn *)(*(uint8_t **)inst + 8))(inst, desc[1]); }
static RV *wr(void *inst, const int *desc) { return (*(var_fn *)(*(uint8_t **)inst + 0xc))(inst, desc[1]); }

// What the native version writes (verify mode records instead of writing)
enum { W_OTH_X, W_OTH_Y, W_CHQ, W_REGION, W_TIMER, W_COUNT };
typedef struct { int dry; int set[W_COUNT]; RV val[W_COUNT]; int value_kind_only[W_COUNT]; void *seed; } writes;

// The original's assignment of a whole RValue (real) to a slot: free the old value if it needs it, then copy
static void put_full(writes *w, int k, void *inst, const int *desc, const RV *v) {
	w->set[k] = 1;
	w->val[k] = *v;
	w->value_kind_only[k] = 0;
	if (w->dry)
		return;
	RV *slot = wr(inst, desc);
	if (NEEDS_FREE(slot))
		free_rvalue(slot);
	*slot = *v;
}

// A constant real: value and kind written, flags kept
static void put_real(writes *w, int k, void *inst, const int *desc, double d) {
	w->set[k] = 1;
	w->val[k] = (RV){d, 0, 0};
	w->value_kind_only[k] = 1;
	if (w->dry)
		return;
	RV *slot = wr(inst, desc);
	if (NEEDS_FREE(slot))
		free_rvalue(slot);
	slot->value = d;
	slot->kind = 0;
}

static int fail(int reason) {
	region_fallback_reason[reason & 7]++;
	return 0;
}

// One grid probe: r = ds_grid_get(regionmap, max(0, ceil(gx)), max(0, ceil(gy))) with gx, gy the quotients
static int probe(void *seed, double qx, double qy, RV *r) {
	RV *map = rd(seed, v_regionmap);
	if (!map)
		return fail(5);
	int grid = int32_rvalue(map);
	double cx = ceil(qx), cy = ceil(qy);
	double mx = cx > 0.0 ? cx : 0.0, my = cy > 0.0 ? cy : 0.0; // YYGML_max(0, v): v only if v > 0
	*r = (RV){0, 0, 0xffffff};
	grid_get(r, grid, (int)(int64_t)mx, (int)(int64_t)my);
	return KIND(r) == 0 ? 1 : fail(6);
}

// 1: done (writes made or recorded), 0: the original must run (nothing written that it wouldn't write again first)
static int region_native(void *self, void *other, writes *w) {
	if (!self || v_region[1] < 0 || v_update_light_timer[1] < 0 || v_oth_x[1] < 0 || v_regionmap[1] < 0)
		return fail(0);
	if (room_seed == -2)
		room_seed = object_find("obj_room_seed");
	if (room_seed < 0)
		return fail(0);
	RV *reg = rd(self, v_region);
	if (!reg || KIND(reg) != 0)
		return fail(1);
	RV prev = *reg;
	RV target = {0, 0, 15};
	uint32_t ref[2] = {(uint32_t)room_seed, 0x1000000}; // an object reference, as the script builds it
	memcpy(&target.value, ref, 8);
	RV exists = {0, 0, 0xffffff};
	instance_exists(&exists, self, other, 1, &target);
	if (KIND(&exists) != 0 && KIND(&exists) != 13) // a real or a bool; BOOL_RValue of either is value > 0.5
		return fail(2);
	if (!(exists.value > 0.5)) {
		put_real(w, W_REGION, self, v_region, 0.0);
	} else {
		uint32_t it[4];
		void *s = self, *o = other;
		int n = new_with(it, &s, &o, &target);
		// exactly one seed: the first instance in *self and an empty list below it (list[0] is the NULL sentinel)
		if (n > 0 && (it[3] - it[2]) / 4 != 1)
			n = 2;
		if (n != 1) {
			if (n > 0)
				delete_with(it, &s, &o);
			return fail(3);
		}
		void *seed = s;
		w->seed = seed;
		RV roomv = {0, 0, 0xffffff};
		builtin_get(seed, v_room[1], (int)0x80000000, &roomv);
		RV *my = rd(seed, v_my_room);
		int ok = my != NULL;
		if (ok && compare(&roomv, my, *epsilon, 0) == 0) {
			RV inst = {0, 0, 6}, xv = {0, 0, 0xffffff}, yv = {0, 0, 0xffffff}, wv = {0, 0, 0xffffff}, hv = {0, 0, 0xffffff};
			memcpy(&inst.value, &self, sizeof(self));
			variable_get(&inst, v_x[1], (int)0x80000000, &xv, 0, 0);
			builtin_get(seed, v_room_width[1], (int)0x80000000, &wv);
			variable_get(&inst, v_y[1], (int)0x80000000, &yv, 0, 0);
			builtin_get(seed, v_room_height[1], (int)0x80000000, &hv);
			if (KIND(&xv) || KIND(&yv) || KIND(&wv) || KIND(&hv)) {
				delete_with(it, &s, &o);
				return fail(4);
			}
			RV mxo = {0, 0, 0xffffff}, myo = {0, 0, 0xffffff}, *ax[2] = {&xv, &wv}, *ay[2] = {&yv, &hv};
			RV *ox = yy_min(&mxo, 2, ax), *oy = yy_min(&myo, 2, ay);
			if (KIND(ox) || KIND(oy)) {
				delete_with(it, &s, &o);
				return fail(4);
			}
			put_full(w, W_OTH_X, seed, v_oth_x, ox);
			put_full(w, W_OTH_Y, seed, v_oth_y, oy);
			double X = ox->value, Y = oy->value, chq = 32.0;
			RV r, zero = {0, 0, 0};
			if (!probe(seed, X / 32.0, (Y - 64.0) / 32.0, &r)) {
				delete_with(it, &s, &o);
				return 0;
			}
			if (compare(&r, &zero, *epsilon, 0) != 0)
				put_full(w, W_REGION, self, v_region, &r);
			else {
				put_real(w, W_CHQ, seed, v_chq, chq);
				const double qx[4] = {(X + chq) / 32.0, (X - chq) / 32.0, X / 32.0, X / 32.0};
				const double qy[4] = {(Y - 64.0) / 32.0, (Y - 64.0) / 32.0, (Y + chq - 64.0) / 32.0, (Y - chq - 64.0) / 32.0};
				for (int k = 0; k < 4; k++) {
					if (!probe(seed, qx[k], qy[k], &r)) {
						delete_with(it, &s, &o);
						return 0;
					}
					if (compare(&r, &zero, *epsilon, 0) != 0) {
						put_full(w, W_REGION, self, v_region, &r);
						break;
					}
				}
			}
		} else if (!ok) {
			delete_with(it, &s, &o);
			return fail(4);
		}
		delete_with(it, &s, &o);
	}
	// if (prev != region) update_light_timer = 0;
	RV now = w->set[W_REGION] ? w->val[W_REGION] : *reg;
	if (w->dry && w->set[W_REGION] && w->value_kind_only[W_REGION]) {
		now = *reg;
		now.value = w->val[W_REGION].value;
		now.kind = 0;
	}
	if (compare(&prev, w->dry ? &now : rd(self, v_region), *epsilon, 0) != 0)
		put_real(w, W_TIMER, self, v_update_light_timer, 0.0);
	return 1;
}

static void ABI native_update_region(void *self, void *other, RV *result, int argc, RV **args) {
	region_native_calls++;
	if (verify) {
		writes w;
		memset(&w, 0, sizeof(w));
		w.dry = 1;
		if (!region_native(self, other, &w)) {
			region_verify_skipped++;
			orig_update_region(self, other, result, argc, args);
			return;
		}
		// The slots before the original runs
		void *seed = w.seed;
		RV *slots[W_COUNT] = {seed ? rd(seed, v_oth_x) : NULL, seed ? rd(seed, v_oth_y) : NULL, seed ? rd(seed, v_chq) : NULL,
			rd(self, v_region), rd(self, v_update_light_timer)};
		RV before[W_COUNT];
		for (int k = 0; k < W_COUNT; k++)
			if (slots[k])
				before[k] = *slots[k];
		orig_update_region(self, other, result, argc, args);
		int bad = 0;
		for (int k = 0; k < W_COUNT; k++) {
			RV *after = k <= W_CHQ ? (seed ? (k == W_OTH_X ? rd(seed, v_oth_x) : k == W_OTH_Y ? rd(seed, v_oth_y) : rd(seed, v_chq)) : NULL)
				: (k == W_REGION ? rd(self, v_region) : rd(self, v_update_light_timer));
			if (!w.set[k]) {
				if (slots[k] && (!after || memcmp(after, &before[k], sizeof(RV))))
					bad = 1;
				continue;
			}
			RV want = w.val[k];
			if (w.value_kind_only[k] && slots[k])
				want.flags = before[k].flags;
			if (!after || memcmp(&after->value, &want.value, 8) || KIND(after) != KIND(&want) ||
					(slots[k] && after->flags != want.flags))
				bad = 1;
		}
		region_verify_paths[w.set[0] | w.set[1] << 1 | w.set[2] << 2 | w.set[3] << 3 | w.set[4] << 4]++;
		if (bad) {
			if (region_verify_mismatch++ < 8)
				debugPrintf("[REGION] verify mismatch (set %d %d %d %d %d)\n", w.set[0], w.set[1], w.set[2], w.set[3], w.set[4]);
		} else
			region_verify_ok++;
		return;
	}
	writes w;
	memset(&w, 0, sizeof(w));
	if (region_native(self, other, &w)) {
		uint32_t *res = (uint32_t *)result; // the script's result: undefined (low word 0, kind 5), as its prologue sets it
		res[0] = 0;
		result->kind = 5;
		return;
	}
	region_native_fallback++;
	orig_update_region(self, other, result, argc, args);
}

int region_native_install(void *(*hook)(uintptr_t entry, void *replacement)) {
#define SYM(name) so_symbol(&yoyoloader_mod, name)
	uintptr_t entry = SYM("_Z24gml_Script_update_regionP9CInstanceS0_R8YYRValueiPPS1_");
	variable_get = (getvalue_fn)SYM("_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	builtin_get = (builtin_get_fn)SYM("_Z26Variable_GetBuiltIn_DirectP12YYObjectBaseiiP6RValue");
	compare = (compare_fn)SYM("_Z12YYCompareValRK6RValueS1_db");
	instance_exists = (builtin_fn)SYM("_Z16F_InstanceExistsR6RValueP9CInstanceS2_iPS_");
	yy_min = (minmax_fn)SYM("_Z9YYGML_minR8YYRValueiPPS_");
	grid_get = (grid_get_fn)SYM("_Z17YYGML_ds_grid_getR8YYRValueiii");
	int32_rvalue = (int32_fn)SYM("_Z12INT32_RValuePK6RValue");
	free_rvalue = (free_fn)SYM("_Z16FREE_RValue__PreP6RValue");
	new_with = (new_fn)SYM("_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	next_with = (next_fn)SYM("_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
	delete_with = (delete_fn)SYM("_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_");
	epsilon = (const double *)SYM("g_GMLMathEpsilon");
	object_find = (int (*)(const char *))SYM("_Z11Object_FindPKc");
	v_region = (const int *)SYM("g_VAR_region");
	v_room = (const int *)SYM("g_VAR_room");
	v_my_room = (const int *)SYM("g_VAR_my_room");
	v_oth_x = (const int *)SYM("g_VAR_oth_x");
	v_oth_y = (const int *)SYM("g_VAR_oth_y");
	v_x = (const int *)SYM("g_VAR_x");
	v_y = (const int *)SYM("g_VAR_y");
	v_room_width = (const int *)SYM("g_VAR_room_width");
	v_room_height = (const int *)SYM("g_VAR_room_height");
	v_regionmap = (const int *)SYM("g_VAR_regionmap");
	v_chq = (const int *)SYM("g_VAR_chq");
	v_update_light_timer = (const int *)SYM("g_VAR_update_light_timer");
#undef SYM
	if (!entry || !variable_get || !builtin_get || !compare || !instance_exists || !yy_min || !grid_get || !int32_rvalue ||
			!free_rvalue || !new_with || !next_with || !delete_with || !epsilon || !object_find || !v_region || !v_room ||
			!v_my_room || !v_oth_x || !v_oth_y || !v_x || !v_y || !v_room_width || !v_room_height || !v_regionmap || !v_chq ||
			!v_update_light_timer)
		return 0;
	verify = exp_flag("regionverify");
	orig_update_region = (script_fn)hook(entry, (void *)native_update_region);
	if (!orig_update_region)
		return 0;
	debugPrintf("update_region: native%s\n", verify ? " (verify mode)" : "");
	return 1;
}
