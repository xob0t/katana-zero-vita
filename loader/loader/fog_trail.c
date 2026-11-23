/*
 * fog_trail.c
 *
 * In the fog rooms (the prison levels and the neighbour's apartment party: rooms with an obj_fogspawner, which is
 * what makes the game's fog), the dust clouds the player kicks up when rolling (obj_dustcloud, an obj_particle) are
 * drawn without their light tint. obj_particle user event 15 (APK 1.1.10, GML lines 1-46) is:
 *
 *     if (point_distance(x, y, xprevious, yprevious) > 1 || region == -1) update_region();      // lighting only
 *     if (color_me) {
 *         if ((global.blackout || blackout) && !spark) {          // two dark silhouette passes
 *             var a = image_alpha, c = image_blend;
 *             image_blend = 4210752; image_alpha = 1; draw_self();
 *             image_blend = c_black; image_alpha = 0.75; draw_self();
 *             image_alpha = a; image_blend = c;
 *         } else draw_self();
 *         if (!spark && !shading) {
 *             image_alpha *= 1.5;
 *             if (prev_recolor_timer_val != global.recolor_timer)                               // the lit layer
 *                 draw_sprite_recolored(sprite_index, image_index, x, y, image_xscale, image_yscale, image_angle, 0);
 *             else draw_sprite_ext(..., light_col, light_alpha);
 *             image_alpha /= 1.5;
 *         }
 *     } else draw_self();
 *
 * Rolling in room_prison_3 keeps ~12 clouds alive, and their lit layer recomputes against the nearby lights every
 * other frame (the lighting timer runs at 30 Hz). In the fog rooms, for obj_dustcloud only, this runs the event
 * without the lighting:
 * - the region update (point_distance on xprevious/yprevious, update_region) is skipped. It writes only the cloud's
 *   region/update_light_timer and obj_room_seed's scratch variables, which only the light-list scripts read;
 * - the lit layer becomes one draw_sprite_ext of the sprite in a fixed colour (FOG_TRAIL_COLOUR) at FOG_TRAIL_ALPHA x
 *   the cloud's raised alpha: the average tint the clouds get in prison_3 (exp_fogtrailsample measures it). No light
 *   recompute: light_col/light_alpha/prev_recolor_timer_val and the lights' rdist are no longer written (only
 *   draw_sprite_recolored and this event read them);
 * - everything else is the original's own sequence of runner calls in its order: the getters, BOOL_RValue, the
 *   Variable_SetValue_Direct writes (the silhouettes' temporary blend/alpha and their restore, image_alpha *= 1.5
 *   and /= 1.5, which round through the float field exactly as before) and draw_self.
 * None of the skipped code draws a random number (update_region, draw_sprite_recolored and the event have no random
 * calls), so gameplay and the RNG stream are unchanged.
 *
 * fogTrailLights=1 in yyl.cfg keeps the original. Debug ([FOGTRAIL] report line): exp_fogtrailsample averages the
 * lit clouds' light colour and light_alpha / raised image_alpha; exp_fogtrailab lights them in every other pair of
 * report windows (in-run A/B, ~4 s per state).
 */
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef void (*event_fn)(void *self, void *other);
typedef RV *ABI (*ref_fn)(void *instance, int id);
typedef int ABI (*bool_fn)(const RV *value);
typedef void ABI (*getvalue_fn)(const RV *instance, int id, int index, RV *out, int, int);
typedef void ABI (*setdirect_fn)(void *instance, int id, int index, RV *value);
typedef void ABI (*draw_self_fn)(void *self);
typedef void ABI (*draw_ext_fn)(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
	int colour, float alpha);
typedef int ABI (*exists_fn)(void *self, void *other, int object);

// The plain lit layer: the clouds' average tint in room_prison_3 (exp_fogtrailsample, Nov 2026)
#define FOG_TRAIL_COLOUR 0x808080
#define FOG_TRAIL_ALPHA 0.5f

