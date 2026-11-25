/*
 * pause_bg.c
 *
 * While the game is paused, the room's tile layers are still drawn every frame (the paused room's instances are
 * deactivated, its layers are not), and then covered. obj_gamepaused Draw (APK 1.1.10) starts with:
 *
 *   draw_set_alpha(1);
 *   if (surface_exists(screencopy)) {
 *       draw_rect_col(vx - 2, vy - 2, vx + vw + 2, vy + vh + 2, c_black, false);   // a stretched white pixel
 *       repeat (10) draw_surface_ext(screencopy, vx, vy, 1, 1, 0, c_white, 1);      // the paused room picture
 *       ...
 *   } else
 *       draw_rect_col(vx, vy, vx + vw, vy + vh, merge_color(c_purple, c_blue, 0.9), false);
 *
 * The rectangle is opaque (alpha 1, normal blending) and covers the view, i.e. the whole application surface: every
 * pixel the tile layers drew is overwritten before anything reads it. On club_4_2 those six tilemap layers cost about
 * 1.5 ms of CPU and 2 ms of GPU per paused frame (they sample room texture pages, one of them outside VRAM).
 *
 * A tilemap element draw (DrawLayerTilemapElement) is skipped when:
 * - obj_gamepaused drew that covering rectangle in the previous frame (its Draw event runs every frame while paused),
 *   with the runner's default render state at that moment (normal blending, all colour channels, no shader, alpha
 *   test, fog or z test, identity world matrix: virtual_capture.c's list from RenderStateManager::Reset), at alpha 1,
 *   filled, into the application surface, containing view 0's camera view (mapped onto the whole surface);
 * - this frame's rectangle hasn't been drawn yet (a layer drawn after it would be visible), and the current target is
 *   the application surface.
 * The frame's own rectangle is checked the same way when it is drawn; a failed check stops the skipping until the
 * next frame with a good one. Nothing in a paused frame reads the application surface between the tile layers and
 * the rectangle: only the pause objects and the persistent masters are active (obj_gamepaused Create deactivates the
 * rest).
 *
 * exp_pausetilesverify (debug): every 60th covered frame draws the tile layers and reads the application surface back
 * (glReadPixels) right after the rectangle: every pixel must be the rectangle's colour at alpha 255, i.e. nothing of
 * the tile layers is left. exp_pausetiles0: tile layers drawn as before. exp_pausetilesab: A/B by report window.
 *
 * Texture page eviction (page_evict.c) unloads pages that weren't drawn for 2 s: the skipped layers' pages stay
 * protected for the whole pause (as when they were drawn every frame), so resuming doesn't reload them.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "hooks.h"
#include "katana_internal.h"

#define SYM(name) so_symbol(&yoyoloader_mod, name)

typedef gml_rvalue RV;
typedef RV *(*script_fn)(void *self, void *other, RV *result, int argc, RV **args);
typedef void (*tilemap_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*void_fn)(void);
typedef int (*surf_int_fn)(int id);

#define KIND(v) ((v)->kind & 0xffffff)

static script_fn orig_rect_col;
static event_func orig_gp_draw;
static tilemap_fn orig_tilemap;
static builtin_fn bi_draw_get_alpha;
static void_fn graphics_flush;
static surf_int_fn surf_width, surf_height;

static const uint8_t *render_states;
static const float *matrices;
static void **active_shader, **next_shader;
static int *native_shader, *next_native_shader, *texture_stage, *surface_stack, *current_targets, *app_surface;
static uint8_t *mark_vertices;
extern uint32_t vgl_framecount;

static int installed, ab_mode, verify;
int pause_tiles_ab_off; // exp_pausetilesab: this report window draws the tile layers

static void *gp_self;
static int in_gp, gp_first_rect;
static uint32_t cover_frame = 0xffffffff; // last frame whose rectangle covered the application surface
static uint32_t frame_rect = 0xffffffff;  // last frame whose rectangle was drawn (good or not)
uint32_t pause_tiles_since;               // vgl_framecount when the skipping started (0: not skipping)
static uint32_t last_skip_vgl;

static unsigned n_skipped, n_drawn, n_cover, n_nocover, verify_frames, verify_bad, verify_bad_pixels;
static unsigned reason[8];
static uint32_t verify_tick;
static int verify_now; // this frame: tiles drawn, read back after the rectangle

// Default render state (RenderStateManager::Reset in APK 1.1.10; virtual_capture.c's list)
static const uint32_t normal_states[38] = {
	0,0,3,2,1,0,5,6,0,4,0,5,1,0,0,0,1,0,0x3f800000,
	0,0,0,0,3,15,0,1,1,1,8,0,0xffffffff,0xffffffff,0,5,6,0,0
};

static uint32_t word(const void *p, unsigned off) {
	return *(const uint32_t *)((const uint8_t *)p + off);
}

int pause_bg_state_ok(void);
static int state_ok(void) {
	if (*active_shader || *next_shader || *native_shader != -1 || *next_native_shader != -1 || *texture_stage != 0 ||
			*mark_vertices)
		return 0;
	for (int i = 0; i < 38; i++)
		if (word(render_states, 0x210 + i * 4) != normal_states[i])
			return 0;
	for (int i = 0; i < 16; i++)
		if (matrices[32 + i] != (i % 5 == 0 ? 1.0f : 0.0f))
			return 0;
	return 1;
}

int pause_bg_state_ok(void) {
	return state_ok();
}

static int fail(int r) {
	reason[r]++;
	return 0;
}

// --- verify: the application surface right after the rectangle ---
#define BAND 48
#define MAX_W 1280
static uint32_t band[MAX_W * BAND];

static void verify_rect(uint32_t expect) {
	int w = surf_width(*app_surface), h = surf_height(*app_surface);
	if (w <= 0 || h <= 0 || w > MAX_W)
		return;
	graphics_flush();
	unsigned bad = 0;
	for (int y0 = 0; y0 < h; y0 += BAND) {
		int n = h - y0 < BAND ? h - y0 : BAND;
		glReadPixels(0, y0, w, n, GL_RGBA, GL_UNSIGNED_BYTE, band);
		for (int i = 0; i < w * n; i++)
			bad += band[i] != expect;
	}
	verify_frames++;
	if (bad) {
		verify_bad++;
		verify_bad_pixels += bad;
	}
}

// view_get_camera(0)'s view x, y, width, height (the event's view_current is 0: Katana draws one view)
static builtin_fn bi_view_get_camera, bi_view_x, bi_view_y, bi_view_w, bi_view_h;
int pause_bg_camera_view(void *self, void *other, double *out);
static int camera_view(void *self, void *other, double *out) {
	RV cam = {0, 0, 5}, zero = {0, 0, 0};
	bi_view_get_camera(&cam, self, other, 1, &zero);
	if (KIND(&cam) != 0 && KIND(&cam) != 15)
		return 0;
	builtin_fn fns[4] = {bi_view_x, bi_view_y, bi_view_w, bi_view_h};
	for (int i = 0; i < 4; i++) {
		RV r = {0, 0, 5};
		fns[i](&r, self, other, 1, &cam);
		if (KIND(&r) != 0 || !isfinite(r.val))
			return 0;
		out[i] = r.val;
	}
	return out[2] > 0 && out[3] > 0;
}

int pause_bg_camera_view(void *self, void *other, double *out) {
	return camera_view(self, other, out);
}

int pause_bg_surface_size(int id, int *w, int *h) {
	if (!surf_width)
		return 0;
	*w = surf_width(id);
	*h = surf_height(id);
	return *w > 0 && *h > 0;
}

// pause_stack.c: obj_gamepaused's Draw is running for self, and the frame and colour of its last covering rectangle
uint32_t pause_bg_rect_frame = 0xffffffff, pause_bg_rect_rgba;
int pause_bg_in_gp(void *self) {
	return in_gp && self == gp_self;
}

// The rectangle: draw_rect_col(x1, y1, x2, y2, colour, outline) as obj_gamepaused's first draw
static int cover_ok(void *self, void *other, int argc, RV **args, uint32_t *rgba) {
	if (argc != 6 || !args)
		return fail(1);
	for (int i = 0; i < 6; i++)
		if (!args[i] || (KIND(args[i]) != 0 && !(i == 5 && KIND(args[i]) == 13)) || !isfinite(args[i]->val))
			return fail(1);
	if (args[5]->val != 0.0 || args[4]->val < 0 || args[4]->val > 0xffffff || args[4]->val != floor(args[4]->val))
		return fail(2);
	RV a = {0, 0, 5};
	bi_draw_get_alpha(&a, self, other, 0, NULL);
	if (KIND(&a) != 0 || a.val != 1.0)
		return fail(3);
	if (current_targets[0] != *app_surface) {
		static int logged;
		if (!logged++)
			debugPrintf("[PAUSETILES] rectangle not drawn into the application surface: target %d (stack %d), application surface %d\n",
				current_targets[0], *surface_stack, *app_surface);
		return fail(4);
	}
	if (!state_ok())
		return fail(7);
	// The rectangle must contain view 0's camera view, which the runner maps onto the whole application surface
	double view[4];
	if (!camera_view(self, other, view))
		return fail(5);
	if (args[0]->val > view[0] || args[1]->val > view[1] || args[2]->val < view[0] + view[2] ||
			args[3]->val < view[1] + view[3])
		return fail(6);
	const uint32_t c = (uint32_t)args[4]->val; // GameMaker colour: 0xBBGGRR
	*rgba = (c & 0xff) | (c & 0xff00) | (c & 0xff0000) | 0xff000000u; // RGBA bytes in memory: R, G, B, A
	return 1;
}

static RV *rect_col(void *self, void *other, RV *result, int argc, RV **args) {
	if (!in_gp || self != gp_self || !gp_first_rect)
		return orig_rect_col(self, other, result, argc, args);
	gp_first_rect = 0; // only the event's first draw: nothing of the event is drawn before it
	const uint32_t f = *frame_count;
	frame_rect = f;
	uint32_t rgba = 0;
	int ok = cover_ok(self, other, argc, args, &rgba);
	RV *r = orig_rect_col(self, other, result, argc, args);
	if (ok) {
		cover_frame = f;
		pause_bg_rect_frame = f;
		pause_bg_rect_rgba = rgba;
		n_cover++;
		if (verify_now)
			verify_rect(rgba);
	} else
		n_nocover++;
	verify_now = 0;
	return r;
}

static void gp_draw(void *self, void *other) {
	gp_self = self;
	in_gp = 1;
	gp_first_rect = 1;
	orig_gp_draw(self, other);
	in_gp = 0;
}

static void tilemap_draw(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
	const uint32_t frame = *frame_count;
	int skip = installed && !(ab_mode && pause_tiles_ab_off) && cover_frame + 1 == frame && frame_rect != frame &&
		current_targets[0] == *app_surface;
	if (skip && verify && (verify_now || verify_tick++ % 60 == 30)) {
		verify_now = 1;
		skip = 0;
	}
	if (skip) {
		if (!pause_tiles_since || vgl_framecount - last_skip_vgl > 1)
			pause_tiles_since = vgl_framecount ? vgl_framecount : 1;
		last_skip_vgl = vgl_framecount;
		n_skipped++;
		return;
	}
	n_drawn++;
	orig_tilemap(a, b, c, d, e, f);
}

// page_evict.c: a page last used at `use` stays resident while the tile layers are skipped (it was in use when the
// skipping started, or later)
int pause_tiles_protects(uint32_t use) {
	if (!pause_tiles_since)
		return 0;
	if (vgl_framecount - last_skip_vgl > 1) { // the skipping ended
		pause_tiles_since = 0;
		return 0;
	}
	return (int32_t)(use - (pause_tiles_since - 120)) >= 0;
}

void pause_bg_report(int frames) {
	if (!installed)
		return;
	if (n_skipped || n_cover || n_nocover)
		debugPrintf("[PAUSETILES] tile layers skipped %.1f drawn %.1f per frame | covered frames %u, not %u%s | reasons %u %u %u %u %u %u %u\n",
			(float)n_skipped / frames, (float)n_drawn / frames, n_cover, n_nocover,
			ab_mode && pause_tiles_ab_off ? " (A/B: drawn this window)" : "", reason[1], reason[2], reason[3],
			reason[4], reason[5], reason[6], reason[7]);
	if (verify && verify_frames)
		debugPrintf("[PAUSETILES] verify: %u frames read back after the rectangle, %u not uniform (%u pixels)\n",
			verify_frames, verify_bad, verify_bad_pixels);
	n_skipped = n_drawn = n_cover = n_nocover = 0;
	if (ab_mode)
		pause_tiles_ab_off = !pause_tiles_ab_off;
}

// patch_katana: entry hooks must be installed before the runner starts (a DrawLayerTilemapElement entry hook
// installed from katana_late_install was never called)
void pause_bg_install(gml_func_entry *table) {
	if (exp_flag("pausetiles0")) {
		debugPrintf("paused room tile layers: drawn (exp_pausetiles0)\n");
		return;
	}
	uintptr_t rect_fn = SYM("_Z24gml_Script_draw_rect_colP9CInstanceS0_R8YYRValueiPPS1_");
	uintptr_t tile_fn = SYM("_Z23DrawLayerTilemapElementP9tagYYRECTP6CLayerP20CLayerTilemapElementfff");
	graphics_flush = (void_fn)SYM("_ZN8Graphics5FlushEv");
	surf_width = (surf_int_fn)SYM("_Z20GR_Surface_Get_Widthi");
	surf_height = (surf_int_fn)SYM("_Z21GR_Surface_Get_Heighti");
	render_states = (const uint8_t *)SYM("g_States");
	matrices = (const float *)SYM("m_CurrentMatrix");
	active_shader = (void **)SYM("g_ActiveUserShader");
	next_shader = (void **)SYM("g_NextActiveUserShader");
	native_shader = (int *)SYM("g_ActiveUserShaderM");
	next_native_shader = (int *)SYM("g_NextActiveUserShaderM");
	texture_stage = (int *)SYM("g_DefaultTextureStage");
	mark_vertices = (uint8_t *)SYM("g_MarkVerts");
	surface_stack = (int *)SYM("StackSP");
	current_targets = (int *)SYM("currenttargets");
	app_surface = (int *)SYM("g_ApplicationSurface");
	if (!rect_fn || !tile_fn || !graphics_flush || !surf_width || !surf_height || !render_states || !matrices ||
			!active_shader || !next_shader || !native_shader || !next_native_shader || !texture_stage ||
			!mark_vertices || !surface_stack || !current_targets || !app_surface || !frame_count) {
		debugPrintf("paused room tile layers: NOT skipped (runner symbols)\n");
		return;
	}
	if (!(orig_gp_draw = gml_replace(table, "gml_Object_obj_gamepaused_Draw_0", gp_draw))) {
		debugPrintf("paused room tile layers: NOT skipped (event)\n");
		return;
	}
	if (!(orig_rect_col = entry_hook(rect_fn, rect_col)) || !(orig_tilemap = entry_hook(tile_fn, tilemap_draw))) {
		// with the rectangle hook alone nothing is skipped (cover_frame is set only when both are in)
		debugPrintf("paused room tile layers: NOT skipped (hooks)\n");
		return;
	}
}

// katana_late_install: the runner's function registry is filled by then
void pause_bg_late_install(void) {
	if (!orig_tilemap || !orig_rect_col)
		return;
	runner_function **functions = (runner_function **)SYM("the_functions");
	int *count = (int *)SYM("the_numb");
	for (int i = 0; functions && *functions && count && i < *count; i++) {
		runner_function *f = &(*functions)[i];
		if (!f->name)
			continue;
		if (!strcmp(f->name, "draw_get_alpha")) bi_draw_get_alpha = f->func;
		else if (!strcmp(f->name, "view_get_camera")) bi_view_get_camera = f->func;
		else if (!strcmp(f->name, "camera_get_view_x")) bi_view_x = f->func;
		else if (!strcmp(f->name, "camera_get_view_y")) bi_view_y = f->func;
		else if (!strcmp(f->name, "camera_get_view_width")) bi_view_w = f->func;
		else if (!strcmp(f->name, "camera_get_view_height")) bi_view_h = f->func;
	}
	if (!bi_draw_get_alpha || !bi_view_get_camera || !bi_view_x || !bi_view_y || !bi_view_w || !bi_view_h) {
		debugPrintf("paused room tile layers: NOT skipped (builtins)\n");
		return;
	}
	verify = debugMode && exp_flag("pausetilesverify");
	ab_mode = debugMode && exp_flag("pausetilesab");
	installed = 1;
	debugPrintf("paused room tile layers: skipped under the pause background%s%s\n", verify ? " (verify)" : "",
		ab_mode ? ", A/B by report window" : "");
}
