/*
 * fog_skip.c
 *
 * fogScale (katana.c) keeps one prison fog puff in N: the others are created by obj_fogspawner's Step and destroyed
 * at the start of their first Step. Every obj_fog is a CInstance, a GC object, so the spawner's ~1 creation per frame
 * drives the generational GC: in room_prison_3 a full old-generation pass (6-8 ms) about 1.3 times a second. The
 * rate follows how many puffs are created, not how many are kept.
 *
 * Here the steady-state spawn (gml_Object_obj_fogspawner_Step_0, GML line 0x83)
 *
 *     bb = instance_create_depth(x + random(32 * image_xscale), y + random(32 * image_yscale), depth, obj_fog);
 *     with (bb) { parent = other.id; alpha_mult = other.alpha; region = other.region; frozen = other.frozen;
 *                 if (other.big) sprite_index = ...; event_user(1); }
 *
 * of a puff that fogScale would discard doesn't create it. Everything that creation and the with block change outside
 * the puff itself is done instead:
 * - the instance id: room_maxid + 1 (the runner's global id counter), returned as the same instance reference, so the
 *   spawner's bb holds the id the puff would have had and with (bb) finds no instance, as it finds none once the
 *   discarded puff is destroyed;
 * - the layer element id (CLayerManager::m_CurrentElementID + 1): instance_create_depth puts the puff on the managed
 *   layer at that depth. The skip only happens when that layer already exists; otherwise the puff would create the
 *   layer (and take a layer id), so it is created and discarded as before;
 * - the random numbers: obj_fog Create draws two (dir = choose(..), frame = round(random(2))). Its other calls
 *   (draw_recolor_init, update_light_array_ext, animation_init, draw_sprite_recolored, instance_exists) and user
 *   event 1 only read globals and write the puff's own variables and arrays; the with block draws none.
 * obj_fog has no Destroy or Clean Up event and no parent. The x/y arguments' random() calls run in the spawner as
 * before. The spawner's one-time prefill (line 0x29, whose with block draws random numbers) still creates and
 * discards. Puffs are counted for the 1-in-N choice the same way in both paths (fog_created), so the same puffs are
 * kept.
 *
 * A discarded puff also exists until its first Step, which is always in the next frame. In that window the only
 * thing that sees it is the particle budget: obj_timeMaster's Step runs obj_opto_master user event 5 after the
 * spawner, and it counts 0.5 * instance_number(obj_fog) into global.particle_scaler. That one instance_number call
 * gets the puffs not created this frame added (patch_budget_call), so the scaler stays the same. (The spawner's own
 * with (obj_fog) when frozen changes only writes the puffs' frozen.)
 *
 * exp_fogskip0 keeps the create-and-destroy path. Debug builds report [FOGSKIP] with the profiler.
 * exp_fogskipverify (debug) creates every puff as before, but first runs the skip on the live state, records the
 * result, puts the state back, and after the puff's user event 1 (the with block's last statement) compares the
 * random state, the id and layer counters, the create counter and the returned id. At every budget call it compares
 * the number of checked puffs alive with the frame model above, and it reports with loops over obj_fog or all that
 * run while one exists. For the first samples it diffs the runner's whole .data/.bss after the creation against the
 * skip: what differs is allocator, GC, layer element pool and renderer bookkeeping (the Create draws a sprite).
 */
#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kubridge.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

#define SPAWN_LINE 0x83
#define SPAWNER_STEP "gml_Object_obj_fogspawner_Step_0"

extern int fog_scale;
extern unsigned fog_created;
unsigned fog_doomed_alive(void);

typedef struct stack_trace {
	struct stack_trace *prev;
	const char *name;
	int line;
} stack_trace;

static builtin_fn orig_create_depth;
static stack_trace **trace_top;
static void **run_room;
static int *room_maxid, *element_id, *layer_id;
static uint32_t *rng_state, *rng_index;
static uint32_t *create_counter; // CInstance::ms_CurrentCreateCounter (64-bit)
static void *(*layer_with_depth)(void *room, int depth, int dynamic_only);
static int (*get_int32)(gml_rvalue *args, int index);
static void (*random_step)(void);
static int fog_object = -1;
static int verify;

