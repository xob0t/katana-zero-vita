/*
 * enemy_half_rate.c
 *
 * Half-rate enemies (enemyHalfRate in yyl.cfg, or the ENEMY AI menu), the scripted-sequence marker, and enemies'
 * search_timeout at 30 ticks/s (combat30.c).
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
 * Half-rate enemies (enemyHalfRate in yyl.cfg: 1 on (default), 0 off; or the ENEMY AI menu, see options_menu_install).
 * Enemy Steps (AI, AI_physics, enemy_hurt: 3-8 ms per tick in combat rooms) run every other tick, which saves
 * 2.3-3.5 ms per tick in heavy rooms.
 * - A Step that runs covers the ticks since the enemy's last one: global.fr is doubled for it (put back after,
 *   unless the Step set fr itself, as enemy_hurt does for hit-stop). Enemy Steps count `search_timeout += 1` per tick
 *   and give up searching once it passes a limit (a `>` test, so a step of two can't skip it): a Step covering two
 *   ticks that advanced it gets the tick it missed.
 * - Half of the enemies run on even ticks and half on odd ones (instance id parity), so every tick carries half of
 *   the AI.
 * - Enemies find their own hits: enemy_hurt, inside their Step, polls place_meeting with obj_slash, obj_projectile
 *   (bullets, throwables), obj_explosion and obj_flamethrower_explosion, at the position AI_physics has just moved
 *   the enemy to. On a skipped tick an enemy with one of those near enough to touch it after that move runs this tick
 *   (fr as it is), so hits land on the tick they would have (eh_hurt_near). Which of them exist is checked for each
 *   enemy: an attack made earlier in the tick (the player's slash) counts for every enemy after it.
 * - Enemies run every tick during scripted sequences (the game drives the player through override_control_init
 *   every tick; such sequences count ticks with exact comparisons), and a Step that didn't run for longer than a skip
 *   (pause, deactivation, the setting just turned on) covers one tick.
 * - At 30 ticks/s (30 FPS IN COMBAT, combat30.c) half-rate is off: every enemy Step runs, covering two 60 Hz ticks
 *   (global.fr is doubled already), so enemies think at 30 Hz, as at half rate, not at 15. A Step that advanced
 *   search_timeout gets the tick it missed. Back at 60, a Step that didn't run as a half-rate one covers one tick.
 * Caveats: enemy movement, aim and reactions update at 30 Hz (sprite animation, alarms and built-in motion stay at
 * 60), and anything an enemy counts in ticks without fr runs at half speed. Playtesting found no difference in enemy
 * behaviour.
 * Debug: the profile report has an [ENEMYHALF] line (steps run and skipped per tick), and enemy deaths
 * (create_corpse) are counted by the kind of Step they happened in.
 *
 * Per enemy (instance id at +0x80) the table holds the tick its Step last ran. A room change or a change of the
 * setting clears it, and entries not seen for EH_STALE ticks are reused (death restarts keep the room, with new ids).
 */
int enemy_half_rate = 1;
int eh_installed; // enemy_half_rate_install found everything half-rate enemies need
#define EH_SLOTS 512
#define EH_STALE 120
unsigned enemyhalf_runs, enemyhalf_doubled, enemyhalf_skips, enemyhalf_forced, enemyhalf_search_ticks; // debug counters
static struct { uint32_t key, last; } eh_table[EH_SLOTS]; // key = id + 1, 0 = empty
static int eh_room = -2;
static int *var_fr, *var_search_timeout;
static int eh_depth;
static runner_function **eh_functions, **eh_current_function;
static const int *eh_fn_collision_rectangle, *eh_fn_instance_exists;
static int eh_collision_rectangle_desc[2]; // no GML calls collision_rectangle (no g_FUNC_): {0, its registry index}
static int *eh_function_count;
typedef int (*builtin_var_get)(void *inst, int index, gml_rvalue *value);
static builtin_var_get eh_bbox[4]; // GV_Bbl, GV_Bbt, GV_Bbr, GV_Bbb
static int *eh_var_timestep, *eh_var_slope_slowdown, *eh_var_slope_tolerance;
static const char *const eh_source_names[] = { "obj_slash", "obj_projectile", "obj_explosion", "obj_flamethrower_explosion" };
#define EH_SOURCES (int)(sizeof(eh_source_names) / sizeof(*eh_source_names))
static int eh_sources[EH_SOURCES], eh_sources_found;
static int (*eh_object_find)(const char *name);

