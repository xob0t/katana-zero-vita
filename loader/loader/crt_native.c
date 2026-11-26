/*
 * crt_native.c
 *
 * The pause screen and the options menus draw a CRT effect row by row in two GML scripts:
 * - draw_crt_surface(surface, step, x, y, xscale, yscale, chance, vhs): obj_gamepaused Draw redraws the paused room
 *   (its 1280x720 screencopy surface) with it every frame, step 4: 180 rows.
 * - draw_crt_image(sprite, subimg, x, y): the menu background of obj_pausemenu, obj_options_main_menu and
 *   obj_optionsmenu Draw, 2-pixel rows.
 * Each row is a few random numbers and one part draw, plus a 1-in-100 run of scan lines:
 *
 *   av_width = <width>; av_i = 0;
 *   while (av_i < <height>) {
 *       distorted = false;
 *       if (instance_exists(obj_vhs_distortion) ...) { ... }          // not handled here: the original runs
 *       if (!distorted) {
 *           if (irandom(<chance>) == 0) {                             // surface: argument6, image: 80
 *               if (irandom(8) == 0) <part>(x + choose(2,-2,..,10,-10) / k, choose(c_white, c_red, c_aqua, c_lime))
 *               else <part>(x + choose(2,-2,..,10,-10) / k', c_white)  // surface: k 8 and 4, image: 1 and 1
 *           } else <part>(x, c_white)                                 // every part draw: alpha random(0.2) + 0.8
 *           if (irandom(100) == 0) {
 *               line_x = 0;
 *               while (line_x < av_width * 2) {
 *                   line_length = irandom(10) * 2;
 *                   if (irandom(2) == 0) {
 *                       draw_set_alpha(random(0.2) + 0.8);
 *                       draw_fastline_width_col(x + line_x, <row y> - 1, x + line_x + line_length, <row y> - 1, 2,
 *                           choose(c_white, c_black));
 *                   }
 *                   line_x += line_length;
 *               }
 *               draw_set_alpha(1);
 *           }
 *       }
 *       av_i += <step>;                                               // surface: argument1, image: 2
 *   }
 *
 * The compiled scripts spend ~17 us per row in Vita3K on RValue copies, variable lookups and argument arrays (3.1 ms
 * per frame for the screencopy, 1.8-2.1 ms for a menu background); the pause and options screens over a paused room
 * ran at 28-33 fps on the Vita. This runs the same rows in C: the same runner calls (irandom, random, choose with the
 * same constant lists, so the random numbers are drawn in the same order; YYGML_draw_surface_part_ext with the same
 * float arguments; draw_sprite_part_ext through YYGML_CallLegacyFunction with the same 12 values; the same
 * draw_set_alpha and draw_fastline_width_col calls) and writes the instance variables the scripts leave behind
 * (av_width, av_i, distorted, line_x, line_length) with their final values, and all of them before a
 * draw_fastline_width_col call. It handles real arguments only (integral where the script converts to an integer,
 * and an integral step of at least 1) and runs the original script whenever obj_vhs_distortion exists (its rows
 * depend on it); nothing else runs during the loop. Doubles round as in the runner: the file is compiled with
 * -ffp-contract=off (loader/exact_sources.cmake).
 *
 * exp_crtnative0 runs the scripts as before. exp_crtfxab runs them as before in every other profiling report window
 * (A/B in one run). exp_crtfxverify (debug) runs the native in a recording mode (no draws), puts the random state and
 * the variables back, runs the original with its part draws and draw_set_alpha calls recorded, and compares the two
 * lists, the random state and the variables after the call. Counters print with the profiler as [CRTFX]. (Until Nov
 * 2026 these two were exp_crtab / exp_crtverify, which also switched collision_round_trip.c's A/B and shadow-tree
 * verify, and the two files shared one crt_ab_off.)
 */

#include <vitasdk.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

#define SYM(name) so_symbol(&yoyoloader_mod, name)

typedef gml_rvalue RV;
typedef RV *(*script_fn)(void *self, void *other, RV *result, int argc, RV **args);
typedef RV *(*var_fn)(void *self, int id);
typedef RV *(*choose_fn)(RV *result, int n, RV **args);
typedef double (*irandom_fn)(long long n);
typedef double (*random_fn)(double n);
typedef void (*part_fn)(int surface, float left, float top, float width, float height, float x, float y,
	float xscale, float yscale, int colour, float alpha);
