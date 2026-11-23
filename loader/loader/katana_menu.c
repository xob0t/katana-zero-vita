/*
 * katana_menu.c
 *
 * GRAPHICS menu rows (FPS MODE, ENEMY AI) and the yes/no prompt's starting selection.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <kubridge.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "lz4/lz4.h"
#include "dialog.h"

#include "katana_internal.h"

/*
 * FPS MODE in the game's own GRAPHICS options (NATIVE / 30 FPS / DYNAMIC), stored in the PC build's WINDOW ZOOM option.
 * The WINDOW ZOOM row is compiled out of the Android GRAPHICS page, but all of its code is still there:
 * - obj_optionsmenu Create copies global.zoom_mode to var_zoom_mode, Step cycles it 0 -> 1 -> 2 -> 3 -> 0 on a toggle
 *   press, Draw shows "1X ZOOM".."4X ZOOM" through translate_option;
 * - SAVE (Other_10) copies it back to global.zoom_mode, and save_options/load_options keep it in KatanaOptions.zero
 *   (an existing slot, so the file and its hash stay compatible);
 * - its only effect, set_windowsize() (Destroy, on a change), is an empty script on Android.
 * The loader adds the row after BLOOD SPRAY (add_pausemenu_choice, GRAPHICS page only), relabels its five strings
 * (translate_option) and limits it to three values (Step: 3 wraps to 1, not 0).
 * Precedence: zoom_mode 0 (every existing save, obj_init and "reset game data") means "not chosen" and follows fpsMode
 * (old name fps30) from yyl.cfg, shown under that mode's name; 1/2/3 are NATIVE/30 FPS/DYNAMIC chosen in the menu and override yyl.cfg.
 * fps_mode_sync reads global.zoom_mode once per frame (t30_update), so a SAVE applies on the next frame, CANCEL never.
 * exp_fpsmenu0 turns the row off (yyl.cfg only); exp_t30force keeps 30 ticks/s everywhere and skips the menu too.
 *
 * ENEMY AI (FULL RATE / HALF RATE: half-rate enemies, katana_t30.c), added after FPS MODE, is the PC build's
 * FULLSCREEN row with its value stored in the PC build's LOW-END OPTIONS flag:
 * - FULLSCREEN has a working row: Step toggles var_fullscreen = !var_fullscreen, Draw shows the game's "ON"/"OFF" for
 *   it. global.fullscreen is no use as storage: obj_miscmaster Room Start sets it to window_get_fullscreen() (always
 *   true on Android), and Destroy runs window_set_fullscreen and display_reset when it changes.
 * - global.show_crap_settings is saved and loaded (KatanaOptions.zero), copied to var_show_crap_settings by Create and
 *   back by SAVE, and set false by obj_init and the reset scripts. Nothing else reads it: the LOW-END OPTIONS popup
 *   that sets it is compiled out on Android.
 * var_fullscreen only carries the shown value through Step and Draw (put back after each), and the choice is in
 * var_show_crap_settings: 0 (every existing save) follows enemyHalfRate from yyl.cfg, shown under that value's name;
 * 1 FULL RATE and 2 HALF RATE are chosen in the menu and override yyl.cfg. global.fullscreen never changes, so Destroy
 * does nothing for it. Draw keeps its row index in the instance variable c: the label is translate_option(choice[c])
 * and the toggle value translate_option("ON" or "OFF") with c unchanged, so an ON/OFF asked for at the c that drew
 * "FULLSCREEN" is this row's and is relabelled. fps_mode_sync reads global.show_crap_settings once per frame, so a SAVE
 * applies on the next frame.
 *
 * Saves from versions up to 1.1.1 may hold 1 or 2 in the PC build's VSYNC option (global.vsync): the TEXT SMOOTHING
 * row (removed) stored its choice there. The game passes it to display_reset(aa, vsync) from load_options at boot; F_DisplayReset stores it
 * in g_Sync (the runner's frame limiter, MainLoop_Process: g_SyncInterval). The display_reset guard keeps that boot call
 * with vsync false, what every save had before (g_Sync 1 -> 0, one GR_D3D_Reset before the first room), and drops the
 * calls obj_optionsmenu Destroy makes, which would free every surface the next frame.
 */