// Scripted sequences: the last tick a cutscene script drove the player (also read by combat30.c, which runs them at
// 60 ticks/s)
uint32_t scripted_tick;
int scripted_hooked;
static gml_script_fn orig_override_control_init;
static void *eh_override_control_init(void *self, void *other, void *result, int argc, void **args) {
	scripted_tick = *frame_count ? *frame_count : 1;
	return orig_override_control_init(self, other, result, argc, args);
}

// Once per frame (katana_after_swap): a change of the setting (GAME menu) starts a fresh table, so no enemy
// covers more than two ticks
void enemy_half_rate_update(void) {
	static int seen = -1;
	if (enemy_half_rate == seen)
		return;
	seen = enemy_half_rate;
	eh_room = -2;
	if (eh_installed)
		debugPrintf("Half-rate enemies %s\n", enemy_half_rate ? "on" : "off");
}

// At the first enemy Step or when the GAME page is built (the object table and the function registry aren't
// loaded at install): the object indices of the hurt sources and collision_rectangle's registry index. Without all
// of them half-rate is off for the session; while the registry is still empty it is asked again later.
static int eh_resolve_sources(void) {
	if (eh_sources_found)
		return 1;
	if (!*eh_functions || *eh_function_count <= 0)
		return 0;
	for (int i = 0; i < *eh_function_count && !eh_fn_collision_rectangle; i++)
		if ((*eh_functions)[i].name && !strcmp((*eh_functions)[i].name, "collision_rectangle")) {
			eh_collision_rectangle_desc[1] = i;
			eh_fn_collision_rectangle = eh_collision_rectangle_desc;
		}
	if (!eh_fn_collision_rectangle) {
		debugPrintf("Half-rate enemies: collision_rectangle not in the function registry, off\n");
		eh_installed = 0;
		return 0;
	}
	int found = 0;
	for (int i = 0; i < EH_SOURCES; i++)
		found += (eh_sources[i] = eh_object_find(eh_source_names[i])) >= 0;
	debugPrintf("Half-rate enemies: hurt sources %d/%d (%s %d, %s %d, %s %d, %s %d)\n", found, EH_SOURCES,
		eh_source_names[0], eh_sources[0], eh_source_names[1], eh_sources[1], eh_source_names[2], eh_sources[2],
		eh_source_names[3], eh_sources[3]);
	if (found != EH_SOURCES) {
		eh_installed = 0;
		return 0;
	}
	return eh_sources_found = 1;
}

// The ENEMY AI row is offered only when half-rate enemies can run (katana_menu.c)
int eh_available(void) {
	return eh_installed && eh_resolve_sources();
}

static uint32_t eh_word(const void *p, size_t off) {
	uint32_t v;
	memcpy(&v, (const char *)p + off, 4);
	return v;
}

// A runner builtin through its registry entry, with g_pFunction set as the GML call sites do. True for a true
// number or bool, or for an instance reference (collision_rectangle's hit; noone is the number -4).
static int eh_builtin(const int *desc, void *self, int argc, gml_rvalue *args) {
	runner_function *f = *eh_functions + desc[1], *saved = *eh_current_function;
	gml_rvalue out = {0, 0, 5};
	*eh_current_function = f;
	((builtin_fn)f->func)(&out, self, self, argc, args);
	*eh_current_function = saved;
	return ((out.kind == 0 || out.kind == 13) && out.val != 0 && out.val != -4) || out.kind == 15;
}

static gml_rvalue eh_object(int index) {
	gml_rvalue r = {0, 0, 15}; // an object reference, as YYC passes obj_* constants
	uint32_t b[2] = {(uint32_t)index, 0x01000000};
	memcpy(&r.val, b, 8);
	return r;
}

