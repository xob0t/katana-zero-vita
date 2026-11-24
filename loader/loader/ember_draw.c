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
 * - The first ember loop of a Draw event runs the runner's iterator, steps over the embers whose parent is certainly
 *   not other.id and records every ember it visits, in order, with a pointer to its parent variable (the ember list,
 *   below).
 * - The later loops of the same event, while that list is current, visit the recorded embers that are still active,
 *   in the recorded order, without the runner's list, and step over those whose parent (read live, when the loop gets
 *   to them, as the original reads it) is certainly not other.id.
 * For an ember stepped over, the original body only read parent and other.id and compared them: no write, no call
 * with side effects (the temporaries it fills are the event's scratch RValues: the id it reads, never refcounted, which
 * nothing reads before writing them again; the epilogue frees only refcounted kinds). Embers whose parent might equal
 * other.id, or whose values aren't plain reals or references, still run the original body, which makes the real
 * comparison and calls event_user(15) itself. So the embers drawn, their order and every variable stay as before.
 *
 * exp_emberdraw0 keeps the original. Debug mode, [EMBERDRAW] report line:
 * - exp_emberdrawverify: the recording loops step over nothing and check, for every ember they would have stepped
 *   over, that the runner's YYCompareVal says not equal ("step-overs"). Every loop served from the list also runs the
 *   runner's own iterator first and compares, in order, all the embers it visits with the list's ("lists") and those
 *   whose parent equals other.id with the list's ("drawn"). Mismatches must stay 0.
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
#include "katana_internal.h"

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
unsigned ember_draw_loops, ember_draw_kept, ember_draw_skipped, ember_draw_odd, ember_draw_nested;
// verify mode: the recording loops' step-overs (YYCompareVal), the cached loops' whole lists and drawn embers
unsigned ember_draw_cmp_ok, ember_draw_cmp_bad, ember_draw_list_ok, ember_draw_list_bad, ember_draw_drawn_ok,
	ember_draw_drawn_bad;
uint32_t ember_draw_us;

#define KIND(v) ((v)->kind & 0xffffff)

// The other.id the loop compares against, read once per loop (its other, the corpse or the player, is the same
// instance for the whole loop, and an instance's id never changes)
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

// The ember's parent variable (YYObjectBase::InternalGetYYVarRef, which creates it if missing, as reading it does)
static RV *parent_of(void *ember) {
	return var_parent[1] < 0 ? NULL : (*(var_fn *)(*(uint8_t **)ember + 8))(ember, var_parent[1]);
}

// ---- The ember list of one obj_drawactors Draw event ----
// The first ember loop of the event runs the runner's iterator and records the embers it visits, in that order, with
// a pointer to each one's parent variable. YYGML_NewWithIteratorEx builds its list when the loop starts, from the
// object's instance list, the pending instance changes (g_InstanceChangeArray) and the pending (de)activations
// (g_InstanceActivateDeactive), filtered by the active flags ((+0x7c & 0x100007) == 4), and leaves nothing behind (it
// resets the instance link fields it borrows); YYGML_WithIteratorNext then walks that list without looking at the
// instances again. The record serves a later loop only while nothing that list depends on has changed since the
// recording loop started (runner of APK 1.1.10, checked in its code):
// - The same Draw event invocation (draw_epoch). The instance lists and the pending arrays are only rebuilt
//   (UpdateActiveLists) and instances only freed (CRoom::RemoveMarked / DeleteInstance) from the main loop
//   (DoAStep_*, Draw_Room / DrawTheRoom between draw passes, StartRoom / EndRoom), never from inside an event, so within
//   one event the recorded instances stay allocated and the arrays only grow.
// - No instance created: every path that adds an instance to the room (instance_create_* through CRoom::AddInstance,
//   instance_copy) takes its id as ++room_maxid. CInstance::ms_CurrentCreateCounter is compared too.
// - No instance (de)activated or changed into another object (ember_draw_set_gen): CInstance::Activate, Deactivate and
//   SetDeactivated are the only writers of the deactivated flag, and ChangeObjectIndex (instance_change) the only
//   writer of g_InstanceChangeArray; each is entry-hooked to count. That catches an ember reactivated without a new
//   queue entry and an instance changed while already queued, which the array counts miss.
// - The change and (de)activation arrays hold as many entries as when the list was recorded.
// An ember destroyed (or deactivated) since is still allocated and its flags tell: a cached loop visits the recorded
// embers active when it starts, as the runner's list would hold them. Parents are read live through the recorded
// pointers: an instance variable is a pool-allocated RValue that the instance's variable map points to, so it stays put
// while the instance lives (the map's rehash moves only the pointers; no game code calls variable_instance_remove).
#define EMBER_MAX 1024
static struct {
	int valid;
	uint32_t epoch, gen, maxid, creates[2], changes, activations;
	int n;
	void *inst[EMBER_MAX];
	RV *parent[EMBER_MAX]; // NULL: no such variable (always visited)
} rec;
static int cache_ok; // the Draw event wrapper and the four hooks are installed
static int recording, rec_overflow;
static uint32_t snap_epoch, snap_gen, snap_maxid, snap_creates[2], snap_changes, snap_activations;
static uint32_t *create_counter, *change_array, *activation_array; // the arrays' count is word 2
static const int *room_maxid;
unsigned ember_draw_cached_loops, ember_draw_recorded_loops;
uint32_t ember_draw_set_gen; // instances (de)activated or changed into another object (hooks below)
static uint32_t draw_epoch;  // obj_drawactors Draw entries and exits
static int draw_depth;