static int fps30_cfg = -1; // fpsMode from yyl.cfg, kept once fps30 follows the menu (-1: the menu isn't installed)
static int enemy_half_cfg = -1; // enemyHalfRate from yyl.cfg, the same for enemy_half_rate
static int *var_show_crap_settings, *var_var_show_crap_settings, *var_var_fullscreen;
static int *var_zoom_mode, *var_var_zoom_mode;
static gml_script_fn orig_add_pausemenu_choice, orig_translate_option;
static event_func orig_optionsmenu_step;
static int fps_row, enemy_row; // the rows the menu shows
static int *var_c;
static event_func orig_optionsmenu_draw;
static void *menu_draw_self; // the obj_optionsmenu running Draw
static double enemy_label_c = -1; // its c when it asked for the "FULLSCREEN" label
static builtin_fn orig_display_reset;
static void (*yy_create_string)(gml_rvalue *, const char *);
static void (*yy_free_rvalue)(gml_rvalue *);

static const char *rv_string(const gml_rvalue *v) {
	ref_t *ref = v && (v->kind & 0xffffff) == VALUE_STRING ? *(ref_t **)v : NULL;
	return ref ? ref->m_thing : NULL;
}

// YYC frees only strings, arrays and objects (kinds 1, 2, 6)
static void rv_free(gml_rvalue *v) {
	if ((1u << (v->kind & 31)) & 0x46)
		yy_free_rvalue(v);
}

static void *fps_add_pausemenu_choice(void *self, void *other, void *result, int argc, void **args) {
	void *r = orig_add_pausemenu_choice(self, other, result, argc, args);
	const char *key = argc == 2 ? rv_string(args[0]) : NULL;
	if (key && !strcmp(key, "BLOOD SPRAY")) { // the GRAPHICS page only (obj_options_main_menu Step)
		static const char *const rows[2] = { "WINDOW ZOOM", "FULLSCREEN" };
		const int shown[2] = { fps_row, enemy_row };
		void *toggle = args[1]; // this call's "toggle"
		for (int i = 0; i < 2; i++) {
			if (!shown[i])
				continue;
			gml_rvalue name = {0}, res = {0};
			yy_create_string(&name, rows[i]);
			void *a[2] = { &name, toggle };
			orig_add_pausemenu_choice(self, other, &res, 2, a);
			rv_free(&name);
			rv_free(&res);
		}
	}
	return r;
}

static const char *const fps_mode_name[3] = { "NATIVE", "DYNAMIC", "30 FPS" }; // by fps30 value

// A real or bool instance variable (var_fullscreen is a bool once Step has toggled it)
static gml_rvalue *instance_num(void *self, int *var) {
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)self)[3];
	gml_rvalue *v = var ? vwrite(self, var[1]) : NULL;
	return v && (v->kind == VALUE_REAL || v->kind == VALUE_BOOL) ? v : NULL;
}

// ENEMY AI's stored choice -> half-rate on: 1 FULL RATE, 2 HALF RATE, anything else (0: not chosen) yyl.cfg's
static int enemy_half_of(double v) {
	return v == 1.0 ? 0 : v == 2.0 ? 1 : enemy_half_cfg > 0;
}

// obj_optionsmenu Draw's row index (only read inside its Draw, where c exists)
static double menu_draw_c(void *self) {
	gml_rvalue *c = instance_num(self, var_c);
	return c ? c->val : -2.0;
}

