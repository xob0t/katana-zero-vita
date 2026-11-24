/* Native single-glyph text for dialogue letters (libyoyo.so SHA256 ab7014c1...; the PLT-patched runner).
 *
 * Every dialogue letter (obj_letter) draws its one character with draw_text_transformed. The runner's path for that
 * is F_DrawTextTransformed -> GR_Text_Draw_Transformed (0xea9e60) -> CFontGM::Draw_String_Transformed. Around the
 * glyph quad it spends most of its time on Split_TextBlock: utf8 decoding into a fresh heap buffer, a heap line
 * node, then both freed again; plus the argument getters, the font lookup and an "M" line height.
 *
 * letter_glyph_draw does what GR_Text_Draw_Transformed does for a one-character string, in the same float operations
 * (this file is built with -ffp-contract=off; VFP vmla/vmls round twice, like separate C multiply and add), and calls
 * the runner's own Draw_String_Transformed with the same arguments, so the quad, its UVs, colour, alpha and the
 * batch it lands in come from the runner. What it leaves out has no effect outside the call: the heap buffer and
 * line node, and the SDF/drop-shadow passes, which it doesn't handle (SDF fonts fall back to the runner).
 *
 * Admission (anything else -> the runner's builtin): a string argument of exactly one code point (the runner's
 * utf8_strlen / utf8_extract_char) that isn't \n or \r (those make Split_TextBlock produce no line), five real
 * arguments, the current font existing (Font_Exists), and not SDF.
 *
 * The dialogue fonts are sprite fonts, so Draw_String_Transformed draws each glyph with CSprite::Draw (its own
 * frustum cull, then GR_Texture_Draw -> Graphics::AllocVerts, which already batches consecutive same-texture quads).
 *
 * Cost on the Vita (500 MHz, exp_lettertime, club_4_2): the runner's call ~32 us per letter with timers, of which
 * Split_TextBlock 5.8 us, its two MemoryManager::Free 4.9 us each, Draw_String_Transformed 9.6 us; the native call
 * 8.5-8.8 us (Draw_String_Transformed is most of it).
 *
 * exp_letterglyph0: off. exp_letterab (letter_native.c): off in every other report window. exp_letterglyphverify:
 * every call runs the native path with the vertex submissions (Graphics::AllocVerts) captured into scratch memory,
 * then the original for real, and compares the submissions (type, texture, format, count, every vertex byte) and the
 * font global. exp_letterglyphverifyfault: the native x coordinate one float step off (negative control).
 * exp_lettertime (debug timing) also times the runner's text internals during letter text calls. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <psp2/kernel/processmgr.h>
#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef const char *ABI (*getstr_fn)(const RV *, int);
typedef float ABI (*getfloat_fn)(const RV *, int);
typedef int ABI (*strlen8_fn)(const char *);
typedef unsigned ABI (*extract_fn)(const char **);
typedef int ABI (*int_int_fn)(int);
typedef void *ABI (*fontdata_fn)(int);
typedef void ABI (*sincos_fn)(float, float *, float *);
typedef int ABI (*height_fn)(void *, const unsigned *);
typedef int ABI (*width_fn)(void *, const unsigned *, float);
typedef unsigned ABI (*colour_fn)(void);
typedef float ABI (*alpha_get_fn)(void);
typedef void ABI (*dst_fn)(void *, float, float, const unsigned *, float, float, float, unsigned, float);
typedef void *ABI (*alloc_verts_fn)(int, void *, int, int);
typedef void ABI (*split_fn)(const char *, int, void *);
typedef void *ABI (*glyph_fn)(void *, int, int);
typedef void ABI (*free_fn)(const void *, int);
typedef void ABI (*void_fn)(void);

static getstr_fn get_string;
static getfloat_fn get_float;
static strlen8_fn utf8_length;
static extract_fn utf8_char;
static int_int_fn font_exists;
static fontdata_fn font_data;
static uintptr_t *sincos_slot;
static height_fn text_height;
static width_fn text_width;
static colour_fn get_colour;
static alpha_get_fn get_alpha;
static dst_fn draw_string;
static int *fontid, *halign, *valign;
static void **thefont;
static const unsigned *letter_m; /* the "M" the runner measures the line height with */

