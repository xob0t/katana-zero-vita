/*
 * katana_display.c
 *
 * Application surface size, lighting at 30 Hz, texture page recovery, dynamic resolution, the
 * per-frame after-swap hook and screen copies of the resized application surface.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <kubridge.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "lz4/lz4.h"
#include "dialog.h"

#include "katana_internal.h"
#include "hang_watch.h"

/*
 * Lighting at 30 Hz: global.recolor_timer is a frame counter, and every lit sprite
 * recomputes its lighting (draw_sprite_recolored) whenever it changes. On odd frames the
 * timer is put back to the previous frame's value before drawing, so lit sprites reuse
 * their cached light colour and lighting refreshes at 30 Hz.
 */
static int lighting30;
static double lighting30_prev;

// The application surface is rendered at the output size instead of the 1280x720 view port size: 960x540 at
// renderRes=960 (1:1 with the screen), the game's native 640x360 at lower render resolutions (a quarter of the
// pixels and VRAM, same image at a 640x368 output; factory2_4: 36.8 -> 39.9 fps, VRAM use 75% -> 57%). Text
// is drawn at half scale for the 1280x720 surface: at 640x360 every other pixel column of a letter is lost
// (960x540 costs 0-1 fps over 640x360 in heavy rooms). The size is requested the way surface_resize on
// application_surface does it (g_NewApplicationSize, applied at the start of Draw_Room): Draw_Room only creates
// the surface when there is none, so writing g_ApplicationWidth/Height alone kept the old surface and drew the
// smaller frame into its corner (a dynamic resolution drop showed the game shrunk). The size the game asked for is
// kept for screen copies (surface_copy_scaled). exp_appsurf1280 keeps the original size, exp_appsurf640 forces
// 640x360.
static int *app_width, *app_height, *app_new_width, *app_new_height;
static char *app_new_size;
static int app_surf_native, app_surf_w = 640, app_surf_h = 360;
static int app_full_w = 640, app_full_h = 360; // size outside GPU-bound rooms (see dynamic_res_update)
static int app_set_w, app_set_h; // size last written by us
static int app_game_w, app_game_h; // size the game set, before the override

/*
 * Drain after the game's own texture page flush. Many rooms call draw_texture_flush at room start, and the room
 * switch and the new room's first frame run in the same Process call. vitaGL keeps the released pages for a few frames
 * (garbage collector purge lists), so the first frame's page loads didn't fit, and the allocator fell back to its forced
 * collection: 34 of the 35 forced collections in a 29-minute playtest were on such frames. The forced collection's
 * last cycle frees what the current frame released, which a scene still being recorded may use; a hang there (an fbo
 * deleted right after drawing to it, its render target destroyed under its open scene) froze the game for good on a
 * device test, the same way the playtest froze entering room_prison_1 right after the game's flush.
 * So the released memory is freed (vgl_collect_garbage_now: GPU finish, every purge cycle) right after the flush
 * returns, once no scene holds unsubmitted commands, so nothing still to run refers to it. It has to be right there:
 * the flush mostly runs inside a Step event (obj_level_complete, room start), and the new room's pages start loading
 * before its Draw phase. On device soaks a drain at the start of Draw came too late for 17 of 21 game flushes (a new
 * page already in PHYCONT), and waiting for the swap was later still (the room switch and the first frame follow in
 * the same Process call). At the call a scene is usually open: the builtin first submits the runner's pending batch
 * (Graphics::Flush) into it. It is ended first (vgl_end_scene_no_depth_stencil) when no draw or clear in it wrote
 * depth or stencil values and both tests are off: the next draw begins a new scene and redraws the scissor mask, and
 * the split drops no depth/stencil contents. Otherwise the drain retries at the start of Draw and after the next
 * presented frame; each wait is logged with its reason.
 * The new room then loads its pages into free VRAM, and the forced collection stays a rare last resort, reported by
 * [FORCEDGC]. The loader's own page recovery after the swap (below) drains the same way. exp_texdrain0 disables.
 */
extern GLboolean vgl_scene_open;
static builtin_fn orig_texture_flush;
static const char *texdrain_pending; // what released the memory, until the drain runs
unsigned texdrain_count;