static void *fps_translate_option(void *self, void *other, void *result, int argc, void **args) {
	const char *key = argc >= 1 ? rv_string(args[0]) : NULL, *text = NULL;
	if (!key)
		return orig_translate_option(self, other, result, argc, args);
	if (!strcmp(key, "WINDOW ZOOM"))
		text = "FPS MODE";
	else if (!strcmp(key, "1X ZOOM")) // zoom_mode 0: not chosen, the yyl.cfg mode
		text = fps_mode_name[fps30_cfg >= 0 && fps30_cfg <= 2 ? fps30_cfg : 1];
	else if (!strcmp(key, "2X ZOOM"))
		text = "NATIVE";
	else if (!strcmp(key, "3X ZOOM"))
		text = "30 FPS";
	else if (!strcmp(key, "4X ZOOM"))
		text = "DYNAMIC";
	else if (enemy_row && !strcmp(key, "FULLSCREEN")) {
		text = "ENEMY AI";
		if (menu_draw_self && self == menu_draw_self)
			enemy_label_c = menu_draw_c(self);
	} else if (menu_draw_self && self == menu_draw_self && enemy_label_c >= 0.0 &&
			(!strcmp(key, "ON") || !strcmp(key, "OFF")) && menu_draw_c(self) == enemy_label_c) {
		text = key[1] == 'N' ? "HALF RATE" : "FULL RATE"; // var_fullscreen shows half-rate on (enemy_show)
	}
	if (!text)
		return orig_translate_option(self, other, result, argc, args);
	yy_create_string(result, text); // frees the old value first
	return result;
}

// var_fullscreen showing ENEMY AI's value (true: HALF RATE) through a Step or Draw, put back after
typedef struct { int set; double val; unsigned kind; } enemy_shown;

static enemy_shown enemy_show(void *self) {
	enemy_shown was = {0};
	gml_rvalue *e = enemy_row ? instance_num(self, var_var_show_crap_settings) : NULL;
	double stored = e ? e->val : 0.0; // copied before the next lookup
	gml_rvalue *f = e ? instance_num(self, var_var_fullscreen) : NULL;
	if (!f)
		return was;
	was = (enemy_shown){ 1, f->val, f->kind };
	f->val = enemy_half_of(stored) ? 1.0 : 0.0;
	return was;
}

static void enemy_unshow(void *self, enemy_shown was) {
	gml_rvalue *f = was.set ? instance_num(self, var_var_fullscreen) : NULL; // looked up again: events add variables
	if (f) {
		f->val = was.val;
		f->kind = was.kind;
	}
}

// - FPS MODE: three values; the game's toggle wraps 3 -> 0 ("not chosen"); make it 3 -> 1;
// - ENEMY AI: two values; the game's var_fullscreen = !var_fullscreen flips the shown value; store the other value (1 or 2).
static void fps_optionsmenu_step(void *self, void *other) {
	gml_rvalue *z = fps_row ? instance_var(self, var_var_zoom_mode) : NULL;
	double zoom = z ? z->val : -1;
	enemy_shown enemy = enemy_show(self);
	orig_optionsmenu_step(self, other);
	if (enemy.set) {
		gml_rvalue *e = instance_num(self, var_var_show_crap_settings);
		int shown = e ? enemy_half_of(e->val) : 0;
		gml_rvalue *f = e ? instance_num(self, var_var_fullscreen) : NULL;
		int flipped = f && (f->val != 0.0) != shown;
		enemy_unshow(self, enemy);
		if (flipped && (e = instance_num(self, var_var_show_crap_settings))) {
			e->val = shown ? 1.0 : 2.0;
			e->kind = VALUE_REAL;
		}
	}
	if (zoom == 3.0 && (z = instance_var(self, var_var_zoom_mode)) && z->val == 0.0)
		z->val = 1.0;
	// From 0 ("not chosen", shown as the yyl.cfg mode) the game steps to 1 (NATIVE), which with fpsMode 0 looks like
	// no change: step to the mode after the one shown instead (menu order NATIVE 1, 30 FPS 2, DYNAMIC 3)
	else if (zoom == 0.0 && (z = instance_var(self, var_var_zoom_mode)) && z->val == 1.0) {
		int shown = fps30_cfg == 0 ? 1 : fps30_cfg == 2 ? 2 : 3; // as fps_translate_option labels it
		z->val = shown % 3 + 1;
	}
}

