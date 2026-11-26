/* Target libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701 (Katana ZERO 1.1.10).
 *
 * move_contact_solid without the redundant collision-tree work (exact).
 *
 * The Dragon's dash aim (player_secondary_dragon, every frame while the dash key is held and the aim line is blocked)
 * calls move_contact_solid(dash_dir, dist): Command_MoveContact walks the player one pixel at a time, and every step
 * runs Command_IsFree, which
 *   A. runs UpdateTree(): the player is dirty from the previous step's move, so its bounding box is recomputed and
 *      CollisionUpdate removes it from the collision R-tree and inserts it again (same rectangle),
 *   B. moves the player to the tested position: bounding box, R-tree remove + insert at the new rectangle,
 *   C. searches the R-tree around the new box for solid instances (Collision_Instance on each candidate),
 *   D. moves the player back.
 * With a few hundred steps per frame that cost 1.2-3.6 ms/frame in room_dragon_3 (more on longer aims).
 *
 * This replaces Command_MoveContact's solid path with the same loop, calling the runner's own SetPosition,
 * UpdateTree, Compute_BoundingBox, GetCollisionTreeForDomain, CollisionUpdate and R-tree Search in the original
 * order, and skips only:
 *  - C when no leaf entry of the tree (other than the player itself, which the candidate callback always skips) can
 *    overlap the searched box. The entries overlapping a sweep box around the whole path are collected once per call
 *    (same overlap test as the runner's Search, checked against the runner's own count of them); a step whose box
 *    lies inside the sweep box and overlaps none of them is free without a search (a box outside it is searched).
 *    During the loop no other entry moves.
 *  - the R-tree remove + insert of A when it is a no-op: the only dirty instance is the player, it stands where the
 *    previous step's B inserted it, and that B called CollisionUpdate once, which removed the entry (found) and
 *    inserted it once, allocating or freeing no tree node (no split, no underflow). Removing the entry B just appended
 *    then restores the tree B started from (covers are recomputed exactly), and inserting the same rectangle again
 *    repeats B's insertion. A still runs (bounding box, dirty list); only CollisionUpdate's tree operations are
 *    skipped, through the tree's own busy byte, which makes CollisionUpdate return before changing anything.
 *  - the R-tree moves of B between the first confirmed one and the last one (see "Deferred tree moves" below): the
 *    tree's live branches only depend on the last tested box as long as no insertion would split a leaf and no
 *    removal would underflow one, which is checked for every step with the R-tree's own insertion rule. One real
 *    CollisionUpdate at the end (or as soon as a step would split or anything else differs) leaves the live tree as
 *    the original's last B does. (Unused branch slots may keep different stale bytes: Search, removal and insertion
 *    only read the first `count` branches.)
 * Only obj_player moves natively, and only while its bounding-box updates can't run GML (no skeleton instance, draw
 * and mask sprites absent or plain bitmaps: a Spine sprite would run skeleton animation and its events). Anything
 * else (other movers, move_contact_all, the managed-objects path, odd instances, non-finite direction, distance or
 * position, a full candidate list) runs the original.
 *
 * Debug flags: exp_movecontact0 (original). exp_mcverify (no deferral; run the skipped work too and compare: a skipped
 * search must find nothing, the tree must be byte-identical after the real remove + insert A skipped, the insertion
 * model must predict the leaf every real insertion used, and the live tree without the player's entry must stay
 * byte-identical from step to step). exp_mcdiff (the deferred path itself: every call runs the original on the live
 * tree and the native on a copy of the tree from the same instance state, and compares the instances, the dirty and
 * test lists and the live trees; the game continues with the original's result). exp_mcab (native off in every other
 * report window), exp_mcprof (time the native), exp_mcdiffbreak (negative control for exp_mcdiff: the native
 * drops its last tree move). Report line [MOVECONTACT].
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include "main.h"
#include "so_util.h"

#pragma GCC optimize("no-fast-math")

#define AAPCS __attribute__((pcs("aapcs")))
typedef void AAPCS (*move_fn)(void *inst, float dir, float maxdist, bool all);
typedef void AAPCS (*setpos_fn)(void *inst, float x, float y);
typedef void AAPCS (*void_fn)(void);
typedef void AAPCS (*bbox_fn)(void *inst, bool update);
typedef void *AAPCS (*tree_fn)(int domain);
typedef bool AAPCS (*cb_fn)(void *inst, void *ctx);
typedef bool AAPCS (*search_fn)(void *tree, void *node, float *rect, int *count, cb_fn cb, void *ctx);
typedef void AAPCS (*update_fn)(void *inst, const float *old_box);
typedef int AAPCS (*remove_rect_fn)(void *tree, void *rect, void *id, void *root);
typedef int AAPCS (*insert_rect_fn)(void *tree, const void *branch, void *root, int level);
typedef void *AAPCS (*alloc_fn)(void *tree);
typedef void AAPCS (*ctor_fn)(void *tree, int nodes);
typedef void AAPCS (*dtor_fn)(void *tree);

static move_fn original_move;
static setpos_fn set_position;
static void_fn update_tree;
static bbox_fn compute_bbox;
static tree_fn tree_for_domain;
static search_fn tree_search;
static cb_fn free_callback;          // Command_IsFree's candidate callback (file-local in the runner)
static uint8_t *free_result;          // the byte that callback clears on a hit
static const uint8_t *managed_objects;
static uintptr_t *dirty_list;         // s_dirty_list: { first, last, link offset }
static uintptr_t *test_list;          // s_test_list, same layout
static void **collision_trees;        // g_collision_trees[3]
static update_fn collision_update;    // the runner's CollisionUpdate (through the counting hook below)
static ctor_fn tree_ctor;
static dtor_fn tree_dtor;

int mc_ab, mc_ab_off, mc_prof;
static int mc_verify, mc_diff, mc_diff_break, in_diff_native;
unsigned mc_calls, mc_fallbacks, mc_steps, mc_search_real, mc_search_skipped, mc_tree_skipped, mc_tree_real;
unsigned mc_verify_ok, mc_verify_bad, mc_prof_us, mc_cand_overflow, mc_cand_mismatch;
unsigned mc_deferred_steps, mc_flushes, mc_model_ok, mc_model_bad, mc_diff_ok, mc_diff_bad, mc_diff_skipped;

static uint32_t word(const void *p, size_t off) { uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v; }
static float scalar(const void *p, size_t off) { float v; memcpy(&v, (const uint8_t *)p + off, 4); return v; }

// ---- Counting hooks: node allocation, CollisionUpdate, RemoveRect (found = 0), InsertRect ----
// AllocNode is the tree's only allocator; frees are inlined pushes onto the pool's free list. The hooks count only
// while a native move is armed: CollisionUpdate calls for the moving instance, tree operations on its tree. (All of
// this runs on the game thread: collision, instance steps and these runner functions are never called from the
// loader's other threads.)
static alloc_fn original_alloc;
static update_fn original_update;
static remove_rect_fn original_remove_rect;
static insert_rect_fn original_insert_rect;
static void *armed_inst, *armed_tree;
static uint32_t node_allocs, update_calls, remove_calls, remove_found, insert_calls;
static void *AAPCS alloc_node(void *tree) {
	if (armed_tree && tree == armed_tree)
		node_allocs++;
	return original_alloc(tree);
}
// collision_round_trip.c (collisionRoundTrip) sits in these hooks too: it may handle a CollisionUpdate (skip a no-op
// tree update) while no native move is armed, and it records every leaf insert.
extern bool crt_update(void *inst, const float *old_box, update_fn orig);
extern void crt_insert(void *tree, const void *branch, int level);
extern void crt_remove_rect_enter(void), crt_remove_rect_leave(void);
static int crt_hooked;
static void AAPCS counted_update(void *inst, const float *old_box) {
	if (armed_inst && inst == armed_inst)
		update_calls++;
	if (crt_hooked && !armed_inst && !armed_tree && crt_update(inst, old_box, original_update))
		return;
	original_update(inst, old_box);
}
static int AAPCS counted_remove_rect(void *tree, void *rect, void *id, void *root) {
	bool counted = armed_tree && tree == armed_tree;
	if (counted)
		remove_calls++;
	crt_remove_rect_enter();
	int r = original_remove_rect(tree, rect, id, root);
	crt_remove_rect_leave();
	if (counted && !r)
		remove_found++;
	return r;
}
static int AAPCS counted_insert_rect(void *tree, const void *branch, void *root, int level) {
	if (armed_tree && tree == armed_tree)
		insert_calls++;
	crt_insert(tree, branch, level);
	return original_insert_rect(tree, branch, root, level);
}

typedef struct { uint32_t pool, allocs, updates, removes, found, inserts; } Mark;
static Mark mark(void *tree) {
	const void *pool = (void *)(uintptr_t)word(tree, 8);
	Mark m = { word(pool, 8) * 0x9e3779b1u ^ word(pool, 0xc) * 0x85ebca77u ^ word(pool, 0x10), node_allocs,
		update_calls, remove_calls, remove_found, insert_calls };
	return m;
}
// One CollisionUpdate that removed the entry (found) and inserted it once, with no node allocated or freed
static bool confirmed_update(const Mark *a, const Mark *b) {
	return a->pool == b->pool && a->allocs == b->allocs && b->updates == a->updates + 1 &&
		b->removes == a->removes + 1 && b->found == a->found + 1 && b->inserts == a->inserts + 1;
}

// Node (0x98 bytes): count, level (0 = leaf), 6 branches of { minx, miny, maxx, maxy, child node, data instance }.
#define NODE_SIZE 0x98
#define NODE_BRANCH(n, i) ((const uint8_t *)(n) + 8 + (i) * 0x18)
#define MAX_CAND 48
typedef struct { float minx, miny, maxx, maxy; } Rect;

// Per Command_MoveContact call
static Rect cand[MAX_CAND];
static int cand_n, cand_state; // 0 = not collected yet, 1 = valid, -1 = unusable (always search)
static Rect sweep;             // the box the candidates were collected for
static bool prev_b_clean;      // the previous step's B was a confirmed update (see confirmed_update)
static float prev_tx, prev_ty; // position the previous step's B inserted the player at
static void *self_inst;
static float sweep_dx, sweep_dy;

static bool finite_rect(const Rect *r) {
	return isfinite(r->minx) && isfinite(r->miny) && isfinite(r->maxx) && isfinite(r->maxy);
}

// The runner's Search leaf/branch test: skip when q.minx > b.maxx, b.minx > q.maxx, q.miny > b.maxy, b.miny > q.maxy.
static bool overlap(const Rect *q, const float *b) {
	return !(q->minx > b[2] || b[0] > q->maxx || q->miny > b[3] || b[1] > q->maxy);
}

static int cand_seen; // overlapping leaf entries, the player's included (the runner's Search counts the same)

static void collect(const void *node, const Rect *q, int depth) {
	int count = (int)word(node, 0), level = (int)word(node, 4);
	if (count < 0 || count > 6 || depth > 16) {
		cand_state = -1;
		return;
	}
	for (int i = 0; i < count && cand_state >= 0; i++) {
		const float *b = (const float *)NODE_BRANCH(node, i);
		if (!overlap(q, b))
			continue;
		if (level > 0) {
			collect((void *)(uintptr_t)word(b, 0x10), q, depth + 1);
			continue;
		}
		cand_seen++;
		if ((void *)(uintptr_t)word(b, 0x14) == self_inst)
			continue; // the candidate callback skips the instance itself (same id)
		if (cand_n == MAX_CAND) {
			cand_state = -1;
			return;
		}
		memcpy(&cand[cand_n++], b, sizeof(Rect));
	}
}

// Leaf entries other than the player overlapping a sweep box: the first tested box stretched along the path, plus
// 4 px. Any entry a step's box overlaps is among them as long as that box lies inside the sweep box (checked per
// step; float drift on long paths far from the origin can leave it).
static void collect_candidates(void *tree, const Rect *first) {
	Rect q = *first;
	if (sweep_dx < 0) q.minx += sweep_dx; else q.maxx += sweep_dx;
	if (sweep_dy < 0) q.miny += sweep_dy; else q.maxy += sweep_dy;
	q.minx -= 4.0f; q.miny -= 4.0f; q.maxx += 4.0f; q.maxy += 4.0f;
	cand_n = cand_seen = 0;
	cand_state = 1;
	sweep = q;
	if (!finite_rect(&q)) {
		cand_state = -1;
		return;
	}
	void *root = (void *)(uintptr_t)word(tree, 0x14);
	collect(root, &q, 0);
	if (cand_state < 0) {
		mc_cand_overflow++;
		return;
	}
	// The runner's Search, without a callback, counts the overlapping leaf entries: the node layout read above holds
	int count = 0;
	tree_search(tree, root, &q.minx, &count, NULL, NULL);
	if (count != cand_seen) {
		cand_state = -1;
		mc_cand_mismatch++;
	}
}

static bool inside_sweep(const Rect *q) {
	return q->minx >= sweep.minx && q->maxx <= sweep.maxx && q->miny >= sweep.miny && q->maxy <= sweep.maxy;
}

// Tree snapshot for exp_mcverify: every node's bytes in depth-first order, with node addresses
static uint32_t snap_a[4096], snap_b[4096];
static int snap(uint32_t *out, const void *node, int n, int depth) {
	if (n < 0 || n + 40 > 4096 || depth > 16)
		return -1;
	out[n++] = (uint32_t)(uintptr_t)node;
	memcpy(&out[n], node, NODE_SIZE);
	n += NODE_SIZE / 4;
	int count = (int)word(node, 0), level = (int)word(node, 4);
	if (count < 0 || count > 6)
		return -1;
	if (level > 0)
		for (int i = 0; i < count && n >= 0; i++)
			n = snap(out, (void *)(uintptr_t)word(NODE_BRANCH(node, i), 0x10), n, depth + 1);
	return n;
}

static bool only_self_dirty(void *inst) {
	uintptr_t first = dirty_list[0];
	if (first == (uintptr_t)dirty_list || !first)
		return false;
	return word((void *)first, 0) == (uintptr_t)dirty_list && first - dirty_list[2] == (uintptr_t)inst;
}

// ---- Deferred tree moves of the player ----
// After a confirmed B the tree is Insert(R, T-), where T- is the tree without the player's entry (its covers are
// exact: removal recomputes them on its path, insertion widens them). Every later B removes the entry (giving T-
// back, when that removal doesn't underflow its leaf) and inserts the new box into T-; the next A undoes and redoes
// that insertion. So while no insertion would split a leaf and no removal would underflow one, the live tree after
// any number of steps is Insert(R_last, T-): the entry can stay where the first confirmed B put it, and one real
// CollisionUpdate from that box to the box the original's tree holds (flush) leaves the live tree exactly as the
// original's last B did. Whether an insertion into T- would split is decided like InsertRectRec: PickBranch at every
// level (least area increase, ties to the smaller area; plain float area in the same operation order), on T- (the
// covers on the entry's path recomputed without it); AddBranch splits a node that already holds 6.
// A deferred B's Compute_BoundingBox must call CollisionUpdate exactly once (it returns at the busy byte), as the
// original's does; the instance's collision flags and object are those of the confirmed B (nothing else runs during
// the loop to change them, or the global switches CollisionUpdate reads).
#define FLAG_MASK (0x8000u | 0x100003u | 0x200000u | 0x20u | 0x6000000u)
static bool deferred;                 // the tree holds the player at def_box; its fields have moved on
static float def_box[4];              // left, top, right, bottom, as CollisionUpdate reads its old box
static float orig_box[4];             // the box the original's tree holds the player at
static uint32_t def_flags, def_object, def_object_flags;
static int mp_n;                      // T- model: the entry's path from the root (node, branch index), covers in T-
static void *mp_node[16];
static int mp_idx[16];
static Rect mp_cover[16];

static Rect branch_rect(const void *node, int i) {
	Rect r;
	memcpy(&r, NODE_BRANCH(node, i), sizeof(r));
	return r;
}

// The runner's NodeCover (InsertRectRec after a split): branch 0, widened by each next one (strict comparisons)
static Rect cover(const void *node, int count, int sub, const Rect *sub_rect) {
	Rect c = sub == 0 ? *sub_rect : branch_rect(node, 0);
	for (int i = 1; i < count; i++) {
		Rect b = i == sub ? *sub_rect : branch_rect(node, i);
		if (c.maxy > b.maxy) b.maxy = c.maxy;
		if (c.miny < b.miny) b.miny = c.miny;
		if (c.maxx > b.maxx) b.maxx = c.maxx;
		if (c.minx < b.minx) b.minx = c.minx;
		c = b;
	}
	return c;
}

static bool find_entry(const void *node, const Rect *e, int depth) {
	int count = (int)word(node, 0), level = (int)word(node, 4);
	if (count < 0 || count > 6 || depth >= 16)
		return false;
	for (int i = 0; i < count; i++) {
		const float *b = (const float *)NODE_BRANCH(node, i);
		if (!overlap(e, b))
			continue;
		mp_node[depth] = (void *)node;
		mp_idx[depth] = i;
		if (level > 0) {
			if (find_entry((void *)(uintptr_t)word(b, 0x10), e, depth + 1))
				return true;
		} else if ((void *)(uintptr_t)word(b, 0x14) == self_inst) {
			mp_n = depth + 1;
			return true;
		}
	}
	return false;
}

// T- model of the current tree, where the player's entry (box e) was just appended to its leaf
static bool model_build(void *tree, const Rect *e) {
	mp_n = 0;
	if (!finite_rect(e) || !find_entry((void *)(uintptr_t)word(tree, 0x14), e, 0))
		return false;
	const void *leaf = mp_node[mp_n - 1];
	int count = (int)word(leaf, 0);
	// last slot; T- keeps at least 2 entries there (the entry can leave without an underflow), unless it is the root
	if (mp_idx[mp_n - 1] != count - 1 || count < 2 || (count - 1 < 2 && mp_n > 1)) {
		mp_n = 0;
		return false;
	}
	mp_cover[mp_n - 1] = cover(leaf, count - 1, -1, NULL);
	for (int d = mp_n - 2; d >= 0; d--)
		mp_cover[d] = cover(mp_node[d], (int)word(mp_node[d], 0), mp_idx[d], &mp_cover[d + 1]);
	return true;
}

// The leaf InsertRectRec would add box a to in T-. *fits: no split there, and the entry could leave it again without
// an underflow.
static const void *model_pick(void *tree, const Rect *a, bool *fits) {
	const void *node = (void *)(uintptr_t)word(tree, 0x14);
	bool on = true;
	int d = 0;
	*fits = false;
	if (!finite_rect(a))
		return NULL;
	while ((int)word(node, 4) > 0) {
		int count = (int)word(node, 0), best = 0;
		if (count < 0 || count > 6 || d >= 15)
			return NULL;
		if (count > 1) {
			float best_incr = 0, best_area = 0;
			for (int i = 0; i < count; i++) {
				Rect b = on && i == mp_idx[d] ? mp_cover[d + 1] : branch_rect(node, i);
				float w = b.maxx - b.minx, h = b.maxy - b.miny;
				float area = w * h;
				float cminy = a->miny < b.miny ? a->miny : b.miny, cmaxy = a->maxy > b.maxy ? a->maxy : b.maxy;
				float cminx = a->minx < b.minx ? a->minx : b.minx, cmaxx = a->maxx > b.maxx ? a->maxx : b.maxx;
				float cw = cmaxx - cminx, ch = cmaxy - cminy;
				float grown = cw * ch;
				float incr = grown - area;
				if (i == 0 || incr < best_incr || (incr == best_incr && area < best_area)) {
					best = i;
					best_incr = incr;
					best_area = area;
				}
			}
		}
		on = on && d < mp_n - 1 && best == mp_idx[d];
		node = (void *)(uintptr_t)word(NODE_BRANCH(node, best), 0x10);
		d++;
	}
	int count = (int)word(node, 0);
	if (count < 0 || count > 6)
		return NULL;
	int in_tminus = count - (on && d == mp_n - 1 && node == mp_node[d] ? 1 : 0);
	*fits = in_tminus <= 5 && (in_tminus >= 2 || d == 0);
	return node;
}

// exp_mcverify: T- as the model sees it, node by node (address, count, level, the live branches; the entry's leaf
// without its last slot, the covers on the entry's path replaced by the model's)
static uint32_t tminus_cur[4096], tminus_prev[4096];
static int tminus_prev_n;
static int tminus_serialize(uint32_t *out, const void *node, int n, int depth) {
	int count = (int)word(node, 0), level = (int)word(node, 4);
	if (n < 0 || n + 3 + 6 * 5 > 4096 || depth >= 16 || count < 0 || count > 6)
		return -1;
	bool on = depth < mp_n && mp_node[depth] == node;
	int live = on && depth == mp_n - 1 ? count - 1 : count;
	out[n++] = (uint32_t)(uintptr_t)node;
	out[n++] = (uint32_t)live;
	out[n++] = (uint32_t)level;
	for (int i = 0; i < live; i++) {
		Rect r = on && level > 0 && i == mp_idx[depth] ? mp_cover[depth + 1] : branch_rect(node, i);
		memcpy(&out[n], &r, sizeof(r));
		n += 4;
		out[n++] = word(NODE_BRANCH(node, i), level > 0 ? 0x10 : 0x14);
	}
	if (level > 0)
		for (int i = 0; i < live && n >= 0; i++)
			n = tminus_serialize(out, (void *)(uintptr_t)word(NODE_BRANCH(node, i), 0x10), n, depth + 1);
	return n;
}

// CollisionUpdate(player, def_box) with the player's box fields temporarily at box: the tree entry moves from where
// it stayed to box, as the original's B did for it
static void move_entry_to(const float *box) {
	float keep[4];
	memcpy(keep, (uint8_t *)self_inst + 0xec, sizeof(keep));
	memcpy((uint8_t *)self_inst + 0xec, box, sizeof(keep));
	collision_update(self_inst, def_box);
	memcpy((uint8_t *)self_inst + 0xec, keep, sizeof(keep));
}

static void flush(void) {
	if (!deferred)
		return;
	deferred = false;
	move_entry_to(orig_box);
	mc_flushes++;
}

static bool collision_state_same(void *inst) {
	uint32_t object = word(inst, 0x68);
	return (word(inst, 0x7c) & FLAG_MASK) == def_flags && object == def_object && object &&
		word((void *)(uintptr_t)object, 0x50) == def_object_flags;
}

static int is_free(void *inst, float tx, float ty) {
	uint32_t flags = word(inst, 0x7c);
	int domain = (flags & 0x4000000) ? 2 : (int)((flags >> 25) & 1);
	// A: UpdateTree, with the player's no-op remove + insert skipped
	void *tree = tree_for_domain(domain);
	bool lock = prev_b_clean && (flags & 0x8000) && !(flags & 0x100003) && only_self_dirty(inst) &&
		scalar(inst, 0xb8) == prev_tx && scalar(inst, 0xbc) == prev_ty && !((uint8_t *)tree)[4];
	if (!lock)
		flush(); // the tree as the original has it before a real UpdateTree
	if (lock && !mc_verify) {
		((uint8_t *)tree)[4] = 1;
		update_tree();
		((uint8_t *)tree)[4] = 0;
		mc_tree_skipped++;
	} else if (lock) {
		void *root = (void *)(uintptr_t)word(tree, 0x14);
		int na = snap(snap_a, root, 0, 0);
		update_tree();
		int nb = snap(snap_b, (void *)(uintptr_t)word(tree, 0x14), 0, 0);
		if (na > 0 && na == nb && !memcmp(snap_a, snap_b, na * 4))
			mc_verify_ok++;
		else
			mc_verify_bad++;
		mc_tree_skipped++;
	} else {
		update_tree();
		mc_tree_real++;
	}
	// B: test position (the runner's Command_IsFree from here on)
	float sx = scalar(inst, 0xb8), sy = scalar(inst, 0xbc);
	set_position(inst, tx, ty);
	uint32_t f = word(inst, 0x7c);
	bool computed = f & 8;
	bool tree_ok = (f & 0x8000) && !(f & 0x100003) && !((uint8_t *)tree)[4];
	float before_box[4];
	memcpy(before_box, (uint8_t *)inst + 0xec, sizeof(before_box)); // the old box CollisionUpdate gets
	Mark before = mark(tree);
	bool deferred_b = deferred && computed && tree_ok && collision_state_same(inst);
	if (deferred_b) {
		// the box moves, the tree entry stays (CollisionUpdate returns at the busy byte)
		((uint8_t *)tree)[4] = 1;
		compute_bbox(inst, true);
		((uint8_t *)tree)[4] = 0;
	} else {
		flush();
		before = mark(tree);
		if (computed)
			compute_bbox(inst, true);
	}
	Mark after = mark(tree);
	float l = scalar(inst, 0xec), t = scalar(inst, 0xf0), r = scalar(inst, 0xf4), b = scalar(inst, 0xf8);
	Rect q;
	q.minx = r; q.maxx = r;
	if (l > r) q.maxx = l;
	if (l < r) q.minx = l;
	q.miny = b; q.maxy = b;
	if (t > b) q.maxy = t;
	if (t < b) q.miny = t;
	if (deferred_b) {
		bool fits = false;
		bool one_call = after.updates == before.updates + 1 && after.removes == before.removes &&
			after.inserts == before.inserts && after.pool == before.pool && after.allocs == before.allocs;
		if (one_call)
			model_pick(tree, &q, &fits);
		if (fits) {
			memcpy(orig_box, (uint8_t *)inst + 0xec, sizeof(orig_box));
			mc_deferred_steps++;
		} else {
			// the tree as the original's before this B's CollisionUpdate, then that CollisionUpdate (if the box
			// computation made one: same old box, same instance state)
			flush();
			if (after.updates != before.updates) {
				Mark m = mark(tree);
				collision_update(inst, before_box);
				Mark n = mark(tree);
				fits = confirmed_update(&m, &n); // prev_b_clean below
			}
		}
		// The next A may skip its remove + insert only if this B left the player's entry at its box for (tx, ty)
		prev_b_clean = fits;
	} else {
		bool clean = computed && confirmed_update(&before, &after);
		prev_b_clean = clean;
		if (clean && tree_ok && !mc_verify && model_build(tree, &q)) {
			deferred = true;
			memcpy(def_box, (uint8_t *)inst + 0xec, sizeof(def_box));
			memcpy(orig_box, def_box, sizeof(orig_box));
			def_flags = word(inst, 0x7c) & FLAG_MASK;
			def_object = word(inst, 0x68);
			def_object_flags = def_object ? word((void *)(uintptr_t)def_object, 0x50) : 0;
		} else if (mc_verify && clean && tree_ok) {
			// exp_mcverify: no deferral; the model built after the previous confirmed B must predict the leaf this B
			// put the entry in, and that it fit
			bool fits = false;
			const void *pred = mp_n ? model_pick(tree, &q, &fits) : NULL;
			bool had = mp_n > 0;
			bool built = model_build(tree, &q);
			// and T- (this tree without the entry, as the model sees it) must be byte-identical to the previous one
			int n = built ? tminus_serialize(tminus_cur, (void *)(uintptr_t)word(tree, 0x14), 0, 0) : -1;
			if (had) {
				if (built && pred == mp_node[mp_n - 1] && fits && n > 0 && n == tminus_prev_n &&
					!memcmp(tminus_cur, tminus_prev, n * 4))
					mc_model_ok++;
				else
					mc_model_bad++;
			}
			memcpy(tminus_prev, tminus_cur, n > 0 ? n * 4 : 0);
			tminus_prev_n = n;
		} else if (mc_verify)
			mp_n = 0;
	}
	prev_tx = tx;
	prev_ty = ty;
	*free_result = 1;
	tree = tree_for_domain(domain);
	// C: search unless no entry near the path overlaps
	if (!cand_state)
		collect_candidates(tree, &q);
	bool search = cand_state < 0 || !inside_sweep(&q);
	for (int i = 0; !search && i < cand_n; i++)
		search = overlap(&q, &cand[i].minx);
	((uint8_t *)tree)[4] = 1;
	int count = 0;
	if (search) {
		tree_search(tree, (void *)(uintptr_t)word(tree, 0x14), &q.minx, &count, free_callback, inst);
		mc_search_real++;
	} else {
		if (mc_verify) {
			tree_search(tree, (void *)(uintptr_t)word(tree, 0x14), &q.minx, &count, free_callback, inst);
			if (*free_result)
				mc_verify_ok++;
			else
				mc_verify_bad++;
		}
		mc_search_skipped++;
	}
	((uint8_t *)tree)[4] = 0;
	// D: back
	set_position(inst, sx, sy);
	return *free_result;
}

static void native_move(void *inst, float dir, float maxdist) {
	float a = dir * 3.1415927f;
	a = a / 180.0f;
	float sn, cs;
	sincosf(a, &sn, &cs);
	long steps = lrintf(maxdist);
	if (maxdist <= 0.0f)
		steps = 1000;
	self_inst = inst;
	uint32_t flags = word(inst, 0x7c);
	armed_tree = tree_for_domain((flags & 0x4000000) ? 2 : (int)((flags >> 25) & 1));
	armed_inst = inst;
	cand_state = 0;
	prev_b_clean = false;
	deferred = false;
	mp_n = 0;
	tminus_prev_n = -1;
	sweep_dx = (float)steps * cs;
	sweep_dy = -(float)steps * sn;
	if (!(steps > 0 && steps <= 4096))
		cand_state = -1; // a long sweep would collect the whole room: search every step
	int free = is_free(inst, scalar(inst, 0xb8), scalar(inst, 0xbc));
	if (steps >= 1 && free) {
		do {
			float tx = cs + scalar(inst, 0xb8), ty = scalar(inst, 0xbc) - sn;
			mc_steps++;
			if (!is_free(inst, tx, ty))
				break;
			set_position(inst, cs + scalar(inst, 0xb8), scalar(inst, 0xbc) - sn);
		} while (--steps != 0);
	}
	if (mc_diff_break && in_diff_native)
		deferred = false; // negative control (exp_mcdiff copy only): the deferred tree move is lost
	flush(); // the tree as the original's last B left it
	armed_inst = armed_tree = NULL;
}

// ---- exp_mcdiff ----
// The instances the two runs can change (the player and everything on the dirty and test lists) are saved, the
// tree is copied node by node into a second tree, the original runs on the live tree, the instances are put back and
// the native runs on the copy (through g_collision_trees), then everything is compared and the original's state
// restored. Instance bytes 0x40..0x174 hold everything SetPosition, Compute_BoundingBox and the list links write.
#define DIFF_MAX 32
#define DIFF_LO 0x40
#define DIFF_HI 0x174
static void *diff_inst[DIFF_MAX];
static uint8_t diff_pre[DIFF_MAX][DIFF_HI - DIFF_LO], diff_orig[DIFF_MAX][DIFF_HI - DIFF_LO];
static int diff_n;
static uint32_t diff_ser_a[8192], diff_ser_b[8192];

static bool diff_add(void *inst) {
	for (int i = 0; i < diff_n; i++)
		if (diff_inst[i] == inst)
			return true;
	if (diff_n == DIFF_MAX)
		return false;
	diff_inst[diff_n++] = inst;
	return true;
}

static bool diff_add_list(uintptr_t *head) {
	uintptr_t node = head[0];
	for (int guard = 0; node && node != (uintptr_t)head; guard++) {
		if (guard >= DIFF_MAX || !diff_add((void *)(node - head[2])))
			return false;
		node = word((void *)node, 0);
	}
	return !node || node == (uintptr_t)head; // never used yet (zeros) or back at the head
}

static void diff_save(uint8_t (*buf)[DIFF_HI - DIFF_LO]) {
	for (int i = 0; i < diff_n; i++)
		memcpy(buf[i], (uint8_t *)diff_inst[i] + DIFF_LO, DIFF_HI - DIFF_LO);
}

static void diff_load(uint8_t (*buf)[DIFF_HI - DIFF_LO]) {
	for (int i = 0; i < diff_n; i++)
		memcpy((uint8_t *)diff_inst[i] + DIFF_LO, buf[i], DIFF_HI - DIFF_LO);
}

static void *clone_node(void *copy, const void *node, int depth) {
	int count = (int)word(node, 0), level = (int)word(node, 4);
	if (depth >= 16 || count < 0 || count > 6)
		return NULL;
	void *m = original_alloc(copy);
	if (!m)
		return NULL;
	memcpy(m, node, NODE_SIZE);
	if (level > 0)
		for (int i = 0; i < count; i++) {
			void *c = clone_node(copy, (void *)(uintptr_t)word(NODE_BRANCH(node, i), 0x10), depth + 1);
			if (!c)
				return NULL;
			uint32_t w = (uint32_t)(uintptr_t)c;
			memcpy((uint8_t *)m + 8 + i * 0x18 + 0x10, &w, 4);
		}
	return m;
}

// The live tree, node by node in depth-first order without addresses: count, level, live branches (box, and the
// instance for leaf entries)
static int live_serialize(uint32_t *out, const void *node, int n, int depth) {
	int count = (int)word(node, 0), level = (int)word(node, 4);
	if (n < 0 || n + 2 + 6 * 5 > 8192 || depth >= 16 || count < 0 || count > 6)
		return -1;
	out[n++] = (uint32_t)count;
	out[n++] = (uint32_t)level;
	for (int i = 0; i < count; i++) {
		memcpy(&out[n], NODE_BRANCH(node, i), 16);
		n += 4;
		out[n++] = level > 0 ? 0 : word(NODE_BRANCH(node, i), 0x14);
	}
	if (level > 0)
		for (int i = 0; i < count && n >= 0; i++)
			n = live_serialize(out, (void *)(uintptr_t)word(NODE_BRANCH(node, i), 0x10), n, depth + 1);
	return n;
}

static bool diff_move(void *inst, float dir, float maxdist) {
	uint32_t flags = word(inst, 0x7c);
	int domain = (flags & 0x4000000) ? 2 : (int)((flags >> 25) & 1);
	void *tree = tree_for_domain(domain);
	diff_n = 0;
	if (!diff_add(inst) || !diff_add_list(dirty_list) || !diff_add_list(test_list) || ((uint8_t *)tree)[4])
		return false;
	// Only the mover's tree is copied: every instance either run can update must live in it, and none may own
	// skeleton data (shared allocations the byte snapshots can't put back)
	for (int i = 0; i < diff_n; i++) {
		uint32_t f = word(diff_inst[i], 0x7c);
		if (((f & 0x4000000) ? 2 : (int)((f >> 25) & 1)) != domain || word(diff_inst[i], 0x70) || word(diff_inst[i], 0x74))
			return false;
	}
	uintptr_t lists_pre[6], lists_orig[6];
	memcpy(lists_pre, dirty_list, 12);
	memcpy(lists_pre + 3, test_list, 12);
	diff_save(diff_pre);
	void *copy = malloc(0x1c);
	if (!copy)
		return false;
	tree_ctor(copy, 0x400);
	memcpy((uint8_t *)copy + 0xc, (uint8_t *)tree + 0xc, 8); // the fields after the pool (+0x14, the root, below)
	memcpy((uint8_t *)copy + 0x18, (uint8_t *)tree + 0x18, 4);
	void *root = clone_node(copy, (void *)(uintptr_t)word(tree, 0x14), 0);
	if (!root) {
		tree_dtor(copy);
		free(copy);
		return false;
	}
	uint32_t w = (uint32_t)(uintptr_t)root;
	memcpy((uint8_t *)copy + 0x14, &w, 4);
	// the original, on the live tree
	original_move(inst, dir, maxdist, false);
	uint8_t res_orig = *free_result;
	diff_save(diff_orig);
	memcpy(lists_orig, dirty_list, 12);
	memcpy(lists_orig + 3, test_list, 12);
	int na = live_serialize(diff_ser_a, (void *)(uintptr_t)word(tree, 0x14), 0, 0);
	// the native, on the copy, from the same state
	diff_load(diff_pre);
	memcpy(dirty_list, lists_pre, 12);
	memcpy(test_list, lists_pre + 3, 12);
	collision_trees[domain] = copy;
	in_diff_native = 1;
	native_move(inst, dir, maxdist);
	in_diff_native = 0;
	collision_trees[domain] = tree;
	int nb = live_serialize(diff_ser_b, (void *)(uintptr_t)word(copy, 0x14), 0, 0);
	bool same = na > 0 && na == nb && !memcmp(diff_ser_a, diff_ser_b, na * 4) && *free_result == res_orig &&
		!memcmp(dirty_list, lists_orig, 12) && !memcmp(test_list, lists_orig + 3, 12);
	for (int i = 0; same && i < diff_n; i++)
		same = !memcmp((uint8_t *)diff_inst[i] + DIFF_LO, diff_orig[i], DIFF_HI - DIFF_LO);
	if (same)
		mc_diff_ok++;
	else if (mc_diff_bad++ < 8)
		debugPrintf("[MOVECONTACT] diff mismatch: tree %d/%d words %s, result %d/%d, lists %d %d\n", na, nb,
			na == nb && na > 0 && !memcmp(diff_ser_a, diff_ser_b, na * 4) ? "same" : "differ", res_orig, *free_result,
			!memcmp(dirty_list, lists_orig, 12), !memcmp(test_list, lists_orig + 3, 12));
	// the game continues with the original's result
	diff_load(diff_orig);
	memcpy(dirty_list, lists_orig, 12);
	memcpy(test_list, lists_orig + 3, 12);
	*free_result = res_orig;
	tree_dtor(copy);
	free(copy);
	return true;
}

// The native only moves obj_player (the Dragon's dash aim) and only while its bounding-box updates can't run GML:
// no skeleton instance (+0x70, +0x74) and the draw and mask sprites absent or plain bitmaps (type 0). A Spine sprite
// would make SetPosition / Compute_BoundingBox create or animate a skeleton, whose events run GML synchronously.
typedef int AAPCS (*find_fn)(const char *name);
typedef int AAPCS (*exists_fn)(int sprite);
typedef void *AAPCS (*sprite_fn)(int sprite);
static find_fn object_find;
static exists_fn sprite_exists;
static sprite_fn sprite_data;
static int player_object = -2; // -2: not looked up yet (Object_Find can't run while the loader installs)

static bool plain_sprite(int sprite) {
	if (sprite < 0 || !sprite_exists(sprite))
		return true;
	const void *sp = sprite_data(sprite);
	return sp && word(sp, 0x84) == 0;
}

static bool native_mover(void *inst) {
	if (player_object == -2)
		player_object = object_find("obj_player");
	return player_object >= 0 && (int)word(inst, 0x84) == player_object && !word(inst, 0x70) && !word(inst, 0x74) &&
		plain_sprite((int)word(inst, 0x88)) && plain_sprite((int)word(inst, 0x14c));
}

int mc_off; // exp_movecontact0: the runner's Command_MoveContact (the counting hooks stay for collision_round_trip.c)
static void AAPCS move_contact(void *inst, float dir, float maxdist, bool all) {
	if (all || mc_off || (mc_ab && mc_ab_off) || !inst || word(inst, 0x50) != 1 || *managed_objects || !isfinite(dir) ||
		!isfinite(maxdist) || !isfinite(scalar(inst, 0xb8)) || !isfinite(scalar(inst, 0xbc)) || !native_mover(inst)) {
		mc_fallbacks++;
		original_move(inst, dir, maxdist, all);
		return;
	}
	uint32_t t0 = mc_prof ? sceKernelGetProcessTimeLow() : 0;
	mc_calls++;
	if (mc_diff) {
		if (!diff_move(inst, dir, maxdist)) {
			mc_diff_skipped++;
			original_move(inst, dir, maxdist, false);
		}
	} else
		native_move(inst, dir, maxdist);
	if (mc_prof)
		mc_prof_us += sceKernelGetProcessTimeLow() - t0;
}

static uintptr_t bias;
static bool resolve(const char *name, uintptr_t addr, uint32_t first_word, uintptr_t *out) {
	uintptr_t p = so_symbol(&yoyoloader_mod, name);
	if (!p || p - bias != addr || (first_word && word((void *)p, 0) != first_word))
		return false;
	*out = p;
	return true;
}

// All five entry hooks or none: every prologue is checked and every trampoline written before any entry is patched
// (hook_addr itself can't fail).
extern void *katana_rx_alloc(size_t size);
static bool hook_all(int n, const uintptr_t *entry, void *const *replacement, void **original) {
	for (int i = 0; i < n; i++) {
		const uint32_t *w = (const uint32_t *)entry[i];
		if (!entry[i] || (entry[i] & 3) || (w[0] & 0xffff4000) != 0xe92d4000 || (w[1] & 0xfffff000) != 0xe28db000)
			return false;
	}
	uint32_t *rx = katana_rx_alloc(16 * n);
	if (!rx)
		return false;
	for (int i = 0; i < n; i++) {
		const uint32_t *w = (const uint32_t *)entry[i];
		uint32_t code[4] = { w[0], w[1], 0xe51ff004, (uint32_t)(entry[i] + 8) };
		kuKernelCpuUnrestrictedMemcpy(rx + 4 * i, code, sizeof(code));
	}
	kuKernelFlushCaches(rx, 16 * n);
	for (int i = 0; i < n; i++)
		original[i] = rx + 4 * i;
	for (int i = 0; i < n; i++) {
		hook_raw(entry[i], (uintptr_t)replacement[i]);
		kuKernelFlushCaches((void *)entry[i], 8);
	}
	return true;
}

int move_contact_native_install(void *(*hook)(uintptr_t entry, void *replacement)) {
	static int installed;
	(void)hook; // entry_hook patches one entry at a time; hook_all below patches all or none
	if (installed)
		return 0;
	uintptr_t move = so_symbol(&yoyoloader_mod, "_Z19Command_MoveContactP9CInstanceffb");
	uintptr_t isfree = so_symbol(&yoyoloader_mod, "_Z14Command_IsFreeP9CInstanceff");
	if (!move || !isfree || move - 0xf66b94 != isfree - 0xf665ac)
		return 0;
	bias = move - 0xf66b94;
	// Command_IsFree's solid path as read above (calls aren't checked: patch_plt_calls.py may retarget them), its
	// callback and result byte (from its literal pool), and CollisionUpdate's busy-byte test
	static const uint32_t expect[][2] = {
		{ 0xf665ac, 0xe92d48f0 }, { 0xf665b0, 0xe28db010 }, { 0xf665e8, 0xe3100301 }, { 0xf66700, 0xe59f50b0 },
		{ 0xf66708, 0xe08f5005 }, { 0xf66710, 0xe5c56000 }, { 0xf66730, 0xe59f0084 }, { 0xf6673c, 0xe08f0000 },
		{ 0xf66744, 0xe5c46004 }, { 0xf66778, 0xe5c46004 }, { 0xf667c8, 0xe92d4c70 }, { 0xf66848, 0x15c06000 },
		{ 0xf66b94, 0xe92d4c70 }, { 0xf66b98, 0xe28db010 }, { 0xf6c494, 0xe5d42004 }, { 0xf6c498, 0xe3520000 },
	};
	for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++)
		if (word((void *)(bias + expect[i][0]), 0) != expect[i][1])
			return 0;
	uintptr_t cb = bias + 0xf66744 + word((void *)(bias + 0xf667bc), 0);
	uintptr_t res = bias + 0xf66710 + word((void *)(bias + 0xf667b8), 0);
	if (cb != bias + 0xf667c8)
		return 0;
	uintptr_t sp, ut, cbb, tfd, ts, cu, rr, ir, al, ct, dt;
	if (!resolve("_ZN9CInstance11SetPositionEff", 0xf297d4, 0, &sp) ||
		!resolve("_Z10UpdateTreev", 0xf6ca58, 0, &ut) ||
		!resolve("_ZN9CInstance19Compute_BoundingBoxEb", 0xf2587c, 0, &cbb) ||
		!resolve("_Z25GetCollisionTreeForDomain15CollisionDomain", 0xf6c25c, 0, &tfd) ||
		!resolve("_ZN5RTreeIP9CInstanceffLi6ELi2EE6SearchEPNS2_4NodeEPNS2_4RectERiPFbS1_PvES8_", 0xdd6224, 0, &ts) ||
		!resolve("_Z15CollisionUpdateP9CInstanceR9tagYYRECT", 0xf6c3fc, 0xe92d4bf0, &cu) ||
		!resolve("_ZN5RTreeIP9CInstanceffLi6ELi2EE10RemoveRectEPNS2_4RectERKS1_PPNS2_4NodeE", 0xf6d488, 0xe92d4bf0, &rr) ||
		!resolve("_ZN5RTreeIP9CInstanceffLi6ELi2EE10InsertRectERKNS2_6BranchEPPNS2_4NodeEi", 0xf69b84, 0xe92d48f0, &ir) ||
		!resolve("_ZN5RTreeIP9CInstanceffLi6ELi2EE9AllocNodeEv", 0xf6a180, 0xe92d4830, &al) ||
		!resolve("_ZN5RTreeIP9CInstanceffLi6ELi2EEC2Ei", 0xf6be50, 0, &ct) ||
		!resolve("_ZN5RTreeIP9CInstanceffLi6ELi2EED2Ev", 0xf6d384, 0, &dt))
		return 0;
	managed_objects = (const uint8_t *)so_symbol(&yoyoloader_mod, "g_enableManagedObjects");
	dirty_list = (uintptr_t *)so_symbol(&yoyoloader_mod, "s_dirty_list");
	test_list = (uintptr_t *)so_symbol(&yoyoloader_mod, "s_test_list");
	collision_trees = (void **)so_symbol(&yoyoloader_mod, "g_collision_trees");
	uintptr_t instance_list = so_symbol(&yoyoloader_mod, "s_instanceList");
	if (!managed_objects || !dirty_list || !test_list || !collision_trees || !instance_list || res != instance_list + 4)
		return 0;
	set_position = (setpos_fn)sp;
	update_tree = (void_fn)ut;
	compute_bbox = (bbox_fn)cbb;
	tree_for_domain = (tree_fn)tfd;
	tree_search = (search_fn)ts;
	collision_update = (update_fn)cu;
	tree_ctor = (ctor_fn)ct;
	tree_dtor = (dtor_fn)dt;
	free_callback = (cb_fn)cb;
	free_result = (uint8_t *)res;
	mc_verify = exp_flag("mcverify");
	mc_diff = exp_flag("mcdiff");
	mc_diff_break = exp_flag("mcdiffbreak");
	mc_ab = exp_flag("mcab");
	mc_prof = exp_flag("mcprof");
	object_find = (find_fn)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	sprite_exists = (exists_fn)so_symbol(&yoyoloader_mod, "_Z13Sprite_Existsi");
	sprite_data = (sprite_fn)so_symbol(&yoyoloader_mod, "_Z11Sprite_Datai");
	if (!object_find || !sprite_exists || !sprite_data)
		return 0;
	const uintptr_t entries[5] = { al, cu, rr, ir, move };
	void *const replacements[5] = { (void *)alloc_node, (void *)counted_update, (void *)counted_remove_rect,
		(void *)counted_insert_rect, (void *)move_contact };
	void *originals[5];
	if (!hook_all(5, entries, replacements, originals))
		return 0;
	original_alloc = (alloc_fn)originals[0];
	original_update = (update_fn)originals[1];
	original_remove_rect = (remove_rect_fn)originals[2];
	original_insert_rect = (insert_rect_fn)originals[3];
	original_move = (move_fn)originals[4];
	installed = 1;
	extern int crt_install(uintptr_t runner_bias, void *insert_original);
	crt_hooked = crt_install(bias, originals[3]);
	debugPrintf("collision round trips (collisionRoundTrip): %s\n", crt_hooked ? "installed" : "NOT installed");
	return 1;
}