static double eh_num(void *self, int *var, double fallback) {
	gml_rvalue *v = var ? gml_vget(self, var[1]) : NULL;
	return v && (v->kind == 0 || v->kind == 13) && isfinite(v->val) ? v->val : fallback;
}

// Could enemy_hurt find something it reacts to this tick? Its place_meeting calls test the enemy's mask after
// AI_physics has moved it (and ai_state_control's at the position before), so the probe is a rectangle that holds
// every place the mask can be by then, tested against the sources' bounding boxes (collision_rectangle, not
// precise: it can only find more):
// - the mask is a fixed sprite per enemy object (OBJT mask: spr_gruntmask, spr_gangstermask, ...), but the AI can
//   flip image_xscale to face the other way: the box is the mask's bbox mirrored about x;
// - AI_physics moves at most |hsp| or |vsp| * fr (both clamped to 20; on a slope hsp * fr * timestep *
//   slope_slowdown) and its collision fixes add at most 2 * |hsp| + 5 (wall_check's search limit), 1 px plus
//   slope_tolerance and the pixel loops (floor, slope, ceiling) a few px: 48 + 20 * fr * that factor + slope_tolerance
//   on every side.
// Existence of each source is checked first, for each enemy: attacks made earlier this tick count.
// The Step covering two ticks needs no probe of the tick it skipped: anything that existed at the enemy's slot that
// tick was probed then (and would have run the Step), and anything made after it was not seen by the full-rate Step
// that tick either; both then test the same final position. Not covered: a move past the margin, e.g. ceiling_check
// pushing an enemy embedded deep in a block (its loop is unbounded) or an AI script that teleports an enemy.
unsigned enemyhalf_probes;
static int eh_hurt_near(void *self) {
	unsigned present = 0;
	for (int i = 0; i < EH_SOURCES; i++) {
		gml_rvalue obj = eh_object(eh_sources[i]);
		if (eh_sources[i] >= 0 && eh_builtin(eh_fn_instance_exists, self, 1, &obj))
			present |= 1u << i;
	}
	if (!present)
		return 0;
	enemyhalf_probes++;
	double box[4];
	for (int k = 0; k < 4; k++) {
		gml_rvalue v = {0, 0, 0};
		eh_bbox[k](self, (int)0x80000000, &v);
		if (v.kind != 0 || !isfinite(v.val))
			return 1; // no box: run the Step
		box[k] = v.val;
	}
	float xf;
	memcpy(&xf, (char *)self + 0xb8, 4);
	double x = xf, half = fmax(fabs(x - box[0]), fabs(box[2] - x));
	gml_rvalue *fr_v = global_var(var_fr);
	double fr = fr_v && fr_v->kind == 0 && isfinite(fr_v->val) ? fabs(fr_v->val) : 1;
	double slope = fabs(eh_num(self, eh_var_timestep, 1)) * fmax(1, fabs(eh_num(self, eh_var_slope_slowdown, 1)));
	double d = 48 + 20 * fmax(fr, 1) * fmax(slope, 1) + fabs(eh_num(self, eh_var_slope_tolerance, 0));
	for (int i = 0; i < EH_SOURCES; i++) {
		if (!(present & (1u << i)))
			continue;
		gml_rvalue args[7] = {{x - half - d, 0, 0}, {box[1] - d, 0, 0}, {x + half + d, 0, 0}, {box[3] + d, 0, 0},
			eh_object(eh_sources[i]), {0, 0, 13}, {1, 0, 13}}; // prec false, notme true
		if (eh_builtin(eh_fn_collision_rectangle, self, 7, args))
			return 1;
	}
	return 0;
}