static void texdrain_run(const char *where) {
	if (!texdrain_pending)
		return;
	extern int vgl_end_scene_no_depth_stencil(void);
	int scene = vgl_end_scene_no_depth_stencil();
	if (scene == 2) {
		extern GLboolean vgl_scene_depth_stencil, depth_test_state, stencil_test_state;
		static const char *const causes[] = { "", "a draw with depth writes", "a draw with the stencil test",
			"a depth clear", "a stencil clear" };
		debugPrintf("[TEXDRAIN] waiting (%s): the open scene %s%s\n", where, vgl_scene_depth_stencil ?
			"wrote depth or stencil values: " : depth_test_state || stencil_test_state ? "has a depth or stencil test on" :
			"could not be ended", vgl_scene_depth_stencil <= 4 ? causes[vgl_scene_depth_stencil] : "");
		return;
	}
	const char *what = texdrain_pending;
	texdrain_pending = NULL;
	uint32_t t0 = sceKernelGetProcessTimeLow();
	size_t before = vglMemFree(VGL_MEM_VRAM);
	extern void vgl_collect_garbage_now(void);
	vgl_collect_garbage_now();
	texdrain_count++;
	debugPrintf("[TEXDRAIN] freed after %s (%s%s, %.1f ms): free vram %uKB -> %uKB, phycont %uKB\n", what, where,
		scene == 1 ? ", ended an empty scene" : scene == 3 ? ", ended the open scene" : "",
		(sceKernelGetProcessTimeLow() - t0) / 1000.0f, before / 1024, vglMemFree(VGL_MEM_VRAM) / 1024,
		vglMemFree(VGL_MEM_PHYCONT) / 1024);
}

// For title_pages.c: frees what was just released, the same way (scene-aware). Returns 1 if it was freed now, 0 if
// it waits for the start of Draw or the next presented frame.
int katana_texdrain_now(const char *what) {
	texdrain_pending = what;
	texdrain_run("after the swap");
	return !texdrain_pending;
}

static void texture_flush_drain(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	orig_texture_flush(result, self, other, argc, args);
	extern void page_restore_cancel(void);
	page_restore_cancel(); // pages the loader's own recovery flush evicted stay unloaded after the game's flush
	texdrain_pending = "the game's texture flush";
	texdrain_run("at the call");
}

// Forced collections since the last presented frame: rare once the drain runs, so each one is logged
static void forced_gc_report(void) {
	extern volatile int vgl_forced_gc_runs, vgl_forced_gc_open_scene;
	extern volatile size_t vgl_forced_gc_size;
	// vitagl.patch: begun off the GL thread (no glFinish, no retire drain); stopped before the cycle that frees the
	// current frame's purge slot (a scene with commands open, or off the GL thread)
	extern uint32_t vgl_forced_gc_off_thread, vgl_forced_gc_capped;
	static int seen, seen_open;
	static uint32_t seen_off, seen_capped;
	if (vgl_forced_gc_runs == seen)
		return;
	debugPrintf("[FORCEDGC] %d forced garbage collection(s) in the last frame, %d with a scene open, %u off the GL thread, "
		"%u kept the current frame's slot, last for %uKB | room %d | free vram %uKB ram %uKB phycont %uKB\n",
		vgl_forced_gc_runs - seen, vgl_forced_gc_open_scene - seen_open, vgl_forced_gc_off_thread - seen_off,
		vgl_forced_gc_capped - seen_capped, (unsigned)(vgl_forced_gc_size / 1024), current_room ? *current_room : -1,
		vglMemFree(VGL_MEM_VRAM) / 1024, vglMemFree(VGL_MEM_RAM) / 1024, vglMemFree(VGL_MEM_PHYCONT) / 1024);
	seen = vgl_forced_gc_runs;
	seen_open = vgl_forced_gc_open_scene;
	seen_off = vgl_forced_gc_off_thread;
	seen_capped = vgl_forced_gc_capped;
}

