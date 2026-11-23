/*
 * asyncsave.c
 *
 * Background save writer. A checkpoint (save_game) used to write two files on the game thread: the Netflix cloud slot
 * (NfxaCloudWrite, 25-55 ms) and, the next frame, KatanaSave.zero through the runner's fopen/fwrite/fclose
 * (20-45 ms, once 1.4 s). Those were the 50-90 ms frames at wave clears in the bike chase, and every other checkpoint
 * and options save paid the same.
 *
 * Now the game thread only copies the data, and one writer thread on the other cores commits it:
 * - Netflix slots: NfxaCloudWrite queues the text and answers success right away, as the synchronous write did when it
 *   succeeded (netflix.c).
 * - Runner save files (the .zero files in the save directory): fopen "w" returns a FILE that keeps the data in memory;
 *   fclose queues the whole file.
 * Each commit writes path.new, fsyncs it and renames it over the file, keeping the old one as path.bak until then
 * (replace_file, the same steps as PR #44's slot commit). A crash right after a checkpoint can lose that checkpoint,
 * never corrupt a file. asyncsave_init finishes a commit that stopped between its two renames.
 *
 * Ordering: one FIFO, so files reach the card in the order the game wrote them. A queued write that hasn't started is
 * dropped when a newer write to the same file arrives, and the newer one goes to the back of the queue; a file never
 * goes back to an older version. Anything else the game does with a pending file (read, stat, remove, rename, open,
 * a directory listing, append) first waits for its write, so it sees what the synchronous version saw. Exit paths and
 * suspend flush the queue with a bound.
 *
 * The freeze locator's "save I/O" flag (hang_io_begin/end, PR #45) is set while the writer commits and while the
 * game thread waits for a pending write. exp_asyncsave0 keeps the old synchronous writes. Debug mode logs
 * [ASYNCSAVE] lines: queue depth, time in the queue, write time.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // fopencookie
#endif
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ASYNCSAVE_HOST
#include <vitasdk.h>
#include "hang_watch.h"
#include "main.h"
#include "so_util.h"
static uint64_t as_now_us(void) {
	return sceKernelGetProcessTimeWide();
}
static void as_sleep_us(unsigned us) {
	sceKernelDelayThread(us);
}
#endif
// Host tests (tests/asyncsave_test.c) include this file after defining sceIoRemove/sceIoRename, hang_io_begin/end,
// debugPrintf, exp_flag, as_now_us and as_sleep_us.

#include "asyncsave.h"
#include "file_replace.h"

#define AS_PATH_MAX 512

int asyncsave_on;
int asyncsave_cfg = 1; // asyncSave in yyl.cfg

typedef struct as_item {
	struct as_item *next;
	char path[AS_PATH_MAX];
	char *data;
	size_t len;
	asyncsave_commit_fn commit;
	uint64_t queued_us;
} as_item;

static pthread_mutex_t as_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t as_wake = PTHREAD_COND_INITIALIZER; // writer: something was queued
static pthread_cond_t as_done = PTHREAD_COND_INITIALIZER; // waiters: a write finished
static as_item *as_head, *as_tail, *as_running;
static int as_pending; // queued + running; written under as_lock, read without it for the nothing-pending fast path
static unsigned as_written, as_dropped, as_failed;
static int as_cpu_mask;
static char as_save_dir[256];
static size_t as_save_dir_len;

// The runner passes "/ux0:..." or "ux0:..."; compare from the device name, ignoring case (FAT)
static const char *as_norm(const char *path) {
	const char *s = strstr(path, "ux0:");
	return s ? s : path;
}

static const char *as_name(const char *path) {
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

// FNV-1a of the data, in the debug log: a check that the card holds what the game wrote
static uint32_t as_fnv(const char *data, size_t len) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < len; i++)
		h = (h ^ (uint8_t)data[i]) * 16777619u;
	return h;
}

static void as_set_pending(int n) {
	__atomic_store_n(&as_pending, n, __ATOMIC_RELEASE);
}

static int as_newer_queued(const char *path) {
	for (as_item *q = as_head; q; q = q->next)
		if (!strcasecmp(q->path, path))
			return 1;
	return 0;
}

static void *as_writer(void *arg) {
	(void)arg;
#ifndef ASYNCSAVE_HOST
	if (as_cpu_mask)
		sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), as_cpu_mask);
#endif
	pthread_mutex_lock(&as_lock);
	for (;;) {
		while (!as_head)
			pthread_cond_wait(&as_wake, &as_lock);
		as_item *it = as_head;
		as_head = it->next;
		if (!as_head)
			as_tail = NULL;
		as_running = it;
		int depth = as_pending;
		pthread_mutex_unlock(&as_lock);

		uint64_t t0 = as_now_us();
		hang_io_begin(); // save I/O: no freeze-locator dump meanwhile
		int res = it->commit(it->path, it->data, it->len);
		int retried = 0;
		if (res < 0) {
			// One retry, unless a newer version of the file is already waiting (it replaces this one anyway)
			pthread_mutex_lock(&as_lock);
			int newer = as_newer_queued(it->path);
			pthread_mutex_unlock(&as_lock);
			if (!newer) {
				as_sleep_us(100 * 1000);
				res = it->commit(it->path, it->data, it->len);
				retried = 1;
			}
		}
		hang_io_end();
		uint64_t t1 = as_now_us();
		debugPrintf("[ASYNCSAVE] %s %s (%u bytes) in %u ms, %u ms after it was queued, %d pending%s\n",
			res < 0 ? "FAILED to write" : "wrote", as_name(it->path), (unsigned)it->len, (unsigned)((t1 - t0) / 1000),
			(unsigned)((t1 - it->queued_us) / 1000), depth - 1, retried ? " (retried)" : "");

		pthread_mutex_lock(&as_lock);
		if (res < 0)
			as_failed++;
		else
			as_written++;
		as_running = NULL;
		as_set_pending(as_pending - 1);
		pthread_cond_broadcast(&as_done);
		pthread_mutex_unlock(&as_lock);
		free(it->data);
		free(it);
		pthread_mutex_lock(&as_lock);
	}
	return NULL;
}

int asyncsave_queue(const char *path, char *data, size_t len, asyncsave_commit_fn commit) {
	if (!asyncsave_on || !path || !data || !commit)
		return -1;
	const char *norm = as_norm(path);
	if (strlen(norm) >= AS_PATH_MAX)
		return -1;
	as_item *it = malloc(sizeof(*it));
	if (!it)
		return -1;
	uint32_t fnv = as_fnv(data, len); // now: once queued, the writer may free data at any time
	strcpy(it->path, norm);
	it->data = data;
	it->len = len;
	it->commit = commit;
	it->next = NULL;
	it->queued_us = as_now_us();

	as_item *drop = NULL;
	pthread_mutex_lock(&as_lock);
	// A queued write of the same file that hasn't started is superseded: drop it, the new one goes to the back
	for (as_item **link = &as_head, *prev = NULL; *link;) {
		as_item *q = *link;
		if (!strcasecmp(q->path, it->path)) {
			*link = q->next;
			if (as_tail == q)
				as_tail = prev;
			q->next = drop;
			drop = q;
			as_set_pending(as_pending - 1);
			as_dropped++;
		} else {
			prev = q;
			link = &q->next;
		}
	}
	if (as_tail)
		as_tail->next = it;
	else
		as_head = it;
	as_tail = it;
	as_set_pending(as_pending + 1);
	int depth = as_pending;
	pthread_cond_signal(&as_wake);
	pthread_mutex_unlock(&as_lock);

	int dropped = 0;
	while (drop) {
		as_item *next = drop->next;
		free(drop->data);
		free(drop);
		drop = next;
		dropped++;
	}
	debugPrintf("[ASYNCSAVE] queued %s (%u bytes, fnv %08x), %d pending%s\n", as_name(norm), (unsigned)len,
		(unsigned)fnv, depth, dropped ? ", replaced an older queued version" : "");
	return 0;
}

static int as_path_busy(const char *path) {
	return (as_running && !strcasecmp(as_running->path, path)) || as_newer_queued(path);
}

void asyncsave_wait_path(const char *path) {
	if (!path || !__atomic_load_n(&as_pending, __ATOMIC_ACQUIRE))
		return;
	const char *norm = as_norm(path);
	pthread_mutex_lock(&as_lock);
	if (!as_path_busy(norm)) {
		pthread_mutex_unlock(&as_lock);
		return;
	}
	uint64_t t0 = as_now_us();
	hang_io_begin();
	while (as_path_busy(norm))
		pthread_cond_wait(&as_done, &as_lock);
	pthread_mutex_unlock(&as_lock);
	hang_io_end();
	debugPrintf("[ASYNCSAVE] waited %u ms for the pending write of %s\n", (unsigned)((as_now_us() - t0) / 1000),
		as_name(norm));
}

void asyncsave_wait_all(void) {
	if (!__atomic_load_n(&as_pending, __ATOMIC_ACQUIRE))
		return;
	uint64_t t0 = as_now_us();
	hang_io_begin();
	pthread_mutex_lock(&as_lock);
	while (as_pending)
		pthread_cond_wait(&as_done, &as_lock);
	pthread_mutex_unlock(&as_lock);
	hang_io_end();
	debugPrintf("[ASYNCSAVE] waited %u ms for all pending writes\n", (unsigned)((as_now_us() - t0) / 1000));
}

int asyncsave_flush(int timeout_ms, const char *why) {
	if (!__atomic_load_n(&as_pending, __ATOMIC_ACQUIRE))
		return 0;
	uint64_t t0 = as_now_us();
	int left;
	// Polls, so it also works from a thread that may be interrupted (exit, abort, power callback)
	while ((left = __atomic_load_n(&as_pending, __ATOMIC_ACQUIRE)) && as_now_us() - t0 < (uint64_t)timeout_ms * 1000)
		as_sleep_us(2000);
	debugPrintf("[ASYNCSAVE] flush (%s): %s after %u ms\n", why, left ? "writes still pending" : "done",
		(unsigned)((as_now_us() - t0) / 1000));
	if (left)
		debugPrintf("[ASYNCSAVE] %d writes not on the card\n", left);
	return left;
}

int asyncsave_commit_file(const char *path, const char *data, size_t len) {
	char stage[AS_PATH_MAX + 8], backup[AS_PATH_MAX + 8];
	snprintf(stage, sizeof(stage), "%s.new", path);
	snprintf(backup, sizeof(backup), "%s.bak", path);
	FILE *f = fopen(stage, "wb");
	if (!f)
		return -1;
	int ok = (len == 0 || fwrite(data, 1, len, f) == len) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0)
		ok = 0;
	if (!ok || replace_file(stage, path, backup) < 0) {
		remove_if_present(stage);
		return -1;
	}
	return 0;
}

int asyncsave_is_save_file(const char *path) {
	if (!as_save_dir_len || !path)
		return 0;
	const char *norm = as_norm(path);
	if (strncasecmp(norm, as_save_dir, as_save_dir_len))
		return 0;
	const char *name = norm + as_save_dir_len;
	size_t len = strlen(name);
	return len > 5 && !strchr(name, '/') && !strcasecmp(name + len - 5, ".zero");
}

// The in-memory FILE for a runner save file
typedef struct {
	char path[AS_PATH_MAX];
	char *buf;
	size_t len, cap, pos;
	int failed;
} as_wfile;

static ssize_t as_ck_write(void *cookie, const char *buf, size_t n) {
	as_wfile *f = cookie;
	size_t end;
	if (f->failed || __builtin_add_overflow(f->pos, n, &end) || end > 64 * 1024 * 1024) {
		f->failed = 1;
		errno = EFBIG;
		return -1;
	}
	if (end > f->cap) {
		size_t cap = f->cap ? f->cap : 4096;
		while (cap < end)
			cap *= 2;
		char *grown = realloc(f->buf, cap);
		if (!grown) {
			f->failed = 1;
			errno = ENOMEM;
			return -1;
		}
		f->buf = grown;
		f->cap = cap;
	}
	if (f->pos > f->len)
		memset(f->buf + f->len, 0, f->pos - f->len); // a seek past the end leaves zeros, as on a file
	memcpy(f->buf + f->pos, buf, n);
	f->pos = end;
	if (end > f->len)
		f->len = end;
	return n;
}

#if defined(ASYNCSAVE_HOST) && defined(__GLIBC__)
typedef __off64_t as_off;
#elif defined(__LARGE64_FILES)
typedef _off64_t as_off;
#else
typedef off_t as_off;
#endif

static int as_ck_seek(void *cookie, as_off *off, int whence) {
	as_wfile *f = cookie;
	long long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long long)f->pos : whence == SEEK_END ? (long long)f->len : -1;
	if (base < 0 || base + (long long)*off < 0) {
		errno = EINVAL;
		return -1;
	}
	f->pos = (size_t)(base + *off);
	*off = (as_off)f->pos;
	return 0;
}

static int as_ck_close(void *cookie) {
	as_wfile *f = cookie;
	int res = -1;
	if (!f->failed) {
		char *data = f->buf ? f->buf : malloc(1); // an empty file is written too (fopen "w" truncates)
		if (data && asyncsave_queue(f->path, data, f->len, asyncsave_commit_file) == 0) {
			res = 0;
		} else if (data) {
			// Can't queue: write it now, as before
			hang_io_begin();
			res = asyncsave_commit_file(f->path, data, f->len) < 0 ? -1 : 0;
			hang_io_end();
			free(data);
		}
	} else {
		// A write failed (out of memory): leave the file on the card as it was rather than write part of it
		debugPrintf("[ASYNCSAVE] %s not written: out of memory while buffering it\n", as_name(f->path));
		free(f->buf);
	}
	free(f);
	if (res < 0)
		errno = EIO;
	return res;
}

FILE *asyncsave_fopen_write(const char *path) {
	if (!asyncsave_on || !path)
		return NULL;
	const char *norm = as_norm(path);
	as_wfile *f = calloc(1, sizeof(*f));
	if (!f || strlen(norm) >= sizeof(f->path)) {
		free(f);
		return NULL;
	}
	strcpy(f->path, norm);
	cookie_io_functions_t io = { NULL, as_ck_write, as_ck_seek, as_ck_close };
	FILE *fp = fopencookie(f, "w", io);
	if (!fp)
		free(f);
	return fp;
}

int asyncsave_remove(const char *path) {
	if (asyncsave_is_save_file(path))
		asyncsave_wait_path(path);
	return sceIoRemove(path);
}

int asyncsave_rename(const char *from, const char *to) {
	if (asyncsave_is_save_file(from))
		asyncsave_wait_path(from);
	if (asyncsave_is_save_file(to))
		asyncsave_wait_path(to);
	return sceIoRename(from, to);
}

static int as_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

// A commit that stopped between its two renames left the old file as name.bak and no name: the complete name.new
// is the newer save (it was synced and closed before the old file moved). Without name.new, put name.bak back.
static void as_recover(const char *dir) {
	char names[16][256];
	int count = 0;
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) && count < 16) {
		size_t n = strlen(e->d_name);
		if (n > 9 && n < sizeof(names[0]) && !strcasecmp(e->d_name + n - 9, ".zero.bak"))
			snprintf(names[count++], sizeof(names[0]), "%.*s", (int)(n - 4), e->d_name);
	}
	closedir(d);
	for (int i = 0; i < count; i++) {
		char base[AS_PATH_MAX], stage[AS_PATH_MAX + 8], backup[AS_PATH_MAX + 8];
		int len = snprintf(base, sizeof(base), "%s%s", dir, names[i]);
		if (len < 0 || (size_t)len >= sizeof(base) || as_exists(base))
			continue;
		snprintf(stage, sizeof(stage), "%s.new", base);
		snprintf(backup, sizeof(backup), "%s.bak", base);
		int from_stage = as_exists(stage);
		int res = sceIoRename(from_stage ? stage : backup, base);
		debugPrintf("[ASYNCSAVE] recovered %s from %s = 0x%08X\n", names[i], from_stage ? ".new" : ".bak", res);
	}
}

#ifndef ASYNCSAVE_HOST
// Suspend (PS button, power button, sleep): give the writer a moment to finish while the app still runs
static int as_power_cb(int notify_id, int notify_count, int power_info, void *common) {
	if (power_info & (SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_BUTTON_PS_PRESS |
		SCE_POWER_CB_BUTTON_POWER_PRESS | SCE_POWER_CB_LOW_BATTERY_SUSPEND | SCE_POWER_CB_THERMAL_SUSPEND))
		asyncsave_flush(2000, "suspend");
	return 0;
}

static int as_power_thread(SceSize args, void *argp) {
	SceUID cb = sceKernelCreateCallback("asyncsave power", 0, as_power_cb, NULL);
	if (cb < 0 || scePowerRegisterCallback(cb) < 0) {
		debugPrintf("[ASYNCSAVE] no power callback (0x%08X)\n", cb);
		return 0;
	}
	for (;;)
		sceKernelDelayThreadCB(60 * 1000 * 1000);
	return 0;
}
#endif

void asyncsave_init(const char *save_dir, int cpu_mask) {
	snprintf(as_save_dir, sizeof(as_save_dir), "%s", as_norm(save_dir));
	as_save_dir_len = strlen(as_save_dir);
	as_cpu_mask = cpu_mask;
	as_recover(save_dir);
	if (!asyncsave_cfg || exp_flag("asyncsave0")) {
		debugPrintf("[ASYNCSAVE] off (%s): saves are written on the game thread\n",
			asyncsave_cfg ? "exp_asyncsave0" : "asyncSave=0");
		return;
	}
	pthread_t thread;
	asyncsave_on = 1; // before the writer can run: asyncsave_queue checks it
	if (pthread_create(&thread, NULL, as_writer, NULL) != 0) {
		asyncsave_on = 0;
		debugPrintf("[ASYNCSAVE] off: no writer thread\n");
		return;
	}
	pthread_detach(thread);
#ifndef ASYNCSAVE_HOST
	SceUID power = sceKernelCreateThread("asyncsave power", as_power_thread, 0x10000100, 0x2000, 0, cpu_mask, NULL);
	if (power >= 0 && sceKernelStartThread(power, 0, NULL) < 0)
		sceKernelDeleteThread(power);
#endif
	debugPrintf("[ASYNCSAVE] on: checkpoint and options saves are written by a background thread\n");
}
