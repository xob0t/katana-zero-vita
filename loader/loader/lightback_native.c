/*
 * lightback_native.c
 *
 * Exact native of obj_lightback user event 0 (APK 1.1.10, 0x42e8fd0), which obj_new_platform_lighting's Draw runs
 * for every light (`with (obj_lightback) event_user(0)`). From the ARM listing:
 *
 *   x = parent.x;                    // Variable_SetValue_Direct -> SV_X -> SetPosition
 *   y = parent.y;
 *   var r = parent.radius / 400;
 *   var a = 1;
 *   var xv = global.xview, yv = global.yview, wv = global.wview, hv = global.hview;
 *   if (x > xv - 200 && x < xv + wv + 200 && y > yv - 200 && y < yv + hv + 200) {
 *       if (global.has_bloodmap) a = (0.75 + global.light_power / 4) * alpha * sphere_alpha / 4;
 *       else                     a = (0.75 + global.light_power / 4) * alpha * sphere_alpha;
 *       draw_sprite_stretched_ext(parent.half_sphere ? sprite 0x43f : sprite 0x440, 0, x - 200 * r, y - 200 * r,
 *                                 400 * r, 400 * r, parent.color, a);   // YYGML_CallLegacyFunction
 *   }
 *
 * room_haunted_cart2 runs it for 129 lights per frame (1.5 ms). The frame audit (Nov 25) put the event's own code at
 * 0.26-0.48 ms per frame in the six heavy rooms after the off-screen part below: mostly the visible lights' RValue
 * temporaries and operator calls. The on-screen native (in-run A/B, exp_lbvisab, whole event timed per call, fights
 * with exp_autowalk) cuts the event per frame by 0.14-0.29 ms in those rooms (9-22 on-screen lights, ~14 us each):
 * club_4_2 0.585 -> 0.336, haunted_alien2 0.561 -> 0.391, mansion_2_2 0.608 -> 0.409, bunker2_3 0.751 -> 0.543,
 * chinatown_4 0.579 -> 0.292, mansion_3 0.432 -> 0.298 ms. What remains is the draw call itself.
 *
 * The native reads the same values the original reads, without calling anything that could raise an error or run
 * code: the parent instance is resolved like Variable_GetValue(int id) resolves it (CInstance's id hash, instance
 * flags & 0x100001 clear), x / y come from the instance's floats as GV_X / GV_Y return them, parent.radius,
 * parent.half_sphere and parent.color are the instance's own variable slots (InternalReadYYVar) and must be plain
 * reals. It predicts the position the two writes leave (SV_X stores (float) of the double it gets; parent.x is the
 * double of a float, so the light gets the parent's float) and evaluates the cull with the original order,
 * association and comparison (YYCompareVal for two reals, live epsilon).
 *
 * Off-screen: the two writes through Variable_SetValue_Direct, in order, with the values the original passes.
 * On-screen: the light's alpha and sphere_alpha (read through the instance getter, as the original reads them),
 * global.has_bloodmap and global.light_power must be reals too; then the two writes, the arithmetic in doubles in the
 * compiled order (built with -ffp-contract=off; YYRValue's operators on two reals are plain double operations),
 * BOOL_RValue for the two tests, x / y read back from the light, and the same draw_sprite_stretched_ext call through
 * YYGML_CallLegacyFunction with the same eight arguments (the sprite as a kind-15 sprite reference, the subimage the
 * game's own constant RValue, parent.color copied whole). Anything else (non-real views or values, a -1/-2 or
 * object-index parent, a missing or deactivated instance, a struct parent, a missing variable) runs the original
 * event before anything has been written. Nothing in the event draws a random number.
 *
 * The original re-reads `parent` before each parent read; nothing between them can change it (the x write is
 * SV_X -> CInstance::SetPosition on the light itself), so one read is the same value.
 *
 * exp_lightbacknative0 keeps the original event; exp_lbvisible0 keeps it for on-screen lights only. exp_lbverify
 * (debugMode only): for each light the native handled, records the runner-level writes (Variable_SetValue_Direct calls)
 * and the draw_sprite_stretched_ext call the native made (the native's draw is recorded, not submitted), then runs the
 * original and records its writes and draws. Both write sequences must match (object, variable, index, value), the
 * draw lists must match (count; every argument's kind and value; flags words are counted apart, since the original
 * passes stack temporaries' uninitialised flags), and the light's position, flags and collision-list links must be
 * unchanged by the original (it re-applies the same values, so SetPosition sees no change). The original's write values
 * don't depend on the light's own position, so its sequence is the one it would issue from the pre-event state too.
 * exp_lbverifyfault (with exp_lbverify) adds 1e-9 to the native's alpha argument as a negative control.
 * exp_lbab (measurement only) switches the native off and on every report window, so one run gives an A/B;
 * exp_lbvisab does the same for the on-screen path only. Both also time the whole event per call (a [LIGHTBACK]
 * event time line per window).
 */
