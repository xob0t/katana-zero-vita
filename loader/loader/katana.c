/*
 * katana.c
 *
 * Runner patches specific to Katana ZERO (GMS2 2024 YYC runner).
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



// Profiling state for global.recolor_timer sampling (see sample_recolor_timer)
uint32_t recolor_changes, recolor_frames;
double last_recolor_timer;

static int (*SaveFileExists)(const char *path, void *unk);
static int (*BundleFileExists)(const char *path);

// Same logic as the runner's FileExists, with logging to trace asset lookups
static int FileExists_hook(const char *path) {
	int in_save = SaveFileExists(path, NULL);
	int in_bundle = in_save ? 0 : BundleFileExists(path);
	debugPrintf("FileExists(%s): save=%d bundle=%d\n", path, in_save, in_bundle);
	return in_save || in_bundle;
}



void **global_obj_ptr;

// Replaces a function at its entry with a native wrapper, returns a trampoline to the original
// (NULL if the prologue isn't the relocatable push {..., lr}; add fp, sp, #imm).
void *entry_hook(uintptr_t entry, void *replacement) {
	uint32_t *w = (uint32_t *)entry;
	if (!entry || (entry & 3) || (w[0] & 0xffff4000) != 0xe92d4000 || (w[1] & 0xfffff000) != 0xe28db000)
		return NULL;
	uint32_t *tramp = katana_rx_alloc(16);
	if (!tramp)
		return NULL;
	uint32_t code[4] = { w[0], w[1], 0xe51ff004, (uint32_t)(entry + 8) };
	kuKernelCpuUnrestrictedMemcpy(tramp, code, sizeof(code));
	kuKernelFlushCaches(tramp, sizeof(code));
	hook_addr(entry, (uintptr_t)replacement);
	kuKernelFlushCaches((void *)entry, 8);
	return tramp;
}

/*
 * Performance throttles for cosmetic effects that are too heavy for the Vita CPU.
 * Implemented by swapping compiled event functions in the runner's g_GMLFuncs table.
 */

#define GML_FUNCS_SIZE 79992
#define PARTICLES_ALWAYS_DRAWN 20

uint32_t *frame_count;

static event_func orig_clothesline_end_step;
static event_func orig_softbody_node_step;
static event_func orig_rain_draw;
static event_func orig_particle_draw;

// Instance pointers are 16-byte aligned; use them for a stable per-instance parity bit
#define INSTANCE_PARITY(self) (((uintptr_t)(self) >> 4) & 1)

// 30 fps mode (see t30_install) is active: ticks run at 30 Hz, so every-other-tick work runs on every tick
int t30_active;

static inline int half_rate_tick(void) {
	return t30_active || !(*frame_count & 1);
}

// Cloth simulation (clothesline + its softbody nodes) runs at 30Hz, and not at all while the
// instance is far outside the view: the clothesline alone costs ~5.5 ms/frame (its Draw is
// 49 KB of GML) even when it isn't on screen. Anything within CLOTH_MARGIN of the view keeps
// running, so the rope is fully simulated before it can come into sight. exp_clothall disables.
#define CLOTH_MARGIN 512.0
void **global_obj_ptr;
static int *var_xview, *var_yview, *var_wview, *var_hview;
static event_func orig_clothesline_draw;
static int cloth_cull;
static uint32_t cloth_culled, cloth_events;
static double cloth_last[6];

double global_real(int *var, double fallback) {
	gml_rvalue *v = gml_vget(*global_obj_ptr, var[1]);
	return (v && v->kind == 0) ? v->val : fallback;
}

static void **run_room;
int *current_room;
static void *cloth_room;
static uint32_t cloth_room_frame;

static int cloth_far_from_view(void *self) {
	if (!cloth_cull)
		return 0;
	// Always simulate for the first 3 seconds in a room so ropes settle exactly as in the
	// original before they can be paused
	// Run_Room can keep pointing at the same CRoom across room changes; the room index does change
	static int cloth_room_index = -1;
	if (*run_room != cloth_room || (current_room && *current_room != cloth_room_index)) {
		cloth_room = *run_room;
		cloth_room_index = current_room ? *current_room : -1;
		cloth_room_frame = *frame_count;
	}
	if (*frame_count - cloth_room_frame < 180)
		return 0;
	float x, y;
	memcpy(&x, (char *)self + 0xb8, 4);
	memcpy(&y, (char *)self + 0xbc, 4);
	double vx = global_real(var_xview, x), vy = global_real(var_yview, y);
	double vw = global_real(var_wview, 0), vh = global_real(var_hview, 0);
	cloth_events++;
	int far = x < vx - CLOTH_MARGIN || x > vx + vw + CLOTH_MARGIN || y < vy - CLOTH_MARGIN || y > vy + vh + CLOTH_MARGIN;
	cloth_culled += far;
	cloth_last[0] = x; cloth_last[1] = y; cloth_last[2] = vx; cloth_last[3] = vy; cloth_last[4] = vw; cloth_last[5] = vh;
	return far;
}

static void clothesline_end_step(void *self, void *other) {
	if (half_rate_tick() && !cloth_far_from_view(self))
		orig_clothesline_end_step(self, other);
}

// Unless ropeLights=1 in yyl.cfg: while a clothesline draws, report obj_lightsource (object 621) as
// absent, so its Draw skips the per-link light tint loop (~80% of its cost: first level near the rope
// 50 -> 57.8 fps, room_chinatown_2 43.8 -> 51.4). Visual only: no coloured light glow on the rope and
// clothes; player support logic is untouched.
static int in_cloth_draw;
static void (*orig_instance_exists)(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args);

static void cloth_instance_exists(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (in_cloth_draw && argc == 1) {
		uint32_t lo;
		memcpy(&lo, &args[0].val, 4);
		if ((args[0].kind == 15 && lo == 621) || (args[0].kind == 0 && args[0].val == 621.0)) {
			result->val = 0.0;
			result->kind = 13;
			return;
		}
	}
	orig_instance_exists(result, self, other, argc, args);
}

static void clothesline_draw(void *self, void *other) {
	if (!cloth_far_from_view(self)) {
		in_cloth_draw++;
		orig_clothesline_draw(self, other);
		in_cloth_draw--;
	}
}

// obj_clothesline_custom (room_chinatown_2) is a separate object with the same structure
static event_func orig_clothesline_custom_draw, orig_clothesline_custom_end_step;

static void clothesline_custom_end_step(void *self, void *other) {
	if (half_rate_tick() && !cloth_far_from_view(self))
		orig_clothesline_custom_end_step(self, other);
}

static void clothesline_custom_draw(void *self, void *other) {
	if (!cloth_far_from_view(self)) {
		in_cloth_draw++;
		orig_clothesline_custom_draw(self, other);
		in_cloth_draw--;
	}
}

static void softbody_node_step(void *self, void *other) {
	if (half_rate_tick() && !cloth_far_from_view(self))
		orig_softbody_node_step(self, other);
}

void katana_cloth_report(int frames) {
	if (cloth_events)
		debugPrintf("[CLOTH] %.0f%% of cloth events culled (far from view)\n", 100.0f * cloth_culled / cloth_events);
	if (cloth_events)
		debugPrintf("[CLOTH] last instance at %.0f,%.0f, view %.0f,%.0f %.0fx%.0f\n", cloth_last[0], cloth_last[1],
			cloth_last[2], cloth_last[3], cloth_last[4], cloth_last[5]);
	cloth_culled = cloth_events = 0;
}

// Each rain emitter is drawn every other frame, alternating between emitters
static void rain_draw(void *self, void *other) {
	if (t30_active || (INSTANCE_PARITY(self) ^ (*frame_count & 1)))
		orig_rain_draw(self, other);
}

// obj_particle user event 15 is its draw routine: past the first few particles in a frame,
// draw the rest on alternating frames
// Reads a variable of a YYObjectBase through its virtual getter (vtable entry 2), as YYC code does

void **global_obj_ptr;
int *var_recolor_timer;

gml_rvalue *gml_vget(void *obj, int slot) {
	gml_rvalue *(*getter)(void *, int) = (*(void ***)obj)[2];
	return getter(obj, slot);
}

// Profiling: how often global.recolor_timer changes (each change makes lit sprites recompute lighting)
static void sample_recolor_timer(void) {
	if (!global_obj_ptr || !*global_obj_ptr || !var_recolor_timer)
		return;
	gml_rvalue *v = gml_vget(*global_obj_ptr, var_recolor_timer[1]);
	if (v && v->kind == 0) {
		if (v->val != last_recolor_timer)
			recolor_changes++;
		last_recolor_timer = v->val;
	}
	recolor_frames++;
}

static void particle_draw(void *self, void *other) {
	static uint32_t last_frame, drawn;
	if (last_frame != *frame_count) {
		last_frame = *frame_count;
		drawn = 0;
		extern int heavy_profiling;
		if (heavy_profiling)
			sample_recolor_timer();
	}
	if (t30_active || drawn++ < PARTICLES_ALWAYS_DRAWN || (INSTANCE_PARITY(self) ^ (*frame_count & 1)))
		orig_particle_draw(self, other);
}