int fog_trail_lights = 0; // fogTrailLights in yyl.cfg: 1 keeps the clouds' original lighting
unsigned fog_trail_native, fog_trail_original;
int fog_trail_ab, fog_trail_ab_off; // exp_fogtrailab (debug): lit in every other pair of report windows
static event_fn original;
static bool_fn bool_value;
static getvalue_fn variable_get;
static setdirect_fn variable_set;
static draw_self_fn draw_self;
static draw_ext_fn draw_sprite_ext;
static exists_fn instance_exists;
static void **global_obj;
static uint32_t *frame_count;
static int dustcloud = -1, fogspawner = -1, sample;

enum { COLOR_ME, BLACKOUT, SPARK, SHADING, IMAGE_ALPHA, IMAGE_BLEND, IMAGE_INDEX, LIGHT_COL, LIGHT_ALPHA, NVARS };
static const char *const var_names[NVARS] = {"g_VAR_color_me", "g_VAR_blackout", "g_VAR_spark", "g_VAR_shading",
	"g_VAR_image_alpha", "g_VAR_image_blend", "g_VAR_image_index", "g_VAR_light_col", "g_VAR_light_alpha"};
static const int *var_desc[NVARS]; // {name, id}: the runner fills the ids at game start, so read them live
#define VAR_ID(n) (var_desc[n][1])

// exp_fogtrailsample: sums over the clouds' original lit layer
double fog_trail_sum[5]; // r, g, b weighted by light_alpha; light_alpha; light_alpha / raised image_alpha
unsigned fog_trail_samples;

static float field(const void *p, size_t o) { float v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }
static uint32_t word(const void *p, size_t o) { uint32_t v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }
static RV *vget(void *inst, int n) { return (*(ref_fn *)(*(uint8_t **)inst + 8))(inst, VAR_ID(n)); }

static int in_fog_room(void *self) {
	static uint32_t frame = ~0u;
	static int fog;
	if (*frame_count != frame) {
		frame = *frame_count;
		fog = instance_exists(self, self, fogspawner);
	}
	return fog;
}

static RV get(void *self, int n) {
	RV inst = {0, 0, 6}, out = {0, 0, 5};
	memcpy(&inst.value, &self, sizeof(self));
	variable_get(&inst, VAR_ID(n), (int)0x80000000, &out, 0, 0);
	return out;
}
static void set(void *self, int n, RV v) { variable_set(self, VAR_ID(n), (int)0x80000000, &v); }
static RV real(double v) { RV r = {v, 0, 0}; return r; }

// The event without its lighting, reading and writing in the original's order with the runner's own calls
static void unlit(void *self) {
	RV *g_blackout = vget(*global_obj, BLACKOUT); // read at the event's start (a reference, tested later)
	if (!bool_value(vget(self, COLOR_ME))) {
		draw_self(self);
		return;
	}
	if ((bool_value(g_blackout) || bool_value(vget(self, BLACKOUT))) && !bool_value(vget(self, SPARK))) {
		RV a = get(self, IMAGE_ALPHA), c = get(self, IMAGE_BLEND);
		set(self, IMAGE_BLEND, real(4210752.0));
		set(self, IMAGE_ALPHA, real(1.0));
		draw_self(self);
		set(self, IMAGE_BLEND, real(0.0));
		set(self, IMAGE_ALPHA, real(0.75));
		draw_self(self);
		set(self, IMAGE_ALPHA, a);
		set(self, IMAGE_BLEND, c);
	} else {
		draw_self(self);
	}
	if (bool_value(vget(self, SPARK)) || bool_value(vget(self, SHADING)))
		return;
	RV a = get(self, IMAGE_ALPHA);
	a.value = a.value * 1.5; // operator*= of two reals (GV_ImageAlpha gives a real)
	set(self, IMAGE_ALPHA, a);
	// The plain lit layer: the instance's sprite, frame, position, scale and angle, a fixed colour
	RV frame = get(self, IMAGE_INDEX);
	if ((frame.kind & 0xffffff) == 0)
		draw_sprite_ext(self, (int)word(self, 0x88), (float)frame.value, field(self, 0xb8), field(self, 0xbc),
			field(self, 0xa4), field(self, 0xa8), field(self, 0xac), FOG_TRAIL_COLOUR, FOG_TRAIL_ALPHA * field(self, 0xb0));
	a = get(self, IMAGE_ALPHA);
	a.value = a.value / 1.5; // operator/= of two reals
	set(self, IMAGE_ALPHA, a);
}

