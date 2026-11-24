/* Target libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701 (Katana ZERO 1.1.10).
 *
 * Collision-tree updates of instances that are back where they were (collisionRoundTrip, near-exact).
 *
 * place_meeting(x+dx, y+dy, obj), instance_place and the other probes move the caller there and back with
 * CInstance::SetPosition, and so does GML that sets x/y and restores them (the smoke clouds' Draw wobble). Each
 * SetPosition marks the instance dirty (CollisionMarkDirty), so the next tree query drains it in UpdateTree:
 * Compute_BoundingBox(inst, true) computes the same box again and CollisionUpdate removes the instance's R-tree entry
 * and inserts the same rectangle (RTree::Remove: search for the entry, condense; InsertRect: choose a leaf, maybe
 * split). Command_InstancePlace's own tree path never moves the prober's entry: it computes the probe box with
 * Compute_BoundingBox(self, false), searches (the candidate callback skips self), moves back and restores the box.
 * So after a round trip the remove + insert only reorders the tree.
 *
 * This skips CollisionUpdate's remove + insert when the instance's entry already holds exactly the rectangle it would
 * insert. The rectangle each instance holds is recorded from every leaf-level RTree::InsertRect (CollisionUpdate,
 * ShouldUseFastCollision's lazy insert of a newly queried object, RebuildTree, RemoveRect's reinsertion of orphaned
 * entries), with the tree it went into. A record is used only while
 *  - the instance's in-tree flag (0x8000 at +0x7c) is set: every write that sets it follows an InsertRect, every
 *    removal that takes the entry out without a reinsert clears it (CollisionRemove, StartRoom, RebuildTree), and
 *    CInstance::Assign copies only flag 8;
 *  - no RTree has been constructed since (CleanCollisions and RebuildTree replace whole trees; each construction
 *    bumps an epoch that invalidates every record);
 *  - the tree is the one CollisionUpdate would update (domain from the flags), and every early return of
 *    CollisionUpdate is ruled out (busy tree, object not in collision, managed objects), so the original would do the
 *    remove + insert.
 * Then the set of tree entries and every entry's rectangle stay as the original leaves them; only the order of entries
 * inside the tree differs (remove + insert moves the entry to wherever insertion puts it). That order decides which of
 * several overlapping instances a first-hit query (instance_place, collision_line, ...) returns and the order of
 * collision lists; booleans (place_meeting, place_free, collision_* tested against noone) can't change.
 *
 * The bounding box is still computed (flag 8 and the box fields as before) and the dirty/test lists are untouched.
 * Inactive while move_contact_native.c moves the player natively (its tree model expects the runner's updates).
 *
 * Setting collisionRoundTrip (yyl.cfg): 1 (default) skips, 0 = original.
 * Debug flags (exp_*): crtab (skip off in every other report window), crtprof (time UpdateTree drains), crtstat
 * (per object and first dirty-mark call site: no-op vs real updates and their time; flag polled each window, needs
 * exp_crtres at boot to install the mark/drain hooks), crtverify (shadow trees, see below), crtverifyfault (negative
 * control: also skips one in 32 real moves). Report lines [CRT], [CRTSTAT], [CRTVERIFY].
 *
 * exp_crtverify: every live tree gets a shadow RTree (the runner's class) when it is constructed, and the shadow gets
 * every operation the original would do: CollisionUpdate's remove + insert (also when skipped live), external
 * RTree::Remove (CollisionRemove) and external leaf inserts (ShouldUseFastCollision, RebuildTree). Every top-level
 * Search/SearchLine on a live tree first runs the same query on both trees with a recording callback (the runner's
 * own traversal, all candidates in visiting order), and compares
 *  - the candidate sets (must be equal), and for rectangle queries the overlapping entries with their rectangles;
 *  - every 64th query (and in each report) all entries of both trees with their rectangles;
 *  - the visiting order; when it differs, the query's result in each order: for the first-hit callbacks of
 *    Command_InstancePlace (instance_place, place_meeting, ...), Command_CollisionLine and Command_CollisionRectangle
 *    (non-list mode) by calling the callback per candidate and restoring its result global; for HandleCollision's
 *    collisionResult (collision events) by letting it append its {self, other} pairs and putting the pair count and
 *    call counter back. Differences are reported per API, PerformColTest caller, GML call site (return address of
 *    YYGML_CallLegacyFunction), self object, running event and the two results' objects.
 * The game continues with the live query.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include "main.h"
#include "so_util.h"

#define AAPCS __attribute__((pcs("aapcs")))
typedef void AAPCS (*update_fn)(void *inst, const float *old_box);
typedef int AAPCS (*insert_rect_fn)(void *tree, const void *branch, void *root, int level);
typedef void *AAPCS (*ctor_fn)(void *tree, int nodes);
typedef void *AAPCS (*dtor_fn)(void *tree);
typedef void AAPCS (*mark_fn)(void *inst);
typedef void AAPCS (*void_fn)(void);
typedef bool AAPCS (*cb_fn)(void *inst, void *ctx);
typedef int AAPCS (*search_fn)(void *tree, void *node, void *rect, int *count, cb_fn cb, void *ctx);
typedef bool AAPCS (*remove_fn)(void *tree, const float *min, const float *max, void *const *id, bool retry);
typedef void *AAPCS (*place_fn)(void *inst, float x, float y, int obj, void *list);
typedef const char *AAPCS (*name_fn)(int obj);

int collision_round_trip = 1; // collisionRoundTrip (yyl.cfg)
int crt_ab, crt_ab_off, crt_prof, crt_stat, crt_verify;
static int crt_fault, crt_research, installed;
unsigned crt_updates, crt_noop, crt_skipped, crt_drain_us, crt_drains, crt_drained;

static insert_rect_fn original_insert;
static ctor_fn original_ctor;
static dtor_fn original_dtor;
static mark_fn original_mark;
static void_fn original_update_tree;
static search_fn original_search, original_search_line;
static remove_fn original_remove;
static place_fn original_place;
static name_fn object_name;
static void **collision_trees;
static const uint8_t *add_all_objects, *managed_objects;
static uintptr_t *dirty_list;
static uintptr_t bias;

static uint32_t word(const void *p, size_t off) { uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v; }

typedef struct { float minx, miny, maxx, maxy; } Rect;
typedef struct { Rect r; void *child, *data; } Branch;

// ---- Records: the rectangle each instance's entry holds (4-way set-associative on the instance pointer) ----
#define SETS 1024
#define WAYS 4
typedef struct { void *inst, *tree; uint32_t epoch; Rect r; uint32_t mark_lr; } Record;
static Record records[SETS][WAYS];
static uint8_t victim[SETS];
static uint32_t epoch = 1;

static unsigned set_of(const void *inst) { return ((uintptr_t)inst >> 3) * 2654435761u >> 22; }
static Record *lookup(const void *inst) {
	Record *s = records[set_of(inst)];
	for (int i = 0; i < WAYS; i++)
		if (s[i].inst == inst)
			return &s[i];
	return NULL;
}
static Record *lookup_or_add(void *inst) {
	unsigned k = set_of(inst);
	Record *s = records[k];
	for (int i = 0; i < WAYS; i++)
		if (s[i].inst == inst)
			return &s[i];
	for (int i = 0; i < WAYS; i++)
		if (!s[i].inst || s[i].epoch != epoch) {
			memset(&s[i], 0, sizeof(Record));
			s[i].inst = inst;
			return &s[i];
		}
	Record *r = &s[victim[k]++ % WAYS]; // evicting only costs that instance a skip
	memset(r, 0, sizeof(Record));
	r->inst = inst;
	return r;
}

static bool is_live(const void *tree) {
	return tree && (tree == collision_trees[0] || tree == collision_trees[1] || tree == collision_trees[2]);
}

// ---- Shadow trees (exp_crtverify) ----
typedef struct { void *live, *shadow; } Shadow;
static Shadow shadows[8];
static int in_update, in_remove, in_shadow_op;
unsigned crtv_queries, crtv_line_queries, crtv_set_bad, crtv_rect_bad, crtv_full_ok, crtv_full_bad, crtv_order_diff,
	crtv_result_checked, crtv_result_diff, crtv_unknown_cb_diff, crtv_list_diff, crtv_unshadowed, crtv_candidates;
static void *shadow_of(const void *live) {
	for (int i = 0; i < 8; i++)
		if (shadows[i].live == live)
			return shadows[i].shadow;
	return NULL;
}
static void shadow_insert(void *live, const Branch *b) {
	void *s = shadow_of(live);
	if (!s)
		return;
	in_shadow_op++;
	original_insert(s, b, (uint8_t *)s + 20, 0);
	in_shadow_op--;
}
static void shadow_remove(void *live, const float *min, const float *max, void *const *id, bool retry) {
	void *s = shadow_of(live);
	if (!s)
		return;
	in_shadow_op++;
	original_remove(s, min, max, id, retry);
	in_shadow_op--;
}

// ---- Hooks ----
static void stat_update(void *inst, int kind, uint32_t us); // kind 0 real move, 1 no-op run, 2 skipped

// Called by move_contact_native.c's CollisionUpdate hook (in front of the runner's CollisionUpdate) when no native
// move is armed. Returns true when the update was handled here (skipped), false to run the original.
bool crt_update(void *inst, const float *old_box, update_fn orig) {
	uint32_t f = word(inst, 0x7c);
	void *obj = (void *)(uintptr_t)word(inst, 0x68);
	if (!obj)
		return false;
	if (!(f & 0x8000) && (f & 0x100003))
		return false;
	int domain = (f & 0x4000000) ? 2 : (int)((f >> 25) & 1);
	void *tree = collision_trees[domain];
	if (!tree || ((uint8_t *)tree)[4])
		return false;
	uint32_t of = word(obj, 0x50);
	if (!(of & 0x10))
		return false;
	if (!(of & 0x28) && !*add_all_objects && !(f & 0x20))
		return false;
	if ((f & 0x200000) && *managed_objects)
		return false;
	// The original removes (when in the tree) and inserts this rectangle (same normalisation as CollisionUpdate)
	const float *bb = (const float *)((const uint8_t *)inst + 0xec);
	Branch nb = { { bb[0] < bb[2] ? bb[0] : bb[2], bb[1] < bb[3] ? bb[1] : bb[3], bb[0] > bb[2] ? bb[0] : bb[2],
		bb[1] > bb[3] ? bb[1] : bb[3] }, NULL, inst };
	Record *r = (f & 0x8000) ? lookup(inst) : NULL;
	bool noop = r && r->tree == tree && r->epoch == epoch && !memcmp(&r->r, &nb.r, sizeof(Rect));
	crt_updates++;
	crt_noop += noop;
	bool skip = noop && collision_round_trip && !(crt_ab && crt_ab_off);
	static unsigned fault_n;
	if (!noop && crt_fault && (f & 0x8000) && collision_round_trip && !(crt_ab && crt_ab_off) && ++fault_n % 32 == 0)
		skip = true; // negative control: a real move left out of the live tree
	if (crt_verify && shadow_of(tree)) {
		// the original's operations on the shadow
		Rect o = { old_box[0] < old_box[2] ? old_box[0] : old_box[2], old_box[1] < old_box[3] ? old_box[1] : old_box[3],
			old_box[0] > old_box[2] ? old_box[0] : old_box[2], old_box[1] > old_box[3] ? old_box[1] : old_box[3] };
		void *id = inst;
		if (f & 0x8000)
			shadow_remove(tree, &o.minx, &o.maxx, &id, true);
		shadow_insert(tree, &nb);
	}
	if (skip) {
		crt_skipped++;
		if (crt_stat)
			stat_update(inst, 2, 0);
		return true;
	}
	uint32_t t0 = crt_stat ? sceKernelGetProcessTimeLow() : 0;
	in_update++;
	orig(inst, old_box);
	in_update--;
	if (crt_stat)
		stat_update(inst, noop ? 1 : 0, sceKernelGetProcessTimeLow() - t0);
	return true;
}

// Called by move_contact_native.c's InsertRect hook before the runner's InsertRect.
void crt_insert(void *tree, const void *branch, int level) {
	if (level != 0 || in_shadow_op || !installed)
		return;
	const Branch *b = branch;
	if (!b->data || !is_live(tree))
		return;
	Record *r = lookup_or_add(b->data);
	r->tree = tree;
	r->epoch = epoch;
	r->r = b->r;
	// external leaf insert (not CollisionUpdate's, not RemoveRect's orphan reinsertion): mirror on the shadow
	if (crt_verify && !in_update && !in_remove)
		shadow_insert(tree, b);
}
void crt_remove_rect_enter(void) { in_remove++; }
void crt_remove_rect_leave(void) { in_remove--; }

unsigned crt_ctors;
static void *AAPCS tree_ctor(void *tree, int nodes) {
	epoch++;
	if (!in_shadow_op)
		crt_ctors++;
	void *r = original_ctor(tree, nodes);
	if (crt_verify && !in_shadow_op) {
		int slot = -1;
		for (int i = 0; i < 8; i++)
			if (shadows[i].live == tree) {
				slot = i;
				break;
			}
		for (int i = 0; i < 8 && slot < 0; i++)
			if (!shadows[i].live)
				slot = i;
		if (slot >= 0) {
			if (shadows[slot].shadow) {
				in_shadow_op++;
				original_dtor(shadows[slot].shadow);
				in_shadow_op--;
				free(shadows[slot].shadow);
			}
			void *s = calloc(1, 32);
			in_shadow_op++;
			original_ctor(s, nodes);
			in_shadow_op--;
			shadows[slot].live = tree;
			shadows[slot].shadow = s;
		} else
			crtv_unshadowed++;
	}
	return r;
}
static void *AAPCS tree_dtor(void *tree) {
	if (crt_verify && !in_shadow_op)
		for (int i = 0; i < 8; i++)
			if (shadows[i].live == tree) {
				in_shadow_op++;
				original_dtor(shadows[i].shadow);
				in_shadow_op--;
				free(shadows[i].shadow);
				shadows[i].live = shadows[i].shadow = NULL;
			}
	epoch++;
	return original_dtor(tree);
}
static bool AAPCS tree_remove(void *tree, const float *min, const float *max, void *const *id, bool retry) {
	bool r = original_remove(tree, min, max, id, retry);
	if (!in_update && !in_shadow_op && is_live(tree))
		shadow_remove(tree, min, max, id, retry);
	return r;
}

// ---- Research: first dirty mark per drain and per-object costs (exp_crtres at boot, exp_crtstat live) ----
static int in_drain;
unsigned crt_marks, crt_first_marks;
static void AAPCS mark_dirty(void *inst) {
	if (crt_stat) {
		crt_marks++;
		if (word(inst, 0x170) != (uintptr_t)dirty_list && word(inst, 0x68)) {
			crt_first_marks++;
			Record *r = lookup_or_add(inst);
			r->mark_lr = (uint32_t)(uintptr_t)__builtin_return_address(0) - bias;
		}
	}
	original_mark(inst);
}
static void AAPCS update_tree(void) {
	if (!(crt_prof || crt_stat) || dirty_list[0] == (uintptr_t)dirty_list || !dirty_list[0]) {
		original_update_tree();
		return;
	}
	uint32_t t0 = sceKernelGetProcessTimeLow();
	in_drain++;
	original_update_tree();
	in_drain--;
	crt_drain_us += sceKernelGetProcessTimeLow() - t0;
	crt_drains++;
}

typedef struct { int obj; uint32_t lr; uint8_t drain; unsigned noop, move, skipped, noop_us, move_us; } Stat;
#define NSTAT 512
static Stat stats[NSTAT];
static void stat_update(void *inst, int kind, uint32_t us) {
	Record *r = lookup(inst);
	uint32_t lr = r ? r->mark_lr : 0;
	int obj = (int)word(inst, 0x84);
	uint8_t drain = in_drain ? 1 : 0;
	unsigned h = ((unsigned)obj * 31u + lr * 2654435761u + drain) % NSTAT;
	for (int i = 0; i < NSTAT; i++, h = (h + 1) % NSTAT) {
		Stat *s = &stats[h];
		if (!s->noop && !s->move && !s->skipped) {
			s->obj = obj;
			s->lr = lr;
			s->drain = drain;
		} else if (s->obj != obj || s->lr != lr || s->drain != drain)
			continue;
		if (kind == 2)
			s->skipped++;
		else if (kind == 1) {
			s->noop++;
			s->noop_us += us;
		} else {
			s->move++;
			s->move_us += us;
		}
		return;
	}
}

// ---- Verify: queries on both trees ----
#define MAXC 256
typedef struct { int n; void *c[MAXC]; } Cands;
static bool AAPCS record_cb(void *inst, void *ctx) {
	Cands *c = ctx;
	if (c->n < MAXC)
		c->c[c->n] = inst;
	c->n++;
	return true;
}
static int cmp_ptr(const void *a, const void *b) {
	uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
	return x < y ? -1 : x > y;
}
typedef struct { void *inst; Rect r; } Entry;
#define MAXE 4096
static Entry ent_a[MAXE], ent_b[MAXE];
static int cmp_entry(const void *a, const void *b) {
	const Entry *x = a, *y = b;
	if (x->inst != y->inst)
		return (uintptr_t)x->inst < (uintptr_t)y->inst ? -1 : 1;
	return memcmp(&x->r, &y->r, sizeof(Rect));
}
static bool overlap(const Rect *q, const Rect *b) {
	return !(q->minx > b->maxx || b->minx > q->maxx || q->miny > b->maxy || b->miny > q->maxy);
}
// leaf entries (overlapping q, or all when q is NULL) of a tree, in Search's visiting order
static int collect(const void *node, const Rect *q, Entry *out, int n, int depth) {
	int count = (int)word(node, 0), level = (int)word(node, 4);
	if (count < 0 || count > 6 || depth > 24)
		return -1;
	for (int i = 0; i < count && n >= 0; i++) {
		const uint8_t *b = (const uint8_t *)node + 8 + i * 0x18;
		Rect r;
		memcpy(&r, b, sizeof(Rect));
		if (q && !overlap(q, &r))
			continue;
		if (level > 0)
			n = collect((void *)(uintptr_t)word(b, 0x10), q, out, n, depth + 1);
		else if (n < MAXE) {
			out[n].inst = (void *)(uintptr_t)word(b, 0x14);
			out[n].r = r;
			n++;
		} else
			return -1;
	}
	return n;
}
static int log_budget = 40;
static bool compare_entries(void *live, void *shadow, const Rect *q, const char *what) {
	int na = collect((void *)(uintptr_t)word(live, 20), q, ent_a, 0, 0);
	int nb = collect((void *)(uintptr_t)word(shadow, 20), q, ent_b, 0, 0);
	if (na < 0 || nb < 0)
		return true; // too large to compare
	qsort(ent_a, na, sizeof(Entry), cmp_entry);
	qsort(ent_b, nb, sizeof(Entry), cmp_entry);
	bool ok = na == nb && !memcmp(ent_a, ent_b, na * sizeof(Entry));
	if (!ok && log_budget > 0) {
		log_budget--;
		int i = 0;
		while (i < na && i < nb && !cmp_entry(&ent_a[i], &ent_b[i]))
			i++;
		debugPrintf("[CRTVERIFY] %s mismatch: live %d shadow %d entries, first difference at %d: live %p obj %d "
			"(%.1f,%.1f)-(%.1f,%.1f) shadow %p obj %d (%.1f,%.1f)-(%.1f,%.1f)\n", what, na, nb, i,
			i < na ? ent_a[i].inst : NULL, i < na ? (int)word(ent_a[i].inst, 0x84) : -1, i < na ? ent_a[i].r.minx : 0,
			i < na ? ent_a[i].r.miny : 0, i < na ? ent_a[i].r.maxx : 0, i < na ? ent_a[i].r.maxy : 0,
			i < nb ? ent_b[i].inst : NULL, i < nb ? (int)word(ent_b[i].inst, 0x84) : -1, i < nb ? ent_b[i].r.minx : 0,
			i < nb ? ent_b[i].r.miny : 0, i < nb ? ent_b[i].r.maxx : 0, i < nb ? ent_b[i].r.maxy : 0);
	}
	return ok;
}

// First-hit queries: the callback returns false on a hit (and writes the instance to its result global) unless the
// query fills a list (list global set). instance_place / place_meeting (Command_InstancePlace), collision_line
// (Command_CollisionLine), collision_rectangle (Command_CollisionRectangle).
enum { API_PLACE, API_LINE, API_RECT, API_EVENT, NAPI };
static const char *const api_names[NAPI] = { "instance place", "collision_line", "collision_rectangle", "collision event" };
typedef struct { uintptr_t cb, result_slot, list_slot; } FirstHit;
static FirstHit first_hits[3];
// Collision events (HandleCollision's callback collisionResult): appends {self, other} pairs to a static array that
// processCollision then runs in order; also counts calls in a global.
static uintptr_t event_cb, event_counter_slot, event_array, event_count;
static uint32_t place_caller, pct_caller, gml_site; // gml_site: YYC return address of YYGML_CallLegacyFunction
static void *query_self; // the instance of the running collision_line / collision_rectangle
unsigned crtv_result_checked_api[NAPI], crtv_result_diff_api[NAPI];
// Result differences per (api, caller, self object, event, live result, original result)
typedef struct { int api; uint32_t caller, site; int self, ev_type, ev_num, a, b; unsigned n; } Diff;
#define NDIFF 1024
static Diff diffs[NDIFF];
static const int *event_type, *event_number;
static void note_diff(int api, uint32_t caller, int self, int a, int b) {
	int et = event_type ? *event_type : -1, en = event_number ? *event_number : -1;
	for (int i = 0; i < NDIFF; i++) {
		Diff *d = &diffs[i];
		if (!d->n) {
			*d = (Diff){ api, caller, gml_site, self, et, en, a, b, 1 };
			return;
		}
		if (d->api == api && d->caller == caller && d->site == gml_site && d->self == self && d->ev_type == et && d->ev_num == en && d->a == a &&
			d->b == b) {
			d->n++;
			return;
		}
	}
}
static void *first_hit(const Cands *c, cb_fn cb, void *ctx, const FirstHit *h) {
	void **result = *(void ***)h->result_slot;
	void *saved = *result;
	void *hit = NULL;
	for (int i = 0; i < c->n && i < MAXC && !hit; i++)
		if (!cb(c->c[i], ctx))
			hit = c->c[i];
	*result = saved;
	return hit;
}
// The pairs collisionResult would append for the candidates in this order (array and counters put back)
static int event_pairs(const Cands *c, cb_fn cb, void *ctx, void **out) {
	int *count = (int *)event_count, *counter = *(int **)event_counter_slot;
	int c0 = *count, k0 = *counter;
	for (int i = 0; i < c->n && i < MAXC; i++)
		cb(c->c[i], ctx);
	int n = *count - c0;
	void **arr = *(void ***)event_array;
	for (int i = 0; i < n && i < MAXC; i++)
		out[i] = arr[(c0 + i) * 2 + 1];
	*count = c0;
	*counter = k0;
	return n;
}
// The API a Command_InstancePlace call serves: PerformColTest's caller (place_meeting, instance_place, place_empty,
// move_and_collide) or the direct callers (Command_IsMeeting, instance_place_list)
static uint32_t place_api_caller(void) {
	return place_caller >= 0xdcb77c && place_caller < 0xdcb960 ? pct_caller : place_caller;
}
static const char *caller_name(int api, uint32_t c) {
	if (api == API_PLACE) {
		if (c == 0xdcba58)
			return "place_meeting";
		if (c == 0xdd2200)
			return "instance_place";
		if (c == 0xdcb748)
			return "place_empty";
		if (c >= 0xdcc5c0 && c < 0xdcce00)
			return "move_and_collide";
		if (c == 0xf6688c)
			return "Command_IsMeeting";
		if (c >= 0xdd2214 && c < 0xdd2400)
			return "instance_place_list";
	}
	return "";
}
static Cands ca, cb_;
static void *pairs_a[MAXC], *pairs_b[MAXC];
static void verify_query(search_fn search, void *tree, void *rect, cb_fn cb, void *ctx, bool line) {
	void *s = shadow_of(tree);
	if (!s)
		return;
	int n = 0;
	ca.n = cb_.n = 0;
	search(tree, (void *)(uintptr_t)word(tree, 20), rect, &n, record_cb, &ca);
	n = 0;
	search(s, (void *)(uintptr_t)word(s, 20), rect, &n, record_cb, &cb_);
	if (line)
		crtv_line_queries++;
	else
		crtv_queries++;
	crtv_candidates += ca.n;
	if (ca.n > MAXC || cb_.n > MAXC)
		return;
	static void *sa[MAXC], *sb[MAXC];
	memcpy(sa, ca.c, ca.n * sizeof(void *));
	memcpy(sb, cb_.c, cb_.n * sizeof(void *));
	qsort(sa, ca.n, sizeof(void *), cmp_ptr);
	qsort(sb, cb_.n, sizeof(void *), cmp_ptr);
	if (ca.n != cb_.n || memcmp(sa, sb, ca.n * sizeof(void *))) {
		crtv_set_bad++;
		if (log_budget > 0) {
			log_budget--;
			debugPrintf("[CRTVERIFY] candidate set mismatch: live %d shadow %d (%s query)\n", ca.n, cb_.n,
				line ? "line" : "rect");
		}
	}
	if (!line && !compare_entries(tree, s, (const Rect *)rect, "query"))
		crtv_rect_bad++;
	if ((crtv_queries + crtv_line_queries) % 64 == 0) {
		if (compare_entries(tree, s, NULL, "full"))
			crtv_full_ok++;
		else
			crtv_full_bad++;
	}
	if (ca.n != cb_.n || !memcmp(ca.c, cb_.c, ca.n * sizeof(void *)))
		return;
	crtv_order_diff++;
	int api = -1;
	for (int i = 0; i < 3; i++)
		if ((uintptr_t)cb == first_hits[i].cb)
			api = i;
	if (api >= 0) {
		if (**(void ***)first_hits[api].list_slot) {
			crtv_list_diff++; // same list contents, other order
			return;
		}
		crtv_result_checked++;
		crtv_result_checked_api[api]++;
		void *a = first_hit(&ca, cb, ctx, &first_hits[api]), *b = first_hit(&cb_, cb, ctx, &first_hits[api]);
		if (a != b) {
			crtv_result_diff++;
			crtv_result_diff_api[api]++;
			// line/rectangle queries pass a parameter block as ctx, not the caller: self = the running instance
			void *self = api == API_PLACE ? ctx : query_self;
			note_diff(api, api == API_PLACE ? place_api_caller() : 0, self ? (int)word(self, 0x84) : -1,
				a ? (int)word(a, 0x84) : -1, b ? (int)word(b, 0x84) : -1);
		}
	} else if ((uintptr_t)cb == event_cb) {
		crtv_result_checked++;
		crtv_result_checked_api[API_EVENT]++;
		int na = event_pairs(&ca, cb, ctx, pairs_a), nb = event_pairs(&cb_, cb, ctx, pairs_b);
		if (na != nb || memcmp(pairs_a, pairs_b, (na < MAXC ? na : MAXC) * sizeof(void *))) {
			crtv_result_diff++;
			crtv_result_diff_api[API_EVENT]++;
			int i = 0;
			while (i < na && i < nb && pairs_a[i] == pairs_b[i])
				i++;
			note_diff(API_EVENT, (uint32_t)(na * 100 + nb), (int)word(ctx, 0x84), i < na ? (int)word(pairs_a[i], 0x84) : -1,
				i < nb ? (int)word(pairs_b[i], 0x84) : -1);
		}
	} else
		crtv_unknown_cb_diff++;
}
typedef struct { uint32_t cb; unsigned n; } CbCount;
static CbCount cbs[16];
static void count_cb(cb_fn cb) {
	uint32_t a = (uint32_t)(uintptr_t)cb - bias;
	for (int i = 0; i < 16; i++) {
		if (cbs[i].cb == a || !cbs[i].n) {
			cbs[i].cb = a;
			cbs[i].n++;
			return;
		}
	}
}
static int in_verify;
static int AAPCS search_hook(void *tree, void *node, void *rect, int *count, cb_fn cb, void *ctx) {
	if (crt_verify && !in_verify && !in_shadow_op && node && node == (void *)(uintptr_t)word(tree, 20) && is_live(tree)) {
		in_verify++;
		count_cb(cb);
		verify_query(original_search, tree, rect, cb, ctx, false);
		in_verify--;
	}
	return original_search(tree, node, rect, count, cb, ctx);
}
static int AAPCS search_line_hook(void *tree, void *node, void *line, int *count, cb_fn cb, void *ctx) {
	if (crt_verify && !in_verify && !in_shadow_op && node && node == (void *)(uintptr_t)word(tree, 20) && is_live(tree)) {
		in_verify++;
		count_cb(cb);
		verify_query(original_search_line, tree, line, cb, ctx, true);
		in_verify--;
	}
	return original_search_line(tree, node, line, count, cb, ctx);
}
static void *AAPCS place_hook(void *inst, float x, float y, int obj, void *list) {
	uint32_t saved = place_caller;
	place_caller = (uint32_t)(uintptr_t)__builtin_return_address(0) - bias;
	void *r = original_place(inst, x, y, obj, list);
	place_caller = saved;
	return r;
}
typedef uint64_t AAPCS (*pct_fn)(void *inst, float x, float y, void *obj);
static pct_fn original_pct;
static uint64_t AAPCS pct_hook(void *inst, float x, float y, void *obj) {
	uint32_t saved = pct_caller;
	pct_caller = (uint32_t)(uintptr_t)__builtin_return_address(0) - bias;
	uint64_t r = original_pct(inst, x, y, obj);
	pct_caller = saved;
	return r;
}

typedef void *AAPCS (*cline_fn)(void *inst, float a, float b, float c, float d, int domain, int obj, bool prec,
	bool notme, void *list);
static cline_fn original_line, original_rect;
static void *AAPCS line_hook(void *inst, float a, float b, float c, float d, int domain, int obj, bool prec, bool notme,
	void *list) {
	void *saved = query_self;
	query_self = inst;
	void *r = original_line(inst, a, b, c, d, domain, obj, prec, notme, list);
	query_self = saved;
	return r;
}
static void *AAPCS rect_hook(void *inst, float a, float b, float c, float d, int domain, int obj, bool prec, bool notme,
	void *list) {
	void *saved = query_self;
	query_self = inst;
	void *r = original_rect(inst, a, b, c, d, domain, obj, prec, notme, list);
	query_self = saved;
	return r;
}

typedef void *AAPCS (*legacy_fn)(void *self, void *other, void *out, int argc, int id, void **args);
static legacy_fn original_legacy;
static void *AAPCS legacy_hook(void *self, void *other, void *out, int argc, int id, void **args) {
	uint32_t saved = gml_site;
	gml_site = (uint32_t)(uintptr_t)__builtin_return_address(0) - bias;
	void *r = original_legacy(self, other, out, argc, id, args);
	gml_site = saved;
	return r;
}

// ---- Report (katana_prof.c, every window) ----
static const char *oname(int obj) {
	const char *n = obj >= 0 && object_name ? object_name(obj) : NULL;
	return n ? n : "?";
}
void crt_report(int frames) {
	if (!installed || frames <= 0)
		return;
	float f = (float)frames;
	if (crt_research)
		crt_stat = exp_flag("crtstat");
	debugPrintf("[CRT] updates %.1f no-op %.1f skipped %.1f per frame | drains %.1f (%.3f ms) | trees built %u%s%s\n",
		crt_updates / f, crt_noop / f, crt_skipped / f, crt_drains / f, crt_drain_us / 1000.0f / f, crt_ctors,
		collision_round_trip ? "" : " | off",
		crt_ab ? (crt_ab_off ? " | A/B: off" : " | A/B: on") : "");
	crt_updates = crt_noop = crt_skipped = crt_drains = crt_drain_us = crt_ctors = 0;
	if (crt_ab)
		crt_ab_off = !crt_ab_off;
	static int windows;
	if (crt_stat && ++windows % 5 == 0) {
		// top 16 by runner time (skipped updates have none: shown by count)
		debugPrintf("[CRTSTAT] marks %.1f first %.1f per frame (last window); per object/first-mark site since last dump:\n",
			crt_marks / f, crt_first_marks / f);
		for (int k = 0; k < 16; k++) {
			int best = -1;
			unsigned bt = 0;
			for (int i = 0; i < NSTAT; i++) {
				unsigned t = stats[i].noop_us + stats[i].move_us + stats[i].skipped * 6;
				if ((stats[i].noop || stats[i].move || stats[i].skipped) && t >= bt) {
					bt = t;
					best = i;
				}
			}
			if (best < 0)
				break;
			Stat *s = &stats[best];
			debugPrintf("[CRTSTAT] %-28s site %07x %s no-op %u (%u us) skipped %u move %u (%u us)\n", oname(s->obj), s->lr,
				s->drain ? "drain" : "direct", s->noop, s->noop_us, s->skipped, s->move, s->move_us);
			s->noop = s->move = s->skipped = 0; // consumed; cleared below
			s->noop_us = s->move_us = 0;
			s->obj = -2;
		}
		memset(stats, 0, sizeof(stats));
	}
	crt_marks = crt_first_marks = 0;
	if (crt_verify) {
		for (int i = 0; i < 3; i++)
			if (collision_trees[i] && shadow_of(collision_trees[i])) {
				if (compare_entries(collision_trees[i], shadow_of(collision_trees[i]), NULL, "report"))
					crtv_full_ok++;
				else
					crtv_full_bad++;
			}
		debugPrintf("[CRTVERIFY] queries %u rect %u line, candidates %u | set mismatch %u, rect mismatch %u, full ok %u "
			"mismatch %u | order differs %u, first hit checked %u (place %u line %u rect %u event %u) differs %u (place %u "
			"line %u rect %u event %u), lists %u, boolean/other callbacks %u | unshadowed %u%s\n", crtv_queries,
			crtv_line_queries, crtv_candidates, crtv_set_bad, crtv_rect_bad, crtv_full_ok, crtv_full_bad, crtv_order_diff,
			crtv_result_checked, crtv_result_checked_api[0], crtv_result_checked_api[1], crtv_result_checked_api[2],
			crtv_result_checked_api[3], crtv_result_diff, crtv_result_diff_api[0], crtv_result_diff_api[1],
			crtv_result_diff_api[2], crtv_result_diff_api[3], crtv_list_diff, crtv_unknown_cb_diff, crtv_unshadowed,
			crt_fault ? " | FAULT" : "");
		if (++windows % 15 == 0) {
			for (int i = 0; i < 16 && cbs[i].n; i++)
				debugPrintf("[CRTVERIFY] callback %07x: %u queries\n", cbs[i].cb, cbs[i].n);
			for (int i = 0; i < NDIFF && diffs[i].n; i++) {
				const Diff *d = &diffs[i];
				if (d->api == API_EVENT)
					debugPrintf("[CRTVERIFY] differs x%u: %s order, %u/%u pairs, self %s, event %d/%d, first differing "
						"other: live %s original %s\n", d->n, api_names[d->api], d->caller / 100, d->caller % 100,
						oname(d->self), d->ev_type, d->ev_num, oname(d->a), oname(d->b));
				else
					debugPrintf("[CRTVERIFY] differs x%u: %s %s (caller %07x, gml %07x), self %s, event %d/%d: live %s "
						"original %s\n", d->n, api_names[d->api], caller_name(d->api, d->caller), d->caller, d->site,
						oname(d->self), d->ev_type, d->ev_num, oname(d->a), oname(d->b));
			}
		}
	}
}

// ---- Install ----
extern void *katana_rx_alloc(size_t size);
// Entry hooks with own trampolines; the two displaced words must be position-independent (checked by the caller).
static bool hook_entries(int n, const uintptr_t *entry, void *const *replacement, void **original) {
	uint32_t *rx = katana_rx_alloc(16 * n);
	if (!rx)
		return false;
	for (int i = 0; i < n; i++) {
		const uint32_t *w = (const uint32_t *)entry[i];
		uint32_t code[4] = { w[0], w[1], 0xe51ff004, (uint32_t)(entry[i] + 8) };
		kuKernelCpuUnrestrictedMemcpy(rx + 4 * i, code, sizeof(code));
	}
	kuKernelFlushCaches(rx, 16 * n);
	for (int i = 0; i < n; i++) {
		original[i] = rx + 4 * i;
		hook_addr(entry[i], (uintptr_t)replacement[i]);
		kuKernelFlushCaches((void *)entry[i], 8);
	}
	return true;
}
static bool std_prologue(uintptr_t e) {
	const uint32_t *w = (const uint32_t *)e;
	return e && !(e & 3) && (w[0] & 0xffff4000) == 0xe92d4000 && (w[1] & 0xfffff000) == 0xe28db000;
}

// Called by move_contact_native_install after its own hooks (they call crt_update / crt_insert), with the trampolines
// of the runner's CollisionUpdate and InsertRect.
int crt_install(uintptr_t runner_bias, void *insert_original) {
	if (installed)
		return 0;
	bias = runner_bias;
	original_insert = (insert_rect_fn)insert_original;
	uintptr_t ct = so_symbol(&yoyoloader_mod, "_ZN5RTreeIP9CInstanceffLi6ELi2EEC2Ei");
	uintptr_t dt = so_symbol(&yoyoloader_mod, "_ZN5RTreeIP9CInstanceffLi6ELi2EED2Ev");
	uintptr_t rm = so_symbol(&yoyoloader_mod, "_ZN5RTreeIP9CInstanceffLi6ELi2EE6RemoveEPKfS4_RKS1_b");
	collision_trees = (void **)so_symbol(&yoyoloader_mod, "g_collision_trees");
	add_all_objects = (const uint8_t *)so_symbol(&yoyoloader_mod, "g_fast_collision_add_all_objects");
	managed_objects = (const uint8_t *)so_symbol(&yoyoloader_mod, "g_enableManagedObjects");
	dirty_list = (uintptr_t *)so_symbol(&yoyoloader_mod, "s_dirty_list");
	object_name = (name_fn)so_symbol(&yoyoloader_mod, "_Z11Object_Namei");
	if (ct - bias != 0xf6be50 || dt - bias != 0xf6d384 || rm - bias != 0xf6c2b4 || !collision_trees || !add_all_objects ||
		!managed_objects || !dirty_list || !std_prologue(ct) || !std_prologue(dt) || !std_prologue(rm))
		return 0;
	// CollisionUpdate's early-return tests as read above (f6c420..f6c4e0)
	static const uint32_t expect[][2] = {
		{ 0xf6c428, 0xe3150902 }, { 0xf6c430, 0xe3002003 }, { 0xf6c434, 0xe3402010 }, { 0xf6c440, 0xe5902068 },
		{ 0xf6c494, 0xe5d42004 }, { 0xf6c4a4, 0xe5923050 }, { 0xf6c4a8, 0xe3130010 }, { 0xf6c4b0, 0xe3130028 },
		{ 0xf6c4cc, 0xe3120602 }, { 0xf6c5f0, 0xe2123020 }, { 0xf6c584, 0xe3150902 },
	};
	for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++)
		if (word((void *)(bias + expect[i][0]), 0) != expect[i][1])
			return 0;
	crt_ab = exp_flag("crtab");
	crt_prof = exp_flag("crtprof");
	crt_research = exp_flag("crtres");
	crt_stat = crt_research && exp_flag("crtstat");
	crt_verify = exp_flag("crtverify");
	crt_fault = exp_flag("crtverifyfault");
	void *originals[3];
	const uintptr_t e1[3] = { ct, dt, rm };
	void *const r1[3] = { (void *)tree_ctor, (void *)tree_dtor, (void *)tree_remove };
	if (!hook_entries(crt_verify ? 3 : 1, e1, r1, originals))
		return 0;
	original_ctor = (ctor_fn)originals[0];
	if (crt_verify) {
		original_dtor = (dtor_fn)originals[1];
		original_remove = (remove_fn)originals[2];
	} else {
		original_dtor = (dtor_fn)dt;
		original_remove = (remove_fn)rm;
	}
	if (crt_prof || crt_research) {
		uintptr_t ut = so_symbol(&yoyoloader_mod, "_Z10UpdateTreev");
		uintptr_t md = so_symbol(&yoyoloader_mod, "_Z18CollisionMarkDirtyP9CInstance");
		// CollisionMarkDirty starts ldr r1, [r0, #104]; cmp r1, #0 (position-independent)
		if (ut - bias == 0xf6ca58 && std_prologue(ut) && md - bias == 0xf6bf40 && word((void *)md, 0) == 0xe5901068 &&
			word((void *)md, 4) == 0xe3510000) {
			const uintptr_t e2[2] = { ut, md };
			void *const r2[2] = { (void *)update_tree, (void *)mark_dirty };
			if (hook_entries(crt_research ? 2 : 1, e2, r2, originals)) {
				original_update_tree = (void_fn)originals[0];
				if (crt_research)
					original_mark = (mark_fn)originals[1];
			}
		}
	}
	if (crt_verify) {
		uintptr_t se = so_symbol(&yoyoloader_mod,
			"_ZN5RTreeIP9CInstanceffLi6ELi2EE6SearchEPNS2_4NodeEPNS2_4RectERiPFbS1_PvES8_");
		uintptr_t sl = so_symbol(&yoyoloader_mod,
			"_ZN5RTreeIP9CInstanceffLi6ELi2EE10SearchLineEPNS2_4NodeEPNS2_4LineERiPFbS1_PvES8_");
		uintptr_t pl = so_symbol(&yoyoloader_mod, "_Z21Command_InstancePlaceP9CInstanceffiP8CDS_List");
		uintptr_t pc = so_symbol(&yoyoloader_mod, "_Z14PerformColTestP9CInstanceffP6RValue");
		uintptr_t lg = so_symbol(&yoyoloader_mod, "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
		uintptr_t cl = so_symbol(&yoyoloader_mod,
			"_Z21Command_CollisionLineP9CInstanceffff15CollisionDomainibbP8CDS_List");
		uintptr_t cr = so_symbol(&yoyoloader_mod,
			"_Z26Command_CollisionRectangleP9CInstanceffff15CollisionDomainibbP8CDS_List");
		event_type = (const int *)so_symbol(&yoyoloader_mod, "Current_Event_Type");
		event_number = (const int *)so_symbol(&yoyoloader_mod, "Current_Event_Number");
		// Callbacks and their globals, from the literal pools (pc-relative loads checked below):
		// Command_InstancePlace: callback add r0, pc, r0 at f65c74; result/list GOT slots ldr r8/r6, [pc, rX] at
		// f65c10/f65c1c. Command_CollisionLine's callback f65128: list slot ldr at f651ec, result slot at f652d8.
		// Command_CollisionRectangle's callback f64718: list slot at f647ec, result slot at f648e0. collisionResult:
		// counter GOT slot at f6b90c, pair array pointer at f6b970, count at f6b980/f6b9d0/f6b9dc (static data).
		static const uint32_t ex[][2] = {
			{ 0xf65c74, 0xe08f0000 }, { 0xf65c10, 0xe79f8008 }, { 0xf65c1c, 0xe79f6006 }, { 0xf651e8, 0xe59f010c },
			{ 0xf651ec, 0xe79f0000 }, { 0xf652d4, 0xe59f0024 }, { 0xf652d8, 0xe79f0000 }, { 0xf647e8, 0xe59f0110 },
			{ 0xf647ec, 0xe79f0000 }, { 0xf648d8, 0xe59f0024 }, { 0xf648e0, 0xe79f0000 }, { 0xf6b90c, 0xe79f0000 },
			{ 0xf6b970, 0xe79f0000 }, { 0xf6b980, 0xe79f1001 }, { 0xf6b9d0, 0xe79f1001 }, { 0xf6b9dc, 0xe08f2002 },
			{ 0xf6b9d8, 0xe7a05181 }, { 0xf6b9e0, 0xe5804004 },
		};
		bool ok = true;
		for (size_t i = 0; i < sizeof(ex) / sizeof(ex[0]); i++)
			ok = ok && word((void *)(bias + ex[i][0]), 0) == ex[i][1];
		uintptr_t B = bias;
#define LIT(at) word((void *)(B + (at)), 0)
		first_hits[API_PLACE] = (FirstHit){ B + 0xf65c7c + LIT(0xf65f70), B + 0xf65c18 + LIT(0xf65f68),
			B + 0xf65c24 + LIT(0xf65f6c) };
		first_hits[API_LINE] = (FirstHit){ B + 0xf65128, B + 0xf652e0 + LIT(0xf65300), B + 0xf651f4 + LIT(0xf652fc) };
		first_hits[API_RECT] = (FirstHit){ B + 0xf64718, B + 0xf648e8 + LIT(0xf64904), B + 0xf647f4 + LIT(0xf64900) };
		event_cb = B + 0xf6b8f0;
		event_counter_slot = B + 0xf6b914 + LIT(0xf6b9f8);
		event_array = B + 0xf6b978 + LIT(0xf6ba00);
		event_count = B + 0xf6b988 + LIT(0xf6ba08);
		ok = ok && event_count == B + 0xf6b9d8 + LIT(0xf6ba18) && event_count == B + 0xf6b9e4 + LIT(0xf6ba1c);
#undef LIT
		if (ok && se - bias == 0xdd6224 && sl - bias == 0xf6afc4 && pl - bias == 0xf65aec && pc - bias == 0xdcb77c &&
			std_prologue(se) && std_prologue(sl) && std_prologue(pl) && std_prologue(pc) && std_prologue(cl) &&
			std_prologue(cr) && std_prologue(lg) && first_hits[API_PLACE].cb == bias + 0xf65f80 && event_type && event_number) {
			const uintptr_t e3[7] = { se, sl, pl, pc, cl, cr, lg };
			void *const r3[7] = { (void *)search_hook, (void *)search_line_hook, (void *)place_hook, (void *)pct_hook,
				(void *)line_hook, (void *)rect_hook, (void *)legacy_hook };
			void *o3[7];
			if (hook_entries(7, e3, r3, o3)) {
				original_legacy = (legacy_fn)o3[6];
				original_search = (search_fn)o3[0];
				original_search_line = (search_fn)o3[1];
				original_place = (place_fn)o3[2];
				original_pct = (pct_fn)o3[3];
				original_line = (cline_fn)o3[4];
				original_rect = (cline_fn)o3[5];
			} else
				crt_verify = 0;
		} else
			crt_verify = 0;
		debugPrintf("collision round trips: verify %s\n", crt_verify ? "on" : "NOT available");
	}
	installed = 1;
	return 1;
}