typedef void (*alpha_fn)(float alpha);
typedef RV *(*legacy_fn)(void *self, void *other, RV *result, int argc, int id, RV **args);
typedef void (*free_fn)(RV *v);
typedef double (*real_fn)(const RV *v);
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;

#define OBJ_VHS_DISTORTION 1044 // obj_vhs_distortion in APK 1.1.10
#define KIND_REAL 0
#define KIND_BOOL 13
#define C_WHITE 16777215.0

enum { V_WIDTH, V_I, V_DISTORTED, V_LINE_X, V_LINE_LENGTH, NVARS };
static const char *const var_symbols[NVARS] = {
	"g_VAR_av_width", "g_VAR_av_i", "g_VAR_distorted", "g_VAR_line_x", "g_VAR_line_length",
};
static int *var_descs[NVARS]; // g_VAR_*: the ids at [1] are set by the runner after the loader installs

static script_fn orig_surface, orig_image, fastline;
static choose_fn choose;
static irandom_fn irandom;
static random_fn random_real;
static part_fn surface_part;
static alpha_fn set_alpha;
static legacy_fn call_legacy;
static free_fn free_rvalue;
static real_fn real_ex; // REAL_RValue_Ex: a reference's id as a real
static builtin_fn instance_exists_fn, surface_width_fn, surface_height_fn, sprite_width_fn, sprite_height_fn;
static int func_sprite_part = -1; // g_FUNC_draw_sprite_part_ext's id (set by the runner after the loader installs)
static int *func_desc, table_state; // 0 not looked up yet, 1 ready, -1 unusable
static const double *epsilon;
static Trace **trace_start;
static uint32_t *rng_state, *rng_index;

// choose()'s argument lists, as the scripts' constants (reals)
static RV offsets[10], colours[4], line_colours[2];
static RV *offset_args[10], *colour_args[4], *line_colour_args[2];

static int installed, ab_mode, verify;
static int crt_fx_ab_off; // exp_crtfxab: this report window runs the original scripts
unsigned crt_calls[2], crt_rows[2], crt_fallbacks[2], crt_lines;
static unsigned fail_reason[8];
static unsigned verify_calls, verify_mismatches, verify_ops;

// --- verify recording: the native's draws (record mode) and the original's (hooks below) ---
enum { OP_SURFACE, OP_SPRITE, OP_ALPHA };
typedef struct { int type; uint32_t w[12]; double d[12]; uint32_t k[12]; } op_t;
#define MAX_OPS 1536
static op_t rec[2][MAX_OPS];
static int rec_n[2], rec_overflow[2];
static int recording = -1; // -1 off, 0 native (draws suppressed), 1 original (draws pass through)
static part_fn orig_surface_part_hook;
static alpha_fn orig_set_alpha_hook;
static builtin_fn orig_sprite_part_builtin;
static runner_function *sprite_part_entry;

static op_t *rec_next(void) {
	if (recording < 0)
		return NULL;
	if (rec_n[recording] >= MAX_OPS) {
		rec_overflow[recording] = 1;
		return NULL;
	}
	op_t *o = &rec[recording][rec_n[recording]++];
	memset(o, 0, sizeof(*o));
	return o;
}

static void rec_part(int surface, float left, float top, float width, float height, float x, float y, float xscale,
		float yscale, int colour, float alpha) {
	op_t *o = rec_next();
	if (!o)
		return;
	float f[9] = { left, top, width, height, x, y, xscale, yscale, alpha };
	o->type = OP_SURFACE;
	o->w[0] = (uint32_t)surface;
	memcpy(&o->w[1], f, sizeof(f));
	o->w[10] = (uint32_t)colour;
}

static void hook_surface_part(int surface, float left, float top, float width, float height, float x, float y,
		float xscale, float yscale, int colour, float alpha) {
	if (recording == 1)
		rec_part(surface, left, top, width, height, x, y, xscale, yscale, colour, alpha);
	orig_surface_part_hook(surface, left, top, width, height, x, y, xscale, yscale, colour, alpha);
}

