/*
 * flame_lights.c
 *
 * The player's flamethrower doesn't light the room (flameLights=1 in yyl.cfg keeps the lights).
 *
 * The flamethrower pickup (room_mansion_2_2, room_mansion_3) fires obj_flamethrower_explosion instances, ~11 alive
 * while firing. player_flamethrower gives about one in four has_light, and that explosion's first Step (APK 1.1.10,
 * lines 2-14) creates an obj_orangelight that follows it; the light's first Step creates its obj_lightback. On the
 * Vita those ~3 extra lights cost ~1.5 ms a frame while firing (mansion_3), and not through their light: setting
 * their radius and alpha to 0 saved nothing measurable. Most of it is the light list rebuild each new light asks for
 * (global.update_lights): every lit sprite then walks all lights in its Draw (update_light_array, and
 * obj_drawactors' update_light_array_sorted, which moves each light it picks to y = -900000 and back through the
 * collision tree). In-run A/B on the Vita (500 MHz, mansion_3, firing, exp_flamelightsab): not creating the lights
 * but still setting the flag -0.34 ms; the skip below 20.91 -> 19.10 ms/frame (47.2 -> 51.6 fps).
 *
 * Here the light isn't created. What its creation and its life change outside the light is done instead:
 * - its instance id and layer element id (room_maxid + 1, CLayerManager::m_CurrentElementID + 1), and the reference
 *   instance_create_depth returns, so the explosion's `light` holds the same value. The skip happens only when the
 *   managed layers at the light's and the lightback's depths exist (otherwise a creation would add a layer);
 * - not its Create's one global write, global.update_lights = true (obj_lightsource Create line 17, inherited): that
 *   flag only asks this frame's Draw to rebuild light lists. Its readers are Draw events and the user events they
 *   draw with (if (global.update_lights) { update_region(); update_light_array(..); }), obj_lightmaster's Draw End
 *   resets it, and the `region` those rebuilds write is read only by lighting code (Steps read it only to compute it
 *   once). The Create draws no random number; its other writes are the light's own variables;
 * - its obj_lightback, created by the light's first Step in the next frame: an id and an element id are taken at the
 *   explosion's next Step if the explosion is still alive after it. The explosion steps before its light (it was
 *   created earlier), and when it dies its Clean Up destroys the light, which then never steps. So at every frame
 *   boundary room_maxid and the element counter are the original's. Within that next frame the lightback's id is
 *   taken at the explosion's Step instead of the light's, so the one or two instances created in between (the next
 *   explosion or its sound; 431 of 460 lights in a Vita3K run) get their ids in another order;
 * - the explosion's own code needs nothing else: it writes the light's variables with YYGML_Variable_SetValue,
 *   which does nothing for an id that doesn't exist (lines 5-12); the rest is behind instance_exists(light)
 *   (lines 134-152: the light's x, y, radius and alpha, and an end-of-life sound that is never reached, because
 *   when alpha <= 0 line 131 has already destroyed the explosion and its Clean Up the light); the Clean Up's
 *   with (light) finds no instance.
 * Nothing else reads these lights: light lists (update_light_array and its variants), recolor and rim-shading draws,
 * light_draw and the lightback event, all drawing code without random draws. The lights' and lightbacks' events draw
 * no random number either (the light doesn't move by itself: moving = 0).
 *
 * Not reproduced: the runner's bookkeeping (allocator, GC, instance lists) and the collision tree's shape. The light
 * has a collision mask and moves every frame, and the light list rebuilds move lights and call collision_line, so
 * the tree's shape differs: a query that names an object (instance_place, collision_line and the like) still finds
 * the same instances, but when several of them overlap the probe it can return another one of them, as with
 * collisionRoundTrip.
 *
 * All of this is APK 1.1.10 code: the explosion Step and Clean Up, the light's Create, Step and the lightsource
 * Create and Destroy it inherits, the lightback's Create and Step, and gml_Script_object_get_depth must match their
 * fingerprints before anything is hooked.
 *
 * exp_flamelightsverify (debugMode) creates the lights as before and checks the skip against them: at each light
 * creation the id, the element counter and the returned reference (and that the Create set global.update_lights to
 * true, the write the skip leaves out); at each frame boundary the
 * ids and element ids the skip would have taken against those the lights and lightbacks took; no random draw in an
 * explosion Step or a flame light's or lightback's events, no other instance created by an explosion Step, and the
 * light already gone when an explosion ends. [FLAMELIGHTS] verify counts must stay 0. exp_flamelightsfault (with
 * verify) leaves out the lightback's ids in every 8th prediction (negative control). exp_flamelightsab (debugMode)
 * creates the lights in every other report window (in-run A/B).
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

int flame_lights = 0; // flameLights (yyl.cfg): 1 keeps the explosion lights

uint32_t code_fingerprint(uintptr_t entry, uint32_t size); // fog_skip.c

typedef struct stack_trace { struct stack_trace *prev; const char *name; int line; } stack_trace;
typedef void *(*script_fn)(void *self, void *other, gml_rvalue *result, int argc, gml_rvalue **args);

// APK 1.1.10 object indices; Object_Find confirms them at the first explosion Step (objects load after install)
enum { OBJ_ORANGELIGHT = 710, OBJ_LIGHTBACK = 617 };

static builtin_fn orig_create_depth;
static event_func orig_expl_step, orig_light_step, orig_lightsource_begin, orig_back_step;
static stack_trace **trace_top;
static void **run_room;
static int *room_maxid, *element_id;
static uint32_t *id_map, *rng_state, *rng_index;
static void *(*layer_with_depth)(void *room, int depth, int dynamic_only);
static int (*get_int32)(const gml_rvalue *args, int index);
static script_fn object_get_depth;
static int (*object_find)(const char *);
static int *var_light, *var_has_light, *var_created_light, *var_back, *var_update_lights;
static int objects_ok; // 0 not checked yet, 1 confirmed, -1 other objects: everything runs the original
static int verify, fault, ab, ab_keep, late_pending;

// Explosions whose light was skipped (verify: created): the lightback's ids are taken at the explosion's next Step
#define MAX_PENDING 64
static struct { int expl; uint32_t serial; } pending[MAX_PENDING];
static int npending;
static uint32_t step_serial; // explosion Step calls, so the Step that skipped doesn't take the lightback ids too

static unsigned skipped, lightbacks, dropped, fallbacks;
static unsigned v_lights, v_light_bad, v_frames, v_frame_bad, v_steps, v_draws, v_extra_ids, v_light_steps,
	v_light_draws, v_end, v_end_alive, v_faults;
static int v_logged;

static void v_log(const char *fmt, int a, int b, int c, int d) {
	if (v_logged++ < 12) {
		char buf[200];
		snprintf(buf, sizeof(buf), fmt, a, b, c, d);
		debugPrintf("[FLAMELIGHTS] %s\n", buf);
	}
}

static void *instance_by_id(int id) {
	if (!id_map || !id_map[0])
		return NULL;
	const uint32_t *e = (const uint32_t *)(uintptr_t)((const uint32_t *)(uintptr_t)id_map[0])[(id_map[1] & (uint32_t)id) * 2];
	for (; e; e = (const uint32_t *)(uintptr_t)e[1])
		if ((int)e[2] == id)
			return (void *)(uintptr_t)e[3];
	return NULL;
}

static int live(void *inst) {
	return inst && !(*(uint32_t *)((uint8_t *)inst + 0x7c) & 1);
}

static int instance_id(void *inst) {
	return *(int *)((uint8_t *)inst + 0x80);
}

// bools are kind 13 with a double payload; reals and int32 too: any payload bit but the sign is true
static int truthy(void *self, int *var) {
	gml_rvalue *v = gml_vget(self, var[1]);
	if (!v)
		return 0;
	uint32_t kind = v->kind & 0xffffff;
	if (kind != 0 && kind != 7 && kind != 13)
		return 0;
	uint64_t bits;
	memcpy(&bits, &v->val, 8);
	return kind == 7 ? (uint32_t)bits != 0 : (bits << 1) != 0;
}

static int ref_id(void *self, int *var) {
	gml_rvalue *v = gml_vget(self, var[1]);
	if (!v)
		return -1;
	uint32_t kind = v->kind & 0xffffff, w[2];
	memcpy(w, &v->val, sizeof(w));
	if (kind == 15)
		return (int)w[0];
	return kind == 0 ? (int)v->val : -1;
}

static int object_arg(const gml_rvalue *v) {
	if ((v->kind & 0xffffff) == 15) {
		uint32_t w[2];
		memcpy(w, &v->val, sizeof(w));
		return (int)w[0];
	}
	return (v->kind & 0xffffff) == 0 ? (int)v->val : -1;
}

typedef struct { uint32_t s[16], i; } rng_t;

static void rng_read(rng_t *r) {
	memcpy(r->s, rng_state, sizeof(r->s));
	r->i = *rng_index;
}

static int rng_same(const rng_t *r) {
	return r->i == *rng_index && !memcmp(r->s, rng_state, sizeof(r->s));
}

static int skipping(void) {
	return !verify && !(ab && ab_keep);
}

// Verify: global.update_lights, read the way obj_lightsource Create reads it (line 0) before writing it (line 17)
static gml_rvalue *update_lights_slot(void) {
	return global_obj_ptr && *global_obj_ptr ? gml_vget(*global_obj_ptr, var_update_lights[1]) : NULL;
}

// The managed layer the lightback goes on (instance_create uses gml_Script_object_get_depth(obj_lightback))
static int lightback_layer_exists(void *self, void *other) {
	gml_rvalue arg = {0}, result = {0}, *args[1] = {&arg};
	uint32_t w[2] = {OBJ_LIGHTBACK, 0x01000000}; // an object reference
	memcpy(&arg.val, w, sizeof(w));
	arg.kind = 15;
	object_get_depth(self, other, &result, 1, args);
	if ((result.kind & 0xffffff) != 0)
		return 0;
	return layer_with_depth(*run_room, (int)result.val, 1) != NULL;
}

static int at_light_site(void) {
	stack_trace *t = *trace_top;
	return t && t->name && !strcmp(t->name, "gml_Object_obj_flamethrower_explosion_Step_0");
}

static void pending_add(int expl) {
	if (npending == MAX_PENDING) { // explosions that never stepped again (room end): drop the oldest
		memmove(pending, pending + 1, (MAX_PENDING - 1) * sizeof(pending[0]));
		npending--;
		dropped++;
	}
	pending[npending].expl = expl;
	pending[npending].serial = step_serial;
	npending++;
}

// ---- verify bookkeeping --------------------------------------------------------------------------------------------

// Ids and element ids the skip would have taken minus those the flame lights' lightbacks took (the lights' own are
// taken by the creation either way); must be 0 at every frame boundary
static int predicted_minus_actual[2];
static uint32_t last_frame;

static void frame_check(void) {
	if (*frame_count == last_frame)
		return;
	last_frame = *frame_count;
	v_frames++;
	if (predicted_minus_actual[0] || predicted_minus_actual[1]) {
		v_frame_bad++;
		v_log("frame %d: lightback ids taken by the skip minus the original's %d, element ids %d", (int)last_frame,
			predicted_minus_actual[0], predicted_minus_actual[1], 0);
		predicted_minus_actual[0] = predicted_minus_actual[1] = 0;
	}
}

#define MAX_LIGHTS 128
static int flame_ids[MAX_LIGHTS], nflame;

static int is_flame(int id) {
	for (int i = 0; i < nflame; i++)
		if (flame_ids[i] == id)
			return 1;
	return 0;
}

static void add_flame(int id) {
	if (id < 0 || is_flame(id))
		return;
	if (nflame == MAX_LIGHTS) // the oldest are long gone: a light lives as long as its explosion
		memmove(flame_ids, flame_ids + 1, (MAX_LIGHTS - 1) * sizeof(int)), nflame--;
	flame_ids[nflame++] = id;
}

// ---- hooks ----------------------------------------------------------------------------------------------------------

static void create_depth_hook(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (objects_ok != 1 || argc != 4 || object_arg(&args[3]) != OBJ_ORANGELIGHT || !self || !at_light_site() ||
			!*run_room) {
		orig_create_depth(result, self, other, argc, args);
		return;
	}
	if (!layer_with_depth(*run_room, get_int32(args, 2), 1) || !lightback_layer_exists(self, other)) {
		fallbacks++;
		orig_create_depth(result, self, other, argc, args);
		return;
	}
	int id = *room_maxid + 1, element = *element_id + 1;
	if (skipping()) {
		*room_maxid = id;
		*element_id = element;
		uint32_t w[2] = {(uint32_t)id, 0x04000001}; // the reference F_InstanceCreateDepth returns
		memcpy(&result->val, w, sizeof(w));
		result->flags = 0;
		result->kind = 15;
		pending_add(instance_id(self));
		skipped++;
		return;
	}
	orig_create_depth(result, self, other, argc, args);
	if (!verify)
		return;
	v_lights++;
	uint32_t w[2];
	memcpy(w, &result->val, sizeof(w));
	gml_rvalue *ul = update_lights_slot();
	uint64_t bits = 0;
	if (ul)
		memcpy(&bits, &ul->val, 8);
	if ((result->kind & 0xffffff) != 15 || (int)w[0] != id || w[1] != 0x04000001 || *room_maxid != id ||
			*element_id != element || !ul || (ul->kind & 0xffffff) != 13 || bits != 0x3ff0000000000000ull) {
		v_light_bad++;
		v_log("light creation: id %d/%d, element %d/%d", (int)w[0], id, *element_id, element);
		v_log("light creation: update_lights kind %d payload %08x%08x, reference type %08x", ul ? (int)ul->kind : -1,
			(int)(bits >> 32), (int)bits, (int)w[1]);
	}
	add_flame(id);
	pending_add(instance_id(self));
}

static void expl_step(void *self, void *other) {
	if (!objects_ok) {
		int o = object_find("obj_orangelight"), b = object_find("obj_lightback");
		objects_ok = o == OBJ_ORANGELIGHT && b == OBJ_LIGHTBACK ? 1 : -1;
		debugPrintf("flame lights: %s (obj_orangelight %d, obj_lightback %d)\n", objects_ok == 1 ? "objects confirmed" :
			"NOT active, other objects", o, b);
	}
	if (verify)
		frame_check();
	uint32_t serial = ++step_serial;
	int expl = instance_id(self), had_light = verify && truthy(self, var_created_light), maxid = *room_maxid;
	rng_t before;
	if (verify)
		rng_read(&before);
	orig_expl_step(self, other);
	// This explosion's light was skipped (verify: created) in an earlier Step. The light's first Step, after this
	// one, creates the lightback, unless this explosion just died and its Clean Up destroyed the light.
	for (int i = 0; i < npending; i++) {
		if (pending[i].expl != expl || pending[i].serial >= serial)
			continue;
		if (live(self)) {
			if (verify) {
				static unsigned n;
				int faulty = fault && (n++ % 8) == 7;
				v_faults += faulty;
				if (!faulty) {
					predicted_minus_actual[0]++;
					predicted_minus_actual[1]++;
				}
			} else {
				++*room_maxid;
				++*element_id;
				lightbacks++;
			}
		}
		pending[i] = pending[--npending];
		break;
	}
	if (!verify)
		return;
	v_steps++;
	if (!rng_same(&before))
		v_draws++;
	int created = !had_light && truthy(self, var_created_light);
	if (*room_maxid - maxid != created)
		v_extra_ids++;
	// The explosion ended in this Step (instance_destroy at line 131): its light must be gone already
	if (!live(self) && truthy(self, var_has_light) && truthy(self, var_created_light)) {
		v_end++;
		if (live(instance_by_id(ref_id(self, var_light))))
			v_end_alive++;
	}
}

// Verify: the flame lights' and lightbacks' events
static void checked(event_func orig, void *self, void *other, int light) {
	int id = instance_id(self);
	if (!is_flame(id)) {
		orig(self, other);
		return;
	}
	rng_t before;
	rng_read(&before);
	int back = light ? ref_id(self, var_back) : -1, maxid = *room_maxid, element = *element_id;
	orig(self, other);
	v_light_steps++;
	if (!rng_same(&before))
		v_light_draws++;
	if (light) {
		int b = ref_id(self, var_back);
		if (b >= 0 && b != back) { // the lightback was created in this Step
			add_flame(b);
			predicted_minus_actual[0] -= *room_maxid - maxid;
			predicted_minus_actual[1] -= *element_id - element;
		}
	}
}

static void light_step(void *self, void *other) { checked(orig_light_step, self, other, 1); }
static void lightsource_begin(void *self, void *other) { checked(orig_lightsource_begin, self, other, 0); }
static void back_step(void *self, void *other) { checked(orig_back_step, self, other, 0); }

void flame_lights_report(int frames) {
	if (!orig_create_depth)
		return;
	if (ab) {
		debugPrintf("[FLAMELIGHTS] A/B window: explosion lights %s\n", ab_keep ? "created (game)" : "not created (skip)");
		ab_keep = !ab_keep;
	}
	if (skipped || fallbacks)
		debugPrintf("[FLAMELIGHTS] %u explosion lights not created (%u lightback ids taken), %u created (no layer), %u "
			"pending dropped\n", skipped, lightbacks, fallbacks, dropped);
	if (verify && (v_steps || v_lights))
		debugPrintf("[FLAMELIGHTS] verify: light creations %u (%u mismatch), frames %u (id counters %u mismatch%s), "
			"explosion Steps %u (random draws in %u, other instances created in %u), flame light/lightback events %u "
			"(random draws in %u), explosions ended with a light %u (light still alive %u)\n", v_lights, v_light_bad,
			v_frames, v_frame_bad, fault ? ", faults injected" : "", v_steps, v_draws, v_extra_ids, v_light_steps,
			v_light_draws, v_end, v_end_alive);
	skipped = lightbacks = fallbacks = 0;
	(void)frames;
}

// ---- install --------------------------------------------------------------------------------------------------------

void flame_lights_install(gml_func_entry *table) {
	if (flame_lights) {
		debugPrintf("flame lights: the game's (flameLights=1)\n");
		return;
	}
#define FL_RESOLVE(var, sym) var = (void *)so_symbol(&yoyoloader_mod, sym)
	FL_RESOLVE(var_light, "g_VAR_light");
	FL_RESOLVE(var_has_light, "g_VAR_has_light");
	FL_RESOLVE(var_created_light, "g_VAR_created_light");
	FL_RESOLVE(var_back, "g_VAR_back");
	FL_RESOLVE(var_update_lights, "g_VAR_update_lights");
	FL_RESOLVE(trace_top, "_ZN13SYYStackTrace8s_pStartE");
	FL_RESOLVE(run_room, "Run_Room");
	FL_RESOLVE(room_maxid, "room_maxid");
	FL_RESOLVE(element_id, "_ZN13CLayerManager18m_CurrentElementIDE");
	FL_RESOLVE(id_map, "_ZN9CInstance14ms_ID2InstanceE");
	FL_RESOLVE(rng_state, "state");
	FL_RESOLVE(rng_index, "g_RndIndex");
	FL_RESOLVE(layer_with_depth, "_ZN13CLayerManager17GetLayerWithDepthEP5CRoomib");
	FL_RESOLVE(get_int32, "_Z10YYGetInt32PK6RValuei");
	FL_RESOLVE(object_get_depth, "_Z27gml_Script_object_get_depthP9CInstanceS0_R8YYRValueiPPS1_");
	FL_RESOLVE(object_find, "_Z11Object_FindPKc");
#undef FL_RESOLVE
	if (!var_light || !var_has_light || !var_created_light || !var_back || !var_update_lights || !trace_top ||
			!run_room || !room_maxid || !element_id || !id_map || !rng_state || !rng_index || !layer_with_depth ||
			!get_int32 || !object_get_depth || !object_find || !global_obj_ptr || !frame_count || !table) {
		debugPrintf("flame lights: NOT installed (symbols)\n");
		return;
	}
	static const struct { const char *symbol; uint32_t size, fingerprint; } code[] = {
		{"_Z44gml_Object_obj_flamethrower_explosion_Step_0P9CInstanceS0_", 0x431c, 0x4c92dbf0},
		{"_Z47gml_Object_obj_flamethrower_explosion_CleanUp_0P9CInstanceS0_", 0x1bc, 0xbc8257c7},
		{"_Z35gml_Object_obj_orangelight_Create_0P9CInstanceS0_", 0xb9c, 0x3a0cfe39},
		{"_Z33gml_Object_obj_orangelight_Step_0P9CInstanceS0_", 0x3280, 0x3b525b26},
		{"_Z35gml_Object_obj_lightsource_Create_0P9CInstanceS0_", 0x994, 0xa2862b5b},
		{"_Z36gml_Object_obj_lightsource_Destroy_0P9CInstanceS0_", 0x18c, 0xba658b74},
		{"_Z33gml_Object_obj_lightback_Create_0P9CInstanceS0_", 0x298, 0x2ec31ace},
		{"_Z31gml_Object_obj_lightback_Step_0P9CInstanceS0_", 0x1bc, 0x29ed6445},
		{"_Z27gml_Script_object_get_depthP9CInstanceS0_R8YYRValueiPPS1_", 0x50c, 0x79904af3},
	};
	for (unsigned i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
		uint32_t f = code_fingerprint(so_symbol(&yoyoloader_mod, code[i].symbol), code[i].size);
		if (f != code[i].fingerprint) {
			debugPrintf("flame lights: NOT installed (%s fingerprint %08x, expected %08x)\n", code[i].symbol, f,
				code[i].fingerprint);
			return;
		}
	}
	verify = debugMode && exp_flag("flamelightsverify");
	fault = verify && exp_flag("flamelightsfault");
	ab = debugMode && !verify && exp_flag("flamelightsab");
	if (!(orig_expl_step = gml_replace(table, "gml_Object_obj_flamethrower_explosion_Step_0", expl_step))) {
		debugPrintf("flame lights: NOT installed (explosion Step)\n");
		return;
	}
	if (verify) {
		orig_light_step = gml_replace(table, "gml_Object_obj_orangelight_Step_0", light_step);
		orig_lightsource_begin = gml_replace(table, "gml_Object_obj_lightsource_Step_1", lightsource_begin);
		orig_back_step = gml_replace(table, "gml_Object_obj_lightback_Step_0", back_step);
		if (!orig_light_step || !orig_lightsource_begin || !orig_back_step) {
			debugPrintf("flame lights: verify NOT installed (light events)\n");
			verify = 0;
		}
	}
	late_pending = 1;
}

// katana_late_install, after hud_sparks_late_install: the runner's function registry is filled by then
void flame_lights_late_install(void) {
	if (!late_pending)
		return;
	late_pending = 0;
	runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	for (int i = 0; functions && count && *functions && i < *count; i++) {
		runner_function *f = &(*functions)[i];
		if (f->name && !strcmp(f->name, "instance_create_depth")) {
			orig_create_depth = builtin_chain((void **)&f->func, create_depth_hook);
			// (chained: see hooks.c)
			break;
		}
	}
	debugPrintf("flame lights: %s%s%s%s\n", orig_create_depth ? "installed" : "NOT installed (instance_create_depth)",
		verify ? " (verify mode)" : "", fault ? " (with faults)" : "", ab ? " (A/B per report window)" : "");
}
