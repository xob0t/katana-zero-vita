/*
 * place_native.c: place_meeting / instance_place (Command_InstancePlace) natively (exact).
 * Target libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701 (Katana ZERO 1.1.10); ELF
 * addresses below.
 *
 * Enemies, the player, ceiling tiles, lasers and corpses call place_meeting / instance_place 100-200 times a frame,
 * mostly against obj_block (bbox-only solids), obj_jumpthrough and obj_trap. The runner's Command_InstancePlace
 * (0xf65aec), on its R-tree path (ShouldUseFastCollision returns 1):
 *   1. ShouldUseFastCollision(selector), UpdateTree() (drains the dirty list), Compute_BoundingBox(self, true) if the
 *      caller's box is stale (flag 8 at +0x7c);
 *   2. saves the box (+0xec..+0xf8) and x/y, SetPosition(self, px, py) (SkeletonAnimation; when x or y changes: new
 *      x/y, flag 8, CollisionMarkDirty), Compute_BoundingBox(self, false) when flag 8 is set (the probe box; clears
 *      flag 8, sets 0x200 for a precise mask);
 *   3. the search rectangle from the box, s_instancePtr = 0, the selector word, s_instanceList = list,
 *      GetCollisionTreeForDomain(domain of the flags), tree lock byte 1, RTree::Search with the callback 0xf65f80,
 *      lock byte 0, s_instanceList = 0;
 *   4. SetPosition(self, old x, old y) (flag 8 and CollisionMarkDirty again), the saved box back; returns s_instancePtr.
 * Search calls the callback for every leaf entry that overlaps the probe; it filters the candidate (not the caller,
 * flags & 0x100003 clear, +0x50 == 1, object == the selector or a descendant through CObjectGM::IsDecendentOf) and
 * tests it with CInstance::Collision_Instance(candidate, caller, false). The first that collides is stored in
 * s_instancePtr and stops the walk.
 *
 * The native (entry hook on Command_InstancePlace) runs step 1 with the runner's functions (UpdateTree only when the
 * dirty list isn't empty: on an empty list it only rewrites the empty sentinel), then replaces steps 2-4 for a caller
 * whose SetPosition and Compute_BoundingBox(false) have no side effects other than the ones reproduced here: no
 * skeleton (+0x70, +0x74 null, sprite_index not a skeleton sprite, mask sprite not a skeleton mask), image_angle 0,
 * a collision sprite (mask_index, else sprite_index) without GetScaledBoundingBox's bitmap flag, no profiler, an
 * object selector 0..99999 and no result list. Then:
 *  - x/y unchanged by the probe (float compare, as SetPosition): the runner's steps 2 and 4 change nothing; the search
 *    uses the current box.
 *  - otherwise the probe x/y, probe box and flags (0x200 from the mask mode, flag 8 clear) are written to the caller
 *    for the search, exactly as the runner has them then. The probe box is Compute_BoundingBox's image_angle-0 path
 *    (f25a84..f25f30) from the sprite's local edges, origin and scale in its operation order (vmla is not fused on
 *    this VFP; this file is built with -ffp-contract=off), with g_Collision_Compatibility_Mode's lrintf (the runner's
 *    import) and -1. A box that isn't finite (YYError in the runner) takes the runner's path. After the search: the
 *    old x/y, the saved box, flag 8, and ONE CollisionMarkDirty(self). The runner marks twice (step 2 and step 4);
 *    CollisionMarkDirty moves the instance to the front of the dirty list (and the test list), so the second mark
 *    decides the final order, and nothing runs between the search and that second mark. A candidate marked during
 *    the search (only the runner's Collision_Instance on a skeleton can) stays behind the caller in both.
 *  - the search: the runner's RTree::Search with the native callback below (same tree, rectangle and count, so the
 *    entries are visited in the runner's order and the walk stops at the same entry), the globals and the tree lock
 *    byte as the runner sets them.
 * The native callback:
 *  - applies the callback's filters, answering IsDecendentOf from a cache keyed on (object index, selector): it reads
 *    only the object hash and the objects' parent links, which the game never changes after loading;
 *  - runs Collision_Instance's steps (f2b690..f2bacc) for candidates without a skeleton whose two collision sprites
 *    are both rectangle masks (mode 0; mode 3 without a skeleton takes the same path): flags & 0x100001, the four
 *    box comparisons with g_Collision_Compatibility_Mode's +1, Sprite_Data of mask_index (else sprite_index) with the
 *    +0x58 collision-data test, then true in compatibility mode, else the floorf(max min + 0.49999) !=
 *    floorf(min max + 0.5) rule on both axes. The comparisons are written so NaN goes the same way as the runner's
 *    branches (vcmp + bge/bls/beq/bne). Precise or rotated-rectangle masks (mode 1, 2) call the runner's
 *    Collision_Instance, and so does a candidate whose skeleton UseSkeletonCollision would look at (+0x70 without a
 *    mask) or whose mask sprite is missing or a skeleton mask (mode 3); a candidate without an object calls the
 *    runner's callback. Collision_Instance with false has no side effects on these paths (it only reads).
 * Every other call runs the runner's Command_InstancePlace, whose Search call (0xf65c80) goes through a veneer to the
 * same native callback when the query allows it. A fallback after step 1 is safe: the original repeats
 * ShouldUseFastCollision (its lazy insert is done, so it only answers), UpdateTree on an empty list and no box.
 * Install checks: the instruction words of every runner path reproduced here (hashes, with calls masked out:
 * patch_plt_calls.py retargets them) and of the callback/Collision_Instance (word list).
 *
 * Measured (Nov 25 2026, Vita in-run A/B, fights with enemies in view, saving per frame of the timed command):
 * bunker2_3 0.44, haunted_alien2 0.36, mansion_3 0.36, mansion_2_2 0.19, club_4_2 0.14 ms; chinatown_4 with smoke
 * bursts 0.10 +- 0.13 (noise). Native command ~7.5 us with its Search, vs ~5 us average for the runner's calls,
 * which include the ones that return at once. Verify: 0 mismatches over 1.96 M native commands in Vita3K (six rooms,
 * deaths, room changes) and 1.07 M on the Vita; the fault control is caught in each category.
 *
 * Switches (exp_*, read at boot): placenative0 (nothing installed); placecmd0 (only the callback, the research
 * version); placeverify (every native query runs both: the native, then the caller, dirty list, test list and
 * globals put back and the runner's Command_InstancePlace with the runner's callback; compares the result, the
 * caller's x/y, box, flags and both list links, the dirty and test lists' contents and order, the result/list/
 * selector globals, the tree lock byte, and every 64th query the whole R-tree (entries and rectangles); every
 * native narrow-phase answer is also compared with the runner's Collision_Instance; fallback queries compare the
 * native and runner callbacks inside the runner's Search; the game continues with the runner's state),
 * placeverifyfault (negative control: one native rectangle answer in 64 inverted, one native query in 64 without
 * its CollisionMarkDirty, one in 64 without flag 8), placeab (natives off in every other report window), placeprof
 * (time per Command_InstancePlace and per Search). Report lines [PLACENATIVE] (debugMode).
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#include "main.h"
#include "so_util.h"
#include "hooks.h"

#define ABI __attribute__((pcs("aapcs")))
#define U32(p, o) (*(uint32_t *)((char *)(p) + (o)))
#define I32(p, o) (*(int32_t *)((char *)(p) + (o)))
#define F32(p, o) (*(float *)((char *)(p) + (o)))
#define PTR(p, o) (*(char **)((char *)(p) + (o)))

typedef bool ABI (*cb_fn)(void *inst, void *ctx);
typedef int ABI (*search_fn)(void *tree, void *node, void *rect, int *count, cb_fn cb, void *ctx);
typedef bool ABI (*collide_fn)(void *inst, void *other, bool check);
typedef bool ABI (*desc_fn)(void *object, int index);
typedef void *ABI (*sprite_fn)(int index);
typedef void *ABI (*place_fn)(void *self, float x, float y, int sel, void *list);
typedef int ABI (*fast_fn)(int sel, void **object);
typedef void ABI (*void_fn)(void);
typedef void ABI (*bbox_fn)(void *self, bool update);
typedef void ABI (*mark_fn)(void *self);
typedef void *ABI (*tree_fn)(int domain);
typedef long ABI (*round_fn)(float);

enum {
	A_COMMAND = 0xf65aec,
	A_SEARCH_SITE = 0xf65c80, // bl RTree::Search in Command_InstancePlace
	A_SEARCH = 0xdd6224,
	A_SEARCH_VENEER = 0x19f3130,
	A_CALLBACK = 0xf65f80,
	A_COLLIDE = 0xf2b690,
};

int place_verify, place_ab, place_prof;
static int place_fault, installed, ab_off;
static search_fn orig_search;
static cb_fn place_cb;
static collide_fn collision_instance;
static desc_fn is_descendant;
static sprite_fn sprite_data;
static const volatile int *selector;
static void **result_ptr, **result_list;
static const uint8_t *profile_mode, *compat_mode;

unsigned pn_queries, pn_other, pn_cands, pn_rect, pn_runner_ci, pn_runner_cb;
unsigned pnv_queries, pnv_bad, pnv_ci, pnv_ci_bad;
static uint32_t prof_us, prof_n;

// The whole command (Command_InstancePlace)
static int cmd_on;          // installed
static int force_runner;    // verify reference run: the runner's callback in place_search
static place_fn orig_place; // trampoline
static fast_fn should_use_fast;
static void_fn update_tree;
static bbox_fn compute_bbox;
static mark_fn mark_dirty;
static tree_fn tree_for_domain;
static round_fn runner_lrintf;
static void **run_room;
static volatile int *selector_w;
static uint32_t *dirty_list, *test_list; // sentinels {first, last, node offset}
unsigned pc_native, pc_still, pc_runner, pc_late, pc_empty; // per window: native (moved / x,y unchanged), runner, runner after step 1
// runner reasons: no room / instance kind, selector not an object, result list, A/B off, caller sprite, not the tree
static unsigned pc_why[6];
static uint32_t pc_us[2], pc_n[2];                 // exp_placeprof: [native path?]
// verify
unsigned pcv_queries, pcv_skipped, pcv_bad, pcv_tree, pcv_bad_kind[8];
static unsigned pcv_n;

// IsDecendentOf(object, selector) by (object index, selector)
#define DSLOTS 4096
static struct { int32_t idx, sel; int32_t val; } desc_cache[DSLOTS];
static int descends(void *obj, int sel) {
	int idx = I32(obj, 0x64);
	unsigned h = ((uint32_t)idx * 2654435761u ^ (uint32_t)sel * 40503u) & (DSLOTS - 1);
	if (desc_cache[h].idx == idx && desc_cache[h].sel == sel)
		return desc_cache[h].val;
	int v = is_descendant(obj, sel) ? 1 : 0;
	desc_cache[h].idx = idx;
	desc_cache[h].sel = sel;
	desc_cache[h].val = v;
	return v;
}

// The per-query state the native callback reads (Search is not re-entered: the callbacks call no GML)
static struct {
	void *self;
	int sel;
	void *self_sprite; // Sprite_Data of the caller's mask (else sprite) index
	float c;           // 1 in compatibility mode, else 0
	int verify;
	float probe[4];    // the native command's probe box
} q;

// Collision_Instance(cand, self, false) for two rectangle masks; -1: call the runner's
static int narrow(void *cand) {
	void *self = q.self;
	if ((U32(cand, 0x7c) | U32(self, 0x7c)) & 0x100001)
		return 0;
	int mask = I32(cand, 0x14c);
	void *sp;
	if (mask >= 0) {
		sp = sprite_data(mask);
		if (!sp)
			return -1;
		if (I32(sp, 0x8c) == 3)
			return -1; // UseSkeletonCollision
	} else {
		if (U32(cand, 0x70))
			return -1;
		sp = NULL;
	}
	float c = q.c;
	// f2b7e8..f2b854: other = self, this = cand
	if (F32(self, 0xec) >= F32(cand, 0xf4) + c)
		return 0;
	if (c + F32(self, 0xf4) <= F32(cand, 0xec))
		return 0;
	if (F32(self, 0xf0) >= c + F32(cand, 0xf8))
		return 0;
	if (c + F32(self, 0xf8) <= F32(cand, 0xf0))
		return 0;
	if (mask < 0)
		sp = sprite_data(I32(cand, 0x88));
	if (!sp || !U32(sp, 0x58))
		return 0;
	void *ss = q.self_sprite;
	if (!ss || !U32(ss, 0x58))
		return 0;
	int m1 = I32(sp, 0x8c), m2 = I32(ss, 0x8c);
	if (m1 == 1 || m1 == 2 || m2 == 1 || m2 == 2)
		return -1; // precise / separating-axis
	pn_rect++;
	if (c != 0.0f)
		return 1;
	// f2ba14..f2bac8
	const float k = 0.49998998641967773f; // 0x3efffeb0
	float l = F32(cand, 0xec), r = F32(cand, 0xf4), ol = F32(self, 0xec), orr = F32(self, 0xf4);
	if (ol > l)
		l = ol;
	if (orr < r)
		r = orr;
	volatile float a = l + k, b = r + 0.5f;
	if (floorf(a) == floorf(b))
		return 0;
	float t = F32(cand, 0xf0), bo = F32(cand, 0xf8), ot = F32(self, 0xf0), ob = F32(self, 0xf8);
	if (ot > t)
		t = ot;
	if (ob < bo)
		bo = ob;
	volatile float a2 = t + k, b2 = bo + 0.5f;
	return floorf(a2) != floorf(b2);
}

static bool ABI native_cb(void *cand, void *ctx) {
	void *self = ctx;
	// the callback's filters (f65f90..f660e0)
	if (cand == self || (U32(cand, 0x7c) & 0x100003) || I32(cand, 0x50) != 1)
		return true;
	void *obj = PTR(cand, 0x68);
	if (!obj) {
		pn_runner_cb++;
		return place_cb(cand, ctx);
	}
	if (I32(obj, 0x64) != q.sel && !descends(obj, q.sel))
		return true;
	pn_cands++;
	int hit = narrow(cand);
	if (hit < 0) {
		pn_runner_ci++;
		hit = collision_instance(cand, self, false);
	} else if (q.verify) {
		pnv_ci++;
		static unsigned fault_n;
		if (place_fault && ++fault_n % 64 == 0)
			hit = !hit;
		if (hit != (int)collision_instance(cand, self, false)) {
			if (pnv_ci_bad < 8)
				debugPrintf("[PLACENATIVE] verify: narrow phase %d, runner %d (object %d, caller object %d)\n", hit, !hit,
					I32(obj, 0x64), PTR(self, 0x68) ? I32(PTR(self, 0x68), 0x64) : -1);
			pnv_ci_bad++;
		}
	}
	if (!hit)
		return true;
	*result_ptr = cand; // f6610c
	return false;
}

static int ABI place_search(void *tree, void *node, void *rect, int *count, cb_fn cb, void *self) {
	int sel = *selector;
	uint32_t t0 = place_prof ? sceKernelGetProcessTimeLow() : 0;
	int r;
	if (cb != place_cb || sel < 0 || sel > 99999 || *result_list || *profile_mode || U32(self, 0x70) ||
			U32(self, 0x74) || (ab_off && !place_verify) || force_runner) {
		if (cb == place_cb)
			pn_other++;
		r = orig_search(tree, node, rect, count, cb, self);
	} else {
		int mask = I32(self, 0x14c);
		void *ss = sprite_data(mask >= 0 ? mask : I32(self, 0x88));
		if (mask >= 0 && (!ss || I32(ss, 0x8c) == 3)) { // UseSkeletonCollision(self) / its null sprite
			pn_other++;
			r = orig_search(tree, node, rect, count, cb, self);
		} else {
			pn_queries++;
			q.self = self;
			q.sel = sel;
			q.self_sprite = ss;
			q.c = *compat_mode ? 1.0f : 0.0f;
			q.verify = place_verify;
			if (!place_verify)
				r = orig_search(tree, node, rect, count, native_cb, self);
			else {
				int n1 = *count;
				int r1 = orig_search(tree, node, rect, &n1, native_cb, self);
				void *res1 = *result_ptr;
				*result_ptr = NULL; // Command_InstancePlace cleared it before the call
				r = orig_search(tree, node, rect, count, cb, self);
				pnv_queries++;
				if (res1 != *result_ptr || r1 != r || n1 != *count) {
					if (pnv_bad < 8)
						debugPrintf("[PLACENATIVE] verify: native %p/%d/%d, runner %p/%d/%d (selector %d)\n", res1, r1, n1,
							*result_ptr, r, *count, sel);
					pnv_bad++;
				}
			}
		}
	}
	if (place_prof) {
		prof_us += sceKernelGetProcessTimeLow() - t0;
		prof_n++;
	}
	return r;
}

// --- The whole command ---

// Compute_BoundingBox's image_angle-0 path (f25a84..f25f30) for the caller at (px, py): box {left, top, right,
// bottom}. 0: not finite (the runner YYErrors), take the runner's path.
static int probe_box(void *self, void *sp, float px, float py, float *box) {
	float l = F32(sp, 0x48), t = F32(sp, 0x4c), r = F32(sp, 0x50), b = F32(sp, 0x54);
	float sx = F32(self, 0xa4), sy = F32(self, 0xa8);
	float w = (r - l) + 1.0f, h = (b - t) + 1.0f;
	float left = px + sx * (l - (float)I32(sp, 0x64));
	float right = left + sx * w;
	int compat = *compat_mode;
	if (compat) {
		left = (float)runner_lrintf(left);
		right = (float)runner_lrintf(right);
	}
	float maxx;
	if (left > right) { // vstrgt / vmovle / vstrgt
		box[0] = right;
		box[2] = maxx = left;
	} else {
		box[0] = left;
		box[2] = maxx = right;
	}
	float top = py + sy * (t - (float)I32(sp, 0x68));
	float bottom = top + sy * h;
	if (compat) {
		top = (float)runner_lrintf(top);
		bottom = (float)runner_lrintf(bottom);
	}
	float maxy;
	if (top > bottom) {
		box[1] = bottom;
		box[3] = maxy = top;
	} else {
		box[1] = top;
		box[3] = maxy = bottom;
	}
	if (compat) {
		box[2] = maxx + -1.0f;
		box[3] = maxy + -1.0f;
	}
	// f25f28..f25f9c: NaN in top, bottom or left, or an infinite edge -> YYError
	for (int i = 0; i < 4; i++)
		if (!isfinite(box[i]))
			return 0;
	return 1;
}

// The caller's collision sprite when steps 2-4 can be native (see the header), else NULL
static void *native_sprite(void *self) {
	if (*profile_mode || U32(self, 0x70) || U32(self, 0x74))
		return NULL;
	void *ds = sprite_data(I32(self, 0x88));
	if (ds && I32(ds, 0x84) == 2) // SkeletonAnimation would create a skeleton
		return NULL;
	int mask = I32(self, 0x14c);
	void *sp = ds;
	if (mask >= 0) {
		sp = sprite_data(mask);
		if (!sp || I32(sp, 0x8c) == 3) // MaskCollisionSkeleton would create one
			return NULL;
	}
	if (!sp || F32(self, 0xac) != 0.0f) // point box / rotated path (NaN angle is rotated in the runner too)
		return NULL;
	const uint8_t *bitmap = (const uint8_t *)PTR(sp, 0x38);
	if (bitmap && bitmap[0x6c]) // GetScaledBoundingBox
		return NULL;
	return sp;
}

static unsigned fault_cmd;

// Steps 2-4 natively; returns s_instancePtr
static void *native_steps(void *self, float px, float py, int sel, void *sp) {
	float x = F32(self, 0xb8), y = F32(self, 0xbc);
	uint32_t saved[4];
	memcpy(saved, (char *)self + 0xec, 16);
	int moved = !(x == px && y == py);
	if (moved) {
		F32(self, 0xb8) = px;
		F32(self, 0xbc) = py;
		memcpy((char *)self + 0xec, q.probe, 16);
		U32(self, 0x7c) = (U32(self, 0x7c) & ~(8u | 0x200u)) | (I32(sp, 0x8c) == 1 ? 0x200u : 0u);
	}
	float l = F32(self, 0xec), t = F32(self, 0xf0), r = F32(self, 0xf4), b = F32(self, 0xf8);
	// f65bd8..f65c3c: max with vmovgt, min with vmovmi (NaN keeps right / bottom)
	float rect[4] = { l < r ? l : r, t < b ? t : b, l > r ? l : r, t > b ? t : b };
	*result_ptr = NULL;
	*selector_w = sel;
	*result_list = NULL;
	uint32_t f = U32(self, 0x7c);
	char *tree = tree_for_domain((f & 0x4000000) ? 2 : (int)((f >> 25) & 1));
	tree[4] = 1;
	int count = 0;
	q.self = self;
	q.sel = sel;
	q.self_sprite = sp;
	q.c = *compat_mode ? 1.0f : 0.0f;
	q.verify = place_verify && !force_runner;
	orig_search(tree, PTR(tree, 0x14), rect, &count, native_cb, self);
	tree[4] = 0;
	*result_list = NULL;
	if (moved) {
		F32(self, 0xb8) = x;
		F32(self, 0xbc) = y;
		unsigned n = place_fault && q.verify ? ++fault_cmd % 64 : 1;
		if (n != 32)
			U32(self, 0x7c) |= 8;
		if (n != 0)
			mark_dirty(self);
		memcpy((char *)self + 0xec, saved, 16);
		pc_native++;
	} else
		pc_still++;
	return *result_ptr;
}

// Verify: state a query can change after step 1
#define MAXL 1024
typedef struct {
	uint32_t self[11]; // 0x7c, 0xb8, 0xbc, 0xec..0xf8, 0x15c..0x164 (test links), 0x168..0x170 (dirty links) below
	uint32_t links[6];
	void *res, *list;
	int sel;
	uint8_t lock;
	int nd, nt;
	void *dirty[MAXL], *test[MAXL];
} Snap;
static Snap s0, sa, sb;
static uint32_t test_words[MAXL][3];

static int walk(uint32_t *sentinel, void **out) {
	int n = 0;
	for (uint32_t *p = (uint32_t *)sentinel[0]; p != sentinel; p = (uint32_t *)p[0]) {
		if (!p || n == MAXL)
			return -1;
		out[n++] = p;
	}
	return n;
}
static void snap(Snap *s, void *self, char *tree) {
	s->self[0] = U32(self, 0x7c);
	s->self[1] = U32(self, 0xb8);
	s->self[2] = U32(self, 0xbc);
	memcpy(&s->self[3], (char *)self + 0xec, 16);
	memcpy(s->links, (char *)self + 0x15c, 24);
	s->res = *result_ptr;
	s->list = *result_list;
	s->sel = *selector_w;
	s->lock = tree ? (uint8_t)tree[4] : 0;
	s->nd = walk(dirty_list, s->dirty);
	s->nt = walk(test_list, s->test);
}
// FNV-1a of the tree's leaf entries (instance and rectangle) in Search's visiting order
static uint32_t tree_hash(const uint32_t *node, uint32_t h, int depth) {
	int count = (int)node[0], level = (int)node[1];
	if (count < 0 || count > 6 || depth > 24)
		return h ^ 0xdeadbeef;
	for (int i = 0; i < count; i++) {
		const uint32_t *br = node + 2 + i * 6;
		if (level > 0)
			h = tree_hash((const uint32_t *)br[4], h, depth + 1);
		else
			for (int k = 0; k < 6; k++)
				if (k != 4)
					h = (h ^ br[k]) * 0x01000193u;
	}
	return h;
}
static void list_reset(uint32_t *sentinel, void **nodes, int n) {
	for (int i = 0; i < n; i++) {
		uint32_t *p = nodes[i];
		p[0] = p[1] = (uint32_t)p;
		p[2] = 0;
	}
	sentinel[0] = sentinel[1] = (uint32_t)sentinel;
}
static void report_bad(int kind, const char *what, void *self, int sel) {
	pcv_bad_kind[kind]++;
	if (pcv_bad < 12)
		debugPrintf("[PLACENATIVE] verify: %s differs (caller object %d, selector %d)\n", what,
			PTR(self, 0x68) ? I32(PTR(self, 0x68), 0x64) : -1, sel);
}

// The native and the runner's steps 2-4 from the same state; the runner's state stays
static void *verify_steps(void *self, float px, float py, int sel, void *list, void *sp) {
	uint32_t f = U32(self, 0x7c);
	char *tree = tree_for_domain((f & 0x4000000) ? 2 : (int)((f >> 25) & 1));
	snap(&s0, self, tree);
	if (s0.nd != 0 || s0.nt < 0) { // after UpdateTree the dirty list is empty
		pcv_skipped++;
		force_runner = 1;
		void *r = orig_place(self, px, py, sel, list);
		force_runner = 0;
		return r;
	}
	for (int i = 0; i < s0.nt; i++)
		memcpy(test_words[i], s0.test[i], 12);
	uint32_t test_sent[2] = { test_list[0], test_list[1] };
	int full = ++pcv_n % 64 == 0;
	uint32_t h0 = full ? tree_hash((const uint32_t *)PTR(tree, 0x14), 0x811c9dc5u, 0) : 0;
	void *ra = native_steps(self, px, py, sel, sp);
	snap(&sa, self, tree);
	uint32_t ha = full ? tree_hash((const uint32_t *)PTR(tree, 0x14), 0x811c9dc5u, 0) : 0;
	// back to s0: lists, caller, globals
	if (sa.nd > 0)
		list_reset(dirty_list, sa.dirty, sa.nd);
	if (sa.nt > 0)
		list_reset(test_list, sa.test, sa.nt);
	for (int i = 0; i < s0.nt; i++)
		memcpy(s0.test[i], test_words[i], 12);
	test_list[0] = test_sent[0];
	test_list[1] = test_sent[1];
	U32(self, 0x7c) = s0.self[0];
	U32(self, 0xb8) = s0.self[1];
	U32(self, 0xbc) = s0.self[2];
	memcpy((char *)self + 0xec, &s0.self[3], 16);
	memcpy((char *)self + 0x15c, s0.links, 24);
	*result_ptr = s0.res;
	*result_list = s0.list;
	*selector_w = s0.sel;
	tree[4] = s0.lock;
	// the runner's whole command (step 1 repeats as no-ops) with the runner's callback
	force_runner = 1;
	void *rb = orig_place(self, px, py, sel, list);
	force_runner = 0;
	snap(&sb, self, tree);
	uint32_t hb = full ? tree_hash((const uint32_t *)PTR(tree, 0x14), 0x811c9dc5u, 0) : 0;
	pcv_queries++;
	unsigned bad0 = pcv_bad;
	if (ra != rb || sa.res != sb.res)
		report_bad(0, "result", self, sel), pcv_bad++;
	if (memcmp(sa.self, sb.self, sizeof(sa.self)))
		report_bad(1, "caller x/y/box/flags", self, sel), pcv_bad++;
	if (memcmp(sa.links, sb.links, sizeof(sa.links)))
		report_bad(2, "caller list links", self, sel), pcv_bad++;
	if (sa.nd != sb.nd || (sa.nd > 0 && memcmp(sa.dirty, sb.dirty, sa.nd * sizeof(void *))))
		report_bad(3, "dirty list", self, sel), pcv_bad++;
	if (sa.nt != sb.nt || (sa.nt > 0 && memcmp(sa.test, sb.test, sa.nt * sizeof(void *))))
		report_bad(4, "test list", self, sel), pcv_bad++;
	if (sa.list != sb.list || sa.sel != sb.sel || sa.lock != sb.lock)
		report_bad(5, "globals", self, sel), pcv_bad++;
	if (full) {
		pcv_tree++;
		if (h0 != ha || ha != hb)
			report_bad(6, "R-tree", self, sel), pcv_bad++;
	}
	if (pcv_bad != bad0 && pcv_bad <= 12)
		debugPrintf("[PLACENATIVE] verify: native %p runner %p, flags %08x/%08x, dirty %d/%d, test %d/%d\n", ra, rb,
			(unsigned)sa.self[0], (unsigned)sb.self[0], sa.nd, sb.nd, sa.nt, sb.nt);
	return rb;
}

static void *ABI place_command(void *self, float px, float py, int sel, void *list) {
	uint32_t t0 = place_prof ? sceKernelGetProcessTimeLow() : 0;
	void *r;
	int native = 0;
	void *sp = NULL;
	int why = !*run_room || I32(self, 0x50) != 1 ? 0 : sel < 0 || sel > 99999 ? 1 : list ? 2 :
		(ab_off && !place_verify) ? 3 : !(sp = native_sprite(self)) ? 4 : -1;
	if (why >= 0) {
		pc_runner++;
		pc_why[why]++;
		r = orig_place(self, px, py, sel, list);
	} else {
		void *object = NULL;
		int how = should_use_fast(sel, &object);
		if (how == -1) { // no instance of the selector: the runner returns at once
			pc_empty++;
			r = NULL;
		}
		else if (how != 1) {
			pc_runner++;
			pc_why[5]++;
			r = orig_place(self, px, py, sel, list); // ShouldUseFastCollision again: no lazy insert on this path
		} else {
			if (dirty_list[0] != (uint32_t)dirty_list)
				update_tree();
			if (U32(self, 0x7c) & 8)
				compute_bbox(self, true);
			int still = F32(self, 0xb8) == px && F32(self, 0xbc) == py;
			if (!still && !probe_box(self, sp, px, py, q.probe)) {
				pc_late++;
				r = orig_place(self, px, py, sel, list);
			} else {
				native = 1;
				r = place_verify ? verify_steps(self, px, py, sel, list, sp) : native_steps(self, px, py, sel, sp);
			}
		}
	}
	if (place_prof) {
		pc_us[native] += sceKernelGetProcessTimeLow() - t0;
		pc_n[native]++;
	}
	return r;
}

void place_native_report(int frames) {
	if (!installed || frames <= 0)
		return;
	float f = (float)frames;
	debugPrintf("[PLACENATIVE] native queries %.1f/frame (candidates %.1f: rectangle %.1f, runner test %.1f, runner "
		"callback %.1f), other %.1f | A/B %s\n",
		pn_queries / f, pn_cands / f, pn_rect / f, pn_runner_ci / f, pn_runner_cb / f, pn_other / f,
		!place_ab ? "-" : ab_off ? "off" : "on");
	if (cmd_on)
		debugPrintf("[PLACENATIVE] command: native %.1f/frame (moved %.1f, x/y unchanged %.1f), runner %.1f (room %.1f, "
			"selector %.1f, list %.1f, A/B %.1f, caller sprite %.1f, not tree %.1f), runner after step 1 %.1f, no instance "
			"%.1f | A/B %s\n",
			(pc_native + pc_still) / f, pc_native / f, pc_still / f, pc_runner / f, pc_why[0] / f, pc_why[1] / f,
			pc_why[2] / f, pc_why[3] / f, pc_why[4] / f, pc_why[5] / f, pc_late / f, pc_empty / f,
			!place_ab ? "-" : ab_off ? "off" : "on");
	memset(pc_why, 0, sizeof(pc_why));
	if (place_verify) {
		debugPrintf("[PLACENATIVE] verify: %u queries (%u mismatches), %u narrow-phase answers (%u mismatches) "
			"(session)\n", pnv_queries, pnv_bad, pnv_ci, pnv_ci_bad);
		if (cmd_on)
			debugPrintf("[PLACENATIVE] verify command: %u queries (%u mismatches: result %u, caller %u, links %u, dirty "
				"list %u, test list %u, globals %u, R-tree %u of %u), %u skipped (session)\n", pcv_queries, pcv_bad,
				pcv_bad_kind[0], pcv_bad_kind[1], pcv_bad_kind[2], pcv_bad_kind[3], pcv_bad_kind[4], pcv_bad_kind[5],
				pcv_bad_kind[6], pcv_tree, pcv_skipped);
	}
	if (place_prof) {
		debugPrintf("[PLACENATIVE] time: Search %.3f ms/frame (%.2f us/query, %.1f queries/frame)\n",
			prof_us / 1000.0f / f, prof_n ? (float)prof_us / prof_n : 0.0f, prof_n / f);
		debugPrintf("[PLACENATIVE] time: command %.3f ms/frame (native %.2f us x %.1f, runner %.2f us x %.1f)\n",
			(pc_us[0] + pc_us[1]) / 1000.0f / f, pc_n[1] ? (float)pc_us[1] / pc_n[1] : 0.0f, pc_n[1] / f,
			pc_n[0] ? (float)pc_us[0] / pc_n[0] : 0.0f, pc_n[0] / f);
	}
	pn_queries = pn_other = pn_cands = pn_rect = pn_runner_ci = pn_runner_cb = 0;
	pc_native = pc_still = pc_runner = pc_late = pc_empty = 0;
	pc_us[0] = pc_us[1] = pc_n[0] = pc_n[1] = 0;
	prof_us = prof_n = 0;
	if (place_ab)
		ab_off = !ab_off;
}

#define SYM(name) so_symbol(&yoyoloader_mod, name)

// FNV-1a over code words, AL b/bl that leave the range masked out (patch_plt_calls.py and veneers retarget them);
// work/placecmd/codehash.py computes the expected values from the ELF
static uint32_t code_hash(uintptr_t start, uintptr_t end) {
	uint32_t h = 0x811c9dc5u;
	for (uintptr_t a = start; a < end; a += 4) {
		uint32_t w = U32(a, 0);
		if ((w >> 24) == 0xea || (w >> 24) == 0xeb) {
			uintptr_t to = a + 8 + (uintptr_t)((int32_t)(w << 8) >> 6);
			if (to < start || to >= end)
				continue;
		}
		h = (h ^ w) * 0x01000193u;
	}
	return h;
}

static int command_install(uintptr_t bias) {
	if (exp_flag("placecmd0")) {
		return -1;
	}
	// Command_InstancePlace's R-tree path, SetPosition, SkeletonAnimation, MaskCollisionSkeleton's no-skeleton paths,
	// Compute_BoundingBox's head, image_angle-0 path and tail
	static const uint32_t ranges[][3] = {
		{ 0xf65af4, 0xf65cc0, 0x4742ccc0 }, { 0xf297d4, 0xf29840, 0x9544e678 }, { 0xf2673c, 0xf267d0, 0xef41355d },
		{ 0xf2610c, 0xf2613c, 0xc55b56fc }, { 0xf26168, 0xf26180, 0x377638f6 }, { 0xf261b4, 0xf261d4, 0x3e9c72e0 },
		{ 0xf25898, 0xf25a58, 0x17096c3f }, { 0xf25a58, 0xf25bc4, 0x0c348173 }, { 0xf25f0c, 0xf260a8, 0xb65e0022 },
	};
	for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
		if (code_hash(bias + ranges[i][0], bias + ranges[i][1]) != ranges[i][2]) {
			debugPrintf("native place_meeting command: code at 0x%08X differs\n", (unsigned)ranges[i][0]);
			return 0;
		}
	uintptr_t set_position = SYM("_ZN9CInstance11SetPositionEff");
	should_use_fast = (fast_fn)SYM("_Z22ShouldUseFastCollisioniPP9CObjectGM");
	update_tree = (void_fn)SYM("_Z10UpdateTreev");
	compute_bbox = (bbox_fn)SYM("_ZN9CInstance19Compute_BoundingBoxEb");
	mark_dirty = (mark_fn)SYM("_Z18CollisionMarkDirtyP9CInstance");
	tree_for_domain = (tree_fn)SYM("_Z25GetCollisionTreeForDomain15CollisionDomain");
	run_room = (void **)SYM("Run_Room");
	dirty_list = (uint32_t *)SYM("s_dirty_list");
	test_list = (uint32_t *)SYM("s_test_list");
	uintptr_t *round_slot = so_got_slot(&yoyoloader_mod, "lrintf");
	if (set_position != bias + 0xf297d4 || (uintptr_t)should_use_fast != bias + 0xf642b8 ||
			(uintptr_t)update_tree != bias + 0xf6ca58 || (uintptr_t)compute_bbox != bias + 0xf2587c ||
			(uintptr_t)mark_dirty != bias + 0xf6bf40 || (uintptr_t)tree_for_domain != bias + 0xf6c25c || !run_room ||
			!dirty_list || !test_list || !round_slot || !*round_slot) {
		debugPrintf("native place_meeting command: symbols\n");
		return 0;
	}
	runner_lrintf = (round_fn)*round_slot;
	selector_w = (volatile int *)selector;
	orig_place = (place_fn)entry_hook(bias + A_COMMAND, (void *)place_command);
	if (!orig_place)
		return 0;
	cmd_on = 1;
	return 1;
}

void place_native_install(void) {
	if (exp_flag("placenative0")) {
		debugPrintf("native place_meeting callback: off (exp_placenative0)\n");
		return;
	}
	uintptr_t command = SYM("_Z21Command_InstancePlaceP9CInstanceffiP8CDS_List");
	uintptr_t bias = command - A_COMMAND;
	uintptr_t search = SYM("_ZN5RTreeIP9CInstanceffLi6ELi2EE6SearchEPNS2_4NodeEPNS2_4RectERiPFbS1_PvES8_");
	collision_instance = (collide_fn)SYM("_ZN9CInstance18Collision_InstanceEPS_b");
	is_descendant = (desc_fn)SYM("_ZN9CObjectGM13IsDecendentOfEi");
	sprite_data = (sprite_fn)SYM("_Z11Sprite_Datai");
	result_ptr = (void **)SYM("s_instancePtr");
	result_list = (void **)SYM("s_instanceList");
	profile_mode = (const uint8_t *)SYM("g_bProfile");
	compat_mode = (const uint8_t *)SYM("g_Collision_Compatibility_Mode");
	if (!command || search != bias + A_SEARCH || (uintptr_t)collision_instance != bias + A_COLLIDE ||
			!is_descendant || !sprite_data || !result_ptr || !result_list || !profile_mode || !compat_mode) {
		debugPrintf("native place_meeting callback: NOT installed (symbols)\n");
		return;
	}
#define AT(a) (bias + (a))
#define LIT(a) U32(AT(a), 0)
	// Command_InstancePlace: selector store, result/list GOT loads, callback address, stm sp, {r0, r9} (callback,
	// self) and mov r0, r4 (tree) before the call. The callback: filters, selector load, result store.
	// Collision_Instance: profiler and compatibility-mode loads, the box tests, Sprite_Data selections, mode tests,
	// the rectangle rule (all compared word for word below; its calls aren't: patch_plt_calls.py retargets them).
	static const uint32_t expect[][2] = {
		{ 0xf65aec, 0xe92d4ff0 }, { 0xf65bf8, 0xe59f0364 }, { 0xf65c04, 0xe08f0000 }, { 0xf65c20, 0xe5804000 },
		{ 0xf65c10, 0xe79f8008 }, { 0xf65c1c, 0xe79f6006 }, { 0xf65c6c, 0xe59f02fc }, { 0xf65c74, 0xe08f0000 },
		{ 0xf65c78, 0xe88d0201 }, { 0xf65c7c, 0xe1a00004 }, { 0xdd6224, 0xe92d4ff0 },
		{ 0xf65f80, 0xe92d4830 }, { 0xf65f90, 0xe1510000 }, { 0xf65fac, 0xe590307c }, { 0xf65fb0, 0xe3005003 },
		{ 0xf65fb4, 0xe3405010 }, { 0xf65fbc, 0x05903050 }, { 0xf65fc0, 0x03530001 }, { 0xf65ff0, 0xe59f3120 },
		{ 0xf65ff4, 0xe79f3003 }, { 0xf65ff8, 0xe3730003 }, { 0xf66050, 0xe308569f }, { 0xf66054, 0xe3405001 },
		{ 0xf66060, 0xe3730001 }, { 0xf66064, 0xca00000f }, { 0xf660a8, 0xe5902068 }, { 0xf660ac, 0xe5925064 },
		{ 0xf66000, 0xe3a02000 }, { 0xf66014, 0xe59f010c }, { 0xf66024, 0x0a000035 }, { 0xf6610c, 0xe5804000 },
		{ 0xf2b690, 0xe92d4ff0 }, { 0xf2b6fc, 0xe3000001 }, { 0xf2b700, 0xe3400010 }, { 0xf2b7a0, 0xe3570000 },
		{ 0xf2b7dc, 0xeeb70a00 }, { 0xf2b7e8, 0xed952a3b }, { 0xf2b7f4, 0x0eb00a41 }, { 0xf2b7f8, 0xed961a3d },
		{ 0xf2b7fc, 0xee311a00 }, { 0xf2b800, 0xeeb42a41 }, { 0xf2b808, 0xaaffffc1 }, { 0xf2b80c, 0xed951a3d },
		{ 0xf2b814, 0xee301a01 }, { 0xf2b818, 0xed972a00 }, { 0xf2b81c, 0xeeb41a42 }, { 0xf2b824, 0x9affffba },
		{ 0xf2b828, 0xed961a3e }, { 0xf2b82c, 0xed952a3c }, { 0xf2b830, 0xee301a01 }, { 0xf2b834, 0xeeb42a41 },
		{ 0xf2b83c, 0xaaffffb4 }, { 0xf2b840, 0xed951a3e }, { 0xf2b844, 0xee300a01 }, { 0xf2b848, 0xed961a3c },
		{ 0xf2b84c, 0xeeb40a41 }, { 0xf2b854, 0x9affffae }, { 0xf2b858, 0xe5960088 }, { 0xf2b85c, 0xe596114c },
		{ 0xf2b864, 0x51a00001 }, { 0xf2b874, 0x15980058 }, { 0xf2b880, 0xe595014c }, { 0xf2b884, 0xe3700001 },
		{ 0xf2b888, 0xd5950088 }, { 0xf2b898, 0x15900058 }, { 0xf2b8a4, 0xe598008c }, { 0xf2b8a8, 0xe3500002 },
		{ 0xf2b8b4, 0xe591108c }, { 0xf2b8b8, 0xe3510002 }, { 0xf2b8e4, 0xe3500001 }, { 0xf2b8e8, 0x13510001 },
		{ 0xf2b914, 0xe590008c }, { 0xf2b918, 0xe3500001 }, { 0xf2b9bc, 0xe3500000 }, { 0xf2b9c4, 0xe3a04001 },
		{ 0xf2ba14, 0xed950a3b }, { 0xf2ba18, 0xed962a3b }, { 0xf2ba1c, 0xed9f8a31 }, { 0xf2ba20, 0xeeb40a42 },
		{ 0xf2ba24, 0xed951a3d }, { 0xf2ba2c, 0xed96aa3d }, { 0xf2ba30, 0xeeb41a4a }, { 0xf2ba34, 0xceb02a40 },
		{ 0xf2ba3c, 0xee320a08 }, { 0xf2ba44, 0x4eb0aa41 }, { 0xf2ba4c, 0xeeb69a00 }, { 0xf2ba54, 0xee3a0a09 },
		{ 0xf2ba68, 0xeeb41a40 }, { 0xf2ba70, 0x0affff27 }, { 0xf2ba74, 0xed950a3c }, { 0xf2ba78, 0xed962a3c },
		{ 0xf2ba7c, 0xed951a3e }, { 0xf2ba84, 0xed96aa3e }, { 0xf2ba90, 0xceb02a40 }, { 0xf2ba98, 0xee320a08 },
		{ 0xf2baa0, 0x4eb0aa41 }, { 0xf2baa8, 0xee3a0a09 }, { 0xf2bac0, 0xeeb41a40 }, { 0xf2bac8, 0x13a04001 },
		{ 0xf2bae4, 0x00000000 }, { 0xf2bae8, 0x3efffeb0 },
		// UseSkeletonCollision: mask_index > -1 -> Sprite_Data(mask)->mode == 3; else +0x70 == 0 -> false
		{ 0xf26404, 0xe590014c }, { 0xf26408, 0xe3700001 }, { 0xf26414, 0xe590008c }, { 0xf26418, 0xe2400003 },
		{ 0xf26428, 0xe5941070 }, { 0xf2642c, 0xe3a00000 }, { 0xf26430, 0xe3510000 }, { 0xf26434, 0x0a000016 },
	};
	for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++)
		if (U32(AT(expect[i][0]), 0) != expect[i][1]) {
			debugPrintf("native place_meeting callback: NOT installed (code at 0x%08X)\n", expect[i][0]);
			return;
		}
	uintptr_t sel_cmd = AT(0xf65c0c) + LIT(0xf65f64), sel_cb = AT(0xf65ffc) + LIT(0xf66118);
	uintptr_t cb = AT(0xf65c7c) + LIT(0xf65f70);
	uintptr_t res_slot = AT(0xf65c18) + LIT(0xf65f68), list_slot = AT(0xf65c24) + LIT(0xf65f6c);
	uintptr_t cb_res_slot = AT(0xf66110) + LIT(0xf6612c);
	uintptr_t prof_slot = AT(0xf2b6d0) + LIT(0xf2baf0), compat_slot = AT(0xf2b7ec) + LIT(0xf2baf8);
	if (sel_cmd != sel_cb || cb != AT(A_CALLBACK) || U32(res_slot, 0) != (uintptr_t)result_ptr ||
			U32(cb_res_slot, 0) != (uintptr_t)result_ptr || U32(list_slot, 0) != (uintptr_t)result_list ||
			U32(prof_slot, 0) != (uintptr_t)profile_mode || U32(compat_slot, 0) != (uintptr_t)compat_mode) {
		debugPrintf("native place_meeting callback: NOT installed (globals)\n");
		return;
	}
	selector = (const volatile int *)sel_cmd;
	place_cb = (cb_fn)cb;
	orig_search = (search_fn)search;
	memset(desc_cache, 0xff, sizeof(desc_cache)); // idx -1 never matches an object
	place_verify = exp_flag("placeverify");
	place_fault = exp_flag("placeverifyfault");
	place_ab = exp_flag("placeab");
	place_prof = exp_flag("placeprof");
	uintptr_t site = AT(A_SEARCH_SITE);
	uint32_t ins = U32(site, 0);
	uintptr_t to = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	uint32_t *veneer = (ins >> 24) == 0xeb && (to == AT(A_SEARCH_VENEER) || to == search)
		? (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8) : NULL;
	int64_t delta = veneer ? (int64_t)(uintptr_t)veneer - (int64_t)site - 8 : 0;
	if (!veneer || delta < -(1 << 25) || delta >= (1 << 25)) {
		debugPrintf("native place_meeting callback: NOT installed (call site)\n");
		return;
	}
	uint32_t code[2] = { 0xe51ff004, (uint32_t)place_search }; // ldr pc, [pc, #-4]; .word place_search
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t branch = 0xeb000000 | (((uint32_t)delta >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &branch, 4);
	kuKernelFlushCaches((void *)site, 4);
	installed = 1;
	debugPrintf("native place_meeting callback: installed%s%s%s\n", place_verify ? " (verify)" : "",
		place_fault ? " (verify fault control)" : "", place_ab ? " (A/B)" : "");
	int cmd = command_install(bias);
	debugPrintf("native place_meeting command: %s%s\n", cmd > 0 ? "installed" : cmd < 0 ? "off (exp_placecmd0)" :
		"NOT installed", cmd > 0 && place_verify ? " (verify)" : "");
#undef AT
#undef LIT
}
