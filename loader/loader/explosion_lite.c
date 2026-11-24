/*
 * explosion_lite.c
 *
 * In the bike chase's helicopter fight each falling mortar (obj_heli_mortar) explodes in its Step event
 * (gml_Object_obj_heli_mortar_Step_0, APK 1.1.10, GML lines 48-56):
 *
 *     screen_shake(10);
 *     instance_create_depth(x, y, -50, obj_heli_mortar_explosion);        // the hitbox (2 frames) and the sound
 *     repeat (30) {
 *         bb = instance_create(x - 80 + irandom(160), y + 20 - irandom(120), obj_explosion_particle);
 *         bb.parent = id;
 *         bb.animation_speed = (random(0.2) + 0.3 - random(0.1)) * 4;
 *     }
 *
 * A volley is 50 mortars: 1,500 explosion particles, ~250 alive at once. Their creation (instance + Create events),
 * their Step and draw (obj_drawactors_bike walks every particle once per mortar) and the garbage they leave are most
 * of the 30-35 ms volley frames.
 *
 * Compromise (fullExplosions=0, the default): only every keep-th particle of each explosion is created (keep 3: 10 of
 * 30). The skipped iterations still make all their random draws (both irandom arguments, both random() calls), so the
 * random sequence and everything that depends on it are unchanged (none of the particle's own events, Create to
 * Destroy, draws random numbers). Their instance_create returns the particle created before it in the same explosion,
 * so `bb.parent = id` writes the same value again and `bb.animation_speed = ...` gives that particle the skipped one's
 * animation speed. The particles are purely visual: the bike player is hurt by obj_heli_mortar_explosion, and nothing in
 * the room collides with or reads obj_explosion_particle (its `deadly` is read only by other levels' scripts and
 * objects). They do get other instance ids than before, which nothing compares.
 *
 * Also under fullExplosions=0, explosion particles skip update_region in their user event 15 (they never use their
 * region: their light list is skipped, see particle_lights.c, and their draw is draw_self).
 *
 * fullExplosions=1 keeps the original. Debug: exp_explokeep=N sets keep, exp_explospeed=P plays the particles' animation
 * (their whole life) at P percent speed, exp_exploshot=K asks for a screenshot 6 frames after the K-th mortar explosion
 * of the room (screenshot.c), exp_explorng counts the random()/irandom() calls made from the explosion loop's code
 * (120 per explosion with every keep).
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <kubridge.h>
#include <psp2/io/fcntl.h>

#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef RV *ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);

int full_explosions = 0; // fullExplosions in yyl.cfg
unsigned explosion_particles_created, explosion_particles_skipped, explosion_regions_skipped;

extern uint32_t *frame_count;
extern int debugMode;
extern volatile int screenshot_request_now;

static script_fn orig_instance_create, orig_update_region;
typedef void ABI (*setvalue_fn)(const RV *inst, int var, int index, RV *value);
static setvalue_fn orig_set_value;
static int speed_percent = 100; // explosion particles' animation speed (their lifetime is one animation)
static int keep = 3, shot_at, shot_checked, explosions, lite = 1, log_rng;
// exp_explorng: random()/irandom() calls made from the explosion loop's code (GML lines 52-54), per explosion
typedef uint64_t ABI (*rand_fn)(uint32_t a, uint32_t b);
static rand_fn orig_random, orig_irandom;
static unsigned loop_draws;
unsigned loop_draws_explosions, loop_draws_bad;
static uint64_t ABI count_random(uint32_t a, uint32_t b) {
	loop_draws++;
	return orig_random(a, b);
}
static uint64_t ABI count_irandom(uint32_t a, uint32_t b) {
	loop_draws++;
	return orig_irandom(a, b);
}
static void *last_self;
static uint32_t last_frame, shot_frame;
static int index_in_explosion, have_last;
static RV last_result;

#define KIND(v) ((v)->kind & 0xffffff)

// The number in ux0:data/gms/shared/exp_<name> (debug), or def
static int exp_number(const char *name, int def) {
	char path[96], text[16] = {0};
	snprintf(path, sizeof(path), "ux0:data/gms/shared/exp_%s", name);
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return def;
	sceIoRead(fd, text, sizeof(text) - 1);
	sceIoClose(fd);
	return atoi(text);
}

static RV *ABI lite_instance_create(void *self, void *other, RV *result, int argc, RV **args) {
	uint32_t frame = frame_count ? *frame_count : 0;
	if (self != last_self || frame != last_frame) { // a new explosion: each mortar explodes once, in one Step
		last_self = self;
		last_frame = frame;
		index_in_explosion = 0;
		have_last = 0;
		if (debugMode && !shot_checked) { // read at the first explosion, so a harness can write it after the start
			shot_checked = 1;
			shot_at = exp_number("exploshot", 0);
			if (shot_at)
				debugPrintf("[EXPLOSION] screenshot after explosion %d%c", shot_at, 10);
		}
		++explosions;
		if (shot_at && explosions == shot_at)
			shot_frame = frame + 6;
		// exp_explorng: the previous explosion's loop made 30 x (2 irandom + 2 random) calls; this explosion's first two
		// irandom calls (the arguments of this instance_create) are already counted
		if (log_rng && orig_random) {
			if (explosions > 1) {
				loop_draws_explosions++;
				if (loop_draws - 2 != 120 && loop_draws_bad++ < 8)
					debugPrintf("[EXPLORNG] explosion %d: %u random draws in its loop (expected 120)%c", explosions - 1,
						loop_draws - 2, 10);
			}
			loop_draws = 2;
		}
	} else
		index_in_explosion++;
	if (keep > 1 && have_last && index_in_explosion % keep) {
		explosion_particles_skipped++;
		*result = last_result; // an instance reference or id: nothing to count
		return result;
	}
	RV *r = orig_instance_create(self, other, result, argc, args);
	explosion_particles_created++;
	have_last = KIND(result) == 15 || KIND(result) == 0;
	if (have_last)
		last_result = *result;
	return r;
}

static RV *ABI lite_update_region(void *self, void *other, RV *result, int argc, RV **args) {
	explosion_regions_skipped++;
	(void)self, (void)other, (void)argc, (void)args;
	uint32_t lo = 0; // what the script's prologue leaves in the result: low word 0, undefined
	memcpy(&result->value, &lo, 4);
	result->kind = 5;
	return result;
}

// bb.animation_speed = (random(0.2) + 0.3 - random(0.1)) * 4: the particle plays its animation (and dies) faster
static void ABI lite_set_animation_speed(const RV *inst, int var, int index, RV *value) {
	if (speed_percent != 100 && value && KIND(value) == 0)
		value->value = value->value * speed_percent / 100.0;
	orig_set_value(inst, var, index, value);
}

// Debug mode, once per frame from screenshot_check: the exp_exploshot trigger
void explosion_lite_frame(void) {
	if (shot_frame && frame_count && *frame_count >= shot_frame) {
		shot_frame = 0;
		screenshot_request_now = 1;
	}
}

static int patch_bl(uintptr_t site, uintptr_t plt, uintptr_t direct, void *wrapper) {
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	if ((ins >> 24) != 0xeb || (target != plt && target != direct))
		return 0;
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	if (!veneer)
		return 0;
	int64_t delta = (int64_t)(uintptr_t)veneer - (int64_t)site - 8;
	if (delta < -(1 << 25) || delta >= (1 << 25))
		return 0;
	uint32_t code[2] = {0xe51ff004, (uint32_t)wrapper}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t bl = 0xeb000000 | (((uint32_t)delta >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
	return 1;
}

int explosion_lite_install(void) {
	// ELF addresses in APK 1.1.10: the mortar Step and its particle instance_create call, the explosion particle's
	// user event 15 and its update_region call, and the PLT entries they call
	enum { STEP = 0x415c288, CREATE_SITE = 0x415dde0, PLT_CREATE = 0x4ab0fc0, SPEED_SITE = 0x415dfe0, PLT_SET = 0x4ab1140,
	       EV15 = 0x3ef05ec, REGION_SITE = 0x3ef0a8c, PLT_REGION = 0x4ab0fd0 };
	uintptr_t step = so_symbol(&yoyoloader_mod, "_Z33gml_Object_obj_heli_mortar_Step_0P9CInstanceS0_");
	uintptr_t ev15 = so_symbol(&yoyoloader_mod, "_Z42gml_Object_obj_explosion_particle_Other_25P9CInstanceS0_");
	orig_instance_create = (script_fn)so_symbol(&yoyoloader_mod, "_Z26gml_Script_instance_createP9CInstanceS0_R8YYRValueiPPS1_");
	orig_set_value = (setvalue_fn)so_symbol(&yoyoloader_mod, "_Z23YYGML_Variable_SetValueRK8YYRValueiiP6RValue");
	orig_update_region = (script_fn)so_symbol(&yoyoloader_mod, "_Z24gml_Script_update_regionP9CInstanceS0_R8YYRValueiPPS1_");
	if (!step || !ev15 || !orig_instance_create || !orig_update_region || !orig_set_value || !frame_count)
		return 0;
	extern int full_explosions;
	lite = !full_explosions;
	log_rng = debugMode && exp_flag("explorng");
	if (log_rng) { // the loop's four random calls (GML lines 52 and 54) go through counting wrappers
		orig_random = (rand_fn)so_symbol(&yoyoloader_mod, "_Z12YYGML_randomd");
		orig_irandom = (rand_fn)so_symbol(&yoyoloader_mod, "_Z13YYGML_irandomx");
		int ok = orig_random && orig_irandom;
		const uintptr_t irandom_sites[2] = {0x415db04, 0x415dc54}, random_sites[2] = {0x415df60, 0x415df70};
		for (int k = 0; ok && k < 2; k++)
			ok = patch_bl(step + irandom_sites[k] - STEP, step + 0x4ab17e0 - STEP, (uintptr_t)orig_irandom, (void *)count_irandom) &&
				patch_bl(step + random_sites[k] - STEP, step + 0x4ab0f80 - STEP, (uintptr_t)orig_random, (void *)count_random);
		if (!ok)
			orig_random = NULL;
		debugPrintf("[EXPLORNG] counting loop draws: %s%c", ok ? "on" : "FAILED", 10);
	}
	keep = lite ? 3 : 1; // fullExplosions=1: every particle is created (the hook only serves the debug screenshot)
	if (debugMode && lite) {
		int k = exp_number("explokeep", 0);
		if (k >= 1 && k <= 30)
			keep = k;
		int sp = exp_number("explospeed", 0);
		if (sp >= 50 && sp <= 400)
			speed_percent = sp;
	}
	if (!patch_bl(step + CREATE_SITE - STEP, step + PLT_CREATE - STEP, (uintptr_t)orig_instance_create, (void *)lite_instance_create))
		return 0;
	if (lite && speed_percent != 100)
		patch_bl(step + SPEED_SITE - STEP, step + PLT_SET - STEP, (uintptr_t)orig_set_value, (void *)lite_set_animation_speed);
	int region = lite && patch_bl(ev15 + REGION_SITE - EV15, ev15 + PLT_REGION - EV15, (uintptr_t)orig_update_region,
		(void *)lite_update_region);
	debugPrintf("explosions: 1 in %d particles per mortar explosion, animation speed %d%%%s%s\n", keep, speed_percent, region ? ", no update_region" : "",
		shot_at ? " (screenshot trigger)" : "");
	return 1;
}
