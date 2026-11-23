/*
 * title_pages.c
 *
 * The title screen's texture pages, loaded between frames.
 *
 * room_title draws from five 2048x2048 texture pages (TXTR 23, 21, 33, 19, and 45 once the menu shows): 80 MB of
 * the 96.6 MB of VRAM. The runner loads a page the first time something draws from it, on the game thread, in
 * 180-340 ms. Entering the title from a level, VRAM still holds the level's pages: the title's first pages land
 * outside VRAM, the spill flush (katana_display.c) empties VRAM after that frame, and every page loads again over
 * the next frames (1.4-1.9 s of stalls in the first 5 s, 13-30 fps). At boot the save/options prompts leave pages
 * 19, 33 and 45 resident plus two half pages; page 21 spills and the flush reloads four pages in one ~1 s frame.
 *
 * So right after the first title frame is presented (katana_after_swap, between frames, after the texture drain of
 * PR #55 found no scene open), the title's pages are made resident in VRAM in one go:
 * - nothing is done while a virtual enemy capture waits to be drawn (a runner barrier such as FlushTexture would draw
 *   it and open a scene);
 * - the runner's batch is flushed (Graphics::Flush); if that opened a scene, nothing more is done;
 * - texture pages the title doesn't use are flushed (Graphics::FlushTexture, what draw_texture_flush does per page),
 *   and so are the title's pages that went outside VRAM (their memory block isn't CDRAM). If texture data went
 *   outside VRAM that isn't one of those pages, nothing is done: the spill flush handles the room as before;
 * - the flushed pages are freed through the scene-aware texture drain (katana_texdrain_now); if it has to wait, the
 *   pages load on use as before;
 * - if the missing title pages don't all fit at once (allocation probes; VRAM can be too fragmented: page 33 once
 *   went outside VRAM from room_prison_3), all pages are flushed (draw_texture_flush, as the spill flush would do)
 *   and the title loads them on use, as before;
 * - otherwise each missing page is loaded with the runner's own texture_prefetch (Graphics::PrefetchTexture, the
 *   normal page-load path). If one still lands outside VRAM, the spill flush runs for it as before.
 * The title then runs without page loads; the stall is one frame at the start of the title.
 * Pages are told apart by their TXTR index (the runner's in-memory game.droid), so the set holds whatever the Vita
 * glyph option did to page 45. exp_titlepages0 disables it (checked before anything is set up).
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

// Texture (APK 1.1.10 runner): +0x18 flags (0x100: not flushable), +0x1c GL name (-1 when not loaded), +0x60 file
// data, +0x64 size, +0x68 next in Texture::ms_pFirst's list
enum { T_FLAGS = 0x18, T_GLID = 0x1c, T_DATA = 0x60, T_SIZE = 0x64, T_NEXT = 0x68 };
#define TEX_U32(t, off) (*(uint32_t *)((uint8_t *)(t) + (off)))
#define NOT_LOADED 0xffffffffu

#define MAX_PAGES 128
#define TITLE_VRAM_RESERVE (8 * 1024 * 1024)

// The title's pages in Katana ZERO 1.1.10's game.droid, in the order it first draws from them
static const int title_pages[] = { 23, 21, 33, 19, 45 };
#define TITLE_PAGES (int)(sizeof(title_pages) / sizeof(title_pages[0]))

static int enabled;
static void (*graphics_flush)(void);
static void (*flush_texture)(void *tex);
static void (*prefetch_texture)(void *tex);
static void (*invalidate_texture_state)(void);
static int (*room_find)(const char *name);
static builtin_fn draw_texture_flush;
static void (**gl_bind_texture)(GLenum target, GLuint name);
static void **texture_first;
static uint8_t **wad_base, **wad_end;

extern size_t vgl_gpu_fallback_bytes;
extern GLboolean vgl_scene_open;
extern int katana_texdrain_now(const char *what);
extern int virtual_capture_busy(void);
extern void *vgl_memalign(size_t alignment, size_t size, vglMemType type);
extern void vgl_free(void *ptr);

static int title_room = -2, last_room = -2;
static void *title_tex[TITLE_PAGES];
static size_t fallback_before; // vgl_gpu_fallback_bytes as the current frame started (title_pages_snapshot)

static uint32_t rd32(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

// The runner Texture of each title page, through the TXTR entries of the in-memory game.droid (file offsets)
static int find_title_pages(void) {
	const uint8_t *base = *wad_base, *end = *wad_end;
	if (!base || !end || end <= base + 8 || memcmp(base, "FORM", 4))
		return 0;
	uint32_t size = end - base;
	for (uint32_t c = 8; c + 12 <= size; c += 8 + rd32(base + c + 4)) {
		if (memcmp(base + c, "TXTR", 4))
			continue;
		uint32_t n = rd32(base + c + 8);
		for (int i = 0; i < TITLE_PAGES; i++) {
			title_tex[i] = NULL;
			uint32_t at = c + 12 + 4 * title_pages[i];
			if ((uint32_t)title_pages[i] >= n || at + 4 > size)
				return 0;
			uint32_t e = rd32(base + at);
			if (e + 28 > size)
				return 0;
			const uint8_t *blob = base + rd32(base + e + 24);
			for (void *t = *texture_first; t; t = (void *)TEX_U32(t, T_NEXT))
				if ((const uint8_t *)TEX_U32(t, T_DATA) == blob)
					title_tex[i] = t;
			if (!title_tex[i] || blob + 16 > end || memcmp(blob, "2zoq", 4))
				return 0;
		}
		return 1;
	}
	return 0;
}

static int title_index(void *t) {
	for (int i = 0; i < TITLE_PAGES; i++)
		if (title_tex[i] == t)
			return i;
	return -1;
}

static size_t page_bytes(void *t) {
	const uint8_t *b = (const uint8_t *)TEX_U32(t, T_DATA);
	return (size_t)(b[4] | b[5] << 8) * (b[6] | b[7] << 8) * 4;
}

// A texture page of game.droid ("2zoq" blob inside the WAD), of at least 1024x1024
static int big_page(void *t) {
	const uint8_t *b = (const uint8_t *)TEX_U32(t, T_DATA);
	if (b < *wad_base || b + 16 > *wad_end || TEX_U32(t, T_SIZE) < 16 || memcmp(b, "2zoq", 4))
		return 0;
	return (b[4] | b[5] << 8) * (b[6] | b[7] << 8) >= 1024 * 1024;
}

// Whether all these allocations fit in VRAM at once with TITLE_VRAM_RESERVE to spare, as the pages will be allocated
// (probes, never used by the GPU, freed right away)
static int all_fit(const size_t *bytes, int n) {
	void *probe[TITLE_PAGES];
	int ok = 1;
	for (int i = 0; i < n; i++)
		if (!(probe[i] = ok ? vgl_memalign(16, bytes[i], VGL_MEM_VRAM) : NULL))
			ok = 0;
	ok = ok && vglMemFree(VGL_MEM_VRAM) >= TITLE_VRAM_RESERVE;
	for (int i = 0; i < n; i++)
		if (probe[i])
			vgl_free(probe[i]);
	return ok;
}

static int missing_pages(size_t *bytes, int *which) {
	int n = 0;
	for (int i = 0; i < TITLE_PAGES; i++) {
		if (TEX_U32(title_tex[i], T_GLID) != NOT_LOADED)
			continue;
		bytes[n] = page_bytes(title_tex[i]);
		which[n++] = i;
	}
	return n;
}

// Whether a loaded texture's storage is in VRAM (vitaGL's VRAM pool is one CDRAM memory block)
static int in_vram(void *t) {
	(*gl_bind_texture)(GL_TEXTURE_2D, TEX_U32(t, T_GLID));
	void *data = vglGetTexDataPointer(GL_TEXTURE_2D);
	SceKernelMemBlockInfo info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	return data && sceKernelGetMemBlockInfoByAddr(data, &info) >= 0 && info.type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
}

// Returns 1 if this frame's texture data outside VRAM was dealt with; *spill_seen is then the spill count to take
// as seen (from before the pages loaded here, so a page of these that still misses VRAM gets the spill flush)
static int make_title_resident(size_t *spill_seen) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	if (virtual_capture_busy()) {
		debugPrintf("[TITLEPAGES] a virtual capture is pending; title pages load on use\n");
		return 0;
	}
	graphics_flush();
	if (vgl_scene_open) {
		debugPrintf("[TITLEPAGES] the runner's batch opened a scene; title pages load on use\n");
		return 0;
	}
	// Title pages outside VRAM are loaded again; any other texture data outside VRAM is left to the spill flush
	size_t spilled = vgl_gpu_fallback_bytes - fallback_before, outside = 0;
	int outside_mask = 0;
	if (spilled) {
		for (int i = 0; i < TITLE_PAGES; i++)
			if (TEX_U32(title_tex[i], T_GLID) != NOT_LOADED && !in_vram(title_tex[i])) {
				outside_mask |= 1 << i;
				outside += page_bytes(title_tex[i]);
			}
		invalidate_texture_state();
		if (outside < spilled) {
			debugPrintf("[TITLEPAGES] %uKB went outside VRAM, %uKB of it title pages; title pages load on use\n",
				(unsigned)(spilled / 1024), (unsigned)(outside / 1024));
			return 0;
		}
	}
	int evicted = 0, reloaded = 0;
	void *next;
	for (void *t = *texture_first; t; t = next) {
		next = (void *)TEX_U32(t, T_NEXT);
		if (TEX_U32(t, T_GLID) == NOT_LOADED || (TEX_U32(t, T_FLAGS) & 0x100) || !big_page(t))
			continue;
		int i = title_index(t);
		if (i < 0)
			evicted++;
		else if (outside_mask & (1 << i))
			reloaded++;
		else
			continue;
		flush_texture(t);
	}
	*spill_seen = vgl_gpu_fallback_bytes; // the title pages outside VRAM are flushed
	if ((evicted || reloaded) && !katana_texdrain_now("the title's page eviction")) {
		debugPrintf("[TITLEPAGES] evicted %d pages, reloaded %d; the drain has to wait, title pages load on use\n",
			evicted, reloaded);
		return 1;
	}
	// The pages left in VRAM can leave it too fragmented for the missing ones: then everything is flushed, as the
	// spill flush would do, and the pages load on use, as before (loading all five here made one 1.8 s frame from
	// room_prison_3, against 1.5 s of stalls spread over the next frames)
	size_t bytes[TITLE_PAGES];
	int which[TITLE_PAGES], n = missing_pages(bytes, which), flushed_all = 0;
	if (n && !all_fit(bytes, n)) {
		gml_rvalue result = {0};
		draw_texture_flush(&result, NULL, NULL, 0, NULL); // drains by itself (PR #55)
		flushed_all = 1;
		n = 0;
	}
	uint32_t t1 = sceKernelGetProcessTimeLow();
	*spill_seen = vgl_gpu_fallback_bytes;
	int loaded = 0;
	for (int k = 0; k < n; k++) {
		if (!all_fit(&bytes[k], 1))
			break; // the rest load on use, as before
		size_t spill = vgl_gpu_fallback_bytes;
		prefetch_texture(title_tex[which[k]]);
		if (vgl_gpu_fallback_bytes != spill)
			debugPrintf("[TITLEPAGES] page %d went outside VRAM\n", title_pages[which[k]]);
		loaded++;
	}
	invalidate_texture_state();
	debugPrintf("[TITLEPAGES] first title frame%s: evicted %d pages, reloaded %d%s; loaded %d of %d pages in %.1fms; "
		"free vram %uKB; %.1fms in all%s\n", spilled ? " (spilled)" : "", evicted, reloaded,
		flushed_all ? "; VRAM too fragmented for the rest: flushed all, pages load on use" : "", loaded, n,
		(sceKernelGetProcessTimeLow() - t1) / 1000.0f, vglMemFree(VGL_MEM_VRAM) / 1024,
		(sceKernelGetProcessTimeLow() - t0) / 1000.0f, vgl_scene_open ? " (a scene is open now)" : "");
	return 1;
}

// In katana_after_swap after the texture drain (no scene open), before the spill flush. Returns 1 if it dealt with
// this frame's texture data outside VRAM (*spill_seen: the spill count to take as seen).
int title_pages_after_swap(size_t *spill_seen) {
	if (!enabled || !current_room)
		return 0;
	int room = *current_room;
	if (room == last_room)
		return 0;
	last_room = room;
	if (title_room == -2)
		title_room = room_find("room_title");
	return room == title_room && room >= 0 && find_title_pages() && make_title_resident(spill_seen);
}

// At the very end of katana_after_swap (after any spill flush): the state the next frame starts from
void title_pages_snapshot(void) {
	fallback_before = vgl_gpu_fallback_bytes;
}

void title_pages_install(void) {
	if (exp_flag("titlepages0"))
		return;
	graphics_flush = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics5FlushEv");
	flush_texture = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics12FlushTextureEPv");
	prefetch_texture = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics15PrefetchTextureEPv");
	invalidate_texture_state = (void *)so_symbol(&yoyoloader_mod, "_Z23_InvalidateTextureStatev");
	room_find = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
	gl_bind_texture = (void *)so_symbol(&yoyoloader_mod, "FuncPtr_glBindTexture");
	draw_texture_flush = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z23F_YoYo_DrawTextureFlushR6RValueP9CInstanceS2_iPS_");
	texture_first = (void **)so_symbol(&yoyoloader_mod, "_ZN7Texture9ms_pFirstE");
	wad_base = (uint8_t **)so_symbol(&yoyoloader_mod, "g_pWADBaseAddress");
	wad_end = (uint8_t **)so_symbol(&yoyoloader_mod, "g_pWADEndAddress");
	uintptr_t load = so_symbol(&yoyoloader_mod, "_Z19LoadTextureFromQOIFP7Texture10eMipEnable");
	uintptr_t flush_all = so_symbol(&yoyoloader_mod, "_ZN8Graphics12TextureFlushEb");
	uintptr_t prefetch = (uintptr_t)prefetch_texture;
	// The Texture offsets above, as the 1.1.10 runner's own code uses them. LoadTextureFromQOIF: ldrd r8, [r4, #0x60]
	// (data, size), ldr r0, [r4, #0x18] (flags), str r1, [r4, #0x18], str r0, [r4, #0x1c] (GL name).
	// TextureFlush: ldr r5, [r5, #0x68] (next), ldrb r0, [r5, #0x19] (flag 0x100). PrefetchTexture: ldr r0, [r4, #0x1c].
	int layout = load && flush_all && prefetch &&
		*(uint32_t *)(load + 0x38) == 0xe1c486d0 && *(uint32_t *)(load + 0x34) == 0xe5940018 &&
		*(uint32_t *)(load + 0xdc) == 0xe5841018 && *(uint32_t *)(load + 0x120) == 0xe584001c &&
		*(uint32_t *)(flush_all + 0x24) == 0xe5955068 && *(uint32_t *)(flush_all + 0x38) == 0xe5d50019 &&
		*(uint32_t *)(prefetch + 0x30) == 0xe594001c;
	enabled = layout && draw_texture_flush && gl_bind_texture && graphics_flush && flush_texture && invalidate_texture_state && room_find && texture_first &&
		wad_base && wad_end;
	debugPrintf("title pages between frames: %s\n", enabled ? "on" : "NOT installed (unknown runner)");
}
