/*
 * letter_freeze.c: dialogue letters never spin on a frozen tick (a game bug that freezes the game).
 *
 * obj_letter's Step starts a wavy (type 2) or rainbow letter with
 *     if ((type == 2 || rainbow) && !set_oscillation) {
 *         while (oscillate_delay > 0) { ...; oscillate_delay -= 1 * global.fr; }
 *         set_oscillation = true;
 *     }
 * With global.fr = 0 the loop never ends. fr is 0 during hit-stop: enemy_hurt sets it the moment a kill lands, in the
 * middle of a tick, and obj_master's user event 0 keeps it 0 while freeze_timer runs. The passive textbox types its
 * letters in its Draw (text_timer -= ...*fr, so only on ticks where time moves); a letter runs its first Step the next
 * tick. When a kill earlier in that tick has stopped time, the game thread spins in the game's own Step forever. Seen
 * in room_club_4_2 test runs (Nov 23 and Nov 25, square pressed every second while Richard's wavy line was typing):
 * the game hung right after a kill, hang.log "event type 3 number 0 object 606 obj_letter". The native Step
 * (letter_native.c) hands every fr <= 0 tick to the game's Step, so that is where it hangs; the game's GML is the same
 * on every platform.
 *
 * Fix: on a tick where fr can't bring the delay down (fr <= 0), a letter that would enter the loop (set_oscillation
 * false, oscillate_delay > 0) runs its Step with set_oscillation reading true, and the flag is put back after. That
 * skips the setup and nothing else: set_oscillation is read only by that condition and written only after the loop. The
 * rest of the Step runs as the game's (with fr = 0 its motion adds zero), and the setup runs at the letter's first
 * Step where time moves. Every other Step goes straight to the original (the native, or the game's Step): an ARM front
 * end keeps the stack word letter_native.c's entry reads (see letter_native_step), so the native stays exact.
 *
 * exp_letterfreeze0: off (the hang comes back). exp_letterfreezerepro (test): right before the first Step of each new
 * wavy or rainbow letter, time stops (global.fr = 0, as a kill does) once per letter, so every typed wavy line
 * exercises the case (outside the title and menu rooms). [LETTERFREEZE] report line (debugMode): setups deferred,
 * repro freezes.
 */
#include <stdint.h>
#include <string.h>
#include <vitasdk.h>
#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

#define ABI __attribute__((pcs("aapcs")))

static int *v_fr, *v_set_osc, *v_delay, *v_type, *v_rainbow, *v_die;
static int guard, repro;
static event_func volatile freeze_orig __attribute__((used));
unsigned letter_freeze_deferred, letter_freeze_repro;

static int number(const gml_rvalue *v, double *out) {
	if (!v)
		return 0;
	switch (v->kind & 0x00ffffff) {
	case 0:
	case 13:
		*out = v->val;
		return 1;
	case 7: {
		int32_t n;
		memcpy(&n, &v->val, 4);
		*out = n;
		return 1;
	}
	case 10: {
		int64_t n;
		memcpy(&n, &v->val, 8);
		*out = (double)n;
		return 1;
	}
	}
	return 0;
}

static gml_rvalue *var(void *owner, int *desc) {
	return owner && desc && desc[1] >= 0 ? gml_vget(owner, desc[1]) : NULL;
}

// exp_letterfreezerepro: stop time before a new wavy/rainbow letter's first Step, once per letter (not in the title
// and menu rooms, so a test reaches its room)
static const char *(*room_name)(int);
static void repro_freeze(void *self, gml_rvalue *fr, double delay) {
	static uint32_t done[64];
	static unsigned next;
	double die, type, rainbow = 0;
	const char *room = room_name && current_room ? room_name(*current_room) : NULL;
	if (!room || strstr(room, "title") || strstr(room, "select") || strstr(room, "menu"))
		return;
	if (!(delay > 0) || !number(var(self, v_die), &die) || die != 0 || !number(var(self, v_type), &type))
		return;
	number(var(self, v_rainbow), &rainbow);
	if (type != 2 && !(rainbow > 0.5))
		return;
	uint32_t id;
	memcpy(&id, (uint8_t *)self + 0x80, 4);
	for (int i = 0; i < 64; i++)
		if (done[i] == id)
			return;
	done[next++ & 63] = id;
	fr->val = 0;
	fr->kind = 0;
	letter_freeze_repro++;
}