static void rec_sprite_part(int argc, const RV *args) {
	op_t *o = rec_next();
	if (!o)
		return;
	o->type = OP_SPRITE;
	for (int i = 0; i < argc && i < 12; i++) {
		o->d[i] = args[i].val;
		o->k[i] = args[i].kind & 0xffffff;
	}
	o->w[0] = (uint32_t)argc;
}

static void hook_sprite_part(RV *result, void *self, void *other, int argc, RV *args) {
	if (recording == 1)
		rec_sprite_part(argc, args);
	orig_sprite_part_builtin(result, self, other, argc, args);
}

static void rec_alpha(float alpha) {
	op_t *o = rec_next();
	if (!o)
		return;
	o->type = OP_ALPHA;
	memcpy(&o->w[0], &alpha, 4);
}

static void hook_set_alpha(float alpha) {
	if (recording == 1)
		rec_alpha(alpha);
	orig_set_alpha_hook(alpha);
}

// --- runner helpers ---

static RV *var_write(void *self, int v) {
	var_fn vwrite = (var_fn)(*(void ***)self)[3];
	return vwrite(self, var_descs[v][1]);
}

static void put_var(void *self, int v, double val, uint32_t kind) {
	RV *r = var_write(self, v);
	if (!r)
		return;
	if ((1u << (r->kind & 31)) & 0x46)
		free_rvalue(r);
	r->val = val;
	r->kind = kind;
}

static int is_zero(double d) {
	return fabs(d) <= *epsilon;
}

static void draw_alpha(float alpha) {
	if (recording == 0)
		rec_alpha(alpha);
	else
		set_alpha(alpha);
}

static int integral(const RV *v, double lo, double hi) {
	return v && (v->kind & 0xffffff) == KIND_REAL && v->val >= lo && v->val <= hi && v->val == floor(v->val);
}

// A surface or sprite: an integral real or a reference (kind 15)
static int asset_arg(const RV *v) {
	return v && ((v->kind & 0xffffff) == 15 || integral(v, -1.0, 1e6));
}

static int real_arg(const RV *v) {
	return v && (v->kind & 0xffffff) == KIND_REAL && isfinite(v->val);
}

static int vhs_distortion(void *self, void *other) {
	RV result = {0, 0, 5}, arg;
	uint32_t ref[4] = {OBJ_VHS_DISTORTION, 0x1000000, 0, 15}; // the scripts' argument: a reference to the object
	memcpy(&arg, ref, sizeof(arg));
	instance_exists_fn(&result, self, other, 1, &arg);
	return result.val != 0.0;
}

static double builtin_real(builtin_fn fn, void *self, void *other, RV *arg, RV *out) {
	*out = (RV){0, 0, 5};
	fn(out, self, other, 1, arg);
	return out->val;
}

static int fail(int reason) {
	fail_reason[reason]++;
	return 0;
}

// First rejection per reason: the argument kinds and values (debug log)
static void fail_log(int reason, int argc, RV **args) {
	if (fail_reason[reason] != 1)
		return;
	char line[256];
	int n = 0;
	for (int i = 0; i < argc && i < 8 && n < (int)sizeof(line) - 40; i++)
		n += snprintf(line + n, sizeof(line) - n, " %u:%g", args[i] ? args[i]->kind : 99u, args[i] ? args[i]->val : 0.0);
	debugPrintf("[CRTFX] first rejection, reason %d, argc %d:%s\n", reason, argc, line);
}

