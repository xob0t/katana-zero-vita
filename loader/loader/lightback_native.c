/*
 * lightback_native.c
 *
 * Exact native of the off-screen path of obj_lightback user event 0 (APK 1.1.10, 0x42e8fd0), which
 * obj_new_platform_lighting's Draw runs for every light (`with (obj_lightback) event_user(0)`):
 *
 *   x = parent.x;                    // Variable_SetValue_Direct -> SV_X -> SetPosition
 *   y = parent.y;
 *   var r = parent.radius / 400;
 *   ...                              // locals only
 *   if (x > xview - 200 && x < xview + wview + 200 && y > yview - 200 && y < yview + hview + 200) {
 *       ...                          // alpha, light power, one draw_sprite_stretched_ext
 *   }
 *
 * room_haunted_cart2 runs it for 129 lights per frame (1.5 ms), most of them off-screen. For those the event
 * has no effect beyond the two position writes, but pays ~10 us of GML temporaries.
 *
 * The native reads the same values with the runner's own getters (pure reads), predicts the position the
 * two writes leave (SV_X stores (float) of the double it gets; parent.x is (double) of a float, so the light
 * gets the parent's float) and evaluates the line-14 cull with YYCompareVal and the live epsilon in the
 * original order and association. A visible light runs the original event from its start, with nothing
 * written yet. An off-screen light gets the two writes through Variable_SetValue_Direct, in order, with the
 * values the original passes, and the event returns. Anything unusual (non-real values, a parent that isn't
 * a plain instance id, a missing instance, -1/-2 ids) runs the original event.
 *
 * exp_lightbacknative0 keeps the original event. exp_lbverify: for each light the native handled as
 * off-screen, also runs the original afterwards and checks that it left x/y unchanged (it writes the same
 * values again) and that it drew nothing (draw_sprite_stretched_ext calls counted around it). exp_lbab
 * (measurement only) switches the native off and on every report window, so one run gives an A/B.
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
typedef int ABI (*VarGetId)(int, int, int, RV *, int, int);
typedef int ABI (*VarGetRV)(const RV *, int, int, RV *, int, int);
typedef int ABI (*VarSet)(void *, int, int, RV *);
typedef int ABI (*VarGetDirect)(void *, int, int, RV *, int, int);
typedef int ABI (*Int32)(const RV *);
typedef void ABI (*Free)(RV *);

#define ARRAY_NONE ((int)0x80000000)

unsigned lightback_native_culled, lightback_native_visible, lightback_native_fallback;
unsigned lightback_verify_calls, lightback_verify_mismatch, lightback_verify_draws;
uint32_t lightback_native_us;
static int lb_verify, lb_timing, lb_ab, lb_ab_off;
static unsigned lb_reject_line[12], lb_reject_count[12];

static Event original;
static Compare compare;
static VarGetId var_get_id;
static VarGetRV var_get_rv;
static VarSet var_set;
static VarGetDirect var_get_direct;
static unsigned reject_kind;
static Int32 int32_value;
static Free free_value;
static void **global;
static double *epsilon;
enum { XVIEW, YVIEW, WVIEW, HVIEW, BLOODMAP, POWER, PARENT, VX, VY, RADIUS, NV };
static const char *const var_names[NV] = {
	"g_VAR_xview", "g_VAR_yview", "g_VAR_wview", "g_VAR_hview", "g_VAR_has_bloodmap", "g_VAR_light_power",
	"g_VAR_parent", "g_VAR_x", "g_VAR_y", "g_VAR_radius" };
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
static inline int id_of(int n) { return vars[n][1]; } // g_VAR_* ids are filled at game start: read them live

// x > lo && x < (a + b) + m, the original's two-compare test of one axis (YYCompareVal, live epsilon)
static int inside_axis(double pos, const RV *view, const RV *size) {
	if (compare_real(pos, view->v.d - 200.0) < 1)
		return 0;
	int r = compare_real(pos, (view->v.d + size->v.d) + 200.0);
	return r != -2 && r < 0;
}

// Returns 1 if the event was handled (off-screen light), 0 to run the original.
static int handle(void *self) {
	if (!self || *(uint32_t *)((char *)self + 0x50) != 1)
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
	// INT32_RValue and look the id up (-1 self, -2 other, not found: an error); kind 6 reads the object directly
	unsigned kind = parent->kind & 0xffffff;
	// The original's result slots start unset (kind 0xffffff); a failed read leaves them so
	RV px = { .v.d = 0, .flags = 0, .kind = 0xffffff }, py = px;
	if (kind <= 15 && ((1u << kind) & 0xa481)) {
		int id = int32_value(parent);
		if (id == -1 || id == -2)
			return reject(__LINE__), 0;
		if (!var_get_id(id, id_of(VX), ARRAY_NONE, &px, 0, 0) || !real(&px))
			return reject(__LINE__), 0;
		if (!var_get_id(id, id_of(VY), ARRAY_NONE, &py, 0, 0) || !real(&py))
			return reject(__LINE__), 0;
	} else if (kind == 6 && parent->v.w[0]) {
		void *obj = (void *)(uintptr_t)parent->v.w[0];
		if (!var_get_direct(obj, id_of(VX), ARRAY_NONE, &px, 0, 0) || !real(&px))
			return reject(__LINE__), 0;
		if (!var_get_direct(obj, id_of(VY), ARRAY_NONE, &py, 0, 0) || !real(&py))
			return reject(__LINE__), 0;
	} else {
		reject_kind = kind;
		return reject(__LINE__), 0;
	}
	// SV_X / SV_Y store (float) of the value; line 14 reads it back as a double
	double x = (double)(float)px.v.d, y = (double)(float)py.v.d;
	if (inside_axis(x, vx, vw) && inside_axis(y, vy, vh)) {
		lightback_native_visible++;
		return 0;
	}
	// Off-screen: the reads the original makes before the cull, then its two writes
	RV *bloodmap = get(g, id_of(BLOODMAP));
	RV *power = get(g, id_of(POWER));
	(void)bloodmap;
	(void)power;
	RV radius = { .v.d = 0, .flags = 0, .kind = 0xffffff };
	var_get_rv(parent, id_of(RADIUS), ARRAY_NONE, &radius, 0, 0);
	if (!real(&radius)) { // parent.radius / 400 would raise an error (or this is a value with a reference)
		if ((1u << (radius.kind & 0x1f)) & 0x46)
			free_value(&radius);
		return reject(__LINE__), 0;
	}
	RV wx = px, wy = py;
	var_set(self, id_of(VX), ARRAY_NONE, &wx);
	var_set(self, id_of(VY), ARRAY_NONE, &wy);
	lightback_native_culled++;
	return 1;
}

// draw_sprite_stretched_ext calls, counted by the verify wrapper
static void *orig_stretched;
typedef void ABI (*Builtin)(RV *, void *, void *, int, RV *);
static unsigned stretched_count;
static void ABI counted_stretched(RV *out, void *self, void *other, int argc, RV *args) {
	stretched_count++;
	((Builtin)orig_stretched)(out, self, other, argc, args);
}

static void ABI lightback_native(void *self, void *other) {
	if (lb_ab_off) {
		original(self, other);
		return;
	}
	uint32_t t0 = lb_timing ? sceKernelGetProcessTimeLow() : 0;
	int done = handle(self);
	if (lb_timing)
		lightback_native_us += sceKernelGetProcessTimeLow() - t0;
	if (!done) {
		original(self, other);
		return;
	}
	if (lb_verify) {
		float x = *(float *)((char *)self + 0xb8), y = *(float *)((char *)self + 0xbc);
		uint32_t flags = *(uint32_t *)((char *)self + 0x7c);
		unsigned before = stretched_count;
		original(self, other);
		lightback_verify_calls++;
		lightback_verify_draws += stretched_count - before;
		if (memcmp(&x, (char *)self + 0xb8, 4) || memcmp(&y, (char *)self + 0xbc, 4) ||
			flags != *(uint32_t *)((char *)self + 0x7c) || stretched_count != before)
			lightback_verify_mismatch++;
	}
}

void lightback_native_report(int frames) {
	if (!original || frames <= 0)
		return;
	debugPrintf("[LIGHTBACK] user 0: off-screen native %.1f, visible (original) %.1f, fallback %.1f per frame | native part %.3f ms%s\n",
		(float)lightback_native_culled / frames, (float)lightback_native_visible / frames,
		(float)lightback_native_fallback / frames, lightback_native_us / 1000.0f / frames,
		lb_ab ? (lb_ab_off ? " | A/B: off" : " | A/B: on") : "");
	if (lb_verify)
		debugPrintf("[LIGHTBACK] verify: %u off-screen calls re-run, %u mismatches, %u draws (session)\n",
			lightback_verify_calls, lightback_verify_mismatch, lightback_verify_draws);
	for (int i = 0; i < 12 && lb_reject_line[i]; i++)
		debugPrintf("[LIGHTBACK] fallback at lightback_native.c:%u x%u (session, last parent kind %u)\n", lb_reject_line[i],
			lb_reject_count[i], reject_kind);
	lightback_native_culled = lightback_native_visible = lightback_native_fallback = 0;
	lightback_native_us = 0;
	if (lb_ab)
		lb_ab_off = !lb_ab_off;
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
			debugPrintf("[LIGHTBACK] verify: counting draw_sprite_stretched_ext\n");
			return;
		}
	}
}

int lightback_native_install(void *(*rx_alloc)(size_t)) {
	if (original || !rx_alloc)
		return 0;
#define SYM(t, n) ((t)so_symbol(&yoyoloader_mod, n))
	compare = SYM(Compare, "_Z12YYCompareValRK6RValueS1_db");
	var_get_id = SYM(VarGetId, "_Z17Variable_GetValueiiiP6RValuebb");
	var_get_rv = SYM(VarGetRV, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	var_set = SYM(VarSet, "_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
	var_get_direct = SYM(VarGetDirect, "_Z24Variable_GetValue_DirectP12YYObjectBaseiiP6RValuebb");
	int32_value = SYM(Int32, "_Z12INT32_RValuePK6RValue");
	free_value = SYM(Free, "_Z16FREE_RValue__PreP6RValue");
	global = SYM(void **, "g_pGlobal");
	epsilon = SYM(double *, "g_GMLMathEpsilon");
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z33gml_Object_obj_lightback_Other_10P9CInstanceS0_");
#undef SYM
	if (!compare || !var_get_id || !var_get_rv || !var_set || !var_get_direct || !int32_value || !free_value || !global || !epsilon || !entry)
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
	lb_verify = exp_flag("lbverify");
	lb_ab = exp_flag("lbab");
	extern int heavy_profiling;
	lb_timing = heavy_profiling;
	hook_addr(entry, (uintptr_t)lightback_native);
	kuKernelFlushCaches((void *)entry, 8);
	return 1;
}
