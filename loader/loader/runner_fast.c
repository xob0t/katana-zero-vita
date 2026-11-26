/*
 * runner_fast.c: exact fast paths for the runner's per-instance work (APK 1.1.10 runner, ELF addresses below).
 *
 * The Nov 25 frame audit put the runner's own per-instance overhead at ~2 ms/frame in the heavy rooms: event dispatch
 * (HandleStep, ExecuteDrawEvent, DrawLayerInstanceElement: ~1.2 ms) and Instance_Animate + HandleInstanceMovement
 * (~0.8 ms, ~430 instances/frame). Three fast paths, each with its own switch:
 *
 * 1. Dispatch (exp_rfdispatch0 off). Every Perform_Event(inst, inst, type, number) goes Perform_Event -> the object
 *    hash -> Perform_Event_Object_ASync -> CObjectGM::GetEventRecursive (a hashed probe of the object's event map)
 *    -> CEvent::Execute -> Code_Execute -> ExecuteIt -> the compiled event. The hot call sites (HandleStep's loop,
 *    DrawLayerInstanceElement, ExecuteDrawEvent's two instance paths, HandleInstanceMovement's path end, Instance_Animate's
 *    animation end) are redirected through veneers to fast_perform, which keeps Perform_Event's checks, takes the
 *    CEvent from a cache keyed on (object_index, type, number) and runs ASync's and CEvent::Execute's bookkeeping
 *    inline (event depth counter at 0x4e5f520 with its limit of 32, Current_Object/Type/Number saved, set and restored,
 *    Argument_Relative cleared and restored, Code_Error_Occured cleared) before it calls the compiled event the way
 *    ExecuteIt does for compiled code (code kind 1 or 2 with a function entry at +0x64: f(self, other, &result, 0, 0)
 *    when +0x6c is 1, else f(self, other, +0x6c, &result), as ExecuteIt's two calls). Other code (the VM, constants)
 *    goes through the runner's CEvent::Execute. A pending room change (New_Room != -1), a non-instance,
 *    the cleanup event (12), numbers outside 0..255 and a full depth counter take the runner's Perform_Event. The
 *    same sites' CObjectGM::HasEventRecursive calls answer from a cache keyed on (object, type, number). Objects' event
 *    maps are written only while the game loads (CHashMap<.., CEvent*>::Insert and DeleteCheckKey are called from
 *    Object_Load, PatchParents and the CObjectGM helpers they call; this runner has no object_event_add); both caches
 *    are still cleared when the room changes. The A/B "off" state still enters fast_perform, which calls Perform_Event. GML exceptions are caught where they are thrown (the loader's __cxa_throw
 *    hook), so skipping Perform_Event's catch frame changes nothing.
 * 2. Movement (exp_rfmove0 off). HandleInstanceMovement runs Adapt_Speed, Adapt_Path, a path-end event and
 *    SetPosition(x + hspeed, y + vspeed) on every active instance. With friction and gravity 0 Adapt_Speed returns at
 *    once, without a path Adapt_Path returns 0, and SetPosition stores nothing when the new position compares equal and
 *    the instance has no skeleton (CInstance::SkeletonAnimation would only create or delete one): such instances are
 *    skipped, and one that moves gets only its SetPosition. Anything else runs the original sequence. A physics world
 *    or the runner's profiler runs the original function.
 * 3. Animation (exp_rfanim0 off). Instance_Animate natively for an instance without a skeleton whose sprite isn't a
 *    sequence or skeleton sprite: the same float operations in the same order (vmla = multiply, round, add), GetFPS's
 *    cached value read directly (the runner's GetFPS when there is none), the wrap into [0, image_number) with the
 *    animation end event. Sprite lookups are Sprite_Data's (checked against it for every sprite at the first call).
 *
 * Switches: exp_runnerfast0 (all off), exp_rfdispatch0, exp_rfmove0, exp_rfanim0. exp_runnerfastverify: the caches
 * are compared with fresh lookups on every call, skipped movement runs the original calls and is compared byte for
 * byte, and the animation is computed natively and then run by the original and compared; [RUNNERFAST] verify counts
 * must stay 0 mismatches. exp_rfab (all three) or exp_rfabdispatch / exp_rfabmove / exp_rfabanim: in-run A/B, the fast
 * paths off in every other report window. exp_rfprof: time spent in HandleInstanceMovement and Instance_Animate per
 * window. Report line [RUNNERFAST] (debugMode).
 */
