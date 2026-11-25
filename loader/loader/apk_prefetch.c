/*
 * apk_prefetch.c
 *
 * Reads the big apk entries the runner loads at startup (game.droid, the audio groups) on a worker thread, while the
 * game thread is still busy with CPU work (libyoyo.so relocation and patching, vglInit, the WAD parse), and hands the
 * bytes to the runner's own reads.
 *
 * The memory card was idle through those phases, then the runner read 72 MB of game.droid and ~32 MB of audio groups
 * at ~12 MB/s with nothing else to do (RunnerLoadGame 6.7 s, the audio groups ~2.7 s of the first room's start).
 *
 * The runner reads entries through libzip, which reads the apk FILE with fread (stdio_mt.c mt_fread). A read whose
 * start lies inside a prefetched entry is copied from the prefetched chunks; the FILE is then moved to where the read
 * would have left it, so libzip and everything after it see exactly the bytes and positions of a normal read. A read
 * that reaches a chunk not read yet waits for it; anything else (other files, a chunk that failed to read or was
 * already handed out and freed, a prefetcher stopped by its memory cap) is an ordinary read from the card.
 *
 * Chunks are freed once every byte was handed out, and all of them at release (after Startup), so the extra memory is
 * what has been read ahead and not used yet, at most PF_MAX_HELD. exp_noprefetch turns it off.
 *
 * A deflated game.droid (prepare_apk.py since Nov 2026: 75 MB -> ~51 MB) is inflated whole on the worker
 * (runner_inflate.c, CRC checked) and handed over by a hook on LoadSave::_ReadFile, which does what the runner's
 * libzip branch of that function does with the inflated bytes: MemoryManager::Alloc(size + 1) with the same arguments,
 * the bytes, a 0 after them, *size. Libzip would otherwise inflate it on the game thread, one 16 KB read at a time.
 * Any other name, the AAssetManager branch, or an entry that failed to read or inflate goes to the runner's function.
 *
 * The inflated bytes are not copied: the worker inflates them into a block laid out as the runner's allocator makes
 * one, and the runner's own Alloc call takes that block. MemoryManager::_Alloc (bucketAlloc returns nothing for this
 * size) calls malloc(size + 32), writes its 32-byte header (+0 0, +4 size, +8 0xdeadc0de, +12 0xbaadb00b, +16 and +20
 * 0, +24..31 as malloc left them), adds the size to its in-use, peak and total counters and the count, and returns
 * block + 32. MemoryManager::Free checks the two magics at -24 and -20, reads the size at -28 for the counters, and
 * frees block = p - 32; a top bit set at -4 marks an AllocAligned block, so the worker zeroes the header. The hook
 * calls Alloc as the runner's code does, and main.c's runner malloc returns the worker's block for that one call
 * (apk_prefetch_runner_malloc: that thread, that exact size), so the header, the counters and the later Free are the
 * runner's own. With #107's copy, the inflated block and the runner's copy of it sat in the heap together at Startup
 * and pushed the newlib heap's high mark from 152 to 233 MB of 240 (page_reserve.c). If the runner's Alloc doesn't
 * take the block (its code differs, or another malloc is bound), the bytes are copied as before.
 */

#include <vitasdk.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "unzip.h"
#include "so_util.h"
#include "hooks.h"
#include <strings.h>
#include <zlib.h>

#define PF_CHUNK (4u << 20)
#define PF_READ (256u << 10)
#define PF_MAX_HELD (96u << 20)
#define PF_MAX_RANGES 8
#define MM_HEADER 32 // MemoryManager::_Alloc's header in front of the bytes it returns

enum { PF_PENDING, PF_READY, PF_FREED, PF_FAILED };

typedef struct {
	const char *name;
	uint64_t start, size;
	int deflated;          // one chunk holding the whole inflated entry, handed over by the _ReadFile hook
	uint32_t comp_size, crc;
	int nchunks;
	uint8_t **chunk;
	uint8_t *state;
	uint32_t *served;
} pf_range;

