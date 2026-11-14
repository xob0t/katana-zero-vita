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

// GameMaker RValue (16 bytes): payload, flags, kind
typedef struct {
	double val;
	uint32_t flags;
	uint32_t kind;
} gml_rvalue;

extern int debugMode;

// Profiling state for global.recolor_timer sampling (see sample_recolor_timer)
static uint32_t recolor_changes, recolor_frames;
static double last_recolor_timer;
void callprof_install(so_module *mod);
void evprof_install(so_module *mod);

static int (*SaveFileExists)(const char *path, void *unk);
static int (*BundleFileExists)(const char *path);

// Same logic as the runner's FileExists, with logging to trace asset lookups
static int FileExists_hook(const char *path) {
	int in_save = SaveFileExists(path, NULL);
	int in_bundle = in_save ? 0 : BundleFileExists(path);
	debugPrintf("FileExists(%s): save=%d bundle=%d\n", path, in_save, in_bundle);
	return in_save || in_bundle;
}

/*
 * Debug-mode stage profiler: redirects the PLT slots of the runner's main loop stages
 * to timing wrappers. Times are inclusive (DoAStep contains DoAStep_Update, etc).
 */
enum {
	S_MAINLOOP,
	S_DOASTEP,
	S_UPDATE,
	S_DRAW,
	S_HANDLE_STEP,
	S_ACTIVE_LISTS,
	S_DRAW_ROOM,
	S_GFX_FLUSH,
	S_AUDIO,
	S_MESSAGES,
	S_GC,
	S_LAYER_TILEMAP,
	S_LAYER_INSTANCE,
	S_LAYER_BACKGROUND,
	S_LAYER_SPRITE,
	S_LAYER_TILE,
	S_COUNT
};

static const char *stage_names[S_COUNT] = {
	"MainLoop_Process", "DoAStep", "DoAStep_Update", "DoAStep_Draw", "HandleStep",
	"UpdateActiveLists", "Draw_Room", "Graphics::Flush", "Audio_Tick", "ProcessMessages",
	"MarkAndSweepGen", "DrawLayerTilemap", "DrawLayerInstance", "DrawLayerBackground", "DrawLayerSprite",
	"DrawLayerTile"
};

static uint32_t stage_time[S_COUNT];
static uint32_t stage_calls[S_COUNT];

#define STAGE_BEGIN uint32_t stage_start = sceKernelGetProcessTimeLow()
#define STAGE_END(id) stage_time[id] += sceKernelGetProcessTimeLow() - stage_start; stage_calls[id]++

#define STAGE_VOID(id, name) \
	static void (*orig_##name)(void); \
	static void prof_##name(void) { STAGE_BEGIN; orig_##name(); STAGE_END(id); }

STAGE_VOID(S_MAINLOOP, MainLoop_Process)
STAGE_VOID(S_DOASTEP, DoAStep)
STAGE_VOID(S_UPDATE, DoAStep_Update)
STAGE_VOID(S_DRAW, DoAStep_Draw)
STAGE_VOID(S_ACTIVE_LISTS, UpdateActiveLists)
STAGE_VOID(S_DRAW_ROOM, Draw_Room)
STAGE_VOID(S_GFX_FLUSH, Graphics_Flush)
STAGE_VOID(S_AUDIO, Audio_Tick)
STAGE_VOID(S_MESSAGES, ProcessMessages)

static void (*orig_HandleStep)(int type);
static void prof_HandleStep(int type) { STAGE_BEGIN; orig_HandleStep(type); STAGE_END(S_HANDLE_STEP); }

// Room layer element drawing (inside Draw_Room); up to six argument words, floats passed as words (softfp)
#define STAGE_LAYER(id, name) 	static void (*orig_##name)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); 	static void prof_##name(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) { 		STAGE_BEGIN; orig_##name(a, b, c, d, e, f); STAGE_END(id); }
STAGE_LAYER(S_LAYER_TILEMAP, DrawLayerTilemap)
// Per-object breakdown of the instance draws (heavy profiling only)
#define LAYER_OBJ_MAX 1500
static uint32_t layer_obj_time[LAYER_OBJ_MAX], layer_obj_calls[LAYER_OBJ_MAX];
static void (*orig_DrawLayerInstance)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static void prof_DrawLayerInstance(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
	STAGE_BEGIN;
	orig_DrawLayerInstance(a, b, c, d, e, f);
	STAGE_END(S_LAYER_INSTANCE);
	char *inst = c ? *(char **)(c + 0x20) : NULL;
	int obj = inst ? *(int *)(inst + 0x84) : -1;
	if (obj >= 0 && obj < LAYER_OBJ_MAX) {
		layer_obj_time[obj] += sceKernelGetProcessTimeLow() - stage_start;
		layer_obj_calls[obj]++;
	}
}
STAGE_LAYER(S_LAYER_BACKGROUND, DrawLayerBackground)
STAGE_LAYER(S_LAYER_SPRITE, DrawLayerSprite)
STAGE_LAYER(S_LAYER_TILE, DrawLayerTile)

static int (*orig_MarkAndSweepGen)(int a, int b, int c);
static int prof_MarkAndSweepGen(int a, int b, int c) { STAGE_BEGIN; int r = orig_MarkAndSweepGen(a, b, c); STAGE_END(S_GC); return r; }

// Script timing: YYC scripts are called through the PLT with this signature
typedef void *(*gml_script_func)(void *self, void *other, void *result, int argc, void **args);

#define SCRIPT_PROF(idx, name) \
	static gml_script_func orig_script_##name; \
	static void *prof_script_##name(void *self, void *other, void *result, int argc, void **args) { \
		uint32_t start = sceKernelGetProcessTimeLow(); \
		void *r = orig_script_##name(self, other, result, argc, args); \
		script_time[idx] += sceKernelGetProcessTimeLow() - start; \
		script_calls[idx]++; \
		return r; \
	}

#define SCRIPT_PROF_NUM 6
static uint32_t script_time[SCRIPT_PROF_NUM], script_calls[SCRIPT_PROF_NUM];
static const char *script_names[SCRIPT_PROF_NUM] = {
	"draw_fastline_width_col", "softbody_constraint_strong", "block_meeting",
	"inside_screen", "update_light_array", "instance_create"
};
SCRIPT_PROF(0, draw_fastline)
SCRIPT_PROF(1, softbody_strong)
SCRIPT_PROF(2, block_meeting)
SCRIPT_PROF(3, inside_screen)
SCRIPT_PROF(4, update_light_array)
SCRIPT_PROF(5, instance_create)

// Generic timed pass-through for runner functions of up to 12 word-sized arguments
// (softfp passes floats in core registers/stack slots, so this forwards them unchanged)
typedef uint64_t (*passthru_func)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define PASSTHRU_MAX 32
static const char *passthru_names[PASSTHRU_MAX];
static passthru_func passthru_orig[PASSTHRU_MAX];
static uint32_t passthru_time[PASSTHRU_MAX], passthru_calls[PASSTHRU_MAX], passthru_max[PASSTHRU_MAX];
static int passthru_num = 0;

