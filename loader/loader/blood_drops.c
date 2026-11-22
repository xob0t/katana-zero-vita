/*
 * blood_drops.c
 *
 * With BLOOD SPRAY off (global.show_blood false) the game still creates the obj_blood drops of every hit and runs
 * their Step: obj_blood Create only sets visible = false. Each Step runs user event 0 (motion, timers) and its floor
 * test: kill = true once block_meeting(x, y) or block_meeting(x - hsp*fr/2, y - vsp*fr/2) hits a block. kill is
 * never reset, so once a drop has landed the test can't change anything, yet it still costs one or two collision
 * queries per tick (block_meeting is ~20 us each). With BLOOD SPRAY off this skips them: while a landed drop
 * (kill already true) runs its Step, block_meeting returns true at once, which takes the same path the original
 * takes (kill = true again, a no-op). Everything else in the event still runs, so the drops move, animate and die
 * on the same tick as before.
 *
 * Why the drops aren't simply destroyed or frozen: with the option off they still matter.
 * - obj_opto_master counts them (instance_number(obj_blood)) into global.particle_scaler, which scales how many
 *   sparks, dust and debris other objects spawn when the game isn't in lag mode.
 * - obj_smokescreen_master Draw draws every on-screen drop that overlaps a smoke cloud, whatever show_blood is.
 * - obj_throwable user event 11 runs collision tests on the drops and moves them; cutscenes destroy them.
 * The floor decals (add_bloodtile), drawactors' drop drawing and the replay recording of drops only run with
 * show_blood true, and every random() call of user event 0 is inside its show_blood branch, so random number use
 * doesn't change either. With BLOOD SPRAY on nothing is skipped. exp_blooddrops0 turns it off.
 * Debug counters print with the profiler as [BLOOD].
 */

#include <vitasdk.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

static int *var_show_blood, *var_kill;
static event_func orig_blood_step;
static gml_script_fn orig_block_meeting;
static void *landed_drop; // the drop whose Step is running with its floor test skipped

static unsigned blood_steps, blood_landed, blood_skipped_tests;
int blood_drops_installed;

// A real or boolean variable of obj (read-only lookup), NULL otherwise
static gml_rvalue *read_var(void *obj, int *var) {
	gml_rvalue *v = obj && var ? gml_vget(obj, var[1]) : NULL;
	return v && (v->kind == 0 || v->kind == 13) ? v : NULL;
}

static void blood_step(void *self, void *other) {
	blood_steps++;
	gml_rvalue *show = read_var(*global_obj_ptr, var_show_blood), *kill;
	// show_blood false as the game's if() reads it (BOOL_RValue: > 0.5), kill exactly as `kill = true` wrote it
	if (!show || show->val > 0.5 || !(kill = read_var(self, var_kill)) || kill->kind != 13 || kill->val != 1.0) {
		orig_blood_step(self, other);
		return;
	}
	blood_landed++;
	landed_drop = self;
	orig_blood_step(self, other);
	landed_drop = NULL;
}

static void *block_meeting_hook(void *self, void *other, void *result, int argc, void **args) {
	if (self && self == landed_drop) {
		gml_rvalue *r = result;
		r->val = 1.0;
		r->flags = 0;
		r->kind = 13; // `return true`
		blood_skipped_tests++;
		return result;
	}
	return orig_block_meeting(self, other, result, argc, args);
}

void blood_drops_report(int frames) {
	if (!blood_drops_installed || !blood_steps)
		return;
	debugPrintf("[BLOOD] drop steps %.1f per tick, landed and hidden %.1f, floor tests skipped %.1f\n",
		(float)blood_steps / frames, (float)blood_landed / frames, (float)blood_skipped_tests / frames);
	blood_steps = blood_landed = blood_skipped_tests = 0;
}

void blood_drops_install(gml_func_entry *table) {
	if (exp_flag("blooddrops0")) {
		debugPrintf("hidden blood drop floor tests: kept (exp_blooddrops0)\n");
		return;
	}
	var_show_blood = (int *)so_symbol(&yoyoloader_mod, "g_VAR_show_blood");
	var_kill = (int *)so_symbol(&yoyoloader_mod, "g_VAR_kill");
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z24gml_Script_block_meetingP9CInstanceS0_R8YYRValueiPPS1_");
	if (var_show_blood && var_kill && global_obj_ptr && entry &&
			(orig_blood_step = gml_replace(table, "gml_Object_obj_blood_Step_0", blood_step))) {
		if ((orig_block_meeting = entry_hook(entry, block_meeting_hook)))
			blood_drops_installed = 1;
		else
			gml_replace(table, "gml_Object_obj_blood_Step_0", orig_blood_step);
	}
	debugPrintf("hidden blood drop floor tests: %s\n", blood_drops_installed ? "skipped once landed" : "NOT installed");
}