static pf_range ranges[PF_MAX_RANGES];
static int n_ranges;
static pthread_mutex_t pf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pf_cond = PTHREAD_COND_INITIALIZER;
static int pf_active;     // ranges set up; serve() looks at reads
static int pf_stop;       // release: the thread stops, nothing more is handed out
static int pf_blocked;    // the thread waits for memory (PF_MAX_HELD): readers mustn't wait for it
static int pf_done;       // the thread has finished
static uint32_t pf_held;  // bytes read ahead and not freed yet
static char pf_path[512];
static uint32_t pf_start_us, pf_end_us, pf_wait_us, pf_waits;
static uint64_t pf_read_bytes, pf_served_bytes;

static uint32_t chunk_len(const pf_range *r, int c) {
	if (r->deflated)
		return (uint32_t)r->size;
	uint64_t left = r->size - (uint64_t)c * PF_CHUNK;
	return left < PF_CHUNK ? (uint32_t)left : PF_CHUNK;
}

// pf_lock held
static void fail_from(int ri, int ci) {
	for (int i = ri; i < n_ranges; i++)
		for (int c = i == ri ? ci : 0; c < ranges[i].nchunks; c++)
			if (ranges[i].state[c] == PF_PENDING)
				ranges[i].state[c] = PF_FAILED;
}

static int pf_cpu_mask;

// A pthread: the lock and condition variable are pthread-embedded's, which need the waiting thread to be one of its
// own (a sceKernelCreateThread thread crashed in pte_osSemaphorePend at the first wait)
static void *prefetch_thread(void *arg) {
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), pf_cpu_mask);
	SceUID fd = sceIoOpen(pf_path, SCE_O_RDONLY, 0);
	int ri = 0, ci = 0;
	for (; fd >= 0 && ri < n_ranges; ri++) {
		pf_range *r = &ranges[ri];
		if (r->deflated) {
			uint32_t len = (uint32_t)r->size;
			pthread_mutex_lock(&pf_lock);
			while (!pf_stop && pf_held + len > PF_MAX_HELD) {
				pf_blocked = 1;
				pthread_cond_broadcast(&pf_cond);
				pthread_cond_wait(&pf_cond, &pf_lock);
			}
			pf_blocked = 0;
			int stop = pf_stop;
			pthread_mutex_unlock(&pf_lock);
			if (stop)
				goto out;
			extern int runner_inflate(const char *path, uint64_t offset, uint32_t comp_size, uint32_t size,
				uint32_t expected_crc, uint8_t *out, int cpu_mask);
			// The runner's block (see the top): its header, the bytes, their 0
			uint8_t *buf = malloc(MM_HEADER + len + 1);
			if (buf)
				memset(buf, 0, MM_HEADER);
			int ok = buf && runner_inflate(pf_path, r->start, r->comp_size, len, r->crc, buf + MM_HEADER, pf_cpu_mask) == 0;
			if (ok)
				buf[MM_HEADER + len] = 0;
			pthread_mutex_lock(&pf_lock);
			if (ok && !pf_stop) {
				r->chunk[0] = buf;
				r->state[0] = PF_READY;
				pf_held += len;
				pf_read_bytes += r->comp_size;
				buf = NULL;
			} else {
				r->state[0] = PF_FAILED;
			}
			pthread_cond_broadcast(&pf_cond);
			pthread_mutex_unlock(&pf_lock);
			free(buf);
			continue;
		}
		for (ci = 0; ci < r->nchunks; ci++) {
			uint32_t len = chunk_len(r, ci);
			pthread_mutex_lock(&pf_lock);
			while (!pf_stop && pf_held + len > PF_MAX_HELD) {
				pf_blocked = 1;
				pthread_cond_broadcast(&pf_cond);
				pthread_cond_wait(&pf_cond, &pf_lock);
			}
			pf_blocked = 0;
			int stop = pf_stop;
			pthread_mutex_unlock(&pf_lock);
			if (stop)
				goto out;
			uint8_t *buf = malloc(len);
			if (!buf)
				goto out;
			uint64_t off = r->start + (uint64_t)ci * PF_CHUNK;
			uint32_t done = 0;
			if (sceIoLseek(fd, off, SCE_SEEK_SET) == (SceOff)off) {
				while (done < len) {
					// 256 KB requests: the card is no faster with bigger ones, and the game thread's small file
					// accesses (yyl.cfg, exp_ flags, vglInit's folders, saves) wait behind each request; behind 4 MB
					// ones they took up to ~0.4 s each
					int n = sceIoRead(fd, buf + done, len - done < PF_READ ? len - done : PF_READ);
					if (n <= 0)
						break;
					done += n;
				}
			}
			if (done != len) {
				free(buf);
				goto out;
			}
			pthread_mutex_lock(&pf_lock);
			if (pf_stop) {
				pthread_mutex_unlock(&pf_lock);
				free(buf);
				goto out;
			}
			r->chunk[ci] = buf;
			r->state[ci] = PF_READY;
			pf_held += len;
			pf_read_bytes += len;
			pthread_cond_broadcast(&pf_cond);
			pthread_mutex_unlock(&pf_lock);
		}
	}
