/*
 * touch_controls.c: the mobile build's touch controls, off by default (touchControls in yyl.cfg).
 *
 * The Netflix build puts a virtual stick and buttons over the game: obj_global_controller's Create makes
 * obj_onScreenKeyboard unconditionally (line 18), and the keyboard's user event 0 runs user event 1 once, which
 * creates the obj_screenBtn buttons, the obj_replayBtns and the obj_screenJoystick. The only switch the game has,
 * the keyboard's onScreenController, isn't usable: off, no buttons exist, and code all over the game reads them
 * without testing that they exist (controller_check: obj_onScreenKeyboard.vKeyList[0].isHeld for gp_shoulderrb and
 * obj_screenJoystick.posValueX/Y; the tutorial cutscenes and obj_pickup: vKeyList[n].signalButton = 30; the replay
 * controller: vKeyList[6..11].keyState). global.gamepadMode (the game's pad mode) hides them but also changes
 * the HUD, the interact prompts and the attack direction.
 *
 * So the instances stay and their per-frame events stop. With touchControls=0 the loader feeds no front touch to
 * the runner (main.c), and without touch these events only keep the controls at rest:
 *  - obj_screenBtn Step / obj_replayBtns Step: keyState only rises while buttonDown, which only the keyboard's Step
 *    sets from a touch. Without one, isHeld / isPressed / switchOn stay false and keyState stays below 1
 *    (it counts down from 0; the replay controller and the buttons only test keyState == 1 / >= 1). The rest is
 *    layout: x / y follow the view, scale, blink and fade.
 *  - obj_screenJoystick Step: posValueX / posValueY / moveAmnt stay 0 and walking false while not dragged. Its
 *    rest path also writes wasNeutral = 20 every frame (line 105: `if (!buttonDown || moveAmnt <= 0.15) wasNeutral
 *    = 20`); Create sets 0. The player's ground crouch checks read it (idle_ground L100, moving_ground L236,
 *    player_walking L124, player_physics L470, slope_land_check L47: `controller_check(key_crouch) &&
 *    (allowCrouch || obj_screenJoystick.wasNeutral > 0 || control_override > 0)`, allowCrouch always false), so
 *    the skip writes the rest value itself; without it down did not crouch.
 *  - obj_screenBtn GUI Draw (sprites already not drawn, katana.c), obj_screenJoystick GUI Draw, obj_replayBtns Draw:
 *    drawing, plus layout. While slow motion runs, the slow-mo button's GUI Draw also made an obj_hudparticle spark
 *    at the (hidden) button every frame (irandom, random, random_range). Those sparks and their random numbers are
 *    gone, as on PC (the game randomizes its seed at start, obj_init, so no run repeats a sequence anyway).
 *  - obj_onScreenKeyboard user event 0, from its Step (and its window resize event): which buttons show and
 *    where (visible, signalButton, itemImg, interactHover, intBtnMax...), read only by the touch controls. Its first
 *    run (onScreenControlSetup still false) runs as before: it creates the buttons.
 * Everything read outside the touch controls stays as it is at rest: the Create values, which the events write back
 * every frame. The keyboard's Step keeps running: it sets backButtonPressed (keyboard_check_pressed(vk_backspace),
 * controller_check_pressed_ext), screenTapped / tappedScreenX/Y (menus), launchGo, gamepadActive and the pause on
 * os_is_paused. The tap circle (obj_mobile_draw_tap_circles) and the GUI edges / lag cog (Draw_64) stay.
 *
 * touchControls=1 is the earlier default: touch works, the controls aren't drawn. touchControls=2 draws them (also
 * exp_touchui). exp_touchab (debug) runs the events in every other report window (in-run A/B); exp_touchverify
 * checks every frame, in both states, that the values readers see are the rest values ([TOUCH] line).
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

int touch_controls = 0; // touchControls (yyl.cfg): 0 off (default), 1 touch without drawn controls, 2 drawn

// APK 1.1.10 object indices; obj_debugKey (vKeyList[11]) inherits obj_screenBtn's events
enum { OBJ_DEBUG_KEY = 330, OBJ_KEYBOARD = 703, OBJ_REPLAY_BTN = 854, OBJ_SCREEN_BTN = 879, OBJ_JOYSTICK = 880 };

static int touch_ab, touch_ab_original, touch_verify;
static unsigned skipped_btn_step, skipped_btn_gui, skipped_stick, skipped_replay, skipped_user0, ran_user0;
static unsigned verify_frames[2], verify_values[2], verify_bad[2], verify_unknown[2];
static unsigned bad_what[16];

#define SKIP(name, counter) \
	static event_func orig_##name; \
	static void skip_##name(void *self, void *other) { \
		if (touch_ab_original) \
			orig_##name(self, other); \
		else \
			counter++; \
	}
SKIP(btn_step, skipped_btn_step)
SKIP(btn_gui, skipped_btn_gui)
SKIP(stick_gui, skipped_stick)
SKIP(replay_step, skipped_replay)
SKIP(replay_draw, skipped_replay)

static int *var_setup, *var_was_neutral;

// The stick Step's rest path at line 105 (0x48801e0: wasNeutral = 20.0, kind real); the rest of the Step stays
// skipped. Only overwrite kinds that hold no reference (real, bool, undefined, unset).
static event_func orig_stick_step;
static void skip_stick_step(void *self, void *other) {
	if (touch_ab_original) {
		orig_stick_step(self, other);
		return;
	}
	skipped_stick++;
	if (!var_was_neutral)
		return;
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)self)[3];
	gml_rvalue *v = vwrite(self, var_was_neutral[1]);
	if (v && (v->kind == 0 || v->kind == 5 || v->kind == 13 || v->kind == 0xffffff)) {
		v->val = 20.0;
		v->flags = 0;
		v->kind = 0;
	}
}

static int truthy(const gml_rvalue *v) {
	return v && (v->kind == 0 || v->kind == 13) && v->val > 0.5;
}

// The keyboard's user event 0: the first run creates the buttons; afterwards it only lays out hidden controls
static event_func orig_user0;
static void keyboard_user0(void *self, void *other) {
	gml_rvalue *setup = gml_vget(self, var_setup[1]);
	if (touch_ab_original || !setup || (setup->kind != 0 && setup->kind != 13) || !truthy(setup)) {
		ran_user0++;
		orig_user0(self, other);
	} else
		skipped_user0++;
}

// exp_touchverify: the instances' state the rest of the game reads, checked after the keyboard's Step
static uint32_t word(const void *p, unsigned offset) {
	uint32_t v;
	memcpy(&v, (const char *)p + offset, 4);
	return v;
}
static void **object_hash;

static void *object_find(int id) {
	void *h = *object_hash;
	if (!h || !word(h, 0))
		return NULL;
	uintptr_t node = word((void *)(uintptr_t)word(h, 0), ((unsigned)id & word(h, 4)) * 8);
	for (unsigned trips = 0; node && trips < 4096; trips++) {
		if ((int)word((void *)node, 8) == id)
			return (void *)(uintptr_t)word((void *)node, 12);
		node = word((void *)node, 4);
	}
	return NULL;
}

enum { V_HELD, V_PRESSED, V_KEYSTATE, V_DOWN, V_SWITCH, V_POSX, V_POSY, V_MOVE, V_WALK, V_MAININPUT, V_TAPPED,
	V_SCREENHELD, V_LAUNCH, V_TRIGGER, V_WASNEUTRAL, V_COUNT };
static const char *const var_names[V_COUNT] = { "isHeld", "isPressed", "keyState", "buttonDown", "switchOn",
	"posValueX", "posValueY", "moveAmnt", "walking", "mainInput", "screenTapped", "screenHeld", "launchGo",
	"triggerLaunch", "wasNeutral" };
static int *vars[V_COUNT];

// rest: 0 false (or unset), 1 == 0, 2 < 1, 3 == -4, 4 == 20
static void check(void *inst, int v, int rest) {
	int s = touch_ab_original;
	gml_rvalue *r = gml_vget(inst, vars[v][1]);
	verify_values[s]++;
	if (!r || r->kind == 0xffffff || r->kind == 5) {
		if (rest == 0)
			return; // never set: reads as unset like before
		verify_unknown[s]++;
		return;
	}
	if (r->kind != 0 && r->kind != 13 && r->kind != 7) {
		verify_unknown[s]++;
		return;
	}
	double d = r->kind == 7 ? (double)(int32_t)word(r, 0) : r->val;
	int ok = rest == 0 ? !(d > 0.5) : rest == 1 ? d == 0.0 : rest == 2 ? d < 1.0 : rest == 3 ? d == -4.0 : d == 20.0;
	if (!ok) {
		verify_bad[s]++;
		bad_what[v]++;
	}
}

static void verify_object(int id, const int *list, int n, const int *rest) {
	void *obj = object_find(id);
	if (!obj)
		return;
	uintptr_t node = word(obj, 0x44);
	for (unsigned trips = 0; node && trips < 256; trips++) {
		void *p = (void *)(uintptr_t)word((void *)node, 8);
		if (p && !(word(p, 0x7c) & 0x100003))
			for (int i = 0; i < n; i++)
				check(p, list[i], rest[i]);
		node = word((void *)node, 0);
	}
}

// wasNeutral reads its Create value 0 once, on the frame the keyboard's first user event creates the stick (this
// check runs before the stick's first Step): one "not at rest: wasNeutral" per session is expected.
static void verify_rest(void) {
	static const int btn[] = { V_HELD, V_PRESSED, V_KEYSTATE, V_DOWN, V_SWITCH }, btn_rest[] = { 0, 0, 2, 0, 0 };
	static const int stick[] = { V_POSX, V_POSY, V_MOVE, V_WALK, V_DOWN, V_WASNEUTRAL },
		stick_rest[] = { 1, 1, 1, 0, 0, 4 };
	static const int kb[] = { V_MAININPUT, V_TAPPED, V_SCREENHELD, V_LAUNCH, V_TRIGGER }, kb_rest[] = { 3, 0, 0, 0, 0 };
	verify_frames[touch_ab_original]++;
	verify_object(OBJ_SCREEN_BTN, btn, 5, btn_rest);
	verify_object(OBJ_DEBUG_KEY, btn, 5, btn_rest);
	verify_object(OBJ_REPLAY_BTN, btn, 5, btn_rest);
	verify_object(OBJ_JOYSTICK, stick, 6, stick_rest);
	verify_object(OBJ_KEYBOARD, kb, 5, kb_rest);
}

static event_func orig_keyboard_step;
static void keyboard_step(void *self, void *other) {
	orig_keyboard_step(self, other);
	verify_rest();
}

void touch_controls_report(int frames) {
	if (touch_controls || frames <= 0)
		return;
	if (touch_ab || touch_verify || skipped_btn_step || ran_user0) {
		char verify[160] = "";
		if (touch_verify)
			snprintf(verify, sizeof(verify), " | verify (skipping / original): frames %u / %u, values %u / %u, "
				"not at rest %u / %u, unknown %u / %u", verify_frames[0], verify_frames[1], verify_values[0],
				verify_values[1], verify_bad[0], verify_bad[1], verify_unknown[0], verify_unknown[1]);
		debugPrintf("[TOUCH] skipped per frame: button Step %.1f, button GUI %.1f, stick %.1f, replay buttons %.1f, "
			"keyboard user 0 %.1f (ran %u)%s%s\n", (float)skipped_btn_step / frames, (float)skipped_btn_gui / frames,
			(float)skipped_stick / frames, (float)skipped_replay / frames, (float)skipped_user0 / frames, ran_user0,
			verify, touch_ab ? (touch_ab_original ? " | A/B: off" : " | A/B: on") : "");
		for (int i = 0; i < V_COUNT; i++)
			if (bad_what[i])
				debugPrintf("[TOUCH] not at rest: %s x%u (session)\n", var_names[i], bad_what[i]);
	}
	skipped_btn_step = skipped_btn_gui = skipped_stick = skipped_replay = skipped_user0 = ran_user0 = 0;
	if (touch_ab)
		touch_ab_original = !touch_ab_original;
}

// Called from patch_performance after the screenBtn GUI Draw patch (katana.c)
void touch_controls_install(gml_func_entry *table) {
	if (touch_controls) {
		debugPrintf("touch controls: on (%s)\n", touch_controls >= 2 ? "drawn" : "not drawn");
		return;
	}
	var_setup = (int *)so_symbol(&yoyoloader_mod, "g_VAR_onScreenControlSetup");
	var_was_neutral = (int *)so_symbol(&yoyoloader_mod, "g_VAR_wasNeutral");
	object_hash = (void **)so_symbol(&yoyoloader_mod, "g_ObjectHash");
	int ok = var_setup != NULL;
	for (int i = 0; i < V_COUNT; i++) {
		char name[48] = "g_VAR_";
		strcat(name, var_names[i]);
		vars[i] = (int *)so_symbol(&yoyoloader_mod, name);
	}
	extern int debugMode;
	touch_ab = debugMode && exp_flag("touchab");
	touch_verify = debugMode && exp_flag("touchverify") && object_hash;
	for (int i = 0; touch_verify && i < V_COUNT; i++)
		touch_verify = vars[i] != NULL;
	event_func orig;
	if (ok && (orig = gml_replace(table, "gml_Object_obj_onScreenKeyboard_Other_10", keyboard_user0)))
		orig_user0 = orig;
#define REPLACE(name, event) \
	if ((orig = gml_replace(table, event, skip_##name))) \
		orig_##name = orig;
	REPLACE(btn_step, "gml_Object_obj_screenBtn_Step_0")
	REPLACE(btn_gui, "gml_Object_obj_screenBtn_Draw_64")
	REPLACE(stick_step, "gml_Object_obj_screenJoystick_Step_0")
	REPLACE(stick_gui, "gml_Object_obj_screenJoystick_Draw_64")
	REPLACE(replay_step, "gml_Object_obj_replayBtns_Step_0")
	REPLACE(replay_draw, "gml_Object_obj_replayBtns_Draw_0")
#undef REPLACE
	if (touch_verify && (orig = gml_replace(table, "gml_Object_obj_onScreenKeyboard_Step_0", keyboard_step)))
		orig_keyboard_step = orig;
	debugPrintf("touch controls: off (keyboard user 0 %s; button, stick and replay button events skipped, stick "
		"wasNeutral %s)%s%s\n", orig_user0 ? "after setup skipped" : "NOT hooked",
		var_was_neutral ? "kept at 20" : "NOT found", touch_ab ? ", A/B" : "", touch_verify ? ", verify" : "");
}
