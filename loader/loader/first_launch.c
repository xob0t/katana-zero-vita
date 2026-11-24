/*
 * first_launch.c: status screen for the first launch.
 *
 * On a first launch the screen stayed black for about two minutes before anything was drawn. That time is spent in
 * vglInit: with HAVE_SHADER_CACHE, vitaGL creates 512 shader cache folders (v/00-FF and f/00-FF under
 * ux0:data/shader_cache/<title id>/v<SHADER_CACHE_MAGIC>), and each sceIoMkdir on the memory card takes about
 * 0.24 s. It skips the loop when v/00 already exists, so later launches spend about 1.4 s in vglInit.
 *
 * first_launch_prepare() runs just before vglInit. If v/00 is missing it creates the same folders itself while
 * showing a status line and a progress bar from a CPU-drawn framebuffer, so vglInit then finds v/00 and skips the
 * loop. The framebuffer is released before returning: vglInit sizes its pools from all free CDRAM, so holding it
 * through vglInit would take 2 MB of VRAM from the game. The display goes black until the loader's splash.
 *
 * v/00 is created last, and only when every other folder exists, so a first launch closed partway through (or one
 * where a folder couldn't be created) resumes on the next launch instead of leaving vitaGL with missing folders
 * (vitaGL's own loop creates v/00 first and never retries). If a folder fails, vglInit runs its own loop as before.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <string.h>
#include "main.h"

extern int debugMode;

// font_utils.h defines a non-static font_size, which profiler.c also defines
#define font_size first_launch_font_size
#include "font_utils.h"
#undef font_size

// vitaGL source/shared.h SHADER_CACHE_MAGIC (vitaGL 464876a). If vitaGL raises it, these folders go unused and
// vitaGL's own loop creates the new ones under a black screen again: no harm beyond the lost status screen.
#define VGL_SHADER_CACHE_MAGIC 2

#define FL_W 960
#define FL_H 544
#define FL_FB_SIZE (FL_W * FL_H * 4)
#define FL_FB_ALLOC ((FL_FB_SIZE + 256 * 1024 - 1) & ~(256 * 1024 - 1))

// A8B8G8R8 as little-endian words: 0xAABBGGRR. The background is assets/splash.png's single colour.
#define FL_BG 0xFF0C0C0C
#define FL_TEXT 0xFFFFFFFF
#define FL_DIM 0xFF9A9A9A
#define FL_BAR_EDGE 0xFF5A5A5A
#define FL_BAR_FILL 0xFFE6E6E6

#define FL_BAR_W 600
#define FL_BAR_H 12
#define FL_BAR_X ((FL_W - FL_BAR_W) / 2)
#define FL_BAR_Y 318

#define FL_EEXIST 0x80010011 // SCE_ERROR_ERRNO_EEXIST

static uint32_t *fl_fb;

static void fl_rect(int x, int y, int w, int h, uint32_t color) {
	for (int yy = y; yy < y + h; yy++) {
		uint32_t *row = fl_fb + yy * FL_W;
		for (int xx = x; xx < x + w; xx++)
			row[xx] = color;
	}
}

static void fl_text(int y, int scale, uint32_t color, const char *s) {
	int x = (FL_W - (int)strlen(s) * FONT_WIDTH * scale) / 2;
	for (; *s; s++, x += FONT_WIDTH * scale) {
		const unsigned char *glyph = &font[(unsigned char)*s * FONT_HEIGHT];
		for (int gy = 0; gy < FONT_HEIGHT; gy++)
			for (int gx = 0; gx < FONT_WIDTH; gx++)
				if ((glyph[gy] >> (7 - gx)) & 1)
					fl_rect(x + gx * scale, y + gy * scale, scale, scale, color);
	}
}

static void fl_progress(int done, int total) {
	int w = (FL_BAR_W - 4) * done / total;
	fl_rect(FL_BAR_X + 2, FL_BAR_Y + 2, w, FL_BAR_H - 4, FL_BAR_FILL);
}

static SceUID fl_show(void) {
	SceUID block = sceKernelAllocMemBlock("first_launch_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FL_FB_ALLOC, NULL);
	if (block < 0)
		return block;
	if (sceKernelGetMemBlockBase(block, (void **)&fl_fb) < 0 || !fl_fb) {
		sceKernelFreeMemBlock(block);
		fl_fb = NULL;
		return -1;
	}

	fl_rect(0, 0, FL_W, FL_H, FL_BG);
	fl_text(222, 2, FL_TEXT, "First launch: preparing the graphics cache.");
	fl_text(256, 2, FL_DIM, "This takes about 3 minutes. Later launches are much faster.");
	fl_rect(FL_BAR_X, FL_BAR_Y, FL_BAR_W, FL_BAR_H, FL_BAR_EDGE);
	fl_rect(FL_BAR_X + 1, FL_BAR_Y + 1, FL_BAR_W - 2, FL_BAR_H - 2, FL_BG);

	SceDisplayFrameBuf param;
	memset(&param, 0, sizeof(param));
	param.size = sizeof(param);
	param.base = fl_fb;
	param.pitch = FL_W;
	param.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	param.width = FL_W;
	param.height = FL_H;
	if (sceDisplaySetFrameBuf(&param, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) {
		sceKernelFreeMemBlock(block);
		fl_fb = NULL;
		return -1;
	}
	return block;
}

// debugMode: answer scripts/vita_screenshot.py, whose request the loader's screenshot thread can't serve yet
static void fl_screenshot_check(void) {
	SceIoStat st;
	if (sceIoGetstat("ux0:data/gms/shared/screenshot.req", &st) < 0)
		return;
	sceIoRemove("ux0:data/gms/shared/screenshot.req");
	SceUID fd = sceIoOpen("ux0:data/gms/shared/screenshot.tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return;
	uint32_t header[3] = {FL_W, FL_H, FL_W};
	int ok = sceIoWrite(fd, header, sizeof(header)) == (int)sizeof(header);
	ok = ok && sceIoWrite(fd, fl_fb, FL_FB_SIZE) == FL_FB_SIZE;
	ok = sceIoClose(fd) >= 0 && ok;
	if (!ok) { // never publish a truncated image
		sceIoRemove("ux0:data/gms/shared/screenshot.tmp");
		return;
	}
	sceIoRemove("ux0:data/gms/shared/screenshot.raw");
	sceIoRename("ux0:data/gms/shared/screenshot.tmp", "ux0:data/gms/shared/screenshot.raw");
}

static void fl_hide(SceUID block) {
	// Black out the display and let it finish scanning the buffer before freeing it. If the display can't be
	// blanked, keep the buffer: 2 MB less VRAM is better than vitaGL reusing memory the display still scans.
	if (sceDisplaySetFrameBuf(NULL, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || sceDisplayWaitVblankStart() < 0 ||
		sceDisplayWaitVblankStart() < 0) {
		debugPrintf("First launch: display not blanked, status buffer kept\n");
		fl_fb = NULL;
		return;
	}
	sceKernelFreeMemBlock(block);
	fl_fb = NULL;
}

void first_launch_prepare(void) {
	// Same title id and paths as vglInitWithCustomSizes (the loader doesn't call vglSetShaderCachePath)
	char titleid[12] = "";
	if (sceAppMgrAppParamGetString(0, 12, titleid, sizeof(titleid)) < 0 || !titleid[0])
		return; // vglInit makes the folders itself
	titleid[sizeof(titleid) - 1] = 0;
	char root[128], path[160];
	snprintf(root, sizeof(root), "ux0:data/shader_cache/%s/v%d", titleid, VGL_SHADER_CACHE_MAGIC);
	snprintf(path, sizeof(path), "%s/v/00", root);
	SceIoStat st;
	if (sceIoGetstat(path, &st) >= 0 && SCE_S_ISDIR(st.st_mode))
		return;

	uint64_t t0 = sceKernelGetProcessTimeWide();
	SceUID block = fl_show();
	debugPrintf("First launch: %s missing, creating the shader cache folders (status screen %s)\n", path,
		block >= 0 ? "shown" : "unavailable");

	sceIoMkdir("ux0:data/shader_cache", 0777);
	snprintf(path, sizeof(path), "ux0:data/shader_cache/%s", titleid);
	sceIoMkdir(path, 0777);
	sceIoMkdir(root, 0777);
	int failed = 0;
	for (int i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/%c", root, i ? 'f' : 'v');
		int r = sceIoMkdir(path, 0777);
		if (r < 0 && r != FL_EEXIST) {
			// The cache can't be made here (full card, a file in the way): leave it to vglInit, which gives up
			// after one failed folder, instead of trying 512 times on every launch
			debugPrintf("First launch: can't create %s (0x%08X), leaving the cache to vitaGL\n", path, r);
			if (block >= 0)
				fl_hide(block);
			return;
		}
	}

	// v/01-FF, f/00-FF, then v/00 last (the "cache exists" marker for vitaGL and for the check above)
	const int total = 512;
	uint64_t last_tick = 0;
	for (int i = 0; i < total; i++) {
		char dir = 'v';
		int sub = 0;
		if (i < 255)
			sub = i + 1;
		else if (i < 511) {
			dir = 'f';
			sub = i - 255;
		}
		if (i == total - 1 && failed) // v/00 would mark the incomplete cache as ready
			break;
		snprintf(path, sizeof(path), "%s/%c/%02X", root, dir, sub);
		int r = sceIoMkdir(path, 0777);
		if (r < 0 && r != FL_EEXIST && failed++ == 0)
			debugPrintf("First launch: can't create %s (0x%08X)\n", path, r);
		if (fl_fb)
			fl_progress(i + 1, total);
		uint64_t now = sceKernelGetProcessTimeWide();
		if (now - last_tick > 1000000) {
			// No input arrives during this, and a static screen would dim
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
			last_tick = now;
			if (debugMode && fl_fb)
				fl_screenshot_check();
		}
	}

	if (block >= 0)
		fl_hide(block);
	if (failed)
		debugPrintf("First launch: %d folders failed in %.1f s; v/00 not created, vglInit retries them\n", failed,
			(sceKernelGetProcessTimeWide() - t0) / 1000000.0);
	else
		debugPrintf("First launch: shader cache folders created in %.1f s\n",
			(sceKernelGetProcessTimeWide() - t0) / 1000000.0);
}