out:
	if (fd >= 0)
		sceIoClose(fd);
	pthread_mutex_lock(&pf_lock);
	fail_from(ri < n_ranges ? ri : n_ranges, ri < n_ranges ? ci : 0);
	pf_done = 1;
	pf_blocked = 0;
	pf_end_us = sceKernelGetProcessTimeLow();
	pthread_cond_broadcast(&pf_cond);
	pthread_mutex_unlock(&pf_lock);
	return NULL;
}

// Locates the stored entries in the open apk and starts the reader. Entries that are compressed or missing are skipped.
void apk_prefetch_start(void *apk, const char *apk_path, int cpu_mask) {
	static const char *const names[] = {
		"assets/game.droid", "assets/audiogroup1.dat", "assets/audiogroup3.dat", "assets/audiogroup2.dat",
		"assets/audiogroup4.dat",
	};
	if (exp_flag("noprefetch"))
		return;
	unzFile zip = (unzFile)apk;
	for (int i = 0; i < (int)(sizeof(names) / sizeof(*names)) && n_ranges < PF_MAX_RANGES; i++) {
		unz_file_info info;
		if (unzLocateFile(zip, names[i], NULL) != UNZ_OK ||
			unzGetCurrentFileInfo(zip, &info, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK || !info.uncompressed_size ||
			(info.compression_method != 0 && (i || info.compression_method != Z_DEFLATED)) ||
			unzOpenCurrentFile(zip) != UNZ_OK)
			continue;
		uint64_t start = unzGetCurrentFileZStreamPos64(zip);
		unzCloseCurrentFile(zip);
		pf_range *r = &ranges[n_ranges];
		r->name = names[i];
		r->start = start;
		r->size = info.uncompressed_size;
		r->deflated = info.compression_method == Z_DEFLATED;
		r->comp_size = info.compressed_size;
		r->crc = info.crc;
		r->nchunks = r->deflated ? 1 : (int)((r->size + PF_CHUNK - 1) / PF_CHUNK);
		r->chunk = calloc(r->nchunks, sizeof(*r->chunk));
		r->state = calloc(r->nchunks, 1);
		r->served = calloc(r->nchunks, sizeof(*r->served));
		if (!r->chunk || !r->state || !r->served) {
			free(r->chunk);
			free(r->state);
			free(r->served);
			break;
		}
		n_ranges++;
	}
	if (!n_ranges)
		return;
	snprintf(pf_path, sizeof(pf_path), "%s", apk_path);
	pf_start_us = sceKernelGetProcessTimeLow();
	pf_cpu_mask = cpu_mask;
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int err = pthread_create(&th, &attr, prefetch_thread, NULL);
	pthread_attr_destroy(&attr);
	if (err) {
		pthread_mutex_lock(&pf_lock);
		fail_from(0, 0);
		pf_done = 1;
		pthread_mutex_unlock(&pf_lock);
		return;
	}
	pf_active = 1;
}

// pf_lock held. The range a file position lies in, or NULL.
static pf_range *range_at(uint64_t pos) {
	for (int i = 0; i < n_ranges; i++)
		if (!ranges[i].deflated && pos >= ranges[i].start && pos < ranges[i].start + ranges[i].size)
			return &ranges[i];
	return NULL;
}

// Called by mt_fread with the apk FILE locked, at the FILE position pos. Copies up to n bytes into buf if pos lies in
// a prefetched entry and every chunk they need is read; returns the byte count (the read stops at the entry's end).
// 0: read from the card. -1: a chunk is still being read, apk_prefetch_wait() then try again.
long apk_prefetch_serve(uint64_t pos, void *buf, size_t n) {
	if (!__atomic_load_n(&pf_active, __ATOMIC_ACQUIRE) || !n)
		return 0;
	pthread_mutex_lock(&pf_lock);
	pf_range *r = pf_stop ? NULL : range_at(pos);
	if (!r) {
		pthread_mutex_unlock(&pf_lock);
		return 0;
	}
	uint64_t end = pos + n < r->start + r->size ? pos + n : r->start + r->size;
	int c0 = (int)((pos - r->start) / PF_CHUNK), c1 = (int)((end - 1 - r->start) / PF_CHUNK);
	for (int c = c0; c <= c1; c++) {
		if (r->state[c] == PF_PENDING) {
			pthread_mutex_unlock(&pf_lock);
			return -1;
		}
		if (r->state[c] != PF_READY) {
			pthread_mutex_unlock(&pf_lock);
			return 0;
		}
	}
	uint8_t *out = buf;
	for (uint64_t p = pos; p < end;) {
		int c = (int)((p - r->start) / PF_CHUNK);
		uint32_t in = (uint32_t)(p - r->start - (uint64_t)c * PF_CHUNK);
		uint32_t len = chunk_len(r, c);
		uint32_t m = end - p < len - in ? (uint32_t)(end - p) : len - in;
		sceClibMemcpy(out, r->chunk[c] + in, m);
		out += m;
		p += m;
		r->served[c] += m;
		if (r->served[c] >= len) {
			// Every byte handed out once: libzip reads an entry front to back, a second read goes to the card
			free(r->chunk[c]);
			r->chunk[c] = NULL;
			r->state[c] = PF_FREED;
			pf_held -= len;
			pthread_cond_broadcast(&pf_cond);
		}
	}
	pf_served_bytes += end - pos;
	pthread_mutex_unlock(&pf_lock);
	return (long)(end - pos);
}

// Waits (without the FILE lock) until the chunks for a read at pos are no longer being read, or the reader stopped
// or is waiting for memory. Returns 1 if trying apk_prefetch_serve() again can be useful.
int apk_prefetch_wait(uint64_t pos, size_t n) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	int again = 0;
	pthread_mutex_lock(&pf_lock);
	for (;;) {
		pf_range *r = pf_stop ? NULL : range_at(pos);
		if (!r)
			break;
		uint64_t end = pos + n < r->start + r->size ? pos + n : r->start + r->size;
		int pending = 0;
		for (int c = (int)((pos - r->start) / PF_CHUNK); c <= (int)((end - 1 - r->start) / PF_CHUNK); c++)
			pending |= r->state[c] == PF_PENDING;
		if (!pending) {
			again = 1;
			break;
		}
		if (pf_done || pf_blocked)
			break;
		pthread_cond_wait(&pf_cond, &pf_lock);
	}
	pf_wait_us += sceKernelGetProcessTimeLow() - t0;
	pf_waits++;
	pthread_mutex_unlock(&pf_lock);
	return again;
}