#include <stdint.h>
#include <string.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#include "main.h"
#include "so_util.h"
#include "hooks.h"

#define ABI __attribute__((pcs("aapcs")))
#define I32(p, o) (*(int32_t *)((char *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((char *)(p) + (o)))
#define PTR(p, o) (*(char **)((char *)(p) + (o)))
#define F32(p, o) (*(float *)((char *)(p) + (o)))

typedef struct { double v; uint32_t flags, kind; } RV;
typedef void ABI (*perform_fn)(void *self, void *other, int type, int number);
typedef void *ABI (*get_event_fn)(void *object, int type, int number);
typedef int ABI (*has_event_fn)(void *object, int type, int number);
typedef void ABI (*execute_fn)(void *event, void *self, void *other);
typedef void ABI (*code_fn)(void *self, void *other, void *result, int a, int b);
typedef void ABI (*code2_fn)(void *self, void *other, int32_t form, void *result);
typedef void ABI (*inst_fn)(void *inst);
typedef int ABI (*inst_int_fn)(void *inst);
typedef void ABI (*setpos_fn)(void *inst, float x, float y);
typedef int ABI (*move_fn)(int arg);
typedef double ABI (*fps_fn)(void *timer);
typedef void *ABI (*sprite_fn)(int index);

// ELF addresses in APK 1.1.10
enum {
	A_HANDLE_STEP = 0xf74e5c,
	A_DEPTH = 0x4e5f520,
	VENEER_PERFORM = 0x19f1160,   // linker veneers the original call sites branch to (patch_plt_calls.py makes most
	VENEER_HAS_EVENT = 0x19fb770, // of them direct calls)
};

static perform_fn orig_perform;
static get_event_fn get_event;
static has_event_fn orig_has;
static execute_fn event_execute;
static inst_fn adapt_speed;
static inst_int_fn adapt_path;
static setpos_fn set_position;
static fps_fn get_fps_fn;
static sprite_fn sprite_data_fn, sprite_exists_fn;
static inst_fn orig_animate;
static move_fn orig_move;

static void **object_hash, **run_room;
static int *new_room, *cur_object, *cur_type, *cur_number, *depth, *n_sprites;
static uint8_t *argument_relative, *code_error, *profile_flag;
static char *game_timer, *sprite_items;
static int64_t *create_counter;

int rf_verify, rf_ab_dispatch, rf_ab_move, rf_ab_anim, rf_prof;
static uint32_t rf_move_us, rf_anim_us, rf_move_frames, rf_anim_calls;
static int disp_on, move_on, anim_on;     // installed
int runner_fast_anim_on;                   // combat30.c: the native animation runs twice at 30 ticks/s
static volatile int disp_off, move_off, anim_off; // A/B windows
static int anim_checked;                  // sprite lookup self-check: 0 not yet, 1 passed, -1 failed

unsigned rf_disp_fast, rf_disp_none, rf_disp_slow, rf_disp_fallback, rf_has_hit, rf_has_miss;
unsigned rf_move_skip, rf_move_setpos, rf_move_full, rf_move_orig;
unsigned rf_anim_native, rf_anim_fallback;
unsigned rf_v_event, rf_v_event_bad, rf_v_has, rf_v_has_bad, rf_v_move, rf_v_move_bad, rf_v_anim, rf_v_anim_bad,
	rf_v_anim_skipped;
static unsigned rf_v_first_bad_line;

// --- caches ---

#define EV_SLOTS 4096
#define HAS_SLOTS 4096
typedef struct { uint32_t key; void *event; } ev_slot;
typedef struct { void *object; uint32_t tn; int has; } has_slot;
static ev_slot ev_cache[EV_SLOTS];
static has_slot has_cache[HAS_SLOTS];
static void *cache_room;

