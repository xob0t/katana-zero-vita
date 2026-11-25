/*
 * option_defaults.c: five GRAPHICS options are off by default. Each can be turned on in the menu, and the choice is
 * saved and kept as before. A yyl.cfg key per option set to 1 keeps the game's default (on):
 *
 *   row          global                 yyl.cfg              what it draws (APK 1.1.10)
 *   RESTART FX   global.restart_shader  restartFxDefault=1   wave distortion of the restart transition (obj_blink and
 *                                                            obj_blink_fxonly Draw_64, also need SHADERS on)
 *   BLOOM FX     global.bloom_shader    bloomDefault=1       bloom pass (obj_zoomblur Draw_64 L63, needs SHADERS on)
 *   REWIND FX    global.rewind_shader   rewindFxDefault=1    wave distortion of the death rewind (obj_rewind Draw_64,
 *                                                            needs SHADERS on; off: obj_rewind destroys itself). The
 *                                                            port skips the rewind unless replays=1: nothing to draw.
 *   SHADERS      global.shaders         shadersDefault=1     the shader passes above and the end-of-level replay's
 *                                                            (obj_replay Draw_64). Its other readers are option menus
 *                                                            and a toggle in obj_gamepaused Step_1 (L54); rim
 *                                                            shading, recolouring, lights, smoke and the pause
 *                                                            picture (crt_native.c, pause_stack.c) don't read it.
 *   BLOOD SPRAY  global.show_blood      bloodSprayDefault=1  blood drops and blood decals (obj_blood, drawactors_bgfg,
 *                                                            the replay's blood); blood_drops.c skips hidden drops
 *
 * The game's defaults are on: obj_init Create sets all five to true among the other defaults (L450-L479) and then
 * runs its user event 0 (event_user(0) at line 592), which reads KatanaOptions.zero through load_options.
 * load_options reads each value with load_variable(<current value>), and load_variable returns that current value at
 * the end of the file; with no file at all load_options saves a new one from the current values. So the value in the
 * global when load_options starts is the default, and setting 0 between obj_init Create's defaults and its
 * event_user(0) changes only the default:
 *  - no KatanaOptions.zero (fresh install): off, and saved off;
 *  - a saved value: load_options reads it over the 0, on or off as the player left it.
 * The wrapper of obj_init's user event 0 writes them, only while obj_init Create runs (the only event_user(0) on it).
 *
 * mobile_reset_game_data also sets all five to true with the other defaults; the title menu and the Netflix SDK
 * object call it, then load_options (at every boot, after the cloud read of the options: "Cleared old KatanaOptions
 * and loading again"). The same 0s are written after the original, so its defaults are off too.
 *
 * Debug (debugMode=1), for measuring what each option costs with it on:
 *  - exp_shadersab, exp_bloodab: flip global.shaders / global.show_blood at every report window ([OPTAB] line).
 *  - exp_restartfxab, exp_rewindfxab: flip global.restart_shader / global.rewind_shader at the start of every
 *    effect (the first Draw_64 of obj_blink, obj_blink_fxonly or obj_rewind after 0.5 s without one).
 *  - exp_optfx: [OPTFX] per report window: frames with the restart or rewind effect drawn and their mean frame
 *    interval, for each state of its option (also on with the two flags above).
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

// yyl.cfg: 1 keeps the game's default (on) for that option
int bloom_default = 0, restart_fx_default = 0, rewind_fx_default = 0, shaders_default = 0, blood_spray_default = 0;

enum { OPT_RESTART, OPT_BLOOM, OPT_REWIND, OPT_SHADERS, OPT_BLOOD, N_OPT };
static struct {
	const char *name, *sym;
	int *keep, *var;
} opts[N_OPT] = {
	{"restart fx", "g_VAR_restart_shader", &restart_fx_default},
	{"bloom", "g_VAR_bloom_shader", &bloom_default},
	{"rewind fx", "g_VAR_rewind_shader", &rewind_fx_default},
	{"shaders", "g_VAR_shaders", &shaders_default},
	{"blood spray", "g_VAR_show_blood", &blood_spray_default},
};
static event_func orig_init_create, orig_init_user0;
static gml_script_fn orig_reset_game_data;
static int in_init_create;

static void set_opt(int i, double val) {
	gml_rvalue *v = global_var(opts[i].var);
	if (v) {
		v->val = val;
		v->kind = 0; // real, as the game writes it
	}
}

static int get_opt(int i) {
	gml_rvalue *v = global_var(opts[i].var);
	return v && v->val > 0.5;
}

static void defaults_off(void) {
	for (int i = 0; i < N_OPT; i++)
		if (opts[i].var && !*opts[i].keep)
			set_opt(i, 0.0);
}

static void init_create(void *self, void *other) {
	in_init_create++;
	orig_init_create(self, other);
	in_init_create--;
}

static void init_user0(void *self, void *other) {
	if (in_init_create)
		defaults_off(); // after Create's defaults, before load_options
	orig_init_user0(self, other);
}

static void *reset_game_data(void *self, void *other, void *result, int argc, void **args) {
	void *r = orig_reset_game_data(self, other, result, argc, args);
	defaults_off();
	return r;
}

// Debug: A/B flips and the restart / rewind effect frames
static int ab_shaders, ab_blood, ab_restart, ab_rewind, fx_log;
enum { FX_BLINK, FX_BLINK_FXONLY, FX_REWIND, N_FX };
static const char *fx_events[N_FX] = {"gml_Object_obj_blink_Draw_64", "gml_Object_obj_blink_fxonly_Draw_64",
	"gml_Object_obj_rewind_Draw_64"};
static event_func orig_fx[N_FX];
static SceUInt64 fx_last[2];               // last effect Draw_64: restart (blink, blink_fxonly), rewind
static uint32_t fx_frame[2], fx_n[2][2];   // [effect][option on]: frames with a previous effect frame right before
static SceUInt64 fx_us[2][2];

static void fx_draw(int fx, void *self, void *other) {
	int kind = fx == FX_REWIND, opt = kind ? OPT_REWIND : OPT_RESTART;
	SceUInt64 now = sceKernelGetProcessTimeWide();
	uint32_t frame = frame_count ? *frame_count : 0;
	if ((kind ? ab_rewind : ab_restart) && (!fx_last[kind] || now - fx_last[kind] > 500000))
		set_opt(opt, get_opt(opt) ? 0.0 : 1.0); // a new effect: the other state
	if (fx_last[kind] && frame == fx_frame[kind] + 1 && now - fx_last[kind] < 500000) {
		int on = get_opt(opt) && get_opt(OPT_SHADERS);
		fx_n[kind][on]++;
		fx_us[kind][on] += now - fx_last[kind];
	}
	if (frame != fx_frame[kind] || !fx_last[kind]) {
		fx_last[kind] = now;
		fx_frame[kind] = frame;
	}
	orig_fx[fx](self, other);
}
static void fx_blink(void *s, void *o) { fx_draw(FX_BLINK, s, o); }
static void fx_blink_fxonly(void *s, void *o) { fx_draw(FX_BLINK_FXONLY, s, o); }
static void fx_rewind(void *s, void *o) { fx_draw(FX_REWIND, s, o); }

void option_defaults_report(int frames) {
	(void)frames;
	if (ab_shaders || ab_blood) {
		// The state of the window just reported, then the other one for the next window
		debugPrintf("[OPTAB] shaders %s blood %s\n", get_opt(OPT_SHADERS) ? "on" : "off",
			get_opt(OPT_BLOOD) ? "on" : "off");
		if (ab_shaders)
			set_opt(OPT_SHADERS, get_opt(OPT_SHADERS) ? 0.0 : 1.0);
		if (ab_blood)
			set_opt(OPT_BLOOD, get_opt(OPT_BLOOD) ? 0.0 : 1.0);
	}
	if (fx_log) {
		for (int k = 0; k < 2; k++)
			if (fx_n[k][0] || fx_n[k][1])
				debugPrintf("[OPTFX] %s: shader on %u frames %.2f ms, off %u frames %.2f ms\n",
					k ? "rewind" : "restart", fx_n[k][1], fx_n[k][1] ? fx_us[k][1] / 1000.0 / fx_n[k][1] : 0.0,
					fx_n[k][0], fx_n[k][0] ? fx_us[k][0] / 1000.0 / fx_n[k][0] : 0.0);
		for (int k = 0; k < 2; k++)
			fx_n[k][0] = fx_n[k][1] = 0, fx_us[k][0] = fx_us[k][1] = 0;
	}
}

static void debug_install(gml_func_entry *table) {
	if (!debugMode)
		return;
	ab_shaders = exp_flag("shadersab");
	ab_blood = exp_flag("bloodab");
	ab_restart = exp_flag("restartfxab");
	ab_rewind = exp_flag("rewindfxab");
	fx_log = exp_flag("optfx") || ab_restart || ab_rewind;
	if (fx_log)
		for (int i = 0; i < N_FX; i++) {
			static const event_func wrappers[N_FX] = {fx_blink, fx_blink_fxonly, fx_rewind};
			orig_fx[i] = gml_replace(table, fx_events[i], wrappers[i]);
			if (!orig_fx[i])
				debugPrintf("option defaults: %s NOT wrapped\n", fx_events[i]);
		}
	if (ab_shaders || ab_blood || fx_log)
		debugPrintf("option defaults: A/B%s%s%s%s%s\n", ab_shaders ? " shaders" : "", ab_blood ? " blood" : "",
			ab_restart ? " restart fx" : "", ab_rewind ? " rewind fx" : "", fx_log ? ", effect frames logged" : "");
}

void option_defaults_install(gml_func_entry *table) {
	int any = 0, missing = 0;
	for (int i = 0; i < N_OPT; i++) {
		opts[i].var = (int *)so_symbol(&yoyoloader_mod, opts[i].sym);
		if (!*opts[i].keep)
			any++;
		if (!opts[i].var)
			missing++;
	}
	if (global_obj_ptr && !missing)
		debug_install(table);
	if (!any) {
		debugPrintf("option defaults: the game's (all kept in yyl.cfg)\n");
		return;
	}
	event_func orig;
	// The Create wrapper alone only counts; user event 0 is wrapped only if Create is
	if (!missing && global_obj_ptr && (orig = gml_replace(table, "gml_Object_obj_init_Create_0", init_create))) {
		orig_init_create = orig;
		if ((orig = gml_replace(table, "gml_Object_obj_init_Other_10", init_user0)))
			orig_init_user0 = orig;
	}
	if (orig_init_user0)
		orig_reset_game_data = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z33gml_Script_mobile_reset_game_dataP9CInstanceS0_R8YYRValueiPPS1_"), reset_game_data);
	char off[96] = "";
	for (int i = 0; i < N_OPT; i++)
		if (!*opts[i].keep)
			snprintf(off + strlen(off), sizeof(off) - strlen(off), "%s%s", off[0] ? ", " : "", opts[i].name);
	debugPrintf("option defaults: %s%s%s\n", orig_init_user0 ? "off: " : "NOT installed", orig_init_user0 ? off : "",
		orig_init_user0 && !orig_reset_game_data ? " (NOT in mobile_reset_game_data)" : "");
}
