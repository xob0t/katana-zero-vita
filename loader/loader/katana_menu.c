/*
 * katana_menu.c
 *
 * GAME menu rows ENEMY AI, ENEMY TIMING and TIMER & SLOW-MO, the options menu without its touch controls page, and
 * the yes/no prompt's starting selection.
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
 * after that: the next room, or a restart (the slash, smoke and death slow-mo rules of pc_rules.c: the next frame).
 *
 * TIMER & SLOW-MO (PC / ANDROID: timer_slowmo.c), added after ENEMY TIMING, is the PC build's VSYNC row with its value
 * in the PC build's VSYNC option, global.vsync, the same way. Its code is there too: Create copies global.vsync to
 * var_vsync and prev_var_vsync, Step toggles var_vsync = !BOOL(var_vsync), Draw shows the game's "ON"/"OFF" for it,
 * SAVE copies it back and save_options/load_options keep it (load_variable returns file_text_read_real as it is: no
 * clamp, no bool). Its only effects are display_reset calls (below), which the guard keeps as they were; nothing else
 * reads global.vsync (obj_init and the reset scripts set it to false). Saves hold 0 or, from TEXT SMOOTHING (below),
 * 1 or 2: all of these mean "not chosen" and follow timerSlowmo from yyl.cfg (0, PC, by default). The menu stores 4
 * (PC) or 5 (ANDROID). var_vsync shows false (PC) or true (ANDROID) through Step and Draw, any change Step makes picks
 * the other choice, and the game's OFF/ON at the row's c (as for ENEMY AI) is relabelled PC/ANDROID.
 * options_menu_sync reads global.vsync once per frame and rewrites the constants; the game uses them the next time it
 * sets the values (the time limit at the next room or restart, the slow-mo meter at the next spawn).
 *
 * Saves from versions up to 1.1.1 may hold 1 or 2 in the PC build's VSYNC option (global.vsync): the TEXT SMOOTHING
 * row (removed) stored its choice there. The game passes it to display_reset(aa, vsync) from load_options at boot; F_DisplayReset stores it
 * in g_Sync (the runner's frame limiter, MainLoop_Process: g_SyncInterval). The display_reset guard keeps that boot call
 * with vsync false, what every save had before (g_Sync 1 -> 0, one GR_D3D_Reset before the first room), and drops the
 * calls obj_optionsmenu Destroy makes (when global.vsync != prev_var_vsync), which would free every surface the next
 * frame. TIMER & SLOW-MO is installed only with the guard.
 *
 * CONTROLS (touchControls=0): the options main menu (obj_options_main_menu Create: GAME, CONTROLS, GRAPHICS, AUDIO,
 * BACK; the title's OPTIONS and the pause menu's both create it) has no CONTROLS row. On Android that row opens the
 * touch controls page and nothing else (Step line 173: "CONTROLS" or "GAMEPAD CONTROLS" ->
 * create_mobile_controls_menu_options): JOYSTICK, INTERACT BUTTON, CUTSCENE BUTTON, LEFT HANDED MODE, EDIT BUTTONS (the
 * drag-to-place screen, obj_mobile_control_menu), RESET TO DEFAULT, SAVE, CANCEL. The values (global.mobile_joystick_on,
 * mobile_interaction_button_in_game / _in_cutscene, mobile_joystick_leftHanded, the button positions and sizes) are
 * read by the touch controls (obj_onScreenKeyboard, obj_screenBtn, obj_screenJoystick) and the tutorials' touch art;
 * the gamepad bindings page (create_controls_menu) is reached only from "KEYBOARD CONTROLS", which Android never adds.
 * The row is left out where it is added: add_pausemenu_choice("CONTROLS", "end") is not run, so max_choice counts four
 * rows and Up/Down wrap over them. Step picks a page by the row's string (choice[current_choice]), not its index, so
 * GAME, GRAPHICS, AUDIO and BACK work as before. Only Create passes "CONTROLS" to add_pausemenu_choice
 * (obj_mobile_control_menu Step and the main menu's Step use it as menu_title only). Every other page's
 * obj_optionsmenu still copies the touch values in Create and back in SAVE, and save_options writes them, so they
 * round-trip unchanged. touchControls 1 or 2 keep the row, and exp_touchmenu0 too.
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
extern int enemy_timing_cfg, enemy_timing_android; // enemy_timing.c
int enemy_timing_available(void);
extern int timer_slowmo_cfg; // timer_slowmo.c
int timer_slowmo_available(void);
void timer_slowmo_apply(int android);
static int timer_slowmo_android;

// ENEMY TIMING and TIMER & SLOW-MO: a PC / ANDROID choice kept in a PC option the Android build doesn't use
typedef struct {
	const char *option;         // the PC row's add_pausemenu_choice key
	int *var_global, *var_menu; // global.<option> (saved) and obj_optionsmenu's var_<option>
	double shown[2];            // var_<option> through Step and Draw: [0] PC, [1] ANDROID
	const int *cfg;             // yyl.cfg's choice while the menu has none (nonzero: ANDROID)
	int *android;               // what applies now (options_menu_sync)
	int row;                    // the menu shows it
} choice_row;
enum { ROW_TIMING, ROW_TIMER, NCHOICE };
static choice_row choices[NCHOICE] = {
	{ "WINDOW ZOOM", NULL, NULL, { 1.0, 2.0 }, &enemy_timing_cfg, &enemy_timing_android, 0 },
	{ "VSYNC", NULL, NULL, { 0.0, 1.0 }, &timer_slowmo_cfg, &timer_slowmo_android, 0 },
};
static double timer_label_c = -1; // the Draw's c when it asked for the "VSYNC" label

// A stored choice -> ANDROID: 4 PC, 5 ANDROID, anything else (not chosen) yyl.cfg's
static int android_of(const choice_row *r, double v) {
	return v == 4.0 ? 0 : v == 5.0 ? 1 : *r->cfg != 0;
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

static int hide_touch_page; // the options main menu without CONTROLS (the touch controls page)

static void *menu_add_pausemenu_choice(void *self, void *other, void *result, int argc, void **args) {
	const char *key = argc == 2 ? rv_string(args[0]) : NULL;
	if (hide_touch_page && key && !strcmp(key, "CONTROLS")) { // obj_options_main_menu Create only
		debugPrintf("options menu: CONTROLS (touch controls page) left out\n");
		return result; // the script returns nothing; the caller's result stays as it set it
	}
	void *r = orig_add_pausemenu_choice(self, other, result, argc, args);
	if (key && !strcmp(key, "SLOWDOWN CONTROL")) { // the GAME page only (obj_options_main_menu Step)
		extern int eh_available(void);
		// ENEMY AI, ENEMY TIMING, TIMER & SLOW-MO
		const char *const rows[3] = { "FULLSCREEN", choices[ROW_TIMING].option, choices[ROW_TIMER].option };
		const int shown[3] = { enemy_row && eh_available(), choices[ROW_TIMING].row, choices[ROW_TIMER].row };
		for (int i = 0; i < 3; i++) {
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
	int in_draw = menu_draw_self && self == menu_draw_self;
	if (choices[ROW_TIMING].row && !strcmp(key, "WINDOW ZOOM"))
		text = "ENEMY TIMING";
	else if (choices[ROW_TIMING].row && strstr(key, "X ZOOM") && strlen(key) == 7) // "1X ZOOM".."4X ZOOM": shows 1 or 2
		text = key[0] == '3' || key[0] == '4' ? "ANDROID" : "PC";
	else if (choices[ROW_TIMER].row && !strcmp(key, "VSYNC")) {
		text = "TIMER & SLOW-MO";
		if (in_draw)
			timer_label_c = menu_draw_c(self);
	} else if (enemy_row && !strcmp(key, "FULLSCREEN")) {
		text = "ENEMY AI";
		if (in_draw)
			enemy_label_c = menu_draw_c(self);
	} else if (in_draw && (enemy_label_c >= 0.0 || timer_label_c >= 0.0) &&
			(!strcmp(key, "ON") || !strcmp(key, "OFF"))) {
		double c = menu_draw_c(self);
		if (c == enemy_label_c)
			text = key[1] == 'N' ? "HALF RATE" : "FULL RATE"; // var_fullscreen shows half-rate on (enemy_show)
		else if (c == timer_label_c)
			text = key[1] == 'N' ? "ANDROID" : "PC"; // var_vsync shows true for ANDROID (choice_show)
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

// The choice rows' var_<option> showing their choices (shown[0] PC, shown[1] ANDROID) through a Step or Draw; the
// stored value goes back after, or the other choice (4/5) if the shown value changed
typedef struct { int set, android; double stored; unsigned kind; } choice_shown;
typedef struct { choice_shown row[NCHOICE]; } choices_shown;

static choices_shown choice_show(void *self) {
	choices_shown was = {0};
	for (int i = 0; i < NCHOICE; i++) {
		const choice_row *r = &choices[i];
		gml_rvalue *z = r->row ? instance_num(self, r->var_menu) : NULL;
		if (!z)
			continue;
		was.row[i] = (choice_shown){ 1, android_of(r, z->val), z->val, z->kind };
		z->val = r->shown[was.row[i].android];
		z->kind = VALUE_REAL;
	}
	return was;
}

// The value to store now: the other choice if the shown value was changed (a toggle), else the one stored before
static double choice_stored(int i, const choice_shown *was, const gml_rvalue *z) {
	if (z->val == choices[i].shown[was->android])
		return was->stored;
	return was->android ? 4.0 : 5.0;
}

static void choice_unshow(void *self, const choices_shown *was) {
	for (int i = 0; i < NCHOICE; i++) {
		// looked up again: events add variables
		gml_rvalue *z = was->row[i].set ? instance_num(self, choices[i].var_menu) : NULL;
		if (!z)
			continue;
		double v = choice_stored(i, &was->row[i], z);
		z->kind = v == was->row[i].stored ? was->row[i].kind : VALUE_REAL;
		z->val = v;
	}
}

// The obj_optionsmenu whose Step runs with var_fullscreen showing ENEMY AI and the choice rows' variables showing
// their choices, and what they held before
static void *step_show_self;
static enemy_shown step_show;
static choices_shown step_choices;
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
	// The choice rows the same way: SAVE copies var_<option> to global.<option>, so it gets the value to store
	double choice_was[NCHOICE];
	unsigned choice_kind[NCHOICE];
	int choice_set[NCHOICE];
	for (int i = 0; i < NCHOICE; i++) {
		gml_rvalue *z = step_show_self == self && step_choices.row[i].set ? instance_num(self, choices[i].var_menu) : NULL;
		choice_set[i] = z != NULL;
		if (z) {
			choice_was[i] = z->val;
			choice_kind[i] = z->kind;
			z->val = choice_stored(i, &step_choices.row[i], z);
			z->kind = VALUE_REAL;
		}
	}
	orig_optionsmenu_save(self, other);
	if (f && (f = instance_num(self, var_var_fullscreen))) { // looked up again: events add variables
		f->val = shown;
		f->kind = shown_kind;
	}
	for (int i = 0; i < NCHOICE; i++) {
		gml_rvalue *z = choice_set[i] ? instance_num(self, choices[i].var_menu) : NULL;
		if (z) {
			z->val = choice_was[i];
			z->kind = choice_kind[i];
		}
	}
}

// ENEMY AI: two values; the game's var_fullscreen = !var_fullscreen flips the shown value; store the other value (1 or 2).
static void enemy_optionsmenu_step(void *self, void *other) {
	enemy_shown enemy = enemy_show(self);
	choices_shown shown_choices = choice_show(self);
	void *prev_self = step_show_self;
	enemy_shown prev_show = step_show;
	choices_shown prev_choices = step_choices;
	step_show_self = self;
	step_show = enemy;
	step_choices = shown_choices;
	orig_optionsmenu_step(self, other);
	step_show_self = prev_self;
	step_show = prev_show;
	step_choices = prev_choices;
	choice_unshow(self, &shown_choices);
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
	for (int i = 0; i < NCHOICE; i++) {
		if (!choices[i].row)
			continue;
		gml_rvalue *z = global_var(choices[i].var_global);
		double v = z && (z->kind == VALUE_REAL || z->kind == VALUE_BOOL) ? z->val : 0.0;
		*choices[i].android = android_of(&choices[i], v);
	}
	timer_slowmo_apply(timer_slowmo_android); // rewrites the constants on a change only
	extern void pc_rules_apply(int android);
	pc_rules_apply(enemy_timing_android); // the enemy_hurt guards, on a change only
}

// ENEMY AI is drawn with var_fullscreen showing its value (enemy_show), and menu_translate_option relabels the value
// (the game's ON/OFF if it can't); TIMER & SLOW-MO the same with var_vsync. Draw only reads those variables and runs
// no user event (SAVE), so the stored values never see the change.
static void enemy_optionsmenu_draw(void *self, void *other) {
	enemy_shown enemy = enemy_show(self);
	choices_shown shown_choices = choice_show(self);
	void *prev_self = menu_draw_self;
	double prev_c = enemy_label_c, prev_timer_c = timer_label_c;
	menu_draw_self = self;
	enemy_label_c = timer_label_c = -1.0;
	orig_optionsmenu_draw(self, other);
	menu_draw_self = prev_self;
	enemy_label_c = prev_c;
	timer_label_c = prev_timer_c;
	enemy_unshow(self, enemy);
	choice_unshow(self, &shown_choices); // Draw doesn't change the variables: puts the stored values back
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

	int want_timing = enemy_timing_available(), want_timer = timer_slowmo_available();
	timer_slowmo_android = timer_slowmo_cfg != 0;
	choices[ROW_TIMING].var_global = (int *)so_symbol(&yoyoloader_mod, "g_VAR_zoom_mode");
	choices[ROW_TIMING].var_menu = (int *)so_symbol(&yoyoloader_mod, "g_VAR_var_zoom_mode");
	choices[ROW_TIMER].var_global = (int *)so_symbol(&yoyoloader_mod, "g_VAR_vsync");
	choices[ROW_TIMER].var_menu = (int *)so_symbol(&yoyoloader_mod, "g_VAR_var_vsync");
	if ((want_enemy || want_timing || want_timer) && yy_create_string && yy_free_rvalue)
		orig_translate_option = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z27gml_Script_translate_optionP9CInstanceS0_R8YYRValueiPPS1_"), menu_translate_option);
	// In this order, so a row is never added without its labels and its Step, Draw and SAVE hooks
	int enemy_ok = want_enemy && orig_translate_option && var_show_crap_settings && var_var_show_crap_settings &&
		var_var_fullscreen && var_c;
	int timing_ok = want_timing && orig_translate_option && choices[ROW_TIMING].var_global &&
		choices[ROW_TIMING].var_menu;
	// TIMER & SLOW-MO also needs the display_reset guard (Destroy and load_options pass global.vsync to display_reset)
	int timer_ok = want_timer && orig_translate_option && orig_display_reset && choices[ROW_TIMER].var_global &&
		choices[ROW_TIMER].var_menu && var_c;
	// Every row needs SAVE to see the values to store
	if (enemy_ok || timing_ok || timer_ok)
		orig_optionsmenu_save = gml_replace(table, "gml_Object_obj_optionsmenu_Other_10", enemy_optionsmenu_save);
	if (orig_optionsmenu_save)
		orig_optionsmenu_step = gml_replace(table, "gml_Object_obj_optionsmenu_Step_0", enemy_optionsmenu_step);
	if (orig_optionsmenu_step)
		orig_optionsmenu_draw = gml_replace(table, "gml_Object_obj_optionsmenu_Draw_0", enemy_optionsmenu_draw);
	// CONTROLS left out of the options main menu needs only this hook
	extern int touch_controls;
	int want_touch_hidden = !touch_controls && !exp_flag("touchmenu0");
	if (orig_optionsmenu_draw || want_touch_hidden)
		orig_add_pausemenu_choice = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z31gml_Script_add_pausemenu_choiceP9CInstanceS0_R8YYRValueiPPS1_"), menu_add_pausemenu_choice);
	int rows_ok = orig_optionsmenu_draw && orig_add_pausemenu_choice;
	enemy_row = enemy_ok && rows_ok;
	choices[ROW_TIMING].row = timing_ok && rows_ok;
	choices[ROW_TIMER].row = timer_ok && rows_ok;
	hide_touch_page = want_touch_hidden && orig_add_pausemenu_choice;
	debugPrintf("options menu CONTROLS (touch controls page): %s\n", hide_touch_page ? "hidden" :
		want_touch_hidden ? "NOT hidden (hook not installed)" : touch_controls ? "kept (touchControls)" :
		"kept (exp_touchmenu0)");
	if (enemy_row)
		enemy_half_cfg = enemy_half_rate; // from here on options_menu_sync owns enemy_half_rate
	debugPrintf("enemy AI menu: %s (yyl.cfg enemyHalfRate %d)\n", enemy_row ? "on" : want_enemy ? "NOT installed" : "off",
		enemy_half_rate);
	debugPrintf("enemy timing menu: %s (yyl.cfg enemyTiming %d)\n", choices[ROW_TIMING].row ? "on" : want_timing ?
		"NOT installed" : "off", enemy_timing_cfg);
	debugPrintf("timer & slow-mo menu: %s (yyl.cfg timerSlowmo %d)\n", choices[ROW_TIMER].row ? "on" : want_timer ?
		"NOT installed" : "off", timer_slowmo_cfg);
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