static inline void cache_check_room(void) {
	void *room = *run_room;
	if (room != cache_room) {
		memset(ev_cache, 0, sizeof(ev_cache));
		memset(has_cache, 0, sizeof(has_cache));
		cache_room = room;
	}
}

// The runner's g_ObjectHash lookup (HandleStep, Perform_Event_Object_ASync): buckets of 8 bytes, chained nodes
// {?, next, key, value}
static void *object_by_index(int index) {
	const char *hash = *(const char **)object_hash;
	if (!hash)
		return NULL;
	const char *buckets = PTR(hash, 0);
	uint32_t mask = U32(hash, 4);
	for (const char *node = PTR(buckets, 8 * ((uint32_t)index & mask)); node; node = PTR(node, 4))
		if (I32(node, 8) == index)
			return PTR(node, 0xc);
	return NULL;
}

// ASync's lookup: the object's CEvent for (type, number), NULL when the object or the event doesn't exist
static inline void *event_fresh(int index, int type, int number) {
	void *object = object_by_index(index);
	return object ? get_event(object, type, number) : NULL;
}

static inline void *event_lookup(int index, int type, int number) {
	uint32_t key = ((uint32_t)(index + 1) << 12) | ((uint32_t)type << 8) | (uint32_t)number;
	ev_slot *s = &ev_cache[(key * 2654435761u) >> 20];
	if (s->key != key) {
		s->event = event_fresh(index, type, number);
		s->key = key;
	} else if (rf_verify) {
		rf_v_event++;
		if (s->event != event_fresh(index, type, number)) {
			rf_v_event_bad++;
			if (!rf_v_first_bad_line)
				rf_v_first_bad_line = __LINE__;
		}
	}
	return s->event;
}

// --- 1. dispatch ---

static void ABI fast_perform(char *self, char *other, int type, int number) {
	if (disp_off) {
		orig_perform(self, other, type, number);
		return;
	}
	if (!self)
		return;
	cache_check_room();
	int index = I32(self, 0x84);
	if (I32(self, 0x50) != 1 || !*run_room || *new_room != -1 || type == 12 || (unsigned)type > 15 ||
			(unsigned)number > 255 || (unsigned)index >= (1u << 19)) {
		rf_disp_fallback++;
		orig_perform(self, other, type, number);
		return;
	}
	void **event = event_lookup(index, type, number);
	if (!event) {
		rf_disp_none++;
		return;
	}
	int d = *depth;
	if (d >= 0x20) {
		rf_disp_fallback++;
		orig_perform(self, other, type, number);
		return;
	}
	*depth = d + 1;
	int saved_object = *cur_object, saved_type = *cur_type, saved_number = *cur_number;
	*cur_type = type;
	*cur_object = (int)(intptr_t)event[1];
	*cur_number = number;
	char *code = event[0];
	if (code) {
		char *entry = PTR(code, 0x64);
		if (U32(code, 8) - 1 < 2 && entry) {
			// ExecuteIt's two call forms for compiled code, with the same arguments (compiled events read only
			// self and other)
			rf_disp_fast++;
			uint8_t relative = *argument_relative;
			*argument_relative = 0;
			*code_error = 0;
			RV result = {0, 0, 0};
			int32_t form = I32(code, 0x6c);
			if (form == 1)
				((code_fn)PTR(entry, 4))(self, other, &result, 0, 0);
			else
				((code2_fn)PTR(entry, 4))(self, other, form, &result);
			*argument_relative = relative;
		} else {
			rf_disp_slow++;
			event_execute(event, self, other);
		}
	}
	*cur_type = saved_type;
	*cur_object = saved_object;
	*cur_number = saved_number;
	*depth = *depth - 1;
}

