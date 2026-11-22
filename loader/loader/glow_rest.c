/*
 * glow_rest.c
 *
 * Ceiling tiles and doors fade a glow in when the player comes near. obj_draw_static_objects runs their user event 14
 * for every tile and door in the room on every frame, on screen or not (bunker2_3: 16 tiles and 7 doors, 0.22 ms per
 * frame). The event is, for obj_ceiling_tile (obj_door reads its threshold from glow_distance and fades out on another
 * floor, `level != obj_player.level`):
 *
 *   if (instance_exists(obj_player)) {
 *       if (point_distance(x, y, obj_player.x, obj_player.y) < 300) { if (glow_alpha < 1) glow_alpha += 0.05; }
 *       else glow_alpha -= 0.05;
 *       glow_alpha = clamp(glow_alpha, 0, 1);
 *       if (glow_alpha > 0 && inside_screen(100)) draw_sprite_ext(<glow sprite>, ...);
 *   }
 *
 * With the glow already at 0 and the player farther than the threshold, the event writes 0 back into glow_alpha and
 * draws nothing; without a player it does nothing at all. Those calls return before the event: the check reads the
 * same values the event reads (instance_exists, obj_player.x/y through the runner's variable getter), and keeps a
 * pixel of margin over the threshold so a rounding difference can't flip the comparison. No random numbers, no
 * other state. exp_glowrest0 runs every event; exp_glowverify (debug) runs it after a skip as well and counts the
 * times glow_alpha didn't stay 0. Counters print with the profiler as [GLOWREST].
 */

#include <vitasdk.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

typedef int (*builtin_var_fn)(void *inst, int index, gml_rvalue *value);
typedef void (*object_var_fn)(int object, int var, int index, gml_rvalue *out, int prepare, int error);

#define OBJ_PLAYER 745 // obj_player in APK 1.1.10
#define TILE_GLOW_DISTANCE 300.0
#define MARGIN 1.0

static int *var_glow_alpha, *var_glow_distance, *var_x, *var_y;
static builtin_var_fn gv_x, gv_y;
static object_var_fn object_var;
static builtin_fn instance_exists_fn;
static event_func orig_tile_u14, orig_door_u14;
static unsigned rest_skips, rest_runs, rest_noplayer, verify_checks, verify_mismatches;
static int verify;

static int real_value(const gml_rvalue *v, double *out) {
	if (!v || (v->kind & 0xffffff) != 0)
		return 0;
	*out = v->val;
	return 1;
}

static int player_exists(void *self, void *other) {
	gml_rvalue result = {0, 0, 5};
	gml_rvalue arg;
	uint32_t ref[4] = {OBJ_PLAYER, 0x1000000, 0, 15}; // the event's argument: an object reference to obj_player
	memcpy(&arg, ref, sizeof(arg));
	instance_exists_fn(&result, self, other, 1, &arg);
	return (result.kind & 0xffffff) == 13 || (result.kind & 0xffffff) == 0 ? result.val > 0.5 : -1;
}

// 1: the event would change nothing (skip it), 0: run it. threshold < 0: read glow_distance (doors)
static int glow_at_rest(void *self, void *other, double threshold) {
	double glow, d;
	gml_rvalue *g = gml_vget(self, var_glow_alpha[1]);
	if (!real_value(g, &glow) || glow != 0.0 || signbit(glow))
		return 0;
	if (threshold < 0 && !real_value(gml_vget(self, var_glow_distance[1]), &threshold))
		return 0;
	int exists = player_exists(self, other);
	if (exists == 0) {
		rest_noplayer++;
		return 1;
	}
	if (exists < 0)
		return 0;
	gml_rvalue px = {0, 0, 5}, py = {0, 0, 5}, sx = {0, 0, 5}, sy = {0, 0, 5};
	object_var(OBJ_PLAYER, var_x[1], (int)0x80000000, &px, 0, 0);
	object_var(OBJ_PLAYER, var_y[1], (int)0x80000000, &py, 0, 0);
	gv_x(self, (int)0x80000000, &sx);
	gv_y(self, (int)0x80000000, &sy);
	double ax, ay, bx, by;
	if (!real_value(&px, &ax) || !real_value(&py, &ay) || !real_value(&sx, &bx) || !real_value(&sy, &by))
		return 0;
	d = sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
	return d > threshold + MARGIN;
}

// exp_glowverify: run the event after a skip decision too and check that glow_alpha is still exactly +0.0 (then the
// event drew nothing either)
static void verify_skip(event_func orig, void *self, void *other) {
	orig(self, other);
	gml_rvalue *g = gml_vget(self, var_glow_alpha[1]);
	double glow;
	verify_checks++;
	if (!real_value(g, &glow) || glow != 0.0 || signbit(glow))
		verify_mismatches++;
}

static void glow_event(event_func orig, void *self, void *other, double threshold) {
	if (glow_at_rest(self, other, threshold)) {
		rest_skips++;
		if (verify)
			verify_skip(orig, self, other);
		return;
	}
	rest_runs++;
	orig(self, other);
}

static void tile_u14(void *self, void *other) {
	glow_event(orig_tile_u14, self, other, TILE_GLOW_DISTANCE);
}

static void door_u14(void *self, void *other) {
	glow_event(orig_door_u14, self, other, -1);
}

void glow_rest_report(int frames) {
	if (!orig_tile_u14 && !orig_door_u14)
		return;
	if (rest_skips || rest_runs)
		debugPrintf("[GLOWREST] tile/door glow events skipped %.1f (no player %.1f) run %.1f per frame\n",
			(float)rest_skips / frames, (float)rest_noplayer / frames, (float)rest_runs / frames);
	if (verify)
		debugPrintf("[GLOWREST] verify: %u skips checked, %u mismatches\n", verify_checks, verify_mismatches);
	rest_skips = rest_runs = rest_noplayer = 0;
}

void glow_rest_install(gml_func_entry *table) {
	if (exp_flag("glowrest0")) {
		debugPrintf("glow events at rest: run (exp_glowrest0)\n");
		return;
	}
	verify = debugMode && exp_flag("glowverify");
	var_glow_alpha = (int *)so_symbol(&yoyoloader_mod, "g_VAR_glow_alpha");
	var_glow_distance = (int *)so_symbol(&yoyoloader_mod, "g_VAR_glow_distance");
	var_x = (int *)so_symbol(&yoyoloader_mod, "g_VAR_x");
	var_y = (int *)so_symbol(&yoyoloader_mod, "g_VAR_y");
	gv_x = (builtin_var_fn)so_symbol(&yoyoloader_mod, "_Z4GV_XP9CInstanceiP6RValue");
	gv_y = (builtin_var_fn)so_symbol(&yoyoloader_mod, "_Z4GV_YP9CInstanceiP6RValue");
	object_var = (object_var_fn)so_symbol(&yoyoloader_mod, "_Z23YYGML_Variable_GetValueiiiP6RValuebb");
	instance_exists_fn = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z16F_InstanceExistsR6RValueP9CInstanceS2_iPS_");
	int ok = var_glow_alpha && var_glow_distance && var_x && var_y && gv_x && gv_y && object_var && instance_exists_fn;
	if (ok) {
		orig_tile_u14 = gml_replace(table, "gml_Object_obj_ceiling_tile_Other_24", tile_u14);
		orig_door_u14 = gml_replace(table, "gml_Object_obj_door_Other_24", door_u14);
	}
	debugPrintf("glow events at rest: tiles %s, doors %s\n", orig_tile_u14 ? "skipped" : "NOT hooked",
		orig_door_u14 ? "skipped" : "NOT hooked");
}