int letter_glyph = 1, letter_glyph_verify, letter_glyph_fault;
extern int letter_ab, letter_ab_off; /* letter_native.c: exp_letterab */
extern unsigned letter_draw_window, letter_line_fills, letter_line_hits;
unsigned letter_glyph_native, letter_glyph_fallback, letter_glyph_reject[8];
unsigned letter_glyph_verified, letter_glyph_mismatch, letter_glyph_quads, letter_glyph_unverified;
static int installed;

/* ---- exp_lettertime: the runner's text internals, timed only inside letter text calls ---- */
extern int letter_timing;
static int in_text;
enum { R_SPLIT, R_DRAWSTRING, R_ALLOCVERTS, R_GETGLYPH, R_FREE, R_FLUSH, R_TEXT, R_NATIVE, NR };
uint64_t letter_glyph_time[NR]; unsigned letter_glyph_time_n[NR];
static split_fn orig_split;
static dst_fn orig_draw_string;
static alloc_verts_fn orig_alloc_verts;
static glyph_fn orig_get_glyph;
static free_fn orig_free;
static void_fn orig_flush;
#define NOW() sceKernelGetProcessTimeWide()
#define TIMED(i, call) do { if (in_text) { uint64_t t0_ = NOW(); call; letter_glyph_time[i] += NOW() - t0_; \
	letter_glyph_time_n[i]++; } else { call; } } while (0)
static void ABI split_timed(const char *s, int w, void *list) { TIMED(R_SPLIT, orig_split(s, w, list)); }
static void ABI draw_string_timed(void *f, float x, float y, const unsigned *s, float xs, float ys, float a, unsigned c,
		float al) { TIMED(R_DRAWSTRING, orig_draw_string(f, x, y, s, xs, ys, a, c, al)); }
static void *ABI glyph_timed(void *f, int c, int b) { void *r; TIMED(R_GETGLYPH, r = orig_get_glyph(f, c, b)); return r; }
static void ABI free_timed(const void *p, int b) { TIMED(R_FREE, orig_free(p, b)); }
static void ABI flush_timed(void) { TIMED(R_FLUSH, orig_flush()); }

/* ---- exp_letterglyphverify: capture of the vertex submissions ---- */
#define MAX_QUADS 4
#define MAX_BYTES (6 * 0x18)
typedef struct { int prim, stride, count; void *tex; uint8_t *at; uint8_t bytes[MAX_BYTES]; } Submit;
typedef struct { Submit s[MAX_QUADS]; int n, overflow; } Capture;
static Capture *capturing; static int capture_scratch; /* 1: native pass, verts go to scratch, nothing drawn */
static uint8_t scratch[MAX_QUADS][MAX_BYTES + 64];
static void capture_close(Capture *c) {
	if (c->n > 0 && c->s[c->n - 1].at) {
		Submit *s = &c->s[c->n - 1];
		int bytes = s->stride * s->count;
		memcpy(s->bytes, s->at, bytes > MAX_BYTES ? MAX_BYTES : bytes);
		s->at = NULL;
	}
}
static void *ABI alloc_verts_hook(int prim, void *tex, int stride, int count) {
	void *r;
	Capture *c = capturing;
	if (c) {
		capture_close(c); /* the previous quad is complete; a flush in this call may reuse its memory */
		if (c->n >= MAX_QUADS || stride * count > MAX_BYTES || stride * count < 0) {
			c->overflow = 1;
			if (capture_scratch)
				return scratch[0];
			return orig_alloc_verts(prim, tex, stride, count);
		}
		Submit *s = &c->s[c->n];
		s->prim = prim; s->tex = tex; s->stride = stride; s->count = count;
		r = capture_scratch ? (void *)scratch[c->n] : orig_alloc_verts(prim, tex, stride, count);
		s->at = r; c->n++;
		return r;
	}
	TIMED(R_ALLOCVERTS, r = orig_alloc_verts(prim, tex, stride, count));
	return r;
}

/* GR_Text_Draw_Transformed (0xea9e60) for one line holding one character, called with line separation -1 and
 * width -1 by F_DrawTextTransformed. 0: not handled, nothing done. */