// 1: this Step must run with the setup skipped (letter_freeze_run)
int ABI __attribute__((used, noinline)) letter_freeze_check(void *self) {
	if (!global_obj_ptr || !*global_obj_ptr)
		return 0;
	gml_rvalue *osc = var(self, v_set_osc), *fr;
	double f, delay;
	if (!osc || ((osc->kind & 0x00ffffff) != 0 && (osc->kind & 0x00ffffff) != 13) || !(osc->val <= 0.5))
		return 0; // set up already (or not a plain flag)
	if (!number(var(self, v_delay), &delay) || !(delay > 0))
		return 0; // no loop to run (plain letters: delay 0)
	if (!number(fr = var(*global_obj_ptr, v_fr), &f))
		return 0;
	if (repro && f > 0 && (fr->kind & 0x00ffffff) == 0)
		repro_freeze(self, fr, delay), number(fr, &f);
	// the loop ends only if each pass brings the delay down: not for fr <= 0 (or NaN, or too small to count)
	return guard && !(delay - f < delay);
}

void ABI __attribute__((used, noinline)) letter_freeze_run(void *self, void *other) {
	gml_rvalue *osc = var(self, v_set_osc), saved = *osc;
	osc->val = 1;
	freeze_orig(self, other);
	*osc = saved;
	letter_freeze_deferred++;
}

/* The Step's entry. Like letter_native_step, it keeps the word at SP-104 (the stale flags word the compiled Step and
 * the native copy into set_oscillation, spd_fr and fx_on) across the check, then tail-calls the original. */
void ABI __attribute__((naked, target("arm"))) letter_freeze_step(void *self __attribute__((unused)),
	void *other __attribute__((unused))) {
	__asm__ volatile(
		"ldr r2, [sp, #-104]\n"
		"push {r0, r1, r2, lr}\n"
		"bl letter_freeze_check\n"
		"cmp r0, #0\n"
		"pop {r0, r1, r2, lr}\n"
		"str r2, [sp, #-104]\n"
		"ldrne r12, =letter_freeze_run\n"
		"bxne r12\n"
		"ldr r12, =freeze_orig\n"
		"ldr r12, [r12]\n"
		"bx r12\n"
		".ltorg");
}

void letter_freeze_install(gml_func_entry *table) {
	guard = !exp_flag("letterfreeze0");
	repro = exp_flag("letterfreezerepro");
	if (!guard && !repro) {
		debugPrintf("letter freeze guard: off (exp_letterfreeze0)\n");
		return;
	}
#define VAR(v, n) v = (int *)so_symbol(&yoyoloader_mod, "g_VAR_" n)
	VAR(v_fr, "fr");
	VAR(v_set_osc, "set_oscillation");
	VAR(v_delay, "oscillate_delay");
	VAR(v_type, "type");
	VAR(v_rainbow, "rainbow");
	VAR(v_die, "die");
#undef VAR
	room_name = (const char *(*)(int))so_symbol(&yoyoloader_mod, "_Z9Room_Namei");
	event_func orig = NULL;
	if (v_fr && v_set_osc && v_delay && v_type && v_rainbow && v_die && table)
		orig = gml_replace(table, "gml_Object_obj_letter_Step_0", letter_freeze_step);
	freeze_orig = orig;
	debugPrintf("letter freeze guard: %s%s%s\n", orig ? "installed" : "NOT installed", guard ? "" : " (guard off)",
		repro ? ", exp_letterfreezerepro: time stops before each new wavy letter" : "");
}

void letter_freeze_report(int frames) {
	(void)frames;
	if (letter_freeze_deferred || letter_freeze_repro)
		debugPrintf("[LETTERFREEZE] letter setups deferred on frozen ticks %u, repro freezes %u (session)\n",
			letter_freeze_deferred, letter_freeze_repro);
}