#include <string.h>
#include "main.h"
#include <stdint.h>
#include <math.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#define ABI __attribute__((pcs("aapcs")))
typedef struct __attribute__((packed, aligned(4))) {
	union { double d; uint64_t bits; uint32_t w[2]; } v;
	uint32_t flags, kind;
} RV;
typedef void ABI (*Event)(void *, void *);
typedef int ABI (*Compare)(const RV *, const RV *, double, int);
typedef RV *ABI (*GetVar)(void *, int);
typedef RV *ABI (*ReadVar)(void *, int);
typedef int ABI (*VarSet)(void *, int, int, RV *);
typedef int ABI (*Int32)(const RV *);
typedef int ABI (*BoolFn)(const RV *);
typedef RV *ABI (*Legacy)(void *self, void *other, RV *result, int argc, int funcid, RV **argv);
typedef void ABI (*FreeFn)(RV *);

#define ARRAY_NONE ((int)0x80000000)
#define REFCOUNTED(k) ((1u << ((k) & 31)) & 0x46u) // string, array, object

unsigned lightback_native_culled, lightback_native_visible, lightback_native_drawn, lightback_native_fallback;
unsigned lightback_verify_calls, lightback_verify_mismatch, lightback_verify_draws;
static unsigned lb_verify_visible, lb_verify_flags, lb_verify_reason[6];
uint32_t lightback_native_us;
static int lb_verify, lb_timing, lb_ab, lb_ab_off, lb_visible = 1, lb_visab, lb_vis_off, lb_fault;
static unsigned lb_reject_line[12], lb_reject_count[12];

static Event original;
static Compare compare;
static ReadVar read_var;
static VarSet var_set;
static unsigned reject_kind;
static Int32 int32_value;
static BoolFn yy_bool;
static Legacy legacy;
static FreeFn yy_free;
static const RV *subimage; // gs_constArg0_50DB0133: the event's constant 0
static const int *stretched_fid; // g_FUNC_draw_sprite_stretched_ext: {?, id}
static void **global;
static double *epsilon;
static const uint32_t *id_hash; // CInstance::ms_ID2Instance: {buckets, mask}
static uint8_t *instance_not_found;
static const int *start_globals; // g_nStartGlobalVariables: ids below it (and <= 9999) are builtins
enum { XVIEW, YVIEW, WVIEW, HVIEW, BLOODMAP, POWER, PARENT, VX, VY, RADIUS, ALPHA, SPHERE_ALPHA, HALF_SPHERE, COLOR, NV };
static const char *const var_names[NV] = {
	"g_VAR_xview", "g_VAR_yview", "g_VAR_wview", "g_VAR_hview", "g_VAR_has_bloodmap", "g_VAR_light_power",
	"g_VAR_parent", "g_VAR_x", "g_VAR_y", "g_VAR_radius", "g_VAR_alpha", "g_VAR_sphere_alpha", "g_VAR_half_sphere",
	"g_VAR_color" };
static const int *vars[NV];

// YYCompareVal for two reals (0xe3ee1c, 0xe3f31c): d = a - b; -1 if d < 0, else 1; 0 if |d| <= epsilon.
// A NaN difference (infinities) takes the runner's own special case.
static inline int compare_real(double a, double b) {
	double d = a - b, eps = *epsilon;
	if (d != d) {
		RV x, y;
		x.v.d = a; x.flags = 0; x.kind = 0;
		y.v.d = b; y.flags = 0; y.kind = 0;
		return compare(&x, &y, eps, 1);
	}
	int r = d < 0 ? -1 : 1;
	return fabs(d) <= eps ? 0 : r;
}