// Once per frame (t30_update): the saved menu choices drive fps30 and enemy_half_rate
void fps_mode_sync(void) {
	if (!*global_obj_ptr)
		return;
	if (fps30_cfg >= 0) {
		gml_rvalue *z = global_var(var_zoom_mode);
		int m = z ? (int)z->val : 0;
		fps30 = m == 1 ? 0 : m == 2 ? 2 : m == 3 ? 1 : fps30_cfg;
	}
	if (enemy_half_cfg >= 0) {
		gml_rvalue *e = global_var(var_show_crap_settings);
		enemy_half_rate = enemy_half_of(e && (e->kind == VALUE_REAL || e->kind == VALUE_BOOL) ? e->val : 0.0);
	}
}

// ENEMY AI is drawn with var_fullscreen showing its value (enemy_show), and fps_translate_option relabels the value
// (the game's ON/OFF if it can't). Draw only reads var_fullscreen and runs no user event (SAVE), so the stored value
// never sees the change.
static void enemy_optionsmenu_draw(void *self, void *other) {
	enemy_shown enemy = enemy_show(self);
	void *prev_self = menu_draw_self;
	double prev_c = enemy_label_c;
	menu_draw_self = self;
	enemy_label_c = -1.0;
	orig_optionsmenu_draw(self, other);
	menu_draw_self = prev_self;
	enemy_label_c = prev_c;
	enemy_unshow(self, enemy);
}

// display_reset is forwarded only from load_options, with vsync false: what the boot call did with every save before
// TEXT SMOOTHING used the VSYNC slot (see the top of this file). The calls from obj_optionsmenu Destroy are dropped:
// they would free every surface the next frame. Nothing else calls it.
static gml_script_fn orig_load_options;
static int in_load_options;

static void *guard_load_options(void *self, void *other, void *result, int argc, void **args) {
	in_load_options++;
	void *r = orig_load_options(self, other, result, argc, args);
	in_load_options--;
	return r;
}

static void guarded_display_reset(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (!in_load_options) {
		debugPrintf("display_reset from the options menu: skipped\n");
		return;
	}
	if (argc < 2) {
		orig_display_reset(result, self, other, argc, args);
		return;
	}
	gml_rvalue a[2] = { args[0], { 0.0, 0, VALUE_REAL } }; // aa as given, vsync false
	orig_display_reset(result, self, other, 2, a);
}

