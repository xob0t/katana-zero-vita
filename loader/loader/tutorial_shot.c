/*
 * tutorial_shot.c
 *
 * The first room's tutorial gunman fires on the PC version's timing, so his bullet is visible in the slow-mo
 * (pcTutorialShot=0 in yyl.cfg keeps the Android timing).
 *
 * The Android build (Netflix, 1.1.10) slowed down enemy shots. AI_init gives every enemy a new
 * sec_attack_timer = 6 (and first_attack_timer 30 instead of the PC's 15), and obj_enemy's Step fires on
 *   Android: if (shoot_timer >= 20 + sec_attack_timer && first_attack_timer <= 0)
 *   PC:      if (shoot_timer > 20 && first_attack_timer <= 0)          (Katana ZERO.exe, same Step, line 65)
 * shoot_timer counts up by global.fr. In room_factory_0 the tutorial turns on the player's slow-mo when Zero walks
 * past x 800 (global.slowdown -= slowdown / 12 every frame, down to 0), a few frames before the gunman's
 * shoot_timer reaches 20.
 * - On PC he fires 6 frames into the slow-down, at fr 0.59: the bullet flies ~195 px (room px, 1280 wide) and stops
 *   in the air in front of Zero.
 * - On Android he fires 28 frames in, at fr 0.09: the bullet moves 29 px and stops inside his own muzzle flash
 *   (which also freezes), so it can't be seen.
 * The first_attack_timer difference doesn't change this shot (it reaches 0 before the gunman aims).
 *
 * Only room_factory_0 changes, the tutorial room: its two obj_enemy instances (the gunman and an off-screen one)
 * get sec_attack_timer 0 right before each of their Steps. Everywhere else enemies keep the Android timing.
 * sec_attack_timer is read only by the enemy fire checks and written only by AI_init, so the zero affects nothing
 * else; it is an instance variable, reset by AI_init when the room restarts.
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

int pc_tutorial_shot = 1; // pcTutorialShot in yyl.cfg

static event_func orig_enemy_step;
static int *var_sec_attack_timer;
static int (*room_find)(const char *name);
static int tutorial_room = -2; // room_factory_0's index once looked up (rooms exist only after the game starts)

static void enemy_step(void *self, void *other) {
	if (tutorial_room == -2)
		tutorial_room = room_find("room_factory_0");
	if (tutorial_room >= 0 && *current_room == tutorial_room) {
		gml_rvalue *v = instance_var(self, var_sec_attack_timer); // NULL unless a real
		if (v)
			v->val = 0;
	}
	orig_enemy_step(self, other);
}

void tutorial_shot_install(gml_func_entry *table) {
	if (!pc_tutorial_shot) {
		debugPrintf("tutorial shot: Android timing (pcTutorialShot=0)\n");
		return;
	}
	room_find = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
	var_sec_attack_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_sec_attack_timer");
	if (!room_find || !var_sec_attack_timer || !current_room) {
		debugPrintf("tutorial shot: NOT installed\n");
		return;
	}
	orig_enemy_step = gml_replace(table, "gml_Object_obj_enemy_Step_0", enemy_step);
	debugPrintf("tutorial shot: %s\n", orig_enemy_step ? "PC timing in room_factory_0" : "NOT installed");
}
