/*
 * memtrace.c
 *
 * Debug-mode tracker for large runner heap allocations: malloc/calloc/realloc/memalign/free
 * handed to the runner go through these wrappers, which remember every live block of at
 * least MEMTRACE_MIN bytes together with the runner offset of its caller.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#define MEMTRACE_MIN (256 * 1024)
#define MEMTRACE_SLOTS 1024

#define CHAIN 4

typedef struct {
	void *ptr;
	uint32_t size;
	uint32_t chain[CHAIN];
} big_block;

static big_block blocks[MEMTRACE_SLOTS];
static int memtrace_on = 0;
static SceKernelLwMutexWork memtrace_mutex;

void memtrace_init(int enable) {
	memtrace_on = enable;
	if (enable)
		sceKernelCreateLwMutex(&memtrace_mutex, "memtrace", 0, 0, NULL);
}

// Backtrace through the runner's frame-pointer chain (clang ARM frames: [fp] = caller fp,
// [fp + 4] = return address), skipping the MemoryManager / CBucket allocator layers
static void walk_frames(uint32_t *fp, uint32_t lr, uint32_t *chain) {
	uintptr_t lo = yoyoloader_mod.text_base, hi = lo + yoyoloader_mod.text_size;
	uint32_t sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	int n = 0;
	uint32_t ret = lr;
	for (int depth = 0; depth < 16 && n < CHAIN; depth++) {
		if (ret >= lo && ret < hi) {
			uint32_t off = ret - lo;
			if (off < 0x1236000 || off >= 0x1238a40) // MemoryManager::* and CBucket<>::Alloc
				chain[n++] = off;
		}
		if ((uintptr_t)fp & 3 || (uintptr_t)fp < sp || (uintptr_t)fp > sp + 0x200000)
			break;
		ret = fp[1];
		fp = (uint32_t *)fp[0];
	}
	while (n < CHAIN)
		chain[n++] = 0;
}

static void track_fp(void *ptr, size_t size, uint32_t lr, uint32_t *fp) {
	if (!memtrace_on || !ptr || size < MEMTRACE_MIN)
		return;
	uint32_t chain[CHAIN];
	walk_frames(fp, lr, chain);
	if (size >= 4 * 1024 * 1024)
		debugPrintf("[MEMTRACE] alloc %uKB at %p: %08X < %08X < %08X < %08X\n", size / 1024, ptr, chain[0], chain[1], chain[2], chain[3]);
	sceKernelLockLwMutex(&memtrace_mutex, 1, NULL);
	for (int i = 0; i < MEMTRACE_SLOTS; i++) {
		if (!blocks[i].ptr) {
			blocks[i].ptr = ptr;
			blocks[i].size = size;
			memcpy(blocks[i].chain, chain, sizeof(chain));
			break;
		}
	}
	sceKernelUnlockLwMutex(&memtrace_mutex, 1);
}

static void untrack(void *ptr) {
	if (!memtrace_on || !ptr)
		return;
	sceKernelLockLwMutex(&memtrace_mutex, 1, NULL);
	for (int i = 0; i < MEMTRACE_SLOTS; i++) {
		if (blocks[i].ptr == ptr) {
			blocks[i].ptr = NULL;
			break;
		}
	}
	sceKernelUnlockLwMutex(&memtrace_mutex, 1);
}

// The exported allocators are ARM stubs that append the runner's fp and lr to the arguments
void *memtrace_malloc_impl(size_t size, uint32_t *fp, uint32_t lr) {
	void *r = vglMalloc(size);
	track_fp(r, size, lr, fp);
	return r;
}

void *memtrace_calloc_impl(size_t num, size_t size, uint32_t *fp, uint32_t lr) {
	void *r = vglCalloc(num, size);
	track_fp(r, num * size, lr, fp);
	return r;
}

void *memtrace_memalign_impl(size_t align, size_t size, uint32_t *fp, uint32_t lr) {
	void *r = vglMemalign(align, size);
	track_fp(r, size, lr, fp);
	return r;
}

void *memtrace_realloc_impl(void *ptr, size_t size, uint32_t *fp, uint32_t lr) {
	untrack(ptr);
	void *r = vglRealloc(ptr, size);
	track_fp(r, size, lr, fp);
	return r;
}

#define ARG1_STUB(name) \
	".global " #name "\n.type " #name ", %function\n" #name ":\n" \
	"	mov r1, r11\n	mov r2, lr\n	ldr ip, =" #name "_impl\n	bx ip\n	.ltorg\n"
#define ARG2_STUB(name) \
	".global " #name "\n.type " #name ", %function\n" #name ":\n" \
	"	mov r2, r11\n	mov r3, lr\n	ldr ip, =" #name "_impl\n	bx ip\n	.ltorg\n"

__asm__(
	".text\n.arm\n"
	ARG1_STUB(memtrace_malloc)
	ARG2_STUB(memtrace_calloc)
	ARG2_STUB(memtrace_memalign)
	ARG2_STUB(memtrace_realloc));

void memtrace_free(void *ptr) {
	untrack(ptr);
	vglFree(ptr);
}

// Logs live large blocks grouped by caller
void memtrace_report(void) {
	if (!memtrace_on)
		return;
	uint32_t callers[64][CHAIN], sizes[64], counts[64];
	int n = 0;
	uint32_t total = 0;
	sceKernelLockLwMutex(&memtrace_mutex, 1, NULL);
	for (int i = 0; i < MEMTRACE_SLOTS; i++) {
		if (!blocks[i].ptr)
			continue;
		total += blocks[i].size;
		int j;
		for (j = 0; j < n; j++) {
			if (!memcmp(callers[j], blocks[i].chain, sizeof(callers[j])))
				break;
		}
		if (j == n) {
			if (n == 64)
				continue;
			memcpy(callers[n], blocks[i].chain, sizeof(callers[n]));
			sizes[n] = counts[n] = 0;
			n++;
		}
		sizes[j] += blocks[i].size;
		counts[j]++;
	}
	sceKernelUnlockLwMutex(&memtrace_mutex, 1);
	debugPrintf("[MEMTRACE] live blocks >= %uKB: %uKB total\n", MEMTRACE_MIN / 1024, total / 1024);
	for (int j = 0; j < n; j++)
		debugPrintf("[MEMTRACE]   %08X < %08X < %08X: %3u blocks %7uKB\n", callers[j][0], callers[j][1], callers[j][2],
			counts[j], sizes[j] / 1024);
}
