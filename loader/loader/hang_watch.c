/*
 * hang_watch.c
 *
 * Freeze locator (debug mode). A playtest froze in the first frame of room_prison_1 after ~29 minutes of play:
 * the log just stopped, no crash dump. The log can't show where the game thread was: the loader prints nothing
 * inside a frame. This thread watches the main loop's frame counter. When no frame finished for HANG_REPORT_S
 * seconds it logs where the game thread is and flushes the log to the memory card:
 * - the loader stage of the frame (hang_stage, set around the runner's Process, the swap, the texture flush and
 *   the vitaGL garbage collection after it),
 * - the runner's current event (Current_Event_Type/Number, Current_Object) and the builtin called last
 *   (g_pFunction),
 * - the game and vitaGL garbage-collector threads' state: running or waiting, and on which kernel object (by name),
 *   plus whether the game thread used CPU since the last report (a busy loop) or not (blocked),
 * - vitaGL's counters (forced collections, failed allocations) and its GC semaphores, a texture page decode in progress,
 *   and newlib heap use.
 * After HANG_DUMP_S seconds without a frame the watchdog crashes the game on purpose, so the system writes a
 * core dump with every thread's registers (a .psp2dmp file in ux0:data; parse it with work/tools-vpc, runner at
 * 0x99000000): the game is frozen by then and has to be closed anyway, and the dump shows exactly where.
 * exp_hangdump dumps right after the first report, exp_hangdump0 never dumps (the report then repeats every
 * HANG_REPEAT_S seconds). A frame that takes 8 s is a freeze in all but name: room loads take up to ~1.3 s.
 * Off with exp_hangwatch0.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "main.h"
#include "so_util.h"

#define HANG_REPORT_S 8
#define HANG_REPEAT_S 30
#define HANG_DUMP_S 30

volatile uint32_t hang_frames;     // main loop iterations, set by the game thread
volatile int hang_stage;           // HANG_STAGE_* of the game thread
volatile int hang_texture_decode;  // a texture page is being decoded (lz4_page_decompress)

static const char *const stage_names[] = {
	"input", "runner Process", "vglSwapBuffers", "after swap (t30/dynres)", "texture flush", "GPU garbage collection",
	"screenshot check", "frame report", "other",
};

extern SceUID game_thread_id;
extern unsigned _newlib_heap_size;
extern size_t newlib_heap_used(void);
extern int debugMode;

static void object_name_of(SceUID uid, char *out, size_t len) {
	SceKernelSemaInfo sema;
	sema.size = sizeof(sema);
	if (sceKernelGetSemaInfo(uid, &sema) >= 0) {
		snprintf(out, len, "sema '%s' count %d/%d waiters %d", sema.name, sema.currentCount, sema.maxCount,
			sema.numWaitThreads);
		return;
	}
	SceKernelMutexInfo mutex;
	mutex.size = sizeof(mutex);
	if (sceKernelGetMutexInfo(uid, &mutex) >= 0) {
		snprintf(out, len, "mutex '%s' locked %d by 0x%x", mutex.name, mutex.currentCount, mutex.currentOwnerId);
		return;
	}
	SceKernelEventFlagInfo flag;
	flag.size = sizeof(flag);
	if (sceKernelGetEventFlagInfo(uid, &flag) >= 0) {
		snprintf(out, len, "event flag '%s' pattern 0x%x", flag.name, flag.currentPattern);
		return;
	}
	snprintf(out, len, "uid 0x%x", uid);
}

static void thread_report(const char *label, SceUID thid, uint64_t *last_clocks) {
	SceKernelThreadInfo info;
	info.size = sizeof(info);
	if (thid <= 0 || sceKernelGetThreadInfo(thid, &info) < 0) {
		debugPrintf("[HANG] %s thread: no info\n", label);
		return;
	}
	char wait[96] = "-";
	if (info.waitId > 0)
		object_name_of(info.waitId, wait, sizeof(wait));
	uint64_t clocks = info.runClocks;
	debugPrintf("[HANG] %s thread '%s': status 0x%x wait type 0x%x on %s | cpu %d priority %d | ran %.1f ms since the last "
		"report\n", label, info.name, info.status, info.waitType, wait, info.currentCpuId, info.currentPriority,
		last_clocks && *last_clocks ? (clocks - *last_clocks) / 1000.0f : -1.0f);
	if (last_clocks)
		*last_clocks = clocks;
}

static int hang_watch_thread(SceSize args, void *argp) {
	int *event_type = (int *)so_symbol(&yoyoloader_mod, "Current_Event_Type");
	int *event_number = (int *)so_symbol(&yoyoloader_mod, "Current_Event_Number");
	int *current_object = (int *)so_symbol(&yoyoloader_mod, "Current_Object");
	void **current_function = (void **)so_symbol(&yoyoloader_mod, "g_pFunction");
	int *current_room = (int *)so_symbol(&yoyoloader_mod, "Current_Room");
	const char *(*object_name)(int) = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_Namei");
	const char *(*room_name)(int) = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_Namei");
	extern SceUID gc_thread;
	extern SceUID gc_mutex[2];
	extern int vgl_forced_gc_allocs, vgl_failed_allocs;
	// Seconds without a frame before the core dump (0: never)
	int dump = exp_flag("hangdump0") ? 0 : exp_flag("hangdump") ? HANG_REPORT_S : HANG_DUMP_S;
	uint32_t last = hang_frames, still = 0, reports = 0;
	uint64_t game_clocks = 0, gc_clocks = 0;
	for (;;) {
		sceKernelDelayThread(1000000);
		uint32_t now = hang_frames;
		if (now != last) {
			if (reports)
				debugPrintf("[HANG] frames again after %u s (frame %u)\n", still, now);
			last = now;
			still = reports = 0;
			continue;
		}
		still++;
		int dump_now = dump && still == (uint32_t)dump;
		if (!dump_now && still != HANG_REPORT_S && (still < HANG_REPORT_S || (still - HANG_REPORT_S) % HANG_REPEAT_S))
			continue;
		reports++;
		int stage = hang_stage;
		int room = current_room ? *current_room : -1;
		debugPrintf("[HANG] no frame for %u s at frame %u: loader stage %d (%s), room %d %s\n", still, now, stage,
			stage_names[stage >= 0 && stage < (int)(sizeof(stage_names) / sizeof(*stage_names)) ? stage : 8], room,
			room >= 0 && room_name ? room_name(room) : "?");
		int obj = current_object ? *current_object : -1;
		const char *fn = current_function && *current_function ? *(const char **)*current_function : NULL;
		debugPrintf("[HANG] runner: event type %d number %d object %d %s | last builtin %s\n",
			event_type ? *event_type : -1, event_number ? *event_number : -1, obj,
			obj >= 0 && object_name ? object_name(obj) : "-", fn ? fn : "-");
		thread_report("game", game_thread_id, &game_clocks);
		thread_report("vitaGL GC", gc_thread, &gc_clocks);
		char push[96], pull[96];
		object_name_of(gc_mutex[0], push, sizeof(push));
		object_name_of(gc_mutex[1], pull, sizeof(pull));
		SceKernelFreeMemorySizeInfo kfree;
		kfree.size = sizeof(kfree);
		sceKernelGetFreeMemorySize(&kfree);
		debugPrintf("[HANG] vitaGL: forced collections %d, failed allocations %d | GC %s, %s | texture "
			"decode in progress %d | newlib heap %u of %u KB used | kernel free user %u KB\n", vgl_forced_gc_allocs,
			vgl_failed_allocs, push, pull, hang_texture_decode,
			(unsigned)(newlib_heap_used() / 1024), _newlib_heap_size / 1024, kfree.size_user / 1024);
		debug_flush();
		if (dump_now) {
			debugPrintf("[HANG] crashing on purpose for a core dump (exp_hangdump0 turns this off)\n");
			debug_flush();
			sceKernelDelayThread(200000);
			*(volatile int *)0 = 0;
		}
	}
	return 0;
}

void hang_watch_start(int cpu_mask) {
	if (!debugMode || exp_flag("hangwatch0"))
		return;
	SceUID th = sceKernelCreateThread("Hang Watch", hang_watch_thread, 0x10000100, 0x4000, 0, cpu_mask, NULL);
	if (th >= 0 && sceKernelStartThread(th, 0, NULL) >= 0)
		debugPrintf("freeze locator: on (report after %d s without a frame, core dump after %d s)\n", HANG_REPORT_S,
			exp_flag("hangdump0") ? 0 : exp_flag("hangdump") ? HANG_REPORT_S : HANG_DUMP_S);
}