static int glyph(const RV *args, int fault) {
	if ((args[2].kind & 0x00ffffff) != 1) { letter_glyph_reject[5]++; return 0; }
	for (int i = 0; i < 6; i++)
		if (i != 2 && (args[i].kind & 0x00ffffff) != 0) { letter_glyph_reject[6]++; return 0; }
	/* F_DrawTextTransformed's argument order: the string first, then x, y, xscale, yscale, angle */
	const char *text = get_string(args, 2);
	if (!text || utf8_length(text) != 1) { letter_glyph_reject[0]++; return 0; }
	const char *p = text;
	unsigned line[2] = { utf8_char(&p), 0 };
	if (!line[0] || line[0] == '\n' || line[0] == '\r') { letter_glyph_reject[1]++; return 0; }
	if (!font_exists(*fontid)) { letter_glyph_reject[2]++; return 0; }
	uint8_t *font = font_data(*fontid);
	if (!font || font[0xef]) { letter_glyph_reject[3]++; return 0; } /* SDF: Font_Start_Rendering_SDF, shadows */
	float x = get_float(args, 0), y = get_float(args, 1), xs = get_float(args, 3), ys = get_float(args, 4);
	float angle = get_float(args, 5);
	*thefont = font;
	float a = angle * 3.1415927410125732f;
	a = a / 180.0f;
	float sn, cs;
	((sincos_fn)*sincos_slot)(a, &sn, &cs);
	int sep = text_height(font, letter_m);
	float h = (float)sep, cy = cs * ys, sy = sn * ys;
	float down_y = cy * h, down_x = sy * h; /* s26, s28 */
	int va = *valign;
	if (va == 2) {
		float n = 1.0f;
		x = x - n * down_x;
		y = y - n * down_y;
	} else if (va == 1) {
		float n = 1.0f, hx = down_x * n, hy = down_y * n;
		x = x - hx * 0.5f;
		y = y - hy * 0.5f;
	}
	float fx = *(float *)(font + 0xe0) * xs, fy = *(float *)(font + 0xe4) * ys;
	float off = 0.0f;
	int ha = *halign;
	if (ha == 1) {
		float w = (float)text_width(font, line, 0.0f);
		w = w * xs;
		off = w * -0.5f;
	} else if (ha == 2) {
		float w = (float)text_width(font, line, 0.0f);
		off = -(w * xs);
	}
	unsigned colour = get_colour();
	float alpha = get_alpha();
	float dx = x + cs * off, dy = y + (-sn) * off;
	dx = dx + sn * 0.0f;
	dy = dy + cs * 0.0f;
	if (fault)
		dx = nextafterf(dx, INFINITY);
	draw_string(font, dx, dy, line, fx, fy, angle, colour, alpha);
	return 1;
}

static int same_capture(const Capture *a, const Capture *b) {
	if (a->overflow || b->overflow || a->n != b->n)
		return 0;
	for (int i = 0; i < a->n; i++) {
		const Submit *x = &a->s[i], *y = &b->s[i];
		if (x->prim != y->prim || x->tex != y->tex || x->stride != y->stride || x->count != y->count ||
				memcmp(x->bytes, y->bytes, x->stride * x->count))
			return 0;
	}
	return 1;
}

static void verify(const RV *args, void (*original)(void *), void *ctx) {
	static Capture native, gml;
	void *font_before = *thefont;
	memset(&native, 0, sizeof(native)); memset(&gml, 0, sizeof(gml));
	capturing = &native; capture_scratch = 1;
	int handled = glyph(args, letter_glyph_fault);
	capture_close(&native);
	void *font_native = *thefont;
	*thefont = font_before;
	capturing = &gml; capture_scratch = 0;
	original(ctx);
	capture_close(&gml);
	capturing = NULL;
	if (!handled) { letter_glyph_unverified++; return; }
	letter_glyph_verified++;
	letter_glyph_quads += gml.n;
	if (font_native != *thefont || !same_capture(&native, &gml)) {
		if (letter_glyph_mismatch++ < 8) {
			debugPrintf("[LETTERGLYPH] mismatch: %d/%d quads, font %p/%p, text kind %u\n", native.n, gml.n,
				font_native, *thefont, args[2].kind);
			for (int i = 0; i < native.n && i < gml.n; i++)
				for (int k = 0; k < native.s[i].stride * native.s[i].count && k < MAX_BYTES; k += 4)
					if (memcmp(native.s[i].bytes + k, gml.s[i].bytes + k, 4)) {
						uint32_t u, v; memcpy(&u, native.s[i].bytes + k, 4); memcpy(&v, gml.s[i].bytes + k, 4);
						debugPrintf("[LETTERGLYPH]   quad %d byte %d: %08x vs %08x\n", i, k, u, v);
						break;
					}
		}
	}
}