static void snapshot(void) {
	snap_epoch = draw_epoch;
	snap_gen = ember_draw_set_gen;
	snap_maxid = (uint32_t)*room_maxid;
	snap_creates[0] = create_counter[0];
	snap_creates[1] = create_counter[1];
	snap_changes = change_array[2];
	snap_activations = activation_array[2];
}

static int rec_current(void) {
	return rec.valid && draw_depth && rec.epoch == draw_epoch && rec.gen == ember_draw_set_gen &&
		rec.maxid == (uint32_t)*room_maxid && rec.creates[0] == create_counter[0] &&
		rec.creates[1] == create_counter[1] && rec.changes == change_array[2] && rec.activations == activation_array[2];
}

static int active(void *inst) {
	return (*(uint32_t *)((uint8_t *)inst + 0x7c) & 0x100007) == 4;
}

typedef void ABI (*inst_fn)(void *inst);
typedef void ABI (*inst_int_fn)(void *inst, int arg);
static inst_fn orig_activate, orig_deactivate;
static inst_int_fn orig_set_deactivated, orig_change_index;
static void ABI hook_activate(void *inst) {
	ember_draw_set_gen++;
	orig_activate(inst);
}
static void ABI hook_deactivate(void *inst) {
	ember_draw_set_gen++;
	orig_deactivate(inst);
}
static void ABI hook_set_deactivated(void *inst, int deactivated) {
	ember_draw_set_gen++;
	orig_set_deactivated(inst, deactivated);
}
static void ABI hook_change_index(void *inst, int index) {
	ember_draw_set_gen++;
	orig_change_index(inst, index);
}

// The patched loop in progress (NULL: none). A loop on any other iterator meanwhile is nested in it (reached from an
// ember's or a corpse's event): it gets the runner's iterator untouched and leaves the state below alone.
static void *cur_it;
static int cur_own, cur_off; // served from the list; A/B off (read once per loop)

// A cached loop's candidates: the recorded embers active when it started (the runner's list pointer stays 0: its
// WithIteratorNext would return 0 and DeleteWithIterator frees nothing, only puts self and other back)
static int own_n, own_pos;
static void *own_list[EMBER_MAX];
static RV *own_parent[EMBER_MAX];

static uint32_t loop_start;

static void loop_done(void) {
	cur_it = NULL;
	if (debugMode)
		ember_draw_us += sceKernelGetProcessTimeLow() - loop_start;
}

// The next candidate whose parent (read now, when the original body would read it) might equal other.id
static int own_next(void **self) {
	while (own_pos < own_n) {
		RV *parent = own_parent[own_pos];
		void *ember = own_list[own_pos++];
		if (parent && differs(parent, &id_value)) {
			ember_draw_skipped++;
			continue;
		}
		*self = ember;
		ember_draw_kept++;
		return 1;
	}
	return 0;
}