// The scan lines of one row (row_y: the row's y before the - 1). The variables are written before each line draw.
static void scan_lines(void *self, void *other, double x, double row_y, double av_width, double av_i,
		double *line_x_out, double *line_length_out) {
	double line_x = 0.0, line_length = *line_length_out;
	crt_lines++;
	while (line_x < av_width * 2.0) {
		line_length = irandom(10);
		line_length = line_length + line_length;
		if (is_zero(irandom(2))) {
			draw_alpha((float)(random_real(0.2) + 0.8));
			RV a[6] = {
				{line_x + x, 0, KIND_REAL}, {row_y - 1.0, 0, KIND_REAL}, {line_x + line_length + x, 0, KIND_REAL},
				{row_y - 1.0, 0, KIND_REAL}, {2.0, 0, KIND_REAL}, {0, 0, 5},
			};
			RV chosen;
			choose(&chosen, 2, line_colour_args);
			a[5] = chosen;
			if (recording != 0) {
				put_var(self, V_WIDTH, av_width, KIND_REAL);
				put_var(self, V_I, av_i, KIND_REAL);
				put_var(self, V_DISTORTED, 0.0, KIND_BOOL);
				put_var(self, V_LINE_X, line_x, KIND_REAL);
				put_var(self, V_LINE_LENGTH, line_length, KIND_REAL);
				RV res = {0, 0, 5};
				RV *ap[6] = { &a[0], &a[1], &a[2], &a[3], &a[4], &a[5] };
				fastline(self, other, &res, 6, ap);
				if ((1u << (res.kind & 31)) & 0x46)
					free_rvalue(&res);
			}
		}
		line_x += line_length;
	}
	draw_alpha(1.0f);
	*line_x_out = line_x;
	*line_length_out = line_length;
}

// Final variable values (the loop writes them on the way in the scripts)
static void finish(void *self, int rows, double av_i, int lines, double line_x, double line_length) {
	put_var(self, V_I, av_i, KIND_REAL);
	if (rows)
		put_var(self, V_DISTORTED, 0.0, KIND_BOOL);
	if (lines) {
		put_var(self, V_LINE_X, line_x, KIND_REAL);
		put_var(self, V_LINE_LENGTH, line_length, KIND_REAL);
	}
}

static int vars_ready(void) {
	for (int i = 0; i < NVARS; i++)
		if (var_descs[i][1] < 0)
			return 0;
	return 1;
}

static int surface_admit(void *self, void *other, RV *result, int argc, RV **args) {
	if (!installed || !vars_ready() || (ab_mode && crt_fx_ab_off) || argc != 8 || !args || !result)
		return fail(0);
	for (int i = 1; i < 7; i++)
		if (!real_arg(args[i]) || args[i] == result) {
			fail(1);
			fail_log(1, argc, args);
			return 0;
		}
	if (!asset_arg(args[0]) || args[0] == result || !integral(args[1], 1.0, 4096.0) || !integral(args[6], 0.0, 1e6))
		return fail(2);
	if (vhs_distortion(self, other))
		return fail(3);
	return 1;
}

static void surface_rows(void *self, void *other, RV **args) {
	// the script's int conversion: the value (a reference's id through REAL_RValue_Ex), truncated
	const int surface = (int)((args[0]->kind & 0xffffff) == KIND_REAL ? args[0]->val : real_ex(args[0]));
	const double step = args[1]->val, x = args[2]->val, y = args[3]->val, xscale = args[4]->val,
		yscale = args[5]->val;
	const long long chance = (long long)args[6]->val;
	RV width_rv, height_rv;
	builtin_real(surface_width_fn, self, other, args[0], &width_rv);
	RV *w = var_write(self, V_WIDTH); // av_width = surface_get_width(argument0)
	if (w) {
		if ((1u << (w->kind & 31)) & 0x46)
			free_rvalue(w);
		*w = width_rv;
	}
	const double av_width = width_rv.val, height = builtin_real(surface_height_fn, self, other, args[0], &height_rv);
	double av_i = 0.0, line_x = 0.0, line_length = 0.0;
	int rows = 0, lines = 0;
	while (av_i < height) {
		rows++;
		const double row_y = y + av_i * yscale;
		RV chosen;
		if (is_zero(irandom(chance))) {
			if (is_zero(irandom(8))) {
				double dx = choose(&chosen, 10, offset_args)->val / 8.0;
				int colour = (int)(long long)choose(&chosen, 4, colour_args)->val;
				double alpha = random_real(0.2);
				(recording == 0 ? rec_part : surface_part)(surface, 0.0f, (float)av_i, (float)av_width, (float)step,
					(float)(x + dx), (float)row_y, (float)xscale, (float)yscale, colour, (float)(alpha + 0.8));
			} else {
				double dx = choose(&chosen, 10, offset_args)->val / 4.0;
				double alpha = random_real(0.2);
				(recording == 0 ? rec_part : surface_part)(surface, 0.0f, (float)av_i, (float)av_width, (float)step,
					(float)(x + dx), (float)row_y, (float)xscale, (float)yscale, 0xffffff, (float)(alpha + 0.8));
			}
		} else {
			double alpha = random_real(0.2);
			(recording == 0 ? rec_part : surface_part)(surface, 0.0f, (float)av_i, (float)av_width, (float)step,
				(float)x, (float)row_y, (float)xscale, (float)yscale, 0xffffff, (float)(alpha + 0.8));
		}
		if (is_zero(irandom(100))) {
			lines = 1;
			scan_lines(self, other, x, row_y, av_width, av_i, &line_x, &line_length);
		}
		av_i += step;
	}
	finish(self, rows, av_i, lines, line_x, line_length);
	crt_rows[0] += rows;
}

