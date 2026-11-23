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
 * UpdateTree, Compute_BoundingBox, GetCollisionTreeForDomain and R-tree Search in the original order, and skips only:
 *  - C when no leaf entry of the tree (other than the player itself, which the candidate callback always skips) can
 *    overlap the searched rectangle. The entries near the whole path are collected once per call (same overlap test
 *    as the runner's Search, checked against the runner's own count of them); during the loop no other entry moves.
 *    A skipped search returns "free", as the original's does when it finds no candidate.
 *  - the R-tree remove + insert of A when it is a no-op: the only dirty instance is the player, it stands where the
 *    previous step's B inserted it, and that B allocated or freed no tree node (no split, no underflow). Removing the
 *    entry B just appended then restores the tree B started from (covers are recomputed exactly), and inserting the
 *    same rectangle again repeats B's insertion. A still runs (bounding box, dirty list); only CollisionUpdate's tree
 *    operations are skipped, through the tree's own busy byte, which makes CollisionUpdate return before changing
 *    anything (the instance's 0x8000 "in tree" flag must already be set).
 *  - the R-tree moves of B between the first clean one and the last one (see "Deferred tree moves" below): the tree
 *    only depends on the last tested box as long as no insertion would split a leaf and no removal would underflow
 *    one, which is checked for every step with the R-tree's own insertion rule. One real CollisionUpdate at the end
 *    (or as soon as a step would split or anything else differs) leaves the tree as the original's last B does.
 * Anything else (move_contact_all, the managed-objects path, odd instances, a full candidate list) runs the original.
 *
 * Debug flags: exp_movecontact0 (original), exp_mcverify (no deferral; run the skipped work too and compare: the search
 * must find nothing, the tree must be byte-identical after the real remove + insert, and the insertion model must
 * predict the leaf every real insertion used), exp_mcab (native off in every other report window), exp_mcprof (time
 * Command_MoveContact). Report line [MOVECONTACT].
 */
#include <stdint.h>
#include <stddef.h>
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

int mc_ab, mc_ab_off, mc_prof;
static int mc_verify;
unsigned mc_calls, mc_fallbacks, mc_steps, mc_search_real, mc_search_skipped, mc_tree_skipped, mc_tree_real;
unsigned mc_verify_ok, mc_verify_bad, mc_prof_us, mc_cand_overflow, mc_cand_mismatch;

static uint32_t word(const void *p, size_t off) { uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v; }
static float scalar(const void *p, size_t off) { float v; memcpy(&v, (const uint8_t *)p + off, 4); return v; }

// Node (0x98 bytes): count, level (0 = leaf), 6 branches of { minx, miny, maxx, maxy, child node, data instance }.
#define NODE_BRANCH(n, i) ((const uint8_t *)(n) + 8 + (i) * 0x18)
#define MAX_CAND 48
typedef struct { float minx, miny, maxx, maxy; } Rect;

// Per Command_MoveContact call
static Rect cand[MAX_CAND];
static int cand_n, cand_state; // 0 = not collected yet, 1 = valid, -1 = unusable (always search)
static bool prev_b_clean;      // the previous step's B allocated/freed no tree node
static float prev_tx, prev_ty; // position the previous step's B inserted the player at
static void *self_inst;
static float sweep_dx, sweep_dy;

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

// Leaf entries other than the player near the whole path. The sweep box is the first tested box stretched along the
// path, plus 4 px (a moved box differs from the translated one by at most 1 px per edge; float drift is far below).
static void collect_candidates(void *tree, const Rect *first) {
	Rect q = *first;
	if (sweep_dx < 0) q.minx += sweep_dx; else q.maxx += sweep_dx;
	if (sweep_dy < 0) q.miny += sweep_dy; else q.maxy += sweep_dy;
	q.minx -= 4.0f; q.miny -= 4.0f; q.maxx += 4.0f; q.maxy += 4.0f;
	cand_n = cand_seen = 0;
	cand_state = 1;
	if (!isfinite(q.minx) || !isfinite(q.miny) || !isfinite(q.maxx) || !isfinite(q.maxy)) {
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

// Tree snapshot for exp_mcverify: every node's bytes in depth-first order, with node addresses
static uint32_t snap_a[4096], snap_b[4096];
static int snap(uint32_t *out, const void *node, int n, int depth) {
	if (n < 0 || n + 40 > 4096 || depth > 16)
		return -1;
	out[n++] = (uint32_t)(uintptr_t)node;
	memcpy(&out[n], node, 0x98);
	n += 0x98 / 4;
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

// Node allocations (AllocNode is the only allocator; frees are inlined pushes onto the pool's free list). A step's B
// allocated or freed nothing when the count and the pool's free-list head / block index / block count are unchanged.
typedef void *AAPCS (*alloc_fn)(void *tree);
static alloc_fn original_alloc;
static uint32_t node_allocs;
static void *AAPCS alloc_node(void *tree) {
	node_allocs++;
	return original_alloc(tree);
}

static uint32_t pool_mark(void *tree) {
	const void *pool = (void *)(uintptr_t)word(tree, 8);
	return (word(pool, 8) * 0x9e3779b1u ^ word(pool, 0xc) * 0x85ebca77u ^ word(pool, 0x10)) + node_allocs * 0xc2b2ae35u;
}

// ---- Deferred tree moves of the player ----
// After a clean B (no node allocated or freed) the tree is Insert(R, T-), where T- is the tree without the player's
// entry (its covers are exact: removal recomputes them on its path, insertion widens them). Every later B removes the
// entry (giving T- back, when that removal doesn't underflow its leaf) and inserts the new box into T-; the next A
// undoes and redoes that insertion. So while no insertion would split a leaf and no removal would underflow one, the
// tree after any number of steps is Insert(R_last, T-): the entry can stay where the first clean B put it, and one
// real CollisionUpdate from that box to the last tested one (flush) leaves the tree exactly as the original's last B
// did. Whether an insertion into T- would split is decided like InsertRectRec: PickBranch at every level (least area
// increase, ties to the smaller area; plain float area in the same operation order), on T- (the covers on the
// entry's path recomputed without it); AddBranch splits a node that already holds 6.
typedef void AAPCS (*update_fn)(void *inst, const float *old_box);
static update_fn collision_update;
static bool deferred;                 // the tree holds the player at def_box; its fields have moved on
static float def_box[4];              // left, top, right, bottom, as CollisionUpdate reads its old box
static int mp_n;                      // T- model: the entry's path from the root (node, branch index), covers in T-
static void *mp_node[16];
static int mp_idx[16];
static Rect mp_cover[16];
unsigned mc_deferred_steps, mc_flushes, mc_model_ok, mc_model_bad;

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
	if (!find_entry((void *)(uintptr_t)word(tree, 0x14), e, 0))
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

static void flush(void) {
	if (!deferred)
		return;
	deferred = false;
	collision_update(self_inst, def_box);
	mc_flushes++;
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
	uint32_t before = pool_mark(tree);
	if (deferred && computed && tree_ok) {
		// the box moves, the tree entry stays (CollisionUpdate returns at the busy byte)
		((uint8_t *)tree)[4] = 1;
		compute_bbox(inst, true);
		((uint8_t *)tree)[4] = 0;
	} else {
		flush();
		before = pool_mark(tree);
		if (computed)
			compute_bbox(inst, true);
	}
	float l = scalar(inst, 0xec), t = scalar(inst, 0xf0), r = scalar(inst, 0xf4), b = scalar(inst, 0xf8);
	Rect q;
	q.minx = r; q.maxx = r;
	if (l > r) q.maxx = l;
	if (l < r) q.minx = l;
	q.miny = b; q.maxy = b;
	if (t > b) q.maxy = t;
	if (t < b) q.miny = t;
	if (deferred) {
		bool fits;
		model_pick(tree, &q, &fits);
		if (fits)
			mc_deferred_steps++;
		else
			flush(); // the original's B: the entry leaves (T-), the new box goes in and splits a leaf here
		// The next A may skip its remove + insert only if this B left the player's entry at its box for (tx, ty)
		prev_b_clean = fits;
	} else {
		bool clean = computed && pool_mark(tree) == before;
		prev_b_clean = clean;
		if (clean && tree_ok && !mc_verify && model_build(tree, &q)) {
			deferred = true;
			def_box[0] = l; def_box[1] = t; def_box[2] = r; def_box[3] = b;
		} else if (mc_verify && clean && tree_ok) {
			// exp_mcverify: no deferral; the model built after the previous clean B must predict the leaf this B put
			// the entry in, and that it fit
			bool fits = false;
			const void *pred = mp_n ? model_pick(tree, &q, &fits) : NULL;
			bool had = mp_n > 0;
			bool built = model_build(tree, &q);
			if (had) {
				if (built && pred == mp_node[mp_n - 1] && fits)
					mc_model_ok++;
				else
					mc_model_bad++;
			}
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
	bool search = cand_state < 0;
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

static void AAPCS move_contact(void *inst, float dir, float maxdist, bool all) {
	if (all || (mc_ab && mc_ab_off) || !inst || word(inst, 0x50) != 1 || *managed_objects) {
		mc_fallbacks++;
		original_move(inst, dir, maxdist, all);
		return;
	}
	uint32_t t0 = mc_prof ? sceKernelGetProcessTimeLow() : 0;
	mc_calls++;
	float a = dir * 3.1415927f;
	a = a / 180.0f;
	float sn, cs;
	sincosf(a, &sn, &cs);
	long steps = lrintf(maxdist);
	if (maxdist <= 0.0f)
		steps = 1000;
	self_inst = inst;
	cand_state = 0;
	prev_b_clean = false;
	deferred = false;
	mp_n = 0;
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
	flush(); // the tree as the original's last B left it
	if (mc_prof)
		mc_prof_us += sceKernelGetProcessTimeLow() - t0;
}

int move_contact_native_install(void *(*hook)(uintptr_t entry, void *replacement)) {
	static int installed;
	if (installed || !hook)
		return 0;
	uintptr_t move = so_symbol(&yoyoloader_mod, "_Z19Command_MoveContactP9CInstanceffb");
	uintptr_t isfree = so_symbol(&yoyoloader_mod, "_Z14Command_IsFreeP9CInstanceff");
	if (!move || !isfree || move - 0xf66b94 != isfree - 0xf665ac)
		return 0;
	uintptr_t bias = move - 0xf66b94;
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
	set_position = (setpos_fn)so_symbol(&yoyoloader_mod, "_ZN9CInstance11SetPositionEff");
	update_tree = (void_fn)so_symbol(&yoyoloader_mod, "_Z10UpdateTreev");
	compute_bbox = (bbox_fn)so_symbol(&yoyoloader_mod, "_ZN9CInstance19Compute_BoundingBoxEb");
	tree_for_domain = (tree_fn)so_symbol(&yoyoloader_mod, "_Z25GetCollisionTreeForDomain15CollisionDomain");
	tree_search = (search_fn)so_symbol(&yoyoloader_mod, "_ZN5RTreeIP9CInstanceffLi6ELi2EE6SearchEPNS2_4NodeEPNS2_4RectERiPFbS1_PvES8_");
	managed_objects = (const uint8_t *)so_symbol(&yoyoloader_mod, "g_enableManagedObjects");
	dirty_list = (uintptr_t *)so_symbol(&yoyoloader_mod, "s_dirty_list");
	uintptr_t instance_list = so_symbol(&yoyoloader_mod, "s_instanceList");
	if (!set_position || !update_tree || !compute_bbox || !tree_for_domain || !tree_search || !managed_objects ||
		!dirty_list || !instance_list || res != instance_list + 4)
		return 0;
	if ((uintptr_t)set_position - bias != 0xf297d4 || (uintptr_t)update_tree - bias != 0xf6ca58 ||
		(uintptr_t)tree_search - bias != 0xdd6224 || (uintptr_t)tree_for_domain - bias != 0xf6c25c)
		return 0;
	collision_update = (update_fn)so_symbol(&yoyoloader_mod, "_Z15CollisionUpdateP9CInstanceR9tagYYRECT");
	if (!collision_update || (uintptr_t)collision_update - bias != 0xf6c3fc)
		return 0;
	free_callback = (cb_fn)cb;
	free_result = (uint8_t *)res;
	mc_verify = exp_flag("mcverify");
	mc_ab = exp_flag("mcab");
	mc_prof = exp_flag("mcprof");
	uintptr_t alloc = so_symbol(&yoyoloader_mod, "_ZN5RTreeIP9CInstanceffLi6ELi2EE9AllocNodeEv");
	if (!alloc || alloc - bias != 0xf6a180 || word((void *)alloc, 0) != 0xe92d4830)
		return 0;
	original_alloc = (alloc_fn)hook(alloc, alloc_node);
	if (!original_alloc)
		return 0;
	original_move = (move_fn)hook(move, move_contact);
	if (!original_move)
		return 0;
	installed = 1;
	return 1;
}