unsigned fog_skipped, fog_skip_fallback;

// Ghosts: puffs not created that would still exist. A discarded puff lives until its first Step, which comes in a
// later frame (g_IOFrameCount). Instances step newest first, so in that frame every fog puff (all newer than the
// spawner and obj_timeMaster) steps before the spawner and the budget: the first fog Step seen in a later frame ends
// the ghosts of earlier frames. While the game is paused (instance_deactivate_all) no puff steps and they live on,
// as the deactivated puff would come back with the instances.
static uint32_t ghost_frame;
static unsigned ghosts_now, ghosts_before; // created in ghost_frame / in earlier frames

static void ghost_add(void) {
	uint32_t now = *frame_count;
	if (now != ghost_frame) {
		ghosts_before += ghosts_now;
		ghosts_now = 0;
		ghost_frame = now;
	}
	ghosts_now++;
}

static unsigned ghosts_alive(void) {
	return ghosts_now + ghosts_before;
}

// Called by katana.c's fog Step hook for every puff, before anything else
void fog_skip_puff_step(void) {
	if (!ghosts_now && !ghosts_before)
		return;
	if (*frame_count != ghost_frame)
		ghosts_now = 0;
	ghosts_before = 0;
}

static int object_arg(const gml_rvalue *v) {
	if ((v->kind & 0xffffff) == 15) { // object reference: index in the low word
		uint32_t w[2];
		memcpy(w, &v->val, sizeof(w));
		return (int)w[0];
	}
	return v->kind == 0 ? (int)v->val : -1;
}

static int at_spawn_site(void) {
	stack_trace *t = *trace_top;
	return t && t->line == SPAWN_LINE && t->name && !strcmp(t->name, SPAWNER_STEP);
}

// What the skip changes; compared field by field in verify mode
typedef struct {
	uint32_t rng[16], rng_index;
	int maxid, element, layer;
	uint32_t create_lo, create_hi;
} fog_state;

static void state_read(fog_state *s) {
	memcpy(s->rng, rng_state, sizeof(s->rng));
	s->rng_index = *rng_index;
	s->maxid = *room_maxid;
	s->element = *element_id;
	s->layer = *layer_id;
	s->create_lo = create_counter[0];
	s->create_hi = create_counter[1];
}

static void state_write(const fog_state *s) {
	memcpy(rng_state, s->rng, sizeof(s->rng));
	*rng_index = s->rng_index;
	*room_maxid = s->maxid;
	*element_id = s->element;
	*layer_id = s->layer;
	create_counter[0] = s->create_lo;
	create_counter[1] = s->create_hi;
}

