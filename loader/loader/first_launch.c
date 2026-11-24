/*
 * first_launch.c: status screen for a launch that builds the shader cache.
 *
 * vitaGL (patches/vitagl.patch) keeps its shader cache flat: every compiled shader is one file in
 * ux0:data/shader_cache/<title id>/flat<SHADER_CACHE_MAGIC>, named by the hash of its source. Upstream vitaGL uses
 * 512 folders (v/00-FF and f/00-FF under .../v<SHADER_CACHE_MAGIC>) and created them all in vglInit; at about
 * 0.35 s per sceIoMkdir on a memory card that took 3 minutes on a first launch (PR #73 showed a progress bar for it).
 * With the flat layout vglInit makes one folder, and a first launch is mostly the runner's Startup compiling the
 * game's shaders, about 17 s more than a warm Startup.
 *
 * first_launch_prepare() runs before vglInit and notes whether the cache is unfinished: the folder or its ready
 * marker is missing. Then the loader draws first_launch_status_image() in place of the splash, a frame that stays on
 * screen through the runner's Startup (the splash is a single colour, so it's the same frame plus two lines of text).
 * first_launch_done() runs after Startup, writes the marker and logs the cache statistics. A launch closed partway
 * through keeps its compiled shaders and shows the status again next time.
 *
 * Caches from older builds (.../v2 with its 512 folders) are left alone: deleting them would cost a few minutes of
 * card time per launch. They can be deleted by hand.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"

extern int debugMode;

// font_utils.h defines a non-static font_size, which profiler.c also defines
#define font_size first_launch_font_size
#include "font_utils.h"
#undef font_size

// patches/vitagl.patch: vgl.c's cache folder name ("flat" + vitaGL source/shared.h SHADER_CACHE_MAGIC) and
// custom_shaders.c's hits, misses, lookup+load us, compile+save us
#define VGL_SHADER_CACHE_DIR "flat2"
extern uint32_t vgl_shader_cache_stats[4];

#define FL_READY_NAME "ready"

#define FL_W 960
#define FL_H 544

// A8B8G8R8 as little-endian words: 0xAABBGGRR. The background is assets/splash.png's single colour.
#define FL_BG 0xFF0C0C0C
#define FL_TEXT 0xFFFFFFFF
#define FL_DIM 0xFF9A9A9A

static char fl_ready[160];
static int fl_cold;
static uint64_t fl_t0;

static void fl_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color) {
	for (int yy = y; yy < y + h; yy++) {
		uint32_t *row = fb + yy * FL_W;
		for (int xx = x; xx < x + w; xx++)
			row[xx] = color;
	}
}

static void fl_text(uint32_t *fb, int y, int scale, uint32_t color, const char *s) {
	int x = (FL_W - (int)strlen(s) * FONT_WIDTH * scale) / 2;
	for (; *s; s++, x += FONT_WIDTH * scale) {
		const unsigned char *glyph = &font[(unsigned char)*s * FONT_HEIGHT];
		for (int gy = 0; gy < FONT_HEIGHT; gy++)
			for (int gx = 0; gx < FONT_WIDTH; gx++)
				if ((glyph[gy] >> (7 - gx)) & 1)
					fl_rect(fb, x + gx * scale, y + gy * scale, scale, scale, color);
	}
}

// debugMode: answer a pending scripts/vita_screenshot.py request with the status image (the loader's screenshot
// thread only serves the main loop, and the frame stays up through Startup)
static void fl_screenshot(const uint32_t *fb) {
	SceIoStat st;
	if (sceIoGetstat("ux0:data/gms/shared/screenshot.req", &st) < 0)
		return;
	sceIoRemove("ux0:data/gms/shared/screenshot.req");
	SceUID fd = sceIoOpen("ux0:data/gms/shared/screenshot.tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return;
	uint32_t header[3] = {FL_W, FL_H, FL_W};
	int ok = sceIoWrite(fd, header, sizeof(header)) == (int)sizeof(header);
	ok = ok && sceIoWrite(fd, fb, FL_W * FL_H * 4) == FL_W * FL_H * 4;
	ok = sceIoClose(fd) >= 0 && ok;
	if (!ok) { // never publish a truncated image
		sceIoRemove("ux0:data/gms/shared/screenshot.tmp");
		return;
	}
	sceIoRemove("ux0:data/gms/shared/screenshot.raw");
	sceIoRename("ux0:data/gms/shared/screenshot.tmp", "ux0:data/gms/shared/screenshot.raw");
}

void first_launch_prepare(void) {
	// Same title id and paths as vglInitWithCustomSizes (the loader doesn't call vglSetShaderCachePath)
	char titleid[12] = "";
	if (sceAppMgrAppParamGetString(0, 12, titleid, sizeof(titleid)) < 0 || !titleid[0])
		return;
	titleid[sizeof(titleid) - 1] = 0;
	snprintf(fl_ready, sizeof(fl_ready), "ux0:data/shader_cache/%s/" VGL_SHADER_CACHE_DIR "/" FL_READY_NAME, titleid);
	SceIoStat st;
	if (sceIoGetstat(fl_ready, &st) >= 0)
		return;
	fl_cold = 1;
	fl_t0 = sceKernelGetProcessTimeWide();
	debugPrintf("First launch: %s missing, the shader cache gets built during Startup\n", fl_ready);
}

uint32_t *first_launch_status_image(int *w, int *h) {
	if (!fl_cold)
		return NULL;
	uint32_t *fb = (uint32_t *)malloc(FL_W * FL_H * 4);
	if (!fb)
		return NULL;
	fl_rect(fb, 0, 0, FL_W, FL_H, FL_BG);
	fl_text(fb, 238, 2, FL_TEXT, "First launch: preparing the graphics shaders.");
	fl_text(fb, 272, 2, FL_DIM, "This takes about a minute. Later launches are faster.");
	if (debugMode)
		fl_screenshot(fb);
	*w = FL_W;
	*h = FL_H;
	return fb;
}

void first_launch_done(void) {
	const uint32_t *s = vgl_shader_cache_stats;
	debugPrintf("Shader cache: %u loaded, %u compiled; lookup+load %.2f s, compile+save %.2f s\n", (unsigned)s[0],
		(unsigned)s[1], s[2] / 1000000.0, s[3] / 1000000.0);
	if (!fl_cold)
		return;
	// Startup compiled (and saved) the shaders the game creates up front; later ones are compiled when first used
	SceUID fd = sceIoOpen(fl_ready, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd >= 0)
		sceIoClose(fd);
	debugPrintf("First launch: shader cache built in %.1f s (marker %s)\n",
		(sceKernelGetProcessTimeWide() - fl_t0) / 1000000.0, fd >= 0 ? "written" : "not written");
}
