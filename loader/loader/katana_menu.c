/*
 * katana_menu.c
 *
 * GAME menu rows ENEMY AI and ENEMY TIMING, and the yes/no prompt's starting selection.
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
 * The rows go on the GAME page after its last gameplay row, SLOWDOWN CONTROL. obj_options_main_menu Step builds each
 * page in an obj_optionsmenu instance (add_pausemenu_choice: SCREENSHAKE, HITPAUSE, RESPONSE TIME, TEXT SPEED,
 * SKIP REWIND, SLOWDOWN CONTROL, SAVE, CANCEL on GAME); only that unit passes "SLOWDOWN CONTROL" to
 * add_pausemenu_choice. obj_optionsmenu Step, Draw and SAVE are the same for every page: Step and SAVE never read
 * menu_title, and Draw reads it only for the title and the layout.
 *
 * ENEMY AI (FULL RATE / HALF RATE: half-rate enemies, enemy_half_rate.c) is the PC build's FULLSCREEN row with its
 * value stored in the PC build's LOW-END OPTIONS flag:
 * - FULLSCREEN has a working row: Step toggles var_fullscreen = !var_fullscreen, Draw shows the game's "ON"/"OFF" for
 *   it. global.fullscreen is no use as storage: obj_miscmaster Room Start sets it to window_get_fullscreen() (always
 *   true on Android), and Destroy runs window_set_fullscreen and display_reset when it changes.
 * - global.show_crap_settings is saved and loaded (KatanaOptions.zero), copied to var_show_crap_settings by Create and
 *   back by SAVE, and set false by obj_init and the reset scripts. Nothing else reads it: the LOW-END OPTIONS popup
 *   that sets it is compiled out on Android.
 * var_fullscreen only carries the shown value through Step and Draw (put back after each, and around SAVE, which
 * Step can run and which copies var_fullscreen to global.fullscreen), and the choice is in
 * var_show_crap_settings: 0 (every existing save) follows enemyHalfRate from yyl.cfg, shown under that value's name;
 * 1 FULL RATE and 2 HALF RATE are chosen in the menu and override yyl.cfg. global.fullscreen never changes, so Destroy
 * does nothing for it. Draw keeps its row index in the instance variable c: the label is translate_option(choice[c])
 * and the toggle value translate_option("ON" or "OFF") with c unchanged, so an ON/OFF asked for at the c that drew
 * "FULLSCREEN" is this row's and is relabelled. options_menu_sync reads global.show_crap_settings once per frame, so a
 * SAVE applies on the next frame.
 *
 * ENEMY TIMING (PC / ANDROID: enemy_timing.c), added after ENEMY AI, is the PC build's WINDOW ZOOM row with its value
 * in the PC build's WINDOW ZOOM option, global.zoom_mode. That row is compiled out of the Android GRAPHICS page, but
 * its code is all there: obj_optionsmenu Create copies global.zoom_mode to var_zoom_mode, Step steps it on a toggle
 * press (0 -> 1 -> 2 -> 3 -> 0), Draw shows "1X ZOOM".."4X ZOOM" for it through translate_option, SAVE copies it back,
 * and save_options/load_options keep it in KatanaOptions.zero. Its only effect, set_windowsize() (Destroy, on a
 * change), is an empty script on Android, and nothing else reads global.zoom_mode (obj_init and the reset scripts set
 * it to 0). It held the removed FPS MODE row's choice (1-3) until Nov 2026, so saves may hold 0-3 there: all of these
 * mean "not chosen" and follow enemyTiming from yyl.cfg (0, PC, by default). The menu stores 4 (PC) or 5 (ANDROID).
 * var_zoom_mode shows 1 ("2X ZOOM", relabelled PC) or 2 ("3X ZOOM", ANDROID) through Step and Draw and holds the stored
 * value otherwise (put back after each, and around SAVE); any change Step makes to the shown value picks the other
 * choice. options_menu_sync reads global.zoom_mode once per frame, and AI_init applies the timing to enemies created
 * after that: the next room, or a restart.
 *
 * Saves from versions up to 1.1.1 may hold 1 or 2 in the PC build's VSYNC option (global.vsync): the TEXT SMOOTHING
 * row (removed) stored its choice there. The game passes it to display_reset(aa, vsync) from load_options at boot; F_DisplayReset stores it
 * in g_Sync (the runner's frame limiter, MainLoop_Process: g_SyncInterval). The display_reset guard keeps that boot call
 * with vsync false, what every save had before (g_Sync 1 -> 0, one GR_D3D_Reset before the first room), and drops the
 * calls obj_optionsmenu Destroy makes, which would free every surface the next frame.
 */