// draw_sprite_part_ext's slot in the runner's function table, looked up at the first call (the ids are assigned
// after the loader installs); verify swaps the slot's function for a recorder
static int table_ready(void) {
	if (table_state)
		return table_state > 0;
	runner_function **functions = (runner_function **)SYM("the_functions");
	int *count = (int *)SYM("the_numb");
	int id = func_desc[1];
	if (!functions || !*functions || !count || id < 0 || id >= *count || !(*functions)[id].name ||
			strcmp((*functions)[id].name, "draw_sprite_part_ext")) {
		debugPrintf("[CRTFX] draw_sprite_part_ext not in the function table (id %d of %d): image rows run the original\n",
			id, count ? *count : -1);
		table_state = -1;
		return 0;
	}
	func_sprite_part = id;
	if (verify) {
		sprite_part_entry = &(*functions)[id];
		orig_sprite_part_builtin = sprite_part_entry->func;
		sprite_part_entry->func = hook_sprite_part;
	}
	table_state = 1;
	return 1;
}

static int image_admit(void *self, void *other, RV *result, int argc, RV **args) {
	if (!table_ready())
		return fail(4);
	if (!installed || !vars_ready() || (ab_mode && crt_fx_ab_off) || argc != 4 || !args || !result)
		return fail(4);
	for (int i = 1; i < 4; i++)
		if (!real_arg(args[i]) || args[i] == result) {
			fail(5);
			fail_log(5, argc, args);
			return 0;
		}
	if (!asset_arg(args[0]) || args[0] == result)
		return fail(6);
	if (vhs_distortion(self, other))
		return fail(7);
	return 1;
}

static void sprite_part(void *self, void *other, RV *a) {
	if (recording == 0) {
		rec_sprite_part(12, a);
		return;
	}
	RV *ap[12];
	for (int i = 0; i < 12; i++)
		ap[i] = &a[i];
	RV res = {0, 0, 5};
	call_legacy(self, other, &res, 12, func_sprite_part, ap);
}

static void image_rows(void *self, void *other, RV **args) {
	const double x = args[2]->val, y = args[3]->val;
	RV width_rv, height_rv;
	builtin_real(sprite_width_fn, self, other, args[0], &width_rv);
	RV *w = var_write(self, V_WIDTH); // av_width = sprite_get_width(argument0)
	if (w) {
		if ((1u << (w->kind & 31)) & 0x46)
			free_rvalue(w);
		*w = width_rv;
	}
	const double av_width = width_rv.val, height = builtin_real(sprite_height_fn, self, other, args[0], &height_rv);
	double av_i = 0.0, line_x = 0.0, line_length = 0.0;
	int rows = 0, lines = 0;
	while (av_i < height) {
		rows++;
		const double row_y = y + av_i * 2.0;
		// draw_sprite_part_ext(sprite, subimg, 0, av_i, av_width, 2, x', row_y, 2, 2, colour, alpha)
		RV a[12] = {
			*args[0], *args[1], {0.0, 0, KIND_REAL}, {av_i, 0, KIND_REAL}, width_rv, {2.0, 0, KIND_REAL},
			{x, 0, KIND_REAL}, {row_y, 0, KIND_REAL}, {2.0, 0, KIND_REAL}, {2.0, 0, KIND_REAL},
			{C_WHITE, 0, KIND_REAL}, {0, 0, KIND_REAL},
		};
		RV chosen;
		if (is_zero(irandom(80))) {
			if (is_zero(irandom(8))) {
				a[6].val = x + choose(&chosen, 10, offset_args)->val;
				a[10] = *choose(&chosen, 4, colour_args);
			} else
				a[6].val = x + choose(&chosen, 10, offset_args)->val;
		}
		a[11].val = random_real(0.2) + 0.8;
		sprite_part(self, other, a);
		if (is_zero(irandom(100))) {
			lines = 1;
			scan_lines(self, other, x, row_y, av_width, av_i, &line_x, &line_length);
		}
		av_i += 2.0;
	}
	finish(self, rows, av_i, lines, line_x, line_length);
	crt_rows[1] += rows;
}

