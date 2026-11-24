/*
 * gamepad_prompts.c: start the game in its gamepad mode, so prompts show buttons from the first frame.
 *
 * The Android build picks touch or gamepad prompts by global.gamepadMode. obj_global_controller's Create sets it
 * false (line 17), and only these write it afterwards (APK 1.1.10, libyoyo.so):
 *  - player_analogue_pressed_check L17: true once a Vita stick passes controller_threshold (tempStopScreenjoystick
 *    reaches 30, lines 5-13). The d-pad and the buttons never set it.
 *  - obj_onScreenKeyboard Step L109: false on a screen tap (with obj_global_controller.gamepad_mode = false, L110).
 *  - player_screenjoystick_pressed_check L44: false when the touch stick is dragged (tempStopGamepad 30).
 * So until the first stick move the game is in touch mode: "tap screen to begin playing" at level start
 * (obj_leveltext Draw L38), the touch tutorial hints in room_factory_0 (obj_cutscene_factory_0 Draw), and the
 * touch variants of the interact prompts, the HUD and draw_control.
 *
 * With touchControls=0 there is no touch input (main.c) and the touch stick stays at rest (touch_controls.c), so
 * neither false writer can run, and setting global.gamepadMode = true after the controller's Create gives the state
 * the game reaches on the first stick move, from the start. Besides the prompts, gamepadMode decides the on-foot
 * slash direction (player_get_attackdirection): the stick's exact angle past controller_threshold, else the
 * 8 directions of the d-pad, as on PC; without it every slash snaps to 8 directions. obj_onScreenKeyboard's first
 * user event 0 (the only run with touchControls=0) also takes its gamepad branch: it hides the touch buttons.
 * touchControls=1/2 keep the game's touch-first start: a tap switches to touch mode anyway.
 */
#include <vitasdk.h>
#include <stdio.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

static int *var_gamepad_mode;
static event_func orig_controller_create;

static void controller_create(void *self, void *other) {
	orig_controller_create(self, other);
	gml_rvalue *v = global_var(var_gamepad_mode);
	if (v) {
		v->val = 1.0;
		v->kind = 13; // bool, as the game writes it
	}
}

void gamepad_prompts_install(gml_func_entry *table) {
	extern int touch_controls;
	if (touch_controls || exp_flag("padprompts0"))
		return;
	var_gamepad_mode = (int *)so_symbol(&yoyoloader_mod, "g_VAR_gamepadMode");
	event_func orig;
	if (var_gamepad_mode && global_obj_ptr &&
		(orig = gml_replace(table, "gml_Object_obj_global_controller_Create_0", controller_create)))
		orig_controller_create = orig;
	debugPrintf("gamepad prompts: %s\n", orig_controller_create ? "from the start" : "NOT hooked");
}
