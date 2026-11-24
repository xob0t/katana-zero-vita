/*
 * hang_watch.c
 *
 * Freeze locator (debug mode). A playtest froze in the first frame of room_prison_1 after ~29 minutes of play:
 * the log just stopped, no crash dump. The log can't show where the game thread was: the loader prints nothing
 * inside a frame. This thread watches the main loop's heartbeat (hang_frames). When no frame started for
 * HANG_REPORT_S seconds it reports where the game thread is, again every HANG_REPEAT_S seconds while it stays
 * frozen:
 * - the loader stage of the frame (hang_stage, set around the runner's Process, the swap, the texture flush and
 *   the vitaGL garbage collection after it),
 * - the runner's current event (Current_Event_Type/Number, Current_Object) and the builtin called last
 *   (g_pFunction),
 * - the game and vitaGL garbage-collector threads' state: running or waiting, and on which kernel object (by name),
 *   plus whether the game thread used CPU since the last report (a busy loop) or not (blocked),
 * - vitaGL's counters (forced collections, failed allocations) and its GC semaphores, a texture page decode in
 *   progress, save I/O in progress, and newlib heap use.
 *
 * The watchdog never waits on the game's logger: the game thread may be the one stuck inside debugPrintf, or the
 * Log Writer inside sceIoWrite. Reports are formatted into a preallocated buffer and handed to a reporter thread,
 * which appends them to the log buffer only if its lock is free (debug_try_append) and writes them to
 * ux0:data/gms/shared/hang.log with its own file handle.
 *
 * Runner state is read without calling into the runner: object names come from a table built on the game thread
 * at the first room change (hang_watch_names), the room name is noted at each room change, and g_pFunction is only followed when
 * it points at an entry of the builtin table.
 *
 * Time the app spends suspended (home screen) or under the system overlay (quick menu) doesn't count: the stall
 * counter restarts when a 1-s sleep took over HANG_GAP_US of system time, and while _sceAppMgrGetAppState reports
 * the system UI on top.
 *
 * exp_hangdump (opt-in): after HANG_DUMP_S seconds the watchdog executes a trap instruction, so the system writes a
 * core dump with every thread's registers (a .psp2dmp file in ux0:data; parse it with work/tools-vpc, runner at
 * 0x99000000). It doesn't while save I/O is in progress or ended under HANG_IO_QUIET_US ago, and it re-reads the
 * heartbeat right before the trap, so a frame that finally arrives cancels it. Off entirely with exp_hangwatch0.
 * A frame that takes 8 s is a freeze in all but name: room loads take up to ~1.3 s.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "main.h"
#include "so_util.h"
#include "hang_watch.h"

#define HANG_REPORT_S 8
#define HANG_REPEAT_S 30
#define HANG_DUMP_S 30
#define HANG_GAP_US 3000000ull       // a 1-s sleep that took this long: the app was suspended
#define HANG_IO_QUIET_US 5000000u    // no dump this soon after save I/O ended
#define HANG_LOG_PATH "ux0:data/gms/shared/hang.log"

int hang_watch_on;
volatile uint32_t hang_frames;     // main loop iterations, written by the game thread only
volatile int hang_stage;           // HANG_STAGE_* of the game thread
volatile int hang_texture_decode;  // a texture page is being decoded (lz4_page_decompress)
static volatile int hang_io_depth;
static volatile uint32_t hang_io_end_us;

static const char *const stage_names[] = {
	"input", "runner Process", "vglSwapBuffers", "after swap (dynres)", "texture flush", "GPU garbage collection",
	"screenshot check", "frame report", "other",
};

extern SceUID game_thread_id;
extern unsigned _newlib_heap_size;
extern size_t newlib_heap_used(void);
extern int debugMode;
extern int debug_try_append(const char *text, int len);

// vitaGL's forced collection entry/exit events (patches/vitagl.patch since PR #55): a freeze inside a forced collection
// shows as begun > finished. Weak, so the locator builds and skips the line with a vitaGL that doesn't have them.
extern volatile int vgl_forced_gc_runs __attribute__((weak));
extern volatile int vgl_forced_gc_done __attribute__((weak));
extern volatile int vgl_forced_gc_open_scene __attribute__((weak));
extern volatile size_t vgl_forced_gc_size __attribute__((weak));

void hang_io_begin(void) {
	if (hang_watch_on)
		__atomic_add_fetch(&hang_io_depth, 1, __ATOMIC_ACQ_REL);
}

void hang_io_end(void) {
	if (!hang_watch_on)
		return;
	__atomic_store_n(&hang_io_end_us, (uint32_t)sceKernelGetSystemTimeWide(), __ATOMIC_RELEASE);
	__atomic_sub_fetch(&hang_io_depth, 1, __ATOMIC_ACQ_REL);
}

// Files the runner opened for writing (KatanaSave.zero, options...): save I/O until they are closed
#define HANG_IO_FILES 8
static void *volatile io_files[HANG_IO_FILES];

void hang_io_file_open(void *file, const char *mode) {
	if (!hang_watch_on || !file || !mode || !strpbrk(mode, "wa+"))
		return;
	for (int i = 0; i < HANG_IO_FILES; i++) {
		void *expected = NULL;
		if (__atomic_compare_exchange_n(&io_files[i], &expected, file, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			hang_io_begin();
			return;
		}
	}
}

void hang_io_file_close(void *file) {
	if (!hang_watch_on || !file)
		return;
	for (int i = 0; i < HANG_IO_FILES; i++) {
		void *expected = file;
		if (__atomic_compare_exchange_n(&io_files[i], &expected, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			hang_io_end();
			return;
		}
	}
}

static int save_io_recent(void) {
	if (__atomic_load_n(&hang_io_depth, __ATOMIC_ACQUIRE) > 0)
		return 1;
	uint32_t end = __atomic_load_n(&hang_io_end_us, __ATOMIC_ACQUIRE);
	return end && (uint32_t)sceKernelGetSystemTimeWide() - end < HANG_IO_QUIET_US;
}

// Immutable snapshots taken on the game thread
static const char **object_names;
static int object_count;
static const char *const *builtin_base; // the_functions: entries of {name, func, argc}
static int builtin_count;
static volatile const char *room_name_now;

void hang_watch_names(void) {
	if (!hang_watch_on || object_names)
		return;
	int *count = (int *)so_symbol(&yoyoloader_mod, "g_ObjectNumber");
	const char *(*object_name)(int) = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_Namei");
	if (count && object_name && *count > 0 && *count < 100000) {
		const char **names = calloc(*count, sizeof(*names));
		for (int i = 0; names && i < *count; i++)
			names[i] = object_name(i);
		object_count = names ? *count : 0;
		object_names = names;
	}
	const char *const **functions = (const char *const **)so_symbol(&yoyoloader_mod, "the_functions");
	int *numb = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	if (functions && *functions && numb) {
		builtin_base = *functions;
		builtin_count = *numb;
	}
}

void hang_watch_room(int room) {
	static const char *(*room_name)(int);
	if (!hang_watch_on)
		return;
	if (!room_name)
		room_name = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_Namei");
	hang_watch_names(); // the object table is loaded by the first room

	room_name_now = room >= 0 && room_name ? room_name(room) : NULL;
}

// The builtin g_pFunction points at, if it points at an entry of the_functions (12 bytes each)
static const char *builtin_name(const void *entry) {
	uintptr_t p = (uintptr_t)entry, base = (uintptr_t)builtin_base;
	if (!base || p < base || p >= base + (uintptr_t)builtin_count * 12 || (p - base) % 12)
		return NULL;
	return *(const char *const *)entry;
}

// Report text: formatted by the watchdog, written out by the reporter thread
static char report[4096];
static int report_len;
static volatile int report_busy;
static SceUID report_sema = -1;

static void say(const char *fmt, ...) {
	if (report_len >= (int)sizeof(report) - 1)
		return;
	uint32_t ms = sceKernelGetProcessTimeLow() / 1000;
	int n = snprintf(report + report_len, sizeof(report) - report_len, "[%5u.%03u] [HANG] ", ms / 1000, ms % 1000);
	if (n > 0)
		report_len += n;
	if (report_len >= (int)sizeof(report) - 1) {
		report_len = sizeof(report) - 1;
		return;
	}
	va_list list;
	va_start(list, fmt);
	n = vsnprintf(report + report_len, sizeof(report) - report_len, fmt, list);
	va_end(list);
	if (n > 0)
		report_len += n;
	if (report_len > (int)sizeof(report) - 1)
		report_len = sizeof(report) - 1;
}

// Starts a report; 0 if the previous one is still being written (it is skipped, nothing waits)
static int report_begin(void) {
	if (__atomic_load_n(&report_busy, __ATOMIC_ACQUIRE))
		return 0;
	report_len = 0;
	return 1;
}

static void report_send(void) {
	__atomic_store_n(&report_busy, 1, __ATOMIC_RELEASE);
	sceKernelSignalSema(report_sema, 1);
}

static int reporter_thread(SceSize args, void *argp) {
	for (;;) {
		if (sceKernelWaitSema(report_sema, 1, NULL) < 0)
			return 0;
		debug_try_append(report, report_len); // skipped if the logger is busy (maybe the stuck part)
		SceUID fd = sceIoOpen(HANG_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
		if (fd >= 0) {
			sceIoWrite(fd, report, report_len);
			sceIoClose(fd);
		}
		__atomic_store_n(&report_busy, 0, __ATOMIC_RELEASE);
	}
	return 0;
}

static void kernel_object_name(SceUID uid, char *out, size_t len) {
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
		say("%s thread: no info\n", label);
		return;
	}
	char wait[96] = "-";
	if (info.waitId > 0)
		kernel_object_name(info.waitId, wait, sizeof(wait));
	uint64_t clocks = info.runClocks;
	say("%s thread '%s': status 0x%x wait type 0x%x on %s | cpu %d priority %d | ran %.1f ms since the last report\n",
		label, info.name, info.status, info.waitType, wait, info.currentCpuId, info.currentPriority,
		*last_clocks ? (clocks - *last_clocks) / 1000.0f : -1.0f);
	*last_clocks = clocks;
}

// The system UI (quick menu, notifications) on top of the app; -1 if unknown
static int app_overlaid(void) {
	SceAppMgrAppState state;
	memset(&state, 0, sizeof(state));
	if (_sceAppMgrGetAppState(&state, sizeof(state), 0x03570011) < 0)
		return -1;
	return state.isSystemUiOverlaid != 0;
}

static int hang_watch_thread(SceSize args, void *argp) {
	int *event_type = (int *)so_symbol(&yoyoloader_mod, "Current_Event_Type");
	int *event_number = (int *)so_symbol(&yoyoloader_mod, "Current_Event_Number");
	int *current_object = (int *)so_symbol(&yoyoloader_mod, "Current_Object");
	void **current_function = (void **)so_symbol(&yoyoloader_mod, "g_pFunction");
	int *current_room = (int *)so_symbol(&yoyoloader_mod, "Current_Room");
	extern SceUID gc_thread;
	extern SceUID gc_mutex[2];
	extern int vgl_forced_gc_allocs, vgl_failed_allocs;
	int dump = exp_flag("hangdump");
	uint32_t last = __atomic_load_n(&hang_frames, __ATOMIC_ACQUIRE), still = 0, reports = 0;
	uint64_t game_clocks = 0, gc_clocks = 0, last_sys = sceKernelGetSystemTimeWide();
	for (;;) {
		sceKernelDelayThread(1000000);
		uint64_t sys = sceKernelGetSystemTimeWide();
		uint64_t gap = sys - last_sys;
		last_sys = sys;
		uint32_t now = __atomic_load_n(&hang_frames, __ATOMIC_ACQUIRE);
		// Suspended (the sleep took far longer than 1 s), or the system UI is on top: start over
		int paused = gap > HANG_GAP_US || app_overlaid() == 1;
		if (now != last || paused) {
			if (reports && report_begin()) {
				say("frames again after %u s (frame %u)%s\n", still, now, paused ? ", or the app was paused" : "");
				report_send();
			}
			last = now;
			still = reports = 0;
			continue;
		}
		still++;
		if (still == HANG_REPORT_S || (still > HANG_REPORT_S && (still - HANG_REPORT_S) % HANG_REPEAT_S == 0)) {
			if (report_begin()) {
				reports++;
				int stage = __atomic_load_n(&hang_stage, __ATOMIC_ACQUIRE);
				int room = current_room ? *current_room : -1;
				const char *room_name = (const char *)room_name_now;
				say("no frame for %u s at frame %u: loader stage %d (%s), room %d %s\n", still, now, stage,
					stage_names[stage >= 0 && stage < (int)(sizeof(stage_names) / sizeof(*stage_names)) ? stage : 8],
					room, room_name ? room_name : "?");
				int obj = current_object ? *current_object : -1;
				const char *fn = current_function ? builtin_name(*current_function) : NULL;
				say("runner: event type %d number %d object %d %s | last builtin %s\n", event_type ? *event_type : -1,
					event_number ? *event_number : -1, obj,
					obj >= 0 && obj < object_count && object_names[obj] ? object_names[obj] : "-", fn ? fn : "-");
				thread_report("game", game_thread_id, &game_clocks);
				thread_report("vitaGL GC", gc_thread, &gc_clocks);
				char push[96], pull[96];
				kernel_object_name(gc_mutex[0], push, sizeof(push));
				kernel_object_name(gc_mutex[1], pull, sizeof(pull));
				SceKernelFreeMemorySizeInfo kfree;
				kfree.size = sizeof(kfree);
				sceKernelGetFreeMemorySize(&kfree);
				say("vitaGL: forced collections %d, failed allocations %d | GC %s, %s | texture decode in progress %d | "
					"save I/O %d | newlib heap %u of %u KB used | kernel free user %u KB\n", vgl_forced_gc_allocs,
					vgl_failed_allocs, push, pull, hang_texture_decode, hang_io_depth,
					(unsigned)(newlib_heap_used() / 1024), _newlib_heap_size / 1024, kfree.size_user / 1024);
				if (&vgl_forced_gc_runs && &vgl_forced_gc_done && &vgl_forced_gc_open_scene && &vgl_forced_gc_size) {
					int runs = vgl_forced_gc_runs, done = vgl_forced_gc_done;
					say("vitaGL forced collections: %d begun, %d finished%s, %d begun with a scene open, last for %u KB\n",
						runs, done, runs != done ? " (STUCK INSIDE ONE)" : "", vgl_forced_gc_open_scene,
						(unsigned)(vgl_forced_gc_size / 1024));
				}
				report_send();
			}
		}
		// The dump doesn't depend on the report having been written
		if (dump && still >= HANG_DUMP_S && !save_io_recent()) {
			if (report_begin()) {
				say("exp_hangdump: no frame for %u s, trapping for a core dump\n", still);
				report_send();
				// Give the reporter a moment; the dump goes ahead either way
				for (int i = 0; i < 20 && __atomic_load_n(&report_busy, __ATOMIC_ACQUIRE); i++)
					sceKernelDelayThread(50000);
			}
			// A frame or a save may have started meanwhile: check right before the trap
			if (__atomic_load_n(&hang_frames, __ATOMIC_ACQUIRE) == last && !save_io_recent())
				__builtin_trap();
		}
	}
	return 0;
}

void hang_watch_start(int cpu_mask) {
	if (!debugMode || exp_flag("hangwatch0"))
		return;
	report_sema = sceKernelCreateSema("Hang Report", 0, 0, 1, NULL);
	SceUID reporter = report_sema >= 0 ? sceKernelCreateThread("Hang Report", reporter_thread, 0x10000100, 0x4000, 0,
		cpu_mask, NULL) : -1;
	if (reporter < 0 || sceKernelStartThread(reporter, 0, NULL) < 0)
		return;
	hang_watch_on = 1; // before the watchdog starts: the heartbeat must already be running when it looks
	SceUID th = sceKernelCreateThread("Hang Watch", hang_watch_thread, 0x10000100, 0x4000, 0, cpu_mask, NULL);
	if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
		hang_watch_on = 0;
		return;
	}
	debugPrintf("freeze locator: on (report after %d s without a frame%s; app state query %d)\n", HANG_REPORT_S,
		exp_flag("hangdump") ? ", core dump after 30 s (exp_hangdump)" : ", no core dump", app_overlaid());
}