/*
 * Frame skipping: when a frame runs over budget, the next frame only runs game logic
 * and the previous image stays on screen. Never skips twice in a row.
 */
// A frame is "behind" when it took longer than a 60 Hz frame plus some slack
#define FRAMESKIP_THRESHOLD_US 17500

static void (*frameskip_orig_draw)(void);
static uint32_t last_frame_start;
static int last_frame_skipped;
static int draw_skipped;
static volatile int draw_skipping;
static uint32_t frames_seen, frames_skipped;

extern int frame_skip;

/*
 * Adaptive partial frame skip: when a frame falls behind, the next frame still runs the
 * whole game logic and the draw pass, but Draw events of ordinary objects return
 * immediately and the frame is not presented. Draw events of story/UI objects (cutscenes,
 * textboxes, menus, transitions...) always run, because they contain logic: skipping them
 * stalled the first tutorial.
 */
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif
#define DRAWSKIP_STUB_WORDS 3
#define DRAWSKIP_ADDRESS (LOAD_ADDRESS + 0xC00000)

static const char *story_words[] = {
	"cutscene", "textbox", "choicebox", "dialog", "pausemenu", "gamepause", "pausecamera",
	"transition", "prompt", "fade", "ending", "credits", "title", "menu", "options",
	"leveltext", "popuptext", "level_end", "go_arrow", "text"
};

static event_func *drawskip_orig;
static const char **drawskip_names;
static uint8_t *drawskip_seen;
static int drawskip_num;

static void drawskip_call(void *self, void *other, int idx) {
	if (!draw_skipping)
		drawskip_orig[idx](self, other);
	else if (drawskip_seen)
		drawskip_seen[idx] = 1;
}

// Debug: lists the Draw events that were skipped since the last call
void katana_drawskip_dump(void) {
	if (!debugMode || !drawskip_seen)
		return;
	for (int i = 0; i < drawskip_num; i++) {
		if (drawskip_seen[i]) {
			debugPrintf("[SKIPPED] %s\n", drawskip_names[i]);
			drawskip_seen[i] = 0;
		}
	}
}

// Debug report for the frame-stats log line
const char *katana_guard_last() {
	static char buf[64];
	snprintf(buf, sizeof(buf), "skipped %u of %u frames", frames_skipped, frames_seen);
	frames_skipped = frames_seen = 0;
	return buf;
}

static int is_story_object(const char *name) {
	const char *obj = name + 11; // skip "gml_Object_"
	for (int i = 0; i < sizeof(story_words) / sizeof(*story_words); i++) {
		if (strstr(obj, story_words[i]))
			return 1;
	}
	return 0;
}

// Only world Draw events are skipped: Draw (0), Draw Begin (72), Draw End (73). GUI draws
// (64, 74, 75) always run, since HUD and touch-button logic lives there.
static int is_skippable_draw(const char *name) {
	if (strncmp(name, "gml_Object_", 11) || is_story_object(name))
		return 0;
	const char *ev = strstr(name, "_Draw_");
	if (!ev)
		return 0;
	ev += 6;
	return !strcmp(ev, "0") || !strcmp(ev, "72") || !strcmp(ev, "73");
}

static void install_draw_skip(gml_func_entry *table) {
	int count = GML_FUNCS_SIZE / sizeof(gml_func_entry);
	int num = 0;
	for (int i = 0; i < count; i++) {
		if (table[i].name && table[i].func && is_skippable_draw(table[i].name))
			num++;
	}
	if (!num)
		return;

	SceSize size = (num * DRAWSKIP_STUB_WORDS * 4 + 0xfff) & ~0xfff;
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = 0x1;
	opt.field_C = DRAWSKIP_ADDRESS;
	SceUID block = kuKernelAllocMemBlock("draw skip", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, size, &opt);
	if (block < 0) {
		debugPrintf("Katana: draw skip allocation failed (0x%08X), disabling frame skip\n", block);
		frame_skip = 0;
		return;
	}
	uint32_t *stubs_rx;
	sceKernelGetMemBlockBase(block, (void **)&stubs_rx);
	uint32_t *stubs = calloc(1, size);
	drawskip_orig = calloc(num, sizeof(event_func));
	drawskip_names = calloc(num, sizeof(char *));
	drawskip_seen = calloc(num, 1);
	drawskip_num = num;

	int n = 0;
	for (int i = 0; i < count && n < num; i++) {
		if (!table[i].name || !table[i].func || !is_skippable_draw(table[i].name))
			continue;
		uint32_t *s = &stubs[n * DRAWSKIP_STUB_WORDS];
		s[0] = 0xE3002000 | ((n >> 12) << 16) | (n & 0xfff); // movw r2, #n
		s[1] = 0xE51FF004;                                   // ldr pc, [pc, #-4]
		s[2] = (uint32_t)&drawskip_call;
		drawskip_orig[n] = table[i].func;
		drawskip_names[n] = table[i].name;
		table[i].func = (event_func)&stubs_rx[n * DRAWSKIP_STUB_WORDS];
		n++;
	}
	kuKernelCpuUnrestrictedMemcpy(stubs_rx, stubs, size);
	kuKernelFlushCaches(stubs_rx, size);
	free(stubs);
	debugPrintf("Katana: draw skip on %d Draw events\n", n);
}

static void DoAStep_Draw_frameskip(void) {
	uint32_t now = sceKernelGetProcessTimeLow();
	uint32_t last_frame_time = now - last_frame_start;
	last_frame_start = now;

	int skip = frame_skip && last_frame_time > FRAMESKIP_THRESHOLD_US && !last_frame_skipped;
	draw_skipping = skip;
	frameskip_orig_draw();
	draw_skipping = 0;
	draw_skipped = skip;
	last_frame_skipped = skip;
	frames_seen++;
	frames_skipped += skip;
}

// Queried by the main loop to avoid presenting a frame that wasn't drawn
int katana_frame_skipped() {
	return draw_skipped;
}

// Executable memory for trampolines of native replacements (entry hooks that still call the
// original): a small bump allocator over one RX block below the runner
#define RX_ARENA_ADDRESS (LOAD_ADDRESS + 0xE00000)
#define RX_ARENA_SIZE 0x10000
static uint8_t *rx_arena;
static size_t rx_arena_used;

void *katana_rx_alloc(size_t size) {
	if (!rx_arena) {
		SceKernelAllocMemBlockKernelOpt opt;
		memset(&opt, 0, sizeof(opt));
		opt.size = sizeof(opt);
		opt.attr = 0x1;
		opt.field_C = RX_ARENA_ADDRESS;
		SceUID block = kuKernelAllocMemBlock("katana rx", 0x0C20D050 /* USER_RX */, RX_ARENA_SIZE, &opt);
		// Only if the fixed address is taken, as in the Vita3K emulator, which puts a 9 MB block of uncached
		// memory at 0x985C6000-0x98E8C000 (on the Vita the fixed address always succeeds): right under the
		// runner's patch block, in bl range of the same code, else anywhere. Any address does for the
		// trampolines (absolute jumps); the veneer users check their range.
		if (block < 0) {
			SceUID fixed = block;
			opt.field_C = yoyoloader_mod.patch_base - RX_ARENA_SIZE;
			block = kuKernelAllocMemBlock("katana rx", 0x0C20D050 /* USER_RX */, RX_ARENA_SIZE, &opt);
			if (block < 0) {
				opt.attr = 0;
				opt.field_C = 0;
				block = kuKernelAllocMemBlock("katana rx", 0x0C20D050 /* USER_RX */, RX_ARENA_SIZE, &opt);
			}
			if (block >= 0 && sceKernelGetMemBlockBase(block, (void **)&rx_arena) >= 0)
				debugPrintf("rx arena: fixed address taken (0x%08X), at %p instead\n", fixed, rx_arena);
		}
		if (block < 0 || sceKernelGetMemBlockBase(block, (void **)&rx_arena) < 0) {
			debugPrintf("rx arena: allocation failed (0x%08X)\n", block);
			rx_arena = NULL;
			return NULL;
		}
	}
	size = (size + 15) & ~15;
	if (rx_arena_used + size > RX_ARENA_SIZE)
		return NULL;
	void *p = rx_arena + rx_arena_used;
	rx_arena_used += size;
	return p;
}

// NurseryAlloc (the runner's per-frame bump allocator) returns cur and advances by exactly the
// requested size, so one odd-sized request misaligns every later chunk. Android's kernel emulates
// the resulting unaligned VFP stores; the Vita faults (e.g. draw_roundrect in obj_passive_textbox:
// room_chinatown_6 crashed on entry). Round every request up to 8 bytes.
static void *(*orig_nursery_alloc)(int size);

static void *nursery_alloc_aligned(int size) {
	return orig_nursery_alloc(size > 0 ? (size + 7) & ~7 : size);
}