// The skip itself: returns the id the puff would have had
static int skip_puff(void) {
	int id = ++*room_maxid;
	++*element_id;
	random_step(); // Create: dir = choose(..)
	random_step(); // Create: frame = round(random(2))
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

static int pending_id = -1;
static int *event_type, *event_number;
static uint32_t last_doomed_frame;
static fog_state predicted;
static int diff_pending;
// Verified puffs (the ones the skip would not create) that still exist, with their creation frame
#define CHECKED_MAX 32
static struct { void *inst; uint32_t frame; } checked[CHECKED_MAX];
static unsigned checked_alive, checked_overflow;

unsigned fog_model_mismatch, fog_verified, fog_mismatch, fog_budget_calls, fog_budget_saw_doomed, fog_with_saw_doomed;
unsigned fog_first_step_same, fog_first_step_later, fog_first_step_later2;
static event_func orig_fog_user1, orig_opto_user5;
static int (*orig_new_with)(void *it, void **self, void **other, const gml_rvalue *target);

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
	int len = snprintf(line, sizeof(line), "[FOGSKIP] diff sample %d: orig changed %d words, skip %d; differing:",
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
			len = snprintf(line, sizeof(line), "[FOGSKIP]  ...");
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

static int instance_id(void *inst) {
	int id;
	memcpy(&id, (uint8_t *)inst + 0x80, sizeof(id));
	return id;
}

static void fog_user1_verify(void *self, void *other) {
	orig_fog_user1(self, other);
	if (pending_id < 0 || instance_id(self) != pending_id)
		return;
	fog_state now;
	state_read(&now);
	ghost_add();
	if (!memcmp(&now, &predicted, sizeof(now)))
		fog_verified++;
	else if (fog_mismatch++ < 8)
		debugPrintf("[FOGSKIP] mismatch id %d: maxid %d/%d element %d/%d layer %d/%d create %u/%u rng %s\n",
			pending_id, now.maxid, predicted.maxid, now.element, predicted.element, now.layer, predicted.layer,
			now.create_lo, predicted.create_lo,
			memcmp(now.rng, predicted.rng, sizeof(now.rng)) || now.rng_index != predicted.rng_index ? "differs" : "same");
	if (diff_pending) {
		diff_orig_n = seg_diff(diff_orig, DIFF_MAX);
		diff_pending = 0;
		diff_report();
		if (++diff_samples >= DIFF_SAMPLES) {
			free(seg_copy);
			seg_copy = NULL;
		}
	}
	pending_id = -1;
	last_doomed_frame = *frame_count;
	for (int i = 0; i < CHECKED_MAX; i++)
		if (!checked[i].inst) {
			checked[i].inst = self;
			checked[i].frame = *frame_count;
			checked_alive++;
			return;
		}
	checked_overflow++;
}


static void opto_user5_verify(void *self, void *other) {
	fog_budget_calls++;
	if (fog_doomed_alive())
		fog_budget_saw_doomed++;
	if (ghosts_alive() != checked_alive && fog_model_mismatch++ < 8)
		debugPrintf("[FOGSKIP] model mismatch: %u alive, model %u\n", checked_alive, ghosts_alive());
	static int logged;
	if (last_doomed_frame && logged < 16) {
		stack_trace *t = *trace_top;
		logged++;
		debugPrintf("[FOGSKIP] budget call: event %d/%d in %s line %d, doomed alive %u, frames since last doomed %u\n",
			event_type ? *event_type : -1, event_number ? *event_number : -1, t && t->name ? t->name : "?",
			t ? t->line : -1, fog_doomed_alive(), *frame_count - last_doomed_frame);
	}
	orig_opto_user5(self, other);
}

static int new_with_verify(void *it, void **self, void **other, const gml_rvalue *target) {
	if (fog_doomed_alive() && target) {
		int all = target->kind == 0 && target->val == -3.0;
		int fog = 0;
		if ((target->kind & 0xffffff) == 15) {
			uint32_t w[2];
			memcpy(w, &target->val, sizeof(w));
			fog = w[1] == 0x01000000 && (int)w[0] == fog_object;
		} else if (target->kind == 0)
			fog = (int)target->val == fog_object;
		if (all || fog) {
			stack_trace *t = *trace_top;
			if (fog_with_saw_doomed++ < 8)
				debugPrintf("[FOGSKIP] with (%s) while a discarded puff exists: %s line %d\n", all ? "all" : "obj_fog",
					t && t->name ? t->name : "?", t ? t->line : -1);
		}
	}
	return orig_new_with(it, self, other, target);
}

// Called by katana.c's fog Step hook when it destroys a discarded puff
void fog_skip_doomed_step(void *self) {
	for (int i = 0; verify && i < CHECKED_MAX; i++)
		if (checked[i].inst == self) {
			uint32_t now = *frame_count;
			if (now == checked[i].frame)
				fog_first_step_same++;
			else if (now == checked[i].frame + 1)
				fog_first_step_later++;
			else
				fog_first_step_later2++;
			checked[i].inst = NULL;
			checked_alive--;
			return;
		}
}

// ---- the particle budget ------------------------------------------------------------------------------------------
// obj_opto_master user event 5 adds 0.5 * instance_number(obj_fog) to the particle count behind global.particle_scaler.
// It runs while a discarded puff created this frame still exists (verify mode: the puff's first Step is always in the
// next frame), so that call (APK 1.1.10 ELF 0x456c218, the 9th instance_number of the event) gets the puffs not
// created this frame added.
static int (*instance_number)(void *self, void *other, int obj);

static int budget_fog_number(void *self, void *other, int obj) {
	int n = instance_number(self, other, obj);
	return obj == fog_object ? n + (int)ghosts_alive() : n;
}

static int patch_budget_call(void) {
	enum { EVENT = 0x456bd20, OBJ_ADD = 0x456c1e8, SITE = 0x456c218, PLT = 0x4ab1700 };
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z35gml_Object_obj_opto_master_Other_15P9CInstanceS0_");
	instance_number = (void *)so_symbol(&yoyoloader_mod, "_Z21YYGML_instance_numberP9CInstanceS0_i");
	if (!entry || !instance_number || *(uint32_t *)(entry + OBJ_ADD - EVENT) != 0xe2890058) // add r0, sb, #0x58
		return 0;
	uintptr_t site = entry + SITE - EVENT, plt = entry + PLT - EVENT;
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	if ((ins >> 24) != 0xeb || (target != plt && target != (uintptr_t)instance_number))
		return 0;
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	int32_t off = veneer ? (int32_t)((uintptr_t)veneer - (site + 8)) : 0;
	if (!veneer || off < -(1 << 25) || off >= (1 << 25))
		return 0;
	uint32_t code[2] = {0xe51ff004, (uint32_t)(uintptr_t)budget_fog_number}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
	return 1;
}

// ---- the hook ------------------------------------------------------------------------------------------------------

static void create_depth_hook(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (argc == 4 && fog_created % fog_scale && object_arg(&args[3]) == fog_object && at_spawn_site()) {
		if (!*run_room || !layer_with_depth(*run_room, get_int32(args, 2), 1)) {
			fog_skip_fallback++;
		} else if (!verify) {
			fog_created++;
			fog_skipped++;
			ghost_add();
			set_instance_ref(result, skip_puff());
			return;
		} else {
			fog_state before;
			state_read(&before);
			if (seg_copy && pending_id < 0) {
				seg_snapshot();
				diff_pending = 1;
			}
			int id = skip_puff();
			state_read(&predicted);
			if (diff_pending)
				diff_emu_n = seg_diff(diff_emu, DIFF_MAX);
			state_write(&before);
			orig_create_depth(result, self, other, argc, args); // fog_create counts it and marks it discarded
			uint32_t w[2];
			memcpy(w, &result->val, sizeof(w));
			if (result->kind != 15 || (int)w[0] != id || w[1] != 0x04000001) {
				if (fog_mismatch++ < 8)
					debugPrintf("[FOGSKIP] mismatch: returned kind %u %08x:%08x, skip id %d\n", result->kind, w[1], w[0], id);
				diff_pending = 0;
				return;
			}
			pending_id = id;
			return;
		}
	}
	orig_create_depth(result, self, other, argc, args);
}

void fog_skip_report(int frames) {
	if (!orig_create_depth)
		return;
	debugPrintf("[FOGSKIP] %s: %u puffs not created, %u fallbacks (no layer yet)\n",
		verify ? "verify" : "skip", fog_skipped, fog_skip_fallback);
	if (verify)
		debugPrintf("[FOGSKIP] verify: %u match, %u mismatch; checked puffs' first Step same frame %u, next %u, later %u "
			"(%u untracked); budget calls %u, seeing a discarded puff %u, ghost model mismatch %u; with loops seeing one %u\n",
			fog_verified, fog_mismatch, fog_first_step_same, fog_first_step_later, fog_first_step_later2, checked_overflow,
			fog_budget_calls, fog_budget_saw_doomed, fog_model_mismatch, fog_with_saw_doomed);
	(void)frames;
}

void fog_skip_install(void) {
	if (fog_scale <= 1 || exp_flag("fogskip0")) {
		debugPrintf("fog skip: off\n");
		return;
	}
#define FS_RESOLVE(var, sym) var = (void *)so_symbol(&yoyoloader_mod, sym)
	FS_RESOLVE(trace_top, "_ZN13SYYStackTrace8s_pStartE");
	FS_RESOLVE(run_room, "Run_Room");
	FS_RESOLVE(room_maxid, "room_maxid");
	FS_RESOLVE(element_id, "_ZN13CLayerManager18m_CurrentElementIDE");
	FS_RESOLVE(layer_id, "_ZN13CLayerManager16m_CurrentLayerIDE");
	FS_RESOLVE(create_counter, "_ZN9CInstance23ms_CurrentCreateCounterE");
	FS_RESOLVE(rng_state, "state");
	FS_RESOLVE(rng_index, "g_RndIndex");
	FS_RESOLVE(layer_with_depth, "_ZN13CLayerManager17GetLayerWithDepthEP5CRoomib");
	FS_RESOLVE(get_int32, "_Z10YYGetInt32PK6RValuei");
	FS_RESOLVE(random_step, "_Z8YYRandomv");
	int (*object_find)(const char *);
	FS_RESOLVE(object_find, "_Z11Object_FindPKc");
	runner_function **functions;
	int *count;
	FS_RESOLVE(functions, "the_functions");
	FS_RESOLVE(count, "the_numb");
#undef FS_RESOLVE
	if (!frame_count || !trace_top || !run_room || !room_maxid || !element_id || !layer_id || !create_counter || !rng_state ||
			!rng_index || !layer_with_depth || !get_int32 || !random_step || !object_find || !functions || !count) {
		debugPrintf("fog skip: NOT installed (symbols)\n");
		return;
	}
	fog_object = object_find("obj_fog");
	verify = debugMode && exp_flag("fogskipverify");
	event_type = (int *)so_symbol(&yoyoloader_mod, "Current_Event_Type");
	event_number = (int *)so_symbol(&yoyoloader_mod, "Current_Event_Number");
	if (verify) {
		orig_fog_user1 = entry_hook(so_symbol(&yoyoloader_mod, "_Z27gml_Object_obj_fog_Other_11P9CInstanceS0_"),
			fog_user1_verify);
		if (!orig_fog_user1) {
			debugPrintf("fog skip: verify NOT installed\n");
			verify = 0;
			return;
		}
		orig_opto_user5 = entry_hook(so_symbol(&yoyoloader_mod, "_Z35gml_Object_obj_opto_master_Other_15P9CInstanceS0_"),
			opto_user5_verify);
		orig_new_with = entry_hook(so_symbol(&yoyoloader_mod,
			"_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue"), new_with_verify);
		size_t total = 0;
		for (int i = 0; i < yoyoloader_mod.n_data && seg_count < 4; i++) {
			seg_base[seg_count] = (uint32_t *)yoyoloader_mod.data_base[i];
			seg_words[seg_count] = yoyoloader_mod.data_size[i] / 4;
			total += seg_words[seg_count++];
		}
		seg_copy = malloc(total * 4);
		debugPrintf("fog skip: verify mode (opto %s, with %s, diff copy %u KB %s)\n", orig_opto_user5 ? "hooked" : "NOT hooked",
			orig_new_with ? "hooked" : "NOT hooked", (unsigned)(total / 256), seg_copy ? "ok" : "FAILED");
	}
	// Without the budget correction the skip would change global.particle_scaler (verify mode checks the model instead)
	if (!verify && !patch_budget_call()) {
		debugPrintf("fog skip: NOT installed (particle budget call site)\n");
		return;
	}
	for (int i = 0; fog_object >= 0 && *functions && i < *count; i++) {
		runner_function *f = &(*functions)[i];
		if (f->name && !strcmp(f->name, "instance_create_depth")) {
			orig_create_depth = f->func;
			f->func = create_depth_hook;
			break;
		}
	}
	debugPrintf("fog skip: %s (obj_fog %d)\n", orig_create_depth ? "installed" : "NOT installed", fog_object);
}