static void reject(unsigned line) {
	lightback_native_fallback++;
	for (int i = 0; i < 12; i++) {
		if (lb_reject_line[i] == line || !lb_reject_line[i]) {
			lb_reject_line[i] = line;
			lb_reject_count[i]++;
			return;
		}
	}
}

static inline int real(const RV *v) { return (v->kind & 0xffffff) == 0; }
static inline int truth_kind(const RV *v) {
	unsigned k = v->kind & 0xffffff;
	return k == 0 || k == 7 || k == 10 || k == 13;
}
static inline int id_of(int n) { return vars[n][1]; } // g_VAR_* ids are filled at game start: read them live

// x > lo && x < (a + b) + m, the original's two-compare test of one axis (YYCompareVal, live epsilon)
static int inside_axis(double pos, const RV *view, const RV *size) {
	if (compare_real(pos, view->v.d - 200.0) < 1)
		return 0;
	int r = compare_real(pos, (view->v.d + size->v.d) + 200.0);
	return r != -2 && r < 0;
}

// The instance Variable_GetValue(int id, ...) reads for an instance id (0xf896ac): the id hash, then flags
// 0x100001 clear. NULL where the original would report "instance not found".
static void *instance_by_id(int id) {
	const uint32_t *buckets = (const uint32_t *)(uintptr_t)id_hash[0];
	if (!buckets)
		return NULL;
	uintptr_t node = buckets[(id_hash[1] & (uint32_t)id) * 2];
	for (unsigned trips = 0; node && trips < 4096; trips++) {
		if (*(int *)(node + 8) == id) {
			void *inst = *(void **)(node + 0xc);
			return inst && !(*(uint32_t *)((char *)inst + 0x7c) & 0x100001) ? inst : NULL;
		}
		node = *(uintptr_t *)(node + 4);
	}
	return NULL;
}

// Verify: the native's draw is recorded here instead of drawn (lb_suppress), the original's recorded and drawn
typedef struct { RV args[8]; int argc; } Draw;
static Draw draw_log[2];
static int draw_count, draw_logging, draw_suppress;

// The on-screen light. Everything is read and checked before the first write; returns 0 (original) if anything
// isn't what the native expects.
static int visible(void *self, void *other, void *g, GetVar get, GetVar self_get, void *inst, int by_id, double x,
	double y) {
	if (id_of(ALPHA) <= 9999 || id_of(SPHERE_ALPHA) <= 9999 || id_of(HALF_SPHERE) <= 9999 || id_of(COLOR) <= 9999)
		return reject(__LINE__), 0;
	RV *radius = read_var(inst, id_of(RADIUS));
	if (!radius || !real(radius))
		return reject(__LINE__), 0;
	RV *bloodmap = get(g, id_of(BLOODMAP));
	RV *power = get(g, id_of(POWER));
	// has_bloodmap only goes through BOOL_RValue: a real, int32, int64 or bool (true / false are kind 13)
	if (!bloodmap || !power || !real(power) || !truth_kind(bloodmap)) {
		reject_kind = (bloodmap ? bloodmap->kind & 0xffffff : 0xff) << 8 | (power ? power->kind & 0xff : 0xff);
		return reject(__LINE__), 0;
	}
	// The original reads these through the instance getter too (it creates a missing variable, as the original's
	// read would; the original then runs and finds it)
	RV *alpha = self_get(self, id_of(ALPHA));
	RV *sphere = self_get(self, id_of(SPHERE_ALPHA));
	if (!alpha || !sphere || !real(alpha) || !real(sphere)) {
		reject_kind = (alpha ? alpha->kind & 0xffffff : 0xff) << 8 | (sphere ? sphere->kind & 0xff : 0xff);
		return reject(__LINE__), 0;
	}
	RV *half = read_var(inst, id_of(HALF_SPHERE));
	RV *color = read_var(inst, id_of(COLOR));
	if (!half || !color || !truth_kind(half) || !real(color) || !real(subimage)) {
		reject_kind = (half ? half->kind & 0xffffff : 0xff) << 8 | (color ? color->kind & 0xff : 0xff);
		return reject(__LINE__), 0;
	}
	if (by_id)
		*instance_not_found = 0;
	RV wx = { .v.d = x, .flags = 0, .kind = 0 }, wy = { .v.d = y, .flags = 0, .kind = 0 };
	var_set(self, id_of(VX), ARRAY_NONE, &wx);
	var_set(self, id_of(VY), ARRAY_NONE, &wy);
	double r = radius->v.d / 400.0;
	double a = power->v.d / 4.0;
	a = 0.75 + a;
	a = a * alpha->v.d;
	a = a * sphere->v.d;
	if (yy_bool(bloodmap))
		a = a / 4.0;
	int sprite = yy_bool(half) ? 0x43f : 0x440;
	double sx = (double)*(float *)((char *)self + 0xb8), sy = (double)*(float *)((char *)self + 0xbc);
	double offset = 200.0 * r, size = 400.0 * r;
	RV args[8];
	memset(args, 0, sizeof(args));
	args[0].v.w[0] = (uint32_t)sprite;
	args[0].v.w[1] = 0x1000001;
	args[0].kind = 15;
	args[1] = *subimage;
	args[2].v.d = sx - offset;
	args[3].v.d = sy - offset;
	args[4].v.d = size;
	args[5].v.d = size;
	args[6] = *color;
	args[7].v.d = lb_fault ? a + 1e-9 : a;
	RV *argv[8] = { &args[0], &args[1], &args[2], &args[3], &args[4], &args[5], &args[6], &args[7] };
	RV result = { .v.d = 0, .flags = 0, .kind = 5 };
	legacy(self, other, &result, 8, stretched_fid[1], argv);
	if (REFCOUNTED(result.kind & 0xffffff))
		yy_free(&result);
	lightback_native_drawn++;
	return 1;
}