#define PASSTHRU_WRAPPER(i) 	static uint64_t passthru_##i(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint32_t j, uint32_t k, uint32_t l, uint32_t m) { 		uint32_t start = sceKernelGetProcessTimeLow(); 		uint64_t r = passthru_orig[i](a, b, c, d, e, f, g, h, j, k, l, m); 		uint32_t dt = sceKernelGetProcessTimeLow() - start; 		passthru_time[i] += dt; 		if (dt > passthru_max[i]) passthru_max[i] = dt; 		passthru_calls[i]++; 		return r; 	}
PASSTHRU_WRAPPER(0) PASSTHRU_WRAPPER(1) PASSTHRU_WRAPPER(2) PASSTHRU_WRAPPER(3)
PASSTHRU_WRAPPER(4) PASSTHRU_WRAPPER(5) PASSTHRU_WRAPPER(6) PASSTHRU_WRAPPER(7)
PASSTHRU_WRAPPER(8) PASSTHRU_WRAPPER(9) PASSTHRU_WRAPPER(10) PASSTHRU_WRAPPER(11)
PASSTHRU_WRAPPER(12) PASSTHRU_WRAPPER(13) PASSTHRU_WRAPPER(14) PASSTHRU_WRAPPER(15)
PASSTHRU_WRAPPER(16) PASSTHRU_WRAPPER(17) PASSTHRU_WRAPPER(18) PASSTHRU_WRAPPER(19)
PASSTHRU_WRAPPER(20) PASSTHRU_WRAPPER(21) PASSTHRU_WRAPPER(22) PASSTHRU_WRAPPER(23)
PASSTHRU_WRAPPER(24) PASSTHRU_WRAPPER(25) PASSTHRU_WRAPPER(26) PASSTHRU_WRAPPER(27)
PASSTHRU_WRAPPER(28) PASSTHRU_WRAPPER(29) PASSTHRU_WRAPPER(30) PASSTHRU_WRAPPER(31)
static void *passthru_wrappers[PASSTHRU_MAX] = {
	passthru_0, passthru_1, passthru_2, passthru_3, passthru_4, passthru_5,
	passthru_6, passthru_7, passthru_8, passthru_9, passthru_10, passthru_11,
	passthru_12, passthru_13, passthru_14, passthru_15, passthru_16, passthru_17,
	passthru_18, passthru_19, passthru_20, passthru_21, passthru_22, passthru_23,
	passthru_24, passthru_25, passthru_26, passthru_27, passthru_28, passthru_29,
	passthru_30, passthru_31
};

static void passthru_hook(const char *label, const char *symbol) {
	if (passthru_num >= PASSTHRU_MAX)
		return;
	uintptr_t *slot = so_got_slot(&yoyoloader_mod, symbol);
	if (!slot)
		return;
	int i = passthru_num++;
	passthru_names[i] = label;
	passthru_orig[i] = (passthru_func)*slot;
	*slot = (uintptr_t)passthru_wrappers[i];
}

static void *entry_hook(uintptr_t entry, void *replacement);

