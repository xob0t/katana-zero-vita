/*
 * gml_Script_draw_sprite_wavy, native. APK 1.10 libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * Lift: work/decomp/lifted/_Z27gml_Script_draw_sprite_wavy*.gml, Ghidra work/decomp/ghidra_wavy/.
 *
 * The nightmare backgrounds (obj_nightmare_background, obj_psychboss_background(_nightmare)) tile a sprite and draw
 * each tile in thin horizontal strips with a cosine sway:
 *
 *     draw_sprite_wavy(spr, sub, x, y, xs, ys, strip, amp, offset, step)
 *     off = offset mod 360; alpha = draw_get_alpha(); col = draw_get_color(); i = 0; wx = cos(degtorad(off)) * amp;
 *     h, w, xo, yo = sprite_get_height/width/xoffset/yoffset(spr)
 *     do {
 *         if (i > h) exit;
 *         if (y + ((i - yo) + strip) * ys > global.yview)
 *             draw_sprite_part_ext(spr, sub, 0, i, w, min(strip, h - i), x + (wx - xo) * xs, y + (i - yo) * ys,
 *                                  xs, ys, col, alpha);
 *         wx = cos(degtorad(off)) * amp; off = (off + step) mod 360; i += strip;
 *     } while (y + (i - yo) * ys <= global.yview + global.hview);
 *
 * room_chinatown_10's ending dialogue draws 476 strips per frame: 5.2 ms, of which the draws themselves are ~2 ms and
 * the rest is the compiled script's RValue bookkeeping. This runs the same loop in doubles and calls the same
 * builtins through the function registry (so builtin profiling still sees them), in the same order with the same
 * arguments. Comparisons use g_GMLMathEpsilon like YYCompareVal; cos and degtorad are the runner's own. Anything
 * other than ten arguments (a sprite reference and numbers) and real view globals runs the original. exp_wavynative0 turns it off.
 *
 * Visual compromise, waveStrips=N in yyl.cfg (default 4; 1 is the original, at most 8): strips N times taller, and the
 * sway phase advances N steps per strip, so the wave keeps its shape in coarser steps with N times fewer draws. At
 * N = 4 the nightmare starfield sways in 16-pixel bands instead of 4-pixel ones (neighbouring bands differ by under
 * 3 px). Only the drawing changes: the script draws no random numbers and writes no game state. Measured in the
 * chinatown_10 nightmare dialogue (in-run A/B): 354 strips and 2.14 ms per frame at N = 1, 183 / 1.15 ms at 2,
 * 93 / 0.61 ms at 4; process 23.9 -> 22.4 ms. Debug: exp_waveab cycles N = 1, 2, 4 through the report windows
 * ([WAVY] report line, "A/B: kN"); exp_waveprof times the script ([WAVYPROF]).
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
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

enum { F_PART_EXT, F_GET_ALPHA, F_GET_COLOR, F_HEIGHT, F_WIDTH, F_XOFFSET, F_YOFFSET, NFUNCS };
static const char *const fn_names[NFUNCS] = {"draw_sprite_part_ext", "draw_get_alpha", "draw_get_color",
	"sprite_get_height", "sprite_get_width", "sprite_get_xoffset", "sprite_get_yoffset"};
static runner_function *fns[NFUNCS];

static script_fn orig_wavy;
static double (*yy_degtorad)(double);
static double (*yy_cos)(double);
static const double *math_epsilon;
static const rv *const_zero; // gs_constArg0_7C299A2F, the strip's left edge
static void **global_obj;
static int *var_yview, *var_hview;

unsigned wavy_native_fast, wavy_native_slow, wavy_native_strips;
int wave_strips = 4;      // waveStrips in yyl.cfg: strip height factor (1 = the original)
int wave_ab, wave_ab_k;   // exp_waveab: factor 1, 2, 4 in turn, one report window each (wave_ab_k, 0 = off)
int wave_prof;            // exp_waveprof
uint32_t wave_prof_us, wave_prof_calls;
unsigned wavy_reject[16], wavy_reject_kind; // argc, const, yview, hview, argument 0-9; the last rejected kind

// exp_wavyverify: the original script draws; the native loop only records its draw_sprite_part_ext arguments, and
// a registry wrapper compares each of the original's calls against the record (value bits and kind of all 12).
static int verify;
#define VERIFY_MAX 2048
static rv verify_args[VERIFY_MAX][12];
static int verify_n, verify_i, verify_drawn; // recorded (at most VERIFY_MAX), compared, strips the native drew
static builtin_fn verify_real;
unsigned wavy_verify_calls, wavy_verify_mismatch;

static void verify_part_ext(rv *result, void *self, void *other, int argc, rv *args) {
	// Out of bounds (an unexpected argc, more strips than the native recorded or than the record holds): counts only
	int in_bounds = argc == 12 && verify_i < verify_n && verify_i < VERIFY_MAX;
	int bad = !in_bounds;
	for (int k = 0; !bad && k < 12; k++)
		bad = memcmp(&args[k].val, &verify_args[verify_i][k].val, 8) || (args[k].kind & 0xffffff) !=
			(verify_args[verify_i][k].kind & 0xffffff);
	if (bad && wavy_verify_mismatch < 8) {
		if (in_bounds)
			debugPrintf("[WAVY] mismatch at strip %d of %d: top %.17g/%.17g x %.17g/%.17g y %.17g/%.17g h %.17g/%.17g\n",
				verify_i, verify_n, args[3].val, verify_args[verify_i][3].val, args[6].val, verify_args[verify_i][6].val,
				args[7].val, verify_args[verify_i][7].val, args[5].val, verify_args[verify_i][5].val);
		else
			debugPrintf("[WAVY] mismatch at strip %d: argc %d, native recorded %d (of %d drawn)\n", verify_i, argc,
				verify_n, verify_drawn);
	}
	wavy_verify_mismatch += bad;
	wavy_verify_calls++;
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

static const rv *global_real(int *var) {
	rv *(*getter)(void *, int) = (*(void ***)*global_obj)[2]; // the compiled read: vtable slot 2
	rv *v = getter(*global_obj, var[1]);
	return v && v->kind == KIND_REAL ? v : NULL;
}

static rv real(double x) {
	rv v = {x, 0, KIND_REAL};
	return v;
}

static rv *wavy_body(void *self, void *other, rv *result, int argc, rv **argv) {
	const rv *yview = NULL, *hview = NULL;
	int ok = argc == 10 && const_zero->kind == KIND_REAL && (yview = global_real(var_yview)) &&
		(hview = global_real(var_hview));
	if (!ok)
		wavy_reject[argc != 10 ? 0 : const_zero->kind != KIND_REAL ? 1 : !yview ? 2 : 3]++;
	for (int i = 0; ok && i < 10; i++)
		// the sprite and subimage are passed through as they are: any kind without a reference count (a sprite is
		// a reference, kind 15); the rest is arithmetic
		if (!(ok = argv[i] && (i < 2 ? !((1u << (argv[i]->kind & 0x1f)) & 0x46u) : argv[i]->kind == KIND_REAL)))
			wavy_reject[4 + i]++, wavy_reject_kind = argv[i] ? argv[i]->kind : 0xdead;
	rv alpha, col, h, w, xo, yo;
	if (ok) {
		call(F_GET_ALPHA, &alpha, self, other, 0, NULL);
		call(F_GET_COLOR, &col, self, other, 0, NULL);
		call(F_HEIGHT, &h, self, other, 1, argv[0]);
		call(F_WIDTH, &w, self, other, 1, argv[0]);
		call(F_XOFFSET, &xo, self, other, 1, argv[0]);
		call(F_YOFFSET, &yo, self, other, 1, argv[0]);
		ok = alpha.kind == KIND_REAL && col.kind == KIND_REAL && h.kind == KIND_REAL && w.kind == KIND_REAL &&
			xo.kind == KIND_REAL && yo.kind == KIND_REAL;
	}
	if (!ok) {
		wavy_native_slow++;
		return orig_wavy(self, other, result, argc, argv);
	}
	wavy_native_fast++;
	result->val = 0;
	result->flags = 0;
	result->kind = 5;
	const double x = argv[2]->val, y = argv[3]->val, xs = argv[4]->val, ys = argv[5]->val;
	// waveStrips: N times taller strips, the phase N steps per strip (not while verifying against the original)
	const int n = wave_ab_k ? wave_ab_k : wave_strips;
	const int k = verify || n < 2 ? 1 : n > 8 ? 8 : n;
	const double strip = argv[6]->val * k, amp = argv[7]->val, step = argv[9]->val * k;
	const rv strip_rv = k == 1 ? *argv[6] : real(strip);
	double off = fmod(argv[8]->val, 360.0);
	double i = 0, wx = yy_cos(yy_degtorad(off)) * amp;
	rv args[12];
	do {
		if (cmp(i, h.val) > 0)
			break;
		if (cmp(y + ((i - yo.val) + strip) * ys, yview->val) > 0) {
			double rest = h.val - i;
			args[0] = *argv[0];
			args[1] = *argv[1];
			args[2] = *const_zero;
			args[3] = real(i);
			args[4] = w;
			args[5] = rest < strip ? real(rest) : strip_rv; // YYGML_min keeps the first unless a later one is smaller
			args[6] = real(x + (wx - xo.val) * xs);
			args[7] = real(y + (i - yo.val) * ys);
			args[8] = *argv[4];
			args[9] = *argv[5];
			args[10] = col;
			args[11] = alpha;
			wavy_native_strips++;
			if (verify) {
				verify_drawn++;
				if (verify_n < VERIFY_MAX)
					memcpy(verify_args[verify_n++], args, sizeof(args));
			} else {
				rv out;
				call(F_PART_EXT, &out, self, other, 12, args);
			}
		}
		wx = yy_cos(yy_degtorad(off)) * amp;
		off = fmod(off + step, 360.0);
		i += strip;
		// the view globals are reread each pass, as the compiled loop does (a draw can't change them)
	} while (cmp(y + (i - yo.val) * ys, yview->val + hview->val) < 1);
	if (verify) {
		verify_real = fns[F_PART_EXT]->func;
		fns[F_PART_EXT]->func = verify_part_ext;
		verify_i = 0;
		orig_wavy(self, other, result, argc, argv);
		fns[F_PART_EXT]->func = verify_real;
		if (verify_i != verify_drawn) {
			if (wavy_verify_mismatch < 8)
				debugPrintf("[WAVY] mismatch: original drew %d strips, native %d\n", verify_i, verify_drawn);
			wavy_verify_mismatch++;
		}
		verify_n = verify_drawn = 0;
	}
	return result;
}

static rv *wavy(void *self, void *other, rv *result, int argc, rv **argv) {
	if (!wave_prof)
		return wavy_body(self, other, result, argc, argv);
	uint32_t t = sceKernelGetProcessTimeLow();
	rv *r = wavy_body(self, other, result, argc, argv);
	wave_prof_us += sceKernelGetProcessTimeLow() - t;
	wave_prof_calls++;
	return r;
}

int wavy_native_install(void *(*entry_hook)(uintptr_t entry, void *replacement)) {
#define SYM(name) so_symbol(&yoyoloader_mod, name)
	uintptr_t entry = SYM("_Z27gml_Script_draw_sprite_wavyP9CInstanceS0_R8YYRValueiPPS1_");
	yy_degtorad = (void *)SYM("_Z14YYGML_degtoradd");
	yy_cos = (void *)SYM("_Z9YYGML_cosd");
	math_epsilon = (const double *)SYM("g_GMLMathEpsilon");
	const_zero = (const rv *)SYM("gs_constArg0_7C299A2F");
	global_obj = (void **)SYM("g_pGlobal");
	var_yview = (int *)SYM("g_VAR_yview");
	var_hview = (int *)SYM("g_VAR_hview");
	runner_function **functions = (runner_function **)SYM("the_functions");
	int *count = (int *)SYM("the_numb");
#undef SYM
	if (!entry || !yy_degtorad || !yy_cos || !math_epsilon || !const_zero || !global_obj || !var_yview ||
			!var_hview || !functions || !*functions || !count)
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
	verify = exp_flag("wavyverify");
	extern int debugMode;
	wave_ab = debugMode && exp_flag("waveab");
	wave_ab_k = wave_ab ? 1 : 0;
	wave_prof = debugMode && exp_flag("waveprof");
	return (orig_wavy = entry_hook(entry, wavy)) != NULL;
}