// Returns 1 if the event was handled, 0 to run the original. Nothing before the two writes has an effect: getters
// of existing variables, the id hash and the instances' own fields.
static int handle(void *self, void *other) {
	if (!self || *(uint32_t *)((char *)self + 0x50) != 1)
		return reject(__LINE__), 0;
	// x / y are builtins (GV_X / GV_Y); radius an instance variable (InternalReadYYVar in Variable_GetValue_Direct)
	if (id_of(VX) > 9999 || id_of(VY) > 9999 || id_of(VX) >= *start_globals || id_of(VY) >= *start_globals ||
		id_of(RADIUS) <= 9999)
		return reject(__LINE__), 0;
	void *g = *global;
	if (!g)
		return reject(__LINE__), 0;
	GetVar get = (*(GetVar **)g)[2];
	RV *vx = get(g, id_of(XVIEW));
	RV *vy = get(g, id_of(YVIEW));
	RV *vw = get(g, id_of(WVIEW));
	RV *vh = get(g, id_of(HVIEW));
	if (!vx || !vy || !vw || !vh || !real(vx) || !real(vy) || !real(vw) || !real(vh))
		return reject(__LINE__), 0;
	GetVar self_get = (*(GetVar **)self)[2];
	RV *parent = self_get(self, id_of(PARENT));
	if (!parent)
		return reject(__LINE__), 0;
	// parent.x goes through YYGML_ErrCheck_Variable_GetValue: kinds real/int32/int64/bool/ref (mask 0xa481) take
	// INT32_RValue and Variable_GetValue(id) (-1 self, -2 other, < 100000 an object index); kind 6 reads the
	// object directly (Variable_GetValue_Direct), which for an instance dispatches the builtin getter.
	unsigned kind = parent->kind & 0xffffff;
	void *inst;
	int by_id = 0;
	if (kind <= 15 && ((1u << kind) & 0xa481)) {
		int id = int32_value(parent);
		if (id <= 99999)
			return reject(__LINE__), 0;
		if (!(inst = instance_by_id(id)))
			return reject(__LINE__), 0;
		by_id = 1;
	} else if (kind == 6 && parent->v.w[0]) {
		inst = (void *)(uintptr_t)parent->v.w[0];
	} else {
		reject_kind = kind;
		return reject(__LINE__), 0;
	}
	if (*(uint32_t *)((char *)inst + 0x50) != 1) { // a struct: its properties could run code
		reject_kind = kind;
		return reject(__LINE__), 0;
	}
	float fx = *(float *)((char *)inst + 0xb8), fy = *(float *)((char *)inst + 0xbc);
	// SV_X / SV_Y store (float) of the double GV_X / GV_Y returned: the parent's floats; line 14 reads them back
	double x = (double)fx, y = (double)fy;
	if (inside_axis(x, vx, vw) && inside_axis(y, vy, vh)) {
		if (!lb_visible || lb_vis_off) {
			lightback_native_visible++;
			return 0;
		}
		return visible(self, other, g, get, self_get, inst, by_id, x, y);
	}
	// Off-screen: the original also reads has_bloodmap, light_power (globals) and parent.radius / 400. A radius
	// that isn't there or isn't a plain real (an error or a conversion in the original) runs the original.
	RV *radius = read_var(inst, id_of(RADIUS));
	if (!radius || !real(radius))
		return reject(__LINE__), 0;
	(void)get(g, id_of(BLOODMAP));
	(void)get(g, id_of(POWER));
	if (by_id)
		*instance_not_found = 0; // Variable_GetValue(int id) clears it on every lookup
	RV wx = { .v.d = x, .flags = 0, .kind = 0 }, wy = { .v.d = y, .flags = 0, .kind = 0 };
	var_set(self, id_of(VX), ARRAY_NONE, &wx);
	var_set(self, id_of(VY), ARRAY_NONE, &wy);
	lightback_native_culled++;
	return 1;
}

