/*
 * katana_menu.c
 *
 * GRAPHICS menu rows (FPS MODE, TEXT SMOOTHING), text smoothing, and the yes/no prompt's
 * starting selection.
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
 * TEXT SMOOTHING (OFF / DIALOGUE / ALL, the text smoothing below) is stored the same way in the PC build's VSYNC
 * option, added after FPS MODE. Its code is there too: Create copies global.vsync to var_vsync/prev_var_vsync, Step
 * toggles var_vsync = !BOOL(var_vsync), Draw shows the game's own "ON"/"OFF" (OFF when var_vsync is false), SAVE copies
 * it back and save_options/load_options keep it (load_variable returns file_text_read_real as it is: no clamp, no bool).
 * obj_init and mobile_reset_game_data set vsync to false, so every existing save holds 0: 0 means DIALOGUE (the old
 * ON), 1 OFF, 2 ALL. Step turns the game's toggle into OFF -> DIALOGUE -> ALL -> OFF. Draw runs with var_vsync set to
 * the game's ON/OFF (ON for DIALOGUE and ALL), and translate_option relabels this row's value: Draw keeps its row index
 * in the instance variable c, the label is translate_option(choice[c]) and the toggle value translate_option("ON" or
 * "OFF") with c unchanged (lines 403-421 and 1061), so an ON/OFF asked for at the c that drew "VSYNC" is this row's.
 * What the game does with vsync:
 * - display_reset(aa, vsync), from load_options at boot and from Destroy when global.vsync != prev_var_vsync.
 *   F_DisplayReset stores vsync in g_Sync (frame limiter, MainLoop_Process: g_SyncInterval) and sets g_DoReset, so
 *   the next frame runs GR_D3D_Reset, which frees every surface. The boot call (vsync false in every save: g_Sync 1 ->
 *   0, one reset) is kept as it was, with vsync forced to false; the calls Destroy makes are dropped.
 * text_smooth_sync reads global.vsync once per frame (katana_after_swap). exp_textmenu0 turns the row off and keeps
 * the text smooth (yyl.cfg only); exp_textsharp keeps it sharp and has no row.
 */
static int fps30_cfg = -1; // fpsMode from yyl.cfg, kept once fps30 follows the menu (-1: the menu isn't installed)
static int *var_zoom_mode, *var_var_zoom_mode;
static gml_script_fn orig_add_pausemenu_choice, orig_translate_option;
static event_func orig_optionsmenu_step;
static int fps_row, text_row; // the rows the menu shows
enum { TEXT_SMOOTH_OFF, TEXT_SMOOTH_DIALOGUE, TEXT_SMOOTH_ALL };
static int text_smooth = TEXT_SMOOTH_DIALOGUE; // the TEXT_SMOOTH_EVENTS wrappers, and with ALL the GR_Text_Draw ones
static int text_smooth_hooked; // of the 7 draw events
static int text_all_hooked; // of the 4 GR_Text_Draw functions
static const double text_smooth_stored[3] = { 1.0, 0.0, 2.0 }; // global.vsync by mode
static int *var_vsync, *var_var_vsync, *var_c;
static event_func orig_optionsmenu_draw;
static void *text_draw_self; // the obj_optionsmenu running Draw
static int text_draw_mode; // its var_vsync, as a mode
static double text_label_c = -1; // its c when it asked for the "VSYNC" label
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
		static const char *const rows[2] = { "WINDOW ZOOM", "VSYNC" };
		void *toggle = args[1]; // this call's "toggle"
		for (int i = 0; i < 2; i++) {
			if (!(i ? text_row : fps_row))
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

// A real or bool instance variable (var_vsync is a bool once Step has toggled it)
static gml_rvalue *instance_num(void *self, int *var) {
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)self)[3];
	gml_rvalue *v = var ? vwrite(self, var[1]) : NULL;
	return v && (v->kind == VALUE_REAL || v->kind == VALUE_BOOL) ? v : NULL;
}

