/*
 * psych_ground.c
 *
 * Native Draw for obj_psychboss_ground, the giant's floor in the last part of the secret Psychiatrist fight (part 5,
 * 159 slices). The Draw event (gml_Object_obj_psychboss_ground_Draw_0, APK 1.1.10) is, from the ARM listing:
 *
 *   var stab = false, stabx = -1000;
 *   with (obj_psychboss_giant) if (attack == 3 && step == 3) { stabx = stabber_x; stab = true; }
 *   var fr = global.fr, xx = x, i = 0;
 *   repeat (slices) {
 *       var pressed = false;
 *       if (instance_exists(obj_player) && abs(obj_player.x - xx) < 64 && obj_player.y > y - 8 && obj_player.vsp >= 0
 *               && place_meeting(xx, y - 2, obj_player)) pressed = true;
 *       if (stab && abs(stabx - xx) < 32) pressure[i] = 18;
 *       else if (pressed) { if (pressure[i] < 10) pressure[i] += abs(pressure[i] - 10) * 0.5 * fr; }
 *       else {
 *           if (i < slices - 1 && pressure[i + 1] * 0.8 > pressure[i] + 0.05) pressure[i] = pressure[i + 1] * 0.8;
 *           if (i > 0 && pressure[i - 1] * 0.8 > pressure[i] + 0.05) pressure[i] = pressure[i - 1] * 0.8;
 *       }
 *       if (pressure[i] > 0) { pressure[i] -= max(abs(pressure[i]) / 5, 0.15) * fr; if (pressure[i] < 0) pressure[i] = 0; }
 *       draw_sprite_part_ext(sprite_index, 0, i * slice_width, 0, slice_width, image_height, xx, y + pressure[i] - 10,
 *           2, 2, c_white, 1);
 *       xx += slice_width * 2; i += 1;
 *   }
 *
 * (the "< 64" and "< 32" tests are `abs(..) + -64 < -epsilon` in doubles; the others go through YYCompareVal). It costs
 * 2.3-2.6 ms per frame on the Vita, of which the 159 draws are 0.86 ms: the rest is variable lookups, RValue
 * temporaries and per-slice instance_exists / obj_player reads.
 *
 * The native does the same in doubles in the same order, looks the player up once per Draw (instance_exists and the
 * variable reads have no side effects and nothing in the loop moves the player), keeps every place_meeting call (they
 * can update the collision tree) and every draw (through YYGML_CallLegacyFunction with the same 12 arguments, the
 * constants being the event's own gs_constArg values), and writes the pressure elements directly: kind 0, flags
 * untouched, except `pressure[i] = pressure[i +- 1] * 0.8`, which copies the neighbour's flags as the operator does.
 * Anything unexpected (a value that isn't a real, a NaN or infinity, a pressure array that ARRAY_LVAL_RValue would copy,
 * grow or refuse, a slices count that isn't a whole number) runs the original event, before anything is written.
 * exp_psychground0 keeps the original; exp_psychgroundverify (debug) runs the native with the draws recorded instead
 * of submitted, restores the pressure array, runs the original with its draws and place_meeting calls recorded, and
 * compares the arrays and both call lists bit for bit ([PSYCHGROUND] report line). exp_psychgroundfault=1 (draw y +
 * 0.001) / =2 (neighbour factor 0.81) are negative controls for the verifier.
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

#define ABI __attribute__((pcs("aapcs")))
typedef gml_rvalue *ABI (*legacy_fn)(void *self, void *other, gml_rvalue *result, int argc, int funcid, gml_rvalue **argv);
typedef int ABI (*compare_fn)(const gml_rvalue *a, const gml_rvalue *b, double eps, int throw_error);
typedef int ABI (*bool_fn)(const gml_rvalue *v);
typedef void ABI (*getvalue_fn)(const gml_rvalue *target, int var, int index, gml_rvalue *out, int a, int b);
typedef void ABI (*errcheck_fn)(const gml_rvalue *target, int var, int index, gml_rvalue *out, void *self, void *other);
typedef int ABI (*new_with_fn)(void *it, void **self, void **other, const gml_rvalue *target);
typedef int ABI (*next_with_fn)(void *it, void **self, void **other);
typedef void ABI (*delete_with_fn)(void *it, void **self, void **other);

#define OBJ_PLAYER 745
#define OBJ_GIANT 823
#define MAX_SLICES 1024

static event_func orig_ground_draw;
static legacy_fn legacy;
static compare_fn compare;
static bool_fn bool_rvalue;
static getvalue_fn get_value;
static errcheck_fn errcheck_get_value;
static new_with_fn new_with;
static next_with_fn next_with;
static delete_with_fn delete_with;
static double *epsilon;
static const uint8_t *cow_enabled;
static gml_rvalue *const_arg[5]; // gs_constArg0..4_3C7ECC56: 0.15, 0, 2, c_white, 1
static int *v_fr, *v_x, *v_y, *v_slices, *v_pressure, *v_slice_width, *v_image_height, *v_sprite_index, *v_vsp,
	*v_attack, *v_step, *v_stabber_x;
static int f_instance_exists = -1, f_place_meeting = -1, f_draw_part = -1; // set once the runner has registered them
static const int *g_func_ids[3];
static int verify, fault, ab_skip;
static unsigned g_native, g_fallback, g_slices, g_verified, g_mismatch, g_fallback_line, g_meets;
static unsigned c_stab, c_pressed, c_rise, c_spread, c_decay, c_zero; // verify: branch coverage (total)

#define KIND(v) ((v)->kind & 0xffffff)
#define FAIL() do { g_fallback_line = __LINE__; goto fallback; } while (0)

// YYCompareVal for two reals: d = a - b; 0 if |d| <= epsilon, else the sign. NaN never gets here (inputs checked).
static inline int cmp(double a, double b) {
	double d = a - b;
	if (fabs(d) <= *epsilon)
		return 0;
	return d < 0 ? -1 : 1;
}

static inline int finite_real(const gml_rvalue *v) {
	return v && KIND(v) == 0 && isfinite(v->val);
}

static inline gml_rvalue real_rv(double d) {
	gml_rvalue v = {d, 0, 0};
	return v;
}

static inline gml_rvalue obj_ref(int obj) {
	gml_rvalue v;
	uint32_t w[4] = {(uint32_t)obj, 0x1000000, 0, 15};
	memcpy(&v, w, sizeof(v));
	return v;
}

// ---------------------------------------------------------------------------------------------------------------
// Verify: recorded calls

typedef struct { gml_rvalue a[12]; } draw_rec;
typedef struct { gml_rvalue a[3]; uint32_t result; } meet_rec;
typedef struct { draw_rec draw[MAX_SLICES]; meet_rec meet[64]; int ndraw, nmeet, overflow; } call_log;
static call_log logs[2], *cur_log;
static int submit_draws = 1;
static builtin_fn real_place_meeting, real_draw_part;
static runner_function *rf_place_meeting, *rf_draw_part;

static void rec_place_meeting(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	real_place_meeting(result, self, other, argc, args);
	if (cur_log && argc == 3) {
		if (cur_log->nmeet < 64) {
			meet_rec *m = &cur_log->meet[cur_log->nmeet++];
			memcpy(m->a, args, sizeof(m->a));
			m->result = result->val > 0.5;
		} else
			cur_log->overflow = 1;
	}
}

static void rec_draw_part(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (cur_log && argc == 12) {
		if (cur_log->ndraw < MAX_SLICES)
			memcpy(cur_log->draw[cur_log->ndraw++].a, args, sizeof(draw_rec));
		else
			cur_log->overflow = 1;
	}
	if (submit_draws)
		real_draw_part(result, self, other, argc, args);
}

static int same_rv(const gml_rvalue *a, const gml_rvalue *b) {
	// value bits and kind; flags of temporaries are whatever the stack held
	return !memcmp(&a->val, &b->val, 8) && KIND(a) == KIND(b);
}

// ---------------------------------------------------------------------------------------------------------------

static int native_draw(void *self, void *other) {
	if (f_instance_exists < 0 || f_place_meeting < 0 || f_draw_part < 0)
		FAIL();
	// with (obj_psychboss_giant): only reads
	int stab = 0;
	double stabx = -1000.0;
	{
		uint32_t it[16] = {0};
		void *s = self, *o = other;
		gml_rvalue target = obj_ref(OBJ_GIANT), three = real_rv(3.0);
		int stab_ok = 1;
		if (new_with(it, &s, &o, &target) > 0) {
			do {
				if (compare(gml_vget(s, v_attack[1]), &three, *epsilon, 0) == 0 &&
						compare(gml_vget(s, v_step[1]), &three, *epsilon, 0) == 0) {
					gml_rvalue *sx = gml_vget(s, v_stabber_x[1]);
					if (finite_real(sx))
						stabx = sx->val;
					else
						stab_ok = 0;
					stab = 1;
				}
			} while (next_with(it, &s, &o));
		}
		delete_with(it, &s, &o);
		if (!stab_ok)
			FAIL();
	}
	gml_rvalue *rfr = gml_vget(*global_obj_ptr, v_fr[1]);
	if (!finite_real(rfr))
		FAIL();
	double fr = rfr->val;
	gml_rvalue self_rv = {0, 0, 6}, gx = {0, 0, 5}, gy = {0, 0, 5}, spr = {0, 0, 5};
	memcpy(&self_rv.val, &self, sizeof(self));
	get_value(&self_rv, v_x[1], (int)0x80000000, &gx, 0, 0);
	get_value(&self_rv, v_y[1], (int)0x80000000, &gy, 0, 0);
	get_value(&self_rv, v_sprite_index[1], (int)0x80000000, &spr, 0, 0);
	if (!finite_real(&gx) || !finite_real(&gy) || ((1u << (spr.kind & 31)) & 0x46))
		FAIL();
	gml_rvalue *rslices = gml_vget(self, v_slices[1]);
	gml_rvalue *rsw = gml_vget(self, v_slice_width[1]);
	gml_rvalue *rih = gml_vget(self, v_image_height[1]);
	gml_rvalue *rp = gml_vget(self, v_pressure[1]);
	if (!finite_real(rslices) || !finite_real(rsw) || !finite_real(rih) || !rp || KIND(rp) != 2)
		FAIL();
	double slices = rslices->val;
	if (slices != floor(slices) || slices < 0 || slices > MAX_SLICES)
		FAIL();
	int n = (int)slices;
	uint8_t *arr = *(uint8_t *__attribute__((may_alias)) *)&rp->val; // the array pointer (may_alias: no punning)
	if (!arr)
		FAIL();
	int len = *(int *)(arr + 0x1c);
	gml_rvalue *p = *(gml_rvalue **)(arr + 4);
	if (len < n || (n && !p) || (arr[0x14] & 1) || (*cow_enabled && *(int *)(arr + 0x10) != 1))
		FAIL();
	for (int k = 0; k < n; k++)
		if (!finite_real(&p[k]))
			FAIL();
	// The player, as the loop reads it (instance_exists, then obj_player.x / .y through the error-checking getter and
	// .vsp through the plain one)
	gml_rvalue player = obj_ref(OBJ_PLAYER), res = {0, 0, 5};
	gml_rvalue *argv1[1] = {&player};
	int exists = bool_rvalue(legacy(self, other, &res, 1, f_instance_exists, argv1));
	double px = 0, py = 0, pvsp = 0;
	if (exists) {
		gml_rvalue a = {0, 0, 5}, b = {0, 0, 5}, c = {0, 0, 5};
		errcheck_get_value(&player, v_x[1], (int)0x80000000, &a, self, other);
		errcheck_get_value(&player, v_y[1], (int)0x80000000, &b, self, other);
		get_value(&player, v_vsp[1], (int)0x80000000, &c, 0, 0);
		if (!finite_real(&a) || !finite_real(&b) || !finite_real(&c))
			FAIL();
		px = a.val;
		py = b.val;
		pvsp = c.val;
	}

	// The loop. Nothing above wrote anything.
	const double eps = *epsilon, sy = gy.val, sw = rsw->val, near = fault == 2 ? 0.81 : 0.8;
	double xx = gx.val, i = 0.0;
	gml_rvalue a_xx, a_left, a_y, a_sw = *rsw, a_ih = *rih, a_spr = spr;
	gml_rvalue *dargv[12] = {&a_spr, const_arg[1], &a_left, const_arg[1], &a_sw, &a_ih, &a_xx, &a_y, const_arg[2],
		const_arg[2], const_arg[3], const_arg[4]};
	for (int k = 0; k < n; k++) {
		int pressed = 0;
		if (exists && fabs(px - xx) + -64.0 < -eps && cmp(py, sy - 8.0) > 0 && cmp(pvsp, 0.0) >= 0) {
			gml_rvalue m0 = real_rv(xx), m1 = real_rv(sy - 2.0), m2 = obj_ref(OBJ_PLAYER), r = {0, 0, 5};
			gml_rvalue *margv[3] = {&m0, &m1, &m2};
			if (bool_rvalue(legacy(self, other, &r, 3, f_place_meeting, margv)))
				pressed = 1;
		}
		if (stab && fabs(stabx - xx) + -32.0 < -eps) {
			p[k].val = 18.0;
			p[k].kind = 0;
			c_stab++;
		} else if (pressed) {
			c_pressed++;
			if (cmp(p[k].val, 10.0) < 0) {
				p[k].val = p[k].val + fabs(p[k].val - 10.0) * 0.5 * fr;
				c_rise++;
			}
		} else {
			if (cmp(i, slices - 1.0) < 0 && cmp(p[k + 1].val * near, p[k].val + 0.05) > 0) {
				p[k].val = p[k + 1].val * near;
				p[k].flags = p[k + 1].flags;
				c_spread++;
			}
			if (cmp(i, 0.0) > 0 && cmp(p[k - 1].val * near, p[k].val + 0.05) > 0) {
				p[k].val = p[k - 1].val * near;
				p[k].flags = p[k - 1].flags;
				c_spread++;
			}
		}
		if (cmp(p[k].val, 0.0) > 0) {
			double m = fabs(p[k].val) / 5.0;
			if (const_arg[0]->val > m) // max(m, 0.15): the first argument unless the second is larger
				m = const_arg[0]->val;
			p[k].val = p[k].val - m * fr;
			c_decay++;
			if (cmp(p[k].val, 0.0) < 0) {
				p[k].val = 0.0;
				c_zero++;
			}
		}
		a_left = real_rv(i * sw);
		a_xx = real_rv(xx);
		a_y = real_rv((sy + p[k].val) - 10.0);
		if (fault == 1)
			a_y.val += 0.001;
		gml_rvalue r = {0, 0, 5};
		legacy(self, other, &r, 12, f_draw_part, dargv);
		xx = xx + sw * 2.0;
		i = i + 1.0;
	}
	g_slices += n;
	return 1;
fallback:
	return 0;
}

static gml_rvalue snap[MAX_SLICES + 1], after_native[MAX_SLICES + 1];

static void ground_draw(void *self, void *other) {
	if (ab_skip) {
		orig_ground_draw(self, other);
		return;
	}
	if (!verify) {
		if (native_draw(self, other))
			g_native++;
		else {
			g_fallback++;
			orig_ground_draw(self, other);
		}
		return;
	}
	// Verify: native (draws recorded only), restore the pressure array, original (calls recorded), compare
	gml_rvalue *rp = gml_vget(self, v_pressure[1]);
	uint8_t *arr = rp && KIND(rp) == 2 ? *(uint8_t *__attribute__((may_alias)) *)&rp->val : NULL;
	int len = arr ? *(int *)(arr + 0x1c) : 0;
	gml_rvalue *p = arr ? *(gml_rvalue **)(arr + 4) : NULL;
	if (!p || len > MAX_SLICES + 1) {
		g_fallback++;
		orig_ground_draw(self, other);
		return;
	}
	memcpy(snap, p, len * sizeof(gml_rvalue));
	rf_place_meeting->func = rec_place_meeting;
	rf_draw_part->func = rec_draw_part;
	memset(&logs[0].ndraw, 0, 3 * sizeof(int));
	memset(&logs[1].ndraw, 0, 3 * sizeof(int));
	cur_log = &logs[0];
	submit_draws = 0;
	int ok = native_draw(self, other);
	submit_draws = 1;
	cur_log = NULL;
	if (!ok) {
		rf_place_meeting->func = real_place_meeting;
		rf_draw_part->func = real_draw_part;
		g_fallback++;
		orig_ground_draw(self, other);
		return;
	}
	g_native++;
	memcpy(after_native, p, len * sizeof(gml_rvalue));
	memcpy(p, snap, len * sizeof(gml_rvalue));
	cur_log = &logs[1];
	orig_ground_draw(self, other);
	cur_log = NULL;
	rf_place_meeting->func = real_place_meeting;
	rf_draw_part->func = real_draw_part;
	// The original may have replaced the array (it shouldn't): compare against what the variable holds now
	rp = gml_vget(self, v_pressure[1]);
	uint8_t *arr2 = rp && KIND(rp) == 2 ? *(uint8_t *__attribute__((may_alias)) *)&rp->val : NULL;
	gml_rvalue *p2 = arr2 ? *(gml_rvalue **)(arr2 + 4) : NULL;
	int len2 = arr2 ? *(int *)(arr2 + 0x1c) : -1;
	int bad = 0;
	if (arr2 != arr || p2 != p || len2 != len) {
		bad++;
		if (g_mismatch < 20)
			debugPrintf("[PSYCHGROUND] verify: pressure array changed (%p %p %d -> %p %p %d)\n", arr, p, len, arr2, p2, len2);
	} else {
		for (int k = 0; k < len; k++)
			if (memcmp(&p[k].val, &after_native[k].val, 8) || p[k].kind != after_native[k].kind ||
					p[k].flags != after_native[k].flags) {
				if (g_mismatch + bad < 20)
					debugPrintf("[PSYCHGROUND] verify: pressure[%d] native %.17g/%x/%x original %.17g/%x/%x\n", k,
						after_native[k].val, after_native[k].flags, after_native[k].kind, p[k].val, p[k].flags, p[k].kind);
				bad++;
			}
	}
	call_log *a = &logs[0], *b = &logs[1];
	if (a->overflow || b->overflow || a->ndraw != b->ndraw || a->nmeet != b->nmeet) {
		if (g_mismatch + bad < 20)
			debugPrintf("[PSYCHGROUND] verify: calls native %d draws %d place_meeting, original %d / %d (overflow %d %d)\n",
				a->ndraw, a->nmeet, b->ndraw, b->nmeet, a->overflow, b->overflow);
		bad++;
	} else {
		for (int k = 0; k < a->ndraw; k++)
			for (int j = 0; j < 12; j++)
				if (!same_rv(&a->draw[k].a[j], &b->draw[k].a[j])) {
					if (g_mismatch + bad < 20)
						debugPrintf("[PSYCHGROUND] verify: draw %d arg %d native %.17g/%x original %.17g/%x\n", k, j,
							a->draw[k].a[j].val, a->draw[k].a[j].kind, b->draw[k].a[j].val, b->draw[k].a[j].kind);
					bad++;
				}
		for (int k = 0; k < a->nmeet; k++) {
			int diff = a->meet[k].result != b->meet[k].result;
			for (int j = 0; j < 3; j++)
				diff |= !same_rv(&a->meet[k].a[j], &b->meet[k].a[j]);
			if (diff) {
				if (g_mismatch + bad < 20)
					debugPrintf("[PSYCHGROUND] verify: place_meeting %d differs (x %.17g/%.17g y %.17g/%.17g result %u/%u)\n",
						k, a->meet[k].a[0].val, b->meet[k].a[0].val, a->meet[k].a[1].val, b->meet[k].a[1].val,
						a->meet[k].result, b->meet[k].result);
				bad++;
			}
		}
	}
	g_verified++;
	g_mismatch += bad;
	g_meets += a->nmeet;
}

void psych_ground_report(int frames, int ab_off) {
	ab_skip = ab_off; // A/B (psych_fix.c): the next window runs the original
	if (!debugMode || !(g_native + g_fallback))
		return;
	debugPrintf("[PSYCHGROUND] native %u, original %u (last reject line %u), %.0f slices per native Draw", g_native,
		g_fallback, g_fallback_line, g_native ? (double)g_slices / g_native : 0.0);
	if (verify)
		debugPrintf(" | verify: %u Draws compared (%u place_meeting calls; slices stabbed %u, pressed %u, rising %u, "
			"spread %u, decaying %u, clamped %u), %u mismatches (total)%s\n", g_verified, g_meets, c_stab, c_pressed, c_rise,
			c_spread, c_decay, c_zero, g_mismatch, fault ? " FAULT INJECTED" : "");
	else
		debugPrintf("\n");
	g_native = g_fallback = g_slices = 0;
}

// patch_performance: the event table entry
int psych_ground_install(gml_func_entry *table) {
#define SYM(n) so_symbol(&yoyoloader_mod, n)
	legacy = (legacy_fn)SYM("_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	compare = (compare_fn)SYM("_Z12YYCompareValRK6RValueS1_db");
	bool_rvalue = (bool_fn)SYM("_Z11BOOL_RValuePK6RValue");
	get_value = (getvalue_fn)SYM("_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	errcheck_get_value = (errcheck_fn)SYM("_Z32YYGML_ErrCheck_Variable_GetValueRK8YYRValueiiP6RValueP9CInstanceS5_");
	new_with = (new_with_fn)SYM("_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	next_with = (next_with_fn)SYM("_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
	delete_with = (delete_with_fn)SYM("_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_");
	epsilon = (double *)SYM("g_GMLMathEpsilon");
	cow_enabled = (const uint8_t *)SYM("g_fCopyOnWriteEnabled");
	static const char *const consts[5] = {"gs_constArg0_3C7ECC56", "gs_constArg1_3C7ECC56", "gs_constArg2_3C7ECC56",
		"gs_constArg3_3C7ECC56", "gs_constArg4_3C7ECC56"};
	for (int k = 0; k < 5; k++)
		if (!(const_arg[k] = (gml_rvalue *)SYM(consts[k])))
			return 0;
	struct { int **v; const char *n; } vars[] = {
		{&v_fr, "g_VAR_fr"}, {&v_x, "g_VAR_x"}, {&v_y, "g_VAR_y"}, {&v_slices, "g_VAR_slices"},
		{&v_pressure, "g_VAR_pressure"}, {&v_slice_width, "g_VAR_slice_width"}, {&v_image_height, "g_VAR_image_height"},
		{&v_sprite_index, "g_VAR_sprite_index"}, {&v_vsp, "g_VAR_vsp"}, {&v_attack, "g_VAR_attack"},
		{&v_step, "g_VAR_step"}, {&v_stabber_x, "g_VAR_stabber_x"},
	};
	for (unsigned k = 0; k < sizeof(vars) / sizeof(vars[0]); k++)
		if (!(*vars[k].v = (int *)SYM(vars[k].n)))
			return 0;
	const int *fe = (const int *)SYM("g_FUNC_instance_exists"), *fp = (const int *)SYM("g_FUNC_place_meeting"),
		*fd = (const int *)SYM("g_FUNC_draw_sprite_part_ext");
	if (!legacy || !compare || !bool_rvalue || !get_value || !errcheck_get_value || !new_with || !next_with ||
			!delete_with || !epsilon || !cow_enabled || !fe || !fp || !fd || !global_obj_ptr)
		return 0;
	g_func_ids[0] = fe;
	g_func_ids[1] = fp;
	g_func_ids[2] = fd;
	// The event's constants: 0.15, 0, 2, c_white, 1 (gs_constArg1 lives in .bss, filled when the runner starts)
	if (const_arg[0]->kind != 0 || const_arg[0]->val != 0.15 || const_arg[1]->kind != 0 || const_arg[1]->val != 0.0 ||
			const_arg[2]->val != 2.0 || const_arg[3]->val != 16777215.0 || const_arg[4]->val != 1.0) {
		debugPrintf("[PSYCHGROUND] unexpected constants\n");
		return 0;
	}
	verify = debugMode && exp_flag("psychgroundverify");
	event_func o = gml_replace(table, "gml_Object_obj_psychboss_ground_Draw_0", ground_draw);
	if (!o)
		return 0;
	orig_ground_draw = o;
	return 1;
}

// katana_late_install: the verify mode's call recorders need the runner's function registry
void psych_ground_late_install(void) {
	if (!g_func_ids[0])
		return;
	f_instance_exists = g_func_ids[0][1];
	f_place_meeting = g_func_ids[1][1];
	f_draw_part = g_func_ids[2][1];
	if (!verify)
		return;
	{
		fault = exp_flag("psychgroundfault") ? 1 : 0;
		FILE *f = fault ? fopen("ux0:data/gms/shared/exp_psychgroundfault", "r") : NULL;
		if (f) {
			char t[4] = {0};
			fread(t, 1, 3, f);
			fclose(f);
			fault = t[0] == '2' ? 2 : 1;
		}
		runner_function **functions = (runner_function **)SYM("the_functions");
		int *count = (int *)SYM("the_numb");
		for (int k = 0; functions && *functions && count && k < *count; k++) {
			runner_function *f2 = &(*functions)[k];
			if (k == f_place_meeting)
				rf_place_meeting = f2;
			else if (k == f_draw_part)
				rf_draw_part = f2;
		}
		if (!rf_place_meeting || !rf_draw_part || strcmp(rf_place_meeting->name, "place_meeting") ||
				strcmp(rf_draw_part->name, "draw_sprite_part_ext")) {
			debugPrintf("[PSYCHGROUND] verify: call recorders NOT installed\n");
			verify = 0;
			return;
		}
		real_place_meeting = rf_place_meeting->func;
		real_draw_part = rf_draw_part->func;
		debugPrintf("[PSYCHGROUND] verify on%s\n", fault ? " (fault injected)" : "");
	}
}
