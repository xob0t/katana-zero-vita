/*
 * screenshot.c
 *
 * Debug-mode remote screenshots: when ux0:data/gms/shared/screenshot.req exists, the
 * currently displayed framebuffer is dumped to screenshot.raw (12-byte header with
 * width, height and pitch, followed by ABGR8888 pixels) and the request is removed.
 */

#include <vitasdk.h>

#include "main.h"

#define SCREENSHOT_REQ "ux0:data/gms/shared/screenshot.req"
#define SCREENSHOT_OUT "ux0:data/gms/shared/screenshot.raw"
#define SCREENSHOT_TMP "ux0:data/gms/shared/screenshot.tmp"
#define CHECK_INTERVAL_FRAMES 30

extern int debugMode;

void screenshot_check() {
	static int frames = 0;
	if (!debugMode || ++frames < CHECK_INTERVAL_FRAMES)
		return;
	frames = 0;

	SceIoStat st;
	if (sceIoGetstat(SCREENSHOT_REQ, &st) < 0)
		return;
	sceIoRemove(SCREENSHOT_REQ);

	SceDisplayFrameBuf fb;
	sceClibMemset(&fb, 0, sizeof(fb));
	fb.size = sizeof(fb);
	if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !fb.base)
		return;

	// Written under a temporary name so the PC never downloads a partial file
	SceUID fd = sceIoOpen(SCREENSHOT_TMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return;
	uint32_t header[3] = { fb.width, fb.height, fb.pitch };
	sceIoWrite(fd, header, sizeof(header));
	sceIoWrite(fd, fb.base, fb.pitch * fb.height * 4);
	sceIoClose(fd);
	sceIoRemove(SCREENSHOT_OUT);
	sceIoRename(SCREENSHOT_TMP, SCREENSHOT_OUT);
}
