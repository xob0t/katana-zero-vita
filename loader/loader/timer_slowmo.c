/*
 * timer_slowmo.c
 *
 * Level time limits and Zero's slow-mo meter of the PC version (TIMER & SLOW-MO: PC, the default). ANDROID keeps the
 * Netflix release's longer ones. GAME menu row TIMER & SLOW-MO (katana_menu.c), or timerSlowmo=1 in yyl.cfg.
 *
 * Found by comparing Katana ZERO.exe from Steam (YYC x86) with libyoyo.so of APK 1.1.10 (work/timingaudit,
 * work/pcmelee). Android raises two constants:
 * - global.max_time, the level's time limit (global.time counts down from it; obj_hud Draw's timer bar shows
 *   time / max_time, obj_master kills the player at time <= 0). Every statement that sets it assigns a constant:
 *   the level controllers' Create and Step events (obj_cutscene_<level>, 82 events, 88 statements: Create sets the
 *   limit and global.time = global.max_time, Step sets both again when the level starts after its intro), obj_master
 *   Create (the default) and the global init (obj_init Create, and on Android also mobile_reset_game_data and
 *   xbox_reset_game_data). Android's value is 1.5 times PC's in each, except club_0 (8100 -> 11850):
 *     PC 6000 7000 8000 8100 8300 8600 9000 (levels), 4000 (obj_master), 6000 (init)
 *     AN 9000 10500 12000 11850 12450 12900 13500 (levels), 6000 (obj_master), 9000 (init)
 *   The other readers (obj_hud Draw, obj_master, obj_drug_sequence, the prison_1 controllers, obj_bunker_elevator,
 *   obj_mansionrobber) are the same code in both builds, and 9000 is also a PC level limit, so the values can't be
 *   mapped after the fact: the statements themselves are changed.
 * - bullettime_max, the slow-mo meter's length: obj_player Other_13 (the katana setup, run at every spawn through
 *   Other_11) sets 760, PC 380 (L11 / PC L9), then bullettime_timer = bullettime_max. The drain (-1 per slow-mo frame,
 *   bullettime_max -= 0.1), the recharge (+0.5), the katana modifiers (* 0.6, * 0.8, + 60), the dragon's += 120 and
 *   the HUD meter (round(bullettime / bullettime_max * 11) segments) are the same code in both builds and work from it.
 *
 * Each of these statements builds its constant's high word with movw/movt (the low word is 0) and stores it, e.g.
 * chinatown_2 Create L27:
 *     movw r1, #0x8200; mov r0, #0; movt r1, #0x40c4; str r0, [r8, #0xc]; strd r0, r1, [r8]   (10500.0)
 * The table below lists the movw and movt of every one, found by tools/pc-vs-android/timer_sites/const_sites.py
 * (register and stack-slot tracking from the getVar of max_time to the store; three by hand: club_7 Create, dragon_6
 * Step, obj_init Create) and checked against the PC exe (gen_table.py: the PC event assigns the PC value and not the
 * Android one). A scan of all of libyoyo.so's code (scan_all.py, scan_lit.py) finds no other movw/movt pair or literal
 * of these values in code, apart from two 6000s that aren't time limits (draw_speedrun_timer, club_0 Step's
 * movement_max, 6000 on PC too). PC values rewrite the two immediates; ANDROID writes the originals back. At install
 * every site must hold the 1.1.10 instructions, else nothing is patched.
 *
 * A change applies from the next time the statement runs: the time limit when the level (or its controller's Step
 * after the intro) sets it, so the next room or a restart; the slow-mo meter at the next spawn (restart, next room).
 * The code is rewritten between frames (options_menu_sync, katana_after_swap), never while GML runs. obj_init runs
 * before load_options, so the boot default follows yyl.cfg's timerSlowmo, not the saved menu choice; nothing reads it
 * before obj_master Create (in every gameplay room) or the level sets its own limit.
 */
#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

