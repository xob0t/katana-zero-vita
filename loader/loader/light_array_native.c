/*
 * light_array_native.c
 *
 * A lit object keeps a list of the lights near it for draw_sprite_recolored (the rim tint). It rebuilds the list with
 * gml_Script_update_light_array (APK 1.1.10, GML lines 3-13):
 *
 *     close_lights = 0;
 *     with (obj_lightsource) {                                   // target 621
 *         if (!argument0 || region == other.region
 *             || (bypass_regioncheck && collision_line(x, y, other.x, other.y - 32, obj_door, false, false) == noone)) {
 *             rdist = point_distance(x, y, other.x, other.y);
 *             if (rdist < radius * 1.25) {
 *                 other.close_light_array[other.close_lights] = id;
 *                 other.close_lights += 1;
 *             }
 *         }
 *     }
 *
 * In the bike chase every particle runs it from its Create event (obj_particle), and the helicopter's far rockets
 * every frame from their draw event: ~90 us per call against the room's lights. A mortar volley creates ~120
 * explosion particles in one frame, 11-14 ms of it.
 *
 * This is the parked light_update.c (untracked; the same loop of update_light_array_ext, verified exact on the Vita)
 * pointed at update_light_array: the with loop keeps its original code; its iterator calls go through veneers to
 * wrappers that run the loop body natively for each light and step on to the next, so the original body only runs
 * for a light the native body won't take. The native body calls the runner's own BOOL_RValue, YYCompareVal,
 * point_distance and collision_line with the same values, in the same order, and writes rdist the way the original
 * does; radius * 1.25 is the same double product the original's operator* makes of a plain real. A light that becomes
 * close goes through the original's own runner calls (Other(), Variable_GetValue/SetValue, operator+=). Before it
 * calls anything, the native body checks every value it reads: plain reals, existing variables, ordinary instance
 * vtables and x/y getters; anything else leaves that light to the original body. An entry hook on the script records
 * its argument for the wrappers.
 *
 * exp_lightarray0 keeps the original. exp_lightarrayverify runs every call twice, the original and then the native
 * body writing for real, and compares what the two passes left: visited lights, rdist, close_lights and the
 * close_light_array entries (see compare_passes; [LIGHTARRAY] report, mismatches must stay 0).
 *
 * lightArrayFast (yyl.cfg, default 1): a light's distance, rdist store and compare inline instead of through
 * F_PointDistance, InternalGetYYVarRefL/InternalGetYYVarRef and YYCompareVal (see body), PushContextStack/
 * PopContextStack and BOOL_RValue of the argument inline, and a close light's two writes straight to the runner's
 * Variable_GetValue_Direct / Variable_SetValue_Direct, which is where YYGML_Variable_GetValue/SetValue of an instance
 * end up (see close_light), without the Other() call and its RValue. Exact, checked by the verify above;
 * exp_lightarrayfastfault (rdist) and exp_lightarrayclosefault (array index) break it on purpose.
 * Where the time went (Vita, a vial's 131 new clouds, ~22 lights and ~9 close ones per call): the with-iterator is a
 * few instructions per light (YYGML_WithIteratorNext), NewWithIterator ~7 us per call; the rest was the per-light body
 * (~2.5 us) and the close-light writes (~3.5 us each through YYGML_Variable_* and Other()). A smoke vial's new clouds
 * each build their list on their first two Draws: ~96 us per call before this, ~55 us with it.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef RV *ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);
typedef int ABI (*new_fn)(void *iterator, void **self, void **other, const void *target);
typedef int ABI (*next_fn)(void *iterator, void **self, void **other);
typedef RV *ABI (*ref_fn)(void *instance, int id);
typedef void ABI (*builtin_fn)(RV *result, void *self, void *other, int argc, RV *args);
typedef int ABI (*bool_fn)(const RV *value);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double epsilon, int less);
typedef RV *ABI (*legacy_fn)(void *self, void *other, RV *result, int argc, int id, RV **args);
typedef void ABI (*getvalue_fn)(const RV *instance, int id, int index, RV *out, int, int);
typedef void ABI (*setvalue_fn)(const RV *instance, int id, int index, RV *value);
typedef int ABI (*int32_fn)(const RV *value);
typedef RV *ABI (*addeq_fn)(RV *value, int n);
typedef void ABI (*free_fn)(RV *value);
typedef void ABI (*push_fn)(void *instance);
typedef void ABI (*pop_fn)(int n);
typedef struct { const char *name; builtin_fn call; int argc; } Function;

static new_fn new_with;
static next_fn next_with;
static ref_fn standard_get, standard_ref;
static bool_fn bool_value;
static compare_fn compare;
static legacy_fn legacy_call;
static getvalue_fn variable_get;
static setvalue_fn variable_set;
static int32_fn int32_value;
static addeq_fn add_eq;
static free_fn release;
static push_fn push_context;
static pop_fn pop_context;
static script_fn original_script;
static Function **functions, **current_function;
static const double *epsilon;
static const void *builtin_variables;
static const int *builtin_count;
static uintptr_t xy_get[2];
static int verify;

enum { REGION, BYPASS, RADIUS, RDIST, CLOSE_LIGHTS, CLOSE_LIGHT_ARRAY, ID, VX, VY, NVARS };
static const char *const var_names[NVARS] = {"g_VAR_region", "g_VAR_bypass_regioncheck", "g_VAR_radius", "g_VAR_rdist",
	"g_VAR_close_lights", "g_VAR_close_light_array", "g_VAR_id", "g_VAR_x", "g_VAR_y"};
static const int *var_desc[NVARS]; // {name, id}: the runner fills the ids at game start, so read them live
#define VAR_ID(n) (var_desc[n][1])
enum { DISTANCE, COLLISION, OTHER, NFUNCS };
static const char *const fn_names[NFUNCS] = {"g_FUNC_point_distance", "g_FUNC_collision_line", "g_FUNC_Other"};
static const char *const fn_symbols[NFUNCS] = {"_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_",
	"_Z15F_CollisionLineR6RValueP9CInstanceS2_iPS_", NULL};
static const int *fn_desc[NFUNCS];
static builtin_fn builtins[NFUNCS];

unsigned light_array_calls, light_array_native, light_array_fallback, light_array_verified, light_array_mismatch;
// Coverage of the region test: lights in another region, and of those the door rays that were blocked or clear;
// rejected lights whose rdist the verifier checked
unsigned light_array_cross_region, light_array_door_blocked, light_array_door_clear, light_array_rejected_checked;
unsigned light_array_entries_checked;
unsigned light_array_reject_line[16], light_array_reject_count[16];
uint32_t light_array_us, light_array_max_us;
unsigned light_array_lights;

// The call being run (an entry hook sets it; nested calls restore the outer one)
static struct { void *self; int argc; RV **args; int ok; } cur;

static int reject(unsigned line) {
	for (int k = 0; k < 16; k++)
		if (!light_array_reject_line[k] || light_array_reject_line[k] == line) {
			light_array_reject_line[k] = line;
			light_array_reject_count[k]++;
			break;
		}
	return 0;
}
#define REQUIRE(c) do { if (!(c)) return reject(__LINE__); } while (0)

static uint32_t word(const void *p, size_t o) { uint32_t v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }
static float scalar(const void *p, size_t o) { float v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }
static RV real(double v) { RV r = {v, 0, 0}; return r; }
static int refcounted(const RV *v) { return (1u << (v->kind & 31)) & 0x46; }

// The instance's variable hash table, if it uses the runner's ordinary variable getters
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
static int table(void *owner, Table *t) {
	if (!owner)
		return 0;
	const void *vt = (const void *)(uintptr_t)word(owner, 0);
	if (!vt || word(vt, 8) != (uintptr_t)standard_get || word(vt, 12) != (uintptr_t)standard_ref)
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
static int plain(const RV *v) { return v && (v->kind & 0xffffff) == 0 && isfinite(v->value); }

// A builtin called the way YYGML_CallLegacyFunction calls it
static void builtin(int n, RV *out, void *self, void *other, int argc, RV *args) {
	Function *saved = *current_function;
	uint32_t z = 0;
	memcpy(&out->value, &z, 4);
	out->flags = 0;
	out->kind = 5;
	*current_function = *functions + fn_desc[n][1];
	builtins[n](out, self, other, argc, args);
	*current_function = saved;
}

// Checks for the whole call (the entry hook): registry entries, the x/y getters, epsilon, the arguments
static int call_ok(void) {
	REQUIRE(*functions && isfinite(*epsilon) && *epsilon >= 0);
	for (int n = 0; n < NVARS; n++)
		REQUIRE(VAR_ID(n) >= 0);
	for (int n = DISTANCE; n <= COLLISION; n++)
		REQUIRE(fn_desc[n][1] >= 0 && fn_desc[n][1] < 16384 && (*functions)[fn_desc[n][1]].call == builtins[n]);
	REQUIRE(fn_desc[OTHER][1] >= 0);
	for (int i = 0; i < 2; i++)
		REQUIRE(*builtin_count > 0 && (uint32_t)VAR_ID(VX + i) < (uint32_t)*builtin_count &&
			word(builtin_variables, 16 * VAR_ID(VX + i) + 4) == xy_get[i]);
	REQUIRE(cur.argc >= 1 && cur.args && cur.args[0]);
	return 1;
}

typedef struct { int distance, close; RV rdist; } Outcome;

// other.close_light_array[other.close_lights] = id; other.close_lights += 1 (the original's runner calls)
static int fast, close_fault;
extern unsigned light_array_close_fast;
typedef int ABI (*getdirect_fn)(void *owner, int id, int index, RV *out, int, int);
typedef int ABI (*setdirect_fn)(void *owner, int id, int index, RV *value);
static getdirect_fn get_direct;
static setdirect_fn set_direct;
static void **context_stack_base;
static int *context_top, *context_max;
static const uint8_t *garbage_collection;
// PushContextStack(owner) (0xecc284): with garbage collection on, append; the runner's own call when it must grow.
// PopContextStack(1) (0xecc33c): top - 1, not below 0. (As smokedraw_native.c does them.)
static inline void push_inline(void *owner) {
	if (!*garbage_collection)
		return;
	int top = *context_top;
	if (top == *context_max) {
		push_context(owner);
		return;
	}
	((void **)*(void ***)context_stack_base)[top] = owner;
	*context_top = top + 1;
}
static inline void pop_inline(void) {
	int top = *context_top - 1;
	*context_top = top < 0 ? 0 : top;
}

static void close_light(void *light, void *caller) {
	if (fast && get_direct && set_direct) {
		// The same writes without the call plumbing (lightArrayFast): YYGML_Variable_GetValue of an instance (kind 6) is
		// Variable_GetValue_Direct(light, id, index, out, false, false) (0xf89f24); Other() in the with body is the
		// caller, and YYGML_Variable_SetValue of a kind-6 target is Variable_SetValue_Direct(caller, ...) (0xf8a070);
		// INT32_RValue of a real is vcvt.s32.f64 (0xec7c6c); operator+=(int) of a real adds (double)n (0xbae58c).
		Table tc;
		RV *count = table(caller, &tc) ? peek(&tc, CLOSE_LIGHTS) : NULL;
		if (count && (count->kind & 0xffffff) == 0 && count->value > -2147483648.0 && count->value < 2147483648.0) {
			RV id = {0, 0, 5};
			if (get_direct(light, VAR_ID(ID), (int)0x80000000, &id, 0, 0)) {
				int n = (int)count->value + close_fault; // close_fault: negative control (exp_lightarrayclosefault)
				set_direct(caller, VAR_ID(CLOSE_LIGHT_ARRAY), n, &id);
				RV *c = peek(&tc, CLOSE_LIGHTS);
				if (c && (c->kind & 0xffffff) == 0)
					c->value = c->value + 1.0;
				else
					add_eq(standard_ref(caller, VAR_ID(CLOSE_LIGHTS)), 1);
				if (refcounted(&id))
					release(&id);
				light_array_close_fast++;
				return;
			}
			if (refcounted(&id))
				release(&id);
		}
	}
	RV *count = standard_get(caller, VAR_ID(CLOSE_LIGHTS));
	RV inst = {0, 0, 6}, id = {0, 0, 5}, res = {0, 0, 5};
	memcpy(&inst.value, &light, sizeof(light));
	variable_get(&inst, VAR_ID(ID), (int)0x80000000, &id, 0, 0);
	RV *target = legacy_call(light, caller, &res, 0, fn_desc[OTHER][1], NULL);
	variable_set(target, VAR_ID(CLOSE_LIGHT_ARRAY), int32_value(count), &id);
	add_eq(standard_ref(caller, VAR_ID(CLOSE_LIGHTS)), 1);
	if (refcounted(&id))
		release(&id);
	if (refcounted(&res))
		release(&res);
}

/*
 * lightArrayFast (yyl.cfg, default 1; 0 = every step through the runner as before): the distance, the rdist store and
 * the compare of a light done inline (see body). F_PointDistance's code is checked at install (the inline copy is of
 * that code). exp_lightarrayfastab flips it every report window ([LIGHTARRAY] fast count).
 */