static void install_nursery_align(void) {
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z12NurseryAlloci");
	if (!entry || (entry & 3) || *(uint32_t *)entry != 0xe92d4df0 || *(uint32_t *)(entry + 4) != 0xe28db018) {
		debugPrintf("nursery align: unexpected NurseryAlloc prologue, not installed\n");
		return;
	}
	uint32_t *tramp = katana_rx_alloc(16);
	if (!tramp)
		return;
	uint32_t code[4] = { 0xe92d4df0, 0xe28db018, 0xe51ff004, (uint32_t)(entry + 8) };
	kuKernelCpuUnrestrictedMemcpy(tramp, code, sizeof(code));
	kuKernelFlushCaches(tramp, sizeof(code));
	orig_nursery_alloc = (void *)tramp;
	hook_addr(entry, (uintptr_t)&nursery_alloc_aligned);
	kuKernelFlushCaches((void *)entry, 8);
	debugPrintf("nursery align: installed\n");
}

// Texture pages: prepare_apk.py stores them as "2zoq" (the runner's bzip2 QOI container) with an
// LZ4 payload marked "LZ4H" instead of bzip2. That keeps the resident game.droid almost as small
// as the original bzip2 pages while decoding far faster.
//
// A page that fails to decode is not harmless: the runner keeps drawing with the broken texture,
// which crashes the GPU and takes the whole Vita down. So BZ2_bzBuffToBuffDecompress is replaced
// at its entry (catching direct calls from any patched runner build, not just PLT calls), bzip2
// input is refused with a clean fatal error, and katana_texture_selftest() checks the runner's
// real call path before the game starts.

// Payload: "LZ4H", u32 compressed size, LZ4 block. The runner passes the whole blob length
// (header included) as src_len, so the block size has to come from the payload itself.
static int lz4_page_decompress(char *dest, unsigned *dest_len, char *src, unsigned src_len, int small, int verbosity) {
	if (src_len < 8 || memcmp(src, "LZ4H", 4))
		fatal_error("game.apk has bzip2 texture pages. Rebuild it with scripts/prepare_apk.py.");
	uint32_t block_len;
	memcpy(&block_len, src + 4, 4);
	extern volatile int hang_texture_decode;
	hang_texture_decode = 1;
	// The runner passes the QOI size from the page header, which prepare_apk.py sets to the exact decoded size: a
	// shorter result means a damaged page, and the rest of the buffer would be uninitialised
	int n = block_len <= src_len - 8 ? LZ4_decompress_safe(src + 8, dest, block_len, *dest_len) : -1;
	if (n < 0 || (unsigned)n != *dest_len)
		fatal_error("LZ4 texture page failed to decode (%u of %u bytes -> %u): %d", block_len, src_len, *dest_len, n);
	hang_texture_decode = 0;
	return 0; // BZ_OK
}

static void install_lz4_pages(void) {
	uintptr_t bz2 = so_symbol(&yoyoloader_mod, "BZ2_bzBuffToBuffDecompress");
	if (!bz2)
		fatal_error("BZ2_bzBuffToBuffDecompress not found in the runner");
	hook_addr(bz2, (uintptr_t)&lz4_page_decompress);
	kuKernelFlushCaches((void *)(bz2 & ~3), 16);
	// Also swap the PLT slot so nothing depends on the entry patch alone
	uintptr_t *slot = so_got_slot(&yoyoloader_mod, "BZ2_bzBuffToBuffDecompress");
	if (slot)
		*slot = (uintptr_t)&lz4_page_decompress;
}

// Game bug fix: obj_new_platform_lighting's User event 0 passes room_width/room_height (pixels) to
// layer_tilemap_create as TILE counts. With its 32 px tiles the maps are 1024x too large (19-25 MB
// each, 3-4 per lit room; room_factory2_3 ran out of memory and crashed). Only the first
// ceil(w/32) x ceil(h/32) cells are ever written or drawn so create the maps
// at that size while that event runs. exp_lightmapbig restores the original sizes.
static event_func orig_lighting_user0;
static int in_lighting_user0;
static void (*orig_layer_tilemap_create)(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args);
static unsigned lightmaps_shrunk;

static void lighting_user0(void *self, void *other) {
	in_lighting_user0++;
	orig_lighting_user0(self, other);
	in_lighting_user0--;
}

// Netflix logo on launch (obj_netflixSDKScreen): skipNetflixLogo=1 (default) zeroes its fade/hold timers right
// after Create and skips its GUI draw. The rest of its Step still runs as in the original (creates
// obj_netflixSDK, waits for sdkReady, sets allowMenu, room_goto), so boot proceeds the same way, just without
// the logo.
int skip_netflix_logo = 1;
static event_func orig_netflix_screen_create, orig_netflix_screen_draw;

static void netflix_screen_create(void *self, void *other) {
	orig_netflix_screen_create(self, other);
	static const char *const timers[] = { "startFadeIn", "waitFade", "logoFade", "logoPause", "loadSdkWait" };
	for (int i = 0; i < 5; i++) {
		char sym[64];
		snprintf(sym, sizeof(sym), "g_VAR_%s", timers[i]);
		int *var = (int *)so_symbol(&yoyoloader_mod, sym);
		gml_rvalue *v = var ? gml_vget(self, var[1]) : NULL;
		if (v && (v->kind == 0 || v->kind == 13))
			v->val = 0;
	}
}

static void netflix_screen_draw(void *self, void *other) {
}

static void layer_tilemap_create_fixed(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (in_lighting_user0 && argc == 6 && args[4].kind == 0 && args[5].kind == 0 &&
		args[4].val > 32.0 && args[5].val > 32.0 && args[4].val < 1e6 && args[5].val < 1e6) {
		gml_rvalue fixed[6];
		memcpy(fixed, args, sizeof(fixed));
		fixed[4].val = ceil(args[4].val / 32.0);
		fixed[5].val = ceil(args[5].val / 32.0);
		lightmaps_shrunk++;
		orig_layer_tilemap_create(result, self, other, argc, fixed);
		return;
	}
	orig_layer_tilemap_create(result, self, other, argc, args);
}

// Debug room jump for fast benchmarking: writing a room name to ux0:data/gms/shared/goto_room makes
// the running game room_goto() there (debug mode only; the files are polled off the game thread, screenshot.c).
void katana_goto_check(void) {
	static int (*room_find)(const char *name);
	static void (*room_goto)(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args);
	// Release builds never look for these files (this used to stat three files on the game thread once a second)
	if (!debugMode)
		return;
	// Debug: ux0:data/gms/shared/level_complete creates obj_level_complete (object 608 in Katana ZERO 1.1.10),
	// as reaching the level exit does
	if (debug_request_take(DEBUG_REQUEST_LEVEL_COMPLETE, NULL, 0)) {
		void *(*command_create)(int object, float x, float y) = (void *)so_symbol(&yoyoloader_mod, "_Z14Command_Createiff");
		debugPrintf("level_complete: creating obj_level_complete\n");
		if (command_create)
			command_create(608, 0.0f, 0.0f);
	}
	// Debug: ux0:data/gms/shared/create_object (an object index) creates that object at (0, 0)
	char text[16];
	if (debug_request_take(DEBUG_REQUEST_CREATE_OBJECT, text, sizeof(text))) {
		void *(*command_create)(int object, float x, float y) = (void *)so_symbol(&yoyoloader_mod, "_Z14Command_Createiff");
		// "<index> view" creates it at the centre of the view instead
		float x = 0.0f, y = 0.0f;
		if (strstr(text, "view")) {
			x = global_real(var_xview, 0) + global_real(var_wview, 0) / 2;
			y = global_real(var_yview, 0) + global_real(var_hview, 0) / 2;
		}
		debugPrintf("create_object: object %d at %.0f,%.0f\n", atoi(text), x, y);
		if (command_create)
			command_create(atoi(text), x, y);
	}
	// Debug: ux0:data/gms/shared/destroy_object (an object name or index) runs instance_destroy(object): every instance
	// of it and of its children, with their Destroy events (e.g. "obj_enemy" clears a room of enemies, no corpses)
	char obj_name[64];
	if (debug_request_take(DEBUG_REQUEST_DESTROY_OBJECT, obj_name, sizeof(obj_name))) {
		for (char *c = obj_name; *c; c++)
			if (*c == '\r' || *c == '\n' || *c == ' ')
				*c = 0;
		int (*object_find)(const char *name) = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
		void (*instance_destroy)(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) =
			(void *)so_symbol(&yoyoloader_mod, "_Z17F_InstanceDestroyR6RValueP9CInstanceS2_iPS_");
		int index = obj_name[0] >= '0' && obj_name[0] <= '9' ? atoi(obj_name) : object_find ? object_find(obj_name) : -1;
		debugPrintf("destroy_object: %s -> %d\n", obj_name, index);
		if (index >= 0 && instance_destroy) {
			gml_rvalue result = {0}, arg = {0};
			arg.val = index;
			arg.kind = 0;
			instance_destroy(&result, NULL, NULL, 1, &arg);
		}
	}
	char name[64];
	if (!debug_request_take(DEBUG_REQUEST_GOTO_ROOM, name, sizeof(name)))
		return;
	for (char *c = name; *c; c++)
		if (*c == '\r' || *c == '\n' || *c == ' ')
			*c = 0;
	if (!room_find) {
		room_find = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
		room_goto = (void *)so_symbol(&yoyoloader_mod, "_Z10F_RoomGotoR6RValueP9CInstanceS2_iPS_");
	}
	int index = room_find ? room_find(name) : -1;
	debugPrintf("goto_room: %s -> %d\n", name, index);
	if (index < 0 || !room_goto)
		return;
	gml_rvalue result = {0}, arg = {0};
	arg.val = index;
	arg.kind = 0;
	room_goto(&result, NULL, NULL, 1, &arg);
}

