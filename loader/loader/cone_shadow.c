/*
 * cone_shadow.c
 *
 * The bike chase's traffic cones (obj_bike_traffic_cone, ~100 on the road in the cone stretch) inherit their Draw event
 * from obj_bike_obstacle (gml_Object_obj_bike_obstacle_Draw_0, APK 1.1.10), which is a single call:
 *
 *     draw_shadow(sprite_index, animation_index, x, y, ...);   // ~20 one-row draw_sprite_part_ext per cone
 *
 * Even with the native draw_shadow (shadow_native.c) that is 4.5-6 ms per frame in the cone stretch.
 *
 * Compromise (coneShadows=0, the default): the cones are drawn without that shadow. The call is redirected (through a
 * veneer, as in bike_draw.c) to a stub that, for an instance whose object is exactly obj_bike_traffic_cone, leaves the
 * result slot as draw_shadow would (low word 0, undefined) and returns; every other obstacle (obj_bike_car,
 * obj_bike_car_wreck, obj_bike_obstacle itself) still draws its shadow. draw_shadow only draws: it reads sprite
 * metrics and its arguments, draws no random numbers and writes nothing but its result, so gameplay and the RNG are
 * unchanged. The cone sprite itself is drawn by obj_drawactors_bike (user event 15) and stays. coneShadows=1 keeps the
 * shadows.
 */
#include <stdint.h>
#include <string.h>
#include <kubridge.h>

#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef RV *ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);

int cone_shadows = 0; // coneShadows in yyl.cfg
unsigned cone_shadows_skipped, cone_shadows_drawn;

static script_fn draw_shadow;
static int (*object_find)(const char *name);
static int traffic_cone = -2;

#define OBJECT_INDEX(inst) (*(int *)((char *)(inst) + 0x84))

static RV *ABI cone_draw_shadow(void *self, void *other, RV *result, int argc, RV **args) {
	if (traffic_cone == -2)
		traffic_cone = object_find("obj_bike_traffic_cone");
	if (self && traffic_cone >= 0 && OBJECT_INDEX(self) == traffic_cone) {
		cone_shadows_skipped++;
		uint32_t lo = 0; // what the compiled script leaves in the result: low word 0, undefined
		memcpy(&result->value, &lo, 4);
		result->kind = 5;
		return result;
	}
	cone_shadows_drawn++;
	return draw_shadow(self, other, result, argc, args);
}

int cone_shadow_install(void) {
	// ELF addresses in APK 1.1.10: the event, its draw_shadow call and the PLT entry it calls
	enum { EVENT = 0x353a534, SITE = 0x353aa54, PLT_SHADOW = 0x4ab21a0 };
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z35gml_Object_obj_bike_obstacle_Draw_0P9CInstanceS0_");
	draw_shadow = (script_fn)so_symbol(&yoyoloader_mod, "_Z22gml_Script_draw_shadowP9CInstanceS0_R8YYRValueiPPS1_");
	object_find = (int (*)(const char *))so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	if (!entry || !draw_shadow || !object_find)
		return 0;
	uintptr_t site = entry + SITE - EVENT, plt = entry + PLT_SHADOW - EVENT;
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	if ((ins >> 24) != 0xeb || (target != plt && target != (uintptr_t)draw_shadow))
		return 0;
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	if (!veneer)
		return 0;
	int64_t delta = (int64_t)(uintptr_t)veneer - (int64_t)site - 8;
	if (delta < -(1 << 25) || delta >= (1 << 25))
		return 0;
	uint32_t code[2] = {0xe51ff004, (uint32_t)&cone_draw_shadow}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t bl = 0xeb000000 | (((uint32_t)delta >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
	debugPrintf("traffic cones: no shadows\n");
	return 1;
}