// global.vsync / var_vsync -> TEXT_SMOOTH_*: 0 (every existing save) DIALOGUE, 1 OFF, 2 ALL, anything else DIALOGUE
static int text_mode_of(double v) {
	return v == 1.0 ? TEXT_SMOOTH_OFF : v == 2.0 ? TEXT_SMOOTH_ALL : TEXT_SMOOTH_DIALOGUE;
}

// obj_optionsmenu Draw's row index (only read inside its Draw, where c exists)
static double text_draw_c(void *self) {
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
	else if (text_row && !strcmp(key, "VSYNC")) {
		text = "TEXT SMOOTHING";
		if (text_draw_self && self == text_draw_self)
			text_label_c = text_draw_c(self);
	} else if (text_draw_self && self == text_draw_self && text_label_c >= 0.0 && (!strcmp(key, "ON") || !strcmp(key, "OFF")) &&
			text_draw_c(self) == text_label_c) {
		// This row's value. OFF stays the game's own, translated (Draw asks for "OFF" then)
		if (text_draw_mode != TEXT_SMOOTH_OFF)
			text = text_draw_mode == TEXT_SMOOTH_ALL ? "ALL" : "DIALOGUE";
	}
	if (!text)
		return orig_translate_option(self, other, result, argc, args);
	yy_create_string(result, text); // frees the old value first
	return result;
}

// Three values each:
// - FPS MODE: the game's toggle wraps 3 -> 0 ("not chosen"); make it 3 -> 1;
// - TEXT SMOOTHING: the game's var_vsync = !var_vsync flips it; make that the next of OFF -> DIALOGUE -> ALL -> OFF
//   (ALL is skipped if its hooks are missing).
static void fps_optionsmenu_step(void *self, void *other) {
	gml_rvalue *z = fps_row ? instance_var(self, var_var_zoom_mode) : NULL;
	double zoom = z ? z->val : -1;
	gml_rvalue *t = text_row ? instance_num(self, var_var_vsync) : NULL;
	int have_smooth = t != NULL;
	double smooth = t ? t->val : 0.0;
	orig_optionsmenu_step(self, other);
	if (zoom == 3.0 && (z = instance_var(self, var_var_zoom_mode)) && z->val == 0.0)
		z->val = 1.0;
	if (have_smooth && (t = instance_num(self, var_var_vsync)) && (t->val != 0.0) != (smooth != 0.0)) {
		int mode = text_mode_of(smooth) + 1;
		if (mode > TEXT_SMOOTH_ALL || (mode == TEXT_SMOOTH_ALL && text_all_hooked < 4))
			mode = TEXT_SMOOTH_OFF;
		t->val = text_smooth_stored[mode];
		t->kind = VALUE_REAL;
	}
}

// Once per frame (t30_update): the saved menu choice drives fps30
void fps_mode_sync(void) {
	if (fps30_cfg < 0 || !*global_obj_ptr)
		return;
	gml_rvalue *z = global_var(var_zoom_mode);
	int m = z ? (int)z->val : 0;
	fps30 = m == 1 ? 0 : m == 2 ? 2 : m == 3 ? 1 : fps30_cfg;
}

// Draw shows OFF for a false var_vsync, and false (every existing save) means DIALOGUE: draw it with var_vsync true
// for DIALOGUE and ALL, false for OFF, and fps_translate_option relabels the value (the game's ON/OFF if it can't).
// Draw only reads var_vsync and runs no user event (SAVE), so the stored value never sees the change.
static void text_optionsmenu_draw(void *self, void *other) {
	gml_rvalue *v = instance_num(self, var_var_vsync);
	if (!v) {
		orig_optionsmenu_draw(self, other);
		return;
	}
	double was = v->val;
	int mode = text_mode_of(was);
	v->val = mode != TEXT_SMOOTH_OFF ? 1.0 : 0.0;
	void *prev_self = text_draw_self;
	int prev_mode = text_draw_mode;
	double prev_c = text_label_c;
	text_draw_self = self;
	text_draw_mode = mode;
	text_label_c = -1.0;
	orig_optionsmenu_draw(self, other);
	text_draw_self = prev_self;
	text_draw_mode = prev_mode;
	text_label_c = prev_c;
	if ((v = instance_num(self, var_var_vsync))) // looked up again: Draw can add instance variables
		v->val = was;
}

