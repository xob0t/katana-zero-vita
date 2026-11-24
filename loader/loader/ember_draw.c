/*
 * ember_draw.c
 *
 * Burning corpses throw embers (obj_corpsefire, object 153): a corpse on fire creates one to three of them on every
 * tick while it flies and in slowing bursts once it lies still, each with parent = the corpse's id. obj_drawactors
 * draws them with their corpse, and the player's with the player (APK 1.1.10, gml_Object_obj_drawactors_Draw_0):
 *
 *     with (obj_enemycorpse) {                        // every corpse in the room, burning or not, on screen or not
 *         if (draw_layer == 0) {
 *             ...
 *             with (obj_corpsefire)                   // GML line 168, iterator calls at +0x23c4 / +0x24ac
 *                 if (parent == other.id) event_user(15);
 *         }
 *     }
 *     ...
 *     with (obj_player) if (draw_layer == 0) {
 *         ...
 *         with (obj_corpsefire)                       // GML line 644, iterator calls at +0x9354 / +0x943c
 *             if (parent == other.id) event_user(15);
 *     }
 *
 * so every corpse walks every ember in GML: in a haunted_alien2 fight with ~130 embers and ~40 corpses that is ~5,000
 * iterations per frame (7.3 ms on the Vita), and for every loop the runner first allocates and fills a list of all
 * the embers (YYGML_NewWithIteratorEx).
 *
 * Both loops keep their original code; only their iterator calls (YYGML_NewWithIterator / YYGML_WithIteratorNext) go
 * through veneers in the code cave to wrappers (as in bike_draw.c):
 * - The first ember loop of a frame runs the runner's iterator, steps over the embers whose parent is certainly not
 *   other.id and records every ember it visits, in order, with its parent value (the frame's ember list, below).
 * - The other loops of the frame, while that list is current, visit only the recorded embers whose parent might
 *   equal their other.id and that are still active, in the recorded order, without the runner's list.
 * For an ember stepped over, the original body only read parent and other.id and compared them: no write, no call
 * with side effects (the temporaries it fills are the event's scratch RValues: the id it reads, never refcounted, which
 * nothing reads before writing them again; the epilogue frees only refcounted kinds). Embers whose parent might equal
 * other.id, or whose values aren't plain reals or references, still run the original body, which makes the real
 * comparison and calls event_user(15) itself. So the embers drawn, their order and every variable stay as before.
 *
 * exp_emberdraw0 keeps the original. Debug mode, [EMBERDRAW] report line:
 * - exp_emberdrawverify: the recording loops step over nothing and count, for every ember they would have stepped
 *   over, whether the runner's YYCompareVal says equal; every loop served from the list also runs the runner's own
 *   iterator and compares the embers whose parent equals other.id, in order, with the list's. Mismatches must stay 0.
 * - exp_emberdrawab: the wrappers pass everything through in every other report window (in-run A/B).
 * - the time from each loop's NewWithIterator to its last iterator call is reported per frame.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#include "main.h"
#include "so_util.h"

extern int debugMode;

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef int ABI (*new_fn)(void *iterator, void **self, void **other, const void *target);
typedef int ABI (*next_fn)(void *iterator, void **self, void **other);
typedef RV *ABI (*var_fn)(void *instance, int id);
typedef void ABI (*getvalue_fn)(const RV *instance, int id, int index, RV *out, int, int);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double epsilon, int throw_error);

static new_fn new_with;
static next_fn next_with;
static getvalue_fn variable_get;
static compare_fn compare;
static const double *epsilon;
static const int *var_parent, *var_id; // {name, id}: ids are filled at game start, read them live
static int verify;

int ember_draw_ab, ember_draw_ab_off;
unsigned ember_draw_loops, ember_draw_kept, ember_draw_skipped, ember_draw_verified, ember_draw_mismatch, ember_draw_odd;
uint32_t ember_draw_us;

#define KIND(v) ((v)->kind & 0xffffff)

// The other.id the loop compares against, read once per loop (its other, the corpse or the player, is the same
// instance for the whole loop)
static void *id_other;
static RV id_value;

static const RV *other_id(void *other) {
	if (other != id_other) {
		RV inst = {0, 0, 6}, out = {0, 0, 0xffffff};
		memcpy(&inst.value, &other, sizeof(other));
		variable_get(&inst, var_id[1], (int)0x80000000, &out, 0, 0);
		id_value = out; // a real or a reference: nothing to free; anything else is never stepped over
		id_other = other;
	}
	return &id_value;
}

// 1 if parent == id is certainly false (YYCompareVal would not return 0)
static int differs(const RV *parent, const RV *id) {
	uint32_t pk = KIND(parent), ik = KIND(id);
	if (pk == 15 && ik == 15) {
		uint32_t pw[2], iw[2];
		memcpy(pw, &parent->value, 8);
		memcpy(iw, &id->value, 8);
		return pw[1] == iw[1] && pw[0] != iw[0]; // same reference type, different index
	}
	if (pk == 0 && ik == 0) // instance ids are whole numbers: half a unit apart is far beyond the epsilon (< 0.5)
		return *epsilon >= 0 && *epsilon < 0.5 && fabs(parent->value - id->value) > 0.5;
	if (pk == 0 && ik == 15)
		return parent->value < 0; // noone / -1 against an instance reference (whose index is never negative)
	ember_draw_odd++;
	return 0;
}

static RV *parent_of(void *ember) {
	return var_parent[1] < 0 ? NULL : (*(var_fn *)(*(uint8_t **)ember + 8))(ember, var_parent[1]);
}

// ---- The frame's ember list ----
// The first ember loop of a frame runs the runner's iterator and records the embers it visits, in that order, with
// their parent values. YYGML_NewWithIteratorEx builds its list from the object's instance list, the pending instance
// changes and the pending (de)activations, filtered by the active flags ((+0x7c & 0x100007) == 4), and leaves nothing
// behind (it resets the instance link fields it borrows). So while no instance has been created
// (CInstance::ms_CurrentCreateCounter) and the change and (de)activation arrays hold as many entries as when the list
// was recorded, in the same frame, the runner would visit the recorded embers again in the same order, minus any that
// stopped being active (destroyed or deactivated in between: their flags tell, and they are still allocated until the
// frame's cleanup). Nothing in the Draw event writes an ember's parent (obj_enemycorpse's Step sets it right after
// creating the ember), so the recorded parents hold too. Later loops of the frame then visit only the embers whose
// recorded parent might equal their other.id, without the runner's list (allocated and filled with every ember for
// every loop) and without walking it.
#define EMBER_MAX 1024
static struct {
	int valid;
	uint32_t frame, creates[2], changes, activations;
	int n;
	void *inst[EMBER_MAX];
	RV parent[EMBER_MAX]; // kind 0xffffff: no such variable (always visited)
} rec;
static int recording, rec_overflow;
static uint32_t snap_creates[2], snap_changes, snap_activations;
static uint32_t *create_counter, *change_array, *activation_array; // the arrays' count is word 2
extern uint32_t *frame_count;
unsigned ember_draw_cached_loops, ember_draw_recorded_loops, ember_draw_verify_loops;

static int rec_current(void) {
	return create_counter && rec.valid && rec.frame == *frame_count && rec.creates[0] == create_counter[0] &&
		rec.creates[1] == create_counter[1] && rec.changes == change_array[2] && rec.activations == activation_array[2];
}

static int active(void *inst) {
	return (*(uint32_t *)((uint8_t *)inst + 0x7c) & 0x100007) == 4;
}

// Our own iteration over a cached loop's candidates (the runner's list pointer stays 0: its WithIteratorNext would
// return 0 and DeleteWithIterator frees nothing, only puts self and other back)
static void *own_iterator;
static int own_n, own_pos;
static void *own_list[EMBER_MAX];

static uint32_t loop_start;

static void loop_done(void) {
	if (debugMode)
		ember_draw_us += sceKernelGetProcessTimeLow() - loop_start;
}

// The runner's iteration (recording, or the A/B off state): steps over embers certainly not other's
static int skip(int r, void *iterator, void **self, void **other) {
	int off = ember_draw_ab && ember_draw_ab_off;
	while (r > 0) {
		RV *parent = *self ? parent_of(*self) : NULL;
		if (recording) {
			if (rec.n < EMBER_MAX) {
				rec.inst[rec.n] = *self;
				if (parent)
					rec.parent[rec.n] = *parent;
				else
					rec.parent[rec.n].kind = 0xffffff;
				rec.n++;
			} else {
				rec_overflow = 1;
			}
		}
		if (off || !parent || !*other || var_id[1] < 0 || !differs(parent, other_id(*other)))
			break;
		if (verify) {
			if (compare(parent, other_id(*other), *epsilon, 0) == 0)
				ember_draw_mismatch++;
			else
				ember_draw_verified++;
			break;
		}
		ember_draw_skipped++;
		r = next_with(iterator, self, other);
	}
	if (r > 0) {
		ember_draw_kept++;
		return r;
	}
	if (recording) {
		recording = 0;
		if (!rec_overflow) {
			rec.valid = 1;
			rec.frame = *frame_count;
			rec.creates[0] = snap_creates[0];
			rec.creates[1] = snap_creates[1];
			rec.changes = snap_changes;
			rec.activations = snap_activations;
		}
	}
	loop_done();
	return r;
}

typedef void ABI (*delete_fn)(void *iterator, void **self, void **other);
static delete_fn delete_with;

// Verify mode: the embers the runner's own loop would draw for this caller (parent compares equal to other.id), in
// order, against the cached candidates that compare equal
static void verify_loop(const void *target, void *caller) {
	uint32_t it[4];
	void *s = caller, *o = NULL;
	int r = new_with(it, &s, &o, target);
	const RV *id = other_id(caller);
	int k = 0, ok = 1;
	while (r > 0) {
		RV *parent = parent_of(s);
		if (parent && compare(parent, id, *epsilon, 0) == 0) {
			while (k < own_n && !(parent_of(own_list[k]) && compare(parent_of(own_list[k]), id, *epsilon, 0) == 0))
				k++;
			if (k >= own_n || own_list[k] != s)
				ok = 0;
			else
				k++;
		}
		r = next_with(it, &s, &o);
	}
	for (; k < own_n; k++)
		if (parent_of(own_list[k]) && compare(parent_of(own_list[k]), id, *epsilon, 0) == 0)
			ok = 0;
	delete_with(it, &s, &o);
	ember_draw_verify_loops++;
	if (ok)
		ember_draw_verified++;
	else if (ember_draw_mismatch++ < 8)
		debugPrintf("[EMBERDRAW] mismatch: a cached loop's embers differ from the runner's\n");
}

static int ABI ember_new_with(void *iterator, void **self, void **other, const void *target) {
	if (debugMode)
		loop_start = sceKernelGetProcessTimeLow();
	ember_draw_loops++;
	id_other = NULL; // the id is read again for each loop
	own_iterator = NULL;
	int off = ember_draw_ab && ember_draw_ab_off;
	if (!off && var_id[1] >= 0 && rec_current()) {
		// As YYGML_NewWithIterator starts: the saved self and other, no list; then other = the caller and self = the
		// first ember visited
		void *caller = *self;
		uint32_t *it = iterator;
		it[0] = (uint32_t)caller;
		it[1] = (uint32_t)*other;
		it[2] = 0;
		it[3] = 0;
		const RV *id = other_id(caller);
		own_n = 0;
		for (int k = 0; k < rec.n; k++) {
			if (KIND(&rec.parent[k]) != 0xffffff && differs(&rec.parent[k], id)) {
				ember_draw_skipped++;
				continue;
			}
			if (active(rec.inst[k]))
				own_list[own_n++] = rec.inst[k];
		}
		ember_draw_cached_loops++;
		if (verify)
			verify_loop(target, caller);
		if (!own_n) {
			loop_done();
			return 0;
		}
		own_iterator = iterator;
		own_pos = 1;
		*other = caller;
		*self = own_list[0];
		ember_draw_kept++;
		return 1;
	}
	recording = 0;
	if (!off && create_counter) {
		recording = 1;
		rec_overflow = 0;
		rec.valid = 0;
		rec.n = 0;
		snap_creates[0] = create_counter[0];
		snap_creates[1] = create_counter[1];
		snap_changes = change_array[2];
		snap_activations = activation_array[2];
		ember_draw_recorded_loops++;
	}
	return skip(new_with(iterator, self, other, target), iterator, self, other);
}

static int ABI ember_next_with(void *iterator, void **self, void **other) {
	if (own_iterator && iterator == own_iterator) {
		if (own_pos < own_n) {
			*self = own_list[own_pos++];
			ember_draw_kept++;
			return 1;
		}
		own_iterator = NULL;
		loop_done();
		return 0;
	}
	return skip(next_with(iterator, self, other), iterator, self, other);
}

// bl from site can reach target (A32: PC + 8, +-32 MB)
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

int ember_draw_install(void) {
	// ELF addresses in APK 1.1.10: the event, the two with targets (mov r0, #0x99 = 153, obj_corpsefire), their
	// iterator call sites and the PLT entries
	enum { EVENT = 0x3ccf400, CORPSE_TARGET = 0x3cd17a4, CORPSE_NEW = 0x3cd17c4, CORPSE_NEXT = 0x3cd18ac,
	       PLAYER_TARGET = 0x3cd8734, PLAYER_NEW = 0x3cd8754, PLAYER_NEXT = 0x3cd883c,
	       PLT_NEW = 0x4ab0e00, PLT_NEXT = 0x4ab0e30 };
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z32gml_Object_obj_drawactors_Draw_0P9CInstanceS0_");
	new_with = (new_fn)so_symbol(&yoyoloader_mod, "_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	next_with = (next_fn)so_symbol(&yoyoloader_mod, "_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
	variable_get = (getvalue_fn)so_symbol(&yoyoloader_mod, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	compare = (compare_fn)so_symbol(&yoyoloader_mod, "_Z12YYCompareValRK6RValueS1_db");
	epsilon = (const double *)so_symbol(&yoyoloader_mod, "g_GMLMathEpsilon");
	var_parent = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_parent");
	var_id = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_id");
	delete_with = (delete_fn)so_symbol(&yoyoloader_mod, "_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_");
	create_counter = (uint32_t *)so_symbol(&yoyoloader_mod, "_ZN9CInstance23ms_CurrentCreateCounterE");
	change_array = (uint32_t *)so_symbol(&yoyoloader_mod, "g_InstanceChangeArray");
	activation_array = (uint32_t *)so_symbol(&yoyoloader_mod, "g_InstanceActivateDeactive");
	if (!change_array || !activation_array || !frame_count)
		create_counter = NULL; // no ember list: every loop steps over the other corpses' embers itself
	if (!entry || !delete_with || !new_with || !next_with || !variable_get || !compare || !epsilon || !var_parent || !var_id)
		return 0;
	// The prologue may be relocated by a profiling entry hook; the body must be the 1.1.10 one
	uint32_t *w = (uint32_t *)entry;
#define AT(a) (entry + (a) - EVENT)
	if (!((w[0] == 0xe92d4ff0 && w[1] == 0xe28db01c) || w[0] == 0xe51ff004) ||
			*(uint32_t *)AT(CORPSE_TARGET) != 0xe3a00099 || *(uint32_t *)AT(PLAYER_TARGET) != 0xe3a00099)
		return 0;
	enum { NSITES = 4 };
	uintptr_t sites[NSITES] = {AT(CORPSE_NEW), AT(CORPSE_NEXT), AT(PLAYER_NEW), AT(PLAYER_NEXT)};
	for (int k = 0; k < NSITES; k++) {
		int is_new = !(k & 1);
		if (!check_call(sites[k], is_new ? AT(PLT_NEW) : AT(PLT_NEXT), is_new ? (uintptr_t)new_with : (uintptr_t)next_with))
			return 0;
	}
	// One block for the four veneers (the sites lie within 29 KB of each other), every displacement checked before
	// any patch, so a failure leaves no site patched
	uint32_t *block = (uint32_t *)so_alloc_near(&yoyoloader_mod, sites[0], 8 * NSITES), *veneers[NSITES];
	if (!block)
		return 0;
	for (int k = 0; k < NSITES; k++)
		if (!bl_reaches(sites[k], (uintptr_t)(veneers[k] = block + 2 * k)))
			return 0;
#undef AT
	for (int k = 0; k < NSITES; k++)
		patch_call(sites[k], k & 1 ? (uintptr_t)&ember_next_with : (uintptr_t)&ember_new_with, veneers[k]);
	if (debugMode) {
		verify = exp_flag("emberdrawverify");
		ember_draw_ab = exp_flag("emberdrawab");
	}
	debugPrintf("ember draw: the corpse and player ember loops step over other corpses' embers%s%s\n",
		verify ? " (verify mode)" : "", ember_draw_ab ? " (A/B per report window)" : "");
	return 1;
}