int light_array_fast_on = 1;
static int fast_ab, fast_fault; // exp_lightarrayfastfault (negative control for exp_lightarrayverify): rdist + 0.5
unsigned light_array_fast, light_array_close_fast;

// The loop body for one light (self) and the caller (other). dry: compute without writing (unused since the
// two-pass verifier).
// Returns 0, having called nothing, if the original body has to run it.
static int body(void *light, void *caller, int dry, Outcome *o) {
	Table tl, tc;
	REQUIRE(light && caller == cur.self);
	REQUIRE(table(light, &tl) && table(caller, &tc));
	float lx = scalar(light, 0xb8), ly = scalar(light, 0xbc), cx = scalar(caller, 0xb8), cy = scalar(caller, 0xbc);
	REQUIRE(isfinite(lx) && isfinite(ly) && isfinite(cx) && isfinite(cy));
	RV *radius = peek(&tl, RADIUS);
	REQUIRE(plain(radius));
	const RV *a0 = cur.args[0];
	REQUIRE((a0->kind & 0xffffff) == 0 || (a0->kind & 0xffffff) == 13);
	RV *region = NULL, *caller_region = NULL, *bypass = NULL;
	if (fast ? a0->value > 0.5 : bool_value(a0)) { // BOOL_RValue of a real or bool (kinds checked above)
		region = peek(&tl, REGION);
		caller_region = peek(&tc, REGION);
		bypass = peek(&tl, BYPASS);
		REQUIRE(plain(region) && plain(caller_region) && bypass &&
			((bypass->kind & 0xffffff) == 0 || (bypass->kind & 0xffffff) == 13));
	}
	light_array_lights++;
	// Checked: from here on the body does what the original does
	o->distance = 1;
	o->close = 0;
	if (region && compare(region, caller_region, *epsilon, 0) != 0) {
		o->distance = 0;
		light_array_cross_region++;
		if (bool_value(bypass)) {
			// collision_line(x, y, other.x, other.y - 32, obj_door, false, false) == noone
			RV args[7] = {real(lx), real(ly), real(cx), real((double)cy - 32.0), {0, 0, 15}, {0, 0, 13}, {0, 0, 13}};
			uint32_t door[2] = {343, 0x01000000};
			memcpy(&args[4].value, door, 8);
			RV hit, noone = real(-4.0);
			builtin(COLLISION, &hit, light, caller, 7, args);
			o->distance = compare(&hit, &noone, *epsilon, 0) == 0;
			if (o->distance)
				light_array_door_clear++;
			else
				light_array_door_blocked++;
			if (refcounted(&hit))
				release(&hit);
		}
	}
	if (!o->distance)
		return 1;
	if (fast && !dry) {
		// The same steps without the runner calls (lightArrayFast): point_distance as F_PointDistance computes it
		// (YYGetFloat of each argument, float dx = x2 - x1, dy = y2 - y1, dy * dy + dx * dx rounded after each step,
		// vsqrt.f32, widened; kind 0, flags as builtin() leaves them), the store into rdist's existing slot (what
		// InternalGetYYVarRefL returns for it), the getters' slots for rdist and radius, and YYCompareVal of two reals
		// (d = a - b; equal within epsilon). A missing rdist slot or a NaN difference takes the steps below.
		RV *slot = peek(&tl, RDIST);
		if (slot) {
			float dx = cx - lx, dy = cy - ly, s = dy * dy;
			s = s + dx * dx;
			float r;
			__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(s));
			o->rdist.value = (double)r + (fast_fault ? 0.5 : 0.0);
			o->rdist.flags = 0;
			o->rdist.kind = 0;
			push_inline(light);
			if (refcounted(slot))
				release(slot);
			*slot = o->rdist;
			pop_inline();
			RV limit = real(radius->value * 1.25);
			double d = slot->value - limit.value;
			int c = d == d ? (fabs(d) <= *epsilon ? 0 : d < 0 ? -1 : 1) : compare(slot, &limit, *epsilon, 1);
			o->close = c != -2 && c < 0;
			light_array_fast++;
			if (o->close)
				close_light(light, caller);
			return 1;
		}
	}
	RV *slot = dry ? NULL : standard_ref(light, VAR_ID(RDIST));
	RV args[4] = {real(lx), real(ly), real(cx), real(cy)};
	builtin(DISTANCE, &o->rdist, light, caller, 4, args);
	RV *rdist = &o->rdist;
	if (!dry) {
		push_context(light);
		if (slot != &o->rdist) {
			if (refcounted(slot))
				release(slot);
			*slot = o->rdist;
		}
		pop_context(1);
		rdist = standard_get(light, VAR_ID(RDIST));
		radius = standard_get(light, VAR_ID(RADIUS));
	}
	RV limit = real(radius->value * 1.25); // operator*(YYRValue const&, double) of a plain real
	int c = compare(rdist, &limit, *epsilon, 1);
	o->close = c != -2 && c < 0;
	if (o->close && !dry)
		close_light(light, caller);
	return 1;
}