/* 1: drawn (natively, or by `original` in verify mode). 0: the caller runs the original. */
int letter_glyph_draw(const RV *args, void (*original)(void *), void *ctx) {
	if (!installed || !letter_glyph || letter_ab_off)
		return 0;
	if (letter_glyph_verify) {
		verify(args, original, ctx);
		return 1;
	}
	uint64_t t0 = letter_timing ? NOW() : 0;
	int r = glyph(args, 0);
	if (letter_timing && r) { letter_glyph_time[R_NATIVE] += NOW() - t0; letter_glyph_time_n[R_NATIVE]++; }
	if (r) letter_glyph_native++; else letter_glyph_fallback++;
	return r;
}

/* exp_lettertime around the original text call */
void letter_glyph_text_begin(void) { in_text = letter_timing; }
void letter_glyph_text_end(uint64_t us) {
	if (in_text) { letter_glyph_time[R_TEXT] += us; letter_glyph_time_n[R_TEXT]++; }
	in_text = 0;
}

void letter_glyph_report(int frames) {
	if (!installed || frames <= 0)
		return;
	if (letter_ab && letter_draw_window)
		debugPrintf("[LETTERAB] %.1f per frame letter Draws, lines %u filled %u hits | A/B: %s\n",
			(float)letter_draw_window / frames, letter_line_fills, letter_line_hits, letter_ab_off ? "off" : "on");
	letter_draw_window = letter_line_fills = letter_line_hits = 0;
	extern int letter_cache_verify;
	extern unsigned letter_cache_checked, letter_cache_mismatch;
	if (letter_cache_verify)
		debugPrintf("[LETTERCACHE] verify: %u cached lookups checked, %u mismatches (totals)\n", letter_cache_checked,
			letter_cache_mismatch);
	if (letter_glyph_native || letter_glyph_fallback)
		debugPrintf("[LETTERGLYPH] native %.1f fallback %.1f per frame (rejects len %u nl %u font %u sdf %u sprite %u)%s\n",
			(float)letter_glyph_native / frames, (float)letter_glyph_fallback / frames, letter_glyph_reject[0],
			letter_glyph_reject[1], letter_glyph_reject[2], letter_glyph_reject[3], letter_glyph_reject[4],
			letter_ab_off ? " (A/B: off)" : "");
	if (letter_glyph_verify)
		debugPrintf("[LETTERGLYPH] verify: %u calls compared (%u quads), %u mismatches, %u not handled (totals; rejects %u %u %u %u %u %u %u)%s\n",
			letter_glyph_verified, letter_glyph_quads, letter_glyph_mismatch, letter_glyph_unverified,
			letter_glyph_reject[0], letter_glyph_reject[1], letter_glyph_reject[2], letter_glyph_reject[3],
			letter_glyph_reject[4], letter_glyph_reject[5], letter_glyph_reject[6],
			letter_glyph_fault ? " | fault on" : "");
	if (letter_timing) {
		static const char *const names[NR] = { "split", "drawstring", "allocverts", "getglyph", "free", "flush",
			"text(orig)", "text(native)" };
		for (int i = 0; i < NR; i++)
			if (letter_glyph_time_n[i])
				debugPrintf("[LETTERTIME] %-12s %7.1f calls/frame %6.2f us each %6.3f ms/frame\n", names[i],
					(float)letter_glyph_time_n[i] / frames, (double)letter_glyph_time[i] / letter_glyph_time_n[i],
					letter_glyph_time[i] / 1000.0 / frames);
		memset(letter_glyph_time, 0, sizeof(letter_glyph_time));
		memset(letter_glyph_time_n, 0, sizeof(letter_glyph_time_n));
	}
	letter_glyph_native = letter_glyph_fallback = 0;
	memset(letter_glyph_reject, 0, sizeof(letter_glyph_reject));
	if (letter_ab)
		letter_ab_off = !letter_ab_off;
}