// draw_sprite_stretched_ext calls, counted (and in verify, recorded) by the verify wrapper
static void *orig_stretched;
typedef void ABI (*Builtin)(RV *, void *, void *, int, RV *);
static unsigned stretched_count;
static void ABI counted_stretched(RV *out, void *self, void *other, int argc, RV *args) {
	if (draw_logging) {
		if (draw_count < 2) {
			Draw *d = &draw_log[draw_count];
			d->argc = argc;
			memset(d->args, 0, sizeof(d->args));
			memcpy(d->args, args, (argc > 0 && argc <= 8 ? argc : 0) * sizeof(RV));
		}
		draw_count++;
		if (draw_suppress)
			return;
	}
	stretched_count++;
	((Builtin)orig_stretched)(out, self, other, argc, args);
}

// Verify: Variable_SetValue_Direct calls logged while a path runs (entry hook, exp_lbverify only)
typedef struct { void *obj; int var, index; uint32_t kind; uint64_t bits; } Write;
static VarSet orig_set;
static Write write_log[8];
static int write_count, write_logging;
static int ABI logged_set(void *obj, int var, int index, RV *value) {
	if (write_logging) {
		if (write_count < 8) {
			Write w = { obj, var, index, value ? value->kind & 0xffffffu : 0xffffffffu, value ? value->v.bits : 0 };
			write_log[write_count] = w;
		}
		write_count++;
	}
	return orig_set(obj, var, index, value);
}

// Position, flags and both collision-list links (CollisionMarkDirty: +0x15c..+0x170)
typedef struct { uint32_t words[2], flags, links[6]; } State;
static void snapshot(void *self, State *s) {
	memcpy(s->words, (char *)self + 0xb8, 8);
	s->flags = *(uint32_t *)((char *)self + 0x7c);
	memcpy(s->links, (char *)self + 0x15c, sizeof(s->links));
}

// Kind and value of every argument; the flags words are compared apart
static int draws_differ(const Draw *a, int na, const Draw *b, int nb, int *flags_differ) {
	if (na != nb || na > 2)
		return 1;
	for (int i = 0; i < na; i++) {
		if (a[i].argc != b[i].argc || a[i].argc != 8)
			return 1;
		for (int j = 0; j < 8; j++) {
			const RV *x = &a[i].args[j], *y = &b[i].args[j];
			if ((x->kind & 0xffffff) != (y->kind & 0xffffff) || x->v.bits != y->v.bits)
				return 1;
			if (x->flags != y->flags)
				*flags_differ = 1;
		}
	}
	return 0;
}

static void ABI lightback_event(void *self, void *other);
// exp_lbab / exp_lbvisab: the whole event's time (native, fallback or original) per report window
static uint32_t lb_event_us;
static void ABI lightback_native(void *self, void *other) {
	if (!lb_ab && !lb_visab) {
		lightback_event(self, other);
		return;
	}
	uint32_t t0 = sceKernelGetProcessTimeLow();
	lightback_event(self, other);
	lb_event_us += sceKernelGetProcessTimeLow() - t0;
}