// Verify mode (exp_lightarrayverify): each call runs twice. Pass 1 is the original script alone; pass 2 runs it again
// with the native body writing for real. For one caller the script is idempotent (close_lights restarts at 0; the
// array entries 0..close_lights-1 and each visited light's rdist are rewritten with the same values), so after each
// pass the verifier reads back through the runner the visited lights in order, every visited light's rdist (bits and
// kind), close_lights, and every array entry 0..close_lights-1; pass 2 must leave exactly what pass 1 left. Pass 2
// also checks that every light the native body rejected (another region, door ray blocked) kept its rdist. In verify
// mode collision_line runs once per pass for a cross-region light; it only queries the room (the runner may refresh
// its collision tree, which doesn't change the answer), so both passes see the same result.
#define REC_MAX 256
static int recording; // 1: pass 1, 2: pass 2
static void *rec_light[2][REC_MAX];
static int rec_n[2], rec_overflow;

static void record(void *light) {
	int p = recording - 1;
	if (rec_n[p] < REC_MAX)
		rec_light[p][rec_n[p]++] = light;
	else
		rec_overflow = 1;
}

typedef struct { int n, lights; RV rdist[REC_MAX], count, entry[REC_MAX]; } Snapshot;
static Snapshot snap[2];

static int same(const RV *a, const RV *b) {
	return !memcmp(&a->value, &b->value, 8) && (a->kind & 0xffffff) == (b->kind & 0xffffff);
}