static int enemy_half_cfg = -1; // enemyHalfRate from yyl.cfg, kept once enemy_half_rate follows the menu (-1: no row)
static int *var_show_crap_settings, *var_var_show_crap_settings, *var_var_fullscreen;
static gml_script_fn orig_add_pausemenu_choice, orig_translate_option;
static event_func orig_optionsmenu_step;
static int enemy_row; // the menu shows ENEMY AI
static int *var_c;
static event_func orig_optionsmenu_draw;
static void *menu_draw_self; // the obj_optionsmenu running Draw
static double enemy_label_c = -1; // its c when it asked for the "FULLSCREEN" label
static builtin_fn orig_display_reset;
static void (*yy_create_string)(gml_rvalue *, const char *);
static void (*yy_free_rvalue)(gml_rvalue *);
static int timing_row; // the menu shows ENEMY TIMING
static int *var_zoom_mode, *var_var_zoom_mode;
extern int enemy_timing_cfg, enemy_timing_android; // enemy_timing.c
int enemy_timing_available(void);

// ENEMY TIMING's stored value -> ANDROID timing: 4 PC, 5 ANDROID, anything else (not chosen) yyl.cfg's enemyTiming
static int timing_android_of(double v) {
	return v == 4.0 ? 0 : v == 5.0 ? 1 : enemy_timing_cfg != 0;
}

static const char *rv_string(const gml_rvalue *v) {
	ref_t *ref = v && (v->kind & 0xffffff) == VALUE_STRING ? *(ref_t **)v : NULL;
	return ref ? ref->m_thing : NULL;
}

// YYC frees only strings, arrays and objects (kinds 1, 2, 6)
static void rv_free(gml_rvalue *v) {
	if ((1u << (v->kind & 31)) & 0x46)
		yy_free_rvalue(v);
}

static void *menu_add_pausemenu_choice(void *self, void *other, void *result, int argc, void **args) {
	void *r = orig_add_pausemenu_choice(self, other, result, argc, args);
	const char *key = argc == 2 ? rv_string(args[0]) : NULL;
	if (key && !strcmp(key, "SLOWDOWN CONTROL")) { // the GAME page only (obj_options_main_menu Step)
		extern int eh_available(void);
		static const char *const rows[2] = { "FULLSCREEN", "WINDOW ZOOM" }; // ENEMY AI, ENEMY TIMING
		const int shown[2] = { enemy_row && eh_available(), timing_row };
		for (int i = 0; i < 2; i++) {
			if (!shown[i])
				continue;
			gml_rvalue name = {0}, res = {0};
			yy_create_string(&name, rows[i]);
			void *a[2] = { &name, args[1] }; // this call's "toggle"
			orig_add_pausemenu_choice(self, other, &res, 2, a);
			rv_free(&name);
			rv_free(&res);
		}
	}
	return r;
}

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

