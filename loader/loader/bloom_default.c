/*
 * bloom_default.c: bloom (GRAPHICS -> BLOOM FX, global.bloom_shader) is off by default. GRAPHICS -> BLOOM FX: ON
 * turns it on, and the choice is saved and kept as before. bloomDefault=1 in yyl.cfg keeps the game's default (on).
 *
 * The game's defaults are on: obj_init Create sets global.bloom_shader = 1.0 among the other defaults and then runs
 * its user event 0 (event_user(0) at line 592), which reads KatanaOptions.zero through load_options. load_options
 * reads each value with load_variable(<current value>), and load_variable returns that current value at the end of
 * the file; with no file at all load_options saves a new one from the current values (APK 1.1.10, libyoyo.so). So
 * the value in the global when load_options starts is the default, and setting 0 between obj_init Create's
 * defaults and its event_user(0) changes only the default:
 *  - no KatanaOptions.zero (fresh install): bloom off, and saved off;
 *  - a saved value: load_options reads it over the 0, on or off as the player left it.
 * The wrapper of obj_init's user event 0 writes it, only while obj_init Create runs (the only event_user(0) on it).
 *
 * mobile_reset_game_data also sets bloom_shader = 1.0 with the other defaults; the title menu and the Netflix SDK
 * object call it, then load_options (at every boot, after the cloud read of the options: "Cleared old KatanaOptions
 * and loading again"). It sets 0 after the original, so its defaults are off too.
 *
 * The only reader is obj_zoomblur Draw_64 (L63): the bloom pass runs when its surface exists and global.shaders,
 * global.app_surface, application_surface_is_enabled() and global.bloom_shader are all true, so bloom also needs
 * GRAPHICS -> SHADERS on (the game's default).
 */
#include <vitasdk.h>
#include <stdio.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

int bloom_default = 0; // bloomDefault in yyl.cfg: the default of global.bloom_shader (the game's is 1)
static int *var_bloom_shader;
static event_func orig_init_create, orig_init_user0;
static gml_script_fn orig_reset_game_data;
static int in_init_create;

static void bloom_off(void) {
	gml_rvalue *v = global_var(var_bloom_shader);
	if (v) {
		v->val = 0.0;
		v->kind = 0; // real, as the game writes it
	}
}

static void init_create(void *self, void *other) {
	in_init_create++;
	orig_init_create(self, other);
	in_init_create--;
}

static void init_user0(void *self, void *other) {
	if (in_init_create)
		bloom_off(); // after Create's defaults, before load_options
	orig_init_user0(self, other);
}

static void *reset_game_data(void *self, void *other, void *result, int argc, void **args) {
	void *r = orig_reset_game_data(self, other, result, argc, args);
	bloom_off();
	return r;
}

void bloom_default_install(gml_func_entry *table) {
	if (bloom_default)
		return;
	var_bloom_shader = (int *)so_symbol(&yoyoloader_mod, "g_VAR_bloom_shader");
	event_func orig;
	// The Create wrapper alone only counts; user event 0 is wrapped only if Create is
	if (var_bloom_shader && global_obj_ptr && (orig = gml_replace(table, "gml_Object_obj_init_Create_0", init_create))) {
		orig_init_create = orig;
		if ((orig = gml_replace(table, "gml_Object_obj_init_Other_10", init_user0)))
			orig_init_user0 = orig;
	}
	if (orig_init_user0)
		orig_reset_game_data = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z33gml_Script_mobile_reset_game_dataP9CInstanceS0_R8YYRValueiPPS1_"), reset_game_data);
	debugPrintf("bloom default: %s%s\n", orig_init_user0 ? "off" : "NOT installed",
		orig_init_user0 && !orig_reset_game_data ? " (NOT in mobile_reset_game_data)" : "");
}