// Laser deaths: obj_sprite_dissolver slices the body into rows of chunk_size pixels (rows = spr_height /
// chunk_size, set by whoever creates it before user event 0), and every row becomes an obj_sprite_chunk with
// its own Step, five additive draws, collision checks and a sound: ~5 ms/frame while it lasts.
// dissolveChunkScale (yyl.cfg, default 4; 1 = original) multiplies chunk_size before that event: a quarter
// of the chunks, each four times as tall.
int dissolve_chunk_scale = 4;
int dancer_glow = 0; // dancerGlow in yyl.cfg: club dancers' rim glow (see patch_performance)
int cathedral_dust = 0; // cathedralDust in yyl.cfg: dust motes in room_cathedral's window light (see patch_performance)
static event_func orig_dissolver_user0;

static void dissolver_user0(void *self, void *other) {
	static int *var_chunk_size;
	if (!var_chunk_size)
		var_chunk_size = (int *)so_symbol(&yoyoloader_mod, "g_VAR_chunk_size");
	gml_rvalue *v = var_chunk_size ? gml_vget(self, var_chunk_size[1]) : NULL;
	if (v && v->kind == 0 && v->val >= 1.0 && v->val < 64.0)
		v->val *= dissolve_chunk_scale;
	orig_dissolver_user0(self, other);
}

static void dissolve_install(void) {
	if (dissolve_chunk_scale <= 1)
		return;
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z40gml_Object_obj_sprite_dissolver_Other_10P9CInstanceS0_");
	orig_dissolver_user0 = entry_hook(entry, dissolver_user0);
	debugPrintf("dissolve chunk scale %d: %s\n", dissolve_chunk_scale, orig_dissolver_user0 ? "installed" : "NOT installed");
}

// Prison fog: obj_fogspawner keeps ~170 obj_fog puffs alive (room_prison_3: ~4.5 ms per frame for Step, Draw
// and spawning). fogScale (yyl.cfg, default 4; 1 = original) keeps one puff in that many and draws the
// survivors denser: draw_alpha a becomes 1 - (1 - a)^sqrt(N) after their Step (puffs only partly overlap, so
// the full 1 - (1 - a)^N looked too opaque).
// The others are destroyed at the start of their first Step, after the original Create and the spawner's
// writes, so every random() call of the spawn still happens. The fog is visual only.
int fog_scale = 4;
unsigned fog_puffs_removed;
static event_func orig_fog_create, orig_fog_step;
static void (*fog_instance_destroy)(void *self, void *other, int argc, void **args);
static int *var_draw_alpha;

// Puffs waiting for their first Step: open-addressing set of instance pointers. Removed entries leave tombstones,
// so the set is cleared whenever it holds no puffs and at each room change (puffs destroyed before their first
// Step, e.g. by the room ending, are never taken out).
#define FOG_SET_SIZE 4096
#define FOG_TOMBSTONE ((void *)1)
static void *doomed_fog[FOG_SET_SIZE];
static unsigned doomed_fog_count;
static int doomed_fog_room = -2;

static void **fog_find(void *p) {
	for (unsigned i = ((uintptr_t)p >> 4) * 2654435761u % FOG_SET_SIZE, n = 0; n < FOG_SET_SIZE; i = (i + 1) % FOG_SET_SIZE, n++) {
		if (doomed_fog[i] == p)
			return &doomed_fog[i];
		if (!doomed_fog[i])
			return NULL;
	}
	return NULL;
}

static void fog_mark(void *p) {
	if (fog_find(p))
		return;
	for (unsigned i = ((uintptr_t)p >> 4) * 2654435761u % FOG_SET_SIZE, n = 0; n < FOG_SET_SIZE; i = (i + 1) % FOG_SET_SIZE, n++) {
		if (!doomed_fog[i] || doomed_fog[i] == FOG_TOMBSTONE) {
			doomed_fog[i] = p;
			doomed_fog_count++;
			return;
		}
	}
	// Set full: the puff is kept
}

static int fog_take(void *p) {
	void **slot = fog_find(p);
	if (!slot)
		return 0;
	if (--doomed_fog_count)
		*slot = FOG_TOMBSTONE;
	else
		memset(doomed_fog, 0, sizeof(doomed_fog));
	return 1;
}

// Discarded puffs that exist now (created, first Step not run yet)
unsigned fog_doomed_alive(void) {
	return doomed_fog_count;
}

// Puffs created, plus those fog_skip.c didn't create: puff n is kept when n % fog_scale == 0
unsigned fog_created;

static void fog_create(void *self, void *other) {
	orig_fog_create(self, other);
	int room = current_room ? *current_room : -1;
	if (room != doomed_fog_room) {
		doomed_fog_room = room;
		doomed_fog_count = 0;
		memset(doomed_fog, 0, sizeof(doomed_fog));
	}
	fog_take(self); // a new instance can reuse the memory of a puff destroyed before its first Step
	if (fog_created++ % fog_scale)
		fog_mark(self);
}

static void fog_step(void *self, void *other) {
	extern void fog_skip_puff_step(void);
	fog_skip_puff_step();
	if (fog_take(self)) {
		extern void fog_skip_doomed_step(void *self);
		fog_skip_doomed_step(self);
		fog_instance_destroy(self, other, 0, NULL);
		fog_puffs_removed++;
		return;
	}
	orig_fog_step(self, other);
	gml_rvalue *a = gml_vget(self, var_draw_alpha[1]);
	if (a && a->kind == 0) {
		double v = a->val < 0.0 ? 0.0 : a->val > 1.0 ? 1.0 : a->val;
		a->val = 1.0 - pow(1.0 - v, sqrt((double)fog_scale));
	}
}

static void fog_install(void) {
	if (fog_scale <= 1)
		return;
	fog_instance_destroy = (void *)so_symbol(&yoyoloader_mod, "_Z22YYGML_instance_destroyP9CInstanceS0_iPP8YYRValue");
	var_draw_alpha = (int *)so_symbol(&yoyoloader_mod, "g_VAR_draw_alpha");
	uintptr_t create = so_symbol(&yoyoloader_mod, "_Z27gml_Object_obj_fog_Create_0P9CInstanceS0_");
	uintptr_t step = so_symbol(&yoyoloader_mod, "_Z25gml_Object_obj_fog_Step_0P9CInstanceS0_");
	if (fog_instance_destroy && var_draw_alpha && create && step && (orig_fog_step = entry_hook(step, fog_step)))
		orig_fog_create = entry_hook(create, fog_create);
	debugPrintf("fog scale %d: %s\n", fog_scale, orig_fog_create && orig_fog_step ? "installed" : "NOT installed");
	if (orig_fog_create && orig_fog_step) {
		extern void fog_skip_install(void);
		fog_skip_install(); // don't create the puffs this would discard (fog_skip.c)
	}
}

// The runner's frame limiter (Timing_Wait -> Timing_Sleep(us, spin)) sets a deadline now + us, usleep()s most of
// it and then busy-spins on clock_gettime until the deadline. With vblank pacing (vsync_pace, main.c) the loader
// already skips the usleep, so the whole wait became a spin: on the title ~7.8 ms of CPU per frame, at the start of
// the frame, so the frame's work started late and any frame with a garbage collection (5-7.5 ms) missed the vblank
// (a hitch about once a second on the title). Frame-sized waits now return at once and the vsynced swap paces the
// game, as intended. exp_sleeppace keeps the original limiter (sleep + spin).
static void (*orig_timing_sleep)(int64_t us, int spin);
unsigned timing_sleep_skipped;
static void timing_sleep_vsync(int64_t us, int spin) {
	extern int vsync_pace;
	extern SceUID game_thread_id;
	if (vsync_pace && us > 0 && us <= 17000 && sceKernelGetThreadId() == game_thread_id) {
		timing_sleep_skipped++;
		return;
	}
	orig_timing_sleep(us, spin);
}