// How many ticks this Step covers: 0 skip, 1 or 2
static int eh_decide(void *self) {
	uint32_t tick = *frame_count;
	if (*current_room != eh_room) {
		eh_room = *current_room;
		memset(eh_table, 0, sizeof(eh_table));
	}
	uint32_t key = eh_word(self, 0x80) + 1, i = (key * 2654435761u) % EH_SLOTS;
	int found = -1, free_slot = -1;
	for (int n = 0; n < EH_SLOTS; n++, i = (i + 1) % EH_SLOTS) {
		if (eh_table[i].key == key) {
			found = i;
			break;
		}
		if (!eh_table[i].key) {
			if (free_slot < 0)
				free_slot = i;
			break;
		}
		if (free_slot < 0 && tick - eh_table[i].last > EH_STALE)
			free_slot = i;
	}
	if (found < 0) {
		if (free_slot < 0) // table full: run every tick
			return 1;
		eh_table[free_slot].key = key;
		// A new enemy runs now (one tick) or next tick (covering this one too), by id parity
		eh_table[free_slot].last = (key ^ tick) & 1 ? tick - 3 : tick - 1;
		found = free_slot;
	}
	uint32_t elapsed = tick - eh_table[found].last;
	// Scripted sequences count ticks exactly: every tick. A Step that didn't run for longer than a skip (pause,
	// deactivation) covers one tick, as the original would.
	if ((scripted_tick && tick - scripted_tick < 3) || elapsed > 2 || elapsed == 0) {
		eh_table[found].last = tick;
		return 1;
	}
	if (elapsed == 2) {
		eh_table[found].last = tick;
		return 2;
	}
	if (eh_hurt_near(self)) {
		eh_table[found].last = tick;
		enemyhalf_forced++;
		return 1;
	}
	return 0;
}

// Debug: enemy deaths (create_corpse calls) by the kind of Step they happened in:
// 0 outside a half-rate enemy Step, 1 a Step run for a hurt source near, 2 a Step covering two ticks, 3 a one-tick Step
static gml_script_fn orig_create_corpse;
static int eh_step_kind;
unsigned enemyhalf_kills[4];
static void *eh_create_corpse(void *self, void *other, void *result, int argc, void **args) {
	static int lines;
	enemyhalf_kills[eh_step_kind]++;
	if (lines++ < 40)
		debugPrintf("[ENEMYHALF] create_corpse at tick %u in %s\n", *frame_count, (const char *const[]){"another event",
			"a step run for a hurt source near", "a step covering 2 ticks", "a one-tick step"}[eh_step_kind]);
	return orig_create_corpse(self, other, result, argc, args);
}

static void eh_run_step(event_func orig, void *self, void *other) {
	unsigned forced = enemyhalf_forced;
	int ticks = eh_decide(self);
	if (!ticks) {
		enemyhalf_skips++;
		return;
	}
	enemyhalf_runs++;
	eh_step_kind = forced != enemyhalf_forced ? 1 : ticks == 2 ? 2 : 3;
	gml_rvalue *fr = NULL, *t;
	double game_fr = 0, scaled = 0, before = -1;
	if (ticks == 2 && (fr = global_var(var_fr)) && fr->kind == 0) {
		game_fr = fr->val;
		scaled = fr->val *= 2;
		enemyhalf_doubled++;
	} else {
		fr = NULL;
	}
	if (ticks == 2 && (t = gml_vget(self, var_search_timeout[1])) && t->kind == 0)
		before = t->val;
	eh_depth++; // an inherited Step runs inside with this decision and fr
	orig(self, other);
	eh_depth--;
	eh_step_kind = 0;
	// enemy_hurt sets fr itself (0 for hit-stop on a kill): keep the game's value then
	if (fr && (fr = global_var(var_fr)) && fr->kind == 0 && fr->val == scaled)
		fr->val = game_fr;
	if (before >= 0 && (t = instance_var(self, var_search_timeout))) {
		double step = t->val - before;
		if (step == 1 || step == 2) {
			t->val += step;
			enemyhalf_search_ticks++;
		}
	}
}

// At 30 ticks/s (combat30.c) every enemy Step runs and covers two 60 Hz ticks (fr is doubled already): one that
// advanced search_timeout (+= 1 per Step) gets the tick it missed, as a Step covering two ticks above
unsigned c30_search_ticks;
static void c30_enemy_step(event_func orig, void *self, void *other) {
	gml_rvalue *t;
	double before = -1;
	if ((t = gml_vget(self, var_search_timeout[1])) && t->kind == 0)
		before = t->val;
	eh_depth++;
	orig(self, other);
	eh_depth--;
	if (before >= 0 && (t = instance_var(self, var_search_timeout))) {
		double step = t->val - before;
		if (step == 1 || step == 2) {
			t->val += step;
			c30_search_ticks++;
		}
	}
}

