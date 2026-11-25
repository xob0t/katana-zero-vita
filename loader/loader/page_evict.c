/*
 * page_evict.c
 *
 * Partial texture page eviction when VRAM runs out.
 *
 * The runner loads texture pages as rooms draw from them and never unloads them, so VRAM fills with pages of earlier
 * rooms and phases. When a new page or surface didn't fit, vitaGL put it in main RAM (slow for the GPU), and the loader
 * then flushed every page after that frame (katana_display.c, texture page recovery): the current room reloaded all
 * of its pages over the next frames. In the secret psychiatrist fight that froze the game for 1.3-1.5 s at both
 * transformations (a 2048x2048 page at the morph into the giant: five pages reloaded at 63-74 ms each; a 1280x720
 * surface at the intro transformation).
 *
 * Instead, vitaGL calls back (vgl_vram_evict_cb, vitagl.patch) right before a GPU allocation would leave VRAM, and the
 * least recently drawn texture pages are unloaded until the request fits:
 * - candidates are game.droid texture pages (the "2zoq" blobs inside the in-memory WAD, which the runner reloads from
 *   there on their next use) whose storage is in VRAM, that vitaGL has not sampled since the current room started or
 *   for EVICT_MIN_AGE frames, and that weren't loaded in that time either (a page loaded this frame is sampled only
 *   when its batch is drawn). At a room change that is every page of the previous rooms the new one hasn't drawn yet
 *   (with only the age rule, a room hop found nothing to unload: the old room's pages were drawn a frame ago). Never the
 *   texture of the runner's pending batch (g_pLastTexture) or a texture vitaGL drew into or attached to a framebuffer,
 *   never a page loaded in the current room that hasn't been drawn yet, and never a page the game asked to have
 *   resident in any room (obj_texture_page_master's Room Start calls texture_prefetch; page_restore.c records the
 *   requests): level select asks for the UI and button glyph pages, and factory_0 draws its glyph page mid-walk;
 * - pages drawn in fewer of the recent rooms go first (a score per page: +1 for each room that drew it, -1 for each
 *   that didn't, up to ROOMS_SCORE_MAX), then the oldest; a page at least as large as the request with the lowest
 *   score is unloaded first (its block alone fits it); a probe allocation after each, until the request fits;
 * - pages are unloaded with the runner's Graphics::FlushTexture, what draw_texture_flush does for every page: the GL
 *   texture is deleted and the Texture marked unloaded, so the runner reloads it on its next use exactly as after a
 *   full flush. vitaGL frees the storage at once when it wasn't sampled for more than FRAME_PURGE_FREQ frames; for a
 *   page sampled more recently (but not in the current frame, so not by the open scene) vgl_tex_idle first waits for
 *   the GPU to finish the submitted scenes (sceGxmFinish, once per frame), so nothing can still read the freed memory.
 * Only on the game (GL) thread, never re-entered. If a room keeps reloading pages it unloaded (more than
 * EVICT_THRASH_RELOADS within EVICT_THRASH_FRAMES), eviction stops for that room: its pages don't fit together, and
 * the spill + one full flush per room (as before) is the cheaper outcome. When eviction can't make room, the
 * allocation falls back as before and the after-swap recovery flush still runs.
 * That is still the case at the psychiatrist's morph into the giant: every resident page had been drawn in the last
 * frames, and the giant's five 16 MB pages next to the battle's surfaces fit in no layout of the VRAM there (23 MB
 * free in pieces of at most 10 MB; render-target surfaces split the small-allocation region). Moving page storage
 * between frames was tried (GPU transfer copies): it can't make a 16 MB block there, and staging through PHYCONT isn't
 * safe with the CPU-cached pools. Leaving the page outside VRAM cost 6-8 fps in the giant phase, so the flush stays.
 * Debug mode logs every page load ([TEXLOAD]) and eviction ([PAGEEVICT]). exp_pageevict0 disables.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

// Texture (APK 1.1.10 runner, see title_pages.c): +0x18 flags (0x100: not flushable), +0x1c GL name (-1 when not
// loaded), +0x60 file data, +0x64 size, +0x68 next in Texture::ms_pFirst's list
enum { T_FLAGS = 0x18, T_GLID = 0x1c, T_DATA = 0x60, T_SIZE = 0x64, T_NEXT = 0x68 };
#define TEX_U32(t, off) (*(uint32_t *)((uint8_t *)(t) + (off)))
#define NOT_LOADED 0xffffffffu
#define OBJ_NOT_USED 0xffffffffu // vitaGL: not sampled since its upload

#define EVICT_MIN_AGE 120         // frames (2 s) without a draw or load before a page may be unloaded
#define EVICT_THRASH_FRAMES 600   // a reload within 10 s of the page's eviction counts as thrash
#define EVICT_THRASH_RELOADS 4    // per room, then eviction stops in that room
#define MAX_TRACK 256
#define ROOMS_SCORE_MAX 8
#define MAX_CANDIDATES 128

extern uint32_t vgl_framecount;
extern size_t vgl_gpu_fallback_bytes;
extern int (*vgl_vram_evict_cb)(size_t size, size_t alignment);
extern void *vgl_tex_usage(GLuint name, uint32_t *last_frame);
int pause_tiles_protects(uint32_t use); // pause_bg.c
extern int vgl_tex_idle(GLuint name);
extern int page_restore_requested(void *tex);
extern void *vgl_memalign(size_t alignment, size_t size, vglMemType type);
extern void vgl_free(void *ptr);

static int enabled, busy, game_thread = -1;
static void (*flush_texture)(void *tex);
static int (*orig_load)(void *tex, int mip);
static void **texture_first, **last_texture;
static uint8_t **wad_base, **wad_end;

// Per runner Texture: the frame it was last loaded in, and the frame it was unloaded here (0: not by us)
static struct { void *tex; uint32_t loaded, evicted; int room; uint8_t rooms; } track[MAX_TRACK];
static int ntrack;

static int room = -2, room_reloads, room_off, room_noroom_logged;
unsigned page_evict_events, page_evict_pages, page_evict_reloads, page_evict_misses;
size_t page_evict_bytes;

static uint32_t rd32(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static int track_find(void *t, int create) {
	for (int i = 0; i < ntrack; i++)
		if (track[i].tex == t)
			return i;
	if (!create)
		return -1;
	if (ntrack == MAX_TRACK) {
		// Keep the most recent loads: drop the oldest entry
		int old = 0;
		for (int i = 1; i < ntrack; i++)
			if (track[i].loaded < track[old].loaded)
				old = i;
		track[old] = track[--ntrack];
	}
	track[ntrack].tex = t;
	track[ntrack].loaded = track[ntrack].evicted = 0;
	track[ntrack].room = -2;
	track[ntrack].rooms = 0;
	return ntrack++;
}

// A texture page of game.droid ("2zoq" blob inside the WAD); its width and height
static int wad_page(void *t, unsigned *w, unsigned *h) {
	const uint8_t *b = (const uint8_t *)TEX_U32(t, T_DATA);
	if (!*wad_base || b < *wad_base || b + 16 > *wad_end || TEX_U32(t, T_SIZE) < 16 || memcmp(b, "2zoq", 4))
		return 0;
	*w = b[4] | b[5] << 8;
	*h = b[6] | b[7] << 8;
	return *w && *h;
}

// The page's TXTR index in game.droid (for the log), -1 if not found
static int page_index(void *t) {
	const uint8_t *base = *wad_base, *end = *wad_end;
	if (!base || !end || end <= base + 8 || memcmp(base, "FORM", 4))
		return -1;
	uint32_t size = end - base, blob = TEX_U32(t, T_DATA) - (uint32_t)base;
	for (uint32_t c = 8; c + 12 <= size; c += 8 + rd32(base + c + 4)) {
		if (memcmp(base + c, "TXTR", 4))
			continue;
		uint32_t n = rd32(base + c + 8);
		for (uint32_t i = 0; i < n && c + 16 + 4 * i <= size; i++) {
			uint32_t e = rd32(base + c + 12 + 4 * i);
			if (e + 28 <= size && rd32(base + e + 24) == blob)
				return i;
		}
		return -1;
	}
	return -1;
}

static int in_vram(void *data) {
	SceKernelMemBlockInfo info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	return sceKernelGetMemBlockInfoByAddr(data, &info) >= 0 && info.type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
}

static int fits(size_t size, size_t alignment) {
	void *p = vgl_memalign(alignment, size, VGL_MEM_VRAM);
	if (!p)
		return 0;
	vgl_free(p);
	return 1;
}

typedef struct {
	void *tex;
	uint32_t last_use;
	size_t bytes;
	int score;
} candidate;

// The current room, and the frame it started in. The runner switches rooms inside Process, so the new room's first
// page loads come before katana_after_swap: every caller syncs here.
static uint32_t room_start;
static void sync_room(void) {
	int r = current_room ? *current_room : -1;
	if (r == room)
		return;
	// Pages drawn in most rooms (the UI and glyph pages) are needed again in the next one: score each page by the rooms
	// it was drawn in lately (+1 for a room, -1 for a room without it), and unload low scores first
	for (void *t = *texture_first; t && room != -2; t = (void *)TEX_U32(t, T_NEXT)) {
		unsigned w, h;
		uint32_t name = TEX_U32(t, T_GLID), last;
		if (name == NOT_LOADED || !wad_page(t, &w, &h) || !vgl_tex_usage(name, &last))
			continue;
		int k = track_find(t, 1);
		if (last != OBJ_NOT_USED && (int32_t)(last - room_start) >= 0) {
			if (track[k].rooms < ROOMS_SCORE_MAX)
				track[k].rooms++;
		} else if (track[k].rooms)
			track[k].rooms--;
	}
	room = r;
	room_start = vgl_framecount;
	room_reloads = room_off = room_noroom_logged = 0;
}

// The pages that may be unloaded now (see the top of the file): lowest room score first, then oldest first, except
// that the first page at least `size` bytes large with the lowest score comes first (its own block fits the request)
static int gather(candidate *c, size_t size) {
	uint32_t now = vgl_framecount;
	sync_room();
	int n = 0;
	void *pending = last_texture ? *last_texture : NULL;
	for (void *t = *texture_first; t && n < MAX_CANDIDATES; t = (void *)TEX_U32(t, T_NEXT)) {
		unsigned w, h;
		uint32_t name = TEX_U32(t, T_GLID), last;
		if (name == NOT_LOADED || (TEX_U32(t, T_FLAGS) & 0x100) || t == pending || !wad_page(t, &w, &h))
			continue;
		void *data = vgl_tex_usage(name, &last);
		if (!data || !in_vram(data))
			continue;
		int k = track_find(t, 0);
		uint32_t loaded = k >= 0 ? track[k].loaded : 0;
		// Loaded in this room and never drawn: the game prefetched it for later in the room (obj_texture_page_master)
		if (last == OBJ_NOT_USED && k >= 0 && track[k].room == room)
			continue;
		uint32_t use = last == OBJ_NOT_USED ? loaded : (int32_t)(last - loaded) > 0 ? last : loaded;
		// Not used in this room, or not for EVICT_MIN_AGE frames
		if ((int32_t)(use - room_start) >= 0 && now - use < EVICT_MIN_AGE)
			continue;
		// The pause skips the room's tile layers (pause_bg.c): their pages stay as if drawn
		if (pause_tiles_protects(use))
			continue;
		// Never a page the game asked to have resident (texture_prefetch, recorded by page_restore.c), in any room: level
		// select asks for the UI and button glyph pages, which levels draw later (factory_0 needs its glyph page
		// mid-walk; unloading it at the room start brought a spill and a full flush mid-room). When those are all that
		// is left, the room's pages don't fit together: the page spills and the recovery flush runs at the room start,
		// as before, with page_restore.c loading the requested pages back.
		if (page_restore_requested(t))
			continue;
		c[n].tex = t;
		c[n].last_use = use;
		c[n].score = k >= 0 ? track[k].rooms : 0;
		c[n].bytes = (size_t)((w + 7) & ~7u) * h * 4;
		n++;
	}
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && (c[j].score < c[j - 1].score ||
				(c[j].score == c[j - 1].score && (int32_t)(c[j].last_use - c[j - 1].last_use) < 0)); j--) {
			candidate x = c[j];
			c[j] = c[j - 1];
			c[j - 1] = x;
		}
	for (int i = 0; i < n; i++)
		if (c[i].bytes >= size && c[i].score <= c[0].score) {
			candidate x = c[i];
			memmove(&c[1], &c[0], i * sizeof(candidate));
			c[0] = x;
			break;
		}
	return n;
}

// Unloads candidates until `size` bytes fit in VRAM; returns how many were unloaded
static int evict_until_fits(size_t size, size_t alignment, const char *where) {
	uint32_t t0 = sceKernelGetProcessTimeLow(), now = vgl_framecount;
	candidate c[MAX_CANDIDATES];
	size_t free_before = vglMemFree(VGL_MEM_VRAM);
	int n = gather(c, size), evicted = 0, ok = 0;
	size_t bytes = 0;
	uint32_t oldest = n ? now - c[0].last_use : 0;
	char pages[96];
	int plen = 0;
	pages[0] = 0;
	for (int i = 0; i < n && !ok; i++) {
		// A page drawn in the last few frames: vitaGL frees its storage only once the GPU is done with it, which this
		// makes true now (GPU finish of the submitted scenes, once per frame)
		if (!vgl_tex_idle(TEX_U32(c[i].tex, T_GLID)))
			continue;
		if (plen < (int)sizeof(pages) - 8)
			plen += snprintf(pages + plen, sizeof(pages) - plen, "%s%d", evicted ? "," : "", page_index(c[i].tex));
		flush_texture(c[i].tex);
		int k = track_find(c[i].tex, 1);
		track[k].evicted = now ? now : 1;
		evicted++;
		bytes += c[i].bytes;
		ok = fits(size, alignment);
	}
	if (!evicted) {
		page_evict_misses++;
		if (!room_noroom_logged++)
			debugPrintf("[PAGEEVICT] %uKB didn't fit in VRAM (free %uKB, %s): no page unused for %d frames; room %d\n",
				(unsigned)(size / 1024), (unsigned)(free_before / 1024), where, EVICT_MIN_AGE, room);
		return 0;
	}
	page_evict_events++;
	page_evict_pages += evicted;
	page_evict_bytes += bytes;
	debugPrintf("[PAGEEVICT] %uKB didn't fit in VRAM (free %uKB, %s): unloaded %d of %d unused pages (%s; %uKB, oldest "
		"unused %u frames) in %.2f ms, %s; free vram %uKB | room %d\n", (unsigned)(size / 1024),
		(unsigned)(free_before / 1024), where, evicted, n, pages, (unsigned)(bytes / 1024), (unsigned)oldest,
		(sceKernelGetProcessTimeLow() - t0) / 1000.0f, ok ? "fits now" : "still doesn't fit", vglMemFree(VGL_MEM_VRAM) / 1024,
		room);
	return evicted;
}

static int evict_cb(size_t size, size_t alignment) {
	int evicted = 0;
	if (enabled && !busy && !room_off && sceKernelGetThreadId() == game_thread) {
		busy = 1;
		evicted = evict_until_fits(size, alignment, "mid-frame");
		busy = 0;
	}
	// Still no room in VRAM: a held heap block for it if it would otherwise have to come from the newlib heap
	// (page_reserve.c)
	if (!evicted || !fits(size, alignment)) {
		extern void page_reserve_release(size_t size, size_t alignment);
		page_reserve_release(size, alignment);
	}
	return evicted;
}

// katana_after_swap, when texture data went outside VRAM: whether any of it is a texture page (0 when eviction is off,
// so the recovery flush runs as before). Other data outside VRAM (a surface) gets no flush: a flush leaves it where it
// is and only reloads every page.
int page_evict_page_outside_vram(void) {
	if (!enabled)
		return 1;
	for (void *t = *texture_first; t; t = (void *)TEX_U32(t, T_NEXT)) {
		unsigned w, h;
		uint32_t name = TEX_U32(t, T_GLID), last;
		if (name == NOT_LOADED || !wad_page(t, &w, &h))
			continue;
		void *data = vgl_tex_usage(name, &last);
		if (data && !in_vram(data))
			return 1;
	}
	return 0;
}

// Between frames (page_reserve.c): texture pages whose storage is in the newlib heap
int page_evict_pages_in_heap(void) {
	if (!orig_load)
		return 0;
	extern int newlib_heap_contains(const void *p);
	int n = 0;
	for (void *t = *texture_first; t; t = (void *)TEX_U32(t, T_NEXT)) {
		unsigned w, h;
		uint32_t name = TEX_U32(t, T_GLID), last;
		if (name == NOT_LOADED || !wad_page(t, &w, &h))
			continue;
		void *data = vgl_tex_usage(name, &last);
		n += data && newlib_heap_contains(data);
	}
	return n;
}

static int load_hook(void *tex, int mip) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	size_t spill = vgl_gpu_fallback_bytes;
	extern int vgl_failed_allocs;
	int failed = vgl_failed_allocs;
	int r = orig_load(tex, mip);
	uint32_t now = vgl_framecount;
	sync_room();
	int k = track_find(tex, 1);
	track[k].loaded = now;
	track[k].room = room;
	const char *again = "";
	if (track[k].evicted) {
		if (now - track[k].evicted < EVICT_THRASH_FRAMES) {
			page_evict_reloads++;
			again = " (unloaded here recently)";
			if (++room_reloads >= EVICT_THRASH_RELOADS && !room_off) {
				room_off = 1;
				debugPrintf("[PAGEEVICT] room %d reloaded %d pages within %d frames of unloading them: no more eviction "
					"in this room\n", room, room_reloads, EVICT_THRASH_FRAMES);
			}
		}
		track[k].evicted = 0;
	}
	if (debugMode) {
		unsigned w = 0, h = 0;
		wad_page(tex, &w, &h);
		debugPrintf("[TEXLOAD] page %d %ux%u in %.1f ms%s%s | free vram %uKB\n", page_index(tex), w, h,
			(sceKernelGetProcessTimeLow() - t0) / 1000.0f, vgl_failed_allocs != failed ? " NOT ALLOCATED (draws blank)" :
			vgl_gpu_fallback_bytes != spill ? " OUTSIDE VRAM" : "", again,
			vglMemFree(VGL_MEM_VRAM) / 1024);
	}
	return r;
}

// katana_after_swap, on the game thread: room changes
void page_evict_frame(void) {
	game_thread = sceKernelGetThreadId();
	sync_room();
}

void page_evict_install(void) {
	// The callback also releases page_reserve.c's heap blocks, with or without eviction
	vgl_vram_evict_cb = evict_cb;
	if (exp_flag("pageevict0")) {
		debugPrintf("texture page eviction: off (exp_pageevict0)\n");
		return;
	}
	flush_texture = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics12FlushTextureEPv");
	texture_first = (void **)so_symbol(&yoyoloader_mod, "_ZN7Texture9ms_pFirstE");
	last_texture = (void **)so_symbol(&yoyoloader_mod, "g_pLastTexture");
	wad_base = (uint8_t **)so_symbol(&yoyoloader_mod, "g_pWADBaseAddress");
	wad_end = (uint8_t **)so_symbol(&yoyoloader_mod, "g_pWADEndAddress");
	uintptr_t load = so_symbol(&yoyoloader_mod, "_Z19LoadTextureFromQOIFP7Texture10eMipEnable");
	uintptr_t flush_all = so_symbol(&yoyoloader_mod, "_ZN8Graphics12TextureFlushEb");
	uintptr_t flush = (uintptr_t)flush_texture;
	// The Texture offsets above, as the 1.1.10 runner's own code uses them (see title_pages.c), and FlushTexture's
	// GL name handling: ldr r2, [r1, #0x1c]! ... str r1(-1), [r4, #0x1c]
	int layout = load && flush_all && flush && last_texture &&
		*(uint32_t *)(load + 0x38) == 0xe1c486d0 && *(uint32_t *)(load + 0x34) == 0xe5940018 &&
		*(uint32_t *)(load + 0xdc) == 0xe5841018 && *(uint32_t *)(load + 0x120) == 0xe584001c &&
		*(uint32_t *)(flush_all + 0x24) == 0xe5955068 && *(uint32_t *)(flush_all + 0x38) == 0xe5d50019 &&
		*(uint32_t *)(flush + 0x50) == 0xe5b1201c && *(uint32_t *)(flush + 0x84) == 0xe584101c;
	if (!layout || !texture_first || !wad_base || !wad_end) {
		debugPrintf("texture page eviction: NOT installed (unknown runner)\n");
		return;
	}
	orig_load = entry_hook(load, load_hook);
	if (!orig_load) {
		debugPrintf("texture page eviction: NOT installed (load hook)\n");
		return;
	}
	enabled = 1;
	debugPrintf("texture page eviction: on (pages unused for %d frames)\n", EVICT_MIN_AGE);
}
