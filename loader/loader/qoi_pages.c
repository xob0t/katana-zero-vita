/*
 * qoi_pages.c
 *
 * Texture page loads, and the title screen's pages.
 *
 * The runner loads a 2048x2048 texture page (16 MB) the first time something draws from it, on the game thread:
 * LoadTextureFromQOIF reads it (ReadQOIFFile: LZ4, see katana.c, then QOI into a 16 MB buffer), uploads it
 * (glTexImage2D) and frees the buffer. One page costs 180-340 ms (read 110-265 ms of which LZ4 11-58 ms, upload
 * 58-84 ms). load_qoif below replaces LoadTextureFromQOIF with the same steps.
 *
 * room_title draws from five pages (TXTR 23, 21, 33, 19, and 45 once the menu shows), 80 MB of the 96.6 MB of VRAM.
 * Entering it from a level, VRAM is full of the level's pages: the first pages land outside VRAM, the loader's spill
 * flush (katana_display.c) empties VRAM after that frame and every page loads again, one frame stalling ~1 s and
 * the next seconds at 13-30 fps (1.4-1.9 s of stalls in the first 5 s on every title entry in the playtest logs).
 * At boot the save/options prompts leave pages 19, 33 and 45 plus two half pages resident; page 21 spills, and the
 * flush reloads 19, 33, 21, 23 in one 1 s frame.
 *
 * So when room_title starts (StartRoom entry, before its instances are created):
 * - Texture pages that aren't the title's are flushed (Graphics::FlushTexture, what draw_texture_flush does per
 *   page) and freed at once (GPU finish + garbage collection). Only when no GPU scene was begun since the last
 *   swap, so nothing recorded but unsubmitted can refer to them (the condition the after-swap flush relies on).
 * - The title's pages that aren't loaded are decoded straight into VRAM by two worker threads (and the game thread
 *   when it gets to a page no worker has started): LZ4, then QOI with gm_qoi.h, bit-identical to the runner's
 *   ReadQOIFFile on all 67 pages (scripts/gm_qoi_check.py, Unicorn), with no 16 MB staging buffer or copy.
 * - When the runner then loads one of those pages, load_qoif waits for its decode if needed and hands the decoded
 *   VRAM to a new GL texture instead of reading the page. Everything else about the load is unchanged.
 * Pages are told apart by their TXTR index, so the title's set holds whatever the Vita glyph option did to page 45.
 * Pages the title loads that aren't in the set are added to it for the next visit. A page is only decoded ahead if
 * it fits in VRAM with TITLE_VRAM_RESERVE to spare; decodes nobody claimed are freed at the next room start.
 * exp_titlepages0 disables all of it (loads are then the runner's own steps, as before). exp_titlepagesverify (debug)
 * compares every page decoded ahead with the runner's ReadQOIFFile of it.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <kubridge.h>
#include <psp2/kernel/cpu.h>
#include <string.h>
#include <stdlib.h>

#include "main.h"
#include "so_util.h"
#include "lz4/lz4.h"
#include "gm_qoi.h"

#include "katana_internal.h"

// Texture (APK 1.1.10 runner): +0 state, +4 width, +8 height, +0x18 flags, +0x1c GL name (-1 when not loaded),
// +0x60 file data, +0x64 size, +0x68 next in Texture::ms_pFirst's list. Flags 0x100: not flushable (surfaces).
enum { T_STATE = 0x00, T_W = 0x04, T_H = 0x08, T_FLAGS = 0x18, T_GLID = 0x1c, T_DATA = 0x60, T_SIZE = 0x64, T_NEXT = 0x68 };
#define TEX_U32(t, off) (*(uint32_t *)((uint8_t *)(t) + (off)))

static void *(*read_qoif)(void *data, int size, int *w, int *h, int premultiply);
static void (*free_qoif)(void *buf);
static void (*invalidate_texture_state)(void);
static void (*generate_mips)(void *tex, int w, int h, int bpp, void *data);
static void (*flush_texture)(void *tex);
static void (**gl_gen_textures)(GLsizei n, GLuint *names);
static void (**gl_bind_texture)(GLenum target, GLuint name);
static void (**gl_tex_image_2d)(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d);
static void **texture_first;
static uint8_t **wad_base, **wad_end;

extern uint32_t lz4_page_us, lz4_page_in, lz4_page_out;
extern unsigned vgl_gpu_fallback_allocs;
extern uint32_t vgl_scene_count;
extern void *vgl_memalign(size_t alignment, size_t size, vglMemType type);
extern void vgl_free(void *ptr);
extern void vgl_collect_garbage_now(void);

#define MAX_PAGES 128
#define MAX_JOBS 8
#define TITLE_VRAM_RESERVE (8 * 1024 * 1024)
#define WORKERS 2

enum { JOB_FREE, JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED };
typedef struct {
	volatile int state;
	void *tex;
	int page;
	const uint8_t *lz4;
	uint32_t lz4_len, qoi_len;
	uint8_t *scratch;
	uint32_t *vram;
	int w, h;
	uint32_t decode_us;
	int core;
	SceUID done; // signalled once per dispatched job, waited for once (claim or cleanup)
} page_job;
static page_job jobs[MAX_JOBS];
static SceUID work_sema = -1;

// The title's pages in Katana ZERO 1.1.10's game.droid, in the order it first draws from them (measured on the Vita)
static const int title_pages[] = { 23, 21, 33, 19, 45 };
static int title_room = -2;
static uint8_t title_set[MAX_PAGES]; // TXTR indices the title draws from
static uint32_t scene_mark = 0xffffffff; // vgl_scene_count right after the last real swap
static int verify; // exp_titlepagesverify (debug): compare each page decoded ahead with the runner's decode

// The pages' TXTR index, from the runner's in-memory game.droid (offsets are file offsets)
static int page_count;
static const uint8_t *page_blob[MAX_PAGES];

static uint32_t rd32(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static void index_pages(void) {
	page_count = 0;
	const uint8_t *base = wad_base ? *wad_base : NULL, *end = wad_end ? *wad_end : NULL;
	if (!base || !end || end <= base || memcmp(base, "FORM", 4))
		return;
	for (const uint8_t *c = base + 8; c + 8 <= end; c += 8 + rd32(c + 4)) {
		if (memcmp(c, "TXTR", 4))
			continue;
		uint32_t n = rd32(c + 8);
		if (n > MAX_PAGES)
			return;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t e = rd32(c + 12 + 4 * i);
			if (e + 28 > (uint32_t)(end - base))
				return;
			uint32_t off = rd32(base + e + 24);
			page_blob[i] = off < (uint32_t)(end - base) ? base + off : NULL;
		}
		page_count = n;
		return;
	}
}

static int page_of(void *tex) {
	const uint8_t *data = (const uint8_t *)TEX_U32(tex, T_DATA);
	for (int i = 0; i < page_count; i++)
		if (page_blob[i] == data)
			return i;
	return -1;
}

// An LZ4 "2zoq" page: "2zoq", u16 width, u16 height, u32 QOI size, "LZ4H", u32 LZ4 size, LZ4 block
static int lz4_page(void *tex, int *w, int *h, uint32_t *qoi_len, uint32_t *lz4_len) {
	const uint8_t *b = (const uint8_t *)TEX_U32(tex, T_DATA);
	uint32_t size = TEX_U32(tex, T_SIZE);
	if (!b || size < 20 || memcmp(b, "2zoq", 4) || memcmp(b + 12, "LZ4H", 4))
		return 0;
	*w = b[4] | b[5] << 8;
	*h = b[6] | b[7] << 8;
	*qoi_len = rd32(b + 8);
	*lz4_len = rd32(b + 16);
	return *w && *h && *lz4_len <= size - 20 && *qoi_len >= 12;
}

static void run_job(page_job *j) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	int ok = LZ4_decompress_safe((const char *)j->lz4, (char *)j->scratch, j->lz4_len, j->qoi_len) == (int)j->qoi_len &&
		!memcmp(j->scratch, "fioq", 4) && (j->scratch[4] | j->scratch[5] << 8) == j->w &&
		(j->scratch[6] | j->scratch[7] << 8) == j->h;
	if (ok) {
		uint32_t len = rd32(j->scratch + 8);
		ok = len <= j->qoi_len - 12 &&
			gm_qoi_decode(j->scratch + 12, j->scratch + 12 + len, j->vram, (uint32_t)j->w * j->h) == 0;
	}
	free(j->scratch);
	j->scratch = NULL;
	j->decode_us = sceKernelGetProcessTimeLow() - t0;
	j->core = sceKernelGetCpuId();
	__sync_synchronize();
	j->state = ok ? JOB_DONE : JOB_FAILED;
	sceKernelSignalSema(j->done, 1);
}

static int worker(SceSize args, void *argp) {
	for (;;) {
		sceKernelWaitSema(work_sema, 1, NULL);
		for (int i = 0; i < MAX_JOBS; i++)
			if (__sync_bool_compare_and_swap(&jobs[i].state, JOB_QUEUED, JOB_RUNNING)) {
				run_job(&jobs[i]);
				break;
			}
	}
	return 0;
}

static int start_workers(void) {
	if (work_sema >= 0)
		return 1;
	for (int i = 0; i < MAX_JOBS; i++)
		if ((jobs[i].done = sceKernelCreateSema("page done", 0, 0, 1, NULL)) < 0)
			return 0;
	work_sema = sceKernelCreateSema("page work", 0, 0, MAX_JOBS, NULL);
	if (work_sema < 0)
		return 0;
	extern int worker_thread_cpu_mask(void);
	int mask = worker_thread_cpu_mask(); // the cores the game thread doesn't run on
	for (int i = 0; i < WORKERS; i++) {
		SceUID th = sceKernelCreateThread("page decoder", worker, 0x10000100, 0x4000, 0, mask, NULL);
		if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
			return i > 0; // the game thread runs whatever the workers don't
	}
	return 1;
}

// Waits for (or runs) a dispatched job; afterwards the job's result can be read and the job freed
static uint32_t finish_job(page_job *j) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	if (__sync_bool_compare_and_swap(&j->state, JOB_QUEUED, JOB_RUNNING))
		run_job(j);
	sceKernelWaitSema(j->done, 1, NULL);
	return sceKernelGetProcessTimeLow() - t0;
}

static void drop_job(page_job *j) {
	finish_job(j);
	vgl_free(j->vram); // never handed to the GPU
	j->vram = NULL;
	j->tex = NULL;
	j->state = JOB_FREE;
}

static page_job *job_for(void *tex) {
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].state != JOB_FREE && jobs[i].tex == tex)
			return &jobs[i];
	return NULL;
}

// A GL texture whose storage is the decoded page: made 8x8 by vitaGL (format, sampler state), then pointed at it
static GLuint texture_from_vram(page_job *j) {
	GLuint id;
	(*gl_gen_textures)(1, &id);
	(*gl_bind_texture)(GL_TEXTURE_2D, id);
	(*gl_tex_image_2d)(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	SceGxmTexture *gt = vglGetGxmTexture(GL_TEXTURE_2D);
	void *small = vglGetTexDataPointer(GL_TEXTURE_2D);
	if (!gt || sceGxmTextureSetWidth(gt, j->w) < 0 || sceGxmTextureSetHeight(gt, j->h) < 0 ||
		sceGxmTextureSetData(gt, j->vram) < 0) {
		// Fall back to a normal upload from the decoded pixels
		(*gl_tex_image_2d)(GL_TEXTURE_2D, 0, GL_RGBA, j->w, j->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, j->vram);
		vgl_free(j->vram);
	} else {
		vglOverloadTexDataPointer(GL_TEXTURE_2D, j->vram);
		if (small)
			vgl_free(small); // never drawn
	}
	j->vram = NULL;
	return id;
}

static int load_qoif(void *tex, int mip) {
	uint32_t flags = TEX_U32(tex, T_FLAGS);
	page_job *j = job_for(tex);
	if (j) {
		uint32_t wait = finish_job(j);
		if (j->state == JOB_DONE) {
			invalidate_texture_state();
			TEX_U32(tex, T_W) = j->w;
			TEX_U32(tex, T_H) = j->h;
			GLuint id = texture_from_vram(j);
			TEX_U32(tex, T_FLAGS) = flags | 0x40;
			if (mip == 1 || (mip == 2 && (flags & 0x10))) {
				void *data = vglGetTexDataPointer(GL_TEXTURE_2D);
				generate_mips(tex, j->w, j->h, 4, data);
			}
			TEX_U32(tex, T_STATE) = 6;
			TEX_U32(tex, T_GLID) = id;
			if (verify) { // the runner's own decode of the page must match what the GPU now samples
				int vw, vh;
				void *ref = read_qoif((void *)TEX_U32(tex, T_DATA), TEX_U32(tex, T_SIZE), &vw, &vh, !(flags & 2));
				int same = ref && vw == j->w && vh == j->h && !memcmp(ref, vglGetTexDataPointer(GL_TEXTURE_2D), (size_t)vw * vh * 4);
				if (ref)
					free_qoif(ref);
				debugPrintf("[TITLEPAGES] verify page %d: %s\n", j->page, same ? "identical to ReadQOIFFile" : "MISMATCH");
			}
			if (debugMode)
				debugPrintf("[TITLEPAGES] page %d ready: waited %.1fms, decoded in %.1fms on core %d\n", j->page,
					wait / 1000.0f, j->decode_us / 1000.0f, j->core);
			j->tex = NULL;
			j->state = JOB_FREE;
			return id;
		}
		debugPrintf("[TITLEPAGES] page %d failed to decode ahead; loading it normally\n", j->page);
		drop_job(j);
	}

	uint32_t t0 = sceKernelGetProcessTimeLow(), lz4_before = lz4_page_us;
	int w, h;
	void *buf = read_qoif((void *)TEX_U32(tex, T_DATA), TEX_U32(tex, T_SIZE), &w, &h, !(flags & 2));
	if (!buf) {
		debugPrintf("[TEXLOAD] page failed to read\n");
		return 0;
	}
	uint32_t t1 = sceKernelGetProcessTimeLow();
	invalidate_texture_state();
	TEX_U32(tex, T_W) = w;
	TEX_U32(tex, T_H) = h;
	GLuint id;
	(*gl_gen_textures)(1, &id);
	(*gl_bind_texture)(GL_TEXTURE_2D, id);
	unsigned spills = vgl_gpu_fallback_allocs;
	(*gl_tex_image_2d)(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	uint32_t t2 = sceKernelGetProcessTimeLow();
	flags = TEX_U32(tex, T_FLAGS);
	TEX_U32(tex, T_FLAGS) = flags | 0x40;
	if (mip == 1 || (mip == 2 && (flags & 0x10)))
		generate_mips(tex, w, h, 4, buf);
	free_qoif(buf);
	TEX_U32(tex, T_STATE) = 6;
	TEX_U32(tex, T_GLID) = id;
	int room = current_room ? *current_room : -1;
	int page = -1;
	if (room == title_room && w * h >= 1024 * 1024 && (page = page_of(tex)) >= 0 && !title_set[page]) {
		title_set[page] = 1;
		debugPrintf("[TITLEPAGES] page %d added to the title's set\n", page);
	}
	if (debugMode && w * h >= 512 * 512)
		debugPrintf("[TEXLOAD] room %d page %d %dx%d lz4 %u->%u mip %d: read %.1fms (lz4 %.1fms) upload %.1fms%s; vram %uKB\n",
			room, page >= 0 ? page : page_of(tex), w, h, lz4_page_in, lz4_page_out, mip, (t1 - t0) / 1000.0f,
			(lz4_page_us - lz4_before) / 1000.0f, (t2 - t1) / 1000.0f, vgl_gpu_fallback_allocs != spills ? " OUTSIDE VRAM" : "",
			vglMemFree(VGL_MEM_VRAM) / 1024);
	return id;
}

// Called right after each real vglSwapBuffers (katana_after_swap)
void qoi_pages_after_swap(void) {
	scene_mark = vgl_scene_count;
}

static void title_start(void) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	index_pages();
	if (!page_count)
		return;
	// Evict the pages the title doesn't draw from; free them now if no scene is open
	int evicted = 0;
	uint32_t evicted_kb = 0, gc_us = 0;
	int scene_free = vgl_scene_count == scene_mark;
	if (scene_free && texture_first) {
		void *next;
		for (void *t = *texture_first; t; t = next) {
			next = (void *)TEX_U32(t, T_NEXT);
			int w, h;
			uint32_t qoi_len, lz4_len;
			if (TEX_U32(t, T_GLID) == 0xffffffff || (TEX_U32(t, T_FLAGS) & 0x100) || !lz4_page(t, &w, &h, &qoi_len, &lz4_len) ||
				w * h < 1024 * 1024)
				continue;
			int page = page_of(t);
			if (page < 0 || title_set[page])
				continue;
			flush_texture(t);
			evicted++;
			evicted_kb += w * h / 256;
		}
		if (evicted) {
			uint32_t g0 = sceKernelGetProcessTimeLow();
			vgl_collect_garbage_now();
			gc_us = sceKernelGetProcessTimeLow() - g0;
		}
	}
	// Decode the title's missing pages ahead
	int queued = 0;
	if (start_workers()) {
		// In the order the title first draws from them, then pages learned on earlier visits
		int order[MAX_PAGES], n = 0;
		for (unsigned i = 0; i < sizeof(title_pages) / sizeof(title_pages[0]); i++)
			if (title_pages[i] < page_count)
				order[n++] = title_pages[i];
		for (int page = 0; page < page_count; page++) {
			int known = 0;
			for (unsigned i = 0; i < sizeof(title_pages) / sizeof(title_pages[0]); i++)
				known |= title_pages[i] == page;
			if (title_set[page] && !known)
				order[n++] = page;
		}
		for (int k = 0; k < n; k++) {
			int page = order[k];
			if (!title_set[page])
				continue;
			void *tex = NULL;
			for (void *t = texture_first ? *texture_first : NULL; t; t = (void *)TEX_U32(t, T_NEXT))
				if ((const uint8_t *)TEX_U32(t, T_DATA) == page_blob[page]) {
					tex = t;
					break;
				}
			int w, h;
			uint32_t qoi_len, lz4_len;
			if (!tex || TEX_U32(tex, T_GLID) != 0xffffffff || job_for(tex) || !lz4_page(tex, &w, &h, &qoi_len, &lz4_len))
				continue;
			size_t size = (size_t)w * h * 4;
			if (vglMemFree(VGL_MEM_VRAM) < size + TITLE_VRAM_RESERVE)
				break;
			page_job *j = NULL;
			for (int i = 0; i < MAX_JOBS && !j; i++)
				if (jobs[i].state == JOB_FREE)
					j = &jobs[i];
			if (!j)
				break;
			uint8_t *scratch = malloc(qoi_len);
			void *vram = scratch ? vgl_memalign(16, size, VGL_MEM_VRAM) : NULL;
			if (!vram) {
				free(scratch);
				break;
			}
			j->tex = tex;
			j->page = page;
			j->lz4 = (const uint8_t *)TEX_U32(tex, T_DATA) + 20;
			j->lz4_len = lz4_len;
			j->qoi_len = qoi_len;
			j->scratch = scratch;
			j->vram = vram;
			j->w = w;
			j->h = h;
			__sync_synchronize();
			j->state = JOB_QUEUED;
			sceKernelSignalSema(work_sema, 1);
			queued++;
		}
	}
	if (debugMode)
		debugPrintf("[TITLEPAGES] start: %s; evicted %d pages (%uKB, gc %.1fms); decoding %d ahead; free vram %uKB (%.1fms)\n",
			scene_free ? "no open scene" : "scene open, nothing evicted", evicted, evicted_kb, gc_us / 1000.0f, queued,
			vglMemFree(VGL_MEM_VRAM) / 1024, (sceKernelGetProcessTimeLow() - t0) / 1000.0f);
}

static void (*orig_start_room)(int room, int load);
static void start_room(int room, int load) {
	// Decodes nobody claimed (the room changed first) are dropped
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].state != JOB_FREE) {
			debugPrintf("[TITLEPAGES] page %d decoded ahead but not used; dropped\n", jobs[i].page);
			drop_job(&jobs[i]);
		}
	if (title_room == -2) {
		int (*room_find)(const char *) = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
		title_room = room_find ? room_find("room_title") : -1;
	}
	if (room == title_room && room >= 0)
		title_start();
	orig_start_room(room, load);
}

void qoi_pages_install(void) {
	read_qoif = (void *)so_symbol(&yoyoloader_mod, "_Z12ReadQOIFFilePviPiS0_b");
	free_qoif = (void *)so_symbol(&yoyoloader_mod, "_Z12FreeQOIFFilePh");
	invalidate_texture_state = (void *)so_symbol(&yoyoloader_mod, "_Z23_InvalidateTextureStatev");
	generate_mips = (void *)so_symbol(&yoyoloader_mod, "_Z12GenerateMipsP7TextureiiiPc");
	flush_texture = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics12FlushTextureEPv");
	gl_gen_textures = (void *)so_symbol(&yoyoloader_mod, "FuncPtr_glGenTextures");
	gl_bind_texture = (void *)so_symbol(&yoyoloader_mod, "FuncPtr_glBindTexture");
	gl_tex_image_2d = (void *)so_symbol(&yoyoloader_mod, "FuncPtr_glTexImage2D");
	texture_first = (void **)so_symbol(&yoyoloader_mod, "_ZN7Texture9ms_pFirstE");
	wad_base = (uint8_t **)so_symbol(&yoyoloader_mod, "g_pWADBaseAddress");
	wad_end = (uint8_t **)so_symbol(&yoyoloader_mod, "g_pWADEndAddress");
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z19LoadTextureFromQOIFP7Texture10eMipEnable");
	uintptr_t tex_flush = so_symbol(&yoyoloader_mod, "_ZN8Graphics12TextureFlushEb");
	// The Texture field offsets above, as the 1.1.10 runner's own code uses them: ldrd r8, [r4, #0x60] (data, size),
	// str r1, [r4, #0x18] (flags), str r0, [r4, #0x1c] (GL name); TextureFlush: ldr r5, [r5, #0x68] (next)
	int layout = entry && tex_flush && *(uint32_t *)(entry + 0x38) == 0xe1c486d0 && *(uint32_t *)(entry + 0xdc) == 0xe5841018 &&
		*(uint32_t *)(entry + 0x120) == 0xe584001c && *(uint32_t *)(tex_flush + 0x24) == 0xe5955068;
	if (!read_qoif || !free_qoif || !invalidate_texture_state || !generate_mips || !gl_gen_textures || !gl_bind_texture ||
		!gl_tex_image_2d || !layout) {
		debugPrintf("texture page loader: unknown runner, its own kept\n");
		return;
	}
	hook_addr(entry, (uintptr_t)&load_qoif);
	kuKernelFlushCaches((void *)(entry & ~3), 16);
	debugPrintf("texture page loader: installed\n");
	verify = debugMode && exp_flag("titlepagesverify");
	if (exp_flag("titlepages0") || !flush_texture || !texture_first || !wad_base || !wad_end)
		return;
	for (unsigned i = 0; i < sizeof(title_pages) / sizeof(title_pages[0]); i++)
		title_set[title_pages[i]] = 1;
	orig_start_room = entry_hook(so_symbol(&yoyoloader_mod, "_Z9StartRoomib"), start_room);
	debugPrintf("title pages ahead: %s\n", orig_start_room ? "installed" : "NOT installed");
}
