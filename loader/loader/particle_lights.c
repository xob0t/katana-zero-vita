/*
 * particle_lights.c
 *
 * Every obj_particle's Create event (gml_Object_obj_particle_Create_0, APK 1.1.10) ends with
 *
 *     draw_recolor_init();
 *     update_light_array(false);
 *
 * which fills the particle's close_lights / close_light_array from every light in the room. In the bike chase's
 * helicopter fight each exploding mortar creates a burst of obj_explosion_particle (60-90 per frame on explosion
 * frames, ~40 us each for this call with the native loop body): 2.4-3.6 ms of the worst frames.
 *
 * An obj_explosion_particle never reads that list. close_lights and close_light_array are read only by scripts that
 * act on their own instance (draw_sprite_recolored and its debug/old versions, draw_surface_recolored,
 * draw_rim_shading_surface(_unbatched), update_light_array(_ext/_sorted)) and by three other objects' user events
 * (obj_heli 6, obj_clubber 6, obj_prisonguard 14). An explosion particle runs none of them: its own events are
 * Create, Step, user event 15 (draw_self) and Animation End (instance_destroy), and the ones it inherits from
 * obj_particle are Step 1 (adds itself to its parent's particle list), user event 13 (draw_self in slow motion) and
 * Destroy. The script draws no random numbers and creates nothing; the only other thing it writes is each light's
 * rdist, which every reader assigns before reading it. So for an explosion particle (object_index exactly
 * obj_explosion_particle, not a child object) the call is skipped; every other particle runs it as before.
 *
 * exp_particlelights0 keeps the original. exp_particlelightsverify runs the call for every particle and checks the
 * claim above while the game runs: entry hooks on the reader scripts count any call whose self is an explosion
 * particle ([PARTLIGHTS] report: "readers on explosion particles" must stay 0).
 */
#include <stdint.h>
#include <string.h>
#include <kubridge.h>

#include "main.h"
#include "so_util.h"

extern int debugMode;

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef void ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);

static script_fn orig_light_array;
static int (*object_find)(const char *name);
static int explosion_particle = -2; // object index, looked up on first use
static int verify;
unsigned particle_lights_skipped, particle_lights_run, particle_lights_reader_hits, particle_lights_reader_calls;

#define OBJECT_INDEX(inst) (*(int *)((char *)(inst) + 0x84))

static int is_explosion_particle(void *self) {
	if (explosion_particle == -2)
		explosion_particle = object_find("obj_explosion_particle");
	return self && explosion_particle >= 0 && OBJECT_INDEX(self) == explosion_particle;
}

static void ABI particle_light_array(void *self, void *other, RV *result, int argc, RV **args) {
	if (!verify && is_explosion_particle(self)) {
		particle_lights_skipped++; // the result slot keeps the undefined the caller put there, as the script leaves it
		return;
	}
	particle_lights_run++;
	orig_light_array(self, other, result, argc, args);
}

// Verify mode: the readers, each behind an entry hook that counts calls on an explosion particle
#define NREADERS 8
static const char *reader_names[NREADERS] = {
	"_Z32gml_Script_draw_sprite_recoloredP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z38gml_Script_draw_sprite_recolored_debugP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z38gml_Script_draw_sprite_recolored_old_2P9CInstanceS0_R8YYRValueiPPS1_",
	"_Z33gml_Script_draw_surface_recoloredP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z35gml_Script_draw_rim_shading_surfaceP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z45gml_Script_draw_rim_shading_surface_unbatchedP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z33gml_Script_update_light_array_extP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z36gml_Script_update_light_array_sortedP9CInstanceS0_R8YYRValueiPPS1_",
};
static script_fn reader_orig[NREADERS];

#define READER(k) \
	static void ABI reader_##k(void *self, void *other, RV *result, int argc, RV **args) { \
		particle_lights_reader_calls++; \
		if (is_explosion_particle(self)) \
			particle_lights_reader_hits++; \
		reader_orig[k](self, other, result, argc, args); \
	}
READER(0) READER(1) READER(2) READER(3) READER(4) READER(5) READER(6) READER(7)
static void *reader_wrappers[NREADERS] = {reader_0, reader_1, reader_2, reader_3, reader_4, reader_5, reader_6, reader_7};

static int bl_reaches(uintptr_t site, uintptr_t target) {
	int64_t delta = (int64_t)target - (int64_t)site - 8;
	return !(delta & 3) && delta >= -(1 << 25) && delta < (1 << 25);
}

int particle_lights_install(void *(*hook)(uintptr_t entry, void *replacement)) {
	// ELF addresses in APK 1.1.10: the event, its update_light_array call and the PLT entry it calls
	enum { EVENT = 0x4580668, SITE = 0x4580d8c, PLT_LIGHT = 0x4ab1810 };
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z32gml_Object_obj_particle_Create_0P9CInstanceS0_");
	orig_light_array = (script_fn)so_symbol(&yoyoloader_mod, "_Z29gml_Script_update_light_arrayP9CInstanceS0_R8YYRValueiPPS1_");
	object_find = (int (*)(const char *))so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	if (!entry || !orig_light_array || !object_find)
		return 0;
	uintptr_t site = entry + SITE - EVENT, plt = entry + PLT_LIGHT - EVENT;
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	if ((ins >> 24) != 0xeb || (target != plt && target != (uintptr_t)orig_light_array))
		return 0;
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	if (!veneer || !bl_reaches(site, (uintptr_t)veneer))
		return 0;
	verify = exp_flag("particlelightsverify");
	if (verify)
		for (int k = 0; k < NREADERS; k++) {
			// exp_recolorlightsprof (recolor_lights.c) times draw_sprite_recolored with its own entry hook, which can't
			// stack on this one: with both flags that reader isn't counted here (logged)
			if (k == 0 && debugMode && exp_flag("recolorlightsprof")) {
				debugPrintf("particle lights: verify doesn't count draw_sprite_recolored (exp_recolorlightsprof times it)%c", 10);
				reader_orig[k] = NULL;
				continue;
			}
			uintptr_t r = so_symbol(&yoyoloader_mod, reader_names[k]);
			reader_orig[k] = r ? (script_fn)hook(r, reader_wrappers[k]) : NULL;
			if (!reader_orig[k]) {
				debugPrintf("particle lights: can't hook %s for verify\n", reader_names[k]);
				return 0;
			}
		}
	uint32_t code[2] = {0xe51ff004, (uint32_t)&particle_light_array}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t bl = 0xeb000000 | ((((uintptr_t)veneer - (site + 8)) >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
	debugPrintf("particle lights: explosion particles skip update_light_array%s\n", verify ? " (verify mode: run, readers hooked)" : "");
	return 1;
}