static void take(int p, void *caller) {
	Snapshot *s = &snap[p];
	Table tc;
	s->lights = rec_n[p];
	for (int i = 0; i < rec_n[p]; i++) {
		Table tl;
		RV *r = table(rec_light[p][i], &tl) ? peek(&tl, RDIST) : NULL;
		if (r)
			s->rdist[i] = *r;
		else
			memset(&s->rdist[i], 0xff, sizeof(RV));
	}
	RV *count = table(caller, &tc) ? peek(&tc, CLOSE_LIGHTS) : NULL;
	memset(&s->count, 0xff, sizeof(RV));
	s->n = 0;
	if (count) {
		s->count = *count;
		int n = plain(count) && count->value > 0 ? (int)count->value : 0;
		RV inst = {0, 0, 6};
		memcpy(&inst.value, &caller, sizeof(caller));
		for (int i = 0; i < n && i < REC_MAX; i++) {
			RV e = {0, 0, 5};
			variable_get(&inst, VAR_ID(CLOSE_LIGHT_ARRAY), i, &e, 0, 0);
			if (refcounted(&e)) { // an instance id never is; a string or array would be a mismatch anyway
				release(&e);
				e.kind = 0xfffffe;
			}
			s->entry[i] = e;
			s->n++;
		}
	}
}