int timer_slowmo_cfg = 0; // timerSlowmo in yyl.cfg: 0 PC, 1 ANDROID
static int applied = -1;  // what the code holds: 0 PC, 1 ANDROID, -1 not installed

// ELF addresses of the movw and movt, the Android value and the PC value of the same statement
static const struct { uint32_t movw, movt; uint16_t android, pc; } sites[] = {
	{ 0x33ba2d8, 0x33ba2dc,  9000, 6000 }, // Script_xbox_reset_game_data
	{ 0x33c5dc0, 0x33c5dc4,  9000, 6000 }, // Script_mobile_reset_game_data
	{ 0x385f3d8, 0x385f3e0,  9000, 6000 }, // Object_obj_cutscene_bunker_boss_2_Create_0
	{ 0x38637d4, 0x38637dc,  9000, 6000 }, // Object_obj_cutscene_bunker_boss_final_Create_0
	{ 0x386a9b8, 0x386a9c0, 10500, 7000 }, // Object_obj_cutscene_bunker_boss_Create_0
	{ 0x386e200, 0x386e208, 10500, 7000 }, // Object_obj_cutscene_bunker_boss_Step_0
	{ 0x387d458, 0x387d460, 12000, 8000 }, // Object_obj_cutscene_bunkermansion_1_Create_0
	{ 0x3880ed0, 0x3880ed8, 12900, 8600 }, // Object_obj_cutscene_bunkermansion_big_Create_0
	{ 0x388209c, 0x38820a4, 12900, 8600 }, // Object_obj_cutscene_bunkermansion_big_Step_0
	{ 0x389aad0, 0x389aad8, 10500, 7000 }, // Object_obj_cutscene_chinatown_1_Create_0
	{ 0x389fd34, 0x389fd3c, 10500, 7000 }, // Object_obj_cutscene_chinatown_1_Step_0
	{ 0x38a809c, 0x38a80a4, 10500, 7000 }, // Object_obj_cutscene_chinatown_1_Step_0
	{ 0x38bd68c, 0x38bd694, 10500, 7000 }, // Object_obj_cutscene_chinatown_2_Create_0
	{ 0x38bef54, 0x38bef5c, 10500, 7000 }, // Object_obj_cutscene_chinatown_2_Step_0
	{ 0x38c0154, 0x38c015c, 10500, 7000 }, // Object_obj_cutscene_chinatown_3_Create_0
	{ 0x38c1304, 0x38c130c, 10500, 7000 }, // Object_obj_cutscene_chinatown_3_Step_0
	{ 0x38c2160, 0x38c2168, 10500, 7000 }, // Object_obj_cutscene_chinatown_4_Create_0
	{ 0x38c3558, 0x38c3560, 10500, 7000 }, // Object_obj_cutscene_chinatown_4_Step_0
	{ 0x38c51c0, 0x38c51c8, 10500, 7000 }, // Object_obj_cutscene_chinatown_5_Create_0
	{ 0x38c6744, 0x38c674c, 10500, 7000 }, // Object_obj_cutscene_chinatown_5_Step_0
	{ 0x38caacc, 0x38caad4, 10500, 7000 }, // Object_obj_cutscene_chinatown_6_Create_0
	{ 0x38ccc14, 0x38ccc1c, 10500, 7000 }, // Object_obj_cutscene_chinatown_6_Step_0
	{ 0x38cda94, 0x38cda9c, 10500, 7000 }, // Object_obj_cutscene_chinatown_7_Create_0
	{ 0x38cec58, 0x38cec60, 10500, 7000 }, // Object_obj_cutscene_chinatown_7_Step_0
	{ 0x38d0888, 0x38d0890, 10500, 7000 }, // Object_obj_cutscene_chinatown_8_Create_0
	{ 0x38d1a74, 0x38d1a7c, 10500, 7000 }, // Object_obj_cutscene_chinatown_8_Step_0
	{ 0x38d3f14, 0x38d3f1c, 10500, 7000 }, // Object_obj_cutscene_chinatown_9_Create_0
	{ 0x38d5134, 0x38d513c, 10500, 7000 }, // Object_obj_cutscene_chinatown_9_Step_0
	{ 0x38d810c, 0x38d8114, 10500, 7000 }, // Object_obj_cutscene_chinatown_basic_Create_0
	{ 0x38d92d8, 0x38d92e0, 10500, 7000 }, // Object_obj_cutscene_chinatown_basic_Step_0
	{ 0x38ea71c, 0x38ea724, 11850, 8100 }, // Object_obj_cutscene_club_0_Create_0
	{ 0x38ece0c, 0x38ece14, 11850, 8100 }, // Object_obj_cutscene_club_0_Step_0
	{ 0x3905860, 0x3905868, 12450, 8300 }, // Object_obj_cutscene_club_4_2_Create_0
	{ 0x3907490, 0x3907498, 12450, 8300 }, // Object_obj_cutscene_club_4_2_Step_0
	{ 0x3921cb4, 0x3921cbc,  9000, 6000 }, // Object_obj_cutscene_club_7_Create_0
	{ 0x3926eec, 0x3926ef4,  9000, 6000 }, // Object_obj_cutscene_club_7_Step_0
	{ 0x392f150, 0x392f158, 10500, 7000 }, // Object_obj_cutscene_dragon_1_Create_0
	{ 0x3933b38, 0x3933b40, 10500, 7000 }, // Object_obj_cutscene_dragon_1_Step_0
	{ 0x39389b8, 0x39389c0, 10500, 7000 }, // Object_obj_cutscene_dragon_2_Create_0
	{ 0x3939fd0, 0x3939fd8, 10500, 7000 }, // Object_obj_cutscene_dragon_2_Step_0
	{ 0x393ae1c, 0x393ae24, 10500, 7000 }, // Object_obj_cutscene_dragon_5_Create_0
	{ 0x393c430, 0x393c438, 10500, 7000 }, // Object_obj_cutscene_dragon_5_Step_0
	{ 0x393d27c, 0x393d284, 10500, 7000 }, // Object_obj_cutscene_dragon_6_Create_0
	{ 0x393e6e0, 0x393e6e8, 10500, 7000 }, // Object_obj_cutscene_dragon_6_Step_0
	{ 0x393f8a0, 0x393f8a8, 10500, 7000 }, // Object_obj_cutscene_dragon_7_Create_0
	{ 0x3940b90, 0x3940b98, 10500, 7000 }, // Object_obj_cutscene_dragon_7_Step_0
	{ 0x3941914, 0x394191c, 10500, 7000 }, // Object_obj_cutscene_dragon_8_Create_0
	{ 0x3942f2c, 0x3942f34, 10500, 7000 }, // Object_obj_cutscene_dragon_8_Step_0
	{ 0x3943d78, 0x3943d80, 10500, 7000 }, // Object_obj_cutscene_dragon_basic_Create_0
	{ 0x3945288, 0x3945290, 10500, 7000 }, // Object_obj_cutscene_dragon_basic_Step_0
	{ 0x39a9c9c, 0x39a9ca4, 10500, 7000 }, // Object_obj_cutscene_factory2_1_Create_0
	{ 0x39abddc, 0x39abde4, 10500, 7000 }, // Object_obj_cutscene_factory2_1_Step_0
	{ 0x39bb858, 0x39bb860,  9000, 6000 }, // Object_obj_cutscene_factory2_4_2_Create_0
	{ 0x39bd0c0, 0x39bd0c8,  9000, 6000 }, // Object_obj_cutscene_factory2_4_2_Step_0
	{ 0x39be718, 0x39be720,  9000, 6000 }, // Object_obj_cutscene_factory2_5_Create_0
	{ 0x39bf9b8, 0x39bf9c0,  9000, 6000 }, // Object_obj_cutscene_factory2_5_Step_0
	{ 0x39c1980, 0x39c1988, 13500, 9000 }, // Object_obj_cutscene_factory2_6_Create_0
	{ 0x39c9c14, 0x39c9c1c, 13500, 9000 }, // Object_obj_cutscene_factory2_6_Step_0
	{ 0x39cd59c, 0x39cd5a4, 13500, 9000 }, // Object_obj_cutscene_factory2_6_Step_0
	{ 0x39d3844, 0x39d384c, 13500, 9000 }, // Object_obj_cutscene_factory2_6_Step_0
	{ 0x39db7ac, 0x39db7b4, 13500, 9000 }, // Object_obj_cutscene_factory2_6_Step_0
	{ 0x39edff4, 0x39edffc, 13500, 9000 }, // Object_obj_cutscene_factory2_6_Other_10
	{ 0x39f1990, 0x39f1998, 13500, 9000 }, // Object_obj_cutscene_factory2_6_Other_10
	{ 0x3a05184, 0x3a0518c, 10500, 7000 }, // Object_obj_cutscene_factory2_7_Create_0
	{ 0x3a075ac, 0x3a075b4, 10500, 7000 }, // Object_obj_cutscene_factory2_7_Step_0
	{ 0x3a110d8, 0x3a110e0, 10500, 7000 }, // Object_obj_cutscene_factory2_8_Create_0
	{ 0x3a133dc, 0x3a133e4, 10500, 7000 }, // Object_obj_cutscene_factory2_8_Step_0
	{ 0x3a1c510, 0x3a1c518, 10500, 7000 }, // Object_obj_cutscene_factory2_9_Create_0
	{ 0x3a1e60c, 0x3a1e614, 10500, 7000 }, // Object_obj_cutscene_factory2_9_Step_0
	{ 0x3a26d68, 0x3a26d70, 10500, 7000 }, // Object_obj_cutscene_factory2_basic_Create_0
	{ 0x3a28008, 0x3a28010, 10500, 7000 }, // Object_obj_cutscene_factory2_basic_Step_0
	{ 0x3a351f0, 0x3a351f8, 10500, 7000 }, // Object_obj_cutscene_govtlab_0_Create_0
	{ 0x3a61a40, 0x3a61a48,  9000, 6000 }, // Object_obj_cutscene_haunted_alien1_Create_0
	{ 0x3a63b5c, 0x3a63b64, 10500, 7000 }, // Object_obj_cutscene_haunted_alien2_Create_0
	{ 0x3a6f148, 0x3a6f150,  9000, 6000 }, // Object_obj_cutscene_haunted_cart1_Create_0
	{ 0x3a74a54, 0x3a74a5c, 10500, 7000 }, // Object_obj_cutscene_haunted_cart2_Create_0
	{ 0x3a941dc, 0x3a941e4,  9000, 6000 }, // Object_obj_cutscene_haunted_light1_Create_0
	{ 0x3a96d54, 0x3a96d5c, 10500, 7000 }, // Object_obj_cutscene_haunted_light2_Create_0
	{ 0x3ac6974, 0x3ac697c, 10500, 7000 }, // Object_obj_cutscene_mansion_3_Create_0
	{ 0x3ac8144, 0x3ac814c, 10500, 7000 }, // Object_obj_cutscene_mansion_3_Step_0
	{ 0x3acae70, 0x3acae78, 10500, 7000 }, // Object_obj_cutscene_mansion_4_Create_0
	{ 0x3acc468, 0x3acc470, 10500, 7000 }, // Object_obj_cutscene_mansion_4_Step_0
	{ 0x3ae78d0, 0x3ae78d8, 10500, 7000 }, // Object_obj_cutscene_mansion_bikechase_Step_0
	{ 0x3b1d2e0, 0x3b1d2e8,  9000, 6000 }, // Object_obj_cutscene_motel_6_Create_0
	{ 0x3b1f6b8, 0x3b1f6c0,  9000, 6000 }, // Object_obj_cutscene_motel_6_Step_0
	{ 0x3b62554, 0x3b6255c, 12900, 8600 }, // Object_obj_cutscene_prison_0_return_Create_0
	{ 0x3b64568, 0x3b64570, 12900, 8600 }, // Object_obj_cutscene_prison_0_return_Step_0
	{ 0x3c3e380, 0x3c3e388,  9000, 6000 }, // Object_obj_cutscene_studio_finale_Create_0
	{ 0x3c78dc0, 0x3c78dc8, 10500, 7000 }, // Object_obj_cutscene_tiedup_Step_0
	{ 0x3c7ee7c, 0x3c7ee84, 10500, 7000 }, // Object_obj_cutscene_tiedup_Step_0
	{ 0x41c9cc8, 0x41c9cd0,  9000, 6000 }, // Object_obj_init_Create_0
	{ 0x4365778, 0x436577c,  6000, 4000 }, // Object_obj_master_Create_0
	{ 0x462e0e4, 0x462e0ec,   760,  380 }, // Object_obj_player_Other_13
};
#define NSITES (sizeof(sites) / sizeof(sites[0]))

