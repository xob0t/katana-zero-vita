/*
 * rain_skip.c
 *
 * With the default cheap rain (rain=1, rain_cheap.c) and with rain=0, every obj_rain drop the game spawns is destroyed
 * right after its Create event (the cheap rain keeps only the first drop of a shower as its carrier). Each of those
 * drops is still a CInstance plus two GML arrays (the Create's prev_x/prev_y trail), all GC objects: in
 * room_factory_0 ~3 drops per frame, and that churn drives the game's generational GC to an old-generation pass of
 * 6-10 ms about three times a second, which pushes 13-15 ms frames over the 16.7 ms budget (the judder near the
 * clothesline).
 *
 * Here those drops aren't created. The spawners (obj_cutscene_factory_0, obj_bgPlayerApt, obj_cutscene_alleyway and
 * obj_cutscene_alleyway_drug Step) all run
 *
 *     bb = instance_create(x + random(..), y, obj_rain);   // gml_Script_instance_create -> instance_create_depth
 *     bb.direction = 270; bb.parent = id;
 *     with (bb) { hsp = 0; vsp = 0; momentum_add(direction, 30); }
 *
 * and a drop that is destroyed in its Create is only marked at that point: it leaves the instance tables at the end of
 * the event sweep. What the creation changes outside the drop is done instead:
 * - the instance id: room_maxid + 1, returned as the same instance reference, so bb holds the id the drop had;
 * - the layer element id (CLayerManager::m_CurrentElementID + 1); the skip only happens when the managed layer at the
 *   drop's depth already exists (otherwise the drop would create it and take a layer id), else the drop is created
 *   and destroyed as before;
 * - the random numbers: the drop's Create draws two (trail_length = choose(..), jumpthrough_collide = choose(..));
 *   everything else in it writes the drop's own variables;
 * - the cheap rain's own drop list (rain_cheap_phantom: the drop's position, as rain_cheap.c's Create wrapper takes it).
 * The statements after the creation see a destroyed (marked) drop in the original and a missing one here, and the
 * runner treats both the same: YYGML_Variable_SetValue and YYGML_Variable_GetValue return without doing anything,
 * with (bb) runs no iteration. The one exception is Variable_SetValue (bb.direction, a built-in variable), which
 * returns 0 for a marked instance but raises an error for an id it can't find: it is entry-hooked to return 0 for the
 * ids skipped in the current frame. Game queries (with, instance_number, instance_exists, collision functions'
 * callbacks) skip marked instances, so nothing else in the game could see the destroyed drop either.
 *
 * Not reproduced: the runner's own bookkeeping (allocator, GC, ID map, layer element pool, the marked-instance count,
 * the collision dirty/test lists). The drops never enter the collision R-tree: CRoom::AddInstance's CollisionInsert
 * computes the box but CollisionUpdate inserts nothing for them (the in-tree flag 0x8000 is never set; verify mode
 * counts it), and a marked instance is left out of later tree updates, so the tree is the same with or without them.
 *
 * All of this is APK 1.1.10 code: the four spawners, gml_Script_instance_create, gml_Script_object_get_depth and the
 * drop's Create must match their 1.1.10 fingerprints (code_fingerprint, fog_skip.c) before anything is hooked.
 *
 * exp_rainskip0 creates and destroys the drops as before. Debug builds report [RAINSKIP] with the profiler.
 * exp_rainskipverify (debug) creates every drop as before, but first runs the skip on the live state, records the
 * result, puts the state back, and after the creation compares the random state, the id, layer and create counters
 * and the returned reference, and checks that the drop was destroyed in its Create, never entered the collision tree
 * and sits where rain_cheap_phantom would put the loader drop. For the first samples it diffs
 * the runner's whole .data/.bss after the creation against the skip. exp_rainskipfault (with verify) leaves one
 * random draw out of every 8th skip, as a negative control.
 */
#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

uint32_t code_fingerprint(uintptr_t entry, uint32_t size); // fog_skip.c

// rain_cheap.c (rain=1): whether a drop created now would be destroyed (the carrier is alive), and the bookkeeping of
// its Create wrapper for one destroyed drop at x, y
int rain_cheap_drop_destroyed(void);
void rain_cheap_phantom(float x, float y);
extern int rain_level, rain_none_installed;

typedef struct stack_trace {
	struct stack_trace *prev;
	const char *name;
	int line;
} stack_trace;