static void compare_passes(void) {
	Snapshot *a = &snap[0], *b = &snap[1];
	const char *why = NULL;
	if (rec_overflow)
		why = "too many lights to record";
	else if (rec_n[0] != rec_n[1] || memcmp(rec_light[0], rec_light[1], rec_n[0] * sizeof(void *)))
		why = "visited lights differ";
	else if (!same(&a->count, &b->count))
		why = "close_lights differs";
	else if (a->n != b->n)
		why = "array length differs";
	for (int i = 0; !why && i < a->lights; i++)
		if (!same(&a->rdist[i], &b->rdist[i]))
			why = "rdist differs";
	for (int i = 0; !why && i < a->n; i++)
		if (!same(&a->entry[i], &b->entry[i]))
			why = "array entry differs";
	if (!why) {
		light_array_verified++;
		light_array_entries_checked += a->n;
	} else if (light_array_mismatch++ < 8)
		debugPrintf("[LIGHTARRAY] mismatch: %s (lights %d/%d, close_lights %.0f/%.0f)\n", why, rec_n[0], rec_n[1],
			a->count.value, b->count.value);
}

// r: the original iterator's result (> 0: *self is the next light, *other the caller)
static int run(int r, void *iterator, void **self, void **other) {
	if (recording == 1 || (recording == 2 && !cur.ok)) { // the original body runs: only record what it visits
		if (r > 0)
			record(*self);
		return r;
	}
	if (!cur.ok)
		return r;
	while (r > 0) {
		Outcome o;
		RV before;
		RV *rd = NULL;
		Table tl;
		if (recording == 2 && table(*self, &tl) && (rd = peek(&tl, RDIST)))
			before = *rd;
		if (!body(*self, *other, 0, &o))
			break;
		if (recording == 2) {
			record(*self);
			if (!o.distance) { // rejected: rdist must be untouched
				RV *now = table(*self, &tl) ? peek(&tl, RDIST) : NULL;
				light_array_rejected_checked++;
				if ((now != NULL) != (rd != NULL) || (now && !same(now, &before))) {
					if (light_array_mismatch++ < 8)
						debugPrintf("[LIGHTARRAY] mismatch: a rejected light's rdist changed\n");
				}
			}
		}
		light_array_native++;
		r = next_with(iterator, self, other);
	}
	if (r > 0) {
		light_array_fallback++;
		if (recording == 2) { // the rest of this call runs in the original body
			cur.ok = 0;
			record(*self);
		}
	}
	return r;
}