static void eh_step(event_func orig, void *self, void *other) {
	extern int c30_active;
	if (c30_active && !eh_depth) {
		c30_enemy_step(orig, self, other);
		return;
	}
	if (enemy_half_rate && eh_installed && !eh_depth && eh_resolve_sources()) {
		eh_run_step(orig, self, other);
		return;
	}
	eh_depth++; // an inherited Step runs inside: only the outermost one decides
	orig(self, other);
	eh_depth--;
}

#define EH_ENEMY_STEPS(X) X(enemy) X(enemy_cop) X(enemy_sci) X(enemy_grunt) X(enemy_meele) X(enemy_redpomp) \
	X(enemy_shotgun) X(enemy_shieldcop) X(enemy_machinegun)
#define EH_ENEMY_WRAP(name) static event_func orig_##name##_step; \
	static void eh_##name##_step(void *self, void *other) { eh_step(orig_##name##_step, self, other); }
EH_ENEMY_STEPS(EH_ENEMY_WRAP)

void enemy_half_rate_install(gml_func_entry *table) {
	var_fr = (int *)so_symbol(&yoyoloader_mod, "g_VAR_fr");
	var_search_timeout = (int *)so_symbol(&yoyoloader_mod, "g_VAR_search_timeout");
	int enemies = 0;
	if (var_fr && var_search_timeout && global_obj_ptr && current_room) {
#define EH_ENEMY_HOOK(name) enemies += !!(orig_##name##_step = gml_replace(table, "gml_Object_obj_" #name "_Step_0", eh_##name##_step));
		EH_ENEMY_STEPS(EH_ENEMY_HOOK)
	}
	// The hurt source objects are looked up by name at the first enemy Step (the object table isn't loaded yet)
	eh_object_find = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	eh_functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	eh_current_function = (runner_function **)so_symbol(&yoyoloader_mod, "g_pFunction");
	eh_fn_instance_exists = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_instance_exists");
	eh_function_count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	eh_var_timestep = (int *)so_symbol(&yoyoloader_mod, "g_VAR_timestep");
	eh_var_slope_slowdown = (int *)so_symbol(&yoyoloader_mod, "g_VAR_slope_slowdown");
	eh_var_slope_tolerance = (int *)so_symbol(&yoyoloader_mod, "g_VAR_slope_tolerance");
	eh_bbox[0] = (builtin_var_get)so_symbol(&yoyoloader_mod, "_Z6GV_BblP9CInstanceiP6RValue");
	eh_bbox[1] = (builtin_var_get)so_symbol(&yoyoloader_mod, "_Z6GV_BbtP9CInstanceiP6RValue");
	eh_bbox[2] = (builtin_var_get)so_symbol(&yoyoloader_mod, "_Z6GV_BbrP9CInstanceiP6RValue");
	eh_bbox[3] = (builtin_var_get)so_symbol(&yoyoloader_mod, "_Z6GV_BbbP9CInstanceiP6RValue");
	int eh_symbols = enemies == 9 && eh_object_find && eh_functions && eh_current_function && eh_function_count &&
		eh_fn_instance_exists && eh_bbox[0] && eh_bbox[1] && eh_bbox[2] && eh_bbox[3];
	// Enemies run every tick in scripted sequences (override_control_init marks them), and combat30.c runs those at 60
	orig_override_control_init = entry_hook(so_symbol(&yoyoloader_mod,
		"_Z32gml_Script_override_control_initP9CInstanceS0_R8YYRValueiPPS1_"), eh_override_control_init);
	scripted_hooked = orig_override_control_init != NULL;
	eh_installed = eh_symbols && orig_override_control_init;
	if (eh_installed && debugMode)
		orig_create_corpse = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z24gml_Script_create_corpseP9CInstanceS0_R8YYRValueiPPS1_"), eh_create_corpse);
	debugPrintf("Half-rate enemies: %s (enemy steps %d/9), yyl.cfg enemyHalfRate %d\n",
		eh_installed ? "installed" : "NOT installed", enemies, enemy_half_rate);
}
