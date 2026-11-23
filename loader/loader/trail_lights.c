/*
 * trail_lights.c
 *
 * The player's afterimage trails (obj_motion_trail, made by player_moving_ground and the other movement scripts while
 * running, rolling, jumping...) are drawn by obj_drawactors through the trail's user event 15. A trail of type != 0
 * tints itself from the lights around it (APK 1.1.10, gml_Object_obj_motion_trail_Other_25, GML lines 50-60):
 *
 *     with (obj_lightsource)                                              // outer with, iterator at sp+0x150
 *         if (point_distance(x, y, other.x, other.y) < 200)
 *             with (other) {                                              // inner with (the trail), sp+0x140
 *                 i = 0;
 *                 repeat (dist / global.trail_smoothness) {
 *                     draw_sprite_ext(sprite_index, animation_index,
 *                         x + lengthdir_x(i, trail_dir) + random(2) * global.fr - random(2) * global.fr,
 *                         y + lengthdir_y(i, trail_dir) + random(2) * global.fr - random(2) * global.fr,
 *                         2 * dir, 2, 0, other.color,
 *                         (1 - point_distance(x, y - 30, other.x, other.y) / 200) * (alpha * other.alpha)
 *                             / (global.trail_alpha / alpha_scale));
 *                     i += global.trail_smoothness;
 *                 }
 *             }
 *
 * so every live trail runs the distance test in GML against every light in the room, every frame, and the draw loop in
 * GML for every light near it. room_club_7 has 66 lights and ~8 trails while the player runs (~4 lights near each):
 * 1.9 ms per frame, the main part of its fps drop while moving.
 *
 * Both with loops keep their original code and order; only their iterator calls (YYGML_NewWithIterator /
 * YYGML_WithIteratorNext) are redirected, through veneers in the code cave after .text, to these wrappers:
 * - Outer loop: lights at least 202 px from the trail (the instances' float x/y, the values GML reads) are stepped over.
 *   For such a light the original body only computed point_distance (a pure builtin) and failed the test: no draw, no
 *   random(), no variable write. Lights that could pass (two pixels of margin over any rounding) still run it.
 * - Inner loop (one instance, the trail): the body above runs natively, calling the runner's own lengthdir_x/y, random,
 *   point_distance and draw_sprite_ext with the same values in the same order and the same double arithmetic, then
 *   the wrapper reports an empty with, so the original jumps to its DeleteWithIterator (which restores self/other).
 *   Anything unexpected (a value that isn't a plain real, a zero divisor, a huge repeat count) runs the original body.
 * So the drawing, the random number sequence and every variable stay exactly as before.
 *
 * exp_traillights0 keeps the original. exp_traillightsnative0 keeps only the outer-loop skipping.
 * Debug (profiling report, [TRAILLIGHTS]):
 * - exp_traillightscheck runs the runner's own point_distance for every skipped light and counts any under 200.
 * - exp_traillightsverify runs the native body without drawing, restores the random state, lets the original body run
 *   with its draw_sprite_ext calls recorded, and compares draws (all arguments bit for bit), the final i and the
 *   random state afterwards. Mismatches must stay 0.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <kubridge.h>

#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef int ABI (*new_fn)(void *iterator, void **self, void **other, const void *target);
typedef int ABI (*next_fn)(void *iterator, void **self, void **other);
typedef void ABI (*builtin_fn)(RV *result, void *self, void *other, int argc, RV *args);
typedef RV *ABI (*var_fn)(void *instance, int id);
typedef float ABI (*lengthdir_fn)(float length, float direction);
typedef double ABI (*random_fn)(double range);
typedef void ABI (*draw_fn)(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
	int colour, float alpha);
typedef void ABI (*getvalue_fn)(const RV *instance, int id, int index, RV *out, int, int);
typedef double ABI (*real_fn)(const RV *value);

static new_fn new_with;
static next_fn next_with;
static builtin_fn point_distance;
static lengthdir_fn lengthdir_x, lengthdir_y;
static random_fn random_fn_;
static draw_fn draw_sprite_ext;
static getvalue_fn variable_get;
static real_fn real_value;
static void **global_obj;
static uint32_t *rng_state, *rng_index;
static int check, native, verify;

enum { SPRITE_INDEX, ANIMATION_INDEX, I, TRAIL_DIR, DIR, COLOR, ALPHA, DIST, ALPHA_SCALE, TRAIL_SMOOTHNESS,
	TRAIL_ALPHA, FR, BLACKOUT, NVARS };
static const char *const var_names[NVARS] = {"g_VAR_sprite_index", "g_VAR_animation_index", "g_VAR_i",
	"g_VAR_trail_dir", "g_VAR_dir", "g_VAR_color", "g_VAR_alpha", "g_VAR_dist", "g_VAR_alpha_scale",
	"g_VAR_trail_smoothness", "g_VAR_trail_alpha", "g_VAR_fr", "g_VAR_blackout"};
static const int *var_desc[NVARS]; // {name, id}: the runner fills the ids at game start, so read them live
#define VAR_ID(n) (var_desc[n][1])

unsigned trail_lights_kept, trail_lights_skipped, trail_lights_violations;
unsigned trail_lights_native, trail_lights_fallback, trail_lights_verified, trail_lights_mismatch;

#define MARGIN_SQ (202.0 * 202.0)

static float fx(void *p) { float v; memcpy(&v, (uint8_t *)p + 0xb8, 4); return v; }
static float fy(void *p) { float v; memcpy(&v, (uint8_t *)p + 0xbc, 4); return v; }

static int far_apart(void *light, void *trail) {
	if (!light || !trail)
		return 0;
	float lx = fx(light), ly = fy(light), tx = fx(trail), ty = fy(trail);
	if (!isfinite(lx) || !isfinite(ly) || !isfinite(tx) || !isfinite(ty))
		return 0;
	double dx = (double)lx - (double)tx, dy = (double)ly - (double)ty;
	if (dx * dx + dy * dy <= MARGIN_SQ)
		return 0;
	if (check && point_distance) {
		// What the original computes: point_distance(x, y, other.x, other.y) in the light's scope
		RV args[4] = {{(double)lx, 0, 0}, {(double)ly, 0, 0}, {(double)tx, 0, 0}, {(double)ty, 0, 0}}, out = {0, 0, 0};
		point_distance(&out, light, trail, 4, args);
		if (out.kind != 0 || out.value < 200.0)
			trail_lights_violations++;
	}
	return 1;
}

// r: the original's result (> 0: *self is the next light). Steps over far lights; *other stays the trail.
static int skip_far(int r, void *iterator, void **self, void **other) {
	while (r > 0 && far_apart(*self, *other)) {
		trail_lights_skipped++;
		r = next_with(iterator, self, other);
	}
	if (r > 0)
		trail_lights_kept++;
	return r;
}

static int ABI trail_new_with(void *iterator, void **self, void **other, const void *target) {
	return skip_far(new_with(iterator, self, other, target), iterator, self, other);
}

static int ABI trail_next_with(void *iterator, void **self, void **other);

// ---- Native inner body ----

typedef struct { int sprite, colour; float subimg, x, y, xs, ys, rot, alpha; } Draw;
#define MAX_DRAWS 64
static Draw nat_draws[MAX_DRAWS], gml_draws[MAX_DRAWS];
static int nat_count, gml_count, capturing, pending;
static double nat_i;
static void *pending_trail;
static uint32_t nat_rng[17];

static RV *get(void *inst, int n) { return (*(var_fn *)(*(uint8_t **)inst + 8))(inst, VAR_ID(n)); }
static RV *ref(void *inst, int n) { return (*(var_fn *)(*(uint8_t **)inst + 12))(inst, VAR_ID(n)); }
static int real(const RV *v) { return v && (v->kind & 0xffffff) == 0; }

// Why the native body fell back: source line -> count (profiling report)
unsigned trail_lights_reject_line[8], trail_lights_reject_count[8];
static int reject(unsigned line) {
	for (int k = 0; k < 8; k++)
		if (!trail_lights_reject_line[k] || trail_lights_reject_line[k] == line) {
			trail_lights_reject_line[k] = line;
			trail_lights_reject_count[k]++;
			break;
		}
	return 0;
}
#define REJECT() return reject(__LINE__)

static void ABI draw_capture(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
		int colour, float alpha) {
	if (capturing) { // counted past MAX_DRAWS too: verify_finish treats an overflow as a mismatch
		if (gml_count < MAX_DRAWS)
			gml_draws[gml_count] = (Draw){sprite, colour, subimg, x, y, xs, ys, rot, alpha};
		gml_count++;
	}
	draw_sprite_ext(self, sprite, subimg, x, y, xs, ys, rot, colour, alpha);
}

// self: the trail, other: the light (inside with (other) in the light's scope). Returns 0 to fall back to the original.
static int native_body(void *self, void *other, int dry) {
	if (!*global_obj)
		REJECT();
	for (int n = 0; n < NVARS; n++)
		if (VAR_ID(n) < 0)
			REJECT();
	RV *g_smooth = get(*global_obj, TRAIL_SMOOTHNESS), *g_alpha = get(*global_obj, TRAIL_ALPHA);
	RV *g_fr = get(*global_obj, FR), *g_black = get(*global_obj, BLACKOUT);
	if (!real(g_smooth) || !real(g_alpha) || !real(g_fr) || !g_black || ((g_black->kind & 0xffffff) != 0 && (g_black->kind & 0xffffff) != 13) || g_black->value != 0)
		REJECT();
	RV *alpha_scale = get(self, ALPHA_SCALE), *dist = get(self, DIST);
	if (!real(alpha_scale) || !real(dist) || alpha_scale->value == 0 || g_smooth->value == 0)
		REJECT();
	double smooth = g_smooth->value, fr = g_fr->value, l100 = g_alpha->value / alpha_scale->value;
	if (l100 == 0)
		REJECT();
	double count = dist->value / smooth;
	if (!(count > -2147483648.0 && count < 1000.0))
		REJECT();
	int n = (int)count; // INT32_RValue: vcvt.s32.f64
	// The values the original reads in the loop (nothing in it changes them)
	RV inst = {0, 0, 6}, spr = {0, 0, 0xffffff};
	memcpy(&inst.value, &self, sizeof(self));
	variable_get(&inst, VAR_ID(SPRITE_INDEX), (int)0x80000000, &spr, 0, 0);
	RV *anim = get(self, ANIMATION_INDEX), *tdir = get(self, TRAIL_DIR), *dir = get(self, DIR);
	RV *colour = get(other, COLOR), *salpha = get(self, ALPHA), *lalpha = get(other, ALPHA);
	// sprite_index is an asset reference (kind 15); the original converts it with REAL_RValue_Ex, then to an integer
	if ((spr.kind & 0xffffff) != 0 && (spr.kind & 0xffffff) != 15)
		REJECT();
	double spr_value = (spr.kind & 0xffffff) == 0 ? spr.value : real_value(&spr);
	if (!(spr_value >= 0 && spr_value < 1e6))
		REJECT();
	if (!real(anim) || !real(tdir) || !real(dir))
		REJECT();
	if (!real(colour) || !(fabs(colour->value) < 9e18))
		REJECT();
	if (!real(salpha) || !real(lalpha))
		REJECT();
	RV *i_slot = ref(self, I);
	if (!i_slot || ((1u << (i_slot->kind & 0x1f)) & 0x46))
		REJECT();
	int sprite = (int)(int64_t)spr_value, col = (int)(int64_t)colour->value;
	double sx = fx(self), sy = fy(self), lx = fx(other), ly = fy(other);
	// point_distance(x, y - 30, other.x, other.y): a pure builtin with the same arguments on every pass
	RV args[4] = {{sx, 0, 0}, {sy - 30.0, 0, 0}, {lx, 0, 0}, {ly, 0, 0}}, pd = {0, 0, 0};
	point_distance(&pd, self, other, 4, args);
	if (!real(&pd))
		REJECT();
	double i = 0;
	if (dry)
		nat_count = 0;
	for (int k = 0; k < n; k++) {
		double x = sx + (double)lengthdir_x((float)i, (float)tdir->value);
		double r = random_fn_(2.0) * fr;
		x = x + r;
		r = random_fn_(2.0) * fr;
		x = x - r;
		double y = sy + (double)lengthdir_y((float)i, (float)tdir->value);
		r = random_fn_(2.0) * fr;
		y = y + r;
		r = random_fn_(2.0) * fr;
		y = y - r;
		double xs = 2.0 * dir->value;
		double a = 1.0 - pd.value / 200.0;
		a = a * (salpha->value * lalpha->value);
		a = a / l100;
		if (dry) {
			if (nat_count < MAX_DRAWS)
				nat_draws[nat_count] = (Draw){sprite, col, (float)anim->value, (float)x, (float)y, (float)xs, 2.0f,
					0.0f, (float)a};
			nat_count++;
		} else {
			draw_sprite_ext(self, sprite, (float)anim->value, (float)x, (float)y, (float)xs, 2.0f, 0.0f, col, (float)a);
		}
		i = i + smooth;
	}
	if (dry) {
		nat_i = i;
	} else {
		i_slot->value = i;
		i_slot->kind = 0;
	}
	return 1;
}

static void verify_finish(void) {
	pending = 0;
	capturing = 0;
	RV *i_slot = get(pending_trail, I);
	int ok = gml_count == nat_count && nat_count <= MAX_DRAWS && !memcmp(gml_draws, nat_draws, sizeof(Draw) * nat_count) &&
		real(i_slot) && !memcmp(&i_slot->value, &nat_i, 8) &&
		!memcmp(rng_state, nat_rng, 64) && *rng_index == nat_rng[16];
	if (ok) {
		trail_lights_verified++;
	} else if (trail_lights_mismatch++ < 8) {
		debugPrintf("[TRAILLIGHTS] mismatch: draws %d/%d i %.9g/%.9g rng %s\n", gml_count, nat_count,
			real(i_slot) ? i_slot->value : -1.0, nat_i, !memcmp(rng_state, nat_rng, 64) && *rng_index == nat_rng[16] ? "same" : "differs");
		for (int k = 0; k < gml_count && k < nat_count && k < MAX_DRAWS && k < 4; k++)
			if (memcmp(&gml_draws[k], &nat_draws[k], sizeof(Draw)))
				debugPrintf("[TRAILLIGHTS]   draw %d: gml spr %d col %d %.9g %.9g %.9g %.9g %.9g | native spr %d col %d %.9g %.9g %.9g %.9g %.9g\n", k,
					gml_draws[k].sprite, gml_draws[k].colour, gml_draws[k].subimg, gml_draws[k].x, gml_draws[k].y,
					gml_draws[k].xs, gml_draws[k].alpha, nat_draws[k].sprite, nat_draws[k].colour, nat_draws[k].subimg,
					nat_draws[k].x, nat_draws[k].y, nat_draws[k].xs, nat_draws[k].alpha);
	}
}

static int ABI trail_inner_new_with(void *iterator, void **self, void **other, const void *target) {
	int r = new_with(iterator, self, other, target);
	if (r != 1 || !*self || !*other)
		return r;
	if (verify) {
		uint32_t saved[17];
		memcpy(saved, rng_state, 64);
		saved[16] = *rng_index;
		if (!native_body(*self, *other, 1)) {
			memcpy(rng_state, saved, 64);
			*rng_index = saved[16];
			trail_lights_fallback++;
			return r;
		}
		memcpy(nat_rng, rng_state, 64);
		nat_rng[16] = *rng_index;
		memcpy(rng_state, saved, 64);
		*rng_index = saved[16];
		pending = 1;
		pending_trail = *self;
		gml_count = 0;
		capturing = 1;
		return r; // the original body runs and is compared at the outer loop's next step
	}
	if (native_body(*self, *other, 0)) {
		trail_lights_native++;
		return 0; // the original goes straight to DeleteWithIterator
	}
	trail_lights_fallback++;
	return r;
}

static int ABI trail_next_with(void *iterator, void **self, void **other) {
	if (pending)
		verify_finish();
	return skip_far(next_with(iterator, self, other), iterator, self, other);
}

// An 8-byte veneer within bl range of site, or NULL
static uint32_t *alloc_veneer(uintptr_t site) {
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	int32_t off = veneer ? (int32_t)((uintptr_t)veneer - (site + 8)) : 0;
	return veneer && off >= -(1 << 25) && off < (1 << 25) ? veneer : NULL;
}

static int patch_call_with(uintptr_t site, uintptr_t plt, uintptr_t direct, uintptr_t wrapper, uint32_t *veneer) {
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	if (!veneer || (ins >> 24) != 0xeb || (target != plt && target != direct))
		return 0;
	int32_t off = (int32_t)((uintptr_t)veneer - (site + 8));
	uint32_t code[2] = {0xe51ff004, wrapper}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
	return 1;
}

static int patch_call(uintptr_t site, uintptr_t plt, uintptr_t direct, uintptr_t wrapper) {
	return patch_call_with(site, plt, direct, wrapper, alloc_veneer(site));
}

static int check_call(uintptr_t site, uintptr_t plt, uintptr_t direct) {
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	return (ins >> 24) == 0xeb && (target == plt || target == direct);
}

int trail_lights_install(void) {
	// ELF addresses in APK 1.1.10: the event, the outer with's target (movw r0, #621 = obj_lightsource), the
	// iterator call sites of both with loops, the inner loop's draw_sprite_ext call and the PLT entries they call
	enum { EVENT = 0x43d3560, TARGET = 0x43d4140, NEW_SITE = 0x43d4160, NEXT_SITE = 0x43d5488,
	       INNER_NEW_SITE = 0x43d4620, DRAW_SITE = 0x43d5224,
	       PLT_NEW = 0x4ab0e00, PLT_NEXT = 0x4ab0e30, PLT_DRAW = 0x4ab0e90 };
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z36gml_Object_obj_motion_trail_Other_25P9CInstanceS0_");
	new_with = (new_fn)so_symbol(&yoyoloader_mod, "_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	next_with = (next_fn)so_symbol(&yoyoloader_mod, "_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
	point_distance = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_");
	if (!entry || !new_with || !next_with)
		return 0;
	// The prologue may be relocated by a profiling entry hook; the body must be the 1.1.10 one
	uint32_t *w = (uint32_t *)entry;
	if (!((w[0] == 0xe92d4ff0 && w[1] == 0xe28db01c) || w[0] == 0xe51ff004) || *(uint32_t *)(entry + TARGET - EVENT) != 0xe300026d)
		return 0;
#define AT(a) (entry + (a) - EVENT)
	if (!check_call(AT(NEW_SITE), AT(PLT_NEW), (uintptr_t)new_with) || !check_call(AT(NEXT_SITE), AT(PLT_NEXT), (uintptr_t)next_with))
		return 0;
	// The native inner body: everything it calls must resolve, and its call sites must match
	lengthdir_x = (lengthdir_fn)so_symbol(&yoyoloader_mod, "_Z17YYGML_lengthdir_xff");
	lengthdir_y = (lengthdir_fn)so_symbol(&yoyoloader_mod, "_Z17YYGML_lengthdir_yff");
	random_fn_ = (random_fn)so_symbol(&yoyoloader_mod, "_Z12YYGML_randomd");
	draw_sprite_ext = (draw_fn)so_symbol(&yoyoloader_mod, "_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
	variable_get = (getvalue_fn)so_symbol(&yoyoloader_mod, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	real_value = (real_fn)so_symbol(&yoyoloader_mod, "_Z14REAL_RValue_ExPK6RValue");
	global_obj = (void **)so_symbol(&yoyoloader_mod, "g_pGlobal");
	rng_state = (uint32_t *)so_symbol(&yoyoloader_mod, "state");
	rng_index = (uint32_t *)so_symbol(&yoyoloader_mod, "g_RndIndex");
	int vars_ok = 1;
	for (int n = 0; n < NVARS; n++) {
		var_desc[n] = (const int *)so_symbol(&yoyoloader_mod, var_names[n]);
		vars_ok = vars_ok && var_desc[n];
	}
	native = !exp_flag("traillightsnative0") && vars_ok && point_distance && lengthdir_x && lengthdir_y && random_fn_ &&
		draw_sprite_ext && variable_get && real_value && global_obj && check_call(AT(INNER_NEW_SITE), AT(PLT_NEW), (uintptr_t)new_with);
	verify = native && rng_state && rng_index && exp_flag("traillightsverify") && check_call(AT(DRAW_SITE), AT(PLT_DRAW), (uintptr_t)draw_sprite_ext);
	// Both veneers first, so a failed allocation leaves neither site patched (both sites were checked above)
	uint32_t *new_veneer = alloc_veneer(AT(NEW_SITE)), *next_veneer = alloc_veneer(AT(NEXT_SITE));
	if (!new_veneer || !next_veneer ||
			!patch_call_with(AT(NEW_SITE), AT(PLT_NEW), (uintptr_t)new_with, (uintptr_t)&trail_new_with, new_veneer) ||
			!patch_call_with(AT(NEXT_SITE), AT(PLT_NEXT), (uintptr_t)next_with, (uintptr_t)&trail_next_with, next_veneer))
		return 0;
	if (native && !patch_call(AT(INNER_NEW_SITE), AT(PLT_NEW), (uintptr_t)new_with, (uintptr_t)&trail_inner_new_with))
		native = verify = 0;
	if (verify && !patch_call(AT(DRAW_SITE), AT(PLT_DRAW), (uintptr_t)draw_sprite_ext, (uintptr_t)&draw_capture))
		verify = 0;
#undef AT
	check = exp_flag("traillightscheck");
	debugPrintf("trail lights: native inner body %s%s\n", native ? "on" : "off", verify ? " (verify mode)" : "");
	return 1;
}
