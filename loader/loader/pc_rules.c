/*
 * pc_rules.c
 *
 * Slash, smoke and death slow-mo rules of the PC version, under the GAME menu's ENEMY TIMING switch (enemy_timing.c):
 * PC (the default) plays them as the Steam build, ANDROID keeps the Netflix release's. Each was compared in both
 * binaries (Katana ZERO.exe from Steam, YYC x86, and libyoyo.so of APK 1.1.10); addresses are ELF addresses in the
 * APK's libyoyo.so. The enemy melee cooldown (whip_attack_timer) is in enemy_timing.c with the other AI_init timers.
 *
 * 1. enemy_hurt (a katana slash reaching an enemy). Android wraps the slash's line-of-sight checks
 *    (collision_line(x, y - 32, hit.x +- 10, hit.y +- 10, obj_block), lines 18-27) in
 *        with (obj_tempdoor) y -= 100000;   ...checks...   with (obj_tempdoor) y += 100000;
 *    (lines 11-13 and 30-32), so the temporary door a broken door leaves behind (obj_tempdoor, a child of obj_block)
 *    doesn't block the kill. PC has no with loop in the function before line 53 and the door blocks. PC: each loop's
 *    guard after YYGML_NewWithIterator, `cmp r4, #1; blt <DeleteWithIterator>` (0x1993f50, 0x1995fb0), becomes an
 *    unconditional b to the same address: the body never runs, as if the loop began with a break (what YYC compiles
 *    a with's break to), so the doors stay where they are and DeleteWithIterator puts self and other back. ANDROID
 *    writes the blt back. (Veneers can't reach this function: it is too far from the code cave.)
 *
 * 2. obj_slash Collision with obj_door. When the slash opens the door, PC sets harmless = 1 at the end of the event
 *    (L49): the slash's next Step makes it harmless (L48 if (harmless) deadly = false). Android sets
 *    trigger_harmless = 2 instead (L54), which its Step counts down while the slash is deadly (L53-58), so the slash
 *    stays deadly 2-3 more frames and can kill an enemy right behind the door. Android also saves image_angle and
 *    image_xscale on entry (L3-4) and puts them back at the end if the slash was turned around (L58-61:
 *    if (change) { image_angle = saved; image_xscale = saved }); PC keeps the turned values (L10-11 image_angle -= 180,
 *    image_xscale = -1, the same lines as Android's L13-14). PC: the event runs as compiled, the restore's
 *    BOOL_RValue(change) (0x48d5384) goes through a veneer that answers false, and after the event a
 *    trigger_harmless of 2 (what L54 stores when the door opens) becomes harmless = 1 with trigger_harmless 0. Nothing
 *    after L54 in the event reads harmless or trigger_harmless, and trigger_harmless has no other reader than the
 *    Step's countdown, which a 0 turns off.
 *
 * 3. Smoke on an enemy in the air. obj_smokescreen_master Step (L20-26, Android only) sets falling_smoke_timer = 3
 *    for an enemy with vsp != 0 that the smoke loses track of, and ai_break_idle (L6-13) then leaves vsp alone until
 *    the timer runs out (falling_smoke_timer -= fr). PC's ai_break_idle zeroes vsp at once (L4 vsp = 0, in the same
 *    idle-state branch). Only ai_break_idle reads the variable, so PC: ai_break_idle starts with falling_smoke_timer
 *    0 when it is above 0, and its own test then takes the vsp = 0 branch.
 *
 * 4. The death slow-mo, obj_master Step (Android obj_master Other_10, run by obj_timeMaster):
 *        PC L105-118:      gameover_timer += 1; if (gameover_timer > 5) { if (gameover_slowdown > 0.2)
 *                          gameover_slowdown -= 0.1; gameover_slowdown -= gsp; gsp += 0.0001; if (<= 0) = 0 }
 *        Android L111-124: gameover_timer += 1; if (global.fr != 0) { ... -= 0.12; ... gsp += 0.00015; ... }
 *    The block's gate differs too (the timing audit only listed the constants): PC runs it from the 6th frame on, also
 *    during the death's hit freeze (fr 0), Android every frame that isn't frozen, so in practice only after the freeze
 *    (Vita3K: the ramp starts at gameover_timer 6 on PC, about 21 on Android). Later in the same event (L160)
 *    global.slowdown follows gameover_slowdown, so the values must be right inside the event. PC: three calls go through veneers. The gate's YYCompareVal(fr, 0)
 *    (0x4369e18) answers whether gameover_timer > 5 (YYCompareVal(gameover_timer, 5) >= 1, as the event's own > tests;
 *    the event's instance comes from a wrapper on Other_10); operator-= with the 0.12 RValue (0x4369f1c) subtracts
 *    0.1; operator+=(0.00015) (0x4369fc0) adds 0.0001. Same values in the same order as PC's statements.
 *
 * Each part installs only if every instruction it relies on is the 1.1.10 one, and logs a line. A change of the
 * setting applies on the next frame for all four (pc_rules_apply, from options_menu_sync between frames).
 */
