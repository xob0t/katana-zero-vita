/*
 * katana_prof.c
 *
 * Debug-mode profiling: main loop stages, layer and per-object draws, scripts, user events and
 * builtins, and the periodic report (katana_prof_report).
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

// The generation this frame's collection worked on (-1: none), for the spike log
static int gc_frame_gen = -1;
static uint32_t stage_time[S_COUNT];
static uint32_t stage_calls[S_COUNT];

// Called once per frame by the main loop: with log set (a slow frame under exp_spikes) prints the stages that took
// over 0.3 ms in that frame. The totals are reset at each report, so a total below the previous one restarts.
void katana_stage_frame(int log) {
	static uint32_t prev[S_COUNT];
	if (log) {
		char line[512];
		int n = 0;
		for (int i = 0; i < S_COUNT && n < (int)sizeof(line) - 48; i++) {
			uint32_t d = stage_time[i] >= prev[i] ? stage_time[i] - prev[i] : stage_time[i];
			if (d >= 300)
				n += snprintf(line + n, sizeof(line) - n, " %s %.1f", stage_names[i], d / 1000.0f);
		}
		debugPrintf("[SPIKE]   stages:%s | gc generation %d\n", n ? line : " (none over 0.3 ms)", gc_frame_gen);
	}
	gc_frame_gen = -1;
	memcpy(prev, stage_time, sizeof(prev));
}

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
static int prof_MarkAndSweepGen(int a, int b, int c) {
	STAGE_BEGIN;
	int r = orig_MarkAndSweepGen(a, b, c);
	STAGE_END(S_GC);
	gc_frame_gen = a;
	return r;
}

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

// exp_lightfast / exp_lightfast_inline: 0 = original events, 1 = hand native, 2 = inlined variant
int light_fast_mode;

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
	extern unsigned t30_letter_skips, t30_search_ticks, t30_enemy_steps, t30_textbox_ticks[2];
	if (t30_letter_skips)
		debugPrintf("[FPS30] %u letter steps skipped on frozen ticks\n", t30_letter_skips);
	if (t30_enemy_steps)
		debugPrintf("[FPS30] enemy steps at 30 ticks/s %u, search ticks added %u\n", t30_enemy_steps, t30_search_ticks);
	{
		// Half-rate enemies, per frame of this report window
		extern unsigned enemyhalf_runs, enemyhalf_doubled, enemyhalf_skips, enemyhalf_forced, enemyhalf_kills[4];
		static unsigned last[4];
		unsigned now[4] = { enemyhalf_runs, enemyhalf_doubled, enemyhalf_skips, enemyhalf_forced };
		if (debugMode && frames > 0 && (now[0] != last[0] || now[2] != last[2]))
			debugPrintf("[FPS30] half-rate enemies per tick: %.1f steps run (%.1f covering 2 ticks), %.1f skipped, %.2f run "
				"for a hurt overlap | search ticks added %u | deaths: other event %u, overlap run %u, 2-tick %u, 1-tick %u\n",
				(float)(now[0] - last[0]) / frames, (float)(now[1] - last[1]) / frames, (float)(now[2] - last[2]) / frames,
				(float)(now[3] - last[3]) / frames, t30_search_ticks, enemyhalf_kills[0], enemyhalf_kills[1],
				enemyhalf_kills[2], enemyhalf_kills[3]);
		memcpy(last, now, sizeof(last));
	}
	extern unsigned t30_textbox_doubled;
	if (t30_textbox_ticks[0] || t30_textbox_ticks[1])
		debugPrintf("[FPS30] textbox ticks at 30 ticks/s: %u, passive %u (typing x2 %u)\n", t30_textbox_ticks[0],
			t30_textbox_ticks[1], t30_textbox_doubled);
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
		extern unsigned letter_native_direct_user, letter_native_cached_slots;
		if (letter_native_direct_user || letter_native_cached_slots)
			debugPrintf("[LETTER] direct user events %u, cached lookups %u\n", letter_native_direct_user, letter_native_cached_slots);
		extern unsigned letter_slot_miss[5];
		extern int letter_timing;
		if (letter_timing)
			debugPrintf("[LETTER] slot misses: empty %u other %u id %u entries %u mask %u\n", letter_slot_miss[0], letter_slot_miss[1],
			letter_slot_miss[2], letter_slot_miss[3], letter_slot_miss[4]);
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
		extern unsigned dossier_circle_fast, dossier_circle_slow, dossier_spawner_fast, dossier_spawner_slow;
		extern unsigned dossier_spd_cached, dossier_spd_looked_up;
		if (dossier_circle_fast || dossier_circle_slow || dossier_spawner_fast || dossier_spawner_slow)
			debugPrintf("[DOSSIER] circle steps %u/%u spawner steps %u/%u (native/original), fire_spd %u cached %u looked up (totals)\n",
				dossier_circle_fast, dossier_circle_slow, dossier_spawner_fast, dossier_spawner_slow, dossier_spd_cached,
				dossier_spd_looked_up);
		extern int dossier_fire_spd(double *out);
		double spd;
		if (dossier_fire_spd(&spd)) {
			static int *fr;
			if (!fr)
				fr = (int *)so_symbol(&yoyoloader_mod, "g_VAR_fr");
			debugPrintf("[DOSSIER] fire_spd %.4f global.fr %.4f\n", spd, fr ? global_real(fr, -1.0) : -1.0);
		}
		extern unsigned dossier_children_dropped, dossier_children_kept;
		if (dossier_children_dropped || dossier_children_kept)
			debugPrintf("[DOSSIER] decorative circles dropped %u kept %u (totals)\n", dossier_children_dropped, dossier_children_kept);
		extern uint64_t letter_time[6];
		extern unsigned letter_time_n[6];
		if (letter_time_n[0]) {
			static const char *names[5] = {"draw", "admit", "text", "font", "user"};
			for (int i = 0; i < 5; i++)
				debugPrintf("[LETTERTIME] %-5s %8u calls %6.2f us each\n", names[i], letter_time_n[i],
					letter_time_n[i] ? (double)letter_time[i] / letter_time_n[i] : 0.0);
			memset(letter_time, 0, sizeof(letter_time));
			memset(letter_time_n, 0, sizeof(letter_time_n));
		}
		extern unsigned wavy_native_fast, wavy_native_slow, wavy_native_strips;
		if (wavy_native_fast || wavy_native_slow)
			debugPrintf("[WAVY] calls %u/%u (native/original), strips %u (totals)\n", wavy_native_fast, wavy_native_slow,
				wavy_native_strips);
		extern unsigned wavy_reject[16], wavy_reject_kind;
		for (int i = 0; i < 14; i++)
			if (wavy_reject[i])
				debugPrintf("[WAVY] reject %d x%u (kind %u)\n", i, wavy_reject[i], wavy_reject_kind);
		extern unsigned wavy_verify_calls, wavy_verify_mismatch;
		if (wavy_verify_calls)
			debugPrintf("[WAVY] verify: %u strips compared, %u mismatches\n", wavy_verify_calls, wavy_verify_mismatch);

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
		extern unsigned trail_lights_kept, trail_lights_skipped, trail_lights_violations;
		extern unsigned trail_lights_native, trail_lights_fallback, trail_lights_verified, trail_lights_mismatch;
		static unsigned last_kept, last_skipped, last_native, last_fallback;
		extern unsigned trail_lights_reject_line[8], trail_lights_reject_count[8];
		if (frames > 0 && (trail_lights_kept != last_kept || trail_lights_skipped != last_skipped || trail_lights_violations)) {
			debugPrintf("[TRAILLIGHTS] lights per frame: %.1f near, %.1f skipped | near: %.1f native, %.1f original | "
				"check violations %u | verify ok %u mismatch %u\n",
				(float)(trail_lights_kept - last_kept) / frames, (float)(trail_lights_skipped - last_skipped) / frames,
				(float)(trail_lights_native - last_native) / frames, (float)(trail_lights_fallback - last_fallback) / frames,
				trail_lights_violations, trail_lights_verified, trail_lights_mismatch);
			for (int k = 0; k < 8 && trail_lights_reject_line[k]; k++)
				debugPrintf("[TRAILLIGHTS] native body fell back at trail_lights.c:%u x%u\n", trail_lights_reject_line[k],
					trail_lights_reject_count[k]);
		}
		last_kept = trail_lights_kept;
		last_skipped = trail_lights_skipped;
		last_native = trail_lights_native;
		last_fallback = trail_lights_fallback;
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
		// exp_laserprof: the native draw's time, and how much of it is draw_sprite_ext; exp_laserab: its fast path is
		// off in every other report window (in-run A/B)
		extern int laser_prof, laser_ab, laser_ab_off;
		extern unsigned laser_prof_us, laser_prof_sprite_us, laser_prof_calls, laser_prof_segments;
		if (laser_prof && frames > 0) {
			debugPrintf("[LASERPROF] native draw_laser %.3f ms/frame in %.2f calls, draw_sprite_ext %.3f ms in %.1f segments%s\n",
				laser_prof_us / 1000.0f / frames, (float)laser_prof_calls / frames, laser_prof_sprite_us / 1000.0f / frames,
				(float)laser_prof_segments / frames, laser_ab ? (laser_ab_off ? " | A/B: off" : " | A/B: on") : "");
			laser_prof_us = laser_prof_sprite_us = laser_prof_calls = laser_prof_segments = 0;
		}
		if (laser_ab)
			laser_ab_off = !laser_ab_off;
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
	extern void fx_rate_report(int frames);
	fx_rate_report(frames);
	extern void glow_rest_report(int frames);
	glow_rest_report(frames);
	extern void wobble_rest_report(int frames);
	wobble_rest_report(frames);
	extern void blood_drops_report(int frames);
	blood_drops_report(frames);
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
		static const char *(*object_name)(int);
		if (!object_name)
			object_name = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_Namei");
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


// Called from patch_katana
void katana_prof_install(void) {
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
