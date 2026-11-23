/*
 * recolor_lights.c
 *
 * Most lit sprites in the game (particles such as the roll's dust clouds, the player, enemies, corpses, throwables)
 * are tinted by gml_Script_draw_sprite_recolored. Whenever global.recolor_timer has changed since the sprite's last
 * call, the script recomputes the tint from the lights the sprite keeps in close_light_array (APK 1.1.10, GML lines
 * 72-120; blackout_here is the local copy of global.blackout the script takes at entry):
 *
 *     var i = 0;
 *     repeat-like loop while (i < close_lights) {
 *         with (close_light_array[i]) {                                   // the iterator: sp+0x78
 *             rdist = point_distance(x, y, other.x, other.y);
 *             if (rdist < radius) {
 *                 var caller_alpha = other.image_alpha;
 *                 var weight = clamp((1 - rdist / radius) * 0.5 * caller_alpha * alpha, 0, 1);
 *                 var rr = r, gg = g, bb = b;
 *                 if (blackout_here) {
 *                     if (darklight) weight /= 2;
 *                     else {
 *                         weight = clamp(weight * 2, 0, 1);
 *                         rr = 255 + (rr - 255) * 0.5; gg = ...; bb = ...;
 *                     }
 *                 } else if (!argument7) weight = clamp(weight * 2, 0, 1);
 *                 other.light_r += rr * weight; other.light_g += gg * weight; other.light_b += bb * weight;
 *                 other.light_alpha += weight;
 *             }
 *         }
 *         i += 1;
 *     }
 *
 * In YYC every light costs ~50 runner calls (getters, RValue operators and temporaries, point_distance through the
 * legacy dispatcher). Rolling in room_prison_3 adds ~12 dust clouds that each recompute this every other frame.
 *
 * Here the loop keeps its original code up to the first light: the array read and YYGML_NewWithIterator of
 * close_light_array[i]. Its two iterator calls (NewWithIterator / YYGML_WithIteratorNext) go through veneers to
 * wrappers that run the body natively for each light the iterator yields, so the original body runs only for a light
 * the native body won't take. When a light's with is done natively, the wrapper also runs the rest of the loop
 * (i += 1, i < close_lights, with (close_light_array[i])) natively, element by element, while each element is an
 * instance id whose light the native body takes; it resolves the id the way NewWithIterator does
 * (CInstance::ms_ID2Instance, active flags), without the 4-byte iterator list NewWithIterator allocates and
 * DeleteWithIterator frees for every with (allocator bookkeeping only), then leaves the original's loop counter at the
 * last element done, so the original's own i += 1 and test go on from there.
 * The native body:
 * - reads x/y/image_alpha as the builtin getters Variable_GetValue_Direct dispatches to do (the instances' floats,
 *   widened) after checking that those registry entries are the 1.1.10 getters;
 * - reads the light's and the caller's own variables from their hash tables (present, plain reals: the original's
 *   getters return the same slots);
 * - computes point_distance as F_PointDistance does (YYGetFloat narrows the widened floats back exactly; single
 *   precision vmul/vmla/vsqrt), after checking that the registry entry the legacy dispatcher calls is F_PointDistance;
 *   the dispatcher's result is {value, flags 0, kind 0};
 * - inlines YYCompareVal's real-real arm (the sign of the difference, 0 within epsilon; a NaN difference goes to the
 *   runner's own function), BOOL_RValue of a real/bool (> 0.5) or undefined (false), and Push/PopContextStack where
 *   the original pushes (the runner's own push when the stack has to grow);
 * - does the RValue operators' real-real arithmetic as doubles in the original order (operator/= is a plain vdiv,
 *   clamp is YYGML_clamp's two compares), and writes rdist and the four sums the way the original's assignments do.
 * blackout_here, argument7 and the loop counter are read from the original's frame: the wrappers get &self = fp-0x10c,
 * &other = fp-0x110 and the iterator at sp+0x78 (fp-0x260), all checked.
 * Anything unexpected (a missing variable, a value that isn't a plain real, an unusual instance or with target)
 * leaves that light or element to the original, before anything has been written.
 *
 * exp_recolorlights0 keeps the original. Debug mode only ([RECOLORLIGHTS] report line):
 * - exp_recolorlightsverify: no batches; for every light the native body computes what it would write without
 *   writing, then the original body runs; the next iterator step compares the light's rdist and the caller's
 *   light_r/g/b/alpha (all 16 bytes) with the native result. Every instance-id with target is also looked up the
 *   batch's way and compared with what NewWithIterator visited. Mismatches must stay 0.
 * - exp_recolorlightsab: the native body is off in every other report window (in-run A/B).
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#ifndef RECOLOR_LIGHTS_TEST
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
extern int debugMode;
#define BUG(n) 0
#endif

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef int ABI (*new_fn)(void *iterator, void **self, void **other, const void *target);
typedef int ABI (*next_fn)(void *iterator, void **self, void **other);
typedef void ABI (*builtin_fn)(RV *result, void *self, void *other, int argc, RV *args);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double epsilon, int less);
typedef void ABI (*push_fn)(void *instance);
typedef struct { const char *name; builtin_fn call; int argc; } Function;

static new_fn new_with;
static next_fn next_with;
static uintptr_t standard_get, standard_ref;
static builtin_fn point_distance;
static compare_fn compare;
static push_fn push_context;
static const uint8_t *gc_enabled;
static void ***context_stack;
static int *context_top, *context_max;
static Function **functions;
static const double *epsilon;
static const uint8_t *builtin_variables;
static const int *builtin_count;
static const RV *undefined_rv;
static uintptr_t getters[3]; // GV_X, GV_Y, GV_ImageAlpha

enum { VX, VY, VIMAGE_ALPHA, RDIST, RADIUS, ALPHA, R, G, B, DARKLIGHT, LIGHT_R, LIGHT_G, LIGHT_B, LIGHT_ALPHA,
       CLOSE_LIGHTS, CLOSE_LIGHT_ARRAY, NVARS };
static const char *const var_names[NVARS] = {"g_VAR_x", "g_VAR_y", "g_VAR_image_alpha", "g_VAR_rdist", "g_VAR_radius",
	"g_VAR_alpha", "g_VAR_r", "g_VAR_g", "g_VAR_b", "g_VAR_darklight", "g_VAR_light_r", "g_VAR_light_g",
	"g_VAR_light_b", "g_VAR_light_alpha", "g_VAR_close_lights", "g_VAR_close_light_array"};
static const int *var_desc[NVARS]; // {name, id}: the runner fills the ids at game start, so read them live
#define VAR_ID(n) (var_desc[n][1])
static const int *distance_desc; // g_FUNC_point_distance
static const uint32_t *id_map;   // CInstance::ms_ID2Instance

int recolor_lights_ab, recolor_lights_ab_off;
static int verify;
unsigned recolor_lights_loops, recolor_lights_native, recolor_lights_fallback, recolor_lights_inside;
unsigned recolor_lights_verified, recolor_lights_mismatch, recolor_lights_batched, recolor_lights_ids_verified;
unsigned recolor_lights_reject_line[16], recolor_lights_reject_count[16];

static int reject(unsigned line) {
	for (int k = 0; k < 16; k++)
		if (!recolor_lights_reject_line[k] || recolor_lights_reject_line[k] == line) {
			recolor_lights_reject_line[k] = line;
			recolor_lights_reject_count[k]++;
			break;
		}
	return 0;
}
#define REQUIRE(c) do { if (!(c)) return reject(__LINE__); } while (0)

static uint32_t word(const void *p, size_t o) { uint32_t v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }
static int refcounted(const RV *v) { return (1u << (v->kind & 31)) & 0x46; }
static int plain(const RV *v) { return v && (v->kind & 0xffffff) == 0; }
static int boolish(const RV *v) { return v && ((v->kind & 0xffffff) == 0 || (v->kind & 0xffffff) == 13); }
// BOOL_RValue of a real or a bool: value > 0.5 (NaN: false); of undefined: false
static int truth(const RV *v) { return (v->kind & 0xffffff) != 5 && v->value > 0.5; }

// YYCompareVal(a, b, epsilon, ...) of two reals: the sign of a - b, 0 within epsilon. A NaN difference (infinities,
// NaNs) goes to the runner's own function.
static int compare_reals(const RV *a, const RV *b) {
	double d = a->value - b->value;
	if (d != d)
		return compare(a, b, *epsilon, 1);
	return fabs(d) <= *epsilon ? 0 : d < 0 ? -1 : 1;
}

// PushContextStack / PopContextStack(1)
static void push(void *instance) {
	if (!*gc_enabled)
		return;
	if (*context_top == *context_max) { // the runner grows the stack
		push_context(instance);
		return;
	}
	(*context_stack)[*context_top] = instance;
	++*context_top;
}
static void pop(void) {
	int top = *context_top - 1;
	*context_top = top < 0 ? 0 : top;
}

// F_PointDistance(x1, y1, x2, y2) of four widened floats (YYGetFloat narrows them back exactly): single precision,
// the products rounded before the sum (vmul + vmla), vsqrt.f32, widened
static double point_distance_f(float x1, float y1, float x2, float y2) {
	float dy = y2 - y1, dx = x2 - x1;
	float s = dy * dy, root;
	s = s + dx * dx;
	__asm__("vsqrt.f32 %0, %1" : "=t"(root) : "t"(s)); // the runner's instruction (no libm call for NaN)
	return (double)root;
}
static float field(const void *p, size_t o) { float v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }

// The instance's variable hash table, if it uses the runner's ordinary variable getters (vtable +8 / +12)
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
static int table(const void *owner, Table *t) {
	const void *vt = (const void *)(uintptr_t)word(owner, 0);
	if (!vt || word(vt, 8) != standard_get || word(vt, 12) != standard_ref)
		return 0;
	const void *ht = (const void *)(uintptr_t)word(owner, 0x24);
	if (!ht)
		return 0;
	t->mask = word(ht, 8);
	t->entries = (const uint8_t *)(uintptr_t)word(ht, 16);
	return t->entries && t->mask <= 65535 && !(t->mask & (t->mask + 1));
}
// A variable's slot without creating it (NULL: the instance doesn't have it)
static RV *peek(const Table *t, int n) {
	uint32_t key = ((uint32_t)VAR_ID(n) + 1) & 0x7fffffff, i = key & t->mask;
	if (!key)
		return NULL;
	for (uint32_t d = 0; d <= t->mask; ++d, i = (i + 1) & t->mask) {
		const uint8_t *e = t->entries + 12 * i;
		uint32_t k = word(e, 8);
		if (k == key)
			return (RV *)(uintptr_t)word(e, 0);
		if (!k || d > ((i - (k & t->mask)) & t->mask))
			return NULL;
	}
	return NULL;
}

static double clamp01(double v) { // YYGML_clamp(v, 0, 1): vcmp/vmovmi, then vcmp/vmovgt (NaN stays NaN)
	if (v < 0.0)
		v = 0.0;
	if (v > 1.0)
		v = 1.0;
	return v;
}

// The original's frame, from the iterator call's arguments
typedef struct { const RV *blackout; int argc; RV **args; RV *counter; } Frame;
static int frame_of(void *iterator, void **self, void **other, Frame *f) {
	uintptr_t fp = (uintptr_t)self + 0x10c;
	if ((uintptr_t)other != fp - 0x110 || (uintptr_t)iterator != fp - 0x260)
		return 0;
	f->blackout = (const RV *)(fp - 0x130);
	f->argc = *(const int *)(fp - 0x2b8); // [sp, #0x20]
	f->args = *(RV ***)(fp + 8);
	f->counter = (RV *)(fp - 0x170); // the loop counter, [sp, #0x168]
	return 1;
}

// Checks for the whole loop: variable ids, the builtin getters and point_distance's registry entry, epsilon
static int loop_ok(void) {
	static int ids_set; // the runner fills the variable ids once, at game start
	REQUIRE(*functions && *builtin_count > 0 && isfinite(*epsilon) && *epsilon >= 0 && *context_stack);
	if (!ids_set) {
		for (int n = 0; n < NVARS; n++)
			REQUIRE(VAR_ID(n) >= 0);
		ids_set = 1;
	}
	for (int n = VX; n <= VIMAGE_ALPHA; n++) // Variable_GetValue_Direct's builtin path: id <= 9999, < the builtin count
		REQUIRE(VAR_ID(n) <= 9999 && VAR_ID(n) < *builtin_count && word(builtin_variables, 16 * VAR_ID(n) + 4) == getters[n]);
	REQUIRE(distance_desc[1] >= 0 && distance_desc[1] < 16384 && (*functions)[distance_desc[1]].call == point_distance);
	return 1;
}

typedef struct {
	RV rdist;          // what rdist is set to
	int inside;        // rdist < radius: the caller's sums change
	double sum[4];     // light_r, light_g, light_b, light_alpha afterwards
	RV *rdist_slot, *sums[4];
} Outcome;

// What the body reads from the caller (the script's self) besides its x/y/image_alpha: looked up once per iterator
// step and kept through a batch (the native body writes only the values of these real slots and the lights' rdist,
// so no variable is created and no slot moves). blackout / arg7: BOOL_RValue of blackout_here / argument7, -1 if
// they aren't a real, a bool (or undefined for argument7).
typedef struct { void *self; Table t; RV *sums[4]; int sums_ok, blackout, arg7; } Caller;
static int caller_ok(const Frame *f, void *caller, Caller *c) {
	// Variable_GetValue_Direct takes the builtin getter path only for instances (+0x50 == 1)
	REQUIRE(caller && word(caller, 0x50) == 1 && table(caller, &c->t));
	c->self = caller;
	c->sums_ok = 1;
	for (int k = 0; k < 4; k++) {
		c->sums[k] = peek(&c->t, LIGHT_R + k);
		c->sums_ok &= plain(c->sums[k]);
	}
	c->blackout = boolish(f->blackout) ? truth(f->blackout) : -1;
	const RV *a7 = f->argc >= 8 ? f->args[7] : undefined_rv;
	c->arg7 = a7 && (boolish(a7) || (a7->kind & 0xffffff) == 5) ? truth(a7) : -1;
	return 1;
}

// The body for one light (self) and the caller (other). write = 0: compute only (verify mode).
// Returns 0, having written nothing, if the original body has to run it.
static int body(const Caller *c, void *light, int write, Outcome *o) {
	Table tl;
	void *caller = c->self;
	REQUIRE(light && light != caller && word(light, 0x50) == 1 && table(light, &tl));
	RV *rdist = peek(&tl, RDIST), *radius = peek(&tl, RADIUS);
	REQUIRE(rdist && !refcounted(rdist) && plain(radius));
	// rdist = point_distance(x, y, other.x, other.y): the legacy dispatcher's result is {value, flags 0, kind 0}
	RV res = {point_distance_f(field(light, 0xb8), field(light, 0xbc), field(caller, 0xb8), field(caller, 0xbc)), 0, 0};
	if (BUG(2))
		res.value = nextafter(res.value, INFINITY);
	o->rdist = res;
	o->rdist_slot = rdist;
	int cmp = compare_reals(&res, radius);
	o->inside = cmp != -2 && cmp <= -1;
	double weight = 0, rr = 0, gg = 0, bb = 0;
	if (o->inside) {
		RV *alpha = peek(&tl, ALPHA), *r = peek(&tl, R), *g = peek(&tl, G), *b = peek(&tl, B);
		REQUIRE(plain(alpha) && plain(r) && plain(g) && plain(b));
		REQUIRE(c->sums_ok && c->blackout >= 0);
		for (int k = 0; k < 4; k++)
			o->sums[k] = c->sums[k];
		int blackout = c->blackout, darklight = 0, arg7 = c->arg7;
		if (blackout) {
			RV *dl = peek(&tl, DARKLIGHT);
			REQUIRE(boolish(dl));
			darklight = truth(dl);
		} else {
			REQUIRE(arg7 >= 0);
		}
		// weight = clamp((1 - rdist / radius) * 0.5 * caller_alpha * alpha, 0, 1)
		double t = res.value / radius->value;
		t = 1.0 - t;
		t = t * 0.5;
		t = t * (double)field(caller, 0xb0); // other.image_alpha
		t = t * alpha->value;
		weight = clamp01(t);
		if (BUG(1))
			weight = t;
		rr = r->value;
		gg = g->value;
		bb = b->value;
		if (blackout) {
			if (darklight) {
				weight = weight / 2.0;
			} else {
				weight = clamp01(weight * 2.0);
				rr = 255.0 + (rr - 255.0) * 0.5;
				gg = 255.0 + (gg - 255.0) * 0.5;
				bb = 255.0 + (bb - 255.0) * 0.5;
			}
		} else if (!arg7) {
			weight = clamp01(weight * 2.0);
		}
		o->sum[0] = o->sums[0]->value + rr * weight;
		o->sum[1] = o->sums[1]->value + gg * weight;
		o->sum[2] = o->sums[2]->value + bb * weight;
		o->sum[3] = o->sums[3]->value + weight;
		if (BUG(3))
			o->sum[1] = o->sums[1]->value + rr * weight;
	}
	if (!write)
		return 1;
	// Checked: from here on the body writes what the original's assignments write
	push(light);
	*rdist = res;
	pop();
	if (o->inside) {
		for (int k = 0; k < 3; k++)
			o->sums[k]->value = o->sum[k];
		push(caller);
		o->sums[3]->value = o->sum[3];
		pop();
	}
	return 1;
}

// Verify mode: the light the original body is running for, and what the native body computed for it
static struct { int on; void *light, *caller; Outcome o; RV before[4]; } pending;

static int same(const RV *a, const RV *b) { return !memcmp(a, b, sizeof(RV)); }

static void verify_finish(void) {
	pending.on = 0;
	const Outcome *o = &pending.o;
	int ok = same(o->rdist_slot, &o->rdist);
	for (int k = 0; k < 4 && ok && o->inside; k++) {
		RV want = {o->sum[k], pending.before[k].flags, pending.before[k].kind};
		ok = same(o->sums[k], &want);
	}
	if (ok) {
		recolor_lights_verified++;
	} else if (recolor_lights_mismatch++ < 8) {
		debugPrintf("[RECOLORLIGHTS] mismatch: inside %d rdist %.17g/%.17g (kind %u/%u)\n", o->inside,
			o->rdist_slot->value, o->rdist.value, o->rdist_slot->kind, o->rdist.kind);
		if (o->inside)
			for (int k = 0; k < 4; k++)
				debugPrintf("[RECOLORLIGHTS]   sum %d: gml %.17g (kind %u flags %x) native %.17g\n", k,
					o->sums[k]->value, o->sums[k]->kind, o->sums[k]->flags, o->sum[k]);
	}
}

// with (id) for an instance id, as YYGML_NewWithIterator resolves it: CInstance::ms_ID2Instance = {buckets, mask};
// a bucket is {first, last}, an element {prev, next, id, instance}; the first element with the id decides, and its
// instance is visited only if active ((flags +0x7c & 0x100007) == 4). NULL: the with visits nothing.
static void *instance_of(int id) {
	const uint32_t *buckets = (const uint32_t *)(uintptr_t)id_map[0];
	const uint8_t *e = (const uint8_t *)(uintptr_t)buckets[2 * (id_map[1] & (uint32_t)id)];
	for (; e; e = (const uint8_t *)(uintptr_t)word(e, 4))
		if ((int)word(e, 8) == id) {
			const void *inst = (const void *)(uintptr_t)word(e, 12);
			return inst && (word(inst, 0x7c) & 0x100007) == 4 ? (void *)inst : NULL;
		}
	return NULL;
}

// The instance id a with target names (an id > 99999 held as a real), or -1: another kind of target
static int target_id(const RV *t) {
	if ((t->kind & 0xffffff) != 0 || !(t->value >= 100000.0 && t->value < 2147483648.0))
		return -1;
	return (int)t->value; // INT32_RValue: truncation
}

// After a light's with has run natively, the rest of the original loop (GML lines 72-74, repeated):
//     i += 1; if (!(i < close_lights)) exit; with (close_light_array[i]) <body>
// runs here, element by element, while each is an instance id and the native body takes its light: the loop counter,
// close_lights and the array are read as the original reads them (plain reals, an array of RValues, the index in
// range; anything else, or a light the native body won't take, ends the batch before that element). The instance is
// found the way NewWithIterator finds it, without its 4-byte iterator list (allocated and freed again for every
// with). The original's counter is then set to the last element done, so its own i += 1 and test continue from there.
static void batch(const Frame *f, const Caller *c) {
	RV *counter = f->counter;
	if (!plain(counter))
		return;
	double i = counter->value;
	for (;;) {
		RV next = {i + 1.0, 0, 0}; // the original: REAL_RValue(i) + 1.0
		RV *count = peek(&c->t, CLOSE_LIGHTS), *array = peek(&c->t, CLOSE_LIGHT_ARRAY);
		if (!plain(count) || compare_reals(&next, count) != -1) // the loop ends (the original tests it again)
			break;
		if (!array || (array->kind & 0xffffff) != 2 || !(next.value >= 0 && next.value < 2147483648.0))
			break;
		const uint8_t *ref = (const uint8_t *)(uintptr_t)word(&array->value, 0);
		int index = (int)next.value;
		if (!ref || index >= (int)word(ref, 0x1c))
			break;
		int id = target_id((const RV *)(uintptr_t)(word(ref, 4) + 16 * (uint32_t)index));
		if (id < 0 || !id_map[0])
			break;
		void *light = instance_of(id);
		Outcome o;
		if (light && !body(c, light, 1, &o))
			break;
		if (light) {
			recolor_lights_native++;
			recolor_lights_inside += o.inside;
		}
		recolor_lights_batched++;
		i = next.value;
	}
	counter->value = i;
}

// r: the iterator's result (> 0: *self is the next light, *other the caller)
static int step(int r, void *iterator, void **self, void **other) {
	if (recolor_lights_ab_off)
		return r;
	Frame f;
	Caller c;
	// the caller is the self the original's with saved (its *other while the with runs)
	void *caller = ((void **)iterator)[0];
	if (!frame_of(iterator, self, other, &f) || !loop_ok() || (r > 0 && *other != caller) || !caller_ok(&f, caller, &c)) {
		recolor_lights_fallback += r > 0;
		return r;
	}
	while (r > 0) {
		Outcome o;
		if (!body(&c, *self, !verify, &o)) {
			recolor_lights_fallback++;
			return r;
		}
		recolor_lights_inside += o.inside;
		if (verify) { // the original body runs; the next iterator step compares
			pending.on = 1;
			pending.light = *self;
			pending.caller = *other;
			pending.o = o;
			if (o.inside)
				for (int k = 0; k < 4; k++)
					pending.before[k] = *o.sums[k];
			return r;
		}
		recolor_lights_native++;
		r = next_with(iterator, self, other);
	}
	if (r == 0 && !verify) // this element's with is done: the original goes on to DeleteWithIterator, then i += 1
		batch(&f, &c);
	return r;
}

static int ABI recolor_new_with(void *iterator, void **self, void **other, const void *target) {
	pending.on = 0;
	recolor_lights_loops++;
	int r = new_with(iterator, self, other, target);
	if (verify && !recolor_lights_ab_off) { // the batch's instance lookup against the runner's
		int id = target_id(target);
		if (id >= 0) {
			void *want = r > 0 ? *self : NULL;
			if (instance_of(id) == want && (r <= 1 || !want))
				recolor_lights_ids_verified++;
			else if (recolor_lights_mismatch++ < 8)
				debugPrintf("[RECOLORLIGHTS] mismatch: with (%d) visits %p (%d), lookup %p\n", id, want, r, instance_of(id));
		}
	}
	return step(r, iterator, self, other);
}

static int ABI recolor_next_with(void *iterator, void **self, void **other) {
	if (pending.on)
		verify_finish();
	return step(next_with(iterator, self, other), iterator, self, other);
}

// The bl at site calls func directly (a PLT-patched runner) or through the 1.1.10 linker veneer to its PLT entry
static int calls(uintptr_t site, uintptr_t veneer, uintptr_t plt, uintptr_t func) {
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	if ((ins >> 24) != 0xeb)
		return 0;
	if (target == func)
		return 1;
	if (target != veneer)
		return 0;
	// movw ip, #lo; movt ip, #hi; add ip, ip, pc; bx ip
	const uint32_t *v = (const uint32_t *)veneer;
	if ((v[0] & 0xfff0f000) != 0xe300c000 || (v[1] & 0xfff0f000) != 0xe340c000 || v[2] != 0xe08cc00f || v[3] != 0xe12fff1c)
		return 0;
	uint32_t imm = (v[0] & 0xfff) | ((v[0] >> 4) & 0xf000) | (((v[1] & 0xfff) | ((v[1] >> 4) & 0xf000)) << 16);
	return veneer + 16 + imm == plt;
}

// FNV-1a over the script's code from its third word (an entry hook may replace the first two), bl offsets masked
// (patch_plt_calls rewrites them)
static uint32_t fingerprint(const uint32_t *w, size_t words) {
	uint32_t h = 2166136261u;
	for (size_t i = 2; i < words; i++) {
		uint32_t v = (w[i] >> 24) == 0xeb ? 0xeb000000 : w[i];
		for (int b = 0; b < 4; b++)
			h = (h ^ ((v >> (8 * b)) & 0xff)) * 16777619u;
	}
	return h;
}

static uint32_t *veneer_near(void *(*rx_alloc)(size_t), uintptr_t site, uintptr_t wrapper) {
	uint32_t *v = rx_alloc ? rx_alloc(8) : NULL;
	int32_t off = v ? (int32_t)((uintptr_t)v - (site + 8)) : 0;
	if (!v || ((uintptr_t)v & 3) || off < -(1 << 25) || off >= (1 << 25))
		return NULL;
	uint32_t code[2] = {0xe51ff004, wrapper}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(v, code, sizeof(code));
	kuKernelFlushCaches(v, sizeof(code));
	return v;
}

static void retarget(uintptr_t site, const uint32_t *veneer) {
	int32_t off = (int32_t)((uintptr_t)veneer - (site + 8));
	uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
}

int recolor_lights_install(void *(*rx_alloc)(size_t)) {
	// ELF addresses in APK 1.1.10: the script, its iterator call sites, their linker veneers and the PLT entries
	enum { SCRIPT = 0x196efc0, SIZE = 13644, NEW_SITE = 0x1970354, NEXT_SITE = 0x19713e0,
	       NEW_VENEER = 0x19ecd40, NEXT_VENEER = 0x19ecd70, PLT_NEW = 0x4ab0e00, PLT_NEXT = 0x4ab0e30 };
	const uint32_t FINGERPRINT = 0x9a2d6a77; // the same on the original and the PLT-patched runner
#define RESOLVE(v, t, n) do { v = (t)so_symbol(&yoyoloader_mod, n); if (!v) return 0; } while (0)
	uintptr_t entry;
	RESOLVE(entry, uintptr_t, "_Z32gml_Script_draw_sprite_recoloredP9CInstanceS0_R8YYRValueiPPS1_");
	RESOLVE(new_with, new_fn, "_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	RESOLVE(next_with, next_fn, "_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
#define AT(a) (entry + (a) - SCRIPT)
	if ((entry & 3) || fingerprint((const uint32_t *)entry, SIZE / 4) != FINGERPRINT ||
			!calls(AT(NEW_SITE), AT(NEW_VENEER), AT(PLT_NEW), (uintptr_t)new_with) ||
			!calls(AT(NEXT_SITE), AT(NEXT_VENEER), AT(PLT_NEXT), (uintptr_t)next_with)) {
		debugPrintf("recolor lights: unexpected code in draw_sprite_recolored\n");
		return 0;
	}
	RESOLVE(standard_get, uintptr_t, "_ZN12YYObjectBase19InternalGetYYVarRefEi");
	RESOLVE(standard_ref, uintptr_t, "_ZN12YYObjectBase20InternalGetYYVarRefLEi");
	RESOLVE(point_distance, builtin_fn, "_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_");
	RESOLVE(compare, compare_fn, "_Z12YYCompareValRK6RValueS1_db");
	RESOLVE(push_context, push_fn, "_Z16PushContextStackP12YYObjectBase");
	RESOLVE(gc_enabled, const uint8_t *, "g_fGarbageCollection");
	RESOLVE(context_stack, void ***, "g_ContextStack");
	RESOLVE(context_top, int *, "g_ContextStackTop");
	RESOLVE(context_max, int *, "g_ContextStackMax");
	RESOLVE(functions, Function **, "the_functions");
	RESOLVE(epsilon, const double *, "g_GMLMathEpsilon");
	RESOLVE(builtin_variables, const uint8_t *, "builtin_variables");
	RESOLVE(builtin_count, const int *, "g_nStartGlobalVariables");
	RESOLVE(undefined_rv, const RV *, "g_undefinedRV");
	RESOLVE(getters[VX], uintptr_t, "_Z4GV_XP9CInstanceiP6RValue");
	RESOLVE(getters[VY], uintptr_t, "_Z4GV_YP9CInstanceiP6RValue");
	RESOLVE(getters[VIMAGE_ALPHA], uintptr_t, "_Z13GV_ImageAlphaP9CInstanceiP6RValue");
	RESOLVE(distance_desc, const int *, "g_FUNC_point_distance");
	RESOLVE(id_map, const uint32_t *, "_ZN9CInstance14ms_ID2InstanceE");
	for (int n = 0; n < NVARS; n++)
		RESOLVE(var_desc[n], const int *, var_names[n]);
#undef RESOLVE
	// Both veneers first, so a failed allocation leaves neither site patched
	uint32_t *new_veneer = veneer_near(rx_alloc, AT(NEW_SITE), (uintptr_t)&recolor_new_with);
	uint32_t *next_veneer = new_veneer ? veneer_near(rx_alloc, AT(NEXT_SITE), (uintptr_t)&recolor_next_with) : NULL;
	if (!next_veneer) {
		debugPrintf("recolor lights: no room for the iterator veneers\n");
		return 0;
	}
	if (debugMode) {
		verify = exp_flag("recolorlightsverify");
		recolor_lights_ab = exp_flag("recolorlightsab");
	}
	retarget(AT(NEW_SITE), new_veneer);
	retarget(AT(NEXT_SITE), next_veneer);
#undef AT
	debugPrintf("recolor lights: native light loop%s%s\n", verify ? " (verify mode)" : "",
		recolor_lights_ab ? " (A/B per report window)" : "");
	return 1;
}