// Stops the reader and frees everything still held (called at the second room change: the startup loads are done)
void apk_prefetch_release(void) {
	if (!__atomic_load_n(&pf_active, __ATOMIC_ACQUIRE))
		return;
	pthread_mutex_lock(&pf_lock);
	if (pf_stop) {
		pthread_mutex_unlock(&pf_lock);
		return;
	}
	pf_stop = 1;
	uint32_t left = 0;
	for (int i = 0; i < n_ranges; i++)
		for (int c = 0; c < ranges[i].nchunks; c++)
			if (ranges[i].state[c] == PF_READY) {
				left += chunk_len(&ranges[i], c);
				free(ranges[i].chunk[c]);
				ranges[i].chunk[c] = NULL;
				ranges[i].state[c] = PF_FREED;
			}
	pf_held = 0;
	pthread_cond_broadcast(&pf_cond);
	debugPrintf("[PREFETCH] %d entries, read %.1f MB in %.2f s (%.3f -> %.3f s), handed out %.1f MB, game thread waited "
		"%.2f s in %u reads, %.1f MB unused freed\n", n_ranges, pf_read_bytes / 1048576.0f,
		((pf_done ? pf_end_us : sceKernelGetProcessTimeLow()) - pf_start_us) / 1000000.0f, pf_start_us / 1000000.0f,
		(pf_done ? pf_end_us : sceKernelGetProcessTimeLow()) / 1000000.0f, pf_served_bytes / 1048576.0f,
		pf_wait_us / 1000000.0f, pf_waits, left / 1048576.0f);
	pthread_mutex_unlock(&pf_lock);
}

