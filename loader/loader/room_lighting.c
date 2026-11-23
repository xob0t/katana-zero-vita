/*
 * room_lighting.c
 *
 * No lamp glow in room_haunted_cart2 and room_club_0 (roomLighting=1 in yyl.cfg keeps it).
 *
 * The game has two lighting switches that no Android menu shows and KatanaOptions.zero doesn't save (obj_init sets
 * both to 1): global.platform_lighting and global.character_lighting. Measured on the Vita with both off, unprofiled:
 * haunted_cart2 27.3 -> 25.3 ms per frame (36 -> 39 fps), club_0 17.2 -> 16.1 ms (57 -> 60 fps). Other heavy rooms
 * gained 0.6 ms or less, so only these two rooms change.
 *
 * Readers in live code (every reference to the two globals in libyoyo.so, by GOT slot):
 * - platform_lighting: obj_new_platform_lighting Draw (`if (platform_lighting && lights_on)`: additive glow of
 *   obj_lightback, obj_garagelamp and obj_spotlight user 0) and obj_drawspheres Draw. obj_new_platform_lighting
 *   user 1 also tests it but nothing runs that event (Create calls user 0).
 * - character_lighting: obj_clubber and obj_heli user 6 (light_drawme: the rim glow drawn on dancers and the
 *   helicopter; the dancers' glow is already skipped with dancerGlow=0). The six lamp user 0 events (obj_halogen
 *   etc.) test it around an empty body, and draw_rim_shading / pipeline_rim_shading (via pipeline_draw) have no
 *   callers.
 * - Writers only: obj_init Create, the reset_game_data scripts, the PC options menus. load_options and save_options
 *   never touch them.
 * All readers only draw; no gameplay code reads either value.
 *
 * The globals themselves are never changed: in those two rooms each reader event runs with the value reading 0 and
 * gets the game's own value back as soon as it returns. Leaving the room, restarting it, death/rewind, the pause
 * menu, resetting game data and saving options all see the game's values, so there's nothing to restore.
 * debugMode: exp_roomlighting1 (read at every Room Start) keeps the glow like roomLighting=1, for A/B runs in one boot.
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

int room_lighting = 0; // roomLighting=1 in yyl.cfg: the game's lighting in haunted_cart2 and club_0 too

static const char *const rooms[] = { "room_haunted_cart2", "room_club_0" };
#define N_ROOMS (sizeof(rooms) / sizeof(*rooms))
static int room_ids[N_ROOMS] = { -2, -2 };
static int *var_platform_lighting, *var_character_lighting;
static int keep_ab; // debugMode: exp_roomlighting1 at the last Room Start
static unsigned hidden_reads;
static int (*room_find)(const char *name);

static int lighting_off_here(void) {
	if (room_lighting || keep_ab || !current_room)
		return 0;
	int room = *current_room;
	for (unsigned i = 0; i < N_ROOMS; i++) {
		if (room_ids[i] < 0 && room_find)
			room_ids[i] = room_find(rooms[i]); // resolved once the room table is loaded
		if (room_ids[i] >= 0 && room == room_ids[i])
			return 1;
	}
	return 0;
}

static gml_rvalue *global_ref(int *var) {
	if (!var || !global_obj_ptr || !*global_obj_ptr)
		return NULL;
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)*global_obj_ptr)[3];
	gml_rvalue *v = vwrite(*global_obj_ptr, var[1]);
	return v && ((v->kind & 0xffffff) == 0 || (v->kind & 0xffffff) == 13) ? v : NULL; // real or bool
}

// Runs orig with the global reading 0 in the two rooms, and puts the game's value back afterwards
static void run_unlit(event_func orig, int *var, void *self, void *other) {
	gml_rvalue *v = lighting_off_here() ? global_ref(var) : NULL;
	if (!v || v->val == 0.0) {
		orig(self, other);
		return;
	}
	double was = v->val;
	v->val = 0.0;
	hidden_reads++;
	orig(self, other);
	if ((v = global_ref(var))) // looked up again: the event may grow the global table
		v->val = was;
}

static event_func orig_platform_draw, orig_spheres_draw, orig_clubber_user6, orig_heli_user6;
static void platform_draw(void *s, void *o) { run_unlit(orig_platform_draw, var_platform_lighting, s, o); }
static void spheres_draw(void *s, void *o) { run_unlit(orig_spheres_draw, var_platform_lighting, s, o); }
static void clubber_user6(void *s, void *o) { run_unlit(orig_clubber_user6, var_character_lighting, s, o); }
static void heli_user6(void *s, void *o) { run_unlit(orig_heli_user6, var_character_lighting, s, o); }

static event_func orig_opto_room_start;
static void opto_room_start(void *self, void *other) {
	orig_opto_room_start(self, other);
	if (!debugMode)
		return;
	keep_ab = exp_flag("roomlighting1");
	static unsigned last_reads;
	int room = current_room ? *current_room : -1;
	debugPrintf("[ROOMLIGHT] room %d: lamp glow %s (%u reads hidden in the last room)\n", room,
		lighting_off_here() ? "off" : "kept", hidden_reads - last_reads);
	last_reads = hidden_reads;
}

void room_lighting_install(gml_func_entry *table) {
	room_find = (void *)so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
	var_platform_lighting = (int *)so_symbol(&yoyoloader_mod, "g_VAR_platform_lighting");
	var_character_lighting = (int *)so_symbol(&yoyoloader_mod, "g_VAR_character_lighting");
	if (room_lighting) {
		debugPrintf("room lighting: the game's own everywhere (roomLighting=1)\n");
		return;
	}
	if (!room_find || !var_platform_lighting || !var_character_lighting || !global_obj_ptr || !current_room) {
		debugPrintf("room lighting: NOT installed\n");
		return;
	}
	orig_platform_draw = gml_replace(table, "gml_Object_obj_new_platform_lighting_Draw_0", platform_draw);
	orig_spheres_draw = gml_replace(table, "gml_Object_obj_drawspheres_Draw_0", spheres_draw);
	extern int dancer_glow; // dancerGlow=0 (default) already returns from the dancers' user 6 at its entry
	if (dancer_glow)
		orig_clubber_user6 = gml_replace(table, "gml_Object_obj_clubber_Other_16", clubber_user6);
	orig_heli_user6 = gml_replace(table, "gml_Object_obj_heli_Other_16", heli_user6);
	if (debugMode)
		orig_opto_room_start = gml_replace(table, "gml_Object_obj_opto_master_Other_4", opto_room_start);
	debugPrintf("room lighting: lamp glow off in haunted_cart2 and club_0 (draw %s, spheres %s, clubber %s, heli %s)\n",
		orig_platform_draw ? "ok" : "MISSING", orig_spheres_draw ? "ok" : "MISSING",
		orig_clubber_user6 ? "ok" : "glow off already", orig_heli_user6 ? "ok" : "MISSING");
}
