/*
 * far_rocket.c
 *
 * In the bike chase's helicopter fight the helicopter first fires rockets into the sky in the background
 * (obj_heli_mortar_far, its own object with spr_heli_mortar_far; each one turns into a falling obj_heli_mortar when it
 * leaves the top of the screen). Their user event 15 (gml_Object_obj_heli_mortar_far_Other_25, APK 1.1.10), called
 * from their Draw event every frame, is
 *
 *     if (region == -1) update_region();
 *     if (update_light_timer <= 0 || ...) { update_light_array(true); update_light_timer = 1; }   // every frame
 *     draw_sprite_ext(sprite_index, animation_index, x, y, dir * width, height, image_angle, image_blend, image_alpha);
 *     draw_sprite_recolored(sprite_index, animation_index, x, y, dir * width, height, image_angle, true);
 *     if (shade) draw_rim_shading_surface(3, 1);
 *
 * draw_rim_shading_surface draws each rocket several times into a shared surface and back (surface_set_target /
 * surface_reset_target and draw_surface_part_ext per light): with 15-25 rockets on screen that is 15-37 ms of
 * Graphics::Flush per frame and frames of 40-65 ms while they rise.
 *
 * Compromise (farRocketLights=0, the default): these small background rockets are drawn unlit, with the plain
 * draw_sprite_ext above. The three light calls in this event are redirected (through veneers, as in bike_draw.c) to
 * stubs that return at once. Nothing else changes:
 * - None of the three scripts draws random numbers or creates/destroys instances (no RNG or gameplay effect).
 * - update_light_array writes the rocket's own close_lights/close_light_array, read only by the two skipped draws,
 *   and each light's rdist, which every reader (update_light_array(_ext), draw_sprite/surface_recolored and their old
 *   versions) assigns before reading it.
 * - draw_rim_shading_surface writes the rocket's own batch arrays, shading, and puts back x, y, width = 2,
 *   height = 2, image_alpha = 1, image_blend = c_white and gpu_set_blendmode(bm_normal) (factors and equations). The stubs only run while all
 *   of those already hold (else the event runs the original calls for that rocket).
 * - The shared rim-shading surface is cleared by every user before it draws into it.
 * farRocketLights=1 keeps the original. exp_farrocketverify (debug) counts the rockets the stubs would have handled
 * but runs the original calls ([FARROCKET] report).
 */
#include <stdint.h>
#include <string.h>
#include <kubridge.h>

#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef void ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);
typedef RV *ABI (*var_fn)(void *instance, int id);
typedef void ABI (*getvalue_fn)(const RV *instance, int id, int index, RV *out, int, int);
typedef void ABI (*builtin_fn)(RV *result, void *self, void *other, int argc, RV *args);

int far_rocket_lights = 0; // farRocketLights in yyl.cfg
unsigned far_rocket_skipped, far_rocket_original, far_rocket_would_skip; // light calls (debug report)

static script_fn orig_light_array, orig_recolored, orig_rim;
static getvalue_fn variable_get;
static builtin_fn get_blendmode;
static int ABI (*get_render_state)(void *states, int state);
static void *render_states;
static const int *var_width, *var_height, *var_image_alpha, *var_image_blend;
static int verify;

#define KIND(v) ((v)->kind & 0xffffff)

static int real_is(void *self, const int *desc, double want, int builtin) {
	RV out = {0, 0, 0xffffff};
	if (desc[1] < 0)
		return 0;
	if (builtin) {
		RV inst = {0, 0, 6};
		memcpy(&inst.value, &self, sizeof(self));
		variable_get(&inst, desc[1], (int)0x80000000, &out, 0, 0);
	} else {
		RV *v = (*(var_fn *)(*(uint8_t **)self + 8))(self, desc[1]);
		if (!v)
			return 0;
		out = *v;
	}
	return KIND(&out) == 0 && out.value == want;
}

// 1: this rocket's light calls can be skipped (the rim shading's put-back values already hold)
static int can_skip(void *self) {
	if (!self)
		return 0;
	RV mode = {0, 0, 0xffffff};
	get_blendmode(&mode, self, NULL, 0, NULL);
	// gpu_get_blendmode only looks at the blend factors; rim shading's gpu_set_blendmode(bm_normal) also puts back the
	// colour and alpha blend equations (render states 0x24/0x25, 0 = add), so those must already be the defaults too
	if (get_render_state(render_states, 0x24) != 0 || get_render_state(render_states, 0x25) != 0)
		return 0;
	return KIND(&mode) == 0 && mode.value == 0 && real_is(self, var_width, 2, 0) && real_is(self, var_height, 2, 0) &&
		real_is(self, var_image_alpha, 1, 1) && real_is(self, var_image_blend, 16777215, 1);
}

// The event calls update_light_array (only when its timer allows), then draw_sprite_recolored, then (if shade)
// draw_rim_shading_surface. Each call decides from the rocket's current state, except that a decision made by the
// call just before in the same event is reused (nothing runs in between but draw_sprite_ext).
static void *after_light, *after_recolor; // the rocket whose previous call just decided, or NULL
static int after_light_skip, after_recolor_skip;