static void ABI lightback_event(void *self, void *other) {
	if (lb_ab_off) {
		original(self, other);
		return;
	}
	int verify = lb_verify && orig_stretched && orig_set;
	if (verify) {
		write_count = 0;
		write_logging = 1;
		draw_count = 0;
		draw_logging = 1;
		draw_suppress = 1;
	}
	unsigned drawn_before = lightback_native_drawn;
	uint32_t t0 = lb_timing ? sceKernelGetProcessTimeLow() : 0;
	int done = handle(self, other);
	if (lb_timing)
		lightback_native_us += sceKernelGetProcessTimeLow() - t0;
	write_logging = 0;
	draw_logging = 0;
	draw_suppress = 0;
	if (!done) {
		original(self, other);
		return;
	}
	if (verify) {
		Write native_log[8];
		Draw native_draws[2];
		int native_count = write_count, native_draw_count = draw_count;
		memcpy(native_log, write_log, sizeof(native_log));
		memcpy(native_draws, draw_log, sizeof(native_draws));
		State after_native, after_original;
		snapshot(self, &after_native);
		unsigned before = stretched_count;
		write_count = 0;
		write_logging = 1;
		draw_count = 0;
		draw_logging = 1;
		original(self, other);
		write_logging = 0;
		draw_logging = 0;
		snapshot(self, &after_original);
		lightback_verify_calls++;
		int was_visible = lightback_native_drawn != drawn_before;
		lb_verify_visible += was_visible;
		if (!was_visible)
			lightback_verify_draws += stretched_count - before;
		int flags_differ = 0, bad = 0;
		if (native_count != write_count || native_count > 8 || memcmp(native_log, write_log, native_count * sizeof(Write)))
			bad = 1, lb_verify_reason[0]++;
		if (memcmp(&after_native, &after_original, sizeof(State)))
			bad = 1, lb_verify_reason[1]++;
		if (draws_differ(native_draws, native_draw_count, draw_log, draw_count, &flags_differ))
			bad = 1, lb_verify_reason[2]++;
		if (stretched_count - before != (unsigned)draw_count)
			bad = 1, lb_verify_reason[3]++;
		lightback_verify_mismatch += bad;
		lb_verify_flags += flags_differ;
	}
}

void lightback_native_report(int frames) {
	if (!original || frames <= 0)
		return;
	debugPrintf("[LIGHTBACK] user 0: off-screen native %.1f, on-screen native %.1f, on-screen original %.1f, fallback %.1f per frame | native part %.3f ms%s%s\n",
		(float)lightback_native_culled / frames, (float)lightback_native_drawn / frames,
		(float)lightback_native_visible / frames, (float)lightback_native_fallback / frames,
		lightback_native_us / 1000.0f / frames,
		lb_ab ? (lb_ab_off ? " | A/B: off" : " | A/B: on") : "",
		lb_visab ? (lb_vis_off ? " | visible A/B: off" : " | visible A/B: on") : "");
	if (lb_ab || lb_visab)
		debugPrintf("[LIGHTBACK] event time %.3f ms/frame (%s)\n", lb_event_us / 1000.0f / frames,
			(lb_ab && lb_ab_off) || (lb_visab && lb_vis_off) ? "off" : "on");
	lb_event_us = 0;
	if (lb_verify)
		debugPrintf("[LIGHTBACK] verify: %u calls re-run (%u on-screen), %u mismatches (writes %u, state %u, draws %u, draw count %u), %u flag-only differences, %u off-screen draws (session)%s\n",
			lightback_verify_calls, lb_verify_visible, lightback_verify_mismatch, lb_verify_reason[0], lb_verify_reason[1],
			lb_verify_reason[2], lb_verify_reason[3], lb_verify_flags, lightback_verify_draws,
			lb_fault ? " FAULT INJECTED" : "");
	for (int i = 0; i < 12 && lb_reject_line[i]; i++)
		debugPrintf("[LIGHTBACK] fallback at lightback_native.c:%u x%u (session, last kinds 0x%x)\n", lb_reject_line[i],
			lb_reject_count[i], reject_kind);
	lightback_native_culled = lightback_native_visible = lightback_native_drawn = lightback_native_fallback = 0;
	lightback_native_us = 0;
	if (lb_ab)
		lb_ab_off = !lb_ab_off;
	if (lb_visab)
		lb_vis_off = !lb_vis_off;
}