static const char *const spawners[] = {"gml_Object_obj_cutscene_factory_0_Step_0", "gml_Object_obj_bgPlayerApt_Step_0",
	"gml_Object_obj_cutscene_alleyway_Step_0", "gml_Object_obj_cutscene_alleyway_drug_Step_0"};

static builtin_fn orig_create_depth;
static stack_trace **trace_top;
static void **run_room;
static int *room_maxid, *element_id, *layer_id;
static uint32_t *rng_state, *rng_index;
static uint32_t *create_counter; // CInstance::ms_CurrentCreateCounter (64-bit)
static uint32_t *id_map;          // CInstance::ms_ID2Instance {buckets, mask, count}
static void *(*layer_with_depth)(void *room, int depth, int dynamic_only);
static int (*get_int32)(gml_rvalue *args, int index);
static float (*get_float)(gml_rvalue *args, int index);
static int (*random_int)(int n);
static int (*orig_set_value)(int id, int var, int index, gml_rvalue *value);
static int rain_object = -1;
static int verify, fault;

unsigned rain_skipped, rain_skip_fallback, rain_skip_setter, rain_skip_total;

// Ids skipped in the current frame (a marked drop leaves the runner's tables at the end of its sweep, in the same frame)
#define PHANTOM_MAX 32
static int phantoms[PHANTOM_MAX], phantom_count;
static uint32_t phantom_frame;

static void phantom_add(int id) {
	if (phantom_frame != *frame_count) {
		phantom_frame = *frame_count;
		phantom_count = 0;
	}
	if (phantom_count < PHANTOM_MAX)
		phantoms[phantom_count++] = id;
}