static void sample_original(void *self) {
	RV *col = vget(self, LIGHT_COL), *la = vget(self, LIGHT_ALPHA);
	double raised = (double)field(self, 0xb0) * 1.5;
	if (!col || !la || (col->kind & 0xffffff) || (la->kind & 0xffffff) || !(raised > 0))
		return;
	uint32_t c = (uint32_t)(int64_t)col->value;
	double w = la->value > 1 ? 1 : la->value;
	fog_trail_sum[0] += (c & 0xff) * w;
	fog_trail_sum[1] += ((c >> 8) & 0xff) * w;
	fog_trail_sum[2] += ((c >> 16) & 0xff) * w;
	fog_trail_sum[3] += w;
	fog_trail_sum[4] += w / raised;
	fog_trail_samples++;
}

static void fog_trail_event(void *self, void *other) {
	if ((int)word(self, 0x84) == dustcloud && word(self, 0x50) == 1 && in_fog_room(self)) {
		if (!fog_trail_lights && !fog_trail_ab_off) {
			unlit(self);
			fog_trail_native++;
			return;
		}
		fog_trail_original++;
		original(self, other);
		if (sample)
			sample_original(self);
		return;
	}
	original(self, other);
}

// Wraps obj_particle's user event 15 (orig: what the caller would have called). Returns the wrapper, or orig.
event_fn fog_trail_install(event_fn orig) {
	extern int debugMode;
	int (*object_find)(const char *) = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	bool_value = (bool_fn)so_symbol(&yoyoloader_mod, "_Z11BOOL_RValuePK6RValue");
	variable_get = (getvalue_fn)so_symbol(&yoyoloader_mod, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	variable_set = (setdirect_fn)so_symbol(&yoyoloader_mod, "_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
	draw_self = (draw_self_fn)so_symbol(&yoyoloader_mod, "_Z15YYGML_draw_selfP9CInstance");
	draw_sprite_ext = (draw_ext_fn)so_symbol(&yoyoloader_mod, "_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
	instance_exists = (exists_fn)so_symbol(&yoyoloader_mod, "_Z21YYGML_instance_existsP9CInstanceS0_i");
	global_obj = (void **)so_symbol(&yoyoloader_mod, "g_pGlobal");
	frame_count = (uint32_t *)so_symbol(&yoyoloader_mod, "g_IOFrameCount");
	int ok = orig && object_find && bool_value && variable_get && variable_set && draw_self && draw_sprite_ext &&
		instance_exists && global_obj && frame_count;
	for (int n = 0; ok && n < NVARS; n++)
		ok = (var_desc[n] = (const int *)so_symbol(&yoyoloader_mod, var_names[n])) != NULL;
	if (ok) {
		dustcloud = object_find("obj_dustcloud");
		fogspawner = object_find("obj_fogspawner");
		ok = dustcloud >= 0 && fogspawner >= 0;
	}
	if (!ok) {
		debugPrintf("fog trail: NOT installed\n");
		return orig;
	}
	original = orig;
	sample = debugMode && exp_flag("fogtrailsample");
	fog_trail_ab = debugMode && exp_flag("fogtrailab");
	debugPrintf("fog trail: roll dust clouds in fog rooms drawn %s%s\n", fog_trail_lights ? "lit (fogTrailLights=1)" :
		"without lighting", sample ? " (sampling their tint)" : "");
	return fog_trail_event;
}