// --- verify ---

typedef struct { RV v[NVARS]; uint32_t rng[16], rng_index; } crt_state;

static void state_read(void *self, crt_state *s) {
	for (int i = 0; i < NVARS; i++) {
		RV *r = var_write(self, i);
		s->v[i] = r ? *r : (RV){0, 0, 0xffffff};
	}
	memcpy(s->rng, rng_state, sizeof(s->rng));
	s->rng_index = *rng_index;
}

static void state_write(void *self, const crt_state *s) {
	for (int i = 0; i < NVARS; i++) {
		RV *r = var_write(self, i);
		if (r && !(((1u << (r->kind & 31)) & 0x46) || ((1u << (s->v[i].kind & 31)) & 0x46)))
			*r = s->v[i];
	}
	memcpy(rng_state, s->rng, sizeof(s->rng));
	*rng_index = s->rng_index;
}

static int op_equal(const op_t *a, const op_t *b) {
	return a->type == b->type && !memcmp(a->w, b->w, sizeof(a->w)) && !memcmp(a->d, b->d, sizeof(a->d)) &&
		!memcmp(a->k, b->k, sizeof(a->k));
}

static void verify_call(int image, void *self, void *other, RV *result, int argc, RV **args) {
	crt_state before, native, after;
	state_read(self, &before);
	rec_n[0] = rec_n[1] = rec_overflow[0] = rec_overflow[1] = 0;
	recording = 0;
	(image ? image_rows : surface_rows)(self, other, args);
	state_read(self, &native);
	state_write(self, &before);
	recording = 1;
	(image ? orig_image : orig_surface)(self, other, result, argc, args);
	recording = -1;
	state_read(self, &after);
	verify_calls++;
	verify_ops += rec_n[1];
	int bad = rec_overflow[0] || rec_overflow[1] || rec_n[0] != rec_n[1];
	int first = -1;
	for (int i = 0; i < rec_n[0] && i < rec_n[1]; i++)
		if (!op_equal(&rec[0][i], &rec[1][i])) {
			bad = 1;
			first = i;
			break;
		}
	int rng_bad = memcmp(native.rng, after.rng, sizeof(native.rng)) || native.rng_index != after.rng_index;
	int var_bad = -1;
	for (int i = 0; i < NVARS; i++)
		if (native.v[i].kind != after.v[i].kind || memcmp(&native.v[i].val, &after.v[i].val, 8))
			var_bad = i;
	if (bad || rng_bad || var_bad >= 0) {
		if (verify_mismatches++ < 12) {
			debugPrintf("[CRTFX] verify mismatch (%s): ops %d/%d overflow %d/%d first differing op %d, rng %s, var %d\n",
				image ? "image" : "surface", rec_n[0], rec_n[1], rec_overflow[0], rec_overflow[1], first,
				rng_bad ? "differs" : "same", var_bad);
			if (first >= 0) {
				const op_t *a = &rec[0][first], *b = &rec[1][first];
				debugPrintf("[CRTFX]   native type %d w %08X %08X %08X %08X %08X %08X %08X d %g %g %g %g %g %g %g %g\n", a->type,
					a->w[0], a->w[1], a->w[2], a->w[3], a->w[5], a->w[6], a->w[9], a->d[3], a->d[4], a->d[6], a->d[7],
					a->d[10], a->d[11], a->d[0], a->d[1]);
				debugPrintf("[CRTFX]   orig   type %d w %08X %08X %08X %08X %08X %08X %08X d %g %g %g %g %g %g %g %g\n", b->type,
					b->w[0], b->w[1], b->w[2], b->w[3], b->w[5], b->w[6], b->w[9], b->d[3], b->d[4], b->d[6], b->d[7],
					b->d[10], b->d[11], b->d[0], b->d[1]);
			}
			if (var_bad >= 0)
				debugPrintf("[CRTFX]   var %d native %g kind %u, original %g kind %u\n", var_bad, native.v[var_bad].val,
					native.v[var_bad].kind, after.v[var_bad].val, after.v[var_bad].kind);
		}
	}
}

