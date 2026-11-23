/*
 * particle_native.c
 *
 * obj_particle user event 15 (gml_Object_obj_particle_Other_25, APK 1.1.10) is the draw routine of every particle
 * that inherits it: the roll's dust clouds, smoke puffs, debris... obj_drawactors runs it for each one every frame:
 *
 *     if (point_distance(x, y, xprevious, yprevious) > 1 || region == -1) update_region();          // GML 1-3
 *     if (color_me) {
 *         if ((global.blackout || blackout) && !spark) {           // two dark silhouette passes (10-19)
 *             var a = image_alpha, c = image_blend;
 *             image_blend = 4210752; image_alpha = 1; draw_self();
 *             image_blend = c_black; image_alpha = 0.75; draw_self();
 *             image_alpha = a; image_blend = c;
 *         } else draw_self();                                       // 27
 *         if (!spark && !shading) {                                 // 30
 *             image_alpha *= 1.5;                                   // 32
 *             if (prev_recolor_timer_val != global.recolor_timer)   // 33
 *                 draw_sprite_recolored(sprite_index, image_index, x, y, image_xscale, image_yscale, image_angle, false);
 *             else draw_sprite_ext(sprite_index, image_index, x, y, image_xscale, image_yscale, image_angle,
 *                                  light_col, light_alpha);         // 39
 *             image_alpha /= 1.5;                                   // 41
 *         }
 *     } else draw_self();                                           // 46
 *
 * In YYC that is ~3,000-4,200 instructions of getters, RValue temporaries and conversions per particle; on the Vita
 * ~35 us per dust cloud besides its draws (rolling: ~12 clouds). This runs the same event natively, exactly:
 * - every call with an effect or a callee is the original's own, in its order: update_region, draw_sprite_recolored,
 *   draw_self and draw_sprite_ext through the event's own call targets (PLT stubs or, PLT-patched, the functions),
 *   Variable_SetValue_Direct for every write, the instance/global getters through their vtables (as YYC does);
 * - builtin reads go straight to the builtin_variables getter Variable_GetValue_Direct would call (id within the
 *   builtin range, an instance); x/y/xprevious/yprevious read the floats those getters (checked) widen;
 * - point_distance is F_PointDistance's single-precision math (its registry entry checked), YYCompareVal's real-real
 *   arm and BOOL_RValue of a real/bool are inlined (any other kind goes to the runner's own function); the cached
 *   draw's conversions are REAL_RValue_Ex (non-reals), the library's own __aeabi_d2lz copy for the sprite,
 *   INT64_RValue for the colour, the same float narrowing; draw_sprite_recolored's eighth argument is gs_constFalse.
 * The checks (an instance, the variable ids, the builtin getters, point_distance's registry entry) all come before
 * anything is called; if one fails the original event runs.
 *
 * exp_particlenative0 keeps the original. [PARTICLE] report line: native / original calls. exp_particleab (debug):
 * the original in every other pair of report windows (in-run A/B, ~4 s per state). exp_particleverify (debug
 * mode): every call first runs the native event dry (its draws and script calls recorded, not made; its setter writes
 * real), keeps the instance's fields 0x7c-0x100 (everything those setters write) and puts them back, then runs the
 * original with its call sites (update_region, the four draw_self, draw_sprite_recolored, draw_sprite_ext) going
 * through veneers that record and forward; the two call records (draw_self: the draw fields at the time; the other
 * calls: all arguments) and the final fields must match. A dry update_region or draw_sprite_recolored writes nothing
 * the event reads afterwards (region / update_light_timer; the light_* variables), so the dry run sees what the
 * original sees.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#ifndef PARTICLE_NATIVE_TEST
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef void (*event_fn)(void *self, void *other);
typedef RV *ABI (*ref_fn)(void *instance, int id);
typedef int ABI (*bool_fn)(const RV *value);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double epsilon, int less);
typedef void ABI (*getvalue_fn)(const RV *instance, int id, int index, RV *out, int, int);
typedef void ABI (*setdirect_fn)(void *instance, int id, int index, RV *value);
typedef int ABI (*builtin_get_fn)(void *instance, int index, RV *out);
typedef RV *ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);
typedef void ABI (*draw_self_fn)(void *self);
typedef void ABI (*draw_ext_fn)(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
	int colour, float alpha);
typedef double ABI (*real_fn)(const RV *value);
typedef int64_t ABI (*int64_fn)(const RV *value);
typedef int64_t ABI (*d2l_fn)(double value);
typedef void ABI (*free_fn)(RV *value);
typedef struct { const char *name; void *call; int argc; } Function;
typedef struct Trace { struct Trace *prev; const char *name; int line; } Trace; // SYYStackTrace

static event_fn original;
static bool_fn bool_value;
static compare_fn compare;
static getvalue_fn variable_get;
static setdirect_fn variable_set;
static real_fn real_value;
static int64_fn int64_value;
static free_fn release;
static script_fn update_region, draw_sprite_recolored;
static draw_self_fn draw_self;
static draw_ext_fn draw_sprite_ext;
static d2l_fn d2l;
static void *point_distance;
static Function **functions;
static const int *distance_desc;
static const double *epsilon;
static const uint8_t *builtin_variables;
static const int *builtin_count;
static const RV *const_false;
static void **global_obj;
static Trace **trace_start;
// The float getters (checked): GV_X, GV_Y, GV_XPrevious, GV_YPrevious, GV_ImageXScale, GV_ImageYScale, GV_ImageAngle,
// GV_ImageAlpha widen the instance's float at these offsets into a real
enum { NFLOAT = 8 };
static uintptr_t float_getters[NFLOAT];
static const char *const float_getter_names[NFLOAT] = {"_Z4GV_XP9CInstanceiP6RValue", "_Z4GV_YP9CInstanceiP6RValue",
	"_Z12GV_XPreviousP9CInstanceiP6RValue", "_Z12GV_YPreviousP9CInstanceiP6RValue",
	"_Z14GV_ImageXScaleP9CInstanceiP6RValue", "_Z14GV_ImageYScaleP9CInstanceiP6RValue",
	"_Z13GV_ImageAngleP9CInstanceiP6RValue", "_Z13GV_ImageAlphaP9CInstanceiP6RValue"};
static const int float_vars[NFLOAT] = {0, 1, 2, 3, 6, 7, 8, 9}; // X, Y, XPREVIOUS, YPREVIOUS, IMAGE_XSCALE..ALPHA
static const uint16_t float_offsets[NFLOAT] = {0xb8, 0xbc, 0xc8, 0xcc, 0xa4, 0xa8, 0xac, 0xb0};
static int8_t float_slot[32]; // var -> index in float_vars, or -1
static int enabled;

enum { X, Y, XPREVIOUS, YPREVIOUS, SPRITE_INDEX, IMAGE_INDEX, IMAGE_XSCALE, IMAGE_YSCALE, IMAGE_ANGLE, IMAGE_ALPHA,
       IMAGE_BLEND, BLACKOUT, RECOLOR_TIMER, REGION, COLOR_ME, SPARK, SHADING, PREV_RECOLOR, LIGHT_COL, LIGHT_ALPHA,
       NVARS };
static const char *const var_names[NVARS] = {"g_VAR_x", "g_VAR_y", "g_VAR_xprevious", "g_VAR_yprevious",
	"g_VAR_sprite_index", "g_VAR_image_index", "g_VAR_image_xscale", "g_VAR_image_yscale", "g_VAR_image_angle",
	"g_VAR_image_alpha", "g_VAR_image_blend", "g_VAR_blackout", "g_VAR_recolor_timer", "g_VAR_region",
	"g_VAR_color_me", "g_VAR_spark", "g_VAR_shading", "g_VAR_prev_recolor_timer_val", "g_VAR_light_col",
	"g_VAR_light_alpha"};
static const int *var_desc[NVARS]; // {name, id}: the runner fills the ids at game start, so read them live
#define VAR_ID(n) (var_desc[n][1])
#define NONE ((int)0x80000000)

unsigned particle_native_calls, particle_native_fallbacks, particle_verified, particle_mismatch;
int particle_ab, particle_ab_off; // exp_particleab (debug): the original in every other pair of report windows

static uint32_t word(const void *p, size_t o) { uint32_t v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }
static float field(const void *p, size_t o) { float v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }
static int refcounted(const RV *v) { return (1u << (v->kind & 31)) & 0x46; }
static RV *vget(void *inst, int n) { return (*(ref_fn *)(*(uint8_t **)inst + 8))(inst, VAR_ID(n)); }
static int is_real(const RV *v) { return (v->kind & 0xffffff) == 0; }

// BOOL_RValue: a real or a bool is value > 0.5 (NaN false); other kinds go to the runner
static int truth(const RV *v) {
	uint32_t k = v->kind & 0xffffff;
	return k == 0 || k == 13 ? v->value > 0.5 : bool_value(v);
}
// YYCompareVal(a, b, epsilon, less): two reals with a non-NaN difference inline (sign, 0 within epsilon)
static int compare_values(const RV *a, const RV *b, int less) {
	if (is_real(a) && is_real(b)) {
		double d = a->value - b->value;
		if (d == d)
			return fabs(d) <= *epsilon ? 0 : d < 0 ? -1 : 1;
	}
	return compare(a, b, *epsilon, less);
}
// A builtin as Variable_GetValue_Direct gets it (checked builtin id, an instance): a float the checked getter widens,
// or that getter's own result
static RV bget(void *self, int n) {
	RV out = {0, 0, 5};
	if (float_slot[n] >= 0) {
		out.value = (double)field(self, float_offsets[(int)float_slot[n]]);
		out.kind = 0;
		return out;
	}
	((builtin_get_fn)(uintptr_t)word(builtin_variables, 16 * VAR_ID(n) + 4))(self, NONE, &out);
	return out;
}
static void set(void *self, int n, RV v) { variable_set(self, VAR_ID(n), NONE, &v); }
static RV real(double v) { RV r = {v, 0, 0}; return r; }
static double as_real(const RV *v) { return is_real(v) ? v->value : real_value(v); }

// The static checks (the variable ids, the builtin getters and point_distance's registry entry) hold from game start
// on: once they pass they are not repeated. Per call: an instance, a global object.
static int checks(void *self) {
	static int static_ok;
	if (word(self, 0x50) != 1 || !*global_obj)
		return 0;
	if (static_ok)
		return 1;
	if (!*functions || !isfinite(*epsilon) || *epsilon < 0)
		return 0;
	for (int n = 0; n < NVARS; n++)
		if (VAR_ID(n) < 0)
			return 0;
	for (int n = X; n <= IMAGE_BLEND; n++)
		if (VAR_ID(n) > 9999 || VAR_ID(n) >= *builtin_count)
			return 0;
	for (int k = 0; k < NFLOAT; k++)
		if (word(builtin_variables, 16 * VAR_ID(float_vars[k]) + 4) != float_getters[k])
			return 0;
	if (!(distance_desc[1] >= 0 && distance_desc[1] < 16384 && (*functions)[distance_desc[1]].call == point_distance))
		return 0;
	static_ok = 1;
	return 1;
}

static void native_event(void *self, void *other) {
	Trace trace = {*trace_start, "gml_Object_obj_particle_Other_25", 0};
	*trace_start = &trace;
	RV *g_blackout = vget(*global_obj, BLACKOUT), *g_timer = vget(*global_obj, RECOLOR_TIMER);
	// 1-3: point_distance(x, y, xprevious, yprevious) > 1 || region == -1
	trace.line = 1;
	float x1 = field(self, 0xb8), y1 = field(self, 0xbc), x2 = field(self, 0xc8), y2 = field(self, 0xcc);
	float dy = y2 - y1, dx = x2 - x1, s = dy * dy, root;
	s = s + dx * dx;
	__asm__("vsqrt.f32 %0, %1" : "=t"(root) : "t"(s));
	RV distance = real((double)root), one = real(1.0), minus_one = real(-1.0);
	int moved = compare_values(&distance, &one, 1) > 0;
	if (moved || compare_values(vget(self, REGION), &minus_one, 0) == 0) {
		trace.line = 3;
		RV res = {0, 0, 5};
		update_region(self, other, &res, 0, NULL);
		if (refcounted(&res))
			release(&res);
	}
	trace.line = 6;
	if (!truth(vget(self, COLOR_ME))) {
		trace.line = 46;
		draw_self(self);
		*trace_start = trace.prev;
		return;
	}
	trace.line = 8;
	if ((truth(g_blackout) || truth(vget(self, BLACKOUT))) && !truth(vget(self, SPARK))) {
		trace.line = 10;
		RV a = bget(self, IMAGE_ALPHA);
		trace.line = 11;
		RV c = bget(self, IMAGE_BLEND);
		trace.line = 12;
		set(self, IMAGE_BLEND, real(4210752.0));
		trace.line = 13;
		set(self, IMAGE_ALPHA, real(1.0));
		trace.line = 14;
		draw_self(self);
		trace.line = 15;
		set(self, IMAGE_BLEND, real(0.0));
		trace.line = 16;
		set(self, IMAGE_ALPHA, real(0.75));
		trace.line = 17;
		draw_self(self);
		trace.line = 18;
		set(self, IMAGE_ALPHA, a);
		trace.line = 19;
		set(self, IMAGE_BLEND, c);
	} else {
		trace.line = 27;
		draw_self(self);
	}
	trace.line = 30;
	if (!truth(vget(self, SPARK)) && !truth(vget(self, SHADING))) {
		trace.line = 32;
		RV a = bget(self, IMAGE_ALPHA);
		a.value = as_real(&a) * 1.5; // operator*= of the getter's real
		a.kind = 0;
		set(self, IMAGE_ALPHA, a);
		trace.line = 33;
		if (compare_values(vget(self, PREV_RECOLOR), g_timer, 0) != 0) {
			trace.line = 35;
			static const int order[7] = {SPRITE_INDEX, IMAGE_INDEX, X, Y, IMAGE_XSCALE, IMAGE_YSCALE, IMAGE_ANGLE};
			RV args[8];
			RV *argv[8];
			for (int k = 0; k < 7; k++) // the original's getter order
				args[k] = bget(self, order[k]);
			args[7] = *const_false;
			for (int k = 0; k < 8; k++)
				argv[k] = &args[k];
			RV res = {0, 0, 5};
			draw_sprite_recolored(self, other, &res, 8, argv);
			if (refcounted(&res))
				release(&res);
		} else {
			trace.line = 39;
			RV *colour = vget(self, LIGHT_COL), *alpha = vget(self, LIGHT_ALPHA);
			RV spr = bget(self, SPRITE_INDEX), img = bget(self, IMAGE_INDEX), vx = bget(self, X), vy = bget(self, Y);
			RV xs = bget(self, IMAGE_XSCALE), ys = bget(self, IMAGE_YSCALE), ang = bget(self, IMAGE_ANGLE);
			int sprite = (int)d2l(as_real(&spr));
			double f_img = as_real(&img), f_x = as_real(&vx), f_y = as_real(&vy), f_xs = as_real(&xs), f_ys = as_real(&ys);
			double f_ang = as_real(&ang);
			int col = (int)int64_value(colour);
			double f_alpha = as_real(alpha);
			draw_sprite_ext(self, sprite, (float)f_img, (float)f_x, (float)f_y, (float)f_xs, (float)f_ys, (float)f_ang, col,
				(float)f_alpha);
		}
		trace.line = 41;
		a = bget(self, IMAGE_ALPHA);
		a.value = as_real(&a) / 1.5; // operator/= of the getter's real
		a.kind = 0;
		set(self, IMAGE_ALPHA, a);
	}
	*trace_start = trace.prev;
}

#ifndef PARTICLE_NATIVE_TEST
// ---- exp_particleverify ----
typedef struct { uint32_t kind, n, w[40]; } Rec;
#define REC_MAX 12
static Rec recs[2][REC_MAX];
static int rec_n[2], rec_pass = -1, verify;
static draw_self_fn real_draw_self;
static draw_ext_fn real_draw_ext;
static script_fn real_update_region, real_recolored;

static void record(uint32_t kind, const uint32_t *w, uint32_t n) {
	if (rec_pass < 0)
		return;
	int p = rec_pass;
	if (rec_n[p] < REC_MAX) {
		Rec *r = &recs[p][rec_n[p]];
		r->kind = kind;
		r->n = n < 40 ? n : 40;
		memcpy(r->w, w, r->n * 4);
	}
	rec_n[p]++;
}
static void record_draw_self(void *self) {
	uint32_t w[10] = {(uintptr_t)self, word(self, 0x88), word(self, 0x98)};
	memcpy(&w[3], (const uint8_t *)self + 0xa0, 28);
	record(1, w, 10);
}
static void record_script(uint32_t kind, void *self, void *other, int argc, RV **args) {
	uint32_t w[40] = {(uintptr_t)self, (uintptr_t)other, (uint32_t)argc};
	uint32_t n = 3;
	for (int k = 0; k < argc && k < 12 && args; k++) {
		memcpy(&w[n], &args[k]->value, 8);
		w[n + 2] = args[k]->kind;
		n += 3;
	}
	record(kind, w, n);
}
static void record_ext(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot, int colour,
		float alpha) {
	float f[7] = {subimg, x, y, xs, ys, rot, alpha};
	uint32_t w[10] = {(uintptr_t)self, (uint32_t)sprite, (uint32_t)colour};
	memcpy(&w[3], f, sizeof(f));
	record(2, w, 10);
}
// The original's call sites (recorded, then the real call)
static void ABI site_draw_self(void *self) { record_draw_self(self); real_draw_self(self); }
static void ABI site_draw_ext(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
		int colour, float alpha) {
	record_ext(self, sprite, subimg, x, y, xs, ys, rot, colour, alpha);
	real_draw_ext(self, sprite, subimg, x, y, xs, ys, rot, colour, alpha);
}
static RV *ABI site_update_region(void *self, void *other, RV *result, int argc, RV **args) {
	record_script(3, self, other, argc, args);
	return real_update_region(self, other, result, argc, args);
}
static RV *ABI site_recolored(void *self, void *other, RV *result, int argc, RV **args) {
	record_script(4, self, other, argc, args);
	return real_recolored(self, other, result, argc, args);
}
// The native's calls in its dry run (recorded, not made)
static void ABI dry_draw_self(void *self) { record_draw_self(self); }
static void ABI dry_draw_ext(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
		int colour, float alpha) {
	record_ext(self, sprite, subimg, x, y, xs, ys, rot, colour, alpha);
}
static RV *ABI dry_update_region(void *self, void *other, RV *result, int argc, RV **args) {
	record_script(3, self, other, argc, args);
	return result;
}
static RV *ABI dry_recolored(void *self, void *other, RV *result, int argc, RV **args) {
	record_script(4, self, other, argc, args);
	return result;
}

static void verify_event(void *self, void *other) {
	uint8_t before[0x84], native_after[0x84];
	memcpy(before, (uint8_t *)self + 0x7c, sizeof(before));
	draw_self_fn ds = draw_self;
	draw_ext_fn de = draw_sprite_ext;
	script_fn ur = update_region, rc = draw_sprite_recolored;
	draw_self = dry_draw_self;
	draw_sprite_ext = dry_draw_ext;
	update_region = dry_update_region;
	draw_sprite_recolored = dry_recolored;
	rec_n[0] = rec_n[1] = 0;
	rec_pass = 0;
	native_event(self, other);
	draw_self = ds;
	draw_sprite_ext = de;
	update_region = ur;
	draw_sprite_recolored = rc;
	memcpy(native_after, (uint8_t *)self + 0x7c, sizeof(native_after));
	memcpy((uint8_t *)self + 0x7c, before, sizeof(before));
	rec_pass = 1;
	original(self, other);
	rec_pass = -1;
	int fields_same = !memcmp((uint8_t *)self + 0x7c, native_after, sizeof(native_after));
	int ok = rec_n[0] == rec_n[1] && rec_n[0] <= REC_MAX && fields_same;
	for (int k = 0; ok && k < rec_n[0]; k++)
		ok = recs[0][k].kind == recs[1][k].kind && recs[0][k].n == recs[1][k].n &&
			!memcmp(recs[0][k].w, recs[1][k].w, recs[0][k].n * 4);
	if (ok) {
		particle_verified++;
	} else if (particle_mismatch++ < 8) {
		debugPrintf("[PARTICLE] mismatch: calls %d/%d (native/original), fields %s\n", rec_n[0], rec_n[1],
			fields_same ? "same" : "differ");
		for (int k = 0; k < rec_n[0] && k < rec_n[1] && k < REC_MAX; k++)
			if (recs[0][k].kind != recs[1][k].kind || memcmp(recs[0][k].w, recs[1][k].w, recs[0][k].n * 4))
				debugPrintf("[PARTICLE]   call %d: kind %u/%u w3 %08x/%08x\n", k, recs[0][k].kind, recs[1][k].kind,
					recs[0][k].w[3], recs[1][k].w[3]);
	}
}

static uint32_t *veneer_to(uintptr_t site, uintptr_t wrapper) {
	uint32_t *v = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	int32_t off = v ? (int32_t)((uintptr_t)v - (site + 8)) : 0;
	if (!v || off < -(1 << 25) || off >= (1 << 25))
		return NULL;
	uint32_t code[2] = {0xe51ff004, wrapper};
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
#endif

static void particle_event(void *self, void *other) {
	if (enabled && !particle_ab_off && checks(self)) {
		particle_native_calls++;
#ifndef PARTICLE_NATIVE_TEST
		if (verify) {
			verify_event(self, other);
			return;
		}
#endif
		native_event(self, other);
		return;
	}
	particle_native_fallbacks++;
	original(self, other);
}

// The target of the bl at site (a PLT stub, or the function itself in a PLT-patched runner), 0 if it isn't a bl
static uintptr_t bl_target(uintptr_t site) {
	uint32_t ins = *(uint32_t *)site;
	return (ins >> 24) == 0xeb ? site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6) : 0;
}

static uint32_t fingerprint(const uint32_t *w, size_t words) {
	uint32_t h = 2166136261u;
	for (size_t i = 2; i < words; i++) {
		uint32_t v = (w[i] >> 24) == 0xeb ? 0xeb000000 : w[i];
		for (int b = 0; b < 4; b++)
			h = (h ^ ((v >> (8 * b)) & 0xff)) * 16777619u;
	}
	return h;
}

// Wraps obj_particle's user event 15 (orig: what the caller would have called). Returns the wrapper, or orig.
event_fn particle_native_install(event_fn orig) {
	enum { EVENT = 0x4581fb8, SIZE = 7268, UPDATE_REGION_SITE = 0x4582554, RECOLORED_SITE = 0x45834a0,
	       D2L_SITE = 0x45834b4, DRAW_SELF_SITE = 0x458266c, DRAW_EXT_SITE = 0x4583620 };
	const uint32_t FINGERPRINT = 0x15a4bc70; // the same on the original and the PLT-patched runner
#define RESOLVE(v, t, n) do { v = (t)so_symbol(&yoyoloader_mod, n); if (!v) goto fail; } while (0)
	uintptr_t entry;
	if (!orig)
		return orig;
	RESOLVE(entry, uintptr_t, "_Z32gml_Object_obj_particle_Other_25P9CInstanceS0_");
	if ((entry & 3) || fingerprint((const uint32_t *)entry, SIZE / 4) != FINGERPRINT)
		goto fail;
#define AT(a) (entry + (a) - EVENT)
	update_region = (script_fn)bl_target(AT(UPDATE_REGION_SITE));
	draw_sprite_recolored = (script_fn)bl_target(AT(RECOLORED_SITE));
	d2l = (d2l_fn)bl_target(AT(D2L_SITE));
	draw_self = (draw_self_fn)bl_target(AT(DRAW_SELF_SITE));
	draw_sprite_ext = (draw_ext_fn)bl_target(AT(DRAW_EXT_SITE));
#undef AT
	if (!update_region || !draw_sprite_recolored || !d2l || !draw_self || !draw_sprite_ext)
		goto fail;
	RESOLVE(bool_value, bool_fn, "_Z11BOOL_RValuePK6RValue");
	RESOLVE(compare, compare_fn, "_Z12YYCompareValRK6RValueS1_db");
	RESOLVE(variable_get, getvalue_fn, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	RESOLVE(variable_set, setdirect_fn, "_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
	RESOLVE(real_value, real_fn, "_Z14REAL_RValue_ExPK6RValue");
	RESOLVE(int64_value, int64_fn, "_Z12INT64_RValuePK6RValue");
	RESOLVE(release, free_fn, "_Z16FREE_RValue__PreP6RValue");
	RESOLVE(point_distance, void *, "_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_");
	RESOLVE(functions, Function **, "the_functions");
	RESOLVE(distance_desc, const int *, "g_FUNC_point_distance");
	RESOLVE(epsilon, const double *, "g_GMLMathEpsilon");
	RESOLVE(builtin_variables, const uint8_t *, "builtin_variables");
	RESOLVE(builtin_count, const int *, "g_nStartGlobalVariables");
	RESOLVE(const_false, const RV *, "gs_constFalse");
	RESOLVE(global_obj, void **, "g_pGlobal");
	RESOLVE(trace_start, Trace **, "_ZN13SYYStackTrace8s_pStartE");
	for (int k = 0; k < NFLOAT; k++)
		RESOLVE(float_getters[k], uintptr_t, float_getter_names[k]);
	memset(float_slot, -1, sizeof(float_slot));
	for (int k = 0; k < NFLOAT; k++)
		float_slot[float_vars[k]] = (int8_t)k;
	for (int n = 0; n < NVARS; n++)
		RESOLVE(var_desc[n], const int *, var_names[n]);
#undef RESOLVE
	original = orig;
	enabled = 1;
#ifndef PARTICLE_NATIVE_TEST
	extern int debugMode;
	if (debugMode && exp_flag("particleverify")) {
		// The original's call sites, checked to call what the native calls; all veneers first, then the patches
		static const uintptr_t sites[7] = {UPDATE_REGION_SITE, DRAW_SELF_SITE, 0x4582680, 0x45828d4, 0x458297c,
			RECOLORED_SITE, DRAW_EXT_SITE};
		const uintptr_t wrappers[7] = {(uintptr_t)&site_update_region, (uintptr_t)&site_draw_self,
			(uintptr_t)&site_draw_self, (uintptr_t)&site_draw_self, (uintptr_t)&site_draw_self,
			(uintptr_t)&site_recolored, (uintptr_t)&site_draw_ext};
		const uintptr_t expect[7] = {(uintptr_t)update_region, (uintptr_t)draw_self, (uintptr_t)draw_self,
			(uintptr_t)draw_self, (uintptr_t)draw_self, (uintptr_t)draw_sprite_recolored, (uintptr_t)draw_sprite_ext};
		uint32_t *veneers[7];
		int ok = 1;
		for (int k = 0; ok && k < 7; k++)
			ok = bl_target(entry + sites[k] - EVENT) == expect[k] &&
				(veneers[k] = veneer_to(entry + sites[k] - EVENT, wrappers[k])) != NULL;
		if (ok) {
			real_update_region = update_region;
			real_draw_self = draw_self;
			real_recolored = draw_sprite_recolored;
			real_draw_ext = draw_sprite_ext;
			for (int k = 0; k < 7; k++)
				retarget(entry + sites[k] - EVENT, veneers[k]);
			verify = 1;
		}
	}
	if (exp_flag("particlenative0"))
		enabled = 0;
	particle_ab = debugMode && exp_flag("particleab");
	debugPrintf("particle native: obj_particle user event 15 %s%s\n", enabled ? "runs natively" : "off (exp_particlenative0)",
		verify ? " (verify mode)" : "");
#endif
	return particle_event;
fail:
	debugPrintf("particle native: NOT installed\n");
	return orig;
}
