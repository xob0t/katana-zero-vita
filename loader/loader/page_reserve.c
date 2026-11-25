/*
 * page_reserve.c
 *
 * Newlib heap kept for the texture pages that don't fit in VRAM.
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
 * memalign(16, n) asks malloc for n plus the alignment plus a minimum chunk.
 *
 * So PR_RESERVE_BLOCKS blocks of a page's size are taken from the heap while it still has large holes (right after the
 * runner's copy of game.droid frees the inflated one, and after Startup) and held. When a GPU allocation of at least
 * PR_MIN_SIZE fits neither VRAM (after eviction) nor vitaGL's other pools, a held block is handed over: vitaGL's next
 * memalign (its newlib fallback, right after the callback) gets that block itself (memalign is wrapped for the
 * loader). When vitaGL frees the page, the block goes back to the heap whole, and between frames it is taken again
 * (malloc of the same size fits that hole exactly) while fewer than PR_RESERVE_BLOCKS pages are in the heap. At most
 * PR_RESERVE_BLOCKS x 16 MB of the heap is set aside this way (the runner uses ~140 MB of the 240 MB heap). Game (GL)
 * thread only. exp_pagereserve0 disables; exp_fragheap (test) splits the free heap holes of 8 MB or more after Startup,
 * the state of a long session.
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

extern SceUID vgl_gl_thread;
extern void *vgl_memalign(size_t alignment, size_t size, vglMemType type);
extern void vgl_free(void *ptr);
extern size_t newlib_heap_used(void);
void *__real_memalign(size_t alignment, size_t size);

static int enabled = -1;
static void *blocks[PR_RESERVE_BLOCKS];
static void *handoff; // the block vitaGL's next large memalign gets
unsigned page_reserve_handoffs, page_reserve_refills;

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
	if (enabled < 0)
		enabled = !exp_flag("pagereserve0");
	return enabled;
}

// Takes free blocks until `want` are held; returns how many it took. malloc of the block size fits a freed block's hole
// exactly (memalign would ask for more).
static int take(int want) {
	int took = 0;
	for (int i = 0; i < PR_RESERVE_BLOCKS && held() < want; i++) {
		if (blocks[i])
			continue;
		blocks[i] = malloc(PR_BLOCK_BYTES);
		if (!blocks[i])
			break;
		took++;
	}
	return took;
}

// Right after the runner's copy of game.droid freed the inflated one (apk_prefetch.c), and after Startup
void page_reserve_fill(const char *where) {
	if (!is_enabled())
		return;
	// The first blocks aligned for any texture (vitaGL asks for 16)
	for (int i = 0; i < PR_RESERVE_BLOCKS; i++)
		if (!blocks[i])
			blocks[i] = __real_memalign(64, PR_BLOCK_BYTES);
	debugPrintf("[PAGERESERVE] %s: %d of %d heap blocks of %uKB held for pages outside VRAM (newlib %uKB sbrk'd)\n",
		where, held(), PR_RESERVE_BLOCKS, PR_BLOCK_BYTES / 1024, (unsigned)(newlib_heap_used() / 1024));
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
// the room reloads them.
void page_reserve_frame(void) {
	if (enabled != 1)
		return;
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
// every other piece kept, so no 16 MB block is left outside the reserve (what a long session reaches)
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
}
