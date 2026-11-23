/*
 * hang_watch.h
 *
 * Game-thread side of the freeze locator (hang_watch.c). With the locator off (release builds, debugMode=0,
 * exp_hangwatch0) every call here is one load of hang_watch_on and a not-taken branch.
 */
#ifndef HANG_WATCH_H
#define HANG_WATCH_H

#include <stdint.h>

extern int hang_watch_on;
extern volatile uint32_t hang_frames;
extern volatile int hang_stage;

enum {
	HANG_STAGE_INPUT, HANG_STAGE_PROCESS, HANG_STAGE_SWAP, HANG_STAGE_AFTER_SWAP, HANG_STAGE_TEXFLUSH,
	HANG_STAGE_GPU_GC, HANG_STAGE_SCREENSHOT, HANG_STAGE_REPORT,
};

// Main loop: one frame started (the heartbeat the watchdog checks)
static inline void hang_heartbeat(void) {
	if (hang_watch_on)
		__atomic_store_n(&hang_frames, hang_frames + 1, __ATOMIC_RELEASE); // only the game thread writes it
}

static inline void hang_set_stage(int stage) {
	if (hang_watch_on)
		__atomic_store_n(&hang_stage, stage, __ATOMIC_RELEASE);
}

// Save I/O in progress (Netflix slot writes, files the runner opens for writing): no core dump meanwhile
void hang_io_begin(void);
void hang_io_end(void);
void hang_io_file_open(void *file, const char *mode);
void hang_io_file_close(void *file);

// Game thread, once the runner is initialised: snapshot the object names and builtin table for the reports
void hang_watch_names(void);
// Game thread, at each room change
void hang_watch_room(int room);

#endif