static int ABI fast_has(void *object, int type, int number) {
	if (disp_off || !object || (unsigned)type > 15 || (unsigned)number > 255)
		return orig_has(object, type, number);
	cache_check_room();
	uint32_t tn = ((uint32_t)type << 8) | (uint32_t)number;
	uint32_t h = (((uint32_t)(uintptr_t)object >> 3) ^ (tn * 0x9e3779b1u)) * 2654435761u;
	has_slot *s = &has_cache[h >> 20];
	if (s->object != object || s->tn != tn) {
		rf_has_miss++;
		s->has = orig_has(object, type, number);
		s->object = object;
		s->tn = tn;
	} else {
		rf_has_hit++;
		if (rf_verify) {
			rf_v_has++;
			if (s->has != orig_has(object, type, number)) {
				rf_v_has_bad++;
				if (!rf_v_first_bad_line)
					rf_v_first_bad_line = __LINE__;
			}
		}
	}
	return s->has;
}

static void perform(char *inst, int type, int number) {
	if (disp_on)
		fast_perform(inst, inst, type, number);
	else
		orig_perform(inst, inst, type, number);
}

// --- sprites ---

static inline char *sprite_data(int index) {
	if (index < 0 || index >= *n_sprites)
		return NULL;
	return (*(char ***)(sprite_items + 4))[index];
}

static int sprite_check(void) {
	int n = *n_sprites;
	for (int i = -2; i < n + 2; i++)
		if (sprite_data(i) != sprite_data_fn(i) || (sprite_data(i) != NULL) != (sprite_exists_fn(i) != 0))
			return -1;
	return 1;
}

// CInstance::SkeletonAnimation returns 0 without side effects: no skeleton instance and no skeleton sprite
static inline int no_skeleton(char *inst) {
	if (PTR(inst, 0x70))
		return 0;
	char *sprite = sprite_data(I32(inst, 0x88));
	return !sprite || I32(sprite, 0x84) != 2;
}

// --- 2. movement ---

// HandleInstanceMovement's "created before this pass" test (subs/sbcs/blt: a signed 64-bit compare)
static inline int created_before(char *inst, int64_t before) {
	return before >= *(int64_t *)(inst + 0x60);
}

static int fast_move_body(int arg) {
	if (move_off || *profile_flag) {
		rf_move_orig++;
		return orig_move(arg);
	}
	char *room = *run_room;
	if (!room || PTR(room, 0x94)) {
		rf_move_orig++;
		return orig_move(arg);
	}
	int64_t before = *create_counter;
	*create_counter = before + 1;
	char *inst = PTR(room, 0x5c);
	while (inst) {
		char *next = PTR(inst, 0x154);
		if (F32(inst, 0xd8) == 0.0f && F32(inst, 0xe0) == 0.0f && !PTR(inst, 0x138)) {
			float x = F32(inst, 0xb8), y = F32(inst, 0xbc);
			float nx = x + F32(inst, 0xe4), ny = y + F32(inst, 0xe8);
			int skip = no_skeleton(inst) && x == nx && y == ny;
			if (rf_verify) {
				// The original calls, compared with what the fast path assumes they do
				static uint8_t snap[0x190];
				memcpy(snap, inst, sizeof(snap));
				adapt_speed(inst);
				int ended = adapt_path(inst);
				int same = !ended && !memcmp(snap, inst, sizeof(snap));
				set_position(inst, F32(inst, 0xb8) + F32(inst, 0xe4), F32(inst, 0xbc) + F32(inst, 0xe8));
				if (skip)
					same = same && !memcmp(snap, inst, sizeof(snap));
				rf_v_move++;
				if (!same) {
					rf_v_move_bad++;
					if (!rf_v_first_bad_line)
						rf_v_first_bad_line = __LINE__;
				}
			} else if (!skip) {
				set_position(inst, nx, ny);
			}
			if (skip)
				rf_move_skip++;
			else
				rf_move_setpos++;
		} else {
			rf_move_full++;
			adapt_speed(inst);
			if (adapt_path(inst) && created_before(inst, before))
				perform(inst, 7, 8);
			set_position(inst, F32(inst, 0xb8) + F32(inst, 0xe4), F32(inst, 0xbc) + F32(inst, 0xe8));
		}
		inst = next;
	}
	return 0;
}

// --- 3. animation ---

typedef struct { float index, wrapped; int fire; } anim_out;