static int ABI update_new_with(void *iterator, void **self, void **other, const void *target) {
	return run(new_with(iterator, self, other, target), iterator, self, other);
}

static int ABI update_next_with(void *iterator, void **self, void **other) {
	return run(next_with(iterator, self, other), iterator, self, other);
}

static RV *ABI update_entry(void *self, void *other, RV *result, int argc, RV **args) {
	typeof(cur) saved = cur;
	int saved_recording = recording;
	uint32_t start = sceKernelGetProcessTimeLow();
	cur.self = self;
	cur.argc = argc;
	cur.args = args;
	cur.ok = 0;
	cur.ok = call_ok();
	light_array_calls++;
	RV *r;
	if (verify && cur.ok) {
		rec_n[0] = rec_n[1] = rec_overflow = 0;
		recording = 1;
		cur.ok = 0;
		original_script(self, other, result, argc, args);
		take(0, self);
		recording = 2;
		cur.ok = 1;
		r = original_script(self, other, result, argc, args);
		take(1, self);
		compare_passes();
	} else {
		recording = 0;
		r = original_script(self, other, result, argc, args);
	}
	uint32_t dt = sceKernelGetProcessTimeLow() - start;
	light_array_us += dt;
	if (dt > light_array_max_us)
		light_array_max_us = dt;
	cur = saved;
	recording = saved_recording;
	return r;
}