// Surface memory is freed once the GPU has finished the scenes that may use it (vitagl.patch: vgl_retire, the scene
// completion fence); totals in debug mode when they change, at most every 2 s
static void rt_free_report(void) {
	extern uint32_t vgl_rt_retired_now, vgl_rt_retired_later, vgl_open_scene_retires, vgl_rt_handle_retires,
		vgl_retire_overflows;
	extern int vgl_retire_pending(void);
	static uint32_t seen, frames;
	frames++;
	uint32_t sum = vgl_rt_retired_now + vgl_rt_retired_later + vgl_open_scene_retires + vgl_rt_handle_retires +
		vgl_retire_overflows;
	if (!debugMode || frames < 120 || sum == seen)
		return;
	frames = 0;
	seen = sum;
	debugPrintf("[RTFREE] render-target frees: %u at once (GPU done), %u after their scene fence | open-scene texture frees "
		"held back %u | render targets retired %u | not queued (no memory) %u | waiting now %d\n", vgl_rt_retired_now,
		vgl_rt_retired_later, vgl_open_scene_retires, vgl_rt_handle_retires, vgl_retire_overflows, vgl_retire_pending());
}

// DoAStep_Draw hook: application surface size, then lighting at 30 Hz
static void (*katana_orig_draw)(void);

static void DoAStep_Draw_katana(void) {
	texdrain_run("at Draw");
	if (app_surf_native && app_width && !*app_new_size && (*app_width != app_surf_w || *app_height != app_surf_h)) {
		debugPrintf("app surface %dx%d -> %dx%d\n", *app_width, *app_height, app_surf_w, app_surf_h);
		if (*app_width != app_set_w || *app_height != app_set_h) {
			app_game_w = *app_width;
			app_game_h = *app_height;
		}
		// Also the size Draw_Room creates the surface at if there is none yet
		*app_width = *app_new_width = app_set_w = app_surf_w;
		*app_height = *app_new_height = app_set_h = app_surf_h;
		*app_new_size = 1;
	}
	if (lighting30) {
		gml_rvalue *(*vwrite)(void *, int) = (*(void ***)*global_obj_ptr)[3];
		gml_rvalue *timer = vwrite(*global_obj_ptr, var_recolor_timer[1]);
		if (timer && timer->kind == 0) {
			if (*frame_count & 1)
				timer->val = lighting30_prev;
			else
				lighting30_prev = timer->val;
		}
	}
	katana_orig_draw();
}

/*
 * Texture page recovery. The runner loads 16 MB texture pages as rooms need them and never unloads them, so
 * over a session VRAM fills with pages of earlier rooms:
 * - A new page no longer fits anywhere and its sprites draw black (room_psychiatrist after the first level
 *   showed only the player and the lights).
 * - More often, vitaGL silently places the new page (or a surface) in main RAM, which the GPU reads far more
 *   slowly: room_club_7 became GPU-bound at 33 fps (swap ~11 ms per frame) instead of 52-59. Forcing large
 *   GPU allocations into RAM reproduced exactly that.
 * In both cases all texture pages are flushed right after that frame is presented (draw_texture_flush; render
 * targets are kept). The runner reloads each page on first use; the flushed pages are freed right away (GPU finish
 * + garbage collection), so the current room's pages reload into VRAM. After a failed allocation this runs at most
 * every 5 s and three times per room; after an allocation outside VRAM at most once per room, so a room whose
 * pages really don't fit doesn't thrash. Costs the reload time (~1 s at a room load). exp_texflush0 disables.
 * Since page eviction (page_evict.c) both are rarer: before an allocation leaves VRAM, the pages not drawn for 2 s
 * are unloaded until it fits, so this flush runs only when that wasn't enough, and only for a texture page outside
 * VRAM (a surface outside VRAM isn't flushed for).
 * Both of those react after a page missed VRAM, which can be too late: after ~5 minutes of level hopping VRAM had
 * 11 MB free (6 MB largest) in room_folder, main RAM 6.6 MB, and loading room_club_0's pages crashed in the
 * allocator. So pages are also flushed ahead of need at a room change: when the frame just presented asked for
 * another room (New_Room, applied at the start of the next frame) and free VRAM is under TEXFLUSH_LOW_VRAM, the new
 * room loads its pages into an emptied VRAM, during the transition. Never mid-room: room_chinatown_3 needs more
 * than VRAM, and a mid-room flush there froze play for 1.4 s and filled VRAM again right after.
 * Note: the runner switches rooms inside Process, so New_Room is usually back to -1 by the time this runs; it never
 * fired in a 29-minute playtest (157 room changes). The drain after the game's own flush (texdrain above) is what
 * frees memory at room changes now.
 */