// GetFPS: the cached rate when there is one (read without the barriers: written on this thread), else the runner's
static inline double fps_now(void) {
	double v = *(volatile double *)(game_timer + 0x18);
	return v > 0.0 ? v : get_fps_fn(game_timer);
}

// Instance_Animate's result for an instance without a skeleton whose sprite is missing or a plain sprite. 0: the
// original must run. Nothing is written.
static int anim_compute(char *inst, anim_out *o) {
	if (I32(inst, 0x50) != 1 || PTR(inst, 0x70))
		return 0;
	char *sprite = sprite_data(I32(inst, 0x88));
	float index = F32(inst, 0x98);
	if (!sprite) {
		index = index + F32(inst, 0xa0);
	} else {
		if (PTR(sprite, 0x34) || I32(sprite, 0x84) == 2)
			return 0;
		if (I32(sprite, 0x80) == 1) {
			index = index + F32(inst, 0xa0) * F32(sprite, 0x7c);
		} else {
			float speed = F32(sprite, 0x7c);
			double fps = fps_now();
			if (fps != 0.0) {
				speed = speed / (float)fps;
				index = index + speed * F32(inst, 0xa0);
			}
		}
	}
	float frames = sprite ? (float)I32(sprite, 0x58) : 0.0f;
	float wrapped = F32(inst, 0x9c);
	o->fire = 0;
	if (index >= frames) {
		wrapped = frames + wrapped;
		index = index - frames;
		o->fire = 1;
	} else if (index < 0.0f) {
		wrapped = wrapped - frames;
		index = frames + index;
		o->fire = 1;
	}
	o->index = index;
	o->wrapped = wrapped;
	return 1;
}

static void fast_animate_body(char *inst) {
	if (anim_off || anim_checked < 0) {
		orig_animate(inst);
		return;
	}
	if (!anim_checked) {
		anim_checked = sprite_check();
		debugPrintf("[RUNNERFAST] sprite lookup check: %s\n", anim_checked > 0 ? "passed" : "FAILED, animation fast path off");
		if (anim_checked < 0) {
			orig_animate(inst);
			return;
		}
	}
	anim_out o;
	if (!anim_compute(inst, &o)) {
		rf_anim_fallback++;
		orig_animate(inst);
		return;
	}
	if (rf_verify) {
		// An animation end event could change what is compared: only compare when the object has none
		int event = o.fire && event_fresh(I32(inst, 0x84), 7, 7) != NULL;
		orig_animate(inst);
		if (event) {
			rf_v_anim_skipped++;
			return;
		}
		rf_v_anim++;
		if (memcmp(&o.index, inst + 0x98, 4) || memcmp(&o.wrapped, inst + 0x9c, 4)) {
			rf_v_anim_bad++;
			if (!rf_v_first_bad_line)
				rf_v_first_bad_line = __LINE__;
		}
		return;
	}
	rf_anim_native++;
	F32(inst, 0x98) = o.index;
	F32(inst, 0x9c) = o.wrapped;
	if (o.fire)
		perform(inst, 7, 7);
}

// exp_rfprof: the time spent in HandleInstanceMovement and Instance_Animate, fast or not (the A/B state)
static int ABI fast_move(int arg) {
	if (!rf_prof)
		return fast_move_body(arg);
	uint32_t t0 = sceKernelGetProcessTimeLow();
	int r = fast_move_body(arg);
	rf_move_us += sceKernelGetProcessTimeLow() - t0;
	rf_move_frames++;
	return r;
}

// At 30 ticks/s (combat30.c) the sprite animation runs twice per tick, as two 60 Hz ticks would; not again for an
// instance its animation end event destroyed or deactivated (flags +0x7c)
extern int c30_active, c30_anim;
#define C30_AGAIN(inst) (c30_active && c30_anim && !(U32(inst, 0x7c) & 3))
static void ABI fast_animate(char *inst) {
	if (!rf_prof) {
		fast_animate_body(inst);
		if (C30_AGAIN(inst))
			fast_animate_body(inst);
		return;
	}
	uint32_t t0 = sceKernelGetProcessTimeLow();
	fast_animate_body(inst);
	if (C30_AGAIN(inst))
		fast_animate_body(inst);
	rf_anim_us += sceKernelGetProcessTimeLow() - t0;
	rf_anim_calls++;
}

