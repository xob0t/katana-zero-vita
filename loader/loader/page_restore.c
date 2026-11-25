/*
 * page_restore.c
 *
 * Texture pages the game asked for, put back after the loader's page recovery flush.
 *
 * The game declares the texture pages it wants resident: obj_texture_page_master's Room Start event calls
 * texture_prefetch for the room's texture groups (room_level_select: pages 19 and 45, the UI and the button glyphs;
 * room_factory_0: pages 3 and 40). The runner then never unloads a page. VRAM holds about six 16 MB pages, so when a
 * room's pages don't fit, one goes outside VRAM, and the loader's page recovery (katana_display.c) flushes every page
 * after that frame so the room's pages reload into VRAM. That also throws away the pages the game had just
 * prefetched, and they then load (180-340 ms each, on the game thread) the first time something draws from them,
 * in the middle of play. In room_factory_0 the room's first frame spills page 23 (the skyline), the flush evicts
 * pages 45, 40 and 3, and they load a few seconds later as the player nears the clothesline: 330 + 156 + 175 ms of
 * stalls in four frames (a 357 ms hitch in a playtest log at the same spot).
 *
 * So when the recovery flush runs, the pages the game had asked for that were still resident are noted, and after the
 * next presented frame (which has reloaded what the room draws, so those pages get VRAM first) they are loaded again
 * between frames with the runner's own Graphics::PrefetchTexture: the current room's requests first, in the game's
 * order, then the other requested pages, most recent request first. They go into VRAM while they fit (an allocation
 * probe of the page's size; one that lands outside VRAM anyway is flushed again and loads on use). The ones that
 * don't fit any more would load outside VRAM anyway once something draws from them, mid-play, so they are loaded
 * outside VRAM now, while the PHYCONT pool keeps PR_OUTSIDE_RESERVE free; otherwise they load on use, as before.
 * The current room's own requests also load when PHYCONT is short of that, wherever vitaGL would put them when the room
 * draws from them: its pools, or a heap block page_reserve.c holds for pages outside VRAM, as long as another held
 * block stays for a page that loads on use. A room entered with one page already in PHYCONT left ~10 MB there, so its
 * prefetched page was skipped and loaded mid-play from a held heap block: room_club_1's page 23 five seconds in (187
 * ms, every playtest), room_club_4's page 14, room_club_0's page 37, room_club_4_2's page 37. Other rooms' requests
 * (level select's UI pages) keep the PHYCONT rule. exp_pagerestoreown0 restores the PHYCONT-only rule for all pages.
 * In room_factory_0 that puts 3 and 40 back into VRAM and 45 (prompts and button glyphs) outside it; before, page 3
 * (the interior's tiles) ended up outside VRAM. The ~680 ms of loads move into the room's first frames.
 * Nothing runs while a scene is open or a virtual enemy capture is pending (as in title_pages.c); if that holds for
 * PR_WAIT_FRAMES frames, or the room changes, the pages load on use. Only pages that were resident before the flush
 * are loaded. exp_pagerestore0 disables; exp_pagerestorevram loads only the pages that fit in VRAM.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>
#include <stdio.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

// Texture (APK 1.1.10 runner, see title_pages.c): +0x18 flags (0x100: not flushable), +0x1c GL name (-1 when not
// loaded), +0x60 file data, +0x64 size, +0x68 next in Texture::ms_pFirst's list
enum { T_FLAGS = 0x18, T_GLID = 0x1c, T_DATA = 0x60, T_SIZE = 0x64, T_NEXT = 0x68 };
#define TEX_U32(t, off) (*(uint32_t *)((uint8_t *)(t) + (off)))
#define NOT_LOADED 0xffffffffu

#define MAX_REQUESTS 64
#define MAX_RESTORE 16
#define PR_WAIT_FRAMES 30
#define PR_OUTSIDE_RESERVE (8 * 1024 * 1024)

typedef struct {
	void *tex;
	int room;
	uint32_t seq;
} request_t;

static int enabled, outside_ok, own_ok;
extern int vgl_failed_allocs;
extern unsigned page_reserve_handoffs;
extern int page_reserve_spare(size_t alignment);
static request_t requests[MAX_REQUESTS];
static int nrequests, in_restore;
static uint32_t request_seq;
static void (*orig_prefetch)(void *tex);
static void (*graphics_flush)(void);
static void (*flush_texture)(void *tex);
static void (*invalidate_texture_state)(void);
static void **texture_first;
static uint8_t **wad_base, **wad_end;

static void *pending[MAX_RESTORE];
static uint8_t pending_own[MAX_RESTORE]; // requested in the room the flush ran in
static int npending, pending_room = -2, pending_wait;

extern size_t vgl_gpu_fallback_bytes;
extern GLboolean vgl_scene_open;
extern int katana_texdrain_now(const char *what);
extern int virtual_capture_busy(void);
extern void *vgl_memalign(size_t alignment, size_t size, vglMemType type);
extern void vgl_free(void *ptr);

static uint32_t rd32(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

// A texture page of game.droid ("2zoq" blob inside the WAD), of at least 1024x1024
static int big_page(void *t) {
	const uint8_t *b = (const uint8_t *)TEX_U32(t, T_DATA);
	if (!*wad_base || b < *wad_base || b + 16 > *wad_end || TEX_U32(t, T_SIZE) < 16 || memcmp(b, "2zoq", 4))
		return 0;
	return (b[4] | b[5] << 8) * (b[6] | b[7] << 8) >= 1024 * 1024;
}

static size_t page_bytes(void *t) {
	const uint8_t *b = (const uint8_t *)TEX_U32(t, T_DATA);
	return (size_t)(b[4] | b[5] << 8) * (b[6] | b[7] << 8) * 4;
}

// TXTR index of a page, for the log (-1 if not found)
static int page_index(void *t) {
	const uint8_t *base = *wad_base, *blob = (const uint8_t *)TEX_U32(t, T_DATA);
	uint32_t size = *wad_end - base;
	for (uint32_t c = 8; c + 12 <= size; c += 8 + rd32(base + c + 4)) {
		if (memcmp(base + c, "TXTR", 4))
			continue;
		uint32_t n = rd32(base + c + 8);
		for (uint32_t i = 0; i < n && c + 12 + 4 * i + 4 <= size; i++) {
			uint32_t e = rd32(base + c + 12 + 4 * i);
			if (e + 28 <= size && base + rd32(base + e + 24) == blob)
				return i;
		}
		break;
	}
	return -1;
}

// Graphics::PrefetchTexture (entry hook): the pages the game asks for, with the room it asked in
static void prefetch_hook(void *tex) {
	if (!in_restore && tex && current_room) {
		int i, oldest = 0;
		for (i = 0; i < nrequests && requests[i].tex != tex; i++)
			if (requests[i].seq < requests[oldest].seq)
				oldest = i;
		if (i == nrequests) {
			if (nrequests < MAX_REQUESTS)
				nrequests++;
			else
				i = oldest;
		}
		if (requests[i].tex != tex || requests[i].room != *current_room)
			requests[i].seq = ++request_seq; // repeated calls in one room keep the first one's order
		requests[i].tex = tex;
		requests[i].room = *current_room;
	}
	orig_prefetch(tex);
}

// For page eviction (page_evict.c): 2 if the game asked for this page in the current room, 1 if in another room (the
// pages level select asks for, the UI and the button glyphs, are drawn in every level), 0 if not
int page_restore_requested(void *tex) {
	int room = current_room ? *current_room : -1;
	for (int i = 0; i < nrequests; i++)
		if (requests[i].tex == tex)
			return requests[i].room == room ? 2 : 1;
	return 0;
}

static int is_texture(void *tex) {
	for (void *t = *texture_first; t; t = (void *)TEX_U32(t, T_NEXT))
		if (t == tex)
			return 1;
	return 0;
}

// Right before the recovery flush: the requested pages that are resident, in the order they'll be restored
void page_restore_before_flush(void) {
	npending = 0;
	if (!enabled || !current_room)
		return;
	int room = *current_room;
	request_t order[MAX_REQUESTS];
	int n = 0;
	for (int i = 0; i < nrequests; i++)
		if (is_texture(requests[i].tex) && TEX_U32(requests[i].tex, T_GLID) != NOT_LOADED &&
				!(TEX_U32(requests[i].tex, T_FLAGS) & 0x100) && big_page(requests[i].tex))
			order[n++] = requests[i];
	// This room's requests first in the game's order, then the rest, most recent request first
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0; b--) {
			request_t *x = &order[b - 1], *y = &order[b];
			int xr = x->room == room, yr = y->room == room;
			int swap = xr != yr ? yr : xr ? y->seq < x->seq : y->seq > x->seq;
			if (!swap)
				break;
			request_t t = *x;
			*x = *y;
			*y = t;
		}
	for (int i = 0; i < n && npending < MAX_RESTORE; i++) {
		pending_own[npending] = order[i].room == room;
		pending[npending++] = order[i].tex;
	}
	pending_room = room;
	pending_wait = 0;
}

// The game flushed the pages itself (draw_texture_flush): nothing is loaded back
void page_restore_cancel(void) {
	if (npending)
		debugPrintf("[PAGERESTORE] the game flushed the texture pages: %d pages load on use\n", npending);
	npending = 0;
}

// In katana_after_swap after the texture drain, every presented frame. Returns 1 if it loaded pages; *spill_seen is
// then the spill count to take as seen.
int page_restore_after_swap(size_t *spill_seen) {
	if (!enabled || !npending)
		return 0;
	if (!current_room || *current_room != pending_room) {
		debugPrintf("[PAGERESTORE] room changed: %d pages load on use\n", npending);
		npending = 0;
		return 0;
	}
	if (virtual_capture_busy() || vgl_scene_open || (graphics_flush(), vgl_scene_open)) {
		if (++pending_wait >= PR_WAIT_FRAMES) {
			debugPrintf("[PAGERESTORE] no free moment in %d frames: %d pages load on use\n", PR_WAIT_FRAMES, npending);
			npending = 0;
		}
		return 0;
	}
	uint32_t t0 = sceKernelGetProcessTimeLow();
	size_t free_before = vglMemFree(VGL_MEM_VRAM);
	int loaded = 0, skipped = 0, already = 0, outside = 0, kept_outside = 0, nrest = 0;
	void *rest[MAX_RESTORE];
	uint8_t rest_own[MAX_RESTORE];
	char skipped_list[96];
	int slen = 0;
	skipped_list[0] = 0;
	char list[200];
	int len = 0;
	list[0] = 0;
	in_restore = 1;
	for (int i = 0; i < npending; i++) {
		void *t = pending[i];
		if (!is_texture(t))
			continue;
		if (TEX_U32(t, T_GLID) != NOT_LOADED) {
			already++; // the presented frame drew from it
			continue;
		}
		// Into VRAM if it fits now (a probe of its size)
		void *probe = vgl_memalign(16, page_bytes(t), VGL_MEM_VRAM);
		if (!probe) {
			rest_own[nrest] = pending_own[i];
			rest[nrest++] = t;
			continue;
		}
		vgl_free(probe);
		size_t spill = vgl_gpu_fallback_bytes;
		uint32_t p0 = sceKernelGetProcessTimeLow();
		orig_prefetch(t);
		int idx = page_index(t);
		if (vgl_gpu_fallback_bytes != spill) {
			// Outside VRAM after all: flushed again, it loads on use
			flush_texture(t);
			outside++;
		} else
			loaded++;
		if (len < (int)sizeof(list) - 24)
			len += snprintf(list + len, sizeof(list) - len, " %d%s(%.0fms)", idx, vgl_gpu_fallback_bytes != spill ? "!" : "",
				(sceKernelGetProcessTimeLow() - p0) / 1000.0f);
	}
	// The pages that don't fit in VRAM any more were resident before the flush too, and if the room draws from them they
	// load then, outside VRAM, in the middle of play (room_factory_0: page 45 near the clothesline, 330 ms). They are
	// loaded now, outside VRAM where they would end up anyway, while the PHYCONT pool keeps PR_OUTSIDE_RESERVE free.
	// The current room's own requests also go wherever the room's draw would put them (vitaGL's pools after VRAM, else a
	// held heap block, page_reserve.c) while a held block stays for a page that loads on use.
	for (int i = 0; i < nrest; i++) {
		void *t = rest[i];
		size_t bytes = page_bytes(t);
		void *probe = outside_ok && vglMemFree(VGL_MEM_PHYCONT) >= bytes + PR_OUTSIDE_RESERVE ?
			vgl_memalign(16, bytes, VGL_MEM_PHYCONT) : NULL;
		if (!probe && outside_ok && own_ok && rest_own[i]) {
			static const vglMemType pools[] = {VGL_MEM_RAM, VGL_MEM_PHYCONT, VGL_MEM_BUDGET};
			for (int p = 0; p < 3 && !probe; p++)
				probe = vgl_memalign(16, bytes, pools[p]);
			if (!probe && page_reserve_spare(16) > 0)
				probe = (void *)1; // vitaGL is handed a held block (page_reserve_release, from page_evict.c's callback)
		}
		if (!probe) {
			skipped++;
			if (slen < (int)sizeof(skipped_list) - 16)
				slen += snprintf(skipped_list + slen, sizeof(skipped_list) - slen, " %d%s", page_index(t),
					rest_own[i] ? "" : "(other room)");
			continue;
		}
		if (probe != (void *)1)
			vgl_free(probe);
		uint32_t p0 = sceKernelGetProcessTimeLow();
		int failed = vgl_failed_allocs;
		unsigned handoffs = page_reserve_handoffs;
		orig_prefetch(t);
		if (vgl_failed_allocs != failed) {
			// Not allocated after all (it would draw black): flushed again, it loads on use as before
			flush_texture(t);
			skipped++;
			continue;
		}
		kept_outside++;
		if (len < (int)sizeof(list) - 24)
			len += snprintf(list + len, sizeof(list) - len, " %d*%s(%.0fms)", page_index(t),
				page_reserve_handoffs != handoffs ? "h" : "", (sceKernelGetProcessTimeLow() - p0) / 1000.0f);
	}
	in_restore = 0;
	invalidate_texture_state();
	if (outside)
		katana_texdrain_now("the page restore's pages outside VRAM");
	*spill_seen = vgl_gpu_fallback_bytes;
	debugPrintf("[PAGERESTORE] room %d: loaded %d requested pages back after the recovery flush:%s (* outside VRAM, h in a "
		"held heap block) | %d drawn "
		"already, %d not loaded%s%s, %d unexpectedly outside VRAM (flushed) | free vram %uKB -> %uKB, phycont %uKB, %.1f ms\n",
		pending_room, loaded + kept_outside, len ? list : " none", already, skipped, slen ? ":" : "", skipped_list, outside,
		free_before / 1024, vglMemFree(VGL_MEM_VRAM) / 1024, vglMemFree(VGL_MEM_PHYCONT) / 1024, (sceKernelGetProcessTimeLow() - t0) / 1000.0f);
	npending = 0;
	return loaded || outside || kept_outside;
}

void page_restore_install(void) {
	if (exp_flag("pagerestore0"))
		return;
	uintptr_t prefetch = so_symbol(&yoyoloader_mod, "_ZN8Graphics15PrefetchTextureEPv");
	graphics_flush = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics5FlushEv");
	flush_texture = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics12FlushTextureEPv");
	invalidate_texture_state = (void *)so_symbol(&yoyoloader_mod, "_Z23_InvalidateTextureStatev");
	texture_first = (void **)so_symbol(&yoyoloader_mod, "_ZN7Texture9ms_pFirstE");
	wad_base = (uint8_t **)so_symbol(&yoyoloader_mod, "g_pWADBaseAddress");
	wad_end = (uint8_t **)so_symbol(&yoyoloader_mod, "g_pWADEndAddress");
	// The Texture offsets above, as the runner uses them (title_pages.c checks the same instructions):
	// PrefetchTexture: ldr r0, [r4, #0x1c]; TextureFlush: ldr r5, [r5, #0x68], ldrb r0, [r5, #0x19]
	uintptr_t flush_all = so_symbol(&yoyoloader_mod, "_ZN8Graphics12TextureFlushEb");
	int layout = prefetch && flush_all && *(uint32_t *)(prefetch + 0x30) == 0xe594001c &&
		*(uint32_t *)(flush_all + 0x24) == 0xe5955068 && *(uint32_t *)(flush_all + 0x38) == 0xe5d50019;
	if (layout && graphics_flush && flush_texture && invalidate_texture_state && texture_first && wad_base && wad_end)
		orig_prefetch = entry_hook(prefetch, (void *)prefetch_hook);
	enabled = orig_prefetch != NULL;
	outside_ok = !exp_flag("pagerestorevram"); // exp_pagerestorevram: only pages that fit in VRAM
	own_ok = !exp_flag("pagerestoreown0");     // exp_pagerestoreown0: outside VRAM only into PHYCONT, for every page
	debugPrintf("requested pages back after the page recovery flush: %s\n", enabled ? "on" : "NOT installed");
}