// Times the GML scripts listed (by name, whitespace separated) in ux0:data/gms/shared/scriptprof.txt.
// Hooked at their entries, so this works with direct-call runners too.
static void scriptprof_hooks(void) {
	static char names[1024];
	SceUID fd = sceIoOpen("ux0:data/gms/shared/scriptprof.txt", SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	int len = sceIoRead(fd, names, sizeof(names) - 1);
	sceIoClose(fd);
	names[len > 0 ? len : 0] = 0;
	char *save, *name = strtok_r(names, " \t\r\n", &save);
	while (name) {
		char sym[256];
		if (!strncmp(name, "_Z", 2)) // raw symbol (runner internals)
			snprintf(sym, sizeof(sym), "%s", name);
		else
			snprintf(sym, sizeof(sym), "_Z%dgml_Script_%sP9CInstanceS0_R8YYRValueiPPS1_", (int)strlen(name) + 11, name);
		// Entry hooks also see direct calls (runners from patch_plt_calls.py)
		if (passthru_num < PASSTHRU_MAX) {
			int i = passthru_num;
			void *tramp = entry_hook(so_symbol(&yoyoloader_mod, sym), passthru_wrappers[i]);
			if (tramp) {
				passthru_num++;
				passthru_names[i] = strdup(name);
				passthru_orig[i] = (passthru_func)tramp;
			} else
				debugPrintf("scriptprof: can't hook %s\n", sym);
		}
		name = strtok_r(NULL, " \t\r\n", &save);
	}
}

// Times the object events listed in ux0:data/gms/shared/userprof.txt (full names, e.g.
// gml_Object_obj_lightmaster_Other_10) with entry hooks. evprof can't stub user events in
// g_GMLFuncs (actors drawn through event_user vanish), but entry hooks leave the table alone.
#define USERPROF_MAX 48
void *katana_rx_alloc(size_t size);
typedef void (*userprof_event)(void *self, void *other);
static userprof_event userprof_orig[USERPROF_MAX];
static const char *userprof_names[USERPROF_MAX];
static uint32_t userprof_time[USERPROF_MAX], userprof_calls[USERPROF_MAX];
static int userprof_num;

static void userprof_call(void *self, void *other, int idx) {
	uint32_t start = sceKernelGetProcessTimeLow();
	userprof_orig[idx](self, other);
	userprof_time[idx] += sceKernelGetProcessTimeLow() - start;
	userprof_calls[idx]++;
}

static void userprof_hooks(void) {
	static char names[4096];
	SceUID fd = sceIoOpen("ux0:data/gms/shared/userprof.txt", SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	int len = sceIoRead(fd, names, sizeof(names) - 1);
	sceIoClose(fd);
	names[len > 0 ? len : 0] = 0;
	char *save, *name = strtok_r(names, " \t\r\n", &save);
	for (; name && userprof_num < USERPROF_MAX; name = strtok_r(NULL, " \t\r\n", &save)) {
		char sym[256];
		snprintf(sym, sizeof(sym), "_Z%d%sP9CInstanceS0_", (int)strlen(name), name);
		uintptr_t entry = so_symbol(&yoyoloader_mod, sym);
		uint32_t *w = (uint32_t *)entry;
		// Only relocate a push {..., lr} followed by add fp, sp, #imm (PC-independent)
		if (!entry || (entry & 3) || (w[0] & 0xffff4000) != 0xe92d4000 || (w[1] & 0xfffff000) != 0xe28db000) {
			debugPrintf("userprof: can't hook %s\n", name);
			continue;
		}
		uint32_t *rx = katana_rx_alloc(32);
		if (!rx)
			return;
		int idx = userprof_num++;
		uint32_t code[7] = {
			0xE3002000 | ((idx >> 12) << 16) | (idx & 0xfff), 0xe51ff004, (uint32_t)&userprof_call, // stub
			w[0], w[1], 0xe51ff004, (uint32_t)(entry + 8), // trampoline to the original
		};
		kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof(code));
		kuKernelFlushCaches(rx, sizeof(code));
		userprof_orig[idx] = (userprof_event)&rx[3];
		userprof_names[idx] = strdup(name);
		hook_addr(entry, (uintptr_t)rx);
		kuKernelFlushCaches((void *)entry, 8);
	}
	debugPrintf("userprof: timing %d events\n", userprof_num);
}

static void stage_hook(const char *symbol, void **orig, void *wrapper) {
	// Entry hook first: the deployed runner calls most functions directly (patch_plt_calls.py), past the GOT
	void *tramp = entry_hook(so_symbol(&yoyoloader_mod, symbol), wrapper);
	if (tramp) {
		*orig = tramp;
		return;
	}
	uintptr_t *slot = so_got_slot(&yoyoloader_mod, symbol);
	if (!slot) {
		debugPrintf("Stage profiler: no PLT slot for %s\n", symbol);
		return;
	}
	*orig = (void *)*slot;
	*slot = (uintptr_t)wrapper;
}

// Times the runtime builtins listed in ux0:data/gms/shared/builtinprof.txt (GML names, e.g.
// place_meeting) by swapping their entries in the runner's function registry. The registry is
// filled during runner init, so this runs from the first profiling report.
typedef struct {
	const char *name;
	void *func;
	int argc;
} runner_function;

static void builtinprof_hooks(void) {
	static char names[1024];
	runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	SceUID fd = sceIoOpen("ux0:data/gms/shared/builtinprof.txt", SCE_O_RDONLY, 0);
	if (fd < 0 || !functions || !*functions || !count)
		return;
	int len = sceIoRead(fd, names, sizeof(names) - 1);
	sceIoClose(fd);
	names[len > 0 ? len : 0] = 0;
	char *save, *name = strtok_r(names, " \t\r\n", &save);
	while (name && passthru_num < PASSTHRU_MAX) {
		for (int i = 0; i < *count; i++) {
			runner_function *f = &(*functions)[i];
			if (f->name && !strcmp(f->name, name)) {
				int slot = passthru_num++;
				passthru_names[slot] = name;
				passthru_orig[slot] = (passthru_func)f->func;
				f->func = passthru_wrappers[slot];
				break;
			}
		}
		name = strtok_r(NULL, " \t\r\n", &save);
	}
}

typedef void (*builtin_fn)(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args);

void katana_cloth_report(int frames);

static gml_rvalue *gml_vget(void *obj, int slot);
static void **global_obj_ptr;

// Replaces a function at its entry with a native wrapper, returns a trampoline to the original
// (NULL if the prologue isn't the relocatable push {..., lr}; add fp, sp, #imm).
static void *entry_hook(uintptr_t entry, void *replacement) {
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

// exp_lightfast / exp_lightfast_inline: 0 = original events, 1 = hand native, 2 = inlined variant
static int light_fast_mode;

// Admission counters of the installed light native, per report window (event 0 = obj_orangelight Step,
// 1 = obj_lightback Step). Reject lines are source lines of the installed variant's file, counts are totals.
static void light_fast_report(int frames) {
	extern unsigned light_fast_hits[2], light_fast_fallbacks[2];
	extern unsigned light_fast_fail_line[64], light_fast_fail_count[2][64], light_fast_fail_overflow[2];
	extern unsigned light_fast_inline_hits[2], light_fast_inline_fallbacks[2];
	extern unsigned light_fast_inline_reject_line[48], light_fast_inline_reject_count[2][48];
	static const char *const events[2] = { "orangelight", "lightback" };
	static unsigned last_hits[2], last_fallbacks[2];
	int inlined = light_fast_mode == 2;
	const unsigned *hits = inlined ? light_fast_inline_hits : light_fast_hits;
	const unsigned *fallbacks = inlined ? light_fast_inline_fallbacks : light_fast_fallbacks;
	if (hits[0] == last_hits[0] && fallbacks[0] == last_fallbacks[0] && hits[1] == last_hits[1] && fallbacks[1] == last_fallbacks[1])
		return;
	debugPrintf("[LIGHTFAST] %s: orangelight %.1f native %.1f fallback | lightback %.1f native %.1f fallback (per frame)\n",
		inlined ? "inline" : "hand", (float)(hits[0] - last_hits[0]) / frames, (float)(fallbacks[0] - last_fallbacks[0]) / frames,
		(float)(hits[1] - last_hits[1]) / frames, (float)(fallbacks[1] - last_fallbacks[1]) / frames);
	for (int e = 0; e < 2; e++) {
		last_hits[e] = hits[e];
		last_fallbacks[e] = fallbacks[e];
		for (int i = 0; i < (inlined ? 48 : 64); i++) {
			unsigned line = inlined ? light_fast_inline_reject_line[i] : light_fast_fail_line[i];
			unsigned count = inlined ? light_fast_inline_reject_count[e][i] : light_fast_fail_count[e][i];
			if (line && count)
				debugPrintf("[LIGHTFAST] %s reject at %s:%u x%u\n", events[e], inlined ? "light_fast_inline.c" : "light_fast.c", line, count);
		}
		if (!inlined && light_fast_fail_overflow[e])
			debugPrintf("[LIGHTFAST] %s reject histogram overflow x%u\n", events[e], light_fast_fail_overflow[e]);
	}
}

void katana_prof_report(int frames) {
	extern unsigned t30_letter_skips;
	if (t30_letter_skips)
		debugPrintf("[FPS30] %u letter steps skipped on frozen ticks\n", t30_letter_skips);
	extern uint32_t sound_limit_skipped;
	if (sound_limit_skipped)
		debugPrintf("[AUDIO] same-sound copies skipped: %u\n", sound_limit_skipped);
	{
		extern unsigned clubber_native_hits, clubber_native_fallbacks, clubber_native_draws, clubber_native_reject_count[13];
		static unsigned last_hits, last_fallbacks;
		if (clubber_native_hits != last_hits || clubber_native_fallbacks != last_fallbacks) {
			char reasons[96] = {0};
			for (int i = 0, n = 0; i < 13; i++)
				if (clubber_native_reject_count[i])
					n += snprintf(reasons + n, sizeof(reasons) - n, " %d:%u", i, clubber_native_reject_count[i]);
			debugPrintf("[CLUBBER] native %u fallback %u draws %u | reasons%s\n", clubber_native_hits - last_hits,
				clubber_native_fallbacks - last_fallbacks, clubber_native_draws, reasons);
			last_hits = clubber_native_hits;
			last_fallbacks = clubber_native_fallbacks;
		}
	}
	{
		extern unsigned letter_native_hits[5], letter_native_fallbacks[5], letter_native_reject_line[64], letter_native_reject_count[64];
		if (letter_native_hits[0] || letter_native_fallbacks[0] || letter_native_hits[1] || letter_native_fallbacks[1])
			debugPrintf("[LETTER] hits %u %u %u %u %u fallbacks %u %u %u %u %u\n", letter_native_hits[0], letter_native_hits[1],
				letter_native_hits[2], letter_native_hits[3], letter_native_hits[4], letter_native_fallbacks[0],
				letter_native_fallbacks[1], letter_native_fallbacks[2], letter_native_fallbacks[3], letter_native_fallbacks[4]);
		for (int i = 0; i < 64 && letter_native_reject_line[i]; i++)
			debugPrintf("[LETTER] reject at letter_native.c:%u x%u\n", letter_native_reject_line[i], letter_native_reject_count[i]);
		extern unsigned minecart_native_hits, minecart_native_fallbacks, minecart_native_fastline_hits, minecart_native_fastline_fallbacks;
		extern unsigned minecart_rail_native_hits, minecart_rail_native_fallbacks, minecart_native_reject_line[64], minecart_native_reject_count[64];
		if (minecart_native_hits || minecart_native_fallbacks)
			debugPrintf("[MINECART] master %u/%u fastline %u/%u rail %u/%u (native/fallback)\n", minecart_native_hits,
				minecart_native_fallbacks, minecart_native_fastline_hits, minecart_native_fastline_fallbacks,
				minecart_rail_native_hits, minecart_rail_native_fallbacks);
		for (int i = 0; i < 64 && minecart_native_reject_line[i]; i++)
			debugPrintf("[MINECART] reject at minecart_native.c:%u x%u\n", minecart_native_reject_line[i], minecart_native_reject_count[i]);
		extern unsigned dustspawner_loop_native_calls, dustspawner_loop_native_hits, dustspawner_loop_native_loops;
		extern unsigned dustspawner_loop_native_passes, dustspawner_loop_native_draws, dustspawner_loop_native_paused;
		extern unsigned dustspawner_loop_native_whole_fallbacks, dustspawner_loop_native_particle_fallbacks;
		if (dustspawner_loop_native_calls)
			debugPrintf("[DUST] calls %u hits %u loops %u passes %u draws %u paused %u | fallbacks whole %u particle %u\n",
				dustspawner_loop_native_calls, dustspawner_loop_native_hits, dustspawner_loop_native_loops,
				dustspawner_loop_native_passes, dustspawner_loop_native_draws, dustspawner_loop_native_paused,
				dustspawner_loop_native_whole_fallbacks, dustspawner_loop_native_particle_fallbacks);
		// Order of the reason enum in dustspawner_native.c
		static const char *const dust_reasons[] = {"profile", "helper", "table", "ids", "missing", "kind", "builtin",
			"instance", "child", "registry", "alias", "managed", "range", "paused", "event_state", "room", "depth", "tail"};
		extern unsigned dustspawner_loop_native_reject_count[18];
		for (int i = 0; i < 18; i++)
			if (dustspawner_loop_native_reject_count[i])
				debugPrintf("[DUST] reject %s x%u\n", dust_reasons[i], dustspawner_loop_native_reject_count[i]);
		extern unsigned flash_mask_hits, flash_mask_fallbacks, flash_mask_culled, flash_mask_draws;
		extern unsigned flash_mask_fail_line[32], flash_mask_fail_count[32];
		if (flash_mask_hits || flash_mask_fallbacks)
			debugPrintf("[FLASHMASK] native %u fallback %u | drawn %u culled %u\n", flash_mask_hits,
				flash_mask_fallbacks, flash_mask_draws, flash_mask_culled);
		for (int i = 0; i < 32 && flash_mask_fail_line[i]; i++)
			debugPrintf("[FLASHMASK] reject at flash_mask.c:%u x%u\n", flash_mask_fail_line[i], flash_mask_fail_count[i]);
	}
	{
		extern unsigned virtual_capture_virtual_captures, virtual_capture_direct_replays, virtual_capture_materializations;
		extern unsigned virtual_capture_materializations_by_reason[11], virtual_capture_rejections_used;
		extern struct { unsigned line, count; } virtual_capture_rejections[128];
		if (virtual_capture_virtual_captures || virtual_capture_materializations || virtual_capture_rejections_used) {
			extern unsigned virtual_capture_relaxed_replays, virtual_capture_clipped_quads;
			debugPrintf("[VCAP] relaxed replays %u clipped quads %u\n", virtual_capture_relaxed_replays, virtual_capture_clipped_quads);
			debugPrintf("[VCAP] virtual %u direct %u materialized %u | reasons %u %u %u %u %u %u %u %u %u %u %u\n",
				virtual_capture_virtual_captures, virtual_capture_direct_replays, virtual_capture_materializations,
				virtual_capture_materializations_by_reason[0], virtual_capture_materializations_by_reason[1],
				virtual_capture_materializations_by_reason[2], virtual_capture_materializations_by_reason[3],
				virtual_capture_materializations_by_reason[4], virtual_capture_materializations_by_reason[5],
				virtual_capture_materializations_by_reason[6], virtual_capture_materializations_by_reason[7],
				virtual_capture_materializations_by_reason[8], virtual_capture_materializations_by_reason[9],
				virtual_capture_materializations_by_reason[10]);
			extern unsigned virtual_capture_materializations_by_hook[];
			extern int virtual_capture_hook_count(void);
			extern const char *virtual_capture_hook_name(int i);
			extern unsigned virtual_capture_armed_hits[];
			for (int i = 0; i < virtual_capture_hook_count(); i++)
				if (virtual_capture_armed_hits[i] > 20)
					debugPrintf("[VCAP] barrier entries %s x%u\n", virtual_capture_hook_name(i), virtual_capture_armed_hits[i]);
			for (int i = 0; i < virtual_capture_hook_count(); i++)
				if (virtual_capture_materializations_by_hook[i])
					debugPrintf("[VCAP] materialized by %s x%u\n", virtual_capture_hook_name(i),
						virtual_capture_materializations_by_hook[i]);
			extern struct { unsigned offset, hook, count; } virtual_capture_callers[64];
			extern unsigned virtual_capture_callers_used;
			for (unsigned i = 0; i < virtual_capture_callers_used && i < 64; i++)
				debugPrintf("[VCAP] caller runner+0x%x -> %s x%u\n", virtual_capture_callers[i].offset,
					virtual_capture_hook_name(virtual_capture_callers[i].hook), virtual_capture_callers[i].count);
			for (unsigned i = 0; i < virtual_capture_rejections_used && i < 128; i++)
				debugPrintf("[VCAP] reject at virtual_capture.c:%u x%u\n", virtual_capture_rejections[i].line,
					virtual_capture_rejections[i].count);
		}
	}
	{
		extern unsigned input_native_calls, input_native_hits, input_native_fallbacks, input_native_rejection_used;
		extern struct { uint32_t line, count; } input_native_rejections[512];
		if (input_native_calls)
			debugPrintf("[INPUT] calls %u native %u fallback %u\n", input_native_calls, input_native_hits, input_native_fallbacks);
		for (unsigned i = 0; i < input_native_rejection_used && i < 16; i++)
			debugPrintf("[INPUT] reject at input_native.c:%u x%u\n", (unsigned)input_native_rejections[i].line,
				(unsigned)input_native_rejections[i].count);
		extern unsigned laser_native_hits, laser_native_fallbacks, laser_native_probe_hits, laser_native_probe_fallbacks;
		extern unsigned laser_native_reject_line[64], laser_native_reject_count[64];
		if (laser_native_hits || laser_native_fallbacks || laser_native_probe_hits || laser_native_probe_fallbacks)
			debugPrintf("[LASER] draw %u/%u probe %u/%u (native/fallback)\n", laser_native_hits, laser_native_fallbacks,
				laser_native_probe_hits, laser_native_probe_fallbacks);
		for (int i = 0; i < 64 && laser_native_reject_line[i]; i++)
			debugPrintf("[LASER] reject at laser_native.c:%u x%u\n", laser_native_reject_line[i], laser_native_reject_count[i]);
	}
	if (debugMode && light_fast_mode)
		light_fast_report(frames);
	{
		extern unsigned rope_fast_hits, rope_fast_fallbacks;
		extern unsigned rf_fail_line[16], rf_fail_count[16];
		for (int i = 0; i < 16 && rf_fail_line[i]; i++)
			debugPrintf("[ROPE] reject at rope_fast.c:%u x%u\n", rf_fail_line[i], rf_fail_count[i]);
		if (rope_fast_hits || rope_fast_fallbacks)
			debugPrintf("[ROPE] fast %u fallback %u\n", rope_fast_hits, rope_fast_fallbacks);
		extern unsigned collision_fast_hits, collision_fallback_hits;
		if (collision_fast_hits || collision_fallback_hits)
			debugPrintf("[BBOX] fast %u fallback %u\n", collision_fast_hits, collision_fallback_hits);
	}
	katana_cloth_report(frames);
	static int builtins_hooked = 0;
	if (!builtins_hooked) {
		builtins_hooked = 1;
		if (exp_flag("builtinprof"))
			builtinprof_hooks();
	}
	if (recolor_frames) {
		debugPrintf("[RECOLOR] global.recolor_timer changed %u times in %u sampled frames (now %.2f)\n", recolor_changes, recolor_frames, last_recolor_timer);
		recolor_changes = recolor_frames = 0;
	}
	{
		// Top objects by instance-draw time
		const char *(*object_name)(int) = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_Namei");
		for (int n = 0; n < 10; n++) {
			int best = -1;
			for (int o = 0; o < LAYER_OBJ_MAX; o++)
				if (layer_obj_calls[o] && (best < 0 || layer_obj_time[o] > layer_obj_time[best]))
					best = o;
			if (best < 0)
				break;
			debugPrintf("[LAYERDRAW] %-28s %6.1f draws %6.2fms\n", object_name ? object_name(best) : "?",
				(float)layer_obj_calls[best] / frames, layer_obj_time[best] / 1000.0f / frames);
			layer_obj_calls[best] = 0;
		}
		memset(layer_obj_time, 0, sizeof(layer_obj_time));
		memset(layer_obj_calls, 0, sizeof(layer_obj_calls));
	}
	for (int i = 0; i < S_COUNT; i++) {
		if (stage_calls[i])
			debugPrintf("[STAGE]  %-18s %5.1f calls %6.2fms\n", stage_names[i], (float)stage_calls[i] / frames, stage_time[i] / 1000.0f / frames);
	}
		for (int i = 0; i < SCRIPT_PROF_NUM; i++) {
		if (script_calls[i])
			debugPrintf("[SCRIPT] %-26s %6.1f calls %6.2fms\n", script_names[i], (float)script_calls[i] / frames, script_time[i] / 1000.0f / frames);
	}
	for (int i = 0; i < passthru_num; i++) {
		if (passthru_calls[i])
			debugPrintf("[RUNNER] %-24s %7.1f calls %6.2fms (max %.2fms)\n", passthru_names[i], (float)passthru_calls[i] / frames,
				passthru_time[i] / 1000.0f / frames, passthru_max[i] / 1000.0f);
	}
	for (int i = 0; i < userprof_num; i++) {
		if (userprof_calls[i])
			debugPrintf("[USEREV] %6.2fms %6.1f calls %s\n", userprof_time[i] / 1000.0f / frames,
				(float)userprof_calls[i] / frames, userprof_names[i]);
	}
	memset(userprof_time, 0, sizeof(userprof_time));
	memset(userprof_calls, 0, sizeof(userprof_calls));
	memset(passthru_time, 0, sizeof(passthru_time));
	memset(passthru_max, 0, sizeof(passthru_max));
	memset(passthru_calls, 0, sizeof(passthru_calls));
	memset(script_time, 0, sizeof(script_time));
	memset(script_calls, 0, sizeof(script_calls));
	memset(stage_time, 0, sizeof(stage_time));
	memset(stage_calls, 0, sizeof(stage_calls));
}

/*
 * Performance throttles for cosmetic effects that are too heavy for the Vita CPU.
 * Implemented by swapping compiled event functions in the runner's g_GMLFuncs table.
 */
typedef void (*event_func)(void *self, void *other);

typedef struct {
	const char *name;
	event_func func;
	void *unk;
} gml_func_entry;

#define GML_FUNCS_SIZE 79992
#define PARTICLES_ALWAYS_DRAWN 20

static uint32_t *frame_count;

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
static gml_rvalue *gml_vget(void *obj, int slot);
static void **global_obj_ptr;
static int *var_xview, *var_yview, *var_wview, *var_hview;
static event_func orig_clothesline_draw;
static int cloth_cull;
static uint32_t cloth_culled, cloth_events;
static double cloth_last[6];

static double global_real(int *var, double fallback) {
	gml_rvalue *v = gml_vget(*global_obj_ptr, var[1]);
	return (v && v->kind == 0) ? v->val : fallback;
}

static void **run_room;
static int *current_room;
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

static void **global_obj_ptr;
static int *var_recolor_timer;

static gml_rvalue *gml_vget(void *obj, int slot) {
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
	extern int debugMode;
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

// DoAStep_Draw hook: application surface size, then lighting at 30 Hz
static void (*katana_orig_draw)(void);

static void DoAStep_Draw_katana(void) {
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
	if (lighting30 && !t30_active) {
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
 */
#define TEXFLUSH_MIN_INTERVAL_US 5000000
#define TEXFLUSH_FAILURE_FLUSHES 3
#define TEXFLUSH_SPILL_BYTES (1024 * 1024)
static builtin_fn texture_flush;
static int texflush_failures_seen, texflush_spill_room = -2;
static size_t texflush_spill_seen;
static uint32_t texflush_last_us;
unsigned texflush_count;

/*
 * Dynamic resolution (dynamicRes, default 1): a 960x540 application surface keeps text sharp, but rooms full of
 * blended light sprites drawn straight onto it (room_cathedral: window beams, ~65 light glows) can run out of
 * GPU fill rate: 43-52 fps with 4-9 ms GPU waits at the swap, 59 at 640x360. When at least DYNRES_BOUND_FRAMES
 * of DYNRES_WINDOW frames wait on the GPU for over DYNRES_SWAP_US at the swap, the room drops to 640x360 until
 * the next room. The first second of a room (texture loads stall the swap too) is not counted. (A light room at
 * 60 fps waits ~0.15 ms; texture pages pushed out of VRAM into RAM make ordinary rooms GPU-bound and drop too.)
 */
#define DYNRES_WINDOW 60
#define DYNRES_BOUND_FRAMES 20
#define DYNRES_SWAP_US 2000
#define DYNRES_SETTLE_FRAMES 60
int dynamic_res = 1;
unsigned dynres_drops;
static int dynres_test; // exp_dynrestest: every frame counts as GPU-bound, so each room drops after ~2 s

static void dynamic_res_update(uint32_t swap_us) {
	static int room = -2, frames, window, bound, dropped;
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
	// In 30 fps mode the swap also waits for the second vblank of each frame: not GPU load
	if (dropped || t30_active || ++frames <= DYNRES_SETTLE_FRAMES)
		return;
	bound += swap_us > DYNRES_SWAP_US || dynres_test;
	if (++window < DYNRES_WINDOW)
		return;
	if (bound >= DYNRES_BOUND_FRAMES) {
		dropped = 1;
		dynres_drops++;
		app_surf_w = 640;
		app_surf_h = 360;
		debugPrintf("[DYNRES] room %d GPU-bound (%d of %d frames waited over %.1f ms at the swap): 640x360\n", room,
			bound, window, DYNRES_SWAP_US / 1000.0f);
	}
	window = bound = 0;
}

// Runs right after vglSwapBuffers, between frames: every scene of the frame has been submitted, so the GPU finish
// and garbage collection below cannot free memory that commands still being recorded refer to
static void t30_update(void);

void katana_after_swap(uint32_t swap_us) {
	t30_update();
	dynamic_res_update(swap_us);
	if (!texture_flush)
		return;
	extern int vgl_failed_allocs;
	extern size_t vgl_gpu_fallback_bytes;
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
		texflush_spill_seen = vgl_gpu_fallback_bytes;
		if (room != texflush_spill_room) {
			texflush_spill_room = room;
			reason = "texture data outside VRAM";
		}
	} else
		texflush_spill_seen = vgl_gpu_fallback_bytes;
	if (reason) {
		failure_pending = 0; // the flush reloads every page, including the one that failed
		gml_rvalue result = {0};
		texture_flush(&result, NULL, NULL, 0, NULL);
		// Free the flushed pages now: otherwise vitaGL releases them a few frames later and the reloads,
		// still not fitting, land in RAM again
		extern void vgl_collect_garbage_now(void);
		vgl_collect_garbage_now();
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

static void appsurf_copy_install(void) {
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
		if (block < 0 || sceKernelGetMemBlockBase(block, (void **)&rx_arena) < 0) {
			debugPrintf("rx arena: allocation failed (0x%08X)\n", block);
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
	// The runner passes the QOI size from the page header, which prepare_apk.py sets to the exact decoded size: a
	// shorter result means a damaged page, and the rest of the buffer would be uninitialised
	int n = block_len <= src_len - 8 ? LZ4_decompress_safe(src + 8, dest, block_len, *dest_len) : -1;
	if (n < 0 || (unsigned)n != *dest_len)
		fatal_error("LZ4 texture page failed to decode (%u of %u bytes -> %u): %d", block_len, src_len, *dest_len, n);
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
// the running game room_goto() there (checked once a second, debug mode only).
void katana_goto_check(void) {
	static uint32_t frames;
	static int (*room_find)(const char *name);
	static void (*room_goto)(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args);
	if (++frames < 60)
		return;
	frames = 0;
	SceIoStat st;
	// Debug: ux0:data/gms/shared/level_complete creates obj_level_complete (object 608 in Katana ZERO 1.1.10),
	// as reaching the level exit does
	if (sceIoGetstat("ux0:data/gms/shared/level_complete", &st) >= 0) {
		sceIoRemove("ux0:data/gms/shared/level_complete");
		void *(*command_create)(int object, float x, float y) = (void *)so_symbol(&yoyoloader_mod, "_Z14Command_Createiff");
		debugPrintf("level_complete: creating obj_level_complete\n");
		if (command_create)
			command_create(608, 0.0f, 0.0f);
	}
	// Debug: ux0:data/gms/shared/create_object (an object index) creates that object at (0, 0)
	if (sceIoGetstat("ux0:data/gms/shared/create_object", &st) >= 0) {
		char text[16] = {0};
		SceUID fd = sceIoOpen("ux0:data/gms/shared/create_object", SCE_O_RDONLY, 0);
		if (fd >= 0) {
			sceIoRead(fd, text, sizeof(text) - 1);
			sceIoClose(fd);
		}
		sceIoRemove("ux0:data/gms/shared/create_object");
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
	if (sceIoGetstat("ux0:data/gms/shared/goto_room", &st) < 0)
		return;
	char name[64] = {0};
	SceUID fd = sceIoOpen("ux0:data/gms/shared/goto_room", SCE_O_RDONLY, 0);
	if (fd >= 0) {
		sceIoRead(fd, name, sizeof(name) - 1);
		sceIoClose(fd);
	}
	sceIoRemove("ux0:data/gms/shared/goto_room");
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

static void fog_create(void *self, void *other) {
	static unsigned created;
	orig_fog_create(self, other);
	int room = current_room ? *current_room : -1;
	if (room != doomed_fog_room) {
		doomed_fog_room = room;
		doomed_fog_count = 0;
		memset(doomed_fog, 0, sizeof(doomed_fog));
	}
	fog_take(self); // a new instance can reuse the memory of a puff destroyed before its first Step
	if (created++ % fog_scale)
		fog_mark(self);
}

static void fog_step(void *self, void *other) {
	if (fog_take(self)) {
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
	dissolve_install();
	fog_install();
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
	uint32_t insn = read_qoif ? *(uint32_t *)(read_qoif + 0xdc) : 0;
	if ((insn & 0xff000000) != 0xeb000000) {
		fatal_error("Texture decoder self-test: unexpected ReadQOIFFile layout (0x%08X); not starting to avoid a GPU crash.", insn);
	}
	int32_t imm = (int32_t)(insn << 8) >> 6;
	int (*decompress)(char *, unsigned *, char *, unsigned, int, int) = (void *)(read_qoif + 0xdc + 8 + imm);

	static const char plain[] = "Katana ZERO texture self-test payload, Katana ZERO texture self-test payload";
	char packed[8 + LZ4_COMPRESSBOUND(sizeof(plain))], out[sizeof(plain)];
	int block = LZ4_compress_default(plain, packed + 8, sizeof(plain), sizeof(packed) - 8);
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

// The mobile on-screen buttons (obj_screenBtn) keep their GUI Draw event, which also runs
// button logic, but their sprites are rendered with colour writes off so they are invisible
static event_func orig_screen_btn_gui;
static void (*graphics_flush)(void);

static void screen_btn_gui_hidden(void *self, void *other) {
	graphics_flush();
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	orig_screen_btn_gui(self, other);
	graphics_flush();
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
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

static gml_rvalue *global_var(int *var) {
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

static event_func gml_replace(gml_func_entry *table, const char *name, event_func wrapper) {
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

/*
 * 30 fps mode (fps30 in yyl.cfg: 0 off, 1 in rooms that can't hold 60 fps (default), 2 everywhere). GameMaker games run one
 * logic tick per frame, so a CPU-bound room at 31 fps plays at half speed. In this mode such a room runs 30 ticks a
 * second (paced by the display: two vblanks per frame) and each tick advances the game twice as far, so it plays at
 * full speed at 30 fps. Not exact: input is read 30 times a second, movement takes steps twice as long, and anything
 * the game counts in ticks without its time factor runs at half speed (enemy search timeouts, dialogue typing, some
 * effect lifetimes). What is converted:
 * - The game's time factor global.fr (slow motion; 0 during hit-stop) is doubled right after obj_master and
 *   obj_cutscene_master user event 0 set it each tick. Before those run, a value we doubled is put back, so a
 *   controller that keeps the previous value (pause) does not double it again.
 * - The hit-stop counter (freeze_timer of those controllers) counts two ticks per tick.
 * - The player's animation runs with fr = 1 (player_animation overrides it around animation_code): 2 in this mode.
 * - The runner's built-in clocks run twice per tick: sprite animation (Instance_Animate, image_speed) and alarms
 *   (HandleAlarm). Built-in motion (speed/hspeed/vspeed, HandleInstanceMovement) too with exp_t30move: off by default,
 *   since much of the game's motion already multiplies its own speeds by fr.
 * - The loader's every-other-tick work (lighting refresh, cloth physics, rain and particle drawing) runs every tick,
 *   and dynamic resolution doesn't count the wait for the second vblank as GPU load.
 * - Dialogue letters don't start on frozen ticks (t30_letter_step).
 * Scripted sequences (the game drives the player: the first level's tutorial, cutscenes) count ticks exactly and break
 * at 30 Hz (the tutorial's slow motion came too late and the player was shot), so they always run at 60 ticks/s.
 * In mode 1 a room switches after T30_SETTLE_FRAMES once T30_SLOW_FRAMES of a T30_WINDOW-frame window took over
 * T30_SLOW_US, and stays at 30 Hz until the next room; rooms that switched once start at 30 Hz the next time (death
 * restarts keep the room). exp_t30anim0, exp_t30alarm0 and exp_t30freeze0 turn the respective conversion off.
 * Debug: exp_t30trace logs the player's position, exp_t30never keeps 60 ticks/s with the hooks installed (a baseline
 * for traces), exp_t30watchdog crashes the game (for a core dump) when ticks stop for 3 s in 30 fps mode.
 */
int fps30 = 1;
#define T30_SETTLE_FRAMES 45
#define T30_WINDOW 45
#define T30_SLOW_FRAMES 40
#define T30_SLOW_US 20000
#define T30_MAX_ROOMS 1024
static int t30_anim, t30_alarm, t30_move, t30_freeze;
static int *var_fr, *var_freeze_timer;
static int t30_fr_scaled; // global.fr currently holds t30_scaled_fr, our doubling of the game's t30_game_fr
static double t30_game_fr, t30_scaled_fr;
static unsigned char t30_heavy_room[T30_MAX_ROOMS];
static event_func orig_master_user0, orig_cutscene_master_user0;
typedef void *(*gml_script_fn)(void *self, void *other, void *result, int argc, void **args);
static gml_script_fn orig_player_animation, orig_animation_code;
static void (*orig_instance_animate)(void *inst);
static void (*orig_handle_alarm)(void);
static void (*orig_handle_movement)(int);
static void (*update_active_lists)(void);
static int in_player_animation;
unsigned t30_switches;

static int t30_never; // exp_t30never: hooks (and exp_t30trace) installed, 30 ticks/s never used: a baseline for traces
static uint32_t t30_scripted_tick; // last tick a cutscene script drove the player
static volatile int t30_stage; // debug: where the 30 fps hooks were last (the exp_t30watchdog report)
static gml_script_fn orig_override_control_init;

static void *t30_override_control_init(void *self, void *other, void *result, int argc, void **args) {
	t30_scripted_tick = *frame_count ? *frame_count : 1;
	return orig_override_control_init(self, other, result, argc, args);
}

static void t30_set(int on) {
	if (on == t30_active)
		return;
	t30_active = on;
	eglSwapInterval(NULL, on ? 2 : 1);
	t30_switches++;
	debugPrintf("[FPS30] room %d: %s\n", current_room ? *current_room : -1, on ? "30 ticks/s, double time step" : "60 ticks/s");
}

// Once per presented frame (katana_after_swap)
static void t30_update(void) {
	static int room = -2, frames, slow;
	static uint64_t last;
	uint64_t now = sceKernelGetProcessTimeWide();
	uint64_t interval = last ? now - last : 0;
	last = now;
	if (!fps30 || t30_never || !current_room)
		return;
	if (*current_room != room) {
		room = *current_room;
		frames = slow = 0;
	}
	// Scripted sequences (the game drives the player through override_control_init every tick) count ticks with
	// exact comparisons: keep them at 60 ticks/s
	if (t30_scripted_tick && *frame_count - t30_scripted_tick < 3) {
		t30_set(0);
		frames = slow = 0;
		return;
	}
	if (fps30 >= 2 || (room >= 0 && room < T30_MAX_ROOMS && t30_heavy_room[room])) {
		t30_set(1);
		return;
	}
	t30_set(0);
	if (++frames <= T30_SETTLE_FRAMES)
		return;
	slow += interval > T30_SLOW_US;
	if (frames < T30_SETTLE_FRAMES + T30_WINDOW)
		return;
	if (slow >= T30_SLOW_FRAMES) {
		if (room >= 0 && room < T30_MAX_ROOMS)
			t30_heavy_room[room] = 1;
		debugPrintf("[FPS30] room %d: %d of %d frames over %.0f ms\n", room, slow, T30_WINDOW, T30_SLOW_US / 1000.0f);
		t30_set(1);
	}
	frames = T30_SETTLE_FRAMES;
	slow = 0;
}

static gml_rvalue *instance_var(void *self, int *var) {
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)self)[3];
	gml_rvalue *v = var ? vwrite(self, var[1]) : NULL;
	return v && v->kind == 0 ? v : NULL;
}

static void t30_time_controller(event_func orig, void *self, void *other) {
	t30_stage = 30;
	gml_rvalue *fr = global_var(var_fr);
	if (t30_fr_scaled && fr && fr->kind == 0 && fr->val == t30_scaled_fr)
		fr->val = t30_game_fr;
	t30_fr_scaled = 0;
	gml_rvalue *freeze;
	if (t30_active && t30_freeze && (freeze = instance_var(self, var_freeze_timer)) && freeze->val >= 1)
		freeze->val -= 1;
	orig(self, other);
	if (t30_active && (fr = global_var(var_fr)) && fr->kind == 0) {
		t30_game_fr = fr->val;
		t30_scaled_fr = fr->val *= 2;
		t30_fr_scaled = 1;
	}
	t30_stage = 0;
}

static void t30_master_user0(void *self, void *other) {
	t30_time_controller(orig_master_user0, self, other);
}

static void t30_cutscene_master_user0(void *self, void *other) {
	t30_time_controller(orig_cutscene_master_user0, self, other);
}

// exp_t30trace (debugMode): the player's position every 6th tick, to compare movement over real time between modes
static int t30_trace;

static void *t30_player_animation(void *self, void *other, void *result, int argc, void **args) {
	if (t30_trace) {
		static uint32_t last_tick;
		if (*frame_count - last_tick >= (t30_active ? 3u : 6u)) {
			last_tick = *frame_count;
			float x, y;
			memcpy(&x, (char *)self + 0xb8, 4);
			memcpy(&y, (char *)self + 0xbc, 4);
			gml_rvalue *fr = global_var(var_fr);
			debugPrintf("[T30POS] %llu us tick %u x %.2f y %.2f fr %.3f\n", sceKernelGetProcessTimeWide(), *frame_count, x, y,
				fr && fr->kind == 0 ? fr->val : -1.0);
		}
	}
	in_player_animation++;
	void *r = orig_player_animation(self, other, result, argc, args);
	in_player_animation--;
	return r;
}

static void *t30_animation_code(void *self, void *other, void *result, int argc, void **args) {
	gml_rvalue *fr;
	if (!t30_active || !in_player_animation || !(fr = global_var(var_fr)) || fr->kind != 0 || fr->val != 1.0)
		return orig_animation_code(self, other, result, argc, args);
	fr->val = 2.0;
	void *r = orig_animation_code(self, other, result, argc, args);
	if ((fr = global_var(var_fr)) && fr->kind == 0 && fr->val == 2.0)
		fr->val = 1.0;
	return r;
}

// obj_letter Step (dialogue letters) first runs `while (oscillate_delay > 0) { ...; oscillate_delay -= 1 * fr; }`:
// endless while time is frozen (fr = 0, hit-stop), and the club's sign letters start with delays in the thousands.
// The original never steps a new letter on a frozen tick; with the shifted timing of 30 fps mode it can (the game
// hung in room_club_4_2 after a death). Such a letter skips its Step until time moves again.
static event_func orig_letter_step;
static int *var_oscillate_delay;
unsigned t30_letter_skips;

static void t30_letter_step(void *self, void *other) {
	gml_rvalue *fr, *delay;
	if (t30_active && (fr = global_var(var_fr)) && fr->kind == 0 && fr->val < 0.001 && (delay = gml_vget(self, var_oscillate_delay[1])) &&
			delay->kind == 0 && delay->val > 0) {
		t30_letter_skips++;
		return;
	}
	orig_letter_step(self, other);
}

static void t30_instance_animate(void *inst) {
	t30_stage = 10;
	orig_instance_animate(inst);
	if (t30_active && t30_anim) {
		t30_stage = 11;
		orig_instance_animate(inst);
	}
	t30_stage = 0;
}

static void t30_handle_alarm(void) {
	t30_stage = 20;
	orig_handle_alarm();
	if (t30_active && t30_alarm) {
		t30_stage = 21;
		update_active_lists();
		t30_stage = 22;
		orig_handle_alarm();
	}
	t30_stage = 0;
}

static int t30_watchdog(SceSize args, void *argp) {
	uint32_t last = 0, still = 0;
	for (;;) {
		sceKernelDelayThread(500000);
		uint32_t now = *frame_count;
		if (now != last || !t30_active) { // loading screens stop ticks for seconds: armed in 30 fps mode only
			last = now;
			still = 0;
		} else if (++still == 6) {
			gml_rvalue *fr = global_var(var_fr);
			debugPrintf("[WATCHDOG] no tick for 3 s at tick %u: 30 fps stage %d, active %d, scripted tick %u, fr %f kind %u, "
				"game fr %f scaled %f (%d)\n", now, t30_stage, t30_active, t30_scripted_tick, fr ? fr->val : -1.0,
				fr ? fr->kind : 99, t30_game_fr, t30_scaled_fr, t30_fr_scaled);
			// A core dump records where every thread is
			sceKernelDelayThread(500000);
			*(volatile int *)0 = 0;
		}
	}
	return 0;
}

static void t30_handle_movement(int arg) {
	orig_handle_movement(arg);
	if (t30_active && t30_move) {
		update_active_lists();
		orig_handle_movement(arg);
	}
}

static void t30_install(gml_func_entry *table) {
	var_fr = (int *)so_symbol(&yoyoloader_mod, "g_VAR_fr");
	var_freeze_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_freeze_timer");
	update_active_lists = (void *)so_symbol(&yoyoloader_mod, "_Z17UpdateActiveListsv");
	if (!var_fr || !var_freeze_timer || !update_active_lists || !global_obj_ptr || !current_room) {
		fps30 = 0;
		debugPrintf("30 fps mode: NOT installed (symbols)\n");
		return;
	}
	orig_master_user0 = gml_replace(table, "gml_Object_obj_master_Other_10", t30_master_user0);
	var_oscillate_delay = (int *)so_symbol(&yoyoloader_mod, "g_VAR_oscillate_delay");
	if (var_oscillate_delay)
		orig_letter_step = gml_replace(table, "gml_Object_obj_letter_Step_0", t30_letter_step);
	orig_cutscene_master_user0 = gml_replace(table, "gml_Object_obj_cutscene_master_Other_10", t30_cutscene_master_user0);
	orig_player_animation = entry_hook(so_symbol(&yoyoloader_mod, "_Z27gml_Script_player_animationP9CInstanceS0_R8YYRValueiPPS1_"), t30_player_animation);
	orig_animation_code = orig_player_animation ? entry_hook(so_symbol(&yoyoloader_mod, "_Z25gml_Script_animation_codeP9CInstanceS0_R8YYRValueiPPS1_"), t30_animation_code) : NULL;
	orig_override_control_init = entry_hook(so_symbol(&yoyoloader_mod, "_Z32gml_Script_override_control_initP9CInstanceS0_R8YYRValueiPPS1_"), t30_override_control_init);
	orig_instance_animate = entry_hook(so_symbol(&yoyoloader_mod, "_Z16Instance_AnimateP9CInstance"), t30_instance_animate);
	orig_handle_alarm = entry_hook(so_symbol(&yoyoloader_mod, "_Z11HandleAlarmv"), t30_handle_alarm);
	orig_handle_movement = entry_hook(so_symbol(&yoyoloader_mod, "_Z22HandleInstanceMovementb"), t30_handle_movement);
	t30_anim = orig_instance_animate && !exp_flag("t30anim0");
	t30_alarm = orig_handle_alarm && !exp_flag("t30alarm0");
	t30_move = orig_handle_movement && exp_flag("t30move");
	t30_freeze = !exp_flag("t30freeze0");
	t30_trace = debugMode && exp_flag("t30trace");
	if (debugMode && exp_flag("t30watchdog")) {
		SceUID th = sceKernelCreateThread("t30 watchdog", t30_watchdog, 0x10000100, 0x2000, 0, 0, NULL);
		if (th >= 0)
			sceKernelStartThread(th, 0, NULL);
	}
	t30_never = exp_flag("t30never");
	if (exp_flag("t30force")) // testing: 30 ticks/s everywhere
		fps30 = 2;
	debugPrintf("30 fps mode %d: master %s, cutscene master %s, player animation %s, scripts %s, letters %s, animate %s, alarms %s, motion %s, hit-stop %s\n",
		fps30, orig_master_user0 ? "hooked" : "NOT hooked", orig_cutscene_master_user0 ? "hooked" : "NOT hooked",
		orig_animation_code ? "hooked" : "NOT hooked", orig_override_control_init ? "hooked" : "NOT hooked",
		orig_letter_step ? "hooked" : "NOT hooked", t30_anim ? "x2" : "x1", t30_alarm ? "x2" : "x1", t30_move ? "x2" : "x1",
		t30_freeze ? "x2" : "x1");
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
	if ((orig = gml_replace(table, "gml_Object_obj_particle_Other_25", particle_draw)))
		orig_particle_draw = orig;

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
	if (!exp_flag("touchui") && graphics_flush && (orig = gml_replace(table, "gml_Object_obj_screenBtn_Draw_64", screen_btn_gui_hidden)))
		orig_screen_btn_gui = orig;

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
	if (!exp_flag("minecartnative0")) { // minecart master and its rail helpers
		extern int minecart_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native minecart master: %s\n", minecart_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	if (!exp_flag("flashmask0")) {
		extern int flash_mask_install(void *(*rx_alloc)(size_t));
		debugPrintf("flashlight mask culling: %s\n", flash_mask_install(katana_rx_alloc) ? "installed" : "NOT installed");
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
	if (!exp_flag("lasernative0")) {
		extern int laser_native_install(void *(*rx_alloc)(size_t));
		debugPrintf("native lasers: %s\n", laser_native_install(katana_rx_alloc) ? "installed" : "NOT installed");
	}
	// Static recompiler kill test (work/cpu_research/kill_test/REPORT.md), default off: exact natives of obj_orangelight
	// Step and obj_lightback Step. exp_lightfast: the hand native (light_fast.c); exp_lightfast_inline: the same
	// event with the runner's compare/truth/variable helpers inlined (light_fast_inline.c). Both hook the same two
	// entries, so only one is installed; the inlined variant wins if both flags exist.
	if (exp_flag("lightfast_inline")) {
		extern int light_fast_inline_install(void *(*rx_alloc)(size_t));
		light_fast_mode = light_fast_inline_install(katana_rx_alloc) ? 2 : 0;
		debugPrintf("light fast: %s (inline)\n", light_fast_mode ? "installed" : "NOT installed");
	} else if (exp_flag("lightfast")) {
		extern int light_fast_install(void *(*rx_alloc)(size_t));
		light_fast_mode = light_fast_install(katana_rx_alloc) ? 1 : 0;
		debugPrintf("light fast: %s (hand)\n", light_fast_mode ? "installed" : "NOT installed");
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

	if (fps30 || exp_flag("t30force"))
		t30_install(table);

	// Frame skipping (and its guard stubs) only when opted in with frameSkip=1
	uintptr_t *draw_slot = frame_skip ? so_got_slot(&yoyoloader_mod, "_Z12DoAStep_Drawv") : NULL;
	if (draw_slot) {
		install_draw_skip(table);
		frameskip_orig_draw = (void *)*draw_slot;
		*draw_slot = (uintptr_t)&DoAStep_Draw_frameskip;
	}

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
	if (!exp_flag("texflush0"))
		texture_flush = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z23F_YoYo_DrawTextureFlushR6RValueP9CInstanceS2_iPS_");
}

void patch_katana() {
	patch_performance();

	SaveFileExists = (void *)so_symbol(&yoyoloader_mod, "_ZN8LoadSave14SaveFileExistsEPKcPv");
	BundleFileExists = (void *)so_symbol(&yoyoloader_mod, "_ZN8LoadSave16BundleFileExistsEPKc");
	uintptr_t file_exists = so_symbol(&yoyoloader_mod, "_Z10FileExistsPKc");
	if (file_exists && SaveFileExists && BundleFileExists)
		hook_addr(file_exists, (uintptr_t)&FileExists_hook);

	extern int heavy_profiling;
	if (heavy_profiling) {
		stage_hook("_Z16MainLoop_Processv", (void **)&orig_MainLoop_Process, prof_MainLoop_Process);
		stage_hook("_Z7DoAStepv", (void **)&orig_DoAStep, prof_DoAStep);
		stage_hook("_Z14DoAStep_Updatev", (void **)&orig_DoAStep_Update, prof_DoAStep_Update);
		stage_hook("_Z12DoAStep_Drawv", (void **)&orig_DoAStep_Draw, prof_DoAStep_Draw);
		stage_hook("_Z10HandleStepi", (void **)&orig_HandleStep, prof_HandleStep);
		stage_hook("_Z17UpdateActiveListsv", (void **)&orig_UpdateActiveLists, prof_UpdateActiveLists);
		stage_hook("_Z9Draw_Roomv", (void **)&orig_Draw_Room, prof_Draw_Room);
		stage_hook("_ZN8Graphics5FlushEv", (void **)&orig_Graphics_Flush, prof_Graphics_Flush);
		stage_hook("_Z10Audio_Tickv", (void **)&orig_Audio_Tick, prof_Audio_Tick);
		stage_hook("_Z15ProcessMessagesv", (void **)&orig_ProcessMessages, prof_ProcessMessages);
		stage_hook("_Z15MarkAndSweepGeniib", (void **)&orig_MarkAndSweepGen, prof_MarkAndSweepGen);
		stage_hook("_Z23DrawLayerTilemapElementP9tagYYRECTP6CLayerP20CLayerTilemapElementfff", (void **)&orig_DrawLayerTilemap, prof_DrawLayerTilemap);
		stage_hook("_Z24DrawLayerInstanceElementP9tagYYRECTP6CLayerP21CLayerInstanceElement", (void **)&orig_DrawLayerInstance, prof_DrawLayerInstance);
		stage_hook("_Z26DrawLayerBackgroundElementP9tagYYRECTP6CLayerP23CLayerBackgroundElement", (void **)&orig_DrawLayerBackground, prof_DrawLayerBackground);
		stage_hook("_Z22DrawLayerSpriteElementP9tagYYRECTP6CLayerP19CLayerSpriteElement", (void **)&orig_DrawLayerSprite, prof_DrawLayerSprite);
		stage_hook("_Z20DrawLayerTileElementP9tagYYRECTP6CLayerP17CLayerTileElement", (void **)&orig_DrawLayerTile, prof_DrawLayerTile);
		passthru_hook("DrawRoomLayers", "_Z14DrawRoomLayersP9tagYYRECTi");
		passthru_hook("DrawLayerTilemap", "_Z23DrawLayerTilemapElementP9tagYYRECTP6CLayerP20CLayerTilemapElementfff");
		passthru_hook("DrawLayerBackground", "_Z26DrawLayerBackgroundElementP9tagYYRECTP6CLayerP23CLayerBackgroundElement");
		passthru_hook("DrawLayerInstance", "_Z24DrawLayerInstanceElementP9tagYYRECTP6CLayerP21CLayerInstanceElement");
		passthru_hook("DrawLayerParticle", "_Z24DrawLayerParticleElementP9tagYYRECTP6CLayerP21CLayerParticleElement");
		passthru_hook("DrawLayerSequence", "_Z24DrawLayerSequenceElementP9tagYYRECTP6CLayerP21CLayerSequenceElement");
		passthru_hook("DrawTile", "_Z8DrawTileP9tagYYRECTP8CTilesetjifff");
		passthru_hook("CRoom::UpdateLayers", "_ZN5CRoom12UpdateLayersEv");
		passthru_hook("FilterHost::LayerBegin", "_ZN10FilterHost10LayerBeginEi");
		passthru_hook("FilterHost::LayerEnd", "_ZN10FilterHost8LayerEndEi");
		passthru_hook("draw_self", "_Z15YYGML_draw_selfP9CInstance");
		passthru_hook("draw_sprite_ext", "_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
		passthru_hook("draw_sprite_recolored", "_Z32gml_Script_draw_sprite_recoloredP9CInstanceS0_R8YYRValueiPPS1_");
		passthru_hook("update_region", "_Z24gml_Script_update_regionP9CInstanceS0_R8YYRValueiPPS1_");
		if (exp_flag("scriptprof"))
			scriptprof_hooks();
		if (exp_flag("userprof"))
			userprof_hooks();
		stage_hook("_Z34gml_Script_draw_fastline_width_colP9CInstanceS0_R8YYRValueiPPS1_", (void **)&orig_script_draw_fastline, prof_script_draw_fastline);
		stage_hook("_Z37gml_Script_softbody_constraint_strongP9CInstanceS0_R8YYRValueiPPS1_", (void **)&orig_script_softbody_strong, prof_script_softbody_strong);
		stage_hook("_Z24gml_Script_block_meetingP9CInstanceS0_R8YYRValueiPPS1_", (void **)&orig_script_block_meeting, prof_script_block_meeting);
		stage_hook("_Z24gml_Script_inside_screenP9CInstanceS0_R8YYRValueiPPS1_", (void **)&orig_script_inside_screen, prof_script_inside_screen);
		stage_hook("_Z29gml_Script_update_light_arrayP9CInstanceS0_R8YYRValueiPPS1_", (void **)&orig_script_update_light_array, prof_script_update_light_array);
		stage_hook("_Z26gml_Script_instance_createP9CInstanceS0_R8YYRValueiPPS1_", (void **)&orig_script_instance_create, prof_script_instance_create);
		evprof_install(&yoyoloader_mod);

		// Call counting slows the game noticeably, so it's opt-in via a flag file
		SceIoStat st;
		if (sceIoGetstat("ux0:data/gms/shared/callprof", &st) >= 0)
			callprof_install(&yoyoloader_mod);
	}
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