// --- install ---

typedef struct { const char *name; uint32_t elf, w0, w1; uintptr_t addr; } helper;
// The runner functions dispatch bypasses: they must be the 1.1.10 code and not hooked by anyone
static helper bypassed[] = {
	{"_Z13Perform_EventP9CInstanceS0_ii", 0xf363f8, 0xe92d4df0, 0xe28db018, 0},
	{"_Z26Perform_Event_Object_ASyncP9CInstanceS0_iii", 0xf361c8, 0xe92d4ff0, 0xe28db01c, 0},
	{"_ZN9CObjectGM17GetEventRecursiveEii", 0xd447e8, 0xe92d4ff0, 0xe28db01c, 0},
	{"_ZN9CObjectGM17HasEventRecursiveEii", 0xd448d4, 0xe92d4ff0, 0xe28db01c, 0},
	{"_ZN6CEvent7ExecuteEP9CInstanceS1_", 0xf35f44, 0xe92d4830, 0xe28db008, 0},
	{"_Z12Code_ExecuteP9CInstanceS0_P5CCodeP6RValuei", 0xeca178, 0xe92d4800, 0xe1a0b00d, 0},
	{"_Z9ExecuteItP9CInstanceS0_P5CCodeP6RValuei", 0xec9fcc, 0xe92d4bf0, 0xe28db018, 0},
};
#define N_BYPASSED (sizeof(bypassed) / sizeof(bypassed[0]))

static int bypassed_intact(uintptr_t bias) {
	for (unsigned i = 0; i < N_BYPASSED; i++) {
		helper *h = &bypassed[i];
		if (!h->addr)
			h->addr = so_symbol(&yoyoloader_mod, h->name);
		if (!h->addr || h->addr != bias + h->elf || U32(h->addr, 0) != h->w0 || U32(h->addr, 4) != h->w1)
			return 0;
		uintptr_t *got = so_got_slot(&yoyoloader_mod, h->name);
		if (got && *got != h->addr)
			return 0;
	}
	return 1;
}