#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

#define ABI __attribute__((pcs("aapcs")))

extern int enemy_timing_android; // enemy_timing.c: 0 PC rules, 1 Android rules

#define AT(elf) (yoyoloader_mod.text_base + (elf))
#define WORD(elf) (*(volatile uint32_t *)AT(elf))

// A bl at an ELF address, sent through an 8-byte veneer (ldr pc, [pc, #-4]; .word wrapper) to a wrapper. word is the
// APK's bl (to a linker veneer or the PLT); callee the function it reaches, which scripts/patch_plt_calls.py may have
// made the direct target instead.
typedef struct { uint32_t site, word; const char *callee; void *wrapper; uint32_t *veneer; } bl_patch;

static int bl_reaches(uintptr_t site, uintptr_t target) {
	int64_t delta = (int64_t)target - (int64_t)site - 8;
	return !(delta & 3) && delta >= -(1 << 25) && delta < (1 << 25);
}

static uintptr_t bl_target(uintptr_t site, uint32_t word) {
	return site + 8 + (uintptr_t)((int32_t)(word << 8) >> 6);
}

// The site holds the APK's bl, or the direct bl to callee that patch_plt_calls.py makes of it
static int is_call(uint32_t site, uint32_t word, const char *callee) {
	uint32_t w = WORD(site);
	if (w == word)
		return 1;
	uintptr_t direct = so_symbol(&yoyoloader_mod, callee);
	return (w >> 24) == 0xeb && direct && bl_target(AT(site), w) == direct;
}

// Every site holds its 1.1.10 call and has a veneer in reach: then all are patched, else none
static int patch_bls(bl_patch *p, int n) {
	for (int k = 0; k < n; k++)
		if (!is_call(p[k].site, p[k].word, p[k].callee)) {
			debugPrintf("pc rules: 0x%07x holds %08x, not the 1.1.10 call\n", (unsigned)p[k].site,
				(unsigned)WORD(p[k].site));
			return 0;
		}
	for (int k = 0; k < n; k++) {
		p[k].veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, AT(p[k].site), 8);
		if (!p[k].veneer || !bl_reaches(AT(p[k].site), (uintptr_t)p[k].veneer))
			return 0;
	}
	for (int k = 0; k < n; k++) {
		uint32_t code[2] = {0xe51ff004, (uint32_t)(uintptr_t)p[k].wrapper};
		kuKernelCpuUnrestrictedMemcpy(p[k].veneer, code, sizeof(code));
		kuKernelFlushCaches(p[k].veneer, sizeof(code));
		int32_t off = (int32_t)((uintptr_t)p[k].veneer - (AT(p[k].site) + 8));
		uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
		kuKernelCpuUnrestrictedMemcpy((void *)AT(p[k].site), &bl, 4);
		kuKernelFlushCaches((void *)AT(p[k].site), 4);
	}
	return 1;
}

