/*
 * enemy_timing.c
 *
 * Enemies attack on the PC version's timing (ENEMY TIMING: PC, the default). ANDROID keeps the Netflix release's
 * slower attacks. GAME menu row ENEMY TIMING (katana_menu.c), or enemyTiming=1 in yyl.cfg for ANDROID.
 *
 * The Android release (1.1.10) slows enemy attacks; found by comparing Katana ZERO.exe from Steam (YYC x86) with
 * libyoyo.so statically, every function that reads or writes the attack timers:
 * - AI_init (every enemy's Create): first_attack_timer = 30, PC 15 (the delay before an enemy's first attack). The
 *   room and difficulty additions after it (+10 in one room, +4 per difficulty step) are the same in both builds.
 * - A new sec_attack_timer (the name doesn't exist in the PC exe), added to every fire threshold:
 *     set by AI_init (6), obj_headhunter Create (6), obj_drone Create (5);
 *     read by obj_enemy, obj_enemy_cop          (PC shoot_timer > 20, Android > 20 + sec_attack_timer),
 *             obj_enemy_machinegun              (PC > 8,  Android > 8 + sec_attack_timer),
 *             obj_enemy_shotgun, _shieldcop, _sci (PC > 40, Android > 40 + sec_attack_timer),
 *             obj_headhunter                    (PC > 15, Android > 15 + sec_attack_timer).
 *   shoot_timer counts up by global.fr, so every shot comes 5-6 frames later.
 * The other constants in the AI scripts and the enemy Steps match (the Android compiler folds some PC expressions,
 * e.g. 7 + round(6) to 13).
 *
 * Visible in the first room: the tutorial turns on the slow-mo (global.slowdown -= slowdown / 12 every frame, down
 * to 0) a few frames before the gunman's shoot_timer reaches 20. On PC he fires 6 frames into the slow-down (fr 0.59)
 * and the bullet stops in the air in front of Zero. With the Android delay he fires 28 frames in (fr 0.09): the
 * bullet moves 29 room px and stays hidden inside his own muzzle flash, which freezes too.
 *
 * PC timing, after AI_init: first_attack_timer -= 15 (what AI_init added past the PC's base) and sec_attack_timer 0;
 * after the headhunter's and the drone's Create: sec_attack_timer 0. A change of the setting applies to enemies
 * created after it: the next room, or a restart.
 * The comparisons run with the runner's epsilon (1e-5 on Android, 1e-10 in the PC exe); that runner difference is
 * the same for every comparison in the game and not part of this.
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

int enemy_timing_cfg = 0; // enemyTiming in yyl.cfg: 0 PC, 1 ANDROID
int enemy_timing_android; // what applies now: the menu's choice, else enemy_timing_cfg (options_menu_sync)

static gml_script_fn orig_ai_init;
static event_func orig_headhunter_create, orig_drone_create;
static int *var_first_attack_timer, *var_sec_attack_timer;
static unsigned logged;

static void sec_attack_pc(void *self) {
	gml_rvalue *v = instance_var(self, var_sec_attack_timer); // NULL unless a real
	if (v)
		v->val = 0.0;
}

static void *ai_init_timing(void *self, void *other, void *result, int argc, void **args) {
	void *r = orig_ai_init(self, other, result, argc, args);
	if (enemy_timing_android || !self)
		return r;
	sec_attack_pc(self);
	gml_rvalue *v = instance_var(self, var_first_attack_timer);
	double first = v ? v->val : -1.0;
	if (v)
		v->val = first - 15.0;
	if (debugMode && logged < 8) {
		logged++;
		debugPrintf("[ENEMYTIMING] PC timing: first_attack_timer %g -> %g, sec_attack_timer 0\n", first, first - 15.0);
	}
	return r;
}

static void headhunter_create_timing(void *self, void *other) {
	orig_headhunter_create(self, other);
	if (!enemy_timing_android)
		sec_attack_pc(self);
}

static void drone_create_timing(void *self, void *other) {
	orig_drone_create(self, other);
	if (!enemy_timing_android)
		sec_attack_pc(self);
}

void enemy_timing_install(gml_func_entry *table) {
	enemy_timing_android = enemy_timing_cfg;
	var_first_attack_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_first_attack_timer");
	var_sec_attack_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_sec_attack_timer");
	if (var_first_attack_timer && var_sec_attack_timer)
		orig_ai_init = entry_hook(so_symbol(&yoyoloader_mod, "_Z18gml_Script_AI_initP9CInstanceS0_R8YYRValueiPPS1_"),
			ai_init_timing);
	if (orig_ai_init) {
		orig_headhunter_create = gml_replace(table, "gml_Object_obj_headhunter_Create_0", headhunter_create_timing);
		orig_drone_create = gml_replace(table, "gml_Object_obj_drone_Create_0", drone_create_timing);
	}
	debugPrintf("enemy timing: %s (yyl.cfg enemyTiming %d; headhunter %s, drone %s)\n",
		orig_ai_init ? "hooked" : "NOT installed", enemy_timing_cfg, orig_headhunter_create ? "hooked" : "not hooked",
		orig_drone_create ? "hooked" : "not hooked");
}

int enemy_timing_available(void) {
	return orig_ai_init != NULL;
}
