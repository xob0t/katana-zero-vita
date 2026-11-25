/*
 * page_reserve.c
 *
 * Newlib heap kept for the texture pages that don't fit in VRAM, and for the runner's buffer that decodes a page.
 *
 * When a room's pages don't fit in VRAM (room_chinatown_2 draws from seven 16 MB pages, VRAM holds five next to the
 * surfaces) and page eviction (page_evict.c) finds nothing to unload, vitaGL places the page in its RAM, PHYCONT and
 * CDLG pools (3.5, ~10 and ~9 MB blocks at most) and last in the newlib heap (memalign). A page there is slower for the
 * GPU, but it draws. Newlib hands out a 16 MB block only from a free hole of that size or from its untouched tail, and
 * over a session the holes get split by the runner's small allocations. Since the deflated game.droid (apk_prefetch.c)
 * the tail is only ~13 MB after Startup (the inflated copy and the runner's copy of it both sat in the heap at the
 * peak), so after ~15 minutes of play no 16 MB block was left: the allocation failed, the runner went on with a texture
 * that has no storage, and its sprites drew as solid white or grey rectangles (room_chinatown_2's level start and
 * HUD). The recovery flush after each failure (katana_display.c) reloaded the pages for 1.4 s every 5 s and the same
 * pages failed again. Even a freed page's own hole doesn't take the page again once its neighbours are in use:
 * memalign(16, n) asks malloc for n plus the alignment plus a minimum chunk. (game.droid is no longer copied since
 * Nov 25: ~158 MB sbrk'd after Startup, a ~85 MB tail, apk_prefetch.c. A long session still splits that tail.)
 *
 * So PR_RESERVE_BLOCKS blocks of a page's size are taken from the heap while it still has large holes (after Startup;
 * with exp_droidcopy also right after the runner's copy of game.droid frees the inflated one) and held. When a GPU
 * allocation of at least PR_MIN_SIZE fits neither VRAM (after eviction) nor vitaGL's other pools, a held block is
 * handed over: vitaGL's next memalign (its newlib fallback, right after the callback) gets that block itself (memalign
 * is wrapped for the loader). When vitaGL frees the page, the block goes back to the heap whole, and between frames it
 * is taken again (malloc of the same size fits that hole exactly) while fewer than PR_RESERVE_BLOCKS pages are in the
 * heap.
 *
 * The page's pixels need a second 16 MB block first: the runner decodes a page (ReadQOIFFile) into a
 * MemoryManager::Alloc buffer of width x height x 4 bytes, malloc(size + 32) (main.c's runner_malloc, normally
 * vglMalloc: the newlib heap, then vitaGL's pools, then a forced garbage collection). In a long session's fragmented
 * heap, with PHYCONT and VRAM taken by pages, that malloc failed too (club_0 -> room_title after 8 minutes: three
 * "Memory allocation failed: 16777216 bytes", each after a forced collection with a scene open). ReadQOIFFile then
 * returns NULL, LoadTextureFromQOIF creates no GL texture and the page's GL name stays -1; the runner binds that name
 * (glBindTexture(GL_TEXTURE_2D, 0xFFFFFFFF)), vitaGL (built without error checks) takes texture_slots[-1] for the next
 * draw, and its garbage GXM texture crashed the GPU and the whole Vita. So one more block is held for that buffer
 * (every held block is PR_HELD_BYTES, a page plus the runner's header, so any of them can take it): runner_malloc hands
 * it out on the GL thread when the heap has no hole for an allocation of a page's size, before vitaGL's pools (which a
 * page that doesn't fit in VRAM needs), and takes it back when the runner frees it (FreeQOIFFile, right after the
 * upload), so the next page's decode finds it again. No
 * forced collection for these: it ran with a scene open (the hazard behind the prison freeze), and a page whose decode
 * still fails is drawn blank and retried (page_evict.c's load hook), so the GPU never sees a missing texture.
 *
 * At most PR_RESERVE_BLOCKS + 1 blocks of PR_HELD_BYTES of the heap are set aside this way (the runner uses ~140 MB of
 * the 240 MB heap; with the blocks ~191 MB is sbrk'd after Startup, the rest a ~55 MB tail). Game (GL) thread only.
 * exp_pagereserve0 disables all of it; exp_reservedecode0 only the decode block (the runner's page buffer from
 * vglMalloc as before). Tests: exp_fragheap splits the free heap holes of 8 MB or more after Startup, the state of a
 * long session; exp_fragpools also takes vitaGL's RAM, PHYCONT and CDLG pools in pieces below a page's size;
 * exp_fragdecode leaves a 2048x2048 page's decode buffer only the decode block (the heap,
 * the pools and VRAM all full, as at the crash: PHYCONT had 10 MB free, VRAM 5 MB).
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

#define PR_RESERVE_BLOCKS 2
#define PR_BLOCK_BYTES (16 * 1024 * 1024) // a 2048x2048 RGBA page
#define PR_MIN_SIZE (4 * 1024 * 1024)
#define PR_HELD_BYTES (PR_BLOCK_BYTES + 4096) // every held block: a page's decode buffer plus MemoryManager's header

extern SceUID vgl_gl_thread;
extern void *vgl_memalign(size_t alignment, size_t size, vglMemType type);
extern void vgl_free(void *ptr);
extern size_t newlib_heap_used(void);
void *__real_memalign(size_t alignment, size_t size);

static int enabled = -1, decode_enabled, frag_decode;
static void *blocks[PR_RESERVE_BLOCKS];
static void *handoff;             // the block vitaGL's next large memalign gets
static void *decode_block;        // held for the runner's page decode buffer
static void *volatile decode_out; // the decode block while the runner has it
unsigned page_reserve_handoffs, page_reserve_refills, page_reserve_decodes, page_reserve_runner_fails;

// vitaGL's newlib fallback (mem_utils.c vgl_memalign for VGL_MEM_EXTERNAL) and the loader's own memalign calls. Only a
// block page_reserve_release just set aside is handed out, to the GL thread's next allocation of a page's size.
void *__wrap_memalign(size_t alignment, size_t size) {
	void *b = handoff;
	if (b && size >= PR_MIN_SIZE && size <= PR_BLOCK_BYTES && !((uintptr_t)b & (alignment - 1)) &&
		sceKernelGetThreadId() == vgl_gl_thread) {
		handoff = NULL;
		page_reserve_handoffs++;
		return b;
	}
	return __real_memalign(alignment, size);
}

static int held(void) {
	int n = 0;
	for (int i = 0; i < PR_RESERVE_BLOCKS; i++)
		n += blocks[i] != NULL;
	return n;
}

static int is_enabled(void) {
	if (enabled < 0) {
		enabled = !exp_flag("pagereserve0");
		decode_enabled = enabled && !exp_flag("reservedecode0");
		frag_decode = enabled && exp_flag("fragdecode");
	}
	return enabled;
}

// Takes free blocks until `want` are held; returns how many it took. malloc of the block size fits a freed block's hole
// exactly (memalign would ask for more).
static int take(int want) {
	int took = 0;
	for (int i = 0; i < PR_RESERVE_BLOCKS && held() < want; i++) {
		if (blocks[i])
			continue;
		blocks[i] = malloc(PR_HELD_BYTES);
		if (!blocks[i])
			break;
		took++;
	}
	return took;
}

// After Startup (the prefetched audio chunks' holes take the first block), and with exp_droidcopy right after the
// runner's copy of game.droid freed the inflated one (apk_prefetch.c)
void page_reserve_fill(const char *where) {
	if (!is_enabled())
		return;
	size_t before = newlib_heap_used();
	// The first blocks aligned for any texture (vitaGL asks for 16)
	for (int i = 0; i < PR_RESERVE_BLOCKS; i++)
		if (!blocks[i])
			blocks[i] = __real_memalign(64, PR_HELD_BYTES);
	if (decode_enabled && !decode_block && !decode_out)
		decode_block = malloc(PR_HELD_BYTES);
	debugPrintf("[PAGERESERVE] %s: %d of %d heap blocks of %uKB held for pages outside VRAM, decode block %s "
		"(newlib %uKB sbrk'd, %uKB before)\n", where, held(), PR_RESERVE_BLOCKS, PR_BLOCK_BYTES / 1024,
		decode_block ? "held" : decode_enabled ? "NOT held" : "off (exp_reservedecode0)",
		(unsigned)(newlib_heap_used() / 1024), (unsigned)(before / 1024));
}

// main.c's malloc for the runner, for sizes of at least PR_MIN_SIZE: the newlib heap (vglMalloc's first choice), then
// on the GL thread the decode block (or, if the runner still has it, a block held for pages: their pixels come before
// their storage), then vitaGL's pools without a forced collection; NULL if none has room (page_evict.c then unloads
// what it may and tries once more, else draws the page blank and loads it again later). Other threads, or with the
// block off: vglMalloc as before.
// exp_fragdecode (test): a 2048x2048 page's decode buffer gets nothing from the heap or the pools, as at the crash (only
// the decode block; with exp_reservedecode0 the allocation fails as vglMalloc's did).
void *page_reserve_runner_malloc(size_t size) {
	int frag = frag_decode && size == PR_BLOCK_BYTES + 32 && sceKernelGetThreadId() == vgl_gl_thread;
	if (frag && !decode_enabled)
		return NULL;
	if (enabled != 1 || !decode_enabled || sceKernelGetThreadId() != vgl_gl_thread)
		return vglMalloc(size);
	void *p = frag ? NULL : malloc(size);
	if (p)
		return p;
	if (size <= PR_HELD_BYTES && !decode_out) {
		int i = 0;
		while (!decode_block && i < PR_RESERVE_BLOCKS && !blocks[i])
			i++;
		void **from = decode_block ? &decode_block : i < PR_RESERVE_BLOCKS ? &blocks[i] : NULL;
		if (from) {
			if (from != &decode_block)
				debugPrintf("[PAGERESERVE] no decode block: the runner's %uKB allocation from a block held for pages\n",
					(unsigned)(size / 1024));
			p = *from;
			*from = NULL;
			decode_out = p;
			page_reserve_decodes++;
			return p;
		}
	}
	static const vglMemType pools[] = {VGL_MEM_RAM, VGL_MEM_PHYCONT, VGL_MEM_BUDGET, VGL_MEM_VRAM};
	for (int i = 0; i < 4 && !frag; i++)
		if ((p = vglAlloc(size, pools[i])))
			return p;
	page_reserve_runner_fails++;
	debugPrintf("[PAGERESERVE] the runner's %uKB allocation fits nowhere (decode block %s): failed without a forced "
		"collection\n", (unsigned)(size / 1024), decode_out ? "in use" : "not held");
	return NULL;
}

// main.c's free and realloc for the runner: whether p is the held block the runner got, which then goes back to the
// reserve (free: the decode block first) or is forgotten (realloc, off the GL thread). Returns 1 if the caller must
// not free p.
int page_reserve_runner_free(void *p, int keep) {
	if (!p || p != decode_out)
		return 0;
	decode_out = NULL;
	if (!keep || sceKernelGetThreadId() != vgl_gl_thread)
		return 0;
	if (!decode_block) {
		decode_block = p;
		return 1;
	}
	for (int i = 0; i < PR_RESERVE_BLOCKS; i++)
		if (!blocks[i]) {
			blocks[i] = p;
			return 1;
		}
	return 0;
}

// vitaGL's eviction callback (page_evict.c), when `size` still doesn't fit in VRAM: a held block for vitaGL's newlib
// fallback if none of its other pools can take the allocation
void page_reserve_release(size_t size, size_t alignment) {
	if (enabled != 1 || handoff || size < PR_MIN_SIZE || size > PR_BLOCK_BYTES ||
		sceKernelGetThreadId() != vgl_gl_thread)
		return;
	int i = 0;
	while (i < PR_RESERVE_BLOCKS && (!blocks[i] || ((uintptr_t)blocks[i] & (alignment - 1))))
		i++;
	if (i == PR_RESERVE_BLOCKS)
		return;
	// vitaGL's next pools after VRAM: if one of them takes it, the reserve stays
	static const vglMemType pools[] = {VGL_MEM_RAM, VGL_MEM_PHYCONT, VGL_MEM_BUDGET};
	for (int p = 0; p < 3; p++) {
		void *probe = vgl_memalign(alignment, size, pools[p]);
		if (probe) {
			vgl_free(probe);
			return;
		}
	}
	handoff = blocks[i];
	blocks[i] = NULL;
	debugPrintf("[PAGERESERVE] %uKB didn't fit in VRAM or vitaGL's pools: a held heap block handed over (%d left)\n",
		(unsigned)(size / 1024), held());
}

// Between frames: a block not handed out after all is held again; blocks are taken back while fewer than
// PR_RESERVE_BLOCKS pages are in the heap (a flush freed them). Both checks are cheap (a walk of the runner's texture
// list, a failed malloc), so this runs every frame: right after a recovery flush frees the pages in the heap, before
// the room reloads them. The decode block is taken again if the runner kept it (an allocation that wasn't a page's
// decode buffer) or freed it off the GL thread.
void page_reserve_frame(void) {
	if (enabled != 1)
		return;
	static unsigned logged_decodes;
	if (page_reserve_decodes != logged_decodes) {
		debugPrintf("[PAGERESERVE] the runner's page decode buffer came from the decode block %u time(s) (session "
			"%u)%s\n", page_reserve_decodes - logged_decodes, page_reserve_decodes,
			decode_out ? "; the runner still has it" : "");
		logged_decodes = page_reserve_decodes;
	}
	if (decode_enabled && !decode_block && (decode_block = malloc(PR_HELD_BYTES)))
		debugPrintf("[PAGERESERVE] decode block taken again\n");
	if (handoff) {
		for (int i = 0; i < PR_RESERVE_BLOCKS; i++)
			if (!blocks[i]) {
				blocks[i] = handoff;
				break;
			}
		handoff = NULL;
	}
	if (held() == PR_RESERVE_BLOCKS)
		return;
	extern int page_evict_pages_in_heap(void);
	int want = PR_RESERVE_BLOCKS - page_evict_pages_in_heap();
	if (held() >= want)
		return;
	int took = take(want);
	if (took) {
		page_reserve_refills += took;
		debugPrintf("[PAGERESERVE] %d heap block(s) taken back: %d held\n", took, held());
	}
}

// exp_fragheap (test): the free heap holes of 8 MB or more, and the untouched tail, are split into 8 MB pieces with
// every other piece kept, so no 16 MB block is left outside the reserve (what a long session reaches). exp_fragpools
// (test, with it): vitaGL's RAM, PHYCONT and CDLG pools are taken in 4 MB pieces and kept, so none of them can hold a
// page's decode buffer either (at the crash PHYCONT had 10 MB free and VRAM 5 MB).
void page_reserve_fragheap(void) {
	if (!exp_flag("fragheap"))
		return;
	enum { PIECE = 8 * 1024 * 1024, MAX = 64 };
	void *p[MAX];
	int n = 0;
	while (n < MAX && (p[n] = malloc(PIECE)))
		n++;
	// A few 1 MB blocks for what's left of the tail, so it can't hold a page either
	for (int i = 0; i < 16; i++)
		if (!malloc(1024 * 1024))
			break;
	for (int i = 0; i < n; i += 2)
		free(p[i]);
	debugPrintf("[PAGERESERVE] exp_fragheap: %d 8 MB pieces taken, every other one freed\n", n);
	if (!exp_flag("fragpools"))
		return;
	static const vglMemType pools[] = {VGL_MEM_RAM, VGL_MEM_PHYCONT, VGL_MEM_BUDGET};
	int pieces = 0;
	for (int i = 0; i < 3; i++)
		while (vglAlloc(4 * 1024 * 1024, pools[i]))
			pieces++;
	debugPrintf("[PAGERESERVE] exp_fragpools: %d 4 MB pieces of vitaGL's pools kept | ram %uKB phycont %uKB cdlg %uKB "
		"free\n", pieces, (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_PHYCONT) / 1024),
		(unsigned)(vglMemFree(VGL_MEM_BUDGET) / 1024));
}