// The 1.1.10 words at a list of ELF addresses (the context a patch relies on)
static int words_are(const uint32_t (*w)[2], int n) {
	for (int k = 0; k < n; k++)
		if (WORD(w[k][0]) != w[k][1]) {
			debugPrintf("pc rules: 0x%07x holds %08x, not the 1.1.10 %08x\n", (unsigned)w[k][0],
				(unsigned)WORD(w[k][0]), (unsigned)w[k][1]);
			return 0;
		}
	return 1;
}

// self.var = val as a real, whatever kind it held (harmless starts as a bool)
static void set_real(void *self, int *var, double val) {
	gml_rvalue *(*vwrite)(void *, int) = (*(void ***)self)[3];
	gml_rvalue *v = vwrite(self, var[1]);
	if (!v || ((1u << (v->kind & 31)) & 0x46)) // never over a string, array or object
		return;
	v->val = val;
	v->flags = 0;
	v->kind = VALUE_REAL;
}

// ---- 1. enemy_hurt: no with (obj_tempdoor) around the line-of-sight checks ----

// The two loop guards: blt (cond LT, 0xb) on Android, b (cond AL, 0xe) on PC, same offset
static const uint32_t hurt_guards[2][2] = { {0x1993f50, 0xba000036}, {0x1995fb0, 0xba00002e} };
static int hurt_installed, hurt_android = 1; // what the code holds

static void hurt_apply(int android) {
	if (!hurt_installed || hurt_android == android)
		return;
	for (int k = 0; k < 2; k++) {
		uint32_t w = android ? hurt_guards[k][1] : (hurt_guards[k][1] & 0x0fffffff) | 0xe0000000;
		kuKernelCpuUnrestrictedMemcpy((void *)AT(hurt_guards[k][0]), &w, 4);
		kuKernelFlushCaches((void *)AT(hurt_guards[k][0]), 4);
	}
	hurt_android = android;
}

static int install_tempdoor(void) {
	uintptr_t hurt = so_symbol(&yoyoloader_mod, "_Z21gml_Script_enemy_hurtP9CInstanceS0_R8YYRValueiPPS1_");
	if (hurt != AT(0x19934d0)) {
		debugPrintf("pc rules: enemy_hurt at +0x%x, not the 1.1.10 address\n", (unsigned)(hurt - AT(0)));
		return 0;
	}
	// Line 11: movw r0, #0x2e9; add r6, r0, #0xf8 (0x3e1, obj_tempdoor), kept in [sp, #0x4c] for line 30's target;
	// each loop: bl NewWithIterator; mov r4, r0; ...; cmp r4, #1; blt; and the blt's target holds the Delete call
	static const uint32_t ctx[][2] = { {0x1993f00, 0xe30002e9}, {0x1993f04, 0xe28060f8}, {0x1993f34, 0xe58d604c},
		{0x1995f64, 0xe59d004c}, {0x1993f28, 0xe1a04000}, {0x1993f4c, 0xe3540001}, {0x1995f88, 0xe1a04000},
		{0x1995fac, 0xe3540001}, {0x1994030, 0xe28d00d0}, {0x1996070, 0xe28d00c0} };
	static const char new_with[] = "_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue",
		delete_with[] = "_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_";
	if (!words_are(ctx, sizeof(ctx) / sizeof(ctx[0])) || !words_are(hurt_guards, 2) ||
			!is_call(0x1993f24, 0xeb016385, new_with) || !is_call(0x1995f84, 0xeb015b6d, new_with) ||
			!is_call(0x199403c, 0xeb01634f, delete_with) || !is_call(0x199607c, 0xeb015b3f, delete_with)) {
		debugPrintf("pc rules: enemy_hurt's with loops are not the 1.1.10 code\n");
		return 0;
	}
	hurt_installed = 1;
	return 1;
}

// ---- 2. obj_slash Collision with obj_door: harmless at once, the turned slash stays turned ----

static event_func orig_slash_door;
static int *var_trigger_harmless, *var_harmless;
static int (*bool_rvalue)(const gml_rvalue *);

static void slash_door_pc(void *self, void *other) {
	orig_slash_door(self, other);
	if (enemy_timing_android)
		return;
	gml_rvalue *t = instance_var(self, var_trigger_harmless);
	if (!t || t->val != 2.0)
		return; // the door didn't open (L54 not reached)
	t->val = 0.0;
	set_real(self, var_harmless, 1.0);
}

