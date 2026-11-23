/*
 * screenshot.c
 *
 * Debug-mode remote screenshots: when ux0:data/gms/shared/screenshot.req exists, the
 * currently displayed framebuffer is dumped to screenshot.raw (12-byte header with
 * width, height and pitch, followed by ABGR8888 pixels) and the request is removed.
 *
 * All memory card access happens on a thread on the worker cores: it polls the debug request files
 * (screenshot.req, goto_room, level_complete, create_object,
 * destroy_object), reads and removes them, and writes screenshots
 * from a copy the game thread made. The game thread used to stat screenshot.req every 30 frames and write the
 * 2 MB screenshot itself, right after vglSwapBuffers: each screenshot froze the game for 0.4-0.5 s (logged as
 * swap time; room_club_1 runs "hitched" every 20 s, exactly the harness's screenshot interval), and a stat can
 * block for as long as the card is busy.
 */

#include <vitasdk.h>
#include <stdlib.h>

#include "file_replace.h"
#include "main.h"

#define SCREENSHOT_REQ "ux0:data/gms/shared/screenshot.req"
#define SCREENSHOT_OUT "ux0:data/gms/shared/screenshot.raw"
#define SCREENSHOT_TMP "ux0:data/gms/shared/screenshot.tmp"
#define SCREENSHOT_BAK "ux0:data/gms/shared/screenshot.bak"
#define DEBUG_REQUEST_POLL_US 500000
#define DEBUG_REQUEST_SLOW_US 50000

extern int debugMode;
int worker_thread_cpu_mask(void);

static const char *const debug_request_paths[DEBUG_REQUEST_COUNT] = {
	SCREENSHOT_REQ,
	"ux0:data/gms/shared/goto_room",
	"ux0:data/gms/shared/level_complete",
	"ux0:data/gms/shared/create_object",
	"ux0:data/gms/shared/destroy_object",
};
static char debug_request_text[DEBUG_REQUEST_COUNT][64];
static volatile int debug_request_pending[DEBUG_REQUEST_COUNT];
static void *volatile shot_pixels; // framebuffer copy waiting to be written (header in shot_header)
static uint32_t shot_header[3];

static void screenshot_write(void) {
	// Written under a temporary name so the PC never downloads a partial file
	SceUID fd = sceIoOpen(SCREENSHOT_TMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd >= 0) {
		int size = shot_header[2] * shot_header[1] * 4;
		int ok = sceIoWrite(fd, shot_header, sizeof(shot_header)) == sizeof(shot_header) &&
			sceIoWrite(fd, shot_pixels, size) == size;
		if (sceIoClose(fd) < 0)
			ok = 0;
		if (!ok || replace_file(SCREENSHOT_TMP, SCREENSHOT_OUT, SCREENSHOT_BAK) < 0) {
			remove_if_present(SCREENSHOT_TMP);
			debugPrintf("Could not replace %s\n", SCREENSHOT_OUT);
		}
	}
	free(shot_pixels);
	__sync_synchronize();
	shot_pixels = NULL;
}

static int debug_request_thread(SceSize args, void *argp) {
	for (;;) {
		if (shot_pixels) {
			// Pairs with the barrier in screenshot_check: the header and pixels are read only after the pointer
			__sync_synchronize();
			screenshot_write();
		}
		for (int i = 0; i < DEBUG_REQUEST_COUNT; i++) {
			if (debug_request_pending[i])
				continue;
			SceIoStat st;
			uint32_t t0 = sceKernelGetProcessTimeLow();
			int found = sceIoGetstat(debug_request_paths[i], &st) >= 0;
			uint32_t us = sceKernelGetProcessTimeLow() - t0;
			if (us > DEBUG_REQUEST_SLOW_US)
				debugPrintf("[DBGREQ] stat %s took %.1fms (request thread)\n", debug_request_paths[i], us / 1000.0f);
			if (!found)
				continue;
			sceClibMemset(debug_request_text[i], 0, sizeof(debug_request_text[i]));
			SceUID fd = sceIoOpen(debug_request_paths[i], SCE_O_RDONLY, 0);
			if (fd >= 0) {
				sceIoRead(fd, debug_request_text[i], sizeof(debug_request_text[i]) - 1);
				sceIoClose(fd);
			}
			sceIoRemove(debug_request_paths[i]);
			__sync_synchronize();
			debug_request_pending[i] = 1;
		}
		// A screenshot copy waits at most a few ms; requests are checked every DEBUG_REQUEST_POLL_US
		for (int waited = 0; waited < DEBUG_REQUEST_POLL_US && !shot_pixels; waited += 10000)
			sceKernelDelayThread(10000);
	}
	return 0;
}

// Game thread, debug mode: 1 when the request file id was found (its content in text), without touching the files
int debug_request_take(int id, char *text, int size) {
	static int started;
	if (!started) {
		started = 1;
		SceUID thid = sceKernelCreateThread("Debug Requests", debug_request_thread, 0x10000100, 0x4000, 0,
			worker_thread_cpu_mask(), NULL);
		int ret = thid >= 0 ? sceKernelStartThread(thid, 0, NULL) : thid;
		if (ret < 0) {
			// Not retried: every debug request (screenshots, goto_room, level_complete, create_object) is off
			debugPrintf("[DBGREQ] could not start the request thread: 0x%08X\n", ret);
			if (thid >= 0)
				sceKernelDeleteThread(thid);
		}
	}
	if (!debug_request_pending[id])
		return 0;
	__sync_synchronize();
	if (text && size > 0) {
		sceClibStrncpy(text, debug_request_text[id], size);
		text[size - 1] = 0;
	}
	__sync_synchronize();
	debug_request_pending[id] = 0;
	return 1;
}

// Game thread, right after the swap: copies the displayed frame for the request thread to write
void screenshot_check() {
	// One screenshot in flight at a time; a request stays pending until the previous one is written
	if (!debugMode || shot_pixels || !debug_request_take(DEBUG_REQUEST_SCREENSHOT, NULL, 0))
		return;

	SceDisplayFrameBuf fb;
	sceClibMemset(&fb, 0, sizeof(fb));
	fb.size = sizeof(fb);
	if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !fb.base)
		return;
	void *pixels = malloc(fb.pitch * fb.height * 4);
	if (!pixels)
		return;
	sceClibMemcpy(pixels, fb.base, fb.pitch * fb.height * 4);
	shot_header[0] = fb.width;
	shot_header[1] = fb.height;
	shot_header[2] = fb.pitch;
	__sync_synchronize();
	shot_pixels = pixels;
}