#define TEXFLUSH_MIN_INTERVAL_US 5000000
#define TEXFLUSH_FAILURE_FLUSHES 3
#define TEXFLUSH_SPILL_BYTES (1024 * 1024)
#define TEXFLUSH_LOW_VRAM (48 * 1024 * 1024)  // room for three 16 MB texture pages
static builtin_fn texture_flush;
static int texflush_failures_seen, texflush_spill_room = -2;
static size_t texflush_spill_seen;
static uint32_t texflush_last_us;
unsigned texflush_count;

/*
 * Dynamic resolution (dynamicRes=1; off by default): a 960x540 application surface keeps text sharp, but rooms full of
 * blended light sprites drawn straight onto it (room_cathedral: window beams, ~65 light glows) can run out of
 * GPU fill rate: 43-52 fps with 4-9 ms GPU waits at the swap, 59 at 640x360. A frame counts as GPU-bound when
 * it missed a vblank (over DYNRES_MISS_US since the previous swap) although its CPU work fit in a frame (under
 * DYNRES_CPU_US) and it waited over DYNRES_SWAP_US at the swap. The swap time alone can't tell: since the runner's
 * frame limiter no longer spins (timing_sleep_vsync), an idle frame's slack is spent waiting for vblank in the swap,
 * ~7 ms in a light 60 fps room, and every room dropped. When at least DYNRES_BOUND_FRAMES of DYNRES_WINDOW frames
 * are GPU-bound (under ~55 fps from the GPU) the room drops to 640x360 until the next room. The first second of a
 * room (texture loads stall the swap too) is not counted. (Texture pages pushed out of VRAM into RAM make ordinary
 * rooms GPU-bound and drop too.)
 * Off by default since Nov 24: with the cathedral dust off (cathedralDust=0) the cathedral holds 59.9 at 960x540, a
 * 154-room playtest never dropped, and the drops that remained (chinatown_10, haunted_0, tiedup, the title, each at
 * 6-10 of 60 frames) came in CPU-bound scenes, where 640x360 only blurred the text.
 */
#define DYNRES_WINDOW 60
#define DYNRES_BOUND_FRAMES 6
#define DYNRES_SWAP_US 2000
#define DYNRES_MISS_US 25000 // 1.5 vblank intervals
#define DYNRES_CPU_US 15000
#define DYNRES_SETTLE_FRAMES 60
int dynamic_res = 0;
unsigned dynres_drops;
static int dynres_test; // exp_dynrestest: every frame counts as GPU-bound, so each room drops after ~2 s

static void dynamic_res_update(uint32_t swap_us, uint32_t work_us) {
	static int room = -2, frames, window, bound, dropped;
	static uint64_t last;
	uint64_t now = sceKernelGetProcessTimeWide();
	uint64_t period = last ? now - last : 0;
	last = now;
	if (!dynamic_res || !app_surf_native || app_full_w <= 640 || !current_room)
		return;
	if (*current_room != room) {
		room = *current_room;
		frames = window = bound = 0;
		if (dropped) {
			dropped = 0;
			app_surf_w = app_full_w;
			app_surf_h = app_full_h;
		}
		return;
	}
	if (dropped || ++frames <= DYNRES_SETTLE_FRAMES)
		return;
	bound += (period > DYNRES_MISS_US && work_us < DYNRES_CPU_US && swap_us > DYNRES_SWAP_US) || dynres_test;
	if (++window < DYNRES_WINDOW)
		return;
	if (bound >= DYNRES_BOUND_FRAMES) {
		dropped = 1;
		dynres_drops++;
		app_surf_w = 640;
		app_surf_h = 360;
		debugPrintf("[DYNRES] room %d GPU-bound (%d of %d frames missed a vblank waiting at the swap): 640x360\n",
			room, bound, window);
	}
	window = bound = 0;
}

// Runs right after vglSwapBuffers, between frames: every scene of the frame has been submitted, so the GPU finish
// and garbage collection below cannot free memory that commands still being recorded refer to