// L58 if (change): the restore of image_angle and image_xscale
static int ABI slash_door_restore(const gml_rvalue *change) {
	return enemy_timing_android ? bool_rvalue(change) : 0;
}

static int install_slash_door(gml_func_entry *table) {
	var_trigger_harmless = (int *)so_symbol(&yoyoloader_mod, "g_VAR_trigger_harmless");
	var_harmless = (int *)so_symbol(&yoyoloader_mod, "g_VAR_harmless");
	bool_rvalue = (void *)so_symbol(&yoyoloader_mod, "_Z11BOOL_RValuePK6RValue");
	if (!var_trigger_harmless || !var_harmless || !bool_rvalue ||
			so_symbol(&yoyoloader_mod, "_Z39gml_Object_obj_slash_Collision_obj_doorP9CInstanceS0_") != AT(0x48d39ec))
		return 0;
	// L58: mov r0, #0x3a; str r0, [fp, #-0x8c]; ldr r0, [sp, #0xc] (change, read at L11); bl BOOL_RValue; cmp; beq
	static const uint32_t ctx[][2] = { {0x48d5378, 0xe3a0003a}, {0x48d5380, 0xe59d000c}, {0x48d5388, 0xe3500000},
		{0x48d538c, 0x0a00004c}, {0x48d3f8c, 0xe58d000c} };
	bl_patch p[1] = { {0x48d5384, 0xeb076ea1, "_Z11BOOL_RValuePK6RValue", (void *)slash_door_restore} };
	if (!words_are(ctx, 5) || !patch_bls(p, 1))
		return 0;
	orig_slash_door = gml_replace(table, "gml_Object_obj_slash_Collision_obj_door", slash_door_pc);
	return orig_slash_door != NULL;
}

// ---- 3. ai_break_idle: vsp = 0 at once for a smoked enemy in the air ----

static gml_script_fn orig_ai_break_idle;
static int *var_falling_smoke_timer;

static void *ai_break_idle_pc(void *self, void *other, void *result, int argc, void **args) {
	if (!enemy_timing_android && self) {
		gml_rvalue *t = instance_var(self, var_falling_smoke_timer);
		if (t && t->val > 0.0)
			t->val = 0.0;
	}
	return orig_ai_break_idle(self, other, result, argc, args);
}

static int install_smoke_fall(void) {
	var_falling_smoke_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_falling_smoke_timer");
	if (var_falling_smoke_timer)
		orig_ai_break_idle = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z24gml_Script_ai_break_idleP9CInstanceS0_R8YYRValueiPPS1_"), ai_break_idle_pc);
	return orig_ai_break_idle != NULL;
}

// ---- 4. obj_master: the death slow-mo ramp ----

typedef int ABI (*compare_fn)(const gml_rvalue *a, const gml_rvalue *b, double eps, int flag);
typedef void *ABI (*sub_fn)(void *lhs, const gml_rvalue *rhs);
typedef void *ABI (*add_real_fn)(void *lhs, double rhs);
static compare_fn yy_compare;
static sub_fn yy_sub;
static add_real_fn yy_add_real;
static event_func orig_master_user0;
static void *master_self; // the obj_master whose Other_10 is running
static int *var_gameover_timer;
static unsigned gate_fallbacks;

static void master_user0(void *self, void *other) {
	void *prev = master_self;
	master_self = self;
	orig_master_user0(self, other);
	master_self = prev;
}

// L114's if (global.fr != 0); PC: if (gameover_timer > 5)
static int ABI death_gate(const gml_rvalue *a, const gml_rvalue *b, double eps, int flag) {
	if (enemy_timing_android)
		return yy_compare(a, b, eps, flag);
	gml_rvalue *t = master_self ? instance_var(master_self, var_gameover_timer) : NULL;
	if (!t) { // not a real (it always is: Create sets 0, L113 adds 1): the Android gate
		if (gate_fallbacks++ < 4)
			debugPrintf("pc rules: death slow-mo gate without a real gameover_timer, Android gate kept\n");
		return yy_compare(a, b, eps, flag);
	}
	gml_rvalue timer = *t, five = { 5.0, 0, VALUE_REAL };
	return yy_compare(&timer, &five, eps, flag) >= 1;
}