static int instance_known(int id) {
	if (!id_map || !id_map[0])
		return 1;
	const uint32_t *e = (const uint32_t *)(uintptr_t)((const uint32_t *)(uintptr_t)id_map[0])[(id_map[1] & (uint32_t)id) * 2];
	for (; e; e = (const uint32_t *)(uintptr_t)e[1])
		if ((int)e[2] == id)
			return 1;
	return 0;
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

// Variable_SetValue(int id, ...): for a skipped drop, what it does for the marked one (return 0, nothing written)
static int set_value_hook(int id, int var, int index, gml_rvalue *value) {
	if (phantom_count && id > 99999 && phantom_frame == *frame_count) {
		for (int i = 0; i < phantom_count; i++)
			if (phantoms[i] == id && !instance_known(id)) {
				rain_skip_setter++;
				return 0;
			}
	}
	return orig_set_value(id, var, index, value);
}

static int code_is_1110(void) {
	static const struct { const char *symbol; uint32_t size, fingerprint; } code[] = {
		{"_Z40gml_Object_obj_cutscene_factory_0_Step_0P9CInstanceS0_", 0xc570, 0xda963ddf},
		{"_Z33gml_Object_obj_bgPlayerApt_Step_0P9CInstanceS0_", 0xdc0, 0x140efafc},
		{"_Z39gml_Object_obj_cutscene_alleyway_Step_0P9CInstanceS0_", 0x52a0, 0x024d1878},
		{"_Z44gml_Object_obj_cutscene_alleyway_drug_Step_0P9CInstanceS0_", 0x2d7c, 0x9eb6112f},
		{"_Z28gml_Object_obj_rain_Create_0P9CInstanceS0_", 0xacc, 0x7e6994f5},              // two choose() draws
		{"_Z26gml_Script_instance_createP9CInstanceS0_R8YYRValueiPPS1_", 0x59c, 0x049bde56}, // -> instance_create_depth
		{"_Z27gml_Script_object_get_depthP9CInstanceS0_R8YYRValueiPPS1_", 0x50c, 0x79904af3},
	};
	for (unsigned i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
		uint32_t f = code_fingerprint(so_symbol(&yoyoloader_mod, code[i].symbol), code[i].size);
		if (f != code[i].fingerprint) {
			debugPrintf("rain skip: %s fingerprint %08x, expected %08x\n", code[i].symbol, f, code[i].fingerprint);
			return 0;
		}
	}
	return 1;
}

static int object_arg(const gml_rvalue *v) {
	if ((v->kind & 0xffffff) == 15) { // object reference: index in the low word
		uint32_t w[2];
		memcpy(w, &v->val, sizeof(w));
		return (int)w[0];
	}
	return v->kind == 0 ? (int)v->val : -1;
}

// instance_create_depth called by gml_Script_instance_create, called by one of the spawners
static int at_spawn_site(void) {
	stack_trace *t = *trace_top;
	if (!t || !t->name || strcmp(t->name, "gml_Script_instance_create") || !(t = t->prev) || !t->name)
		return 0;
	for (unsigned i = 0; i < sizeof(spawners) / sizeof(*spawners); i++)
		if (!strcmp(t->name, spawners[i]))
			return 1;
	return 0;
}

// Whether the drop would be destroyed in its Create: always with rain=0, while the carrier lives with cheap rain
static int drop_destroyed(void) {
	return rain_level == 0 ? rain_none_installed : rain_level == 1 && rain_cheap_drop_destroyed();
}

// What the skip changes; compared field by field in verify mode
typedef struct {
	uint32_t rng[16], rng_index;
	int maxid, element, layer;
	uint32_t create_lo, create_hi;
} rain_state;

static void state_read(rain_state *s) {
	memcpy(s->rng, rng_state, sizeof(s->rng));
	s->rng_index = *rng_index;
	s->maxid = *room_maxid;
	s->element = *element_id;
	s->layer = *layer_id;
	s->create_lo = create_counter[0];
	s->create_hi = create_counter[1];
}

static void state_write(const rain_state *s) {
	memcpy(rng_state, s->rng, sizeof(s->rng));
	*rng_index = s->rng_index;
	*room_maxid = s->maxid;
	*element_id = s->element;
	*layer_id = s->layer;
	create_counter[0] = s->create_lo;
	create_counter[1] = s->create_hi;
}

// The skip itself: returns the id the drop would have had
static int skip_drop(int faulty) {
	int id = ++*room_maxid;
	++*element_id;
	random_int(2); // Create: trail_length = choose(..)
	if (!faulty)
		random_int(2); // Create: jumpthrough_collide = choose(true, false)
	return id;
}

static void set_instance_ref(gml_rvalue *result, int id) {
	uint32_t w[2] = {(uint32_t)id, 0x04000001}; // as F_InstanceCreateDepth returns it
	memcpy(&result->val, w, sizeof(w));
	result->kind = 15;
}

// ---- verify mode -------------------------------------------------------------------------------------------------

#define DIFF_SAMPLES 6
#define DIFF_MAX 384
static uint32_t *seg_copy;
static size_t seg_words[4];
static uint32_t *seg_base[4];
static int seg_count, diff_samples;
static struct { uint32_t off, val; } diff_emu[DIFF_MAX], diff_orig[DIFF_MAX];
static int diff_emu_n, diff_orig_n;

unsigned rain_verified, rain_mismatch, rain_not_destroyed, rain_in_tree, rain_faults, rain_xy_mismatch;

static void seg_snapshot(void) {
	uint32_t *p = seg_copy;
	for (int i = 0; i < seg_count; i++) {
		memcpy(p, seg_base[i], seg_words[i] * 4);
		p += seg_words[i];
	}
}

static int seg_diff(void *out_v, int max) {
	struct { uint32_t off, val; } *out = out_v;
	int n = 0;
	const uint32_t *p = seg_copy;
	for (int i = 0; i < seg_count; i++) {
		for (size_t w = 0; w < seg_words[i]; w++)
			if (seg_base[i][w] != p[w] && n < max) {
				out[n].off = (uint32_t)((uintptr_t)&seg_base[i][w] - yoyoloader_mod.text_base);
				out[n].val = seg_base[i][w];
				n++;
			}
		p += seg_words[i];
	}
	return n;
}

static void diff_report(void) {
	// Words the original creation changed whose value differs from the skip's (or that the skip changed and the
	// original did not). Offsets are .so virtual addresses.
	int shown = 0;
	char line[512];
	int len = snprintf(line, sizeof(line), "[RAINSKIP] diff sample %d: orig changed %d words, skip %d; differing:",
		diff_samples, diff_orig_n, diff_emu_n);
	for (int i = 0; i < diff_orig_n; i++) {
		uint32_t emu = 0;
		int found = 0;
		for (int j = 0; j < diff_emu_n; j++)
			if (diff_emu[j].off == diff_orig[i].off) {
				emu = diff_emu[j].val;
				found = 1;
				break;
			}
		if (found && emu == diff_orig[i].val)
			continue;
		if (len < (int)sizeof(line) - 40)
			len += snprintf(line + len, sizeof(line) - len, " %x%s", diff_orig[i].off, found ? "!" : "");
		if (++shown % 24 == 0) {
			debugPrintf("%s\n", line);
			len = snprintf(line, sizeof(line), "[RAINSKIP]  ...");
		}
	}
	for (int j = 0; j < diff_emu_n; j++) {
		int found = 0;
		for (int i = 0; i < diff_orig_n && !found; i++)
			found = diff_orig[i].off == diff_emu[j].off;
		if (!found && len < (int)sizeof(line) - 40)
			len += snprintf(line + len, sizeof(line) - len, " skip-only:%x", diff_emu[j].off);
	}
	debugPrintf("%s\n", line);
}

static void verify_create(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	static unsigned n;
	int faulty = fault && (n++ % 8) == 7;
	rain_state before, predicted, now;
	state_read(&before);
	int diff = seg_copy != NULL;
	if (diff)
		seg_snapshot();
	int id = skip_drop(faulty);
	state_read(&predicted);
	if (diff)
		diff_emu_n = seg_diff(diff_emu, DIFF_MAX);
	state_write(&before);
	orig_create_depth(result, self, other, argc, args);
	state_read(&now);
	rain_faults += faulty;
	uint32_t w[2];
	memcpy(w, &result->val, sizeof(w));
	void *drop = instance_by_id(id);
	uint32_t flags = drop ? *(uint32_t *)((uint8_t *)drop + 0x7c) : 0;
	if (!drop || !(flags & 1)) {
		if (rain_not_destroyed++ < 8)
			debugPrintf("[RAINSKIP] drop %d %s after its creation (flags %08x)\n", id, drop ? "not destroyed" : "not found", flags);
	}
	if (flags & 0x8000)
		rain_in_tree++;
	// The position rain_cheap_phantom gets is the one the drop was created at
	float x = get_float(args, 0), y = get_float(args, 1);
	if (drop && (memcmp((uint8_t *)drop + 0xb8, &x, 4) || memcmp((uint8_t *)drop + 0xbc, &y, 4)) && rain_xy_mismatch++ < 4)
		debugPrintf("[RAINSKIP] drop %d at %f,%f, arguments %f,%f\n", id, *(float *)((uint8_t *)drop + 0xb8),
			*(float *)((uint8_t *)drop + 0xbc), x, y);
	if (result->kind == 15 && (int)w[0] == id && w[1] == 0x04000001 && !memcmp(&now, &predicted, sizeof(now)))
		rain_verified++;
	else if (rain_mismatch++ < 8)
		debugPrintf("[RAINSKIP] mismatch id %d: returned kind %u %08x:%08x, maxid %d/%d element %d/%d layer %d/%d "
			"create %u/%u rng %s%s\n", id, result->kind, w[1], w[0], now.maxid, predicted.maxid, now.element,
			predicted.element, now.layer, predicted.layer, now.create_lo, predicted.create_lo,
			memcmp(now.rng, predicted.rng, sizeof(now.rng)) || now.rng_index != predicted.rng_index ? "differs" : "same",
			faulty ? " (fault)" : "");
	if (diff) {
		diff_orig_n = seg_diff(diff_orig, DIFF_MAX);
		diff_report();
		if (++diff_samples >= DIFF_SAMPLES) {
			free(seg_copy);
			seg_copy = NULL;
		}
	}
}

// ---- the hook ------------------------------------------------------------------------------------------------------

static void create_depth_hook(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (argc == 4 && object_arg(&args[3]) == rain_object && at_spawn_site() && drop_destroyed()) {
		if (!*run_room || !layer_with_depth(*run_room, get_int32(args, 2), 1)) {
			rain_skip_fallback++;
		} else if (verify) {
			verify_create(result, self, other, argc, args);
			return;
		} else {
			float x = get_float(args, 0), y = get_float(args, 1);
			int id = skip_drop(0);
			if (rain_level == 1)
				rain_cheap_phantom(x, y);
			phantom_add(id);
			rain_skipped++;
			rain_skip_total++;
			set_instance_ref(result, id);
			return;
		}
	}
	orig_create_depth(result, self, other, argc, args);
}

void rain_skip_report(int frames) {
	if (!orig_create_depth || (!rain_skipped && !rain_skip_fallback && !rain_verified && !rain_mismatch))
		return;
	debugPrintf("[RAINSKIP] %s: %.2f drops not created/frame (%u), %u fallbacks (no layer yet), %u setter calls on "
		"skipped drops\n", verify ? "verify" : "skip", frames ? (float)rain_skipped / frames : 0.0f, rain_skipped,
		rain_skip_fallback, rain_skip_setter);
	if (verify)
		debugPrintf("[RAINSKIP] verify: %u match, %u mismatch (%u faults injected); drops not destroyed in their Create %u, "
			"in the collision tree %u, position mismatches %u\n", rain_verified, rain_mismatch, rain_faults,
			rain_not_destroyed, rain_in_tree, rain_xy_mismatch);
	rain_skipped = rain_skip_fallback = rain_skip_setter = 0;
}

void rain_skip_install(void) {
	if ((rain_level != 0 && rain_level != 1) || exp_flag("rainskip0")) {
		debugPrintf("rain skip: off\n");
		return;
	}
#define RS_RESOLVE(var, sym) var = (void *)so_symbol(&yoyoloader_mod, sym)
	RS_RESOLVE(trace_top, "_ZN13SYYStackTrace8s_pStartE");
	RS_RESOLVE(run_room, "Run_Room");
	RS_RESOLVE(room_maxid, "room_maxid");
	RS_RESOLVE(element_id, "_ZN13CLayerManager18m_CurrentElementIDE");
	RS_RESOLVE(layer_id, "_ZN13CLayerManager16m_CurrentLayerIDE");
	RS_RESOLVE(create_counter, "_ZN9CInstance23ms_CurrentCreateCounterE");
	RS_RESOLVE(id_map, "_ZN9CInstance14ms_ID2InstanceE");
	RS_RESOLVE(rng_state, "state");
	RS_RESOLVE(rng_index, "g_RndIndex");
	RS_RESOLVE(layer_with_depth, "_ZN13CLayerManager17GetLayerWithDepthEP5CRoomib");
	RS_RESOLVE(get_int32, "_Z10YYGetInt32PK6RValuei");
	RS_RESOLVE(get_float, "_Z10YYGetFloatPK6RValuei");
	RS_RESOLVE(random_int, "_Z8YYRandomi");
	int (*object_find)(const char *);
	RS_RESOLVE(object_find, "_Z11Object_FindPKc");
	runner_function **functions;
	int *count;
	RS_RESOLVE(functions, "the_functions");
	RS_RESOLVE(count, "the_numb");
	uintptr_t set_value = so_symbol(&yoyoloader_mod, "_Z17Variable_SetValueiiiP6RValue");
#undef RS_RESOLVE
	if (!frame_count || !trace_top || !run_room || !room_maxid || !element_id || !layer_id || !create_counter || !id_map ||
			!rng_state || !rng_index || !layer_with_depth || !get_int32 || !get_float || !random_int || !object_find ||
			!functions || !count || !set_value) {
		debugPrintf("rain skip: NOT installed (symbols)\n");
		return;
	}
	if (!code_is_1110()) {
		debugPrintf("rain skip: NOT installed (not the APK 1.1.10 code)\n");
		return;
	}
	rain_object = object_find("obj_rain");
	verify = debugMode && exp_flag("rainskipverify");
	fault = verify && exp_flag("rainskipfault");
	if (rain_object < 0) {
		debugPrintf("rain skip: NOT installed (obj_rain)\n");
		return;
	}
	if (!verify && !(orig_set_value = entry_hook(set_value, set_value_hook))) {
		debugPrintf("rain skip: NOT installed (Variable_SetValue hook)\n");
		return;
	}
	if (verify) {
		size_t total = 0;
		for (int i = 0; i < yoyoloader_mod.n_data && seg_count < 4; i++) {
			seg_base[seg_count] = (uint32_t *)yoyoloader_mod.data_base[i];
			seg_words[seg_count] = yoyoloader_mod.data_size[i] / 4;
			total += seg_words[seg_count++];
		}
		seg_copy = malloc(total * 4);
		debugPrintf("rain skip: verify mode%s (diff copy %u KB %s)\n", fault ? " with faults" : "", (unsigned)(total / 256),
			seg_copy ? "ok" : "FAILED");
	}
	for (int i = 0; *functions && i < *count; i++) {
		runner_function *f = &(*functions)[i];
		if (f->name && !strcmp(f->name, "instance_create_depth")) {
			orig_create_depth = f->func;
			f->func = create_depth_hook;
			break;
		}
	}
	debugPrintf("rain skip: %s (obj_rain %d, rain=%d)\n", orig_create_depth ? "installed" : "NOT installed", rain_object,
		rain_level);
}