// The runner's iteration (recording, or the A/B off state): steps over embers certainly not other's
static int skip(int r, void *iterator, void **self, void **other) {
	while (r > 0) {
		if (cur_off)
			break;
		RV *parent = *self ? parent_of(*self) : NULL;
		if (recording) {
			if (rec.n < EMBER_MAX) {
				rec.inst[rec.n] = *self;
				rec.parent[rec.n] = parent;
				rec.n++;
			} else {
				rec_overflow = 1;
			}
		}
		if (!parent || !*other || var_id[1] < 0 || !differs(parent, other_id(*other)))
			break;
		if (verify) {
			if (compare(parent, other_id(*other), *epsilon, 0) == 0)
				ember_draw_cmp_bad++;
			else
				ember_draw_cmp_ok++;
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
			rec.epoch = snap_epoch;
			rec.gen = snap_gen;
			rec.maxid = snap_maxid;
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

// Verify mode, for a loop served from the list (before it runs): the runner's own loop for this caller must visit
// exactly the candidates, in order, and so draw the same embers (parent compares equal to other.id)
static void verify_loop(const void *target, void *caller) {
	uint32_t it[4];
	void *s = caller, *o = NULL;
	int r = new_with(it, &s, &o, target);
	const RV *id = other_id(caller);
	int k = 0, d = 0, list_ok = 1, drawn_ok = 1;
	while (r > 0) {
		if (k >= own_n || own_list[k] != s)
			list_ok = 0;
		k++;
		RV *parent = parent_of(s);
		if (parent && compare(parent, id, *epsilon, 0) == 0) {
			while (d < own_n && !(own_parent[d] && compare(own_parent[d], id, *epsilon, 0) == 0))
				d++;
			if (d >= own_n || own_list[d] != s)
				drawn_ok = 0;
			else
				d++;
		}
		r = next_with(it, &s, &o);
	}
	if (k != own_n)
		list_ok = 0;
	for (; d < own_n; d++)
		if (own_parent[d] && compare(own_parent[d], id, *epsilon, 0) == 0)
			drawn_ok = 0;
	delete_with(it, &s, &o);
	if (list_ok)
		ember_draw_list_ok++;
	else if (ember_draw_list_bad++ < 8)
		debugPrintf("[EMBERDRAW] mismatch: a cached loop's ember list differs from the runner's\n");
	if (drawn_ok)
		ember_draw_drawn_ok++;
	else if (ember_draw_drawn_bad++ < 8)
		debugPrintf("[EMBERDRAW] mismatch: a cached loop's drawn embers differ from the runner's\n");
}

static int ABI ember_new_with(void *iterator, void **self, void **other, const void *target) {
	if (cur_it && cur_it != iterator) { // nested in a patched loop
		ember_draw_nested++;
		return new_with(iterator, self, other, target);
	}
	if (debugMode)
		loop_start = sceKernelGetProcessTimeLow();
	ember_draw_loops++;
	id_other = NULL; // the id is read again for each loop
	recording = 0;
	cur_it = NULL;
	cur_off = ember_draw_ab && ember_draw_ab_off;
	if (!cur_off && cache_ok && var_id[1] >= 0 && rec_current()) {
		// As YYGML_NewWithIterator starts: the saved self and other, no list; then other = the caller and self = the
		// first ember visited
		void *caller = *self;
		uint32_t *it = iterator;
		it[0] = (uint32_t)caller;
		it[1] = (uint32_t)*other;
		it[2] = 0;
		it[3] = 0;
		other_id(caller);
		own_n = 0;
		for (int k = 0; k < rec.n; k++)
			if (active(rec.inst[k])) {
				own_list[own_n] = rec.inst[k];
				own_parent[own_n++] = rec.parent[k];
			}
		ember_draw_cached_loops++;
		if (verify)
			verify_loop(target, caller);
		own_pos = 0;
		if (!own_next(self)) {
			loop_done();
			return 0;
		}
		cur_it = iterator;
		cur_own = 1;
		*other = caller;
		return 1;
	}
	if (!cur_off && cache_ok && draw_depth) {
		recording = 1;
		rec_overflow = 0;
		rec.valid = 0;
		rec.n = 0;
		snapshot();
		ember_draw_recorded_loops++;
	}
	cur_it = iterator;
	cur_own = 0;
	return skip(new_with(iterator, self, other, target), iterator, self, other);
}

static int ABI ember_next_with(void *iterator, void **self, void **other) {
	if (!cur_it || iterator != cur_it)
		return next_with(iterator, self, other);
	if (cur_own) {
		if (own_next(self))
			return 1;
		loop_done();
		return 0;
	}
	return skip(next_with(iterator, self, other), iterator, self, other);
}

// obj_drawactors' Draw event: the ember list lives for one invocation
static event_func orig_drawactors_draw;
static void drawactors_draw(void *self, void *other) {
	if (!draw_depth) { // a loop left unfinished (an exception thrown through the event) ends here
		cur_it = NULL;
		recording = 0;
	}
	draw_depth++;
	draw_epoch++;
	orig_drawactors_draw(self, other);
	draw_epoch++;
	draw_depth--;
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

int ember_draw_install(gml_func_entry *table) {
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
	room_maxid = (const int *)so_symbol(&yoyoloader_mod, "room_maxid"); // Room_Get_maxid's global
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
	// The ember list needs the Draw event wrapper and the four instance-set hooks; without them every loop steps over
	// the other corpses' embers itself, from the runner's iterator
	if (table && create_counter && change_array && activation_array && room_maxid) {
		uintptr_t act = so_symbol(&yoyoloader_mod, "_ZN9CInstance8ActivateEv"),
			deact = so_symbol(&yoyoloader_mod, "_ZN9CInstance10DeactivateEv"),
			set_deact = so_symbol(&yoyoloader_mod, "_ZN9CInstance14SetDeactivatedEb"),
			change = so_symbol(&yoyoloader_mod, "_ZN9CInstance17ChangeObjectIndexEi");
		if ((orig_activate = (inst_fn)entry_hook(act, hook_activate)) &&
				(orig_deactivate = (inst_fn)entry_hook(deact, hook_deactivate)) &&
				(orig_set_deactivated = (inst_int_fn)entry_hook(set_deact, hook_set_deactivated)) &&
				(orig_change_index = (inst_int_fn)entry_hook(change, hook_change_index)) &&
				(orig_drawactors_draw = gml_replace(table, "gml_Object_obj_drawactors_Draw_0", drawactors_draw)))
			cache_ok = 1;
	}
	if (debugMode) {
		verify = exp_flag("emberdrawverify");
		ember_draw_ab = exp_flag("emberdrawab");
	}
	debugPrintf("ember draw: the corpse and player ember loops step over other corpses' embers%s%s%s\n",
		cache_ok ? ", ember list per Draw event" : " (no ember list)", verify ? " (verify mode)" : "",
		ember_draw_ab ? " (A/B per report window)" : "");
	return 1;
}