static int check_call(uintptr_t site, uintptr_t plt, uintptr_t direct) {
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	return (ins >> 24) == 0xeb && (target == plt || target == direct);
}

// A veneer for a bl at site (in the code cave, within bl range), or NULL
static uint32_t *veneer_for(uintptr_t site) {
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	int32_t off = (int32_t)((uintptr_t)veneer - (site + 8));
	return veneer && off >= -(1 << 25) && off < (1 << 25) ? veneer : NULL;
}

// Points the bl at site to the veneer, which jumps to wrapper
static void patch_call(uintptr_t site, uint32_t *veneer, uintptr_t wrapper) {
	uint32_t code[2] = {0xe51ff004, wrapper}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	int32_t off = (int32_t)((uintptr_t)veneer - (site + 8));
	uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
}

int light_array_install(void *(*rx_alloc)(size_t)) {
	// ELF addresses in APK 1.1.10: the script, its with target (movw r0, #621 = obj_lightsource), its iterator call
	// sites and the PLT entries they call
	enum { SCRIPT = 0x338756c, TARGET = 0x3387734, NEW_SITE = 0x3387758, NEXT_SITE = 0x3388380,
	       PLT_NEW = 0x4ab0e00, PLT_NEXT = 0x4ab0e30 };
#define RESOLVE(v, t, n) do { v = (t)so_symbol(&yoyoloader_mod, n); if (!v) return 0; } while (0)
	uintptr_t entry;
	RESOLVE(entry, uintptr_t, "_Z29gml_Script_update_light_arrayP9CInstanceS0_R8YYRValueiPPS1_");
	RESOLVE(new_with, new_fn, "_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	RESOLVE(next_with, next_fn, "_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
#define AT(a) (entry + (a) - SCRIPT)
	uint32_t *w = (uint32_t *)entry;
	if ((entry & 3) || w[0] != 0xe92d4ff0 || w[1] != 0xe28db01c || *(uint32_t *)AT(TARGET) != 0xe300026d ||
			!check_call(AT(NEW_SITE), AT(PLT_NEW), (uintptr_t)new_with) ||
			!check_call(AT(NEXT_SITE), AT(PLT_NEXT), (uintptr_t)next_with)) {
		debugPrintf("light array: unexpected code in update_light_array\n");
		return 0;
	}
	RESOLVE(standard_get, ref_fn, "_ZN12YYObjectBase19InternalGetYYVarRefEi");
	RESOLVE(standard_ref, ref_fn, "_ZN12YYObjectBase20InternalGetYYVarRefLEi");
	RESOLVE(bool_value, bool_fn, "_Z11BOOL_RValuePK6RValue");
	RESOLVE(compare, compare_fn, "_Z12YYCompareValRK6RValueS1_db");
	RESOLVE(legacy_call, legacy_fn, "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	RESOLVE(variable_get, getvalue_fn, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	RESOLVE(variable_set, setvalue_fn, "_Z23YYGML_Variable_SetValueRK8YYRValueiiP6RValue");
	RESOLVE(int32_value, int32_fn, "_Z12INT32_RValuePK6RValue");
	RESOLVE(add_eq, addeq_fn, "_ZN8YYRValuepLEi");
	RESOLVE(release, free_fn, "_Z16FREE_RValue__PreP6RValue");
	RESOLVE(push_context, push_fn, "_Z16PushContextStackP12YYObjectBase");
	RESOLVE(pop_context, pop_fn, "_Z15PopContextStacki");
	RESOLVE(functions, Function **, "the_functions");
	RESOLVE(current_function, Function **, "g_pFunction");
	RESOLVE(epsilon, const double *, "g_GMLMathEpsilon");
	RESOLVE(builtin_variables, const void *, "builtin_variables");
	RESOLVE(builtin_count, const int *, "g_nStartGlobalVariables");
	RESOLVE(xy_get[0], uintptr_t, "_Z4GV_XP9CInstanceiP6RValue");
	RESOLVE(xy_get[1], uintptr_t, "_Z4GV_YP9CInstanceiP6RValue");
	for (int n = 0; n < NVARS; n++)
		RESOLVE(var_desc[n], const int *, var_names[n]);
	for (int n = 0; n < NFUNCS; n++) {
		RESOLVE(fn_desc[n], const int *, fn_names[n]);
		if (fn_symbols[n])
			RESOLVE(builtins[n], builtin_fn, fn_symbols[n]);
	}
#undef RESOLVE
	// The original, run from a trampoline: its first two instructions, then a jump back
	uint32_t *rx = rx_alloc(16);
	if (!rx)
		return 0;
	uint32_t code[4] = {w[0], w[1], 0xe51ff004, (uint32_t)(entry + 8)};
	kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof(code));
	kuKernelFlushCaches(rx, sizeof(code));
	original_script = (script_fn)rx;
	verify = exp_flag("lightarrayverify");
	// lightArrayFast: F_PointDistance must be the code the inline distance copies (its bl words, to YYGetFloat, vary
	// with patch_plt_calls and are not compared)
	static const uint32_t pd[34] = {0xe92d48f0, 0xe28db010, 0xed2d8b02, 0xe59b5008, 0xe1a04000, 0xe3a00000, 0xe3a01002,
		0xe584000c, 0xe1a00005, 0, 0xe1a06000, 0xe1a00005, 0xe3a01000, 0, 0xe1a07000, 0xe1a00005, 0xe3a01003, 0,
		0xee080a10, 0xe1a00005, 0xe3a01001, 0, 0xee000a10, 0xee016a10, 0xee380a40, 0xee027a10, 0xee311a42, 0xee200a00,
		0xee010a01, 0xeeb10ac0, 0xeef70ac0, 0xedc40b00, 0xecbd8b02, 0xe8bd88f0};
	const uint32_t *pdc = (const uint32_t *)(uintptr_t)builtins[DISTANCE];
	fast = light_array_fast_on;
	for (int i = 0; i < 34 && fast; i++)
		if (pd[i] ? pdc[i] != pd[i] : (pdc[i] >> 24) != 0xeb)
			fast = 0;
	get_direct = (getdirect_fn)so_symbol(&yoyoloader_mod, "_Z24Variable_GetValue_DirectP12YYObjectBaseiiP6RValuebb");
	set_direct = (setdirect_fn)so_symbol(&yoyoloader_mod, "_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
	context_stack_base = (void **)so_symbol(&yoyoloader_mod, "g_ContextStack");
	context_top = (int *)so_symbol(&yoyoloader_mod, "g_ContextStackTop");
	context_max = (int *)so_symbol(&yoyoloader_mod, "g_ContextStackMax");
	garbage_collection = (const uint8_t *)so_symbol(&yoyoloader_mod, "g_fGarbageCollection");
	if (!context_stack_base || !context_top || !context_max || !garbage_collection)
		fast = 0;
	fast_ab = fast && exp_flag("lightarrayfastab");
	fast_fault = fast && verify && exp_flag("lightarrayfastfault");
	close_fault = fast && verify && exp_flag("lightarrayclosefault");
	uint32_t *new_veneer = veneer_for(AT(NEW_SITE)), *next_veneer = new_veneer ? veneer_for(AT(NEXT_SITE)) : NULL;
	if (!next_veneer) {
		debugPrintf("light array: no room for the iterator veneers\n");
		return 0;
	}
	patch_call(AT(NEW_SITE), new_veneer, (uintptr_t)&update_new_with);
	patch_call(AT(NEXT_SITE), next_veneer, (uintptr_t)&update_next_with);
	hook_raw(entry, (uintptr_t)&update_entry);
	kuKernelFlushCaches((void *)entry, 8);
#undef AT
	debugPrintf("light array: native loop body%s\n", verify ? " (verify mode)" : "");
	return 1;
}

void light_array_fast_report(int frames) {
	if (light_array_fast || fast_ab)
		debugPrintf("[LIGHTARRAYFAST] lightArrayFast %d: %.1f lights, %.1f close-light writes inline per frame%s\n", fast,
			(float)light_array_fast / (frames > 0 ? frames : 1), (float)light_array_close_fast / (frames > 0 ? frames : 1),
			fast_ab ? (fast ? " | A/B: on" : " | A/B: off") : "");
	light_array_fast = light_array_close_fast = 0;
	if (fast_ab)
		fast = !fast;
}