#define AT(elf) ((uint32_t *)(yoyoloader_mod.text_base + (elf)))

// movw/movt rd, #imm16 with rd and the condition of the original word
static uint32_t with_imm16(uint32_t word, uint32_t imm) {
	return (word & 0xfff0f000) | ((imm >> 12) << 16) | (imm & 0xfff);
}

// The high word of a whole number below 2^16 as a double (the low word is 0 for all of them)
static uint32_t high_word(unsigned v) {
	union { double d; uint64_t u; } x = { (double)v };
	return (uint32_t)(x.u >> 32);
}

static int site_ok(int k) {
	uint32_t w = *AT(sites[k].movw), t = *AT(sites[k].movt), hi = high_word(sites[k].android);
	return (w & 0x0ff00000) == 0x03000000 && (t & 0x0ff00000) == 0x03400000 && ((w ^ t) & 0x0000f000) == 0 &&
		w == with_imm16(w, hi & 0xffff) && t == with_imm16(t, hi >> 16);
}

static void write_word(uint32_t *at, uint32_t word) {
	if (*at == word)
		return;
	kuKernelCpuUnrestrictedMemcpy(at, &word, 4);
	kuKernelFlushCaches(at, 4);
}

// Puts the PC (0) or Android (1) values in every site; only between frames
void timer_slowmo_apply(int android) {
	android = android != 0;
	if (applied < 0 || applied == android)
		return;
	for (unsigned k = 0; k < NSITES; k++) {
		uint32_t hi = high_word(android ? sites[k].android : sites[k].pc);
		write_word(AT(sites[k].movw), with_imm16(*AT(sites[k].movw), hi & 0xffff));
		write_word(AT(sites[k].movt), with_imm16(*AT(sites[k].movt), hi >> 16));
	}
	applied = android;
	debugPrintf("timer & slow-mo: %s time limits and slow-mo meter\n", android ? "ANDROID" : "PC");
}

int timer_slowmo_available(void) {
	return applied >= 0;
}

void timer_slowmo_install(void) {
	unsigned bad = 0;
	for (unsigned k = 0; k < NSITES; k++)
		if (!site_ok(k)) {
			if (bad++ < 4)
				debugPrintf("timer & slow-mo: site %u (0x%07x) is not the 1.1.10 instruction\n", k, (unsigned)sites[k].movw);
		}
	if (bad) {
		debugPrintf("timer & slow-mo: NOT installed (%u of %u sites differ)\n", bad, (unsigned)NSITES);
		return;
	}
	applied = 1; // the code as loaded
	timer_slowmo_apply(timer_slowmo_cfg);
	debugPrintf("timer & slow-mo: %u sites (yyl.cfg timerSlowmo %d)\n", (unsigned)NSITES, timer_slowmo_cfg);
}