// Points one bl/b at `site` (to the linker veneer or straight to `func`) at replacement through an 8-byte veneer
static int patch_call(uintptr_t bias, uint32_t site_elf, uint32_t veneer_elf, uintptr_t func, void *replacement) {
	uintptr_t site = bias + site_elf;
	uint32_t ins = U32(site, 0);
	if ((ins >> 24) != 0xeb && (ins >> 24) != 0xea)
		return 0;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	if (target != bias + veneer_elf && target != func)
		return 0;
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	if (!veneer)
		return 0;
	int64_t delta = (int64_t)(uintptr_t)veneer - (int64_t)site - 8;
	if (delta < -(1 << 25) || delta >= (1 << 25))
		return 0;
	uint32_t code[2] = {0xe51ff004, (uint32_t)replacement}; // ldr pc, [pc, #-4]; .word replacement
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t branch = (ins & 0xff000000) | (((uint32_t)delta >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &branch, 4);
	kuKernelFlushCaches((void *)site, 4);
	return 1;
}

#define SYM(name) so_symbol(&yoyoloader_mod, name)

void runner_fast_install(void) {
	if (exp_flag("runnerfast0")) {
		debugPrintf("runner fast paths: off (exp_runnerfast0)\n");
		return;
	}
	uintptr_t handle_step = SYM("_Z10HandleStepi");
	uintptr_t bias = handle_step - A_HANDLE_STEP;
	object_hash = (void **)SYM("g_ObjectHash");
	run_room = (void **)SYM("Run_Room");
	new_room = (int *)SYM("New_Room");
	cur_object = (int *)SYM("Current_Object");
	cur_type = (int *)SYM("Current_Event_Type");
	cur_number = (int *)SYM("Current_Event_Number");
	argument_relative = (uint8_t *)SYM("Argument_Relative");
	code_error = (uint8_t *)SYM("Code_Error_Occured");
	profile_flag = (uint8_t *)SYM("g_bProfile");
	game_timer = (char *)SYM("g_GameTimer");
	sprite_items = (char *)SYM("g_SpriteItems");
	n_sprites = (int *)SYM("g_NumberOfSprites");
	create_counter = (int64_t *)SYM("_ZN9CInstance23ms_CurrentCreateCounterE");
	orig_perform = (perform_fn)SYM("_Z13Perform_EventP9CInstanceS0_ii");
	get_event = (get_event_fn)SYM("_ZN9CObjectGM17GetEventRecursiveEii");
	orig_has = (has_event_fn)SYM("_ZN9CObjectGM17HasEventRecursiveEii");
	event_execute = (execute_fn)SYM("_ZN6CEvent7ExecuteEP9CInstanceS1_");
	adapt_speed = (inst_fn)SYM("_ZN9CInstance11Adapt_SpeedEv");
	adapt_path = (inst_int_fn)SYM("_ZN9CInstance10Adapt_PathEv");
	set_position = (setpos_fn)SYM("_ZN9CInstance11SetPositionEff");
	get_fps_fn = (fps_fn)SYM("_ZN13CTimingSource6GetFPSEv");
	sprite_data_fn = (sprite_fn)SYM("_Z11Sprite_Datai");
	sprite_exists_fn = (sprite_fn)SYM("_Z13Sprite_Existsi");
	uintptr_t animate = SYM("_Z16Instance_AnimateP9CInstance");
	uintptr_t move = SYM("_Z22HandleInstanceMovementb");
	if (!handle_step || !object_hash || !run_room || !new_room || !cur_object || !cur_type || !cur_number ||
			!argument_relative || !code_error || !profile_flag || !game_timer || !sprite_items || !n_sprites ||
			!create_counter || !orig_perform || !get_event || !orig_has || !event_execute || !adapt_speed ||
			!adapt_path || !set_position || !get_fps_fn || !sprite_data_fn || !sprite_exists_fn || !animate || !move ||
			animate != bias + 0xf7bd84 || move != bias + 0xf7ae28) {
		debugPrintf("runner fast paths: NOT installed (symbols)\n");
		return;
	}
	depth = (int *)(bias + A_DEPTH);
	rf_verify = exp_flag("runnerfastverify");
	rf_prof = exp_flag("rfprof");
	int ab = exp_flag("rfab");
	rf_ab_dispatch = ab || exp_flag("rfabdispatch");
	rf_ab_move = ab || exp_flag("rfabmove");
	rf_ab_anim = ab || exp_flag("rfabanim");

	if (!exp_flag("rfdispatch0")) {
		if (!bypassed_intact(bias)) {
			debugPrintf("runner fast paths: dispatch NOT installed (Perform_Event chain not the 1.1.10 code or hooked)\n");
		} else {
			static const struct { uint32_t site, plt; int has; } sites[] = {
				{0xf74fa4, VENEER_PERFORM, 0},    // HandleStep
				{0xef10c8, VENEER_HAS_EVENT, 1}, // DrawLayerInstanceElement
				{0xef10f0, VENEER_PERFORM, 0},
				{0xeeff5c, VENEER_HAS_EVENT, 1}, // ExecuteDrawEvent, layer loop
				{0xeeff78, VENEER_PERFORM, 0},
				{0xef0318, VENEER_HAS_EVENT, 1}, // ExecuteDrawEvent, single instance
				{0xef0374, VENEER_PERFORM, 0},
				{0xf7af88, VENEER_PERFORM, 0},   // HandleInstanceMovement, path end
				{0xf7bfc0, VENEER_PERFORM, 0},   // Instance_Animate, animation end (tail call)
			};
			int patched = 0;
			for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
				int ok = sites[i].has ? patch_call(bias, sites[i].site, sites[i].plt, (uintptr_t)orig_has, fast_has)
					: patch_call(bias, sites[i].site, sites[i].plt, (uintptr_t)orig_perform, fast_perform);
				if (ok)
					patched++;
				else
					debugPrintf("runner fast paths: call site 0x%08X not patched\n", sites[i].site);
			}
			disp_on = patched > 0;
			debugPrintf("runner fast paths: dispatch %s (%d of %u call sites)\n", disp_on ? "installed" : "NOT installed",
				patched, (unsigned)(sizeof(sites) / sizeof(sites[0])));
		}
	}
	if (!exp_flag("rfmove0")) {
		move_on = (orig_move = (move_fn)entry_hook(move, fast_move)) != NULL;
		debugPrintf("runner fast paths: movement %s\n", move_on ? "installed" : "NOT installed");
	}
	if (!exp_flag("rfanim0")) {
		anim_on = (orig_animate = (inst_fn)entry_hook(animate, fast_animate)) != NULL;
		runner_fast_anim_on = anim_on;
		debugPrintf("runner fast paths: animation %s\n", anim_on ? "installed" : "NOT installed");
	}
	if (rf_verify)
		debugPrintf("runner fast paths: verify on (exp_runnerfastverify)\n");
}