// display_reset is forwarded only from load_options, with vsync false: what the boot call does with every save
// today (g_Sync 0, one GR_D3D_Reset before the first room). The calls from obj_optionsmenu Destroy (a VSYNC or
// FULLSCREEN change) are dropped: they would free every surface the next frame. Nothing else calls it.
static gml_script_fn orig_load_options;
static int in_load_options;

static void *text_load_options(void *self, void *other, void *result, int argc, void **args) {
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

// Once per frame (katana_after_swap): the saved TEXT SMOOTHING choice (global.vsync false: DIALOGUE)
void text_smooth_sync(void) {
	if (!text_row || !*global_obj_ptr)
		return;
	gml_rvalue *v = global_var(var_vsync);
	text_smooth = text_mode_of(v ? v->val : 0.0);
}

// The FPS MODE and TEXT SMOOTHING rows, each installed only with everything it needs
void fps_menu_install(gml_func_entry *table) {
	int want_fps = !exp_flag("fpsmenu0") && !exp_flag("t30force") && t30_installed;
	int want_text = text_smooth && text_smooth_hooked && global_obj_ptr && !exp_flag("textmenu0");
	var_zoom_mode = (int *)so_symbol(&yoyoloader_mod, "g_VAR_zoom_mode");
	var_var_zoom_mode = (int *)so_symbol(&yoyoloader_mod, "g_VAR_var_zoom_mode");
	var_vsync = (int *)so_symbol(&yoyoloader_mod, "g_VAR_vsync");
	var_var_vsync = (int *)so_symbol(&yoyoloader_mod, "g_VAR_var_vsync");
	var_c = (int *)so_symbol(&yoyoloader_mod, "g_VAR_c");
	yy_create_string = (void *)so_symbol(&yoyoloader_mod, "_Z14YYCreateStringP6RValuePKc");
	yy_free_rvalue = (void *)so_symbol(&yoyoloader_mod, "_Z16FREE_RValue__PreP6RValue");

	// The display_reset guard whatever the menu: it keeps the boot call as it is today for any saved vsync.
	// load_options first, so the guard never sees the boot call without knowing where it comes from.
	orig_load_options = entry_hook(so_symbol(&yoyoloader_mod,
		"_Z23gml_Script_load_optionsP9CInstanceS0_R8YYRValueiPPS1_"), text_load_options);
	if (orig_load_options)
		orig_display_reset = entry_hook(so_symbol(&yoyoloader_mod, "_Z14F_DisplayResetR6RValueP9CInstanceS2_iPS_"),
			guarded_display_reset);
	debugPrintf("display_reset guard: %s\n", orig_display_reset ? "on" : "NOT installed");

	if ((want_fps || want_text) && yy_create_string && yy_free_rvalue)
		orig_translate_option = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z27gml_Script_translate_optionP9CInstanceS0_R8YYRValueiPPS1_"), fps_translate_option);
	// In this order, so a row is never added without its labels and its Step (and Draw) hook
	int fps_ok = want_fps && orig_translate_option && var_zoom_mode && var_var_zoom_mode;
	int text_ok = want_text && orig_translate_option && orig_display_reset && var_vsync && var_var_vsync && var_c;
	if (fps_ok || text_ok)
		orig_optionsmenu_step = gml_replace(table, "gml_Object_obj_optionsmenu_Step_0", fps_optionsmenu_step);
	if (text_ok && orig_optionsmenu_step)
		orig_optionsmenu_draw = gml_replace(table, "gml_Object_obj_optionsmenu_Draw_0", text_optionsmenu_draw);
	if ((fps_ok || orig_optionsmenu_draw) && orig_optionsmenu_step)
		orig_add_pausemenu_choice = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z31gml_Script_add_pausemenu_choiceP9CInstanceS0_R8YYRValueiPPS1_"), fps_add_pausemenu_choice);
	if (orig_add_pausemenu_choice) {
		fps_row = fps_ok;
		text_row = !!orig_optionsmenu_draw;
	}
	if (fps_row)
		fps30_cfg = fps30; // from here on fps_mode_sync owns fps30
	debugPrintf("FPS mode menu: %s (yyl.cfg fpsMode %d)\n", fps_row ? "on" : want_fps ? "NOT installed" : "off", fps30);
	debugPrintf("text smoothing menu: %s%s\n", text_row ? "on" : want_text ? "NOT installed" : "off",
		text_row && text_all_hooked < 4 ? " (no ALL)" : "");
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

// Dialogue text is drawn at half scale for the 1280x720 application surface, so on the 960x540 one a text pixel is
// 0.75 of a screen pixel: with nearest filtering letter strokes come out one or two pixels wide at random. The
// letters (obj_letter: dialogue is drawn one letter instance at a time, also the club's sign letters) and the textbox
// and choice box Draw events run with texture interpolation on (restored afterwards), so dialogue text is smooth and
// even while sprites, HUD and menus stay sharp. The wrappers are always installed and smooth only while text_smooth
// is on: the GRAPHICS menu's TEXT SMOOTHING row (see fps_menu_install). exp_textsharp keeps it off.
static int (*get_tex_interp)(void);
static void (*set_tex_interp)(int on);
#define TEXT_SMOOTH_EVENTS(X) X(letter) X(textbox) X(passive_textbox) X(old_textbox) X(blanktextbox) X(choicebox) \
	X(choicebox_hauntedelevator)
#define TEXT_SMOOTH_WRAP(name) static event_func orig_##name##_draw; \
	static void smooth_##name##_draw(void *self, void *other) { \
		if (!text_smooth) { \
			orig_##name##_draw(self, other); \
			return; \
		} \
		int was = get_tex_interp() & 1; \
		set_tex_interp(1); \
		orig_##name##_draw(self, other); \
		set_tex_interp(was); \
	}
TEXT_SMOOTH_EVENTS(TEXT_SMOOTH_WRAP)

// TEXT SMOOTHING ALL: every font draw (menus, HUD, prompts, the level select's tape labels from obj_tape Other_10)
// with interpolation on. Every draw_text* builtin, F_ and YYGML_ alike, ends in one of these four, and each is the
// only caller of its CFontGM::Draw_String* (sprite fonts and TrueType fonts both). Sprites stay as they are; text
// drawn as sprites (obj_letter's dialogue letters) is the TEXT_SMOOTH_EVENTS wrappers' part.
static void (*orig_gr_text_draw)(float x, float y, const char *s, int sep, int w);
static void (*orig_gr_text_draw_color)(float x, float y, const char *s, int sep, int w, unsigned c1, unsigned c2,
	unsigned c3, unsigned c4, float alpha);
static void (*orig_gr_text_draw_transformed)(float x, float y, const char *s, int sep, int w, float xs, float ys,
	float angle);
static void (*orig_gr_text_draw_transformed_color)(float x, float y, const char *s, int sep, int w, float xs, float ys,
	float angle, unsigned c1, unsigned c2, unsigned c3, unsigned c4, float alpha);

static int text_all_begin(void) {
	if (text_smooth != TEXT_SMOOTH_ALL || (get_tex_interp() & 1))
		return 0;
	set_tex_interp(1);
	return 1;
}

static void text_all_end(int on) {
	if (on)
		set_tex_interp(0);
}

static void all_gr_text_draw(float x, float y, const char *s, int sep, int w) {
	int on = text_all_begin();
	orig_gr_text_draw(x, y, s, sep, w);
	text_all_end(on);
}

static void all_gr_text_draw_color(float x, float y, const char *s, int sep, int w, unsigned c1, unsigned c2,
		unsigned c3, unsigned c4, float alpha) {
	int on = text_all_begin();
	orig_gr_text_draw_color(x, y, s, sep, w, c1, c2, c3, c4, alpha);
	text_all_end(on);
}

static void all_gr_text_draw_transformed(float x, float y, const char *s, int sep, int w, float xs, float ys,
		float angle) {
	int on = text_all_begin();
	orig_gr_text_draw_transformed(x, y, s, sep, w, xs, ys, angle);
	text_all_end(on);
}

static void all_gr_text_draw_transformed_color(float x, float y, const char *s, int sep, int w, float xs, float ys,
		float angle, unsigned c1, unsigned c2, unsigned c3, unsigned c4, float alpha) {
	int on = text_all_begin();
	orig_gr_text_draw_transformed_color(x, y, s, sep, w, xs, ys, angle, c1, c2, c3, c4, alpha);
	text_all_end(on);
}

static void text_all_install(void) {
	text_all_hooked += !!(orig_gr_text_draw = entry_hook(so_symbol(&yoyoloader_mod, "_Z12GR_Text_DrawffPKcii"),
		all_gr_text_draw));
	text_all_hooked += !!(orig_gr_text_draw_color = entry_hook(so_symbol(&yoyoloader_mod,
		"_Z18GR_Text_Draw_ColorffPKciijjjjf"), all_gr_text_draw_color));
	text_all_hooked += !!(orig_gr_text_draw_transformed = entry_hook(so_symbol(&yoyoloader_mod,
		"_Z24GR_Text_Draw_TransformedffPKciifff"), all_gr_text_draw_transformed));
	text_all_hooked += !!(orig_gr_text_draw_transformed_color = entry_hook(so_symbol(&yoyoloader_mod,
		"_Z30GR_Text_Draw_Transformed_ColorffPKciifffjjjjf"), all_gr_text_draw_transformed_color));
}

// Called from patch_performance: the prompt preselect and dialogue text smoothing
void katana_menu_install(gml_func_entry *table) {
	event_func orig;
	var_selection = (int *)so_symbol(&yoyoloader_mod, "g_VAR_selection");
	var_default_select = (int *)so_symbol(&yoyoloader_mod, "g_VAR_defaultSelect");
	if (var_selection && var_default_select && !exp_flag("touchui") &&
		(orig = gml_replace(table, "gml_Object_obj_interaction_Create_0", interaction_create_preselect)))
		orig_interaction_create = orig;
	debugPrintf("prompts: default choice highlight %s\n", orig_interaction_create ? "on" : "NOT hooked");
	get_tex_interp = (void *)so_symbol(&yoyoloader_mod, "_Z32GR_D3D_Get_Texture_Interpolationv");
	set_tex_interp = (void *)so_symbol(&yoyoloader_mod, "_Z32GR_D3D_Set_Texture_Interpolationb");
	text_smooth = exp_flag("textsharp") ? TEXT_SMOOTH_OFF : TEXT_SMOOTH_DIALOGUE;
	if (get_tex_interp && set_tex_interp) {
#define TEXT_SMOOTH_HOOK(name) text_smooth_hooked += !!(orig_##name##_draw = \
			gml_replace(table, "gml_Object_obj_" #name "_Draw_0", smooth_##name##_draw));
		TEXT_SMOOTH_EVENTS(TEXT_SMOOTH_HOOK)
		// ALL is only reachable from the menu row (exp_textsharp has none, exp_textmenu0 keeps DIALOGUE)
		if (text_smooth && !exp_flag("textmenu0"))
			text_all_install();
	}
	debugPrintf("text smoothing: %d of 7 dialogue draw events, %d of 4 text draws, %s\n", text_smooth_hooked,
		text_all_hooked, text_smooth ? "on" : "off");
}
