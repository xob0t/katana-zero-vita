/*
 * dancer_glow.c
 *
 * Cheap dancer rim glow (dancerGlow=2, the default): the club dancers' coloured rim light, drawn by the loader with at
 * most one sprite per side instead of the game's two per side per light.
 *
 * The game's glow is obj_clubber user event 6, run by obj_drawactors for every dancer each frame (see
 * work/decomp/out/gml_Object_obj_clubber_Other_16.gml):
 *
 *   light_drawme = inside_screen(40) && global.character_lighting;
 *   for each light in close_light_array (obj_lightsource ids from update_light_array, refreshed every 60 frames):
 *     with (light) if (alpha > 0):
 *       d = point_distance(light, dancer.x, dancer.y - 32); if (d <= 200):
 *         dancer.rim_color = color; dancer.rim_dir = point_direction(light, dancer);
 *         dancer.rim_alpha = (1 - d / 200) * alpha * 4;
 *         with (dancer) if (light_drawme): for each side whose half-plane holds rim_dir (bottom 0..180, top 180..360,
 *           left 90..270, right 270..450): repeat (2) draw_sprite_ext(side_sprite, animation_index, x, y, dir * width,
 *           height, image_angle, rim_color, image_alpha * rim_alpha * (1 - |rim_dir - side| / 90));
 *
 * That is up to 4 outline draws per light per dancer, through two nested with iterators and RValue arithmetic: ~285
 * draws a frame in the busy club rooms, 44 fps against 52-56 with the event skipped (dancerGlow=0).
 *
 * Here the event is replaced (g_GMLFuncs entry) by a native that does the same light selection, distances, angles
 * and alphas, then composites the layers per side before drawing: draws of one sprite with the same transform under
 * normal blending stack as
 *     dst' = c_n a_n + (1 - a_n)(... (c_1 a_1 + (1 - a_1) dst))  =  C A + (1 - A) dst,
 *     A = 1 - prod(1 - a_i),  C = sum(c_i a_i prod_{j > i}(1 - a_j)) / A
 * which is exact for texels of alpha 0 or 1 (the outline sprites are pixel art), up to 8-bit rounding of the vertex
 * colour. So each side is one draw with the merged colour and alpha: at most 4 per visible dancer, 2 for a dancer
 * with one light. The only visible difference from the game: where two side outlines of one dancer overlap (the
 * corner pixels), the game paints them in light order and this paints them in side order.
 *
 * Nothing is written: the game's event sets rim_color, rim_dir, rim_alpha, dir_alpha and light_drawme on the
 * dancers, which only that event reads; this leaves them as dancerGlow=0 does (never set). No RNG, no instances,
 * no allocation (variables are read from the instances' hash maps without the runner's creating getters).
 * Anything unexpected (a missing variable, a non-numeric value, a different variable storage) draws no glow for that
 * dancer.
 *
 * Testing (debugMode): exp_dglowprof times the native ([DGLOW] line), exp_dglowab cycles the report windows through
 * no glow / cheap / the game's event, for in-run A/B/C costs; exp_dglowmax1 keeps one draw per light pair (the
 * strongest side only). exp_dglowcompare: the game's glow in place and, in the same frame, a copy of each dancer
 * 64 px lower with the cheap glow (side-by-side screenshots).
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

typedef void (*draw_ext_fn)(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
	int colour, float alpha);

enum { V_CLOSE_LIGHTS, V_CLOSE_ARRAY, V_BOTTOM, V_TOP, V_LEFT, V_RIGHT, V_ANIMATION, V_DIR, V_WIDTH, V_HEIGHT,
	V_ALPHA, V_COLOR, V_CHARACTER, V_XVIEW, V_YVIEW, V_WVIEW, V_HVIEW, NVARS };
static const char *const var_names[NVARS] = { "close_lights", "close_light_array", "bottom_sprite", "top_sprite",
	"left_sprite", "right_sprite", "animation_index", "dir", "width", "height", "alpha", "color",
	"character_lighting", "xview", "yview", "wview", "hview" };
static const uint32_t *vars[NVARS];

#define MAX_LIGHTS 32
#define RANGE 200.0

static event_func orig_event;
static draw_ext_fn draw_sprite_ext;
static const uint32_t *id_hash;
static uintptr_t getter_read, getter_write;
static event_func body_event;
static int compare;
static int prof, ab, ab_phase, max1;

static unsigned dg_calls, dg_visible, dg_pairs, dg_draws, dg_skips, dg_frames, dg_us;
static uint32_t dg_last_frame;
static unsigned skip_lines[8], skip_counts[8];

static uint32_t word(const void *p, size_t o) {
	uint32_t v;
	memcpy(&v, (const uint8_t *)p + o, 4);
	return v;
}

static float field(const void *p, size_t o) {
	float v;
	memcpy(&v, (const uint8_t *)p + o, 4);
	return v;
}

// An instance's (or the global object's) variable map: the runner's robin-hood hash of slot+1 -> RValue *.
// Same layout as clubber_native.c; only the standard vtable getters are trusted to mean that storage.
typedef struct {
	const uint8_t *entries;
	uint32_t mask;
} vtable_t;

static int var_table(const void *owner, vtable_t *t) {
	if (!owner)
		return 0;
	const void *vt = (const void *)(uintptr_t)word(owner, 0);
	if (!vt || word(vt, 8) != getter_read || word(vt, 12) != getter_write || word(owner, 4))
		return 0;
	const void *ht = (const void *)(uintptr_t)word(owner, 0x24);
	if (!ht)
		return 0;
	t->mask = word(ht, 8);
	t->entries = (const uint8_t *)(uintptr_t)word(ht, 16);
	return t->entries && t->mask <= 65535 && !(t->mask & (t->mask + 1));
}

static const gml_rvalue *peek(const vtable_t *t, int n) {
	uint32_t key = (vars[n][1] + 1) & 0x7fffffff, i = key & t->mask;
	if (!key)
		return NULL;
	for (uint32_t d = 0; d <= t->mask; ++d, i = (i + 1) & t->mask) {
		const uint8_t *e = t->entries + 12 * i;
		uint32_t k = word(e, 8);
		if (k == key)
			return (const gml_rvalue *)(uintptr_t)word(e, 0);
		if (!k || d > ((i - (k & t->mask)) & t->mask))
			return NULL;
	}
	return NULL;
}

static int num(const vtable_t *t, int n, double *out) {
	const gml_rvalue *v = peek(t, n);
	if (!v || (v->kind != 0 && v->kind != 13) || !isfinite(v->val))
		return 0;
	*out = v->val;
	return 1;
}

// Sprite ids are reals or typed sprite references (kind 15, type 0x01000001 in the payload's high word)
static int sprite_id(const vtable_t *t, int n, int *out) {
	const gml_rvalue *v = peek(t, n);
	if (!v)
		return 0;
	if (v->kind == 0 && isfinite(v->val) && v->val >= 0 && v->val < 1048576) {
		*out = (int)v->val;
		return 1;
	}
	if (v->kind == 15 && word(v, 4) == 0x01000001 && word(v, 0) < 1048576) {
		*out = (int)word(v, 0);
		return 1;
	}
	return 0;
}

static void *instance_find(uint32_t id) {
	const void *buckets = (const void *)(uintptr_t)id_hash[0];
	uint32_t mask = id_hash[1];
	if (!buckets || mask > 1048575 || (mask & (mask + 1)))
		return NULL;
	const void *node = (const void *)(uintptr_t)word(buckets, 8 * (id & mask));
	for (unsigned chain = 0; node && chain < 4096; ++chain, node = (const void *)(uintptr_t)word(node, 4))
		if (word(node, 8) == id)
			return (void *)(uintptr_t)word(node, 12);
	return NULL;
}

typedef struct {
	float r, g, b, t; // premultiplied colour (0..255) and transmission of the stacked layers
	int used;
} side_t;

static void stack(side_t *s, int colour, double a) {
	if (a <= 0)
		return;
	if (a > 1)
		a = 1;
	float A = (float)(1.0 - (1.0 - a) * (1.0 - a)); // the game draws each side twice
	float k = 1.0f - A;
	s->r = s->r * k + (float)(colour & 255) * A;
	s->g = s->g * k + (float)((colour >> 8) & 255) * A;
	s->b = s->b * k + (float)((colour >> 16) & 255) * A;
	s->t *= k;
	s->used = 1;
}


#define SKIP do { skip_line = __LINE__; goto skip; } while (0)

static void cheap_glow(void *self, float draw_dy) {
	int skip_line = 0;
	vtable_t self_t, global_t;
	if (!global_obj_ptr || !var_table(self, &self_t) || !var_table(*global_obj_ptr, &global_t))
		SKIP;
	// light_drawme = inside_screen(40) && global.character_lighting
	double xv, yv, wv, hv, character;
	if (!num(&global_t, V_XVIEW, &xv) || !num(&global_t, V_YVIEW, &yv) || !num(&global_t, V_WVIEW, &wv) ||
		!num(&global_t, V_HVIEW, &hv) || !num(&global_t, V_CHARACTER, &character))
		SKIP;
	double ax = field(self, 0xb8), ay = field(self, 0xbc);
	if (!(ax > xv - 40 && ax < xv + wv + 40 && ay > yv - 40 && ay < yv + hv + 40) || character == 0)
		return;
	dg_visible++;
	double count;
	if (!num(&self_t, V_CLOSE_LIGHTS, &count) || count < 0)
		SKIP;
	int n = (int)ceil(count);
	if (n > MAX_LIGHTS)
		n = MAX_LIGHTS;
	if (n <= 0)
		return;
	const gml_rvalue *arr = peek(&self_t, V_CLOSE_ARRAY);
	if (!arr || arr->kind != 2 || !word(arr, 0))
		SKIP;
	const void *header = (const void *)(uintptr_t)word(arr, 0);
	const gml_rvalue *items = (const gml_rvalue *)(uintptr_t)word(header, 4);
	if (!items || word(header, 0x1c) < (uint32_t)n)
		SKIP;

	side_t sides[4];
	memset(sides, 0, sizeof(sides));
	for (int s = 0; s < 4; s++)
		sides[s].t = 1;
	float image_alpha = field(self, 0xb0);
	for (int i = 0; i < n; i++) {
		const gml_rvalue *e = &items[i];
		uint32_t id;
		if (e->kind == 15 && word(e, 4) == 0x04000001)
			id = word(e, 0);
		else if (e->kind == 0 && e->val >= 100000 && e->val <= 2147483647.0)
			id = (uint32_t)e->val;
		else
			continue;
		void *light = instance_find(id);
		if (!light || light == self || (word(light, 0x7c) & 0x100007) != 4) // what a with() would skip
			continue;
		vtable_t lt;
		double alpha, colour;
		if (!var_table(light, &lt) || !num(&lt, V_ALPHA, &alpha) || !num(&lt, V_COLOR, &colour) || !(alpha > 0))
			continue;
		double lx = field(light, 0xb8), ly = field(light, 0xbc);
		double dx = ax - lx, dy = (ay - 32) - ly;
		double d = sqrt(dx * dx + dy * dy);
		if (!(d <= RANGE))
			continue;
		dg_pairs++;
		double rim_alpha = (1.0 - d / RANGE) * alpha * 4.0 * image_alpha;
		double dir = atan2(-(ay - ly), ax - lx) * (180.0 / M_PI);
		if (dir < 0)
			dir += 360.0;
		int c = (int)(int64_t)llround(colour);
		double w[4] = { 0, 0, 0, 0 }; // bottom, top, left, right
		if (dir > 0 && dir < 180)
			w[0] = 1.0 - fabs(dir - 90.0) / 90.0;
		if (dir > 180 && dir < 360)
			w[1] = 1.0 - fabs(dir - 270.0) / 90.0;
		if (dir > 90 && dir < 270)
			w[2] = 1.0 - fabs(dir - 180.0) / 90.0;
		double wrapped = dir < 180 ? dir + 360.0 : dir;
		if (wrapped > 270 && wrapped < 450)
			w[3] = 1.0 - fabs(wrapped - 360.0) / 90.0;
		if (max1) {
			int best = 0;
			for (int s = 1; s < 4; s++)
				if (w[s] > w[best])
					best = s;
			for (int s = 0; s < 4; s++)
				if (s != best)
					w[s] = 0;
		}
		for (int s = 0; s < 4; s++)
			if (w[s] > 0)
				stack(&sides[s], c, rim_alpha * w[s]);
	}

	int sprites[4];
	double sub, facing, width, height;
	if (!sprite_id(&self_t, V_BOTTOM, &sprites[0]) || !sprite_id(&self_t, V_TOP, &sprites[1]) ||
		!sprite_id(&self_t, V_LEFT, &sprites[2]) || !sprite_id(&self_t, V_RIGHT, &sprites[3]) ||
		!num(&self_t, V_ANIMATION, &sub) || !num(&self_t, V_DIR, &facing) || !num(&self_t, V_WIDTH, &width) ||
		!num(&self_t, V_HEIGHT, &height))
		SKIP;
	float x = field(self, 0xb8), y = field(self, 0xbc) + draw_dy, angle = field(self, 0xac);
	for (int s = 0; s < 4; s++) {
		side_t *sd = &sides[s];
		float A = 1.0f - sd->t;
		if (!sd->used || A < 1.0f / 512.0f)
			continue;
		int r = (int)(sd->r / A + 0.5f), g = (int)(sd->g / A + 0.5f), b = (int)(sd->b / A + 0.5f);
		r = r > 255 ? 255 : r;
		g = g > 255 ? 255 : g;
		b = b > 255 ? 255 : b;
		draw_sprite_ext(self, sprites[s], (float)sub, x, y, (float)(facing * width), (float)height, angle,
			r | g << 8 | b << 16, A);
		dg_draws++;
	}
	return;
skip:
	dg_skips++;
	for (int i = 0; i < 8; i++)
		if (skip_lines[i] == skip_line || !skip_lines[i]) {
			skip_lines[i] = skip_line;
			skip_counts[i]++;
			break;
		}
}

static void cheap_event(void *self, void *other) {
	uint32_t now = frame_count ? *frame_count : 0;
	if (now != dg_last_frame) {
		dg_last_frame = now;
		dg_frames++;
	}
	dg_calls++;
	int mode = ab ? ab_phase : 1;
	// exp_dglowab: report windows cycle none / cheap / the game's event
	if (mode == 0)
		return;
	if (mode == 2) {
		orig_event(self, other);
		return;
	}
	// exp_dglowcompare (screenshots): the game's glow in place, and a copy of the dancer 64 px lower with the
	// cheap glow, in the same frame
	if (compare && body_event) {
		orig_event(self, other);
		float y = field(self, 0xbc), up = y + 64.0f;
		memcpy((uint8_t *)self + 0xbc, &up, 4);
		body_event(self, other);
		memcpy((uint8_t *)self + 0xbc, &y, 4);
		cheap_glow(self, 64.0f);
		return;
	}
	uint64_t t0 = prof ? sceKernelGetProcessTimeWide() : 0;
	cheap_glow(self, 0);
	if (prof)
		dg_us += (unsigned)(sceKernelGetProcessTimeWide() - t0);
}

void dancer_glow_report(int frames) {
	if (!dg_calls)
		return;
	float f = dg_frames ? (float)dg_frames : 1.0f;
	char timing[48] = "";
	if (prof)
		snprintf(timing, sizeof(timing), ", native %.3f ms/frame", dg_us / 1000.0f / f);
	static const char *const phase[3] = { " | A/B: none", " | A/B: cheap", " | A/B: full" };
	debugPrintf("[DGLOW] cheap: %.1f dancers/frame, %.1f visible, %.1f light pairs, %.1f draws/frame, %u skipped%s%s\n",
		dg_calls / f, dg_visible / f, dg_pairs / f, dg_draws / f, dg_skips, timing, ab ? phase[ab_phase] : "");
	for (int i = 0; i < 8 && skip_lines[i]; i++)
		debugPrintf("[DGLOW] skipped at dancer_glow.c:%u x%u\n", skip_lines[i], skip_counts[i]);
	memset(skip_lines, 0, sizeof(skip_lines));
	memset(skip_counts, 0, sizeof(skip_counts));
	if (ab)
		ab_phase = (ab_phase + 1) % 3;
	dg_calls = dg_visible = dg_pairs = dg_draws = dg_skips = dg_frames = dg_us = 0;
	(void)frames;
}

// Returns 1 if installed; the caller turns the glow off otherwise
int dancer_glow_install(gml_func_entry *table) {
	draw_sprite_ext = (draw_ext_fn)so_symbol(&yoyoloader_mod, "_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
	id_hash = (const uint32_t *)so_symbol(&yoyoloader_mod, "_ZN9CInstance14ms_ID2InstanceE");
	getter_read = so_symbol(&yoyoloader_mod, "_ZN12YYObjectBase19InternalGetYYVarRefEi");
	getter_write = so_symbol(&yoyoloader_mod, "_ZN12YYObjectBase20InternalGetYYVarRefLEi");
	int ok = draw_sprite_ext && id_hash && getter_read && getter_write && global_obj_ptr;
	for (int i = 0; ok && i < NVARS; i++) {
		char name[64];
		snprintf(name, sizeof(name), "g_VAR_%s", var_names[i]);
		ok = (vars[i] = (const uint32_t *)so_symbol(&yoyoloader_mod, name)) != NULL;
	}
	if (!ok || !(orig_event = gml_replace(table, "gml_Object_obj_clubber_Other_16", cheap_event))) {
		debugPrintf("cheap dancer glow: symbols missing\n");
		return 0;
	}
	prof = exp_flag("dglowprof");
	ab = exp_flag("dglowab");
	max1 = exp_flag("dglowmax1");
	compare = debugMode && exp_flag("dglowcompare");
	body_event = (event_func)so_symbol(&yoyoloader_mod, "_Z31gml_Object_obj_clubber_Other_25P9CInstanceS0_");
	debugPrintf("cheap dancer glow: installed%s\n", max1 ? " (one side per light)" : "");
	return 1;
}