// --- entries ---

static RV *run(int image, void *self, void *other, RV *result, int argc, RV **args) {
	Trace trace = { *trace_start, image ? "gml_Script_draw_crt_image" : "gml_Script_draw_crt_surface", 12 };
	*trace_start = &trace;
	result->kind = 5;
	result->val = 0.0;
	(image ? image_rows : surface_rows)(self, other, args);
	*trace_start = trace.previous;
	return result;
}

static RV *crt_surface(void *self, void *other, RV *result, int argc, RV **args) {
	if (recording >= 0 || !surface_admit(self, other, result, argc, args)) {
		crt_fallbacks[0]++;
		return orig_surface(self, other, result, argc, args);
	}
	crt_calls[0]++;
	if (verify) {
		verify_call(0, self, other, result, argc, args);
		return result;
	}
	return run(0, self, other, result, argc, args);
}

static RV *crt_image(void *self, void *other, RV *result, int argc, RV **args) {
	if (recording >= 0 || !image_admit(self, other, result, argc, args)) {
		crt_fallbacks[1]++;
		return orig_image(self, other, result, argc, args);
	}
	crt_calls[1]++;
	if (verify) {
		verify_call(1, self, other, result, argc, args);
		return result;
	}
	return run(1, self, other, result, argc, args);
}

void crt_native_report(int frames) {
	if (!installed)
		return;
	if (crt_calls[0] || crt_calls[1] || crt_fallbacks[0] || crt_fallbacks[1])
		debugPrintf("[CRTFX] native surface %.1f calls %.0f rows, image %.1f calls %.0f rows, scan lines %.2f, original "
			"%.1f/%.1f per frame%s | reasons %u %u %u %u %u %u %u %u\n", (float)crt_calls[0] / frames,
			(float)crt_rows[0] / frames, (float)crt_calls[1] / frames, (float)crt_rows[1] / frames,
			(float)crt_lines / frames, (float)crt_fallbacks[0] / frames, (float)crt_fallbacks[1] / frames,
			ab_mode && crt_fx_ab_off ? " (A/B: original this window)" : "", fail_reason[0], fail_reason[1],
			fail_reason[2], fail_reason[3], fail_reason[4], fail_reason[5], fail_reason[6], fail_reason[7]);
	if (verify)
		debugPrintf("[CRTFX] verify: %u calls compared, %u draw ops, %u mismatches\n", verify_calls, verify_ops,
			verify_mismatches);
	crt_calls[0] = crt_calls[1] = crt_rows[0] = crt_rows[1] = crt_fallbacks[0] = crt_fallbacks[1] = crt_lines = 0;
	if (ab_mode)
		crt_fx_ab_off = !crt_fx_ab_off;
}

