/*
 * smoke_collog.c (debug: exp_smokecollog, debugMode)
 *
 * What smoke clouds' collision queries return, to compare smokeWobble=1 with the original (smokedraw_native.c). The
 * wobble change keeps every box and tree rectangle but not the order of the collision tree's leaves, and the order
 * decides only which instance a first-hit query returns when several instances match. So this logs every
 * place_meeting and instance_place made with a cloud as self (its Step's three contact gates and fx_rate.c's copies
 * of them) and, for each instance_place, how many instances could have been the answer: every instance of the target
 * object (instance_find) tested alone with place_meeting(x, y, id). One candidate: the result can't depend on the
 * order. Two or more: it can (counted, the first cases printed).
 * Also counted for instance_place calls by anything else (any target with at most 64 instances), since the clouds
 * share the tree with everything in their domain.
 * Per report window ([SMOKECOL]): queries and hits by kind, instance_place calls with 2+ candidates, and an FNV hash of
 * every cloud query (frame, cloud id, function, target, x, y, result) so two runs can be compared window by window.
 * The registry entries are swapped on the first cloud Draw (the_functions is filled after the loader installs).
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

extern int debugMode;

static runner_function **functions, **current_function;
static const int *fn_place_meeting, *fn_instance_place, *fn_instance_find;
static builtin_fn real_place_meeting, real_instance_place, real_instance_find;
static int (*object_find)(const char *name);
static const uint32_t *frame;
static int enabled, swapped, cloud_object = -2, depth;

static unsigned q_pm, q_pm_hit, q_ip, q_ip_hit, q_ip_ambiguous, q_ip_candidates;
static unsigned o_ip, o_ip_ambiguous, o_ip_unchecked;
static unsigned total_ambiguous, total_o_ambiguous, printed;
static uint32_t hash = 2166136261u;

static void mix(uint32_t v) {
	for (int i = 0; i < 4; i++) {
		hash ^= (v >> (8 * i)) & 0xff;
		hash *= 16777619u;
	}
}

static inline uint32_t word(const void *p, size_t o) {
	uint32_t v;
	memcpy(&v, (const uint8_t *)p + o, 4);
	return v;
}

static void call(builtin_fn f, int index, gml_rvalue *out, void *self, int argc, gml_rvalue *args) {
	runner_function *entry = *functions + index, *saved = *current_function;
	*current_function = entry;
	f(out, self, self, argc, args);
	*current_function = saved;
}
// An instance id or object index out of a result or argument: a real, or a reference's low word; -4 (noone) if neither
static int as_id(const gml_rvalue *v) {
	uint32_t kind = v->kind & 0xffffff;
	if (kind == 0 || kind == 13)
		return (int)v->val;
	if (kind == 15 || kind == 7 || kind == 10) {
		int32_t lo;
		memcpy(&lo, &v->val, 4);
		return lo;
	}
	return -4;
}
static int truth(const gml_rvalue *v) {
	uint32_t kind = v->kind & 0xffffff;
	return (kind == 0 || kind == 13) && v->val > 0.5;
}

// Instances of the target that place_meeting(x, y, id) finds alone, up to 64; -1 if there are more instances
static int candidates(void *self, const gml_rvalue *args, int *ids, int max) {
	int hits = 0;
	for (int n = 0; n < 64; n++) {
		gml_rvalue fa[2] = {args[2], {(double)n, 0, 0}}, found = {0, 0, 5};
		call(real_instance_find, fn_instance_find[1], &found, self, 2, fa);
		int id = as_id(&found);
		if (id < 0)
			return hits;
		gml_rvalue pa[3] = {args[0], args[1], {(double)id, 0, 0}}, hit = {0, 0, 5};
		call(real_place_meeting, fn_place_meeting[1], &hit, self, 3, pa);
		if (truth(&hit)) {
			if (hits < max)
				ids[hits] = id;
			hits++;
		}
	}
	return -1;
}

static int is_cloud(void *self) {
	if (cloud_object == -2)
		cloud_object = object_find("obj_smokecloud");
	return self && (int32_t)word(self, 0x84) == cloud_object;
}

static void log_query(void *self, int fn, const gml_rvalue *args, const gml_rvalue *result) {
	mix(*frame);
	mix(word(self, 0x80));
	mix((uint32_t)fn);
	mix((uint32_t)as_id(&args[2]));
	float x = (float)args[0].val, y = (float)args[1].val;
	uint32_t xb, yb;
	memcpy(&xb, &x, 4);
	memcpy(&yb, &y, 4);
	mix(xb);
	mix(yb);
	mix(fn ? (uint32_t)as_id(result) : (uint32_t)truth(result));
}

static void wrap_place_meeting(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	real_place_meeting(result, self, other, argc, args);
	if (depth || argc != 3 || !is_cloud(self))
		return;
	q_pm++;
	q_pm_hit += truth(result);
	log_query(self, 0, args, result);
}

/*
 * Which enemy a cloud picks matters to the enemy: the cloud's Step sets the picked one's whip_timer to 30 and its
 * first_attack_timer to at least 20, which hold off its close attack. Per enemy that was a candidate of some cloud's
 * instance_place in a frame: picked by at least one cloud that frame or not. A run of frames as a candidate without
 * being picked is when its timers could run down where another order would have refreshed them; the longest run is
 * reported (whip_timer lasts 30 frames).
 */