void runner_fast_report(int frames) {
	if ((!disp_on && !move_on && !anim_on) || frames <= 0)
		return;
	// A hook added on a bypassed helper since install (profilers): dispatch stays off from then on
	static unsigned generation;
	if (disp_on && generation != so_hook_generation) {
		generation = so_hook_generation;
		if (!bypassed_intact((uintptr_t)orig_perform - 0xf363f8)) {
			disp_on = 0;
			disp_off = 1;
			rf_ab_dispatch = 0;
			debugPrintf("[RUNNERFAST] dispatch off: a bypassed helper was hooked\n");
		}
	}
	float f = (float)frames;
	debugPrintf("[RUNNERFAST] dispatch: fast %.1f, runner %.1f, no event %.1f, fallback %.1f; has-event hit %.1f miss "
		"%.1f | move: skipped %.1f, setpos %.1f, full %.1f, original %.1f | anim: native %.1f, fallback %.1f (per frame) | "
		"A/B dispatch %s move %s anim %s\n",
		rf_disp_fast / f, rf_disp_slow / f, rf_disp_none / f, rf_disp_fallback / f, rf_has_hit / f, rf_has_miss / f,
		rf_move_skip / f, rf_move_setpos / f, rf_move_full / f, rf_move_orig / f, rf_anim_native / f,
		rf_anim_fallback / f,
		!disp_on ? "-" : disp_off ? "off" : "on", !move_on ? "-" : move_off ? "off" : "on",
		!anim_on ? "-" : anim_off ? "off" : "on");
	if (rf_verify)
		debugPrintf("[RUNNERFAST] verify: events %u (%u mismatches), has-event %u (%u), movement %u (%u), animation %u "
			"(%u; %u not compared: end event) (session)\n",
			rf_v_event, rf_v_event_bad, rf_v_has, rf_v_has_bad, rf_v_move, rf_v_move_bad, rf_v_anim, rf_v_anim_bad,
			rf_v_anim_skipped);
	if (rf_v_first_bad_line)
		debugPrintf("[RUNNERFAST] first mismatch: runner_fast.c:%u\n", rf_v_first_bad_line);
	rf_disp_fast = rf_disp_slow = rf_disp_none = rf_disp_fallback = rf_has_hit = rf_has_miss = 0;
	rf_move_skip = rf_move_setpos = rf_move_full = rf_move_orig = 0;
	rf_anim_native = rf_anim_fallback = 0;
	if (rf_prof) {
		debugPrintf("[RUNNERFAST] time: movement %.3f ms/frame, animation %.3f ms/frame (%.1f instances, %.3f us each)\n",
			rf_move_us / 1000.0f / f, rf_anim_us / 1000.0f / f, rf_anim_calls / f,
			rf_anim_calls ? (float)rf_anim_us / rf_anim_calls : 0.0f);
		rf_move_us = rf_anim_us = rf_move_frames = rf_anim_calls = 0;
	}
	if (rf_ab_dispatch)
		disp_off = !disp_off;
	if (rf_ab_move)
		move_off = !move_off;
	if (rf_ab_anim)
		anim_off = !anim_off;
}