void crt_native_install(void) {
	if (exp_flag("crtnative0")) {
		debugPrintf("CRT effect rows: original scripts (exp_crtnative0)\n");
		return;
	}
	for (int i = 0; i < NVARS; i++) {
		int *v = (int *)SYM(var_symbols[i]);
		if (!v) {
			debugPrintf("CRT effect rows: NOT installed (no %s)\n", var_symbols[i]);
			return;
		}
		var_descs[i] = v;
	}
	choose = (choose_fn)SYM("_Z12YYGML_chooseR8YYRValueiPPS_");
	irandom = (irandom_fn)SYM("_Z13YYGML_irandomx");
	random_real = (random_fn)SYM("_Z12YYGML_randomd");
	surface_part = (part_fn)SYM("_Z27YYGML_draw_surface_part_extiffffffffif");
	set_alpha = (alpha_fn)SYM("_Z20YYGML_draw_set_alphaf");
	call_legacy = (legacy_fn)SYM("_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	free_rvalue = (free_fn)SYM("_Z16FREE_RValue__PreP6RValue");
	real_ex = (real_fn)SYM("_Z14REAL_RValue_ExPK6RValue");
	instance_exists_fn = (builtin_fn)SYM("_Z16F_InstanceExistsR6RValueP9CInstanceS2_iPS_");
	surface_width_fn = (builtin_fn)SYM("_Z17F_SurfaceGetWidthR6RValueP9CInstanceS2_iPS_");
	surface_height_fn = (builtin_fn)SYM("_Z18F_SurfaceGetHeightR6RValueP9CInstanceS2_iPS_");
	sprite_width_fn = (builtin_fn)SYM("_Z16F_SpriteGetWidthR6RValueP9CInstanceS2_iPS_");
	sprite_height_fn = (builtin_fn)SYM("_Z17F_SpriteGetHeightR6RValueP9CInstanceS2_iPS_");
	fastline = (script_fn)SYM("_Z34gml_Script_draw_fastline_width_colP9CInstanceS0_R8YYRValueiPPS1_");
	int *func = (int *)SYM("g_FUNC_draw_sprite_part_ext");
	epsilon = (const double *)SYM("g_GMLMathEpsilon");
	trace_start = (Trace **)SYM("_ZN13SYYStackTrace8s_pStartE");
	rng_state = (uint32_t *)SYM("state");
	rng_index = (uint32_t *)SYM("g_RndIndex");
	if (!choose || !irandom || !random_real || !surface_part || !set_alpha || !call_legacy || !free_rvalue || !real_ex ||
			!instance_exists_fn || !surface_width_fn || !surface_height_fn || !sprite_width_fn || !sprite_height_fn ||
			!fastline || !func || !epsilon || !trace_start || !rng_state || !rng_index) {
		debugPrintf("CRT effect rows: NOT installed (runner symbols missing)\n");
		return;
	}
	func_desc = func;
	static const double offset_values[10] = { 2, -2, 4, -4, 6, -6, 8, -8, 10, -10 };
	static const double colour_values[4] = { 16777215.0, 255.0, 16776960.0, 65280.0 };
	for (int i = 0; i < 10; i++) {
		offsets[i] = (RV){ offset_values[i], 0, KIND_REAL };
		offset_args[i] = &offsets[i];
	}
	for (int i = 0; i < 4; i++) {
		colours[i] = (RV){ colour_values[i], 0, KIND_REAL };
		colour_args[i] = &colours[i];
	}
	line_colours[0] = (RV){ C_WHITE, 0, KIND_REAL };
	line_colours[1] = (RV){ 0.0, 0, KIND_REAL };
	line_colour_args[0] = &line_colours[0];
	line_colour_args[1] = &line_colours[1];

	verify = debugMode && exp_flag("crtfxverify");
	ab_mode = debugMode && exp_flag("crtfxab");
	if (verify) {
		orig_surface_part_hook = entry_hook((uintptr_t)surface_part, hook_surface_part);
		orig_set_alpha_hook = entry_hook((uintptr_t)set_alpha, hook_set_alpha);
		if (!orig_surface_part_hook || !orig_set_alpha_hook) {
			debugPrintf("CRT effect rows: verify hooks NOT installed, verify off\n");
			verify = 0;
		} else {
			// the native's own draws must not be recorded as the original's
			surface_part = orig_surface_part_hook;
			set_alpha = orig_set_alpha_hook;
		}
	}
	orig_surface = entry_hook((uintptr_t)SYM("_Z27gml_Script_draw_crt_surfaceP9CInstanceS0_R8YYRValueiPPS1_"), crt_surface);
	orig_image = entry_hook((uintptr_t)SYM("_Z25gml_Script_draw_crt_imageP9CInstanceS0_R8YYRValueiPPS1_"), crt_image);
	installed = 1;
	debugPrintf("CRT effect rows: surface %s, image %s%s%s\n", orig_surface ? "native" : "NOT installed",
		orig_image ? "native" : "NOT installed", verify ? ", verify on" : "", ab_mode ? ", A/B by report window" : "");
}