// pf_lock held. The deflated range called name (as _ReadFile gets it: "assets/game.droid"), or NULL.
static pf_range *deflated_range(const char *name) {
	for (int i = 0; i < n_ranges; i++)
		if (ranges[i].deflated && !strcasecmp(ranges[i].name, name))
			return &ranges[i];
	return NULL;
}

// Waits for the deflated entry name. Returns its inflated bytes (still held: apk_prefetch_release or take frees them),
// NULL if it isn't prefetched, failed, or was handed over already.
const uint8_t *apk_prefetch_peek(const char *name, uint32_t *size) {
	if (!__atomic_load_n(&pf_active, __ATOMIC_ACQUIRE))
		return NULL;
	uint32_t t0 = sceKernelGetProcessTimeLow();
	pthread_mutex_lock(&pf_lock);
	pf_range *r = deflated_range(name);
	while (r && r->state[0] == PF_PENDING && !pf_stop && !pf_done)
		pthread_cond_wait(&pf_cond, &pf_lock);
	const uint8_t *p = r && !pf_stop && r->state[0] == PF_READY ? r->chunk[0] + MM_HEADER : NULL;
	if (p)
		*size = (uint32_t)r->size;
	pf_wait_us += sceKernelGetProcessTimeLow() - t0;
	pf_waits++;
	pthread_mutex_unlock(&pf_lock);
	return p;
}

// The inflated bytes of name were handed over (the _ReadFile hook): freed if they were copied, else the runner has them
static void deflated_done(const char *name, int copied) {
	pthread_mutex_lock(&pf_lock);
	pf_range *r = deflated_range(name);
	if (r && r->state[0] == PF_READY) {
		if (copied)
			free(r->chunk[0]);
		r->chunk[0] = NULL;
		r->state[0] = PF_FREED;
		pf_held -= (uint32_t)r->size;
		pf_served_bytes += r->size;
		pthread_cond_broadcast(&pf_cond);
	}
	pthread_mutex_unlock(&pf_lock);
}

// LoadSave::_ReadFile(name, size_out) of runner 1.1.10. Its libzip branch (g_Android_UseDynamicAssetDelivery 0, the
// loader's setting): zip_fopen, zip_stat, buf = MemoryManager::Alloc(st.size + 1, <source file>, 230, false),
// buf[st.size] = 0, zip_fread until done, zip_fclose (its result unused), *size_out = st.size if size_out, return buf.
static void *(*orig_readfile)(const char *name, uint32_t *size);
static void *(*mm_alloc)(uint32_t size, const char *file, int line, int flag);
static const uint8_t *dad_flag;
static const char *readfile_source;
static int hook_on, alloc_layout_ok;

// The worker's block for the runner's next malloc of handover_size on handover_thread (one call)
static void *volatile handover_block;
static size_t handover_size;
static SceUID handover_thread;

// main.c's runner malloc, for sizes of at least 4 MB: the worker's block while readfile_hook's Alloc call runs
void *apk_prefetch_runner_malloc(size_t size) {
	void *b = handover_block;
	if (!b || size != handover_size || sceKernelGetThreadId() != handover_thread)
		return NULL;
	handover_block = NULL;
	return b;
}