uint32_t after_swap_dynres_us; // debug: dynamic_res_update of the last frame
static void after_swap(uint32_t swap_us, uint32_t work_us);
void katana_after_swap(uint32_t swap_us, uint32_t work_us) {
	after_swap(swap_us, work_us);
	// Heap blocks for pages outside VRAM taken back once a flush freed the pages that used them (page_reserve.c)
	extern void page_reserve_frame(void);
	page_reserve_frame();
	// The title pages' state as the next frame starts, after any recovery flush above
	extern void title_pages_snapshot(void);
	title_pages_snapshot();
	extern void psych_fix_frame(void);
	psych_fix_frame();
}

static void after_swap(uint32_t swap_us, uint32_t work_us) {
	options_menu_sync();
	enemy_half_rate_update();
	uint32_t s0 = sceKernelGetProcessTimeLow();
	dynamic_res_update(swap_us, work_us);
	after_swap_dynres_us = sceKernelGetProcessTimeLow() - s0;
	rt_free_report();
	extern void page_evict_frame(void);
	page_evict_frame();
	// A frame that skipped its swap (frameSkip) may leave scenes open: no collection until a presented frame
	if (vgl_scene_open)
		return;
	texdrain_run("after the swap");
	forced_gc_report();
	extern int vgl_failed_allocs;
	extern size_t vgl_gpu_fallback_bytes;
	// The title screen's pages are made resident right after its first frame (title_pages.c). It acknowledges a spill
	// only when every byte of it was a title page it reloaded into VRAM.
	extern int title_pages_after_swap(size_t *spill_seen);
	size_t title_spill_seen;
	if (title_pages_after_swap(&title_spill_seen))
		texflush_spill_seen = title_spill_seen;
	// The pages the game asked for that the last recovery flush evicted, loaded back now that the room has drawn
	extern int page_restore_after_swap(size_t *spill_seen);
	size_t restore_spill_seen;
	if (page_restore_after_swap(&restore_spill_seen))
		texflush_spill_seen = restore_spill_seen;
	if (!texture_flush)
		return;
	const char *reason = NULL;
	uint32_t now = sceKernelGetProcessTimeLow();
	// A failure within TEXFLUSH_MIN_INTERVAL_US of the last flush is flushed for once the interval is over: dropping
	// it left the page that failed (room_mansion_0: the font page, text drawn as solid boxes) missing for good.
	// At most TEXFLUSH_FAILURE_FLUSHES per room, so a room whose pages never fit doesn't pause every few seconds.
	static int failure_pending, failure_room = -2, failure_flushes;
	int room = current_room ? *current_room : -1;
	if (room != failure_room) {
		failure_room = room;
		failure_flushes = 0;
	}
	if (vgl_failed_allocs != texflush_failures_seen) {
		texflush_failures_seen = vgl_failed_allocs;
		failure_pending = failure_flushes < TEXFLUSH_FAILURE_FLUSHES;
	}
	if (failure_pending && (!texflush_count || now - texflush_last_us >= TEXFLUSH_MIN_INTERVAL_US)) {
		failure_flushes++;
		reason = "allocation failure";
	}
	if (vgl_gpu_fallback_bytes - texflush_spill_seen >= TEXFLUSH_SPILL_BYTES) {
		// With page eviction on (page_evict.c), data outside VRAM that isn't a texture page (a surface) isn't flushed
		// for: the flush leaves it where it is and only reloads every page (1.3 s at the psychiatrist's intro
		// transformation); eviction makes room for the next allocations instead
		extern int page_evict_page_outside_vram(void);
		int page = page_evict_page_outside_vram();
		texflush_spill_seen = vgl_gpu_fallback_bytes;
		if (!page)
			debugPrintf("[TEXFLUSH] no flush: the data outside VRAM is not a texture page (free vram %uKB)\n",
				vglMemFree(VGL_MEM_VRAM) / 1024);
		else if (room != texflush_spill_room) {
			texflush_spill_room = room;
			reason = "texture data outside VRAM";
		}
	} else
		texflush_spill_seen = vgl_gpu_fallback_bytes;
	static int *new_room;
	if (!new_room)
		new_room = (int *)so_symbol(&yoyoloader_mod, "New_Room");
	if (!reason && new_room && *new_room >= 0 && *new_room != room && vglMemFree(VGL_MEM_VRAM) < TEXFLUSH_LOW_VRAM)
		reason = "low free VRAM before a room change";
	if (reason) {
		failure_pending = 0; // the flush reloads every page, including the one that failed
		gml_rvalue result = {0};
		hang_set_stage(HANG_STAGE_TEXFLUSH);
		extern void page_restore_before_flush(void);
		page_restore_before_flush(); // the requested pages it evicts are loaded back after the next frame
		texture_flush(&result, NULL, NULL, 0, NULL);
		// Free the flushed pages now: otherwise vitaGL releases them a few frames later and the reloads,
		// still not fitting, land in RAM again. Only with no scene open: the flush call itself can open one
		// (texdrain above); then the drain runs at the start of Draw or after the next swap
		texdrain_pending = "the page recovery flush";
		hang_set_stage(HANG_STAGE_GPU_GC);
		texdrain_run("after the swap");
		hang_set_stage(HANG_STAGE_AFTER_SWAP);
		texflush_last_us = sceKernelGetProcessTimeLow();
		texflush_count++;
		debugPrintf("[TEXFLUSH] texture pages flushed after %s (%.1f ms); free vram now %uKB\n", reason,
			(texflush_last_us - now) / 1000.0f, vglMemFree(VGL_MEM_VRAM) / 1024);
	}
}