int letter_glyph_install(void *(*entry_hook)(uintptr_t entry, void *replacement)) {
	if (installed)
		return 1;
#define RESOLVE(v, t, n) do { v = (t)so_symbol(&yoyoloader_mod, n); if (!v) return 0; } while (0)
	uintptr_t f_text, gr_text;
	RESOLVE(f_text, uintptr_t, "_Z21F_DrawTextTransformedR6RValueP9CInstanceS2_iPS_");
	RESOLVE(gr_text, uintptr_t, "_Z24GR_Text_Draw_TransformedffPKciifff");
	/* the runner this was read from: both entries at their offsets, the same prologues */
	if (f_text < 0xddddf8 || gr_text - f_text != 0xea9e60 - 0xddddf8 || *(uint32_t *)f_text != 0xe92d4bf0 ||
			*(uint32_t *)gr_text != 0xe92d4ff0 || *(uint32_t *)(gr_text + 0x10) != 0xe24dd038)
		return 0;
	uintptr_t bias = f_text - 0xddddf8;
	/* the "M" literal GR_Text_Draw_Transformed passes to TextHeight (ldr r1, [pc, #0x358]; add r1, pc, r1) */
	uint32_t ldr = *(uint32_t *)(gr_text + 0xf0), add = *(uint32_t *)(gr_text + 0xf4);
	if (ldr != 0xe59f1358 || add != 0xe08f1001)
		return 0;
	letter_m = (const unsigned *)(gr_text + 0xf4 + 8 + *(uint32_t *)(gr_text + 0xf0 + 8 + 0x358));
	/* a char "M" passed as unsigned * (unaligned); TextHeight only reads the font's fields */
	if (((const char *)letter_m)[0] != 'M' || ((const char *)letter_m)[1] != 0)
		return 0;
	RESOLVE(get_string, getstr_fn, "_Z11YYGetStringPK6RValuei");
	RESOLVE(get_float, getfloat_fn, "_Z10YYGetFloatPK6RValuei");
	RESOLVE(utf8_length, strlen8_fn, "utf8_strlen");
	RESOLVE(utf8_char, extract_fn, "utf8_extract_char");
	RESOLVE(font_exists, int_int_fn, "_Z11Font_Existsi");
	RESOLVE(font_data, fontdata_fn, "_Z9Font_Datai");
	RESOLVE(text_height, height_fn, "_ZN7CFontGM10TextHeightEPj");
	RESOLVE(text_width, width_fn, "_ZN7CFontGM9TextWidthEPjf");
	RESOLVE(get_colour, colour_fn, "_Z17GR_Draw_Get_Colorv");
	RESOLVE(get_alpha, alpha_get_fn, "_Z17GR_Draw_Get_Alphav");
	RESOLVE(draw_string, dst_fn, "_ZN7CFontGM23Draw_String_TransformedEffPjfffjf");
	RESOLVE(fontid, int *, "_ZN13Graphics_Text6fontidE");
	RESOLVE(halign, int *, "_ZN13Graphics_Text6halignE");
	RESOLVE(valign, int *, "_ZN13Graphics_Text6valignE");
	RESOLVE(thefont, void **, "_ZN13Graphics_Text7thefontE");
	if (!(sincos_slot = so_got_slot(&yoyoloader_mod, "sincosf")) || !*sincos_slot)
		return 0;
	(void)bias;
	letter_glyph_verify = exp_flag("letterglyphverify");
	letter_glyph_fault = letter_glyph_verify && exp_flag("letterglyphverifyfault");
	int timing = exp_flag("lettertime"); // letter_timing itself is set later in the boot
	if (letter_glyph_verify || timing) {
		uintptr_t av;
		RESOLVE(av, uintptr_t, "_ZN8Graphics10AllocVertsE9ePrimTypePvii");
		if (!(orig_alloc_verts = entry_hook(av, alloc_verts_hook))) {
			letter_glyph_verify = 0;
			return 0;
		}
	}
	if (timing) {
		uintptr_t s, d, g, f, fl;
		RESOLVE(s, uintptr_t, "_Z15Split_TextBlockPKciP11TStringList");
		RESOLVE(g, uintptr_t, "_ZN7CFontGM8GetGlyphEib");
		RESOLVE(f, uintptr_t, "_ZN13MemoryManager4FreeEPKvb");
		RESOLVE(fl, uintptr_t, "_ZN8Graphics5FlushEv");
		d = (uintptr_t)draw_string;
		orig_split = entry_hook(s, split_timed);
		orig_get_glyph = entry_hook(g, glyph_timed);
		orig_free = entry_hook(f, free_timed);
		orig_flush = entry_hook(fl, flush_timed);
		orig_draw_string = entry_hook(d, draw_string_timed);
		debugPrintf("letter glyph timers: split %p drawstring %p getglyph %p free %p flush %p\n", orig_split,
			orig_draw_string, orig_get_glyph, orig_free, orig_flush);
	}
#undef RESOLVE
	installed = 1;
	return 1;
}