static void *readfile_hook(const char *name, uint32_t *size) {
	if (hook_on && name && !*dad_flag) {
		uint32_t len;
		const uint8_t *data = apk_prefetch_peek(name, &len);
		if (data) {
			uint32_t t0 = sceKernelGetProcessTimeLow();
			if (alloc_layout_ok) {
				handover_size = MM_HEADER + len + 1;
				handover_thread = sceKernelGetThreadId();
				handover_block = (void *)(data - MM_HEADER);
			}
			uint8_t *buf = mm_alloc(len + 1, readfile_source, 230, 0);
			int taken = alloc_layout_ok && !handover_block;
			handover_block = NULL;
			if (buf && taken && buf != data) {
				// Can't happen with the checked _Alloc; the block then belongs to the runner (leaked, not freed twice)
				debugPrintf("[PREFETCH] the runner took game.droid's block but Alloc returned %p: copied\n", buf);
				taken = 0;
				sceClibMemcpy(buf, data, len);
				buf[len] = 0;
				deflated_done(name, 0);
			} else if (buf) {
				if (!taken) {
					sceClibMemcpy(buf, data, len);
					buf[len] = 0;
				}
				deflated_done(name, !taken);
			}
			if (buf) {
				debugPrintf("[PREFETCH] game.droid handed over %s in %.1f ms (%u bytes)\n",
					taken ? "in place" : "by a copy", (sceKernelGetProcessTimeLow() - t0) / 1000.0f, len);
				if (size)
					*size = len;
				// After a copy, the inflated block's hole is the heap's largest: blocks for pages outside VRAM come from
				// it. Handed over in place there is no hole; they are taken after Startup, from the prefetched audio
				// chunks' holes
				extern void page_reserve_fill(const char *where);
				if (!taken)
					page_reserve_fill("after the game.droid handover");
				return buf;
			}
		}
	}
	return orig_readfile(name, size);
}

// Before the runner's Startup. Only when a deflated entry is being prefetched.
void apk_prefetch_install_hooks(void) {
	int any = 0;
	for (int i = 0; i < n_ranges; i++)
		any |= ranges[i].deflated;
	if (!__atomic_load_n(&pf_active, __ATOMIC_ACQUIRE) || !any)
		return;
	uintptr_t f = so_symbol(&yoyoloader_mod, "_ZN8LoadSave9_ReadFileEPKcPj");
	mm_alloc = (void *)so_symbol(&yoyoloader_mod, "_ZN13MemoryManager5AllocEjPKcib");
	dad_flag = (const uint8_t *)so_symbol(&yoyoloader_mod, "g_Android_UseDynamicAssetDelivery");
	// The libzip branch's Alloc call: mov r2, #230 / ldr r1, [pc, #252] / ... / add r1, pc, r1 (the source file name)
	const uint32_t *code = (const uint32_t *)f;
	if (!f || !mm_alloc || !dad_flag || code[0x188 / 4] != 0xe3a020e6 || code[0x18c / 4] != 0xe59f10fc ||
		code[0x19c / 4] != 0xe08f1001) {
		debugPrintf("[PREFETCH] _ReadFile layout differs: deflated entries go through libzip\n");
		return;
	}
	readfile_source = (const char *)(f + 0x19c + 8 + code[0x290 / 4]);
	// MemoryManager::_Alloc of runner 1.1.10: malloc(size + 32) (add r0, r5, #32), the header (str r8, [r0] /
	// str r5, [r0, #4] / vst1.32 {d16-d17}, [r7] at +8), block + 32 returned (add r0, r0, #32)
	const uint32_t *ac = (const uint32_t *)so_symbol(&yoyoloader_mod, "_ZN13MemoryManager6_AllocEjPKcib");
	alloc_layout_ok = ac && ac[0x58 / 4] == 0xe2850020 && ac[0x88 / 4] == 0xe5808000 && ac[0x8c / 4] == 0xe5805004 &&
		ac[0xa8 / 4] == 0xe2800020 && ac[0xac / 4] == 0xf4470a8f && !exp_flag("droidcopy");
	if (!alloc_layout_ok)
		debugPrintf("[PREFETCH] game.droid is handed over by a copy (MemoryManager::_Alloc differs, or exp_droidcopy)\n");
	orig_readfile = entry_hook(f, readfile_hook);
	hook_on = orig_readfile != NULL;
}
