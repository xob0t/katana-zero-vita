/*
 * gml_Script_draw_shadow, native. APK 1.1.10 libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * Disassembly: work/bike/ds.asm, from work/decomp/annotate.py (both untracked).
 *
 * The bike chase draws a ground shadow under every road object (cones, cars, barriers...) from its Draw event:
 * draw_shadow(sprite_index, animation_index, x, y, angle, alpha, fade). The script redraws the sprite's rows above its
 * origin one pixel row at a time, black, flattened along the angle and fading out (GML lines 12-38):
 *
 *     w = sprite_get_width(spr); xo = sprite_get_xoffset(spr) * 2; yo = sprite_get_yoffset(spr);
 *     h = sprite_get_height(spr); h -= h - yo;
 *     j = 0; last = -1;
 *     for (i = h - 1; i > 0; i--) {
 *         v = floor(lengthdir_y(j, angle));
 *         if (v != last) {
 *             last = v;
 *             draw_sprite_part_ext(spr, sub, 0, i, w, 1, x - xo + lengthdir_x(j * 2, angle), y + v * 2, 2, 2, c_black,
 *                                  alpha);
 *         }
 *         j += 1;
 *         alpha -= fade;
 *         if (alpha <= 0) break;
 *     }
 *
 * The traffic-cone stretch has ~100 cones on the road: 11 ms per frame in their Draw events, most of it the compiled
 * script's RValue bookkeeping around ~20 one-row draws per cone. This runs the same loop in doubles and calls the
 * same builtins through the function registry, in the same order with the same arguments (the constants are the
 * script's own gs_constArg values, passed as they are). Comparisons use g_GMLMathEpsilon like YYCompareVal;
 * lengthdir_x/y are the runner's own, called with the same float arguments. Anything other than seven arguments (a
 * sprite and subimage without a reference count, then five reals) runs the original. exp_shadownative0 turns it off;
 * exp_shadowverify lets the original draw and compares each of its draw_sprite_part_ext calls with the native
 * record (value bits and kind of all 12 arguments), and the result slot with what the native path writes (low word 0,
 * kind 5, as the compiled script does). [SHADOW] report: mismatches must stay 0; a call with more rows than the
 * record holds is counted as an overflow and not compared.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <kubridge.h>
#include "main.h"
#include "so_util.h"

typedef struct {
	double val;
	uint32_t flags;
	uint32_t kind;
} rv;

#define KIND_REAL 0

typedef rv *(*script_fn)(void *self, void *other, rv *result, int argc, rv **argv);
typedef void (*builtin_fn)(rv *result, void *self, void *other, int argc, rv *args);
typedef struct {
	const char *name;
	builtin_fn func;
	int argc;
} runner_function;

enum { F_PART_EXT, F_WIDTH, F_XOFFSET, F_YOFFSET, F_HEIGHT, NFUNCS };
static const char *const fn_names[NFUNCS] = {"draw_sprite_part_ext", "sprite_get_width", "sprite_get_xoffset",
	"sprite_get_yoffset", "sprite_get_height"};
static runner_function *fns[NFUNCS];

static script_fn orig_shadow;
static float (*yy_lengthdir_x)(float, float);
static float (*yy_lengthdir_y)(float, float);
static const double *math_epsilon;
static const rv *const_0, *const_1, *const_2; // gs_constArg0/1/2_5AEC818E: 0 (left edge, c_black), 1 (row height), 2 (scale)

unsigned shadow_native_fast, shadow_native_slow, shadow_native_rows;
unsigned shadow_reject[9], shadow_reject_kind; // argc, constants, argument 0-6

static int verify;
#define VERIFY_MAX 512
static rv verify_args[VERIFY_MAX][12];
static int verify_n, verify_i, verify_overflow;
static builtin_fn verify_real;
unsigned shadow_verify_calls, shadow_verify_mismatch, shadow_verify_overflow, shadow_verify_results;

static void verify_part_ext(rv *result, void *self, void *other, int argc, rv *args) {
	if (verify_overflow) { // the record is incomplete: this call's rows aren't compared (counted as an overflow)
		verify_real(result, self, other, argc, args);
		return;
	}
	int in_record = verify_i < verify_n, bad = argc != 12 || !in_record;
	for (int k = 0; !bad && k < 12; k++)
		bad = memcmp(&args[k].val, &verify_args[verify_i][k].val, 8) || (args[k].kind & 0xffffff) !=
			(verify_args[verify_i][k].kind & 0xffffff);
	if (bad && shadow_verify_mismatch < 8) {
		if (in_record && argc == 12)
			debugPrintf("[SHADOW] mismatch at row %d of %d: top %.17g/%.17g x %.17g/%.17g y %.17g/%.17g a %.17g/%.17g\n",
				verify_i, verify_n, args[3].val, verify_args[verify_i][3].val, args[6].val, verify_args[verify_i][6].val,
				args[7].val, verify_args[verify_i][7].val, args[11].val, verify_args[verify_i][11].val);
		else
			debugPrintf("[SHADOW] mismatch: original row %d (argc %d), native recorded %d\n", verify_i, argc, verify_n);
	}
	shadow_verify_mismatch += bad;
	shadow_verify_calls++;
	verify_i++;
	verify_real(result, self, other, argc, args);
}

static int cmp(double a, double b) {
	double d = a - b;
	return fabs(d) <= *math_epsilon ? 0 : d < 0 ? -1 : 1;
}

static void call(int n, rv *out, void *self, void *other, int argc, rv *args) {
	out->val = 0;
	out->flags = 0;
	out->kind = 5; // undefined, as the compiled call's result slot
	fns[n]->func(out, self, other, argc, args);
}

static rv real(double x) {
	rv v = {x, 0, KIND_REAL};
	return v;
}

static rv *shadow(void *self, void *other, rv *result, int argc, rv **argv) {
	int ok = argc == 7 && const_0->kind == KIND_REAL && const_1->kind == KIND_REAL && const_2->kind == KIND_REAL;
	if (!ok)
		shadow_reject[argc != 7 ? 0 : 1]++;
	for (int i = 0; ok && i < 7; i++)
		// the sprite and subimage are passed through as they are (any kind without a reference count; a sprite is a
		// reference, kind 15); the rest is arithmetic
		if (!(ok = argv[i] && (i < 2 ? !((1u << (argv[i]->kind & 0x1f)) & 0x46u) : argv[i]->kind == KIND_REAL)))
			shadow_reject[2 + i]++, shadow_reject_kind = argv[i] ? argv[i]->kind : 0xdead;
	rv w, xo, yo, h;
	if (ok) {
		call(F_WIDTH, &w, self, other, 1, argv[0]);
		call(F_XOFFSET, &xo, self, other, 1, argv[0]);
		call(F_YOFFSET, &yo, self, other, 1, argv[0]);
		call(F_HEIGHT, &h, self, other, 1, argv[0]);
		ok = w.kind == KIND_REAL && xo.kind == KIND_REAL && yo.kind == KIND_REAL && h.kind == KIND_REAL;
	}
	if (!ok) {
		shadow_native_slow++;
		return orig_shadow(self, other, result, argc, argv);
	}
	shadow_native_fast++;
	// The compiled script stores 0 in the result's low word and makes it undefined (kind 5; 0x1957b7c, 0x1957c0c); the
	// high word and the flags stay as they were
	rv expected = *result;
	uint32_t lo = 0;
	memcpy(&expected.val, &lo, 4);
	expected.kind = 5;
	if (!verify)
		*result = expected;
	const double x = argv[2]->val, y = argv[3]->val;
	const float angle = (float)argv[4]->val;
	double alpha = argv[5]->val;
	const double fade = argv[6]->val;
	const double xo2 = xo.val * 2.0;
	double hh = h.val;
	hh -= hh - yo.val;
	double j = 0, last = -1;
	rv args[12];
	for (double i = hh - 1.0; cmp(i, 0) > 0; i += -1.0) {
		double v = floor((double)yy_lengthdir_y((float)j, angle));
		if (cmp(v, last) != 0) {
			last = v;
			args[0] = *argv[0];
			args[1] = *argv[1];
			args[2] = *const_0;
			args[3] = real(i);
			args[4] = w;
			args[5] = *const_1;
			args[6] = real((x - xo2) + (double)yy_lengthdir_x((float)(j * 2.0), angle));
			args[7] = real(y + v * 2.0);
			args[8] = *const_2;
			args[9] = *const_2;
			args[10] = *const_0;
			args[11] = real(alpha);
			shadow_native_rows++;
			if (verify) {
				if (verify_n < VERIFY_MAX)
					memcpy(verify_args[verify_n++], args, sizeof(args));
				else
					verify_overflow = 1;
			} else {
				rv out;
				call(F_PART_EXT, &out, self, other, 12, args);
			}
		}
		j += 1.0;
		alpha -= fade;
		if (cmp(alpha, 0) <= 0)
			break;
	}
	if (verify) {
		verify_real = fns[F_PART_EXT]->func;
		fns[F_PART_EXT]->func = verify_part_ext;
		verify_i = 0;
		orig_shadow(self, other, result, argc, argv);
		fns[F_PART_EXT]->func = verify_real;
		if (verify_overflow) {
			shadow_verify_overflow++; // more rows than the record holds: this call isn't compared
		} else if (verify_i != verify_n) {
			if (shadow_verify_mismatch < 8)
				debugPrintf("[SHADOW] mismatch: original drew %d rows, native %d\n", verify_i, verify_n);
			shadow_verify_mismatch++;
		}
		if (memcmp(result, &expected, sizeof(rv))) {
			if (shadow_verify_mismatch < 8)
				debugPrintf("[SHADOW] result mismatch: kind %u/%u flags %u/%u\n", result->kind, expected.kind,
					result->flags, expected.flags);
			shadow_verify_mismatch++;
		}
		shadow_verify_results++;
		verify_n = 0;
		verify_overflow = 0;
	}
	return result;
}

int shadow_native_install(void *(*entry_hook)(uintptr_t entry, void *replacement)) {
#define SYM(name) so_symbol(&yoyoloader_mod, name)
	uintptr_t entry = SYM("_Z22gml_Script_draw_shadowP9CInstanceS0_R8YYRValueiPPS1_");
	yy_lengthdir_x = (void *)SYM("_Z17YYGML_lengthdir_xff");
	yy_lengthdir_y = (void *)SYM("_Z17YYGML_lengthdir_yff");
	math_epsilon = (const double *)SYM("g_GMLMathEpsilon");
	const_0 = (const rv *)SYM("gs_constArg0_5AEC818E");
	const_1 = (const rv *)SYM("gs_constArg1_5AEC818E");
	const_2 = (const rv *)SYM("gs_constArg2_5AEC818E");
	runner_function **functions = (runner_function **)SYM("the_functions");
	int *count = (int *)SYM("the_numb");
#undef SYM
	if (!entry || !yy_lengthdir_x || !yy_lengthdir_y || !math_epsilon || !const_0 || !const_1 || !const_2 ||
			!functions || !*functions || !count)
		return 0;
	for (int n = 0; n < NFUNCS; n++) {
		for (int i = 0; i < *count && !fns[n]; i++) {
			runner_function *f = &(*functions)[i];
			if (f->name && !strcmp(f->name, fn_names[n]))
				fns[n] = f;
		}
		if (!fns[n])
			return 0;
	}
	verify = exp_flag("shadowverify");
	return (orig_shadow = entry_hook(entry, shadow)) != NULL;
}