/*
 * Screen copies with the resized application surface: effects that copy application_surface into a surface
 * of the size the game set for it (1280x720; obj_drug_sequence, the medicine effect at the psychiatrist) got
 * the image in its top-left corner and showed the screen shrunk. surface_copy of application_surface to (0, 0)
 * of a surface of that size is drawn stretched to fill it instead (blending off, so pixels and alpha are copied
 * as is). Copies into surfaces of any other size, and of other surfaces, are left alone.
 */
static int *app_surface_id; // g_ApplicationSurface

// application_surface is a surface reference (kind 15, id in the low word, 0x08000004 above it); a stored copy
// may also be a plain number
static int is_app_surface(const gml_rvalue *v) {
	if (!app_surface_id)
		return 0;
	if (v->kind == 0)
		return v->val == (double)*app_surface_id;
	if ((v->kind & 0xffffff) == 15) {
		uint32_t words[2];
		memcpy(words, &v->val, sizeof(words));
		return words[1] == 0x08000004 && (int)words[0] == *app_surface_id;
	}
	return 0;
}
static builtin_fn orig_surface_copy, surface_width, surface_height, surface_set_target, surface_reset_target,
	draw_surface_stretched_ext, gpu_get_blendenable, gpu_set_blendenable;
unsigned appsurf_scaled_copies;

static double builtin_real(builtin_fn fn, int argc, gml_rvalue *args) {
	gml_rvalue result = {0};
	fn(&result, NULL, NULL, argc, args);
	return result.val;
}

static void surface_copy_scaled(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (argc == 4 && app_game_w && args[1].kind == 0 && args[2].kind == 0 && args[1].val == 0.0 &&
		args[2].val == 0.0 && is_app_surface(&args[3])) {
		double sw = builtin_real(surface_width, 1, &args[3]), sh = builtin_real(surface_height, 1, &args[3]);
		double dw = builtin_real(surface_width, 1, &args[0]), dh = builtin_real(surface_height, 1, &args[0]);
		if ((sw != dw || sh != dh) && dw == app_game_w && dh == app_game_h) {
			gml_rvalue blend = {0}, r = {0};
			gpu_get_blendenable(&blend, self, other, 0, NULL);
			gml_rvalue off = {0};
			gpu_set_blendenable(&r, self, other, 1, &off);
			surface_set_target(&r, self, other, 1, &args[0]);
			gml_rvalue draw[7] = { args[3], {0}, {0}, {dw}, {dh}, {16777215.0}, {1.0} }; // c_white, alpha 1
			draw_surface_stretched_ext(&r, self, other, 7, draw);
			surface_reset_target(&r, self, other, 0, NULL);
			gpu_set_blendenable(&r, self, other, 1, &blend);
			if (!appsurf_scaled_copies++)
				debugPrintf("app surface copy scaled: %.0fx%.0f -> %.0fx%.0f\n", sw, sh, dw, dh);
			result->kind = 0;
			result->val = 0.0;
			return;
		}
	}
	orig_surface_copy(result, self, other, argc, args);
}