static void *menu_translate_option(void *self, void *other, void *result, int argc, void **args) {
	const char *key = argc >= 1 ? rv_string(args[0]) : NULL, *text = NULL;
	if (!key)
		return orig_translate_option(self, other, result, argc, args);
	if (timing_row && !strcmp(key, "WINDOW ZOOM"))
		text = "ENEMY TIMING";
	else if (timing_row && strstr(key, "X ZOOM") && strlen(key) == 7) // "1X ZOOM".."4X ZOOM": var_zoom_mode shows 1 or 2
		text = key[0] == '3' || key[0] == '4' ? "ANDROID" : "PC";
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

// var_zoom_mode showing ENEMY TIMING's choice (1 PC, 2 ANDROID) through a Step or Draw; the stored value goes back
// after, or the other choice (4/5) if the shown value changed
typedef struct { int set; double stored, shown; unsigned kind; } timing_shown;

static timing_shown timing_show(void *self) {
	timing_shown was = {0};
	gml_rvalue *z = timing_row ? instance_num(self, var_var_zoom_mode) : NULL;
	if (!z)
		return was;
	was = (timing_shown){ 1, z->val, timing_android_of(z->val) ? 2.0 : 1.0, z->kind };
	z->val = was.shown;
	z->kind = VALUE_REAL;
	return was;
}

// The value to store now: the other choice if the shown value was changed (a toggle), else the one stored before
static double timing_stored(const timing_shown *was, const gml_rvalue *z) {
	if (z->val == was->shown)
		return was->stored;
	return was->shown == 2.0 ? 4.0 : 5.0;
}

static void timing_unshow(void *self, timing_shown was) {
	gml_rvalue *z = was.set ? instance_num(self, var_var_zoom_mode) : NULL; // looked up again: events add variables
	if (!z)
		return;
	double v = timing_stored(&was, z);
	z->kind = v == was.stored ? was.kind : VALUE_REAL;
	z->val = v;
}

// The obj_optionsmenu whose Step runs with var_fullscreen showing ENEMY AI and var_zoom_mode showing ENEMY TIMING,
// and what they held before
static void *step_show_self;
static enemy_shown step_show;
static timing_shown step_timing;
static event_func orig_optionsmenu_save;

// SAVE (user event 0, run by Step on the SAVE choice) copies var_fullscreen to global.fullscreen: it runs with the
// real value, and the shown one (or the toggle's flip of it) is put back after for the rest of Step
static void enemy_optionsmenu_save(void *self, void *other) {
	gml_rvalue *f = step_show_self == self && step_show.set ? instance_num(self, var_var_fullscreen) : NULL;
	double shown = 0.0;
	unsigned shown_kind = 0;
	if (f) {
		shown = f->val;
		shown_kind = f->kind;
		f->val = step_show.val;
		f->kind = step_show.kind;
	}
	// ENEMY TIMING the same way: SAVE copies var_zoom_mode to global.zoom_mode, so it gets the value to store
	gml_rvalue *z = step_show_self == self && step_timing.set ? instance_num(self, var_var_zoom_mode) : NULL;
	double zoom_shown = 0.0;
	if (z) {
		zoom_shown = z->val;
		z->val = timing_stored(&step_timing, z);
		z->kind = VALUE_REAL;
	}
	orig_optionsmenu_save(self, other);
	if (f && (f = instance_num(self, var_var_fullscreen))) { // looked up again: events add variables
		f->val = shown;
		f->kind = shown_kind;
	}
	if (z && (z = instance_num(self, var_var_zoom_mode))) {
		z->val = zoom_shown;
		z->kind = VALUE_REAL;
	}
}

// ENEMY AI: two values; the game's var_fullscreen = !var_fullscreen flips the shown value; store the other value (1 or 2).
static void enemy_optionsmenu_step(void *self, void *other) {
	enemy_shown enemy = enemy_show(self);
	timing_shown timing = timing_show(self);
	void *prev_self = step_show_self;
	enemy_shown prev_show = step_show;
	timing_shown prev_timing = step_timing;
	step_show_self = self;
	step_show = enemy;
	step_timing = timing;
	orig_optionsmenu_step(self, other);
	step_show_self = prev_self;
	step_show = prev_show;
	step_timing = prev_timing;
	timing_unshow(self, timing);
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
}

// Once per frame (katana_after_swap): the saved menu choice drives enemy_half_rate
void options_menu_sync(void) {
	if (!global_obj_ptr || !*global_obj_ptr)
		return;
	if (enemy_half_cfg >= 0) {
		gml_rvalue *e = global_var(var_show_crap_settings);
		enemy_half_rate = enemy_half_of(e && (e->kind == VALUE_REAL || e->kind == VALUE_BOOL) ? e->val : 0.0);
	}
	if (timing_row) {
		gml_rvalue *z = global_var(var_zoom_mode);
		enemy_timing_android = timing_android_of(z && z->kind == VALUE_REAL ? z->val : 0.0);
	}
}

// ENEMY AI is drawn with var_fullscreen showing its value (enemy_show), and fps_translate_option relabels the value
// (the game's ON/OFF if it can't). Draw only reads var_fullscreen and runs no user event (SAVE), so the stored value
// never sees the change.
static void enemy_optionsmenu_draw(void *self, void *other) {
	enemy_shown enemy = enemy_show(self);
	timing_shown timing = timing_show(self);
	void *prev_self = menu_draw_self;
	double prev_c = enemy_label_c;
	menu_draw_self = self;
	enemy_label_c = -1.0;
	orig_optionsmenu_draw(self, other);
	menu_draw_self = prev_self;
	enemy_label_c = prev_c;
	enemy_unshow(self, enemy);
	timing_unshow(self, timing); // Draw doesn't change var_zoom_mode: puts the stored value back
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

// The display_reset guard, and the ENEMY AI row, installed only with everything it needs
void options_menu_install(gml_func_entry *table) {
	int want_enemy = eh_installed;
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

	int want_timing = enemy_timing_available();
	var_zoom_mode = (int *)so_symbol(&yoyoloader_mod, "g_VAR_zoom_mode");
	var_var_zoom_mode = (int *)so_symbol(&yoyoloader_mod, "g_VAR_var_zoom_mode");
	if ((want_enemy || want_timing) && yy_create_string && yy_free_rvalue)
		orig_translate_option = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z27gml_Script_translate_optionP9CInstanceS0_R8YYRValueiPPS1_"), menu_translate_option);
	// In this order, so a row is never added without its labels and its Step, Draw and SAVE hooks
	int enemy_ok = want_enemy && orig_translate_option && var_show_crap_settings && var_var_show_crap_settings &&
		var_var_fullscreen && var_c;
	int timing_ok = want_timing && orig_translate_option && var_zoom_mode && var_var_zoom_mode;
	// Both rows need SAVE to see the values to store
	if (enemy_ok || timing_ok)
		orig_optionsmenu_save = gml_replace(table, "gml_Object_obj_optionsmenu_Other_10", enemy_optionsmenu_save);
	if (orig_optionsmenu_save)
		orig_optionsmenu_step = gml_replace(table, "gml_Object_obj_optionsmenu_Step_0", enemy_optionsmenu_step);
	if (orig_optionsmenu_step)
		orig_optionsmenu_draw = gml_replace(table, "gml_Object_obj_optionsmenu_Draw_0", enemy_optionsmenu_draw);
	if (orig_optionsmenu_draw)
		orig_add_pausemenu_choice = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z31gml_Script_add_pausemenu_choiceP9CInstanceS0_R8YYRValueiPPS1_"), menu_add_pausemenu_choice);
	enemy_row = enemy_ok && orig_add_pausemenu_choice;
	timing_row = timing_ok && orig_add_pausemenu_choice;
	if (enemy_row)
		enemy_half_cfg = enemy_half_rate; // from here on options_menu_sync owns enemy_half_rate
	debugPrintf("enemy AI menu: %s (yyl.cfg enemyHalfRate %d)\n", enemy_row ? "on" : want_enemy ? "NOT installed" : "off",
		enemy_half_rate);
	debugPrintf("enemy timing menu: %s (yyl.cfg enemyTiming %d)\n", timing_row ? "on" : want_timing ? "NOT installed" :
		"off", enemy_timing_cfg);
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
	extern int touch_controls;
	if (var_selection && var_default_select && touch_controls < 2 &&
		(orig = gml_replace(table, "gml_Object_obj_interaction_Create_0", interaction_create_preselect)))
		orig_interaction_create = orig;
	debugPrintf("prompts: default choice highlight %s\n", orig_interaction_create ? "on" : "NOT hooked");
}
