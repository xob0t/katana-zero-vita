/*
 * bike_draw.c
 *
 * The bike chase (room_mansion_bikechase) draws its actors through obj_drawactors_bike's Draw event (APK 1.1.10,
 * gml_Object_obj_drawactors_bike_Draw_0). For every actor, in depth order, it runs (GML lines 129-157):
 *
 *     if (has_particles)
 *         with (obj_particle)                                  // target 717, iterator call at +0x2f40
 *             if (parent == other.id)
 *                 if (inside_screen(max(sprite_height * 2, sprite_width * 2))) {
 *                     event_user(15);
 *                     event_user(13);
 *                 }
 *     if (has_trail) {
 *         gpu_set_blendmode(bm_add);
 *         with (obj_bike_motion_trail)                         // target 29
 *             if (parent == other.id)
 *                 event_user(15);
 *         gpu_set_blendmode(bm_normal);
 *     }
 *
 * so every actor that owns particles walks every particle in the room in GML. When the helicopter's mortars and
 * rockets explode, ~50 mortar actors each walk ~250 explosion particles: ~12,000 GML iterations per frame, 30+ ms of
 * the frame (the drop on every rocket explosion).
 *
 * Both with loops keep their original code and order; only their iterator calls (YYGML_NewWithIterator /
 * YYGML_WithIteratorNext) are redirected, through veneers in the code cave (as in trail_lights.c), to wrappers that
 * step over instances whose parent is certainly not other.id. For such an instance the original body only read
 * parent and other.id and compared them (YYCompareVal): no write, no call with side effects. Instances whose parent
 * might equal other.id (or whose values aren't plain reals or references) still run the original body, which makes
 * the real comparison. So the particles drawn, their order and every variable stay exactly as before.
 *
 * Before that, the event sorts the actors by y into actor_list with an insertion sort in GML (lines 25-89, three
 * with loops over obj_actor, obj_bikeplayer and obj_bike_object):
 *
 *     with (...) if (draw_layer == 0) {
 *         found = false; pos = -1;
 *         while (!found) {
 *             pos += 1;
 *             if (pos >= ds_list_size(other.actor_list)) found = true;
 *             else if (y <= ds_list_find_value(other.actor_list, pos).y) found = true;
 *         }
 *         ds_list_insert(other.actor_list, pos, id);
 *     }
 *
 * O(n^2) GML: with the ~100 traffic cones on the road that is ~5,000 list lookups per frame, ~10 ms. The loops' iterator
 * calls go to wrappers that run this body natively for each instance: the same draw_layer comparison, the same
 * YYCompareVal(y, list[pos].y) for each position (from a copy of the y values already in the list, which nothing
 * changes while the list is built), and the runner's own ds_list_insert with the same list, position and id. The copy
 * starts when the first loop begins with an empty list; if the list size ever differs from the copy, or a value
 * isn't a plain real, the original body runs from then on (until the next frame). The list ends up the same, so the
 * draw order is the same.
 *
 * The particle loop's list is also cut before the loop starts (part_new_with): the particles' parents are read once
 * per Draw event and reused while the iterator list holds the same instances (see "Particle loop" below).
 *
 * exp_bikedraw0 keeps the original. exp_bikedrawverify never steps over anything; it runs the runner's YYCompareVal
 * for every instance the wrapper would have stepped over and counts any that compared equal, and lets the original
 * sort body run, checking that it inserted each actor where the native search said ([BIKEDRAW] report; mismatches
 * must stay 0).
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#include "main.h"
#include "so_util.h"

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef int ABI (*new_fn)(void *iterator, void **self, void **other, const void *target);
typedef int ABI (*next_fn)(void *iterator, void **self, void **other);
typedef RV *ABI (*var_fn)(void *instance, int id);
typedef void ABI (*getvalue_fn)(const RV *instance, int id, int index, RV *out, int, int);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double epsilon, int throw_error);
typedef void ABI (*free_fn)(RV *value);
typedef void ABI (*builtin_fn)(RV *result, void *self, void *other, int argc, RV *args);

static new_fn new_with;
static next_fn next_with;
static getvalue_fn variable_get;
static compare_fn compare;
static const double *epsilon;
static const int *var_parent, *var_id; // {name, id}: ids are filled at game start, read them live
static const int *var_draw_layer, *var_actor_list, *var_y;
static builtin_fn list_size, list_insert, list_find;
static int verify;

unsigned bike_draw_kept, bike_draw_skipped, bike_draw_verified, bike_draw_mismatch, bike_draw_odd;
unsigned bike_sort_native, bike_sort_layer, bike_sort_fallback, bike_sort_verified, bike_sort_mismatch;

#define KIND(v) ((v)->kind & 0xffffff)

// The other.id the loop compares against, for the current other (the actor)
static void *id_other;
static RV id_value;

static const RV *other_id(void *other) {
	if (other != id_other) {
		RV inst = {0, 0, 6}, out = {0, 0, 0xffffff};
		memcpy(&inst.value, &other, sizeof(other));
		variable_get(&inst, var_id[1], (int)0x80000000, &out, 0, 0);
		// An id is a real or a reference: nothing to free. Anything else is never stepped over.
		id_value = out;
		id_other = other;
	}
	return &id_value;
}

// 1: a parent value certainly differs from the id; 0: it may be equal; -1: kinds this doesn't decide (both 0 and -1
// leave the instance to the original body, which makes the real comparison)
static int differs(const RV *parent, const RV *id) {
	uint32_t pk = KIND(parent), ik = KIND(id);
	if (pk == 0 && ik == 0) // instance ids are whole numbers: half a unit apart is far beyond the comparison's epsilon
		return *epsilon >= 0 && *epsilon < 0.5 && fabs(parent->value - id->value) > 0.5; // only while eps < 0.5
	if (pk == 15 && ik == 15) {
		uint32_t pw[2], iw[2];
		memcpy(pw, &parent->value, 8);
		memcpy(iw, &id->value, 8);
		return pw[1] == iw[1] && pw[0] != iw[0]; // same reference type, different index
	}
	if (pk == 0 && ik == 15)
		return parent->value < 0; // noone / -1 against an instance reference (whose index is never negative)
	return -1;
}

static RV *parent_of(void *self) {
	return self && var_parent[1] >= 0 ? (*(var_fn *)(*(uint8_t **)self + 8))(self, var_parent[1]) : NULL;
}

// 1 if parent == other.id is certainly false for this instance
static int certainly_other(void *self, void *other) {
	if (!self || !other || var_parent[1] < 0 || var_id[1] < 0)
		return 0;
	RV *parent = parent_of(self);
	const RV *id = other_id(other);
	if (!parent)
		return 0;
	int d = differs(parent, id);
	if (d < 0)
		bike_draw_odd++;
	if (d <= 0)
		return 0;
	if (verify) {
		if (compare(parent, id, *epsilon, 0) == 0)
			bike_draw_mismatch++;
		else
			bike_draw_verified++;
		return 0;
	}
	return 1;
}

unsigned bike_draw_skip_us; // time in the wrappers' stepping (heavy profiling only)
extern int heavy_profiling;

static int skip(int r, void *iterator, void **self, void **other) {
	uint32_t t0 = heavy_profiling ? sceKernelGetProcessTimeLow() : 0;
	while (r > 0 && certainly_other(*self, *other)) {
		bike_draw_skipped++;
		r = next_with(iterator, self, other);
	}
	if (r > 0)
		bike_draw_kept++;
	if (heavy_profiling)
		bike_draw_skip_us += sceKernelGetProcessTimeLow() - t0;
	return r;
}

static int ABI bike_new_with(void *iterator, void **self, void **other, const void *target) {
	id_other = NULL; // the id is read again for each loop (the actor could in principle change it in between)
	return skip(new_with(iterator, self, other, target), iterator, self, other);
}

static int ABI bike_next_with(void *iterator, void **self, void **other) {
	return skip(next_with(iterator, self, other), iterator, self, other);
}

// ---- Particle loop: the parents of the particles, read once per Draw event ----
//
// The particle loop runs once for every actor that owns particles (50 mortars in a volley) over every particle
// (~250): its iterator list is the same each time. The first loop of a Draw event reads each particle's parent once;
// later loops whose iterator list is the same instances in the same order reuse those values, and the list is cut
// down to the particles whose parent may equal other.id before the loop starts (same order). Nothing in the Draw event
// writes a particle's parent (the loop bodies are particles' and actors' user events 13/14/15/6/8, none of which
// touches parent), so each comparison the original would make at that particle's turn has the same result.
// exp_bikedrawverify checks exactly that: the lists aren't cut, and at each particle's turn the cached decision is
// compared with the live parent.

typedef struct { void *self, *other; void **list, **cursor; } with_iter; // SWithIterator; Next walks down to list[0] = NULL

#define PCACHE_MAX 2048
static void *pc_inst[PCACHE_MAX]; // the loop's instances in visiting order
static RV pc_parent[PCACHE_MAX];  // their parent, when first read in this Draw event (kind 0xdead: no variable)
static int pc_n;                  // 0: nothing read yet in this Draw event
static void *pc_keep[PCACHE_MAX];
unsigned bike_pcache_hits, bike_pcache_reads, bike_pcache_odd, bike_pcache_verified, bike_pcache_mismatch,
	bike_pcache_conservative;

// Verify mode: the cached decision per visiting position for the running loop
static void *pv_iter;
static int8_t pv_dec[PCACHE_MAX];
static int pv_pos, pv_n;

static void pv_check(void *iterator, void *self, void *other) {
	if (iterator != pv_iter || pv_pos >= pv_n || !self || !other)
		return;
	int dec = pv_dec[pv_pos++];
	RV *parent = parent_of(self);
	int live = parent ? differs(parent, other_id(other)) : -1;
	if (dec == 1 && live != 1)
		bike_pcache_mismatch++; // would have stepped over a particle the original might draw
	else if (dec != 1 && live == 1)
		bike_pcache_conservative++; // kept one the original skips (exact, only slower)
	else
		bike_pcache_verified++;
}

// 1: the iterator list was cut (r set), 0: leave this loop to the stepping wrappers
static int part_cut(void *iterator, void **self, void **other, int *r) {
	with_iter *w = iterator;
	if (*r <= 0 || !*self || !*other || var_parent[1] < 0 || var_id[1] < 0 || !w->list || w->cursor < w->list + 1 ||
			w->list[0])
		return 0;
	int n = w->cursor - w->list; // *self, then cursor[-1] ... list[1]
	if (n > PCACHE_MAX)
		return 0;
	int same = pc_n == n && pc_inst[0] == *self;
	for (int i = 1; same && i < n; i++)
		same = pc_inst[i] == w->cursor[-i];
	if (!same) {
		for (int i = 0; i < n; i++) {
			void *inst = i ? w->cursor[-i] : *self;
			RV *p = parent_of(inst);
			pc_inst[i] = inst;
			if (p)
				pc_parent[i] = *p; // a real or a reference is copied as is; any other kind is never decided
			else
				pc_parent[i] = (RV){0, 0, 0xdead};
		}
		pc_n = n;
		bike_pcache_reads += n;
	} else
		bike_pcache_hits += n;
	const RV *id = other_id(*other);
	int m = 0;
	for (int i = 0; i < n; i++) {
		int d = KIND(&pc_parent[i]) == 0xdead ? -1 : differs(&pc_parent[i], id);
		if (d < 0)
			bike_pcache_odd++;
		if (verify)
			pv_dec[i] = (int8_t)d;
		else if (d != 1)
			pc_keep[m++] = pc_inst[i];
	}
	if (verify) {
		pv_iter = iterator;
		pv_pos = 0;
		pv_n = n;
		return 0;
	}
	bike_draw_kept += m ? 1 : 0; // the others are counted as their turn comes
	bike_draw_skipped += n - m;
	if (m) {
		*self = pc_keep[0];
		for (int j = 1; j < m; j++)
			w->list[m - j] = pc_keep[j];
		w->cursor = w->list + m;
	}
	*r = m;
	return 1;
}

extern int debugMode;
static int no_cut; // exp_bikepcache0 (debug): step as in the first version
unsigned bike_draw_newwith_us; // the runner's NewWithIterator for the particle loop (heavy profiling only)

static int ABI part_new_with(void *iterator, void **self, void **other, const void *target) {
	id_other = NULL;
	uint32_t t0 = heavy_profiling ? sceKernelGetProcessTimeLow() : 0;
	int r = new_with(iterator, self, other, target);
	uint32_t t1 = heavy_profiling ? sceKernelGetProcessTimeLow() : 0;
	int cut = !no_cut && part_cut(iterator, self, other, &r);
	if (heavy_profiling) {
		bike_draw_newwith_us += t1 - t0;
		bike_draw_skip_us += sceKernelGetProcessTimeLow() - t1;
	}
	if (cut)
		return r;
	if (verify && r > 0)
		pv_check(iterator, *self, *other);
	return skip(r, iterator, self, other);
}

static int ABI part_next_with(void *iterator, void **self, void **other) {
	int r = next_with(iterator, self, other);
	if (verify && r > 0)
		pv_check(iterator, *self, *other);
	// A cut list holds only particles whose parent may equal other.id: the stepping test keeps each of them (with
	// their parent unchanged) at the cost of one variable read; a loop that wasn't cut steps as before
	return skip(r, iterator, self, other);
}

// ---- Actor sort ----

#define SORT_MAX 1024
static RV sort_y[SORT_MAX]; // y of each actor in actor_list, in list order
static int sort_n, sort_valid, sort_start;
static RV *var_of(void *inst, const int *desc) { return (*(var_fn *)(*(uint8_t **)inst + 8))(inst, desc[1]); }

// Why the native sort stopped: source line -> count, and the last odd kind (profiling report)
unsigned bike_sort_reject_line[8], bike_sort_reject_count[8], bike_sort_reject_kind;
static int sort_reject(unsigned line, uint32_t kind) {
	bike_sort_reject_kind = kind;
	for (int k = 0; k < 8; k++)
		if (!bike_sort_reject_line[k] || bike_sort_reject_line[k] == line) {
			bike_sort_reject_line[k] = line;
			bike_sort_reject_count[k]++;
			break;
		}
	return sort_valid = 0;
}
#define SORT_REJECT(kind) return sort_reject(__LINE__, kind)

static int list_count(void *self, void *other, const RV *list) {
	RV args[1] = {*list}, out = {0, 0, 0xffffff};
	list_size(&out, self, other, 1, args);
	return KIND(&out) == 0 && out.value >= 0 && out.value < 1e6 ? (int)out.value : -1;
}

// Verify mode: where the native search put the last actor, checked after the original body inserted it
static int pending_pos = -1;
static RV pending_y, pending_id, pending_list;
static void *pending_self, *pending_other;

static void verify_pending(void) {
	if (pending_pos < 0)
		return;
	int pos = pending_pos;
	pending_pos = -1;
	int n = list_count(pending_self, pending_other, &pending_list);
	RV args[2] = {pending_list, {(double)pos, 0, 0}}, out = {0, 0, 0xffffff};
	list_find(&out, pending_self, pending_other, 2, args);
	int ok = n == sort_n + 1 && KIND(&out) == KIND(&pending_id) && !memcmp(&out.value, &pending_id.value, 8);
	if (ok) {
		bike_sort_verified++;
		memmove(&sort_y[pos + 1], &sort_y[pos], (sort_n - pos) * sizeof(RV));
		sort_y[pos] = pending_y;
		sort_n++;
	} else {
		if (bike_sort_mismatch++ < 8)
			debugPrintf("[BIKEDRAW] sort mismatch: native pos %d, list size %d (expected %d)\n", pos, n, sort_n + 1);
		sort_valid = 0;
	}
}

// The sort body for self (other: obj_drawactors_bike). 1: done natively, 0: let the original body run.
static int sort_body(void *self, void *other) {
	verify_pending();
	if (!sort_valid || !self || !other || var_draw_layer[1] < 0 || var_actor_list[1] < 0 || var_y[1] < 0 || var_id[1] < 0)
		return 0;
	RV zero = {0, 0, 0};
	RV *layer = var_of(self, var_draw_layer);
	if (!layer || KIND(layer) != 0)
		SORT_REJECT(layer ? KIND(layer) : 0xdead);
	if (compare(layer, &zero, *epsilon, 0) != 0) {
		bike_sort_layer++;
		return 1; // draw_layer != 0: the original does nothing
	}
	RV *list = var_of(other, var_actor_list);
	if (!list || (KIND(list) != 0 && KIND(list) != 15)) // a list id: a number or a ds_list reference
		SORT_REJECT(list ? KIND(list) : 0xdead);
	RV lst = *list;
	if (sort_start) { // the list was cleared right before the first loop
		sort_start = 0;
		if (list_count(self, other, &lst) != 0)
			SORT_REJECT(0xffff);
	}
	if (list_count(self, other, &lst) != sort_n || sort_n >= SORT_MAX)
		SORT_REJECT(sort_n);
	RV inst = {0, 0, 6}, ys = {0, 0, 0xffffff}, id = {0, 0, 0xffffff};
	memcpy(&inst.value, &self, sizeof(self));
	variable_get(&inst, var_y[1], (int)0x80000000, &ys, 0, 0);
	variable_get(&inst, var_id[1], (int)0x80000000, &id, 0, 0);
	if (KIND(&ys) != 0)
		SORT_REJECT(KIND(&ys));
	if (KIND(&id) != 0 && KIND(&id) != 15)
		SORT_REJECT(KIND(&id));
	int pos = 0;
	for (; pos < sort_n; pos++) {
		int c = compare(&ys, &sort_y[pos], *epsilon, 1);
		if (c != -2 && c <= 0)
			break;
	}
	if (verify) {
		pending_pos = pos;
		pending_y = ys;
		pending_id = id;
		pending_list = lst;
		pending_self = self;
		pending_other = other;
		return 0;
	}
	RV args[3] = {lst, {(double)pos, 0, 0}, id}, out = {0, 0, 0xffffff};
	list_insert(&out, self, other, 3, args);
	memmove(&sort_y[pos + 1], &sort_y[pos], (sort_n - pos) * sizeof(RV));
	sort_y[pos] = ys;
	sort_n++;
	bike_sort_native++;
	return 1;
}

unsigned bike_sort_us; // time in the native sort (heavy profiling only)

static int sort_run(int r, void *iterator, void **self, void **other) {
	uint32_t t0 = heavy_profiling ? sceKernelGetProcessTimeLow() : 0;
	while (r > 0 && sort_body(*self, *other))
		r = next_with(iterator, self, other);
	if (r > 0 && !verify)
		bike_sort_fallback++;
	if (heavy_profiling)
		bike_sort_us += sceKernelGetProcessTimeLow() - t0;
	return r;
}

// First loop (with obj_actor): actor_list was just cleared. The copy starts at the first actor of any of the three
// loops (obj_actor may have no instances), whose other is obj_drawactors_bike.

static int ABI sort_first_new_with(void *iterator, void **self, void **other, const void *target) {
	pending_pos = -1;
	pc_n = 0; // a new Draw event: the particles' parents are read again
	pv_iter = NULL;
	sort_n = 0;
	sort_valid = 1;
	sort_start = 1;
	return sort_run(new_with(iterator, self, other, target), iterator, self, other);
}

static int ABI sort_new_with(void *iterator, void **self, void **other, const void *target) {
	return sort_run(new_with(iterator, self, other, target), iterator, self, other);
}

static int ABI sort_next_with(void *iterator, void **self, void **other) {
	verify_pending(); // the original body just ran for the previous instance (verify mode)
	return sort_run(next_with(iterator, self, other), iterator, self, other);
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

int bike_draw_install(void) {
	// ELF addresses in APK 1.1.10: the event, the with targets (movw r0, #0x165 then add #0x168 = 717, obj_particle;
	// mov r0, #0x1d = 29, obj_bike_motion_trail; #0x2d = 45, obj_bikeplayer; #0x1e = 30, obj_bike_object), the iterator
	// call sites and the PLT entries
	enum { EVENT = 0x3cca464, PART_TARGET = 0x3ccd380, PART_NEW = 0x3ccd3a4, PART_NEXT = 0x3ccd774,
	       TRAIL_TARGET = 0x3ccd8bc, TRAIL_NEW = 0x3ccd8dc, TRAIL_NEXT = 0x3ccd9c8,
	       SORT1_NEW = 0x3ccb0b4, SORT1_NEXT = 0x3ccb7e0, SORT2_TARGET = 0x3ccb80c, SORT2_NEW = 0x3ccb830,
	       SORT2_NEXT = 0x3ccbf58, SORT3_TARGET = 0x3ccbf84, SORT3_NEW = 0x3ccbfa8, SORT3_NEXT = 0x3ccc6d0,
	       PLT_NEW = 0x4ab0e00, PLT_NEXT = 0x4ab0e30 };
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z37gml_Object_obj_drawactors_bike_Draw_0P9CInstanceS0_");
	new_with = (new_fn)so_symbol(&yoyoloader_mod, "_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	next_with = (next_fn)so_symbol(&yoyoloader_mod, "_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
	variable_get = (getvalue_fn)so_symbol(&yoyoloader_mod, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	compare = (compare_fn)so_symbol(&yoyoloader_mod, "_Z12YYCompareValRK6RValueS1_db");
	epsilon = (const double *)so_symbol(&yoyoloader_mod, "g_GMLMathEpsilon");
	list_size = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z12F_DsListSizeR6RValueP9CInstanceS2_iPS_");
	list_insert = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z14F_DsListInsertR6RValueP9CInstanceS2_iPS_");
	list_find = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z17F_DsListFindValueR6RValueP9CInstanceS2_iPS_");
	var_parent = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_parent");
	var_id = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_id");
	var_draw_layer = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_draw_layer");
	var_actor_list = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_actor_list");
	var_y = (const int *)so_symbol(&yoyoloader_mod, "g_VAR_y");
	if (!entry || !new_with || !next_with || !variable_get || !compare || !epsilon || !var_parent || !var_id ||
			!list_size || !list_insert || !list_find || !var_draw_layer || !var_actor_list || !var_y)
		return 0;
	// The prologue may be relocated by a profiling entry hook; the body must be the 1.1.10 one
	uint32_t *w = (uint32_t *)entry;
#define AT(a) (entry + (a) - EVENT)
	if (!((w[0] == 0xe92d4ff0 && w[1] == 0xe28db01c) || w[0] == 0xe51ff004) ||
			*(uint32_t *)AT(PART_TARGET) != 0xe3000165 || *(uint32_t *)AT(PART_TARGET + 4) != 0xe2800f5a ||
			*(uint32_t *)AT(TRAIL_TARGET) != 0xe3a0001d || *(uint32_t *)AT(SORT2_TARGET) != 0xe3a0002d ||
			*(uint32_t *)AT(SORT3_TARGET) != 0xe3a0001e)
		return 0;
	enum { NSITES = 10 };
	uintptr_t sites[NSITES] = {AT(PART_NEW), AT(PART_NEXT), AT(TRAIL_NEW), AT(TRAIL_NEXT), AT(SORT1_NEW), AT(SORT1_NEXT),
		AT(SORT2_NEW), AT(SORT2_NEXT), AT(SORT3_NEW), AT(SORT3_NEXT)};
	uintptr_t wrappers[NSITES] = {(uintptr_t)&part_new_with, (uintptr_t)&part_next_with, (uintptr_t)&bike_new_with,
		(uintptr_t)&bike_next_with, (uintptr_t)&sort_first_new_with, (uintptr_t)&sort_next_with,
		(uintptr_t)&sort_new_with, (uintptr_t)&sort_next_with, (uintptr_t)&sort_new_with, (uintptr_t)&sort_next_with};
	for (int k = 0; k < NSITES; k++) {
		int is_new = !(k & 1);
		if (!check_call(sites[k], is_new ? AT(PLT_NEW) : AT(PLT_NEXT), is_new ? (uintptr_t)new_with : (uintptr_t)next_with))
			return 0;
	}
	// One block for all ten veneers (the sites lie within 12 KB of each other), every displacement checked before any
	// patch, so a failure leaves no site patched and costs the cave one allocation at most
	uint32_t *block = (uint32_t *)so_alloc_near(&yoyoloader_mod, sites[0], 8 * NSITES), *veneers[NSITES];
	if (!block)
		return 0;
	for (int k = 0; k < NSITES; k++)
		if (!bl_reaches(sites[k], (uintptr_t)(veneers[k] = block + 2 * k)))
			return 0;
#undef AT
	// All of them or none
	for (int k = 0; k < NSITES; k++)
		patch_call(sites[k], wrappers[k], veneers[k]);
	verify = exp_flag("bikedrawverify");
	no_cut = debugMode && exp_flag("bikepcache0");
	debugPrintf("bike draw: native actor sort, particle/trail loops step over other actors' instances%s\n",
		verify ? " (verify mode)" : "");
	return 1;
}