// The FPS MODE and ENEMY AI rows, each installed only with everything it needs
void fps_menu_install(gml_func_entry *table) {
	int want_fps = !exp_flag("fpsmenu0") && !exp_flag("t30force") && t30_installed;
	int want_enemy = eh_installed;
	var_zoom_mode = (int *)so_symbol(&yoyoloader_mod, "g_VAR_zoom_mode");
	var_var_zoom_mode = (int *)so_symbol(&yoyoloader_mod, "g_VAR_var_zoom_mode");
	var_c = (int *)so_symbol(&yoyoloader_mod, "g_VAR_c");
	var_show_crap_settings = (int *)so_symbol(&yoyoloader_mod, "g_VAR_show_crap_settings");
	var_var_show_crap_settings = (int *)so_symbol(&yoyoloader_mod, "g_VAR_var_show_crap_settings");
	var_var_fullscreen = (int *)so_symbol(&yoyoloader_mod, "g_VAR_var_fullscreen");
	yy_create_string = (void *)so_symbol(&yoyoloader_mod, "_Z14YYCreateStringP6RValuePKc");
	yy_free_rvalue = (void *)so_symbol(&yoyoloader_mod, "_Z16FREE_RValue__PreP6RValue");

	// The display_reset guard whatever the menu: it keeps the boot call as it was before for any saved vsync.
	// load_options first, so the guard never sees the boot call without knowing where it comes from.
	orig_load_options = entry_hook(so_symbol(&yoyoloader_mod,
		"_Z23gml_Script_load_optionsP9CInstanceS0_R8YYRValueiPPS1_"), guard_load_options);
	if (orig_load_options)
		orig_display_reset = entry_hook(so_symbol(&yoyoloader_mod, "_Z14F_DisplayResetR6RValueP9CInstanceS2_iPS_"),
			guarded_display_reset);
	debugPrintf("display_reset guard: %s\n", orig_display_reset ? "on" : "NOT installed");

	if ((want_fps || want_enemy) && yy_create_string && yy_free_rvalue)
		orig_translate_option = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z27gml_Script_translate_optionP9CInstanceS0_R8YYRValueiPPS1_"), fps_translate_option);
	// In this order, so a row is never added without its labels and its Step (and Draw) hook
	int fps_ok = want_fps && orig_translate_option && var_zoom_mode && var_var_zoom_mode;
	int enemy_ok = want_enemy && orig_translate_option && var_show_crap_settings && var_var_show_crap_settings &&
		var_var_fullscreen && var_c;
	if (fps_ok || enemy_ok)
		orig_optionsmenu_step = gml_replace(table, "gml_Object_obj_optionsmenu_Step_0", fps_optionsmenu_step);
	if (enemy_ok && orig_optionsmenu_step)
		orig_optionsmenu_draw = gml_replace(table, "gml_Object_obj_optionsmenu_Draw_0", enemy_optionsmenu_draw);
	if ((fps_ok || orig_optionsmenu_draw) && orig_optionsmenu_step)
		orig_add_pausemenu_choice = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z31gml_Script_add_pausemenu_choiceP9CInstanceS0_R8YYRValueiPPS1_"), fps_add_pausemenu_choice);
	if (orig_add_pausemenu_choice) {
		fps_row = fps_ok;
		enemy_row = enemy_ok && orig_optionsmenu_draw;
	}
	if (fps_row)
		fps30_cfg = fps30; // from here on fps_mode_sync owns fps30
	if (enemy_row)
		enemy_half_cfg = enemy_half_rate; // and enemy_half_rate
	debugPrintf("FPS mode menu: %s (yyl.cfg fpsMode %d)\n", fps_row ? "on" : want_fps ? "NOT installed" : "off", fps30);
	debugPrintf("enemy AI menu: %s (yyl.cfg enemyHalfRate %d)\n", enemy_row ? "on" : want_enemy ? "NOT installed" : "off",
		enemy_half_rate);
}

// obj_interaction is the yes/no prompt behind interact_binary (pause, title and level-select confirms, interactable
// objects). Its Create ends with selection = -1 for touch: nothing is highlighted until a d-pad press, and the first
// press only reaches choice 0. Start on defaultSelect like the PC build; confirming with no selection already picked
// defaultSelect (Step), so only the starting highlight changes. Dialogue choices (obj_choicebox) start at 0 already.
static event_func orig_interaction_create;
static int *var_selection, *var_default_select;

static void interaction_create_preselect(void *self, void *other) {
	orig_interaction_create(self, other);
	gml_rvalue *def = instance_var(self, var_default_select);
	double d = def ? def->val : 0.0;  // copy before the next lookup
	gml_rvalue *sel = instance_var(self, var_selection);
	if (sel && sel->val == -1.0)
		sel->val = d;
}

// Called from patch_performance: the prompt preselect
void katana_menu_install(gml_func_entry *table) {
	event_func orig;
	var_selection = (int *)so_symbol(&yoyoloader_mod, "g_VAR_selection");
	var_default_select = (int *)so_symbol(&yoyoloader_mod, "g_VAR_defaultSelect");
	if (var_selection && var_default_select && !exp_flag("touchui") &&
		(orig = gml_replace(table, "gml_Object_obj_interaction_Create_0", interaction_create_preselect)))
		orig_interaction_create = orig;
	debugPrintf("prompts: default choice highlight %s\n", orig_interaction_create ? "on" : "NOT hooked");
}
