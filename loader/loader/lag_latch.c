/*
 * lag_latch.c: keep the game's lag mode on once leaving it failed (lagLatch, 2 by default; visible)
 *
 * The game's own effects governor, obj_opto_master user event 5 (work/cpu_research/lag_mode.md), enters "lag mode"
 * when fps_real (1 / work time) stays under 70 and leaves it after 30 ticks at 80 or more. Out of lag mode it turns
 * rim shading back on. Where full rim shading can't hold 60 (chinatown_4 with several enemies in view: 7-8 frames
 * of ~31 ms), the game leaves lag mode, drops 7-8 frames, re-enters it, and repeats until its own latch (4 dips in
 * one room: underThreshDips >= 4 keeps it on until the next Room Start). That is 3-4 bursts of stutter in the first
 * ~10 s after each entry or restart, then rim shading stays off for the room anyway.
 *
 * lagLatch=1 takes the game's latch at the first failed exit instead of the fourth: when the game re-enters lag mode
 * within RETRY_TICKS of leaving it, underThreshDips is raised to 4. lagLatch=2 also remembers the room until the
 * app quits: a later entry or restart of a room that latched starts in lag mode, latched, with rim shading off
 * (underThreshTiming 30, underThreshDips 4, global.rimshadingFade 0, global.rim_shading false): no burst at all
 * after the first time.
 * Visible: rim shading (and the other lag-mode effect cuts: blood lifetime, trail smoothness, particle budget) stays
 * off where the game would have turned it on for a second or two between dips. The steady state of such a room is
 * the same as the game's. Nothing else reads these variables (lag_mode.md: only obj_opto_master references them).
 */
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

int lag_latch = 2; // yyl.cfg lagLatch (default 2): 0 off (game behaviour), 1 latch at the first failed exit, 2 also per room

#define RETRY_TICKS 60 // a dip this soon after leaving lag mode counts as a failed exit (chinatown_4: 5-9 ticks)
#define MAX_ROOMS 1024

static event_func orig_user5, orig_room_start;
static int *var_timing, *var_dips, *var_fade, *var_rim;
static uint32_t left_tick; // frame_count at the last exit from lag mode (0: none in this room)
static uint8_t latched_room[MAX_ROOMS / 8];
static unsigned latches, room_latches, exits, failed_exits;

static void user5(void *self, void *other) {
	gml_rvalue *t = instance_var(self, var_timing), *d = instance_var(self, var_dips);
	double t0 = t ? t->val : 0, d0 = d ? d->val : 0;
	orig_user5(self, other);
	t = instance_var(self, var_timing), d = instance_var(self, var_dips);
	if (!t || !d)
		return;
	if (t0 != 0 && t->val == 0) { // "~!lag mode recovered"
		left_tick = *frame_count ? *frame_count : 1;
		exits++;
		return;
	}
	if (d->val > d0 && left_tick) { // "~!BELOW THRESHOLD" after an exit
		if (*frame_count - left_tick <= RETRY_TICKS) {
			failed_exits++;
			if (d->val < 4) {
				d->val = 4; // the game's own latch: no more exits until the next Room Start
				latches++;
			}
			int room = *current_room;
			if (lag_latch >= 2 && room >= 0 && room < MAX_ROOMS)
				latched_room[room >> 3] |= 1 << (room & 7);
		}
		left_tick = 0;
	}
}

static void room_start(void *self, void *other) {
	orig_room_start(self, other); // sets underThreshDips = 0
	left_tick = 0;
	int room = *current_room;
	if (lag_latch >= 2 && room >= 0 && room < MAX_ROOMS && (latched_room[room >> 3] >> (room & 7) & 1)) {
		// Start in lag mode, latched, with rim shading already faded out: the state this room ends up in after the
		// entry hitch, without the 20 ticks of fading rim shading (the first burst of a room entered at full quality)
		gml_rvalue *d = instance_var(self, var_dips), *t = instance_var(self, var_timing);
		gml_rvalue *fade = global_var(var_fade), *rim = global_var(var_rim);
		if (d && t && fade && rim) {
			d->val = 4;
			t->val = 30;
			fade->val = 0;
			rim->val = 0;
			room_latches++;
		}
	}
}

void lag_latch_report(int frames) {
	(void)frames;
	if (orig_user5 && (exits || failed_exits || room_latches))
		debugPrintf("[LAGLATCH] mode %d: exits %u, failed exits %u, latched %u, room starts latched %u\n", lag_latch,
			exits, failed_exits, latches, room_latches);
	exits = failed_exits = latches = room_latches = 0;
}

void lag_latch_install(gml_func_entry *table) {
	if (!lag_latch) {
		debugPrintf("lag mode latch: off (lagLatch=0)\n");
		return;
	}
	var_timing = (int *)so_symbol(&yoyoloader_mod, "g_VAR_underThreshTiming");
	var_dips = (int *)so_symbol(&yoyoloader_mod, "g_VAR_underThreshDips");
	var_fade = (int *)so_symbol(&yoyoloader_mod, "g_VAR_rimshadingFade");
	var_rim = (int *)so_symbol(&yoyoloader_mod, "g_VAR_rim_shading");
	if (!var_timing || !var_dips || !var_fade || !var_rim || !frame_count || !current_room || !global_obj_ptr) {
		debugPrintf("lag mode latch: NOT installed (variables missing)\n");
		return;
	}
	orig_user5 = gml_replace(table, "gml_Object_obj_opto_master_Other_15", user5);
	if (orig_user5)
		orig_room_start = gml_replace(table, "gml_Object_obj_opto_master_Other_4", room_start);
	if (orig_user5 && !orig_room_start) { // put user 5 back: the latch needs both
		gml_replace(table, "gml_Object_obj_opto_master_Other_15", orig_user5);
		orig_user5 = NULL;
	}
	debugPrintf("lag mode latch: %s (lagLatch=%d)\n", orig_user5 ? "installed" : "NOT installed", lag_latch);
}