// Hooks that need the runner initialised (function registry, g_FUNC_* slots): called once
// right before the main loop
// Audio voice limit (maxVoices in yyl.cfg, default 32; 0 keeps the runner's 128): a safety net behind the
// same-sound limit (sound_limit.c). Too many voices make one mixer pass hold the audio lock that every
// audio_* call on the game thread needs. The game never calls audio_channel_num itself; this applies it
// once, like that builtin does.
int max_voices = 32;

void katana_late_install(void) {
	static int done;
	if (done++)
		return;
	if (max_voices > 0) {
		int (*set_voices)(int) = (void *)so_symbol(&yoyoloader_mod, "_Z24Audio_EngineSetNumVoicesi");
		if (set_voices) {
			set_voices(max_voices);
			debugPrintf("audio voices: %d\n", max_voices);
		}
	}
	extern void sound_limit_install(void);
	sound_limit_install();
	extern void gcalloc_install(void);
	gcalloc_install();
	dissolve_install();
	fog_install();
	if (!exp_flag("dossiernative0")) {
		extern int dossier_native_install(void *(*hook)(uintptr_t entry, void *replacement));
		debugPrintf("native dossier burn steps: %s\n", dossier_native_install(entry_hook) ? "installed" : "NOT installed");
	}
	extern int letter_timing;
	letter_timing = exp_flag("lettertime");
	if (!exp_flag("wavynative0")) {
		extern int wavy_native_install(void *(*hook)(uintptr_t entry, void *replacement));
		debugPrintf("native draw_sprite_wavy: %s\n", wavy_native_install(entry_hook) ? "installed" : "NOT installed");
	}
	if (!exp_flag("shadownative0")) {
		extern int shadow_native_install(void *(*hook)(uintptr_t entry, void *replacement));
		debugPrintf("native draw_shadow: %s\n", shadow_native_install(entry_hook) ? "installed" : "NOT installed");
	}
	appsurf_copy_install();
	if (orig_lighting_user0) {
		runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
		int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
		for (int i = 0; functions && *functions && count && i < *count; i++) {
			runner_function *f = &(*functions)[i];
			if (f->name && !strcmp(f->name, "layer_tilemap_create")) {
				orig_layer_tilemap_create = f->func;
				f->func = layer_tilemap_create_fixed;
				debugPrintf("lighting tilemap size fix: installed\n");
			}
		}
	}
	if (!exp_flag("inputnative0")) {
		extern int input_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native controller_check_pressed_ext: %s\n", input_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Exact native controller_check (held input, held_native.c): 2-4 calls per frame in every gameplay room at
	// ~145 us each (getters over the on-screen keyboard's key list). exp_heldnative0: the original script.
	if (!exp_flag("heldnative0")) {
		extern int held_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native controller_check: %s\n", held_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	if (!exp_flag("vcapture0")) {
		extern int virtual_capture_install(void *(*rx_alloc)(size_t));
		extern int virtual_capture_relaxed;
		virtual_capture_relaxed = !exp_flag("vcapexact");
		debugPrintf("virtual enemy captures: %s\n", virtual_capture_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	extern int rope_lights;
	if (!rope_lights) {
		runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
		int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
		for (int i = 0; functions && *functions && count && i < *count; i++) {
			runner_function *f = &(*functions)[i];
			if (f->name && !strcmp(f->name, "instance_exists")) {
				orig_instance_exists = f->func;
				f->func = cloth_instance_exists;
				debugPrintf("clothesline without light tint: installed\n");
				break;
			}
		}
	}
}

// Decodes a small LZ4 payload through the exact call ReadQOIFFile makes for "2zoq" pages
// (its bl at +0xdc), so a runner build that bypasses our hook fails here, before any texture
// is drawn.
void katana_texture_selftest(void) {
	uintptr_t read_qoif = so_symbol(&yoyoloader_mod, "_Z12ReadQOIFFilePviPiS0_b");
	uintptr_t text = yoyoloader_mod.text_base;
	size_t size = yoyoloader_mod.text_size;
	uint32_t insn = 0;
	if (!(read_qoif & 3) && read_qoif >= text && size >= 0xe0 && read_qoif - text <= size - 0xe0)
		insn = *(uint32_t *)(read_qoif + 0xdc);
	if ((insn & 0xff000000) != 0xeb000000) {
		fatal_error("Texture decoder self-test: unexpected ReadQOIFFile layout (0x%08X); not starting to avoid a GPU crash.", insn);
	}
	int32_t imm = (int32_t)(insn << 8) >> 6;
	uintptr_t target = read_qoif + 0xdc + 8 + imm;
	if (target < text || target - text > size - 4)
		fatal_error("Texture decoder self-test: call target outside runner text; refusing to start.");
	int (*decompress)(char *, unsigned *, char *, unsigned, int, int) = (void *)target;

	static const char plain[] = "Katana ZERO texture self-test payload, Katana ZERO texture self-test payload";
	// ReadQOIFFile includes the 12-byte page header in the decoder's input length.
	char packed[8 + LZ4_COMPRESSBOUND(sizeof(plain)) + 12] = {0}, out[sizeof(plain)];
	int block = LZ4_compress_default(plain, packed + 8, sizeof(plain), LZ4_COMPRESSBOUND(sizeof(plain)));
	if (block <= 0)
		fatal_error("Texture decoder self-test: could not prepare LZ4 payload.");
	memcpy(packed, "LZ4H", 4);
	memcpy(packed + 4, &block, 4);
	unsigned out_len = sizeof(out);
	int r = decompress(out, &out_len, packed, 8 + block + 12, 1, 0);
	if (r != 0 || out_len != sizeof(plain) || memcmp(out, plain, sizeof(plain)))
		fatal_error("Texture decoder self-test failed (%d). The runner bypasses the LZ4 hook; not starting to avoid a GPU crash.", r);
	debugPrintf("texture self-test passed\n");
}

// The mobile on-screen keyboard (virtual stick/buttons) isn't needed
// when playing with the Vita's buttons, and its Draw event costs ~1.25 ms per frame
static void touch_ui_draw_skip(void *self, void *other) {
}

// The mobile on-screen buttons (obj_screenBtn) keep their GUI Draw event, which also runs button logic (layout, hit
// areas, blink and press state, instance_create, random), but must stay invisible. The event's only drawing is seven
// draw_sprite_ext calls (no draw state changes), so those calls become NOPs: the call returns nothing and every
// register it clobbers is reloaded after it. Saves building and submitting the sprites plus two Graphics::Flush per
// button (screenBtn GUI 0.33 ms per tick with 9 buttons). If a call site doesn't match, the sprites are drawn with
// colour writes off instead, flushed around each button.
static event_func orig_screen_btn_gui;
static void (*graphics_flush)(void);

static void screen_btn_gui_hidden(void *self, void *other) {
	graphics_flush();
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	orig_screen_btn_gui(self, other);
	graphics_flush();
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static int screen_btn_nodraw_install(void) {
	// ELF addresses in APK 1.10: the event, its draw_sprite_ext call sites and the PLT entry they call
	enum { EVENT = 0x48742b0, PLT_DRAW_SPRITE_EXT = 0x4ab0e90 };
	static const uint32_t sites[] = {0x4876f1c, 0x48777f0, 0x4879de4, 0x487a1c8, 0x487a368, 0x487a780, 0x487ac14};
	static const uint32_t nop = 0xe320f000;
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z32gml_Object_obj_screenBtn_Draw_64P9CInstanceS0_");
	uintptr_t draw = so_symbol(&yoyoloader_mod, "_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
	if (!entry || !draw || ((uint32_t *)entry)[0] != 0xe92d4ff0 || ((uint32_t *)entry)[1] != 0xe28db01c)
		return 0;
	for (unsigned i = 0; i < sizeof(sites) / sizeof(*sites); i++) {
		uintptr_t at = entry + sites[i] - EVENT;
		uint32_t ins = *(uint32_t *)at;
		uintptr_t target = at + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
		if ((ins >> 24) != 0xeb || (target != entry + PLT_DRAW_SPRITE_EXT - EVENT && target != draw))
			return 0;
	}
	for (unsigned i = 0; i < sizeof(sites) / sizeof(*sites); i++) {
		uintptr_t at = entry + sites[i] - EVENT;
		kuKernelCpuUnrestrictedMemcpy((void *)at, &nop, 4);
		kuKernelFlushCaches((void *)at, 4);
	}
	return 1;
}

/*
 * Rain thinning (rain=0|1 in yyl.cfg): obj_rain drops are destroyed right after their Create event (all of them,
 * or every second one), so random number use is unchanged and obj_rain has no Destroy event to trigger.
 */
static event_func orig_rain_create;
static void (*instance_destroy_fn)(void *self, void *other, int argc, void **args);

static int rain_keep_none;

static void rain_create(void *self, void *other) {
	static uint32_t created;
	orig_rain_create(self, other);
	if (rain_keep_none || (created++ & 1))
		instance_destroy_fn(self, other, 0, NULL);
}

// Replays are off by default (replays=1 in yyl.cfg restores them): the game's own replay option
// (global.replay_enabled, also in the graphics options menu) is forced off before the recorder's
// End Step, which then skips all snapshot recording. No VHS replay / death rewind, but no ~15 MB/min
// heap growth (it runs the Vita out of memory after a few minutes in a room) and ~2 fps more in
// heavy rooms.
static event_func orig_record_end_step;
static int *var_replay_enabled;

// The bunker chapter ends with a replay of its levels: in room_bunker_10_2 the replay master's (persistent through the
// chapter) user event 0 starts it (room_bunker_replay, then the recorded rooms), and room_stage_results follows. With
// nothing recorded its playback (user event 2) read past the end of an empty recording and the game crashed. Without
// replays the chapter ends like every other level: the replay master goes and the game moves on to the results.
// exp_bunkerreplay keeps the original (which then needs replays=1 to have a recording).
static event_func orig_replaymaster_user0;
static void replaymaster_user0_noreplay(void *self, void *other) {
	int (*room_find)(const char *) = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
	void (*room_goto)(gml_rvalue *, void *, void *, int, gml_rvalue *) =
		(void *)so_symbol(&yoyoloader_mod, "_Z10F_RoomGotoR6RValueP9CInstanceS2_iPS_");
	int here = room_find ? room_find("room_bunker_10_2") : -1;
	int results = room_find ? room_find("room_stage_results") : -1;
	if (here < 0 || results < 0 || !current_room || *current_room != here || !room_goto || !instance_destroy_fn) {
		orig_replaymaster_user0(self, other);
		return;
	}
	debugPrintf("bunker replay: skipped (replays off), to room_stage_results\n");
	instance_destroy_fn(self, other, 0, NULL);
	gml_rvalue result = {0}, arg = {0};
	arg.val = results;
	room_goto(&result, self, other, 1, &arg);
}

// exp_bunkertrace (debugMode): logs the replay master's Room End and user events 0-2 with the room they run in
static event_func replaymaster_ev[4];
static void replaymaster_trace(int ev, const char *name, void *self, void *other) {
	static unsigned lines;
	const char *(*room_name)(int) = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_Namei");
	if (lines++ < 400 || ev != 3)
		debugPrintf("[BUNKERTRACE] tick %u room %s: %s\n", *frame_count,
			room_name && current_room ? room_name(*current_room) : "?", name);
	replaymaster_ev[ev](self, other);
}
static void replaymaster_trace4(void *s, void *o) { replaymaster_trace(0, "Room End", s, o); }
static void replaymaster_trace10(void *s, void *o) { replaymaster_trace(1, "user 0", s, o); }
static void replaymaster_trace11(void *s, void *o) { replaymaster_trace(2, "user 1", s, o); }
static void replaymaster_trace12(void *s, void *o) { replaymaster_trace(3, "user 2", s, o); }

static void record_end_step_noreplay(void *self, void *other) {
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)*global_obj_ptr)[3];
	gml_rvalue *v = vwrite(*global_obj_ptr, var_replay_enabled[1]);
	if (v && (v->kind == 0 || v->kind == 13)) {
		v->val = 0.0;
		v->kind = 0;
	}
	orig_record_end_step(self, other);
}

// With nothing recorded, the end-of-level VHS replay and the death rewind would read empty recorder
// arrays (obj_record Draw: "trying to index variable that is not an array", then a crash at level
// completion). Use the game's own skips instead: the speedrun "skip replay" path in obj_level_complete
// Create (taken when global.speedrun && global.speedrun_skip_replay: removes the cursor, commits queued
// story flags, room_transition to the next room) and the "skip rewind" option for obj_record Draw. The
// globals are only changed while those events run and restored afterwards, so options/saves keep the
// player's values.
static event_func orig_level_complete_create, orig_record_draw;
static int *var_speedrun, *var_speedrun_skip_replay, *var_skip_rewind;

gml_rvalue *global_var(int *var) {
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)*global_obj_ptr)[3];
	gml_rvalue *v = var ? vwrite(*global_obj_ptr, var[1]) : NULL;
	return v && (v->kind == 0 || v->kind == 13) ? v : NULL;
}

static void level_complete_create_noreplay(void *self, void *other) {
	gml_rvalue *a = global_var(var_speedrun), *b = global_var(var_speedrun_skip_replay);
	gml_rvalue sa, sb;
	if (a && b) {
		sa = *a;
		sb = *b;
		a->val = b->val = 1.0;
	}
	orig_level_complete_create(self, other);
	if (a && b) {
		// Re-fetch: the event may have reallocated the global variable table
		gml_rvalue *a2 = global_var(var_speedrun), *b2 = global_var(var_speedrun_skip_replay);
		if (a2)
			*a2 = sa;
		if (b2)
			*b2 = sb;
	}
}

static void record_draw_noreplay(void *self, void *other) {
	gml_rvalue *r = global_var(var_skip_rewind);
	gml_rvalue sr;
	if (r) {
		sr = *r;
		r->val = 1.0;
	}
	orig_record_draw(self, other);
	if (r && (r = global_var(var_skip_rewind)))
		*r = sr;
}

event_func gml_replace(gml_func_entry *table, const char *name, event_func wrapper) {
	for (int i = 0; i < GML_FUNCS_SIZE / sizeof(gml_func_entry); i++) {
		if (table[i].name && !strcmp(table[i].name, name)) {
			event_func orig = table[i].func;
			table[i].func = wrapper;
			return orig;
		}
	}
	debugPrintf("Katana: %s not found in g_GMLFuncs\n", name);
	return NULL;
}

static void patch_performance() {
	gml_func_entry *table = (gml_func_entry *)so_symbol(&yoyoloader_mod, "g_GMLFuncs");
	frame_count = (uint32_t *)so_symbol(&yoyoloader_mod, "g_IOFrameCount");
	global_obj_ptr = (void **)so_symbol(&yoyoloader_mod, "g_pGlobal");
	var_recolor_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_recolor_timer");
	if (!table || !frame_count)
		return;

	event_func orig;
	var_xview = (int *)so_symbol(&yoyoloader_mod, "g_VAR_xview");
	var_yview = (int *)so_symbol(&yoyoloader_mod, "g_VAR_yview");
	var_wview = (int *)so_symbol(&yoyoloader_mod, "g_VAR_wview");
	var_hview = (int *)so_symbol(&yoyoloader_mod, "g_VAR_hview");
	run_room = (void **)so_symbol(&yoyoloader_mod, "Run_Room");
	current_room = (int *)so_symbol(&yoyoloader_mod, "Current_Room");
	cloth_cull = var_xview && var_yview && var_wview && var_hview && global_obj_ptr && run_room && frame_count &&
		!exp_flag("clothall");
	if ((orig = gml_replace(table, "gml_Object_obj_clothesline_Draw_0", clothesline_draw)))
		orig_clothesline_draw = orig;
	if ((orig = gml_replace(table, "gml_Object_obj_clothesline_Step_2", clothesline_end_step)))
		orig_clothesline_end_step = orig;
	if ((orig = gml_replace(table, "gml_Object_obj_clothesline_custom_Draw_0", clothesline_custom_draw)))
		orig_clothesline_custom_draw = orig;
	if ((orig = gml_replace(table, "gml_Object_obj_clothesline_custom_Step_2", clothesline_custom_end_step)))
		orig_clothesline_custom_end_step = orig;
	if ((orig = gml_replace(table, "gml_Object_obj_softbody_node_Step_0", softbody_node_step)))
		orig_softbody_node_step = orig;
	if ((orig = gml_replace(table, "gml_Object_obj_rain_Draw_0", rain_draw)))
		orig_rain_draw = orig;
	if ((orig = gml_replace(table, "gml_Object_obj_particle_Other_25", particle_draw))) {
		// In the fog rooms the roll's dust clouds skip their light tint (fogTrailLights=1 keeps it; fog_trail.c)
		extern event_func fog_trail_install(event_func orig);
		orig_particle_draw = fog_trail_install(orig);
	}

	instance_destroy_fn = (void *)so_symbol(&yoyoloader_mod, "_Z22YYGML_instance_destroyP9CInstanceS0_iPP8YYRValue");
	// rain in yyl.cfg: 0 removes rain entirely (default: first level 39 -> 47 fps), 1 keeps every
	// second drop, 2 keeps all of them
	extern int rain_level;
	rain_keep_none = rain_level <= 0;
	if (rain_level < 2 && instance_destroy_fn && (orig = gml_replace(table, "gml_Object_obj_rain_Create_0", rain_create)))
		orig_rain_create = orig;

	// Skipped by default (buttons are used on Vita); exp_touchui brings the virtual controls back
	if (!exp_flag("touchui"))
		gml_replace(table, "gml_Object_obj_onScreenKeyboard_Draw_0", touch_ui_draw_skip);
	graphics_flush = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics5FlushEv");
	if (!exp_flag("touchui")) {
		int nodraw = !exp_flag("btnnodraw0") && screen_btn_nodraw_install();
		if (!nodraw && graphics_flush && (orig = gml_replace(table, "gml_Object_obj_screenBtn_Draw_64", screen_btn_gui_hidden)))
			orig_screen_btn_gui = orig;
		debugPrintf("on-screen buttons: %s\n", nodraw ? "sprites not drawn" : "drawn with colour writes off");
	}
	katana_menu_install(table);

	katana_display_install();
	var_replay_enabled = (int *)so_symbol(&yoyoloader_mod, "g_VAR_replay_enabled");
	extern int replays;
	if (!replays && var_replay_enabled && global_obj_ptr &&
		(orig = gml_replace(table, "gml_Object_obj_record_Step_2", record_end_step_noreplay)))
		orig_record_end_step = orig;
	var_speedrun = (int *)so_symbol(&yoyoloader_mod, "g_VAR_speedrun");
	var_speedrun_skip_replay = (int *)so_symbol(&yoyoloader_mod, "g_VAR_speedrun_skip_replay");
	var_skip_rewind = (int *)so_symbol(&yoyoloader_mod, "g_VAR_skip_rewind");
	if (!replays && orig_record_end_step) {
		if (var_speedrun && var_speedrun_skip_replay &&
			(orig = gml_replace(table, "gml_Object_obj_level_complete_Create_0", level_complete_create_noreplay)))
			orig_level_complete_create = orig;
		if (var_skip_rewind && (orig = gml_replace(table, "gml_Object_obj_record_Draw_0", record_draw_noreplay)))
			orig_record_draw = orig;
		if (debugMode && exp_flag("bunkertrace")) {
			replaymaster_ev[0] = gml_replace(table, "gml_Object_obj_bunker_replaymaster_Other_4", replaymaster_trace4);
			replaymaster_ev[1] = gml_replace(table, "gml_Object_obj_bunker_replaymaster_Other_10", replaymaster_trace10);
			replaymaster_ev[2] = gml_replace(table, "gml_Object_obj_bunker_replaymaster_Other_11", replaymaster_trace11);
			replaymaster_ev[3] = gml_replace(table, "gml_Object_obj_bunker_replaymaster_Other_12", replaymaster_trace12);
		}
		if (!exp_flag("bunkerreplay") && (orig = gml_replace(table, "gml_Object_obj_bunker_replaymaster_Other_10",
				replaymaster_user0_noreplay)))
			orig_replaymaster_user0 = orig;
		debugPrintf("replays: off (bunker chapter replay %s)\n", orig_replaymaster_user0 ? "skipped" : "kept");
	}

	// Fast native rope solver: Unicorn-verified against the original ARM code,
	// ~100% fast-path hits on hardware; rope active 47.4 -> 50 fps, min 43 -> 48.5. exp_ropefast0 disables.
	if (!exp_flag("ropefast0")) {
		extern int rope_fast_install(void *rx, size_t rx_size);
		void *rx = katana_rx_alloc(16);
		debugPrintf("fast rope solver: %s\n", rx && rope_fast_install(rx, 16) ? "installed" : "NOT installed");
	}
	// Native bounding-box fast path: ~0.7 ms/frame in the bunker room; checked bit-for-bit against the original on
	// ~670k real calls on hardware (0 mismatches). exp_nativebbox0 disables.
	if (!exp_flag("nativebbox0")) {
		extern int collision_install(void *rx, size_t rx_size);
		void *rx = katana_rx_alloc(16);
		debugPrintf("native bbox: %s\n", rx && collision_install(rx, 16) ? "installed" : "NOT installed");
	}

	// Texture pages repacked by prepare_apk.py as LZ4 inside the runner's "2zoq" container
	install_lz4_pages();
	install_nursery_align();
	if (!exp_flag("letternative0")) {
		extern int letter_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native dialogue letters: %s\n", letter_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Exact natives of the view tests inside_screen_point / inside_screen (visibility_native.c): the runner's own
	// global getters and comparisons without the GML temporaries. In-run A/B (exp_visab), process per frame:
	// haunted_cart2 -0.68 ms (450 + 57 calls), club_4 with smoke -0.43 ms, bunker2_3 -0.23 ms.
	// exp_visibilitynative0: the original scripts.
	if (!exp_flag("visibilitynative0")) {
		extern int visibility_native_install(void *(*rx_alloc)(size_t));
		int r = visibility_native_install(katana_rx_alloc);
		debugPrintf("native inside_screen_point %s, inside_screen %s\n", r & 1 ? "installed" : "NOT installed",
			r & 2 ? "installed" : "NOT installed");
	}
	if (!exp_flag("minecartnative0")) { // minecart master and its rail helpers
		extern int minecart_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native minecart master: %s\n", minecart_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	if (!exp_flag("flashmask0")) {
		extern int flash_mask_install(void *(*rx_alloc)(size_t));
		debugPrintf("flashlight mask culling: %s\n", flash_mask_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Bike chase: each actor's particle and trail with-loops step over other actors' instances (exact; bike_draw.c)
	if (!exp_flag("bikedraw0")) {
		extern int bike_draw_install(void);
		debugPrintf("bike draw loops: %s\n", bike_draw_install() ? "installed" : "NOT installed");
	}
	// update_region natively (exact; region_native.c): moving particles and every explosion particle call it from their
	// draw event, 120-270 calls per frame in the bike chase's helicopter volleys at ~21 us each
	if (!exp_flag("regionnative0")) {
		extern int region_native_install(void *(*hook)(uintptr_t entry, void *replacement));
		debugPrintf("native update_region: %s\n", region_native_install(entry_hook) ? "installed" : "NOT installed");
	}
	// Explosion particles skip update_light_array in their inherited Create: they never read the light list (exact;
	// particle_lights.c). Bike chase volleys: 60-90 particles per explosion frame, ~40 us each.
	if (!exp_flag("particlelights0")) {
		extern int particle_lights_install(void *(*hook)(uintptr_t entry, void *replacement));
		debugPrintf("explosion particle lights: %s\n", particle_lights_install(entry_hook) ? "installed" : "NOT installed");
	}
	// Bike chase helicopter fight: the rockets rising in the background (obj_heli_mortar_far) are drawn unlit; their
	// light list, light tint and rim shading cost 15-37 ms of render-target flushes per frame while 15-25 are on
	// screen (far_rocket.c). farRocketLights=1 keeps them lit.
	extern int far_rocket_lights;
	if (!far_rocket_lights) {
		extern int far_rocket_install(void);
		debugPrintf("far rockets unlit: %s\n", far_rocket_install() ? "installed" : "NOT installed");
	}
	// update_light_array's loop body runs natively per light (exact; light_array_native.c): particles' Create events
	if (!exp_flag("lightarray0")) {
		extern int light_array_install(void *(*rx_alloc)(size_t));
		debugPrintf("native update_light_array: %s\n", light_array_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// The player's afterimage trails tint themselves from nearby lights: far lights skipped, the tint drawn natively
	// (exact; trail_lights.c). club_7 while running: 1.9 -> 0.8 ms per frame.
	if (!exp_flag("traillights0")) {
		extern int trail_lights_install(void);
		debugPrintf("trail light culling: %s\n", trail_lights_install() ? "installed" : "NOT installed");
	}
	// Lit sprites (particles, the roll's dust clouds, actors) recompute their tint from their nearby lights in
	// draw_sprite_recolored's with loop whenever global.recolor_timer moves: the loop body runs natively (exact;
	// recolor_lights.c)
	if (!exp_flag("recolorlights0")) {
		extern int recolor_lights_install(void *(*rx_alloc)(size_t));
		debugPrintf("recolor light loop: %s\n", recolor_lights_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Club dancers' rim glow (obj_clubber user event 6): each dancer re-tints itself from every nearby club light
	// with 2-4 outline sprites per light, each drawn twice: ~285 extra draws per frame in the club, 44 fps vs
	// 52-56 without it. dancerGlow=0 (default) skips the event; the club light show itself stays. dancerGlow=1
	// keeps it, with an exact native version of the event (exp_clubbernative0: the original GML).
	extern int dancer_glow;
	if (!dancer_glow) {
		static const uint32_t bx_lr = 0xe12fff1e;
		uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z31gml_Object_obj_clubber_Other_16P9CInstanceS0_");
		if (entry) {
			kuKernelCpuUnrestrictedMemcpy((void *)entry, &bx_lr, 4);
			kuKernelFlushCaches((void *)entry, 4);
		}
		debugPrintf("dancer rim glow: off\n");
	} else if (!exp_flag("clubbernative0")) {
		extern int clubber_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native dancer rim glow: %s\n", clubber_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Native ceiling-laser beam draw and height probe (laser_native.c). The draw's fast path builds no temporary
	// array: bunker2_2 with 6 beams on screen, frames over 18 ms 230/min -> 1.4/min (GC passes), process 15.2 ->
	// 14.2 ms. exp_laserfast0 keeps the original array; exp_lasernative0 the original GML.
	if (!exp_flag("lasernative0")) {
		extern int laser_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native lasers: %s\n", laser_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Native smoke cloud Draw (smokedraw_native.c): the event with its variable plumbing inlined, every helper, setter
	// and draw call kept in order (ARM differential fixture work/smokedraw/). exp_smokedraw0 keeps the original GML.
	if (!exp_flag("smokedraw0")) {
		extern int smokedraw_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native smoke cloud Draw: %s\n", smokedraw_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Exact natives of obj_orangelight Step and obj_lightback Step, from the static recompiler kill test
	// (work/cpu_research/kill_test/REPORT.md): the events with the runner's compare/truth/variable helpers inlined
	// (light_fast_inline.c). room_haunted_cart2 (129 lights): orangelight 2.13 -> 1.53 ms, lightback 0.51 -> 0.42 ms.
	// Calls it can't take (~6%) run the original event. exp_lightfast0: the original GML; exp_lightfast: the older
	// hand native (light_fast.c) instead. Both hook the same two entries, so only one is installed.
	if (exp_flag("lightfast")) {
		extern int light_fast_install(void *(*rx_alloc)(size_t));
		light_fast_mode = light_fast_install(katana_rx_alloc) ? 1 : 0;
		debugPrintf("light fast: %s (hand)\n", light_fast_mode ? "installed" : "NOT installed");
	} else if (!exp_flag("lightfast0")) {
		extern int light_fast_inline_install(void *(*rx_alloc)(size_t));
		light_fast_mode = light_fast_inline_install(katana_rx_alloc) ? 2 : 0;
		debugPrintf("light fast: %s (inline)\n", light_fast_mode ? "installed" : "NOT installed");
	}
	// Exact native of the off-screen path of obj_lightback user event 0, the lighting pass's per-light event
	// (lightback_native.c): visible lights run the original. In-run A/B (exp_lbab): haunted_cart2 (111 of 129
	// lights off-screen) -0.44 ms process per frame. exp_lightbacknative0: the original for every light.
	if (!exp_flag("lightbacknative0")) {
		extern int lightback_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native off-screen lightback: %s\n", lightback_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Cathedral dust (obj_cathedral_dustspawner): every visible spawner redraws all ~208 particles through
	// event_user(0), ~7.4 ms per frame: room_cathedral runs at 41 fps with it, 59 without. cathedralDust=0 (default)
	// stubs the spawner's Step (creates the particles) and Draw; the window light itself stays. cathedralDust=1
	// keeps it, with an exact native version of the Draw loop (46 fps; exp_dustnative0: the original GML).
	extern int cathedral_dust;
	if (!cathedral_dust) {
		static const uint32_t bx_lr = 0xe12fff1e;
		static const char *const events[] = {"_Z43gml_Object_obj_cathedral_dustspawner_Step_0P9CInstanceS0_",
			"_Z43gml_Object_obj_cathedral_dustspawner_Draw_0P9CInstanceS0_"};
		for (int i = 0; i < 2; i++) {
			uintptr_t entry = so_symbol(&yoyoloader_mod, events[i]);
			if (entry) {
				kuKernelCpuUnrestrictedMemcpy((void *)entry, &bx_lr, 4);
				kuKernelFlushCaches((void *)entry, 4);
			}
			debugPrintf("cathedral dust: off, %s %s\n", events[i], entry ? "stubbed" : "NOT found");
		}
	} else if (!exp_flag("dustnative0")) {
		extern int dustspawner_loop_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native cathedral dust: %s\n", dustspawner_loop_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Debug: ux0:data/gms/shared/stub_events lists object events (obj_name_Draw_0, one per line) that return at
	// once, to measure what an event costs
	FILE *stubs = debugMode ? fopen("ux0:data/gms/shared/stub_events", "r") : NULL;
	if (stubs) {
		static const uint32_t bx_lr = 0xe12fff1e;
		char line[96], symbol[160];
		while (fgets(line, sizeof(line), stubs)) {
			line[strcspn(line, "\r\n ")] = 0;
			if (!line[0])
				continue;
			snprintf(symbol, sizeof(symbol), "_Z%dgml_Object_%sP9CInstanceS0_", (int)strlen(line) + 11, line);
			uintptr_t entry = so_symbol(&yoyoloader_mod, symbol);
			if (entry) {
				kuKernelCpuUnrestrictedMemcpy((void *)entry, &bx_lr, 4);
				kuKernelFlushCaches((void *)entry, 4);
			}
			debugPrintf("stub_events: %s %s\n", line, entry ? "stubbed" : "NOT found");
		}
		fclose(stubs);
	}
	if (skip_netflix_logo) {
		if ((orig = gml_replace(table, "gml_Object_obj_netflixSDKScreen_Create_0", netflix_screen_create)))
			orig_netflix_screen_create = orig;
		if ((orig = gml_replace(table, "gml_Object_obj_netflixSDKScreen_Draw_64", netflix_screen_draw)))
			orig_netflix_screen_draw = orig;
		debugPrintf("Netflix logo skip: %s\n", orig_netflix_screen_create ? "installed" : "NOT installed");
	}
	if (!exp_flag("lightmapbig") && (orig = gml_replace(table, "gml_Object_obj_new_platform_lighting_Other_10", lighting_user0)))
		orig_lighting_user0 = orig;

	orig_timing_sleep = entry_hook(so_symbol(&yoyoloader_mod, "Timing_Sleep"), timing_sleep_vsync);
	debugPrintf("frame limiter spin: %s\n", orig_timing_sleep ? "skipped with vblank pacing" : "NOT hooked");

	// Whatever yyl.cfg says: the GRAPHICS menu can turn 30 fps mode on, and it needs the time doubling then
	t30_install(table);
	fps_menu_install(table);
	extern void fx_rate_install(gml_func_entry *table);
	fx_rate_install(table); // after t30_install: uses its animation_code hook
	extern void glow_rest_install(gml_func_entry *table);
	glow_rest_install(table);
	extern void wobble_rest_install(gml_func_entry *table);
	wobble_rest_install(table);
	extern void blood_drops_install(gml_func_entry *table);
	blood_drops_install(table);

	// Frame skipping (and its guard stubs) only when opted in with frameSkip=1
	uintptr_t *draw_slot = frame_skip ? so_got_slot(&yoyoloader_mod, "_Z12DoAStep_Drawv") : NULL;
	if (draw_slot) {
		install_draw_skip(table);
		frameskip_orig_draw = (void *)*draw_slot;
		*draw_slot = (uintptr_t)&DoAStep_Draw_frameskip;
	}

	katana_display_hook_draw();
}

void patch_katana() {
	patch_performance();

	SaveFileExists = (void *)so_symbol(&yoyoloader_mod, "_ZN8LoadSave14SaveFileExistsEPKcPv");
	BundleFileExists = (void *)so_symbol(&yoyoloader_mod, "_ZN8LoadSave16BundleFileExistsEPKc");
	uintptr_t file_exists = so_symbol(&yoyoloader_mod, "_Z10FileExistsPKc");
	if (file_exists && SaveFileExists && BundleFileExists)
		hook_addr(file_exists, (uintptr_t)&FileExists_hook);

	katana_prof_install();
}

/*
 * Measures the real ARM clock: times a chain of dependent adds at the current clock and
 * at a requested 333MHz, then restores the original setting. If an overclocking plugin
 * pins the clock both timings match.
 */
static uint32_t clock_probe_us(void) {
	uint32_t n = 4000000, x = 1;
	uint32_t start = sceKernelGetProcessTimeLow();
	__asm__ volatile(
		"1:\n\t"
		"add %1, %1, %1\n\t"
		"add %1, %1, %1\n\t"
		"add %1, %1, %1\n\t"
		"add %1, %1, %1\n\t"
		"subs %0, %0, #1\n\t"
		"bne 1b\n\t"
		: "+r"(n), "+r"(x) : : "cc");
	return sceKernelGetProcessTimeLow() - start;
}

void katana_clock_probe(void) {
	int reported = scePowerGetArmClockFrequency();
	uint32_t t_current = clock_probe_us();
	scePowerSetArmClockFrequency(333);
	uint32_t t_333 = clock_probe_us();
	scePowerSetArmClockFrequency(reported);
	uint32_t t_restored = clock_probe_us();
	debugPrintf("Clock probe: reported %d MHz | loop %u us now, %u us at 333MHz request, %u us restored -> ~%d MHz\n",
		reported, t_current, t_333, t_restored, t_current ? (int)(333.0 * t_333 / t_current) : 0);

	// Ask for 500MHz: works when an overclocking plugin (PSVshell) allows it
	int res = scePowerSetArmClockFrequency(500);
	uint32_t t_500 = clock_probe_us();
	debugPrintf("Clock probe: 500MHz request = 0x%08X, reported %d MHz, loop %u us -> ~%d MHz\n",
		res, scePowerGetArmClockFrequency(), t_500, t_500 ? (int)(333.0 * t_333 / t_500) : 0);
	if (t_500 >= t_restored)
		scePowerSetArmClockFrequency(reported);
}