void appsurf_copy_install(void) {
	runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	if (!app_surf_native || !functions || !*functions || !count)
		return;
	runner_function *copy = NULL;
	for (int i = 0; i < *count; i++) {
		runner_function *f = &(*functions)[i];
		if (!f->name)
			continue;
		if (!strcmp(f->name, "surface_copy"))
			copy = f;
		else if (!strcmp(f->name, "surface_get_width"))
			surface_width = f->func;
		else if (!strcmp(f->name, "surface_get_height"))
			surface_height = f->func;
		else if (!strcmp(f->name, "surface_set_target"))
			surface_set_target = f->func;
		else if (!strcmp(f->name, "surface_reset_target"))
			surface_reset_target = f->func;
		else if (!strcmp(f->name, "draw_surface_stretched_ext"))
			draw_surface_stretched_ext = f->func;
		else if (!strcmp(f->name, "gpu_get_blendenable"))
			gpu_get_blendenable = f->func;
		else if (!strcmp(f->name, "gpu_set_blendenable"))
			gpu_set_blendenable = f->func;
	}
	if (!copy || !surface_width || !surface_height || !surface_set_target || !surface_reset_target ||
			!draw_surface_stretched_ext || !gpu_get_blendenable || !gpu_set_blendenable) {
		debugPrintf("app surface copy scaling: NOT installed\n");
		return;
	}
	orig_surface_copy = copy->func;
	copy->func = surface_copy_scaled;
	debugPrintf("app surface copy scaling: installed\n");
}

// Called from patch_performance: the application surface size
void katana_display_install(void) {
	app_width = (int *)so_symbol(&yoyoloader_mod, "g_ApplicationWidth");
	app_height = (int *)so_symbol(&yoyoloader_mod, "g_ApplicationHeight");
	app_new_size = (char *)so_symbol(&yoyoloader_mod, "g_NewApplicationSize");
	app_new_width = (int *)so_symbol(&yoyoloader_mod, "g_NewApplicationWidth");
	app_new_height = (int *)so_symbol(&yoyoloader_mod, "g_NewApplicationHeight");
	if (!app_new_size || !app_new_width || !app_new_height)
		app_width = NULL;
	app_surface_id = (int *)so_symbol(&yoyoloader_mod, "g_ApplicationSurface");
	app_surf_native = !exp_flag("appsurf1280");
	dynres_test = exp_flag("dynrestest");
	// Output size at 16:9 when larger than the game's 640x360 (renderRes=960: 960x540)
	extern int screen_w;
	if (screen_w > 640 && !exp_flag("appsurf640")) {
		app_surf_w = app_full_w = screen_w;
		app_surf_h = app_full_h = screen_w * 9 / 16;
	}
}

// Called at the end of patch_performance: draw hook and texture page recovery
void katana_display_hook_draw(void) {
	// Application surface size and dynamic resolution, and lighting at 30 Hz (default on: global.recolor_timer is
	// only a lighting-cache generation counter; exp_lighting60 restores per-frame lighting)
	uintptr_t *slot = so_got_slot(&yoyoloader_mod, "_Z12DoAStep_Drawv");
	if (slot) {
		lighting30 = global_obj_ptr && var_recolor_timer && !exp_flag("lighting60");
		katana_orig_draw = (void *)*slot;
		*slot = (uintptr_t)&DoAStep_Draw_katana;
	}
	debugPrintf("draw hook: %s, lighting %s\n", slot ? "installed" : "NOT installed", lighting30 ? "30 Hz" : "60 Hz");

	// Default on: reload texture pages that failed to allocate or spilled out of VRAM (katana_after_swap)
	uintptr_t flush = (uintptr_t)so_symbol(&yoyoloader_mod, "_Z23F_YoYo_DrawTextureFlushR6RValueP9CInstanceS2_iPS_");
	if (!exp_flag("texdrain0") && flush && (orig_texture_flush = entry_hook(flush, texture_flush_drain)))
		flush = (uintptr_t)orig_texture_flush; // the loader's own flush below drains by itself
	debugPrintf("texture flush drain: %s\n", orig_texture_flush ? "installed" : "NOT installed");
	if (!exp_flag("texflush0"))
		texture_flush = (builtin_fn)flush;
}