// The verify wrapper needs the builtin registry (filled during runner init): installed at the first report
void lightback_native_late(void) {
	if (!original || !lb_verify || orig_stretched)
		return;
	typedef struct { const char *name; void *func; int argc; } runner_function;
	runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	for (int i = 0; functions && *functions && count && i < *count; i++) {
		runner_function *f = &(*functions)[i];
		if (f->name && !strcmp(f->name, "draw_sprite_stretched_ext")) {
			orig_stretched = f->func;
			f->func = (void *)counted_stretched;
			debugPrintf("[LIGHTBACK] verify: recording draw_sprite_stretched_ext\n");
			return;
		}
	}
}

int lightback_native_install(void *(*rx_alloc)(size_t)) {
	if (original || !rx_alloc)
		return 0;
#define SYM(t, n) ((t)so_symbol(&yoyoloader_mod, n))
	compare = SYM(Compare, "_Z12YYCompareValRK6RValueS1_db");
	read_var = SYM(ReadVar, "_ZN12YYObjectBase17InternalReadYYVarEi");
	var_set = SYM(VarSet, "_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
	int32_value = SYM(Int32, "_Z12INT32_RValuePK6RValue");
	yy_bool = SYM(BoolFn, "_Z11BOOL_RValuePK6RValue");
	legacy = SYM(Legacy, "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	yy_free = SYM(FreeFn, "_Z16FREE_RValue__PreP6RValue");
	subimage = SYM(const RV *, "gs_constArg0_50DB0133");
	stretched_fid = SYM(const int *, "g_FUNC_draw_sprite_stretched_ext");
	id_hash = SYM(const uint32_t *, "_ZN9CInstance14ms_ID2InstanceE");
	instance_not_found = SYM(uint8_t *, "g_fInstanceNotFound");
	start_globals = SYM(const int *, "g_nStartGlobalVariables");
	global = SYM(void **, "g_pGlobal");
	epsilon = SYM(double *, "g_GMLMathEpsilon");
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z33gml_Object_obj_lightback_Other_10P9CInstanceS0_");
#undef SYM
	if (!compare || !read_var || !var_set || !int32_value || !id_hash || !instance_not_found || !start_globals || !global || !epsilon || !entry)
		return 0;
	for (int i = 0; i < NV; i++)
		if (!(vars[i] = (const int *)so_symbol(&yoyoloader_mod, var_names[i])))
			return 0;
	uint32_t *w = (uint32_t *)entry;
	if ((entry & 3) || w[0] != 0xe92d4ff0 || w[1] != 0xe28db01c)
		return 0;
	uint32_t *rx = rx_alloc(16);
	if (!rx || ((uintptr_t)rx & 3))
		return 0;
	uint32_t code[4] = { w[0], w[1], 0xe51ff004, (uint32_t)(entry + 8) };
	kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof(code));
	kuKernelFlushCaches(rx, sizeof(code));
	original = (Event)rx;
	// The on-screen path needs the draw call's helpers and the subimage constant (0, a real)
	lb_visible = !exp_flag("lbvisible0") && yy_bool && legacy && yy_free && subimage && stretched_fid;
	debugPrintf("[LIGHTBACK] on-screen native %s\n", lb_visible ? "installed" : exp_flag("lbvisible0") ? "off (exp_lbvisible0)" : "NOT installed");
	extern int debugMode;
	lb_verify = debugMode && exp_flag("lbverify");
	if (lb_verify) {
		extern void *entry_hook(uintptr_t entry, void *replacement);
		orig_set = (VarSet)entry_hook((uintptr_t)var_set, (void *)logged_set);
		debugPrintf("[LIGHTBACK] verify: write log %s\n", orig_set ? "installed" : "NOT installed (no verify)");
		lb_fault = exp_flag("lbverifyfault");
	}
	lb_ab = exp_flag("lbab");
	lb_visab = exp_flag("lbvisab");
	extern int heavy_profiling;
	lb_timing = heavy_profiling;
	hook_addr(entry, (uintptr_t)lightback_native);
	kuKernelFlushCaches((void *)entry, 8);
	return 1;
}