#define ENEMIES 64
static struct { int id; uint32_t cand_frame, pick_frame; unsigned streak; } enemies[ENEMIES];
static uint32_t cur_frame = (uint32_t)-1;
static unsigned cand_frames, unpicked_frames, longest_streak, session_longest, streak_over[3]; // > 5, 15, 30 frames
static void close_frame(void) {
	for (int i = 0; i < ENEMIES; i++) {
		if (!enemies[i].id || enemies[i].cand_frame != cur_frame)
			continue;
		cand_frames++;
		if (enemies[i].pick_frame == cur_frame) {
			enemies[i].streak = 0;
			continue;
		}
		unpicked_frames++;
		unsigned s = ++enemies[i].streak;
		if (s > longest_streak)
			longest_streak = s;
		if (s > session_longest)
			session_longest = s;
		streak_over[0] += s == 6;
		streak_over[1] += s == 16;
		streak_over[2] += s == 31;
	}
}
static int enemy_slot(int id) {
	int free_slot = -1;
	for (int i = 0; i < ENEMIES; i++) {
		if (enemies[i].id == id)
			return i;
		if (free_slot < 0 && (!enemies[i].id || cur_frame - enemies[i].cand_frame > 600))
			free_slot = i;
	}
	if (free_slot >= 0) {
		enemies[free_slot].id = id;
		enemies[free_slot].streak = 0;
		enemies[free_slot].cand_frame = enemies[free_slot].pick_frame = (uint32_t)-2;
	}
	return free_slot;
}
static void note_pick(const int *ids, int n, int picked) {
	if (*frame != cur_frame) {
		if (cur_frame != (uint32_t)-1)
			close_frame();
		cur_frame = *frame;
	}
	for (int k = 0; k < n && k < 8; k++) {
		int s = enemy_slot(ids[k]);
		if (s < 0)
			continue;
		enemies[s].cand_frame = cur_frame;
		if (ids[k] == picked)
			enemies[s].pick_frame = cur_frame;
	}
}

