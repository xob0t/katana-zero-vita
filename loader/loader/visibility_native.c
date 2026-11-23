/*
 * visibility_native.c
 *
 * Exact natives of the two view tests every heavy room calls hundreds of times per frame
 * (APK 1.1.10 runner, compiled YYC):
 *
 *   inside_screen_point(px, py, margin)   0x3145a98, the minecart rails, smoke and others
 *   inside_screen(margin) on self x/y     0x314677c, drawactors, static objects, smoke clouds
 *
 *   return px > xview - margin && px < (xview + wview) + margin
 *       && py > yview - margin && py < (yview + hview) + margin;
 *
 * Both scripts only read: four globals through g_pGlobal's vtable getter (in that order), the
 * arguments, and for inside_screen self.x / self.y. They write nothing but the result.
 * The native keeps the runner's own getter calls and YYCompareVal (with the live epsilon) in
 * the original order and with the original short-circuit, so it returns the same value for the
 * same state. Arithmetic is the operator path for two reals: one double add or subtract, in the
 * original association. Anything else (a non-real operand, a missing argument, a non-instance
 * self, a result slot that overlaps an input) runs the original script.
 *
 * x and y: the original reads them through YYGML_Variable_GetValue on a self wrapper, which
 * ends in the builtin getter GV_X/GV_Y: (double) of the float at +0xb8/+0xbc.
 *
 * exp_visibilitynative0 keeps the original scripts. exp_visverify runs the original after each
 * native call and counts results that differ (report line [VIS]).
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
typedef RV *ABI (*Script)(void *, void *, RV *, int, RV **);
typedef int ABI (*Compare)(const RV *, const RV *, double, int);
typedef RV *ABI (*GetVar)(void *, int);

unsigned vis_point_native, vis_point_fallback, vis_screen_native, vis_screen_fallback;
unsigned vis_verify_calls, vis_verify_mismatch;
uint32_t vis_point_us, vis_screen_us;
int vis_verify, vis_timing;
static unsigned vis_reject_line[8], vis_reject_count[8];

static Script orig_point, orig_screen;
static Compare compare;
static void **global;
static double *epsilon;
static const int *var_xview, *var_yview, *var_wview, *var_hview;

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
	for (int i = 0; i < 8; i++) {
		if (vis_reject_line[i] == line || !vis_reject_line[i]) {
			vis_reject_line[i] = line;
			vis_reject_count[i]++;
			return;
		}
	}
}

static inline int real(const RV *v) { return (v->kind & 0xffffff) == 0; }
static inline int apart(const RV *a, const RV *b) {
	uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
	return x >= y + 16 || y >= x + 16;
}

// The four comparisons with the operands already read. Returns 1/0, or -1 to fall back.
static int test(void *self, RV *result, const RV *px, const RV *py, const RV *m) {
	void *g = *global;
	if (!g)
		return -1;
	GetVar get = (*(GetVar **)g)[2];
	// g_VAR_* ids are filled at game start: read them live
	RV *vx = get(g, var_xview[1]);
	RV *vy = get(g, var_yview[1]);
	RV *vw = get(g, var_wview[1]);
	RV *vh = get(g, var_hview[1]);
	if (!vx || !vy || !vw || !vh || !real(vx) || !real(vy) || !real(vw) || !real(vh) || !real(m))
		return -1;
	// The original clears the result before it copies its operands
	if (!apart(result, vx) || !apart(result, vy) || !apart(result, vw) || !apart(result, vh) || !apart(result, m))
		return -1;
	if (px && (!real(px) || !apart(result, px)))
		return -1;
	if (py && (!real(py) || !apart(result, py)))
		return -1;
	double x = px ? px->v.d : (double)*(float *)((char *)self + 0xb8);
	double y = py ? py->v.d : (double)*(float *)((char *)self + 0xbc);
	double margin = m->v.d;
	if (compare_real(x, vx->v.d - margin) < 1)
		return 0;
	int r = compare_real(x, (vx->v.d + vw->v.d) + margin);
	if (r == -2 || r >= 0)
		return 0;
	if (compare_real(y, vy->v.d - margin) < 1)
		return 0;
	r = compare_real(y, (vy->v.d + vh->v.d) + margin);
	if (r == -2 || r >= 0)
		return 0;
	return 1;
}

static void verify(Script orig, void *self, void *other, RV *result, int argc, RV **argv, const RV *before) {
	RV check = *before;
	orig(self, other, &check, argc, argv);
	vis_verify_calls++;
	if (check.kind != result->kind || check.v.bits != result->v.bits || check.flags != result->flags)
		vis_verify_mismatch++;
}

static RV *ABI vis_point(void *self, void *other, RV *result, int argc, RV **argv) {
	uint32_t t0 = vis_timing ? sceKernelGetProcessTimeLow() : 0;
	int r = -1;
	if (result && argc >= 3 && argv && argv[0] && argv[1] && argv[2])
		r = test(self, result, argv[0], argv[1], argv[2]);
	if (r >= 0) {
		RV before = *result;
		result->v.d = r;
		result->kind = 13;
		vis_point_native++;
		if (vis_verify)
			verify(orig_point, self, other, result, argc, argv, &before);
	} else {
		vis_point_fallback++;
		reject(__LINE__);
		orig_point(self, other, result, argc, argv);
	}
	if (vis_timing)
		vis_point_us += sceKernelGetProcessTimeLow() - t0;
	return result;
}

static RV *ABI vis_screen(void *self, void *other, RV *result, int argc, RV **argv) {
	uint32_t t0 = vis_timing ? sceKernelGetProcessTimeLow() : 0;
	int r = -1;
	if (result && self && *(uint32_t *)((char *)self + 0x50) == 1 && argc >= 1 && argv && argv[0])
		r = test(self, result, NULL, NULL, argv[0]);
	if (r >= 0) {
		RV before = *result;
		result->v.d = r;
		result->kind = 13;
		vis_screen_native++;
		if (vis_verify)
			verify(orig_screen, self, other, result, argc, argv, &before);
	} else {
		vis_screen_fallback++;
		reject(__LINE__);
		orig_screen(self, other, result, argc, argv);
	}
	if (vis_timing)
		vis_screen_us += sceKernelGetProcessTimeLow() - t0;
	return result;
}

void visibility_native_report(int frames) {
	if (!orig_point && !orig_screen)
		return;
	if (frames > 0)
		debugPrintf("[VIS] inside_screen_point native %.1f fallback %.1f | inside_screen native %.1f fallback %.1f per frame\n",
			(float)vis_point_native / frames, (float)vis_point_fallback / frames,
			(float)vis_screen_native / frames, (float)vis_screen_fallback / frames);
	if (vis_timing && frames > 0)
		debugPrintf("[VIS] time: point %.3f ms, screen %.3f ms per frame\n", vis_point_us / 1000.0f / frames,
			vis_screen_us / 1000.0f / frames);
	if (vis_verify)
		debugPrintf("[VIS] verify: %u calls, %u mismatches (session)\n", vis_verify_calls, vis_verify_mismatch);
	for (int i = 0; i < 8 && vis_reject_line[i]; i++)
		debugPrintf("[VIS] fallback at visibility_native.c:%u x%u (session)\n", vis_reject_line[i], vis_reject_count[i]);
	vis_point_native = vis_point_fallback = vis_screen_native = vis_screen_fallback = 0;
	vis_point_us = vis_screen_us = 0;
}

static Script hook(uintptr_t entry, void *replacement, void *(*rx_alloc)(size_t)) {
	uint32_t *w = (uint32_t *)entry;
	if (!entry || (entry & 3) || w[0] != 0xe92d4ff0 || w[1] != 0xe28db01c)
		return NULL;
	uint32_t *rx = rx_alloc(16);
	if (!rx || ((uintptr_t)rx & 3))
		return NULL;
	uint32_t code[4] = { w[0], w[1], 0xe51ff004, (uint32_t)(entry + 8) };
	kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof(code));
	kuKernelFlushCaches(rx, sizeof(code));
	hook_addr(entry, (uintptr_t)replacement);
	kuKernelFlushCaches((void *)entry, 8);
	return (Script)rx;
}

int visibility_native_install(void *(*rx_alloc)(size_t)) {
	if (orig_point || orig_screen || !rx_alloc)
		return 0;
	compare = (Compare)so_symbol(&yoyoloader_mod, "_Z12YYCompareValRK6RValueS1_db");
	global = (void **)so_symbol(&yoyoloader_mod, "g_pGlobal");
	epsilon = (double *)so_symbol(&yoyoloader_mod, "g_GMLMathEpsilon");
	var_xview = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_xview");
	var_yview = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_yview");
	var_wview = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_wview");
	var_hview = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_hview");
	uintptr_t point = so_symbol(&yoyoloader_mod, "_Z30gml_Script_inside_screen_pointP9CInstanceS0_R8YYRValueiPPS1_");
	uintptr_t screen = so_symbol(&yoyoloader_mod, "_Z24gml_Script_inside_screenP9CInstanceS0_R8YYRValueiPPS1_");
	if (!compare || !global || !epsilon || !var_xview || !var_yview || !var_wview || !var_hview || !point || !screen)
		return 0;
	vis_verify = exp_flag("visverify");
	extern int heavy_profiling;
	vis_timing = heavy_profiling;
	orig_point = hook(point, vis_point, rx_alloc);
	orig_screen = hook(screen, vis_screen, rx_alloc);
	return (orig_point ? 1 : 0) + (orig_screen ? 2 : 0);
}