static int decide(void *self) {
	int skip = can_skip(self);
	if (skip && verify) {
		far_rocket_would_skip++;
		skip = 0;
	}
	if (skip)
		far_rocket_skipped++;
	else
		far_rocket_original++;
	return skip;
}

static void ABI far_light_array(void *self, void *other, RV *result, int argc, RV **args) {
	after_recolor = NULL;
	after_light = self;
	if ((after_light_skip = decide(self)))
		return;
	orig_light_array(self, other, result, argc, args);
}

static void ABI far_recolored(void *self, void *other, RV *result, int argc, RV **args) {
	int skip = self && self == after_light ? after_light_skip : decide(self);
	after_light = NULL;
	after_recolor = self;
	if ((after_recolor_skip = skip))
		return;
	orig_recolored(self, other, result, argc, args);
}

static void ABI far_rim(void *self, void *other, RV *result, int argc, RV **args) {
	int skip = self && self == after_recolor ? after_recolor_skip : decide(self);
	after_recolor = NULL;
	if (skip)
		return;
	orig_rim(self, other, result, argc, args);
}

static int bl_reaches(uintptr_t site, uintptr_t target) {
	int64_t delta = (int64_t)target - (int64_t)site - 8;
	return !(delta & 3) && delta >= -(1 << 25) && delta < (1 << 25);
}

static int check_call(uintptr_t site, uintptr_t plt, uintptr_t direct) {
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	return (ins >> 24) == 0xeb && (target == plt || target == direct);
}

static void patch_call(uintptr_t site, uintptr_t wrapper, uint32_t *veneer) {
	int32_t off = (int32_t)((uintptr_t)veneer - (site + 8));
	uint32_t code[2] = {0xe51ff004, wrapper}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
}

int far_rocket_install(void) {
	// ELF addresses in APK 1.1.10: the event, its three call sites and the PLT entries they call
	enum { EVENT = 0x4159d3c, LIGHT_SITE = 0x415a0a0, RECOLOR_SITE = 0x415aac0, RIM_SITE = 0x415aca0,
	       PLT_LIGHT = 0x4ab1810, PLT_RECOLOR = 0x4ab1410, PLT_RIM = 0x4ab1820 };
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z39gml_Object_obj_heli_mortar_far_Other_25P9CInstanceS0_");
	orig_light_array = (script_fn)so_symbol(&yoyoloader_mod, "_Z29gml_Script_update_light_arrayP9CInstanceS0_R8YYRValueiPPS1_");
	orig_recolored = (script_fn)so_symbol(&yoyoloader_mod, "_Z32gml_Script_draw_sprite_recoloredP9CInstanceS0_R8YYRValueiPPS1_");
	orig_rim = (script_fn)so_symbol(&yoyoloader_mod, "_Z35gml_Script_draw_rim_shading_surfaceP9CInstanceS0_R8YYRValueiPPS1_");
	variable_get = (getvalue_fn)so_symbol(&yoyoloader_mod, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	get_blendmode = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z17F_GPUGetBlendModeR6RValueP9CInstanceS2_iPS_");
	get_render_state = (int ABI (*)(void *, int))so_symbol(&yoyoloader_mod, "_ZN18RenderStateManager14GetRenderStateE12eRenderState");
	render_states = (void *)so_symbol(&yoyoloader_mod, "g_States");
	var_width = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_width");
	var_height = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_height");
	var_image_alpha = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_image_alpha");
	var_image_blend = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_image_blend");
	if (!entry || !orig_light_array || !orig_recolored || !orig_rim || !variable_get || !get_blendmode || !get_render_state || !render_states || !var_width ||
			!var_height || !var_image_alpha || !var_image_blend)
		return 0;
#define AT(a) (entry + (a) - EVENT)
	enum { NSITES = 3 };
	uintptr_t sites[NSITES] = {AT(LIGHT_SITE), AT(RECOLOR_SITE), AT(RIM_SITE)};
	uintptr_t plts[NSITES] = {AT(PLT_LIGHT), AT(PLT_RECOLOR), AT(PLT_RIM)};
	uintptr_t direct[NSITES] = {(uintptr_t)orig_light_array, (uintptr_t)orig_recolored, (uintptr_t)orig_rim};
	uintptr_t wrappers[NSITES] = {(uintptr_t)&far_light_array, (uintptr_t)&far_recolored, (uintptr_t)&far_rim};
#undef AT
	for (int k = 0; k < NSITES; k++)
		if (!check_call(sites[k], plts[k], direct[k]))
			return 0;
	uint32_t *block = (uint32_t *)so_alloc_near(&yoyoloader_mod, sites[0], 8 * NSITES);
	if (!block)
		return 0;
	for (int k = 0; k < NSITES; k++)
		if (!bl_reaches(sites[k], (uintptr_t)(block + 2 * k)))
			return 0;
	for (int k = 0; k < NSITES; k++)
		patch_call(sites[k], wrappers[k], block + 2 * k);
	verify = exp_flag("farrocketverify");
	debugPrintf("far rockets: drawn unlit%s\n", verify ? " (verify mode: original calls run)" : "");
	return 1;
}