static void wrap_instance_place(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	real_instance_place(result, self, other, argc, args);
	if (depth || argc != 3)
		return;
	depth++;
	gml_rvalue saved = *result; // the candidate queries leave it alone, but keep the caller's result exact
	int cloud = is_cloud(self), ids[8];
	int n = candidates(self, args, ids, 8);
	*result = saved;
	depth--;
	if (cloud && n > 0)
		note_pick(ids, n, as_id(result));
	if (cloud) {
		q_ip++;
		q_ip_hit += as_id(result) >= 0;
		log_query(self, 1, args, result);
		if (n >= 2) {
			q_ip_ambiguous++;
			total_ambiguous++;
		}
		if (n > 0)
			q_ip_candidates += (unsigned)n;
	} else {
		o_ip++;
		if (n < 0)
			o_ip_unchecked++;
		else if (n >= 2) {
			o_ip_ambiguous++;
			total_o_ambiguous++;
		}
	}
	if (n >= 2 && printed < 24) {
		printed++;
		debugPrintf("[SMOKECOL] frame %u: instance_place by %s %u (object %d) at %.2f,%.2f on object %d: %d candidates "
			"(%d, %d%s), returned %d\n", *frame, cloud ? "cloud" : "instance", word(self, 0x80), (int32_t)word(self, 0x84),
			args[0].val, args[1].val, as_id(&args[2]), n, ids[0], ids[1], n > 2 ? ", ..." : "", as_id(result));
	}
}

void smoke_collog_enable(void) {
	if (!debugMode)
		return;
	functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	current_function = (runner_function **)so_symbol(&yoyoloader_mod, "g_pFunction");
	fn_place_meeting = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_place_meeting");
	fn_instance_place = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_instance_place");
	fn_instance_find = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_instance_find");
	object_find = (int (*)(const char *))so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
	frame = (const uint32_t *)so_symbol(&yoyoloader_mod, "g_IOFrameCount");
	enabled = functions && current_function && fn_place_meeting && fn_instance_place && fn_instance_find &&
		object_find && frame;
	debugPrintf("smoke collision log: %s\n", enabled ? "on" : "NOT available");
}

// On each cloud Draw: swap the registry entries once they exist
void smoke_collog_frame(void) {
	if (!enabled || swapped || !*functions)
		return;
	runner_function *pm = *functions + fn_place_meeting[1], *ip = *functions + fn_instance_place[1],
		*find = *functions + fn_instance_find[1];
	if (!pm->func || !ip->func || !find->func)
		return;
	real_place_meeting = (builtin_fn)pm->func;
	real_instance_place = (builtin_fn)ip->func;
	real_instance_find = (builtin_fn)find->func;
	builtin_chain((void **)&pm->func, (void *)wrap_place_meeting); // chained: fx_rate.c swaps place_meeting in its
	builtin_chain((void **)&ip->func, (void *)wrap_instance_place); // verify, and these pass every call on
	swapped = 1;
	debugPrintf("smoke collision log: place_meeting and instance_place wrapped\n");
}

void smoke_collog_report(int frames) {
	(void)frames;
	if (!swapped)
		return;
	debugPrintf("[SMOKECOL] clouds: place_meeting %u (%u hit), instance_place %u (%u hit, %u candidates, %u with 2+); "
		"others: instance_place %u (%u with 2+, %u unchecked); hash %08x; session 2+: clouds %u, others %u\n", q_pm,
		q_pm_hit, q_ip, q_ip_hit, q_ip_candidates, q_ip_ambiguous, o_ip, o_ip_ambiguous, o_ip_unchecked, hash,
		total_ambiguous, total_o_ambiguous);
	debugPrintf("[SMOKECOL] enemies: %u candidate-frames, %u not picked by any cloud, longest run %u frames; session: "
		"longest %u, runs over 5/15/30 frames %u/%u/%u\n", cand_frames, unpicked_frames, longest_streak, session_longest,
		streak_over[0], streak_over[1], streak_over[2]);
	cand_frames = unpicked_frames = longest_streak = 0;
	q_pm =q_pm_hit = q_ip = q_ip_hit = q_ip_ambiguous = q_ip_candidates = o_ip = o_ip_ambiguous = o_ip_unchecked = 0;
	hash = 2166136261u;
}
