/*
 * boot_watch.c
 *
 * exp_bootwatch: startup stall locator. A cold launch sometimes stopped for good inside the runner's Startup, right
 * after the audio groups started loading ("Audio group N -> Loading...", "OS language detected"), with no crash dump.
 * The freeze locator (hang_watch.c) only starts with the main loop, so nothing reported it.
 *
 * With the switch, a watchdog thread starts right after yyl.cfg is read. Until the main loop starts, a stall is no new
 * log line for BW_REPORT_S seconds (a normal boot's longest gap is ~3.7 s, the WAD parse). It then writes a report
 * of every thread of the process to ux0:data/gms/shared/hang.log with its own file handle (never through the logger,
 * which may be the stuck part): name, status, what it waits on (kernel object name and state), CPU time used since the
 * last report, priority, stack, and for threads the runner created (pthread_create) the runner function they run.
 * Threads are found by asking for the info of every UID near the game thread's (ours answer, other processes' don't).
 * After BW_DUMP_S seconds it executes a trap instruction, so the system writes a core dump with every thread's
 * registers (.psp2dmp files in ux0:data; parse with work/tools-vpc, runner at 0x99000000). The main loop's start ends it.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "main.h"
#include "so_util.h"

#define BW_REPORT_S 8
#define BW_DUMP_S 20
#define BW_LOG_PATH "ux0:data/gms/shared/hang.log"
#define BW_MAX_THREADS 48
#define BW_SCAN 0x8000 // UIDs scanned on each side of the game thread's

extern int setup_ended;
extern uint32_t debug_printf_calls;

int bw_on; // exp_bootwatch: extra startup log lines elsewhere
static SceUID bw_game;

// Threads with a known start function (runner pthreads, loader workers)
static struct {
	SceUID id;
	const char *label;
	uintptr_t start;
} bw_known[BW_MAX_THREADS];
static int bw_known_n;

void boot_watch_thread(const char *label, void *start) {
	if (!bw_on)
		return;
	int i = __atomic_fetch_add(&bw_known_n, 1, __ATOMIC_ACQ_REL);
	if (i >= BW_MAX_THREADS)
		return;
	bw_known[i].label = label;
	bw_known[i].start = (uintptr_t)start;
	__atomic_store_n(&bw_known[i].id, sceKernelGetThreadId(), __ATOMIC_RELEASE);
}

static char out[16384];
static int out_len;

static void say(const char *fmt, ...) {
	if (out_len >= (int)sizeof(out) - 1)
		return;
	uint32_t ms = sceKernelGetProcessTimeLow() / 1000;
	int n = snprintf(out + out_len, sizeof(out) - out_len, "[%5u.%03u] [BOOTWATCH] ", ms / 1000, ms % 1000);
	if (n > 0)
		out_len += n;
	if (out_len >= (int)sizeof(out) - 1) {
		out_len = sizeof(out) - 1;
		return;
	}
	va_list list;
	va_start(list, fmt);
	n = vsnprintf(out + out_len, sizeof(out) - out_len, fmt, list);
	va_end(list);
	if (n > 0)
		out_len += n;
	if (out_len > (int)sizeof(out) - 1)
		out_len = sizeof(out) - 1;
}

static void flush_out(void) {
	SceUID fd = sceIoOpen(BW_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, out, out_len);
		sceIoClose(fd);
	}
	out_len = 0;
}

static void object_name(SceUID uid, char *buf, size_t len) {
	SceKernelSemaInfo sema;
	sema.size = sizeof(sema);
	if (sceKernelGetSemaInfo(uid, &sema) >= 0) {
		snprintf(buf, len, "sema '%s' count %d/%d waiters %d", sema.name, sema.currentCount, sema.maxCount,
			sema.numWaitThreads);
		return;
	}
	SceKernelMutexInfo mutex;
	mutex.size = sizeof(mutex);
	if (sceKernelGetMutexInfo(uid, &mutex) >= 0) {
		snprintf(buf, len, "mutex '%s' locked %d by 0x%x waiters %d", mutex.name, mutex.currentCount,
			mutex.currentOwnerId, mutex.numWaitThreads);
		return;
	}
	SceKernelEventFlagInfo flag;
	flag.size = sizeof(flag);
	if (sceKernelGetEventFlagInfo(uid, &flag) >= 0) {
		snprintf(buf, len, "event flag '%s' pattern 0x%x waiters %d", flag.name, flag.currentPattern,
			flag.numWaitThreads);
		return;
	}
	snprintf(buf, len, "uid 0x%x", uid);
}

static struct {
	SceUID id;
	uint64_t clocks;
} seen[BW_MAX_THREADS];
static int seen_n;

static void thread_line(SceUID id, const SceKernelThreadInfo *info) {
	char wait[128] = "-";
	if (info->waitId > 0)
		object_name(info->waitId, wait, sizeof(wait));
	uint64_t last = 0;
	int k;
	for (k = 0; k < seen_n && seen[k].id != id; k++)
		;
	if (k < seen_n)
		last = seen[k].clocks;
	else if (seen_n < BW_MAX_THREADS)
		seen[seen_n++].id = id;
	if (k < BW_MAX_THREADS)
		seen[k].clocks = info->runClocks;
	const char *label = id == bw_game ? "game" : NULL;
	uintptr_t start = 0;
	for (int i = 0; i < bw_known_n && i < BW_MAX_THREADS; i++)
		if (__atomic_load_n(&bw_known[i].id, __ATOMIC_ACQUIRE) == id) {
			label = bw_known[i].label;
			start = bw_known[i].start;
		}
	uintptr_t base = (uintptr_t)yoyoloader_mod.text_base;
	char where[64] = "";
	if (start && start >= base && start < base + yoyoloader_mod.text_size)
		snprintf(where, sizeof(where), " start runner+0x%x", (unsigned)(start - base));
	else if (start)
		snprintf(where, sizeof(where), " start %p", (void *)start);
	say("0x%x '%s'%s%s%s: status 0x%x wait type 0x%x on %s | cpu %d prio %d/%d | ran %.1f ms%s | stack %p+0x%x, "
		"entry %p\n", id, info->name, label ? " [" : "", label ? label : "", label ? "]" : "", info->status,
		info->waitType, wait, info->currentCpuId, info->currentPriority, info->initPriority,
		last ? (info->runClocks - last) / 1000.0f : info->runClocks / 1000.0f, last ? " since the last report" : " total",
		info->stack, info->stackSize, info->entry);
	if (where[0])
		say("  0x%x:%s\n", id, where);
}

static void report(unsigned still) {
	say("no log line for %u s before the main loop (log lines %u)\n", still, debug_printf_calls);
	int found = 0;
	SceKernelThreadInfo info;
	for (int d = -BW_SCAN; d <= BW_SCAN; d++) {
		SceUID id = bw_game + d;
		info.size = sizeof(info);
		if (sceKernelGetThreadInfo(id, &info) < 0)
			continue;
		thread_line(id, &info);
		found++;
	}
	extern void apk_prefetch_report(void (*say)(const char *fmt, ...));
	apk_prefetch_report(say);
	extern volatile int mt_lock_owner, pf_lock_owner;
	extern void log_mutex_words(uint32_t *log, uint32_t *android);
	uint32_t lw[2][8];
	log_mutex_words(lw[0], lw[1]);
	say("apk FILE lock (stdio_mt) held by 0x%x, prefetch lock by 0x%x | log mutex words %08x %08x %08x %08x, android log "
		"mutex words %08x %08x %08x %08x\n", mt_lock_owner, pf_lock_owner, lw[0][0], lw[0][1], lw[0][2], lw[0][3], lw[1][0],
		lw[1][1], lw[1][2], lw[1][3]);
	say("%d threads\n", found);
	flush_out();
}

static int boot_watch_main(SceSize args, void *argp) {
	uint32_t last = __atomic_load_n(&debug_printf_calls, __ATOMIC_ACQUIRE);
	unsigned still = 0;
	int reported = 0;
	for (;;) {
		sceKernelDelayThread(1000000);
		if (__atomic_load_n(&setup_ended, __ATOMIC_ACQUIRE))
			return 0;
		uint32_t now = __atomic_load_n(&debug_printf_calls, __ATOMIC_ACQUIRE);
		if (now != last) {
			if (reported) {
				say("log lines again after %u s\n", still);
				flush_out();
			}
			last = now;
			still = reported = 0;
			continue;
		}
		still++;
		if (still == BW_REPORT_S || still == BW_DUMP_S - 2) {
			report(still);
			reported = 1;
		}
		if (still >= BW_DUMP_S && __atomic_load_n(&debug_printf_calls, __ATOMIC_ACQUIRE) == last) {
			say("trapping for a core dump\n");
			flush_out();
			__builtin_trap();
		}
	}
	return 0;
}

// The game thread, after yyl.cfg is read
// atexit: a process that ends through newlib's exit() (not abort_hook, which logs) leaves a line and the buffered log
static void bw_atexit(void) {
	if (__atomic_load_n(&setup_ended, __ATOMIC_ACQUIRE))
		return;
	debugPrintf("[BOOTWATCH] process exiting before the main loop\n");
	extern void debug_flush(void);
	debug_flush();
}

void boot_watch_start(int cpu_mask) {
	if (!exp_flag("bootwatch"))
		return;
	bw_game = sceKernelGetThreadId();
	bw_on = 1;
	atexit(bw_atexit);
	SceUID th = sceKernelCreateThread("Boot Watch", boot_watch_main, 0x10000100 - 10, 0x4000, 0, cpu_mask, NULL);
	if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
		debugPrintf("boot watch: could not start (0x%x)\n", th);
		return;
	}
	debugPrintf("boot watch: on (report after %d s without a log line before the main loop, core dump after %d s)\n",
		BW_REPORT_S, BW_DUMP_S);
}

// exp_bootwatchtest: the game thread stops for 30 s before the runner's Startup (tests the report and the dump)
void boot_watch_test(void) {
	if (bw_on && exp_flag("bootwatchtest")) {
		debugPrintf("boot watch: test stall of 30 s\n");
		sceKernelDelayThread(30 * 1000000);
	}
}