// L118 gameover_slowdown -= 0.12; PC -= 0.1
static void *ABI death_ramp_sub(void *lhs, const gml_rvalue *rhs) {
	if (!enemy_timing_android && rhs->kind == VALUE_REAL && rhs->val == 0.12) {
		gml_rvalue pc = *rhs;
		pc.val = 0.1;
		return yy_sub(lhs, &pc);
	}
	return yy_sub(lhs, rhs);
}

// L121 gsp += 0.00015; PC += 0.0001
static void *ABI death_gsp_add(void *lhs, double rhs) {
	return yy_add_real(lhs, !enemy_timing_android && rhs == 0.00015 ? 0.0001 : rhs);
}

static int install_death_slowmo(gml_func_entry *table) {
	yy_compare = (compare_fn)so_symbol(&yoyoloader_mod, "_Z12YYCompareValRK6RValueS1_db");
	yy_sub = (sub_fn)so_symbol(&yoyoloader_mod, "_ZN8YYRValuemIERKS_");
	yy_add_real = (add_real_fn)so_symbol(&yoyoloader_mod, "_ZN8YYRValuepLEd");
	var_gameover_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_gameover_timer");
	if (!yy_compare || !yy_sub || !yy_add_real || !var_gameover_timer ||
			so_symbol(&yoyoloader_mod, "_Z30gml_Object_obj_master_Other_10P9CInstanceS0_") != AT(0x4367b18))
		return 0;
	// L114 compares [sp, #0x3c] (global.fr, read at 0x4367c6c) with a 0 RValue; L118 builds 0.12 in [fp, #-0x110]
	// (movw/movt 0x3fbeb851 0xeb851eb8); L121 loads 0.00015 from its literal (vldr d16, [pc, #0x2a0]) into r2:r3
	static const uint32_t ctx[][2] = { {0x4369e14, 0xe59d003c}, {0x4367ee8, 0xe58da03c}, {0x4369ef8, 0xe30b1851},
		{0x4369efc, 0xe3431fbe}, {0x4369f04, 0xe3011eb8}, {0x4369f08, 0xe34e1b85}, {0x4369fb8, 0xeddf0ba8},
		{0x4369fbc, 0xec532b30} };
	if (!words_are(ctx, 8) || *(const double *)AT(0x436a260) != 0.00015)
		return 0;
	bl_patch p[3] = { {0x4369e18, 0xeb1d1c80, "_Z12YYCompareValRK6RValueS1_db", (void *)death_gate},
		{0x4369f1c, 0xeb1d1c47, "_ZN8YYRValuemIERKS_", (void *)death_ramp_sub},
		{0x4369fc0, 0xeb1d1c6a, "_ZN8YYRValuepLEd", (void *)death_gsp_add} };
	// The instance first: a patched gate without it falls back to the Android gate
	orig_master_user0 = gml_replace(table, "gml_Object_obj_master_Other_10", master_user0);
	return orig_master_user0 && patch_bls(p, 3);
}

// Between frames: the code-rewriting part follows the switch
void pc_rules_apply(int android) {
	hurt_apply(android != 0);
}

void pc_rules_install(gml_func_entry *table) {
	int tempdoor = install_tempdoor(), slash = install_slash_door(table), smoke = install_smoke_fall(),
		death = install_death_slowmo(table);
	pc_rules_apply(enemy_timing_android);
	debugPrintf("pc rules (ENEMY TIMING %s): slash through temp doors %s, slash vs door %s, smoked enemy in the air %s, "
		"death slow-mo %s\n", enemy_timing_android ? "ANDROID" : "PC", tempdoor ? "on" : "NOT installed",
		slash ? "on" : "NOT installed", smoke ? "on" : "NOT installed", death ? "on" : "NOT installed");
}
