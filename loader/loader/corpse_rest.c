/*
 * corpse_rest.c
 *
 * Bodies at rest (obj_enemycorpse). A body stays in the room once it has stopped, and its Step (43 KB of YYC) and
 * its user event 14 (from obj_drawactors' Draw, every body every frame) keep running. For a body that has settled
 * (dead, animation on its last frame, no fire left to spread, no explosion anywhere, not moved since the last tick)
 * the Step's writes all put back the values that are already there. What it still does that matters:
 *   - ground_meeting(x, y + 1): collision queries, which move the body to y + 1 and back (that marks it dirty for the
 *     collision tree) and can drain the tree's dirty list. It runs here too, through the real script, at the point
 *     of the event order where the Step would have called it;
 *   - with its answer, begin_animation(ground_sprite or fly_sprite, 0.3, false) and, on the ground, bloodend_timer
 *     counting up to 100;
 *   - with time_out on, the countdown timeout_timer -= 1 * global.fr.
 * Those are done here the way the Step does them (the same double arithmetic; begin_animation through the real script
 * unless it would only write the same sprite and speed again), and the Step itself is not run. Everything else the
 * Step reads at rest is checked first (pure reads, YYCompareVal semantics with the runner's epsilon), and anything
 * outside the proven fixed point runs the Step:
 *   - animation_index == animation_max (the pistol-death blood branch is off), just_hit already bool false, dead true,
 *     mask_index already spr_corpsemask, slope and first_frame already bool false, x/y equal to xprevious/yprevious,
 *     region != -1, global.fr a real;
 *   - not on fire: no obj_explosion instance (place_meeting has then nothing to find and touches nothing); on fire:
 *     fire_alpha exactly 1 (the +0.01 clamps back to 1) and fire_timer >= 3 (no more embers);
 *   - time_out: the countdown stays above 100 after this tick's step (the blink and the instance_destroy run the Step).
 * User event 14 for a body is chest_hit/roll_hit sounds at given frames of the hurt-ground and fall sprites (each test
 * needs animation_float == 0), animation_code() and ai_set_hair() when has_hair. When chest_hit and roll_hit are
 * already bool false, animation_float isn't 0 and has_hair is false, the event is only animation_code(), which runs
 * here through the real script (any body, moving or not).
 *
 * exp_corpserest0 runs the original events. Debug: exp_corpserestverify runs both versions of each skipped event on the
 * same starting state (the variables, CInstance fields 0x84-0x150 and the random number state put back in between)
 * and compares every instance variable (kind and value; flags of reals and bools aren't compared, the Step writes
 * stack garbage there), the fields, the random number state and room_maxid. The original runs second, so the game
 * keeps the original's result (the body's collision query then runs twice). exp_corpserestfault1 (Step) / exp_corpserestfault2 (user 14)
 * are negative controls. exp_corpserestab switches the skips off and on every report window; exp_corpserestclass
 * times the original events of bodies at rest and of the others (with the skips off). Report: [CORPSEREST].
 */

#include <vitasdk.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

typedef gml_rvalue *(*read_var_fn)(void *obj, int id);
typedef int (*compare_fn)(const gml_rvalue *a, const gml_rvalue *b, double eps, int mode);
typedef int (*bool_fn)(const gml_rvalue *v);
typedef void (*free_fn)(gml_rvalue *v);
typedef int (*builtin_var_fn)(void *inst, int index, gml_rvalue *value);
typedef int (*find_fn)(const char *name);

#define NO_INDEX ((int)0x80000000)
#define KIND(v) ((v)->kind & 0xffffff)
#define REFCOUNTED(k) ((1u << ((k) & 31)) & 0x46u) // string, array, object: the kinds YYC frees and addrefs

static read_var_fn read_var;
static compare_fn yy_compare;
static bool_fn yy_bool;
static free_fn yy_free;
static builtin_var_fn gv_x, gv_y, gv_sprite;
static builtin_fn instance_exists_fn;
static find_fn object_find, sprite_find;
static gml_script_fn ground_meeting, begin_animation, animation_code;
static const double *epsilon;
static const gml_rvalue *const_speed, *const_false; // the Step's begin_animation constants: 0.3 and false
static uint32_t *rng_state, *rng_index;
static int *room_maxid;

#define CR_VARS(X) X(animation_index) X(animation_max) X(animation_float) X(animation_speed) X(just_hit) X(on_fire) \
	X(fire_alpha) X(fire_timer) X(dead) X(slope) X(region) X(time_out) X(timeout_timer) X(first_frame) \
	X(bloodend_timer) X(ground_sprite) X(fly_sprite) X(chest_hit) X(roll_hit) X(has_hair) X(fr)
#define CR_VAR_DECL(name) static int *v_##name;
CR_VARS(CR_VAR_DECL)

static event_func orig_step, orig_u14;
static int installed, resolved, disabled, verify, fault, ab, ab_off, classify;
static int obj_explosion = -1, spr_corpsemask = -1;

// Debug counters (per report window unless noted)
static unsigned n_step_fast, n_step_orig, n_u14_fast, n_u14_orig;
static uint32_t step_us, u14_us, ground_us, class_rest_us, class_other_us, class_u14_rest_us, class_u14_other_us;
static unsigned class_rest, class_other, class_u14_rest, class_u14_other;
static unsigned verify_steps, verify_step_bad, verify_u14, verify_u14_bad, verify_skipped; // session
static unsigned bad_vars, bad_fields, bad_rng, bad_maxid; // session
#define CR_LINES 16
static unsigned reject_line[CR_LINES], reject_count[CR_LINES]; // session
static void reject(unsigned line) {
	for (int i = 0; i < CR_LINES; i++) {
		if (reject_line[i] == line || !reject_line[i]) {
			reject_line[i] = line;
			reject_count[i]++;
			return;
		}
	}
}
#define NO(cond) do { if (cond) { if (debugMode) reject(__LINE__); return 0; } } while (0)

static gml_rvalue *var(void *self, int *v) {
	return read_var(self, v[1]);
}

static int number(const gml_rvalue *v, double *out) {
	if (!v || (KIND(v) != 0 && KIND(v) != 13) || isnan(v->val))
		return 0;
	*out = v->val;
	return 1;
}

static int bool_false(const gml_rvalue *v) { // the value `x = false` leaves: kind bool, payload +0.0
	uint64_t bits;
	if (!v || KIND(v) != 13)
		return 0;
	memcpy(&bits, &v->val, 8);
	return bits == 0;
}

// YYCompareVal(a, b) for two numbers: 0 within the runner's epsilon, else the sign of a - b. 2: not two plain numbers
// (the Step runs then).
static int compare(const gml_rvalue *a, const gml_rvalue *b) {
	double x, y;
	if (!number(a, &x) || !number(b, &y))
		return 2;
	double d = x - y;
	if (isnan(d))
		return 2;
	return fabs(d) <= *epsilon ? 0 : d < 0 ? -1 : 1;
}
static int compare_real(const gml_rvalue *a, double b) {
	gml_rvalue r = {b, 0, 0};
	return compare(a, &r);
}

// BOOL_RValue for a number or bool (> 0.5); -1 for anything else
static int truth(const gml_rvalue *v) {
	double x;
	return number(v, &x) ? x > 0.5 : -1;
}

static int instance_exists(void *self, void *other, int object) {
	gml_rvalue result = {0, 0, 5}, arg;
	uint32_t ref[4] = {(uint32_t)object, 0x1000000, 0, 15}; // an object reference, as YYC passes obj_* constants
	memcpy(&arg, ref, sizeof(arg));
	instance_exists_fn(&result, self, other, 1, &arg);
	return truth(&result);
}

static float field_f(void *self, int off) {
	float f;
	memcpy(&f, (char *)self + off, 4);
	return f;
}

static int resolve(void) {
	if (resolved)
		return resolved > 0;
	obj_explosion = object_find("obj_explosion");
	spr_corpsemask = sprite_find("spr_corpsemask");
	uint64_t speed_bits;
	memcpy(&speed_bits, &const_speed->val, 8);
	int consts = KIND(const_speed) == 0 && const_speed->val == 0.3 && KIND(const_false) == 13 && const_false->val == 0;
	resolved = obj_explosion >= 0 && spr_corpsemask >= 0 && consts ? 1 : -1;
	debugPrintf("bodies at rest: obj_explosion %d, spr_corpsemask %d, Step constants %s -> %s\n", obj_explosion,
		spr_corpsemask, consts ? "ok" : "UNEXPECTED", resolved > 0 ? "skipping" : "OFF");
	return resolved > 0;
}

// What the fast Step needs from the checks
typedef struct {
	gml_rvalue *timeout; // the countdown to write (time_out on), or NULL
	double timeout_next;
	double fr;
} rest_plan;

// 1: the body is at the fixed point (the Step reduces to its ground query, see the top of the file); 0: run the Step
static int step_at_rest(void *self, void *other, rest_plan *p) {
	gml_rvalue *a, *b;
	double d;
	// L3: if (fall_sprite == spr_grunt_pistoldeath && animation_index != animation_max): off when they're equal
	NO(compare(var(self, v_animation_index), var(self, v_animation_max)) != 0);
	// L86: just_hit = false (when obj_master.freeze_timer <= 0): already false
	NO(!bool_false(var(self, v_just_hit)));
	int fire = truth(var(self, v_on_fire));
	NO(fire < 0);
	if (!fire) {
		// L93: place_meeting(x, y, obj_explosion) finds nothing without an instance and doesn't touch the body
		NO(instance_exists(self, other, obj_explosion) != 0);
	} else {
		// L100: fire_alpha += 0.01; if (fire_alpha > 1) fire_alpha = 1 (a real)
		a = var(self, v_fire_alpha);
		NO(!a || KIND(a) != 0 || a->val != 1.0);
		// L240: if (on_fire && fire_timer < 3) embers
		NO(compare_real(var(self, v_fire_timer), 3) < 0 || compare_real(var(self, v_fire_timer), 3) == 2);
	}
	// L107: if (dead == 0) physics, else the resting branch; dead must be exactly true for L270's `time_out && dead`
	a = var(self, v_dead);
	NO(!number(a, &d) || d != 1.0);
	// L238: mask_index = spr_corpsemask (Set_MaskIndex returns early when unchanged); L239: slope = false
	NO(*(int *)((char *)self + 0x14c) != spr_corpsemask);
	NO(!bool_false(var(self, v_slope)));
	// L263: point_distance(x, y, xprevious, yprevious) <= 1 && region != -1, else update_region()
	float x = field_f(self, 0xb8), y = field_f(self, 0xbc);
	NO(x != field_f(self, 0xc8) || y != field_f(self, 0xcc));
	int r = compare_real(var(self, v_region), -1);
	NO(r == 0 || r == 2);
	a = global_var(v_fr);
	NO(!a || KIND(a) != 0 || isnan(a->val));
	p->fr = a->val;
	// L270: if (time_out && dead) { timeout_timer -= 1 * global.fr; if (timeout_timer <= 100) blink; if (<= 0) destroy }
	int t = truth(var(self, v_time_out));
	NO(t < 0);
	p->timeout = NULL;
	if (t) {
		b = var(self, v_timeout_timer);
		NO(!b || KIND(b) != 0);
		gml_rvalue next = {b->val - 1.0 * p->fr, 0, 0};
		r = compare_real(&next, 100);
		NO(r <= 0 || r == 2);
		p->timeout = b;
		p->timeout_next = next.val;
	}
	// L290: first_frame = false; L293: if (slope) ... else the ground query
	NO(!bool_false(var(self, v_first_frame)));
	// The tail (after the query) needs these to be plain values
	b = var(self, v_bloodend_timer);
	NO(!b || KIND(b) != 0);
	b = var(self, v_ground_sprite);
	NO(!b || REFCOUNTED(KIND(b)) || KIND(b) == 5 || KIND(b) == 0xffffff);
	b = var(self, v_fly_sprite);
	NO(!b || REFCOUNTED(KIND(b)) || KIND(b) == 5 || KIND(b) == 0xffffff);
	b = var(self, v_animation_speed);
	NO(!b);
	return 1;
}

// The Step of a body at rest: the countdown, the ground query and what follows it, as the Step does them
static void step_fast(void *self, void *other, const rest_plan *p) {
	if (p->timeout)
		p->timeout->val = p->timeout_next;
	// L300: ground_meeting(x, y + 1): x and y + 1 as the reals the builtin getters give
	gml_rvalue ax = {0, 0, 5}, ay = {0, 0, 5}, res = {0, 0, 5};
	gv_x(self, NO_INDEX, &ax);
	gv_y(self, NO_INDEX, &ay);
	ay.val += 1;
	void *args[3] = {&ax, &ay, NULL};
	uint32_t t0 = debugMode ? sceKernelGetProcessTimeLow() : 0;
	gml_rvalue *g = ground_meeting(self, other, &res, 2, args);
	if (debugMode)
		ground_us += sceKernelGetProcessTimeLow() - t0;
	int ground = g && (KIND(g) == 0 || KIND(g) == 13) ? g->val > 0.5 : yy_bool(g);
	if (REFCOUNTED(KIND(&res)))
		yy_free(&res);
	gml_rvalue *sprite;
	if (ground) {
		// L303: if (bloodend_timer < 100) bloodend_timer += 1 * global.fr
		gml_rvalue *b = var(self, v_bloodend_timer);
		if (compare_real(b, 100) == -1)
			b->val = b->val + 1.0 * p->fr;
		sprite = var(self, v_ground_sprite);
	} else {
		sprite = var(self, v_fly_sprite);
	}
	// begin_animation(sprite, 0.3, false): with the same sprite it only writes animation_speed = 0.3
	gml_rvalue cur = {0, 0, 5};
	gv_sprite(self, NO_INDEX, &cur);
	gml_rvalue *speed = var(self, v_animation_speed);
	uint64_t a, b;
	memcpy(&a, &speed->val, 8);
	memcpy(&b, &const_speed->val, 8);
	if (yy_compare(&cur, sprite, *epsilon, 0) == 0 && KIND(speed) == 0 && a == b)
		return;
	gml_rvalue arg0 = *sprite, arg1 = *const_speed, arg2 = *const_false, res2 = {0, 0, 5};
	void *bargs[3] = {&arg0, &arg1, &arg2};
	begin_animation(self, other, &res2, 3, bargs);
	if (REFCOUNTED(KIND(&res2)))
		yy_free(&res2);
}

// User event 14 reduces to animation_code() when no sound can play and there's no hair
static int u14_plain(void *self) {
	NO(!bool_false(var(self, v_chest_hit)) || !bool_false(var(self, v_roll_hit)));
	int r = compare_real(var(self, v_animation_float), 0); // every sound test needs animation_float == 0
	NO(r == 0 || r == 2);
	NO(truth(var(self, v_has_hair)) != 0);
	return 1;
}

static void u14_fast(void *self, void *other) {
	gml_rvalue res = {0, 0, 5};
	void *args[1] = {NULL};
	animation_code(self, other, &res, 0, args);
	if (REFCOUNTED(KIND(&res)))
		yy_free(&res);
}

// exp_corpserestverify: a snapshot of what an event can change on the body
#define SNAP_VARS 384
typedef struct {
	int n, overflow;
	int key[SNAP_VARS];
	gml_rvalue *ptr[SNAP_VARS];
	gml_rvalue val[SNAP_VARS];
	uint8_t fields[0x150 - 0x84];
	uint32_t flags, rng[16], rng_i;
	int maxid;
} snapshot;
static snapshot snap_before, snap_fast, snap_orig;

typedef struct { gml_rvalue *v; int key; uint32_t hash; } var_elem;
typedef struct { int size, used, mask, grow; var_elem *elems; } var_map;

static void take(void *self, snapshot *s) {
	var_map *m = *(var_map **)((char *)self + 0x24);
	s->n = s->overflow = 0;
	for (int i = 0; m && i < m->size; i++) {
		if (!m->elems[i].hash)
			continue;
		if (s->n == SNAP_VARS) {
			s->overflow = 1;
			break;
		}
		s->key[s->n] = m->elems[i].key;
		s->ptr[s->n] = m->elems[i].v;
		s->val[s->n] = *m->elems[i].v;
		s->n++;
	}
	memcpy(s->fields, (char *)self + 0x84, sizeof(s->fields));
	memcpy(&s->flags, (char *)self + 0x7c, 4);
	memcpy(s->rng, rng_state, sizeof(s->rng));
	s->rng_i = *rng_index;
	s->maxid = *room_maxid;
}

// Puts back the variables, the fields and the random state of `s` (only plain values are written back)
static int put_back(void *self, const snapshot *s) {
	int ok = 1;
	for (int i = 0; i < s->n; i++) {
		gml_rvalue *v = s->ptr[i];
		if (REFCOUNTED(KIND(v)) || REFCOUNTED(KIND(&s->val[i]))) {
			if (memcmp(v, &s->val[i], sizeof(*v)))
				ok = 0; // a string/array/struct changed: can't put it back safely
			continue;
		}
		*v = s->val[i];
	}
	memcpy((char *)self + 0x84, s->fields, sizeof(s->fields));
	memcpy(rng_state, s->rng, sizeof(s->rng));
	*rng_index = s->rng_i;
	return ok;
}

static int same_value(const gml_rvalue *a, const gml_rvalue *b) {
	if (KIND(a) != KIND(b))
		return 0;
	if (KIND(a) == 0 || KIND(a) == 13)
		return !memcmp(&a->val, &b->val, 8);
	return !memcmp(a, b, sizeof(*a));
}

static int same(const snapshot *a, const snapshot *b) {
	int ok = 1;
	if (a->n != b->n || a->overflow || b->overflow) {
		bad_vars++;
		ok = 0;
	} else {
		for (int i = 0; i < a->n; i++) {
			if (a->key[i] != b->key[i] || !same_value(&a->val[i], &b->val[i])) {
				bad_vars++;
				ok = 0;
				break;
			}
		}
	}
	if (memcmp(a->fields, b->fields, sizeof(a->fields)) || a->flags != b->flags) {
		bad_fields++;
		ok = 0;
	}
	if (memcmp(a->rng, b->rng, sizeof(a->rng)) || a->rng_i != b->rng_i) {
		bad_rng++;
		ok = 0;
	}
	if (a->maxid != b->maxid) {
		bad_maxid++;
		ok = 0;
	}
	return ok;
}

static void verify_step(void *self, void *other, const rest_plan *p) {
	take(self, &snap_before);
	step_fast(self, other, p);
	if (fault == 1) {
		gml_rvalue *b = var(self, v_bloodend_timer);
		if (b && KIND(b) == 0) {
			uint64_t bits;
			memcpy(&bits, &b->val, 8);
			bits ^= 1;
			memcpy(&b->val, &bits, 8);
		}
	}
	take(self, &snap_fast);
	if (!put_back(self, &snap_before)) {
		verify_skipped++;
		orig_step(self, other);
		return;
	}
	orig_step(self, other);
	take(self, &snap_orig);
	verify_steps++;
	if (!same(&snap_fast, &snap_orig))
		verify_step_bad++;
}

static void verify_u14_event(void *self, void *other) {
	take(self, &snap_before);
	if (fault != 2)
		u14_fast(self, other);
	take(self, &snap_fast);
	if (!put_back(self, &snap_before)) {
		verify_skipped++;
		orig_u14(self, other);
		return;
	}
	orig_u14(self, other);
	take(self, &snap_orig);
	verify_u14++;
	if (!same(&snap_fast, &snap_orig))
		verify_u14_bad++;
}

static int active(void) {
	return !disabled && !ab_off && resolve();
}

static void corpse_step(void *self, void *other) {
	uint32_t t0 = debugMode ? sceKernelGetProcessTimeLow() : 0;
	rest_plan p;
	if (active() && step_at_rest(self, other, &p)) {
		n_step_fast++;
		if (verify)
			verify_step(self, other, &p);
		else
			step_fast(self, other, &p);
	} else if (classify && resolve()) {
		int rest = step_at_rest(self, other, &p);
		uint32_t c0 = sceKernelGetProcessTimeLow();
		orig_step(self, other);
		uint32_t dt = sceKernelGetProcessTimeLow() - c0;
		if (rest)
			class_rest++, class_rest_us += dt;
		else
			class_other++, class_other_us += dt;
		n_step_orig++;
	} else {
		n_step_orig++;
		orig_step(self, other);
	}
	if (debugMode)
		step_us += sceKernelGetProcessTimeLow() - t0;
}

static void corpse_u14(void *self, void *other) {
	uint32_t t0 = debugMode ? sceKernelGetProcessTimeLow() : 0;
	if (active() && u14_plain(self)) {
		n_u14_fast++;
		if (verify)
			verify_u14_event(self, other);
		else
			u14_fast(self, other);
	} else if (classify && resolve()) {
		int plain = u14_plain(self);
		uint32_t c0 = sceKernelGetProcessTimeLow();
		orig_u14(self, other);
		uint32_t dt = sceKernelGetProcessTimeLow() - c0;
		if (plain)
			class_u14_rest++, class_u14_rest_us += dt;
		else
			class_u14_other++, class_u14_other_us += dt;
		n_u14_orig++;
	} else {
		n_u14_orig++;
		orig_u14(self, other);
	}
	if (debugMode)
		u14_us += sceKernelGetProcessTimeLow() - t0;
}

void corpse_rest_report(int frames) {
	if (!installed || frames <= 0)
		return;
	float f = (float)frames;
	if (n_step_fast || n_step_orig || n_u14_fast || n_u14_orig)
		debugPrintf("[CORPSEREST] Step at rest %.1f (ground query %.3f ms) original %.1f | user 14 plain %.1f original %.1f "
			"per frame | time Step %.3f ms user 14 %.3f ms%s\n", n_step_fast / f, ground_us / 1000.0f / f, n_step_orig / f,
			n_u14_fast / f, n_u14_orig / f, step_us / 1000.0f / f, u14_us / 1000.0f / f,
			ab ? (ab_off ? " | A/B: off" : " | A/B: on") : "");
	if (classify && (class_rest || class_other))
		debugPrintf("[CORPSEREST] original events: Step at rest %.1f x %.1f us, other %.1f x %.1f us | user 14 plain "
			"%.1f x %.1f us, other %.1f x %.1f us (per frame)\n", class_rest / f,
			class_rest ? (float)class_rest_us / class_rest : 0.0f, class_other / f,
			class_other ? (float)class_other_us / class_other : 0.0f, class_u14_rest / f,
			class_u14_rest ? (float)class_u14_rest_us / class_u14_rest : 0.0f, class_u14_other / f,
			class_u14_other ? (float)class_u14_other_us / class_u14_other : 0.0f);
	if (verify)
		debugPrintf("[CORPSEREST] verify: Step %u checked %u mismatches, user 14 %u checked %u mismatches, %u not "
			"compared | vars %u fields %u rng %u maxid %u (session)%s\n", verify_steps, verify_step_bad, verify_u14,
			verify_u14_bad, verify_skipped, bad_vars, bad_fields, bad_rng, bad_maxid,
			fault ? " | FAULT INJECTED" : "");
	for (int i = 0; i < CR_LINES && reject_line[i]; i++)
		debugPrintf("[CORPSEREST] original at corpse_rest.c:%u x%u (session)\n", reject_line[i], reject_count[i]);
	n_step_fast = n_step_orig = n_u14_fast = n_u14_orig = 0;
	step_us = u14_us = ground_us = 0;
	class_rest = class_other = class_u14_rest = class_u14_other = 0;
	class_rest_us = class_other_us = class_u14_rest_us = class_u14_other_us = 0;
	if (ab)
		ab_off = !ab_off;
}

void corpse_rest_install(gml_func_entry *table) {
	if (exp_flag("corpserest0")) {
		debugPrintf("bodies at rest: original events (exp_corpserest0)\n");
		return;
	}
#define SYM(name) so_symbol(&yoyoloader_mod, name)
#define CR_VAR_LOOKUP(name) v_##name = (int *)SYM("g_VAR_" #name);
	CR_VARS(CR_VAR_LOOKUP)
	int vars = 1;
#define CR_VAR_CHECK(name) vars &= v_##name != NULL;
	CR_VARS(CR_VAR_CHECK)
	read_var = (read_var_fn)SYM("_ZN12YYObjectBase17InternalReadYYVarEi");
	yy_compare = (compare_fn)SYM("_Z12YYCompareValRK6RValueS1_db");
	yy_bool = (bool_fn)SYM("_Z11BOOL_RValuePK6RValue");
	yy_free = (free_fn)SYM("_Z16FREE_RValue__PreP6RValue");
	gv_x = (builtin_var_fn)SYM("_Z4GV_XP9CInstanceiP6RValue");
	gv_y = (builtin_var_fn)SYM("_Z4GV_YP9CInstanceiP6RValue");
	gv_sprite = (builtin_var_fn)SYM("_Z14GV_SpriteIndexP9CInstanceiP6RValue");
	instance_exists_fn = (builtin_fn)SYM("_Z16F_InstanceExistsR6RValueP9CInstanceS2_iPS_");
	object_find = (find_fn)SYM("_Z11Object_FindPKc");
	sprite_find = (find_fn)SYM("_Z11Sprite_FindPKc");
	ground_meeting = (gml_script_fn)SYM("_Z25gml_Script_ground_meetingP9CInstanceS0_R8YYRValueiPPS1_");
	begin_animation = (gml_script_fn)SYM("_Z26gml_Script_begin_animationP9CInstanceS0_R8YYRValueiPPS1_");
	animation_code = (gml_script_fn)SYM("_Z25gml_Script_animation_codeP9CInstanceS0_R8YYRValueiPPS1_");
	epsilon = (const double *)SYM("g_GMLMathEpsilon");
	const_speed = (const gml_rvalue *)SYM("gs_constArg7_9FDBDD1B");
	const_false = (const gml_rvalue *)SYM("gs_constFalse");
	rng_state = (uint32_t *)SYM("state");
	rng_index = (uint32_t *)SYM("g_RndIndex");
	room_maxid = (int *)SYM("room_maxid");
	int ok = vars && read_var && yy_compare && yy_bool && yy_free && gv_x && gv_y && gv_sprite && instance_exists_fn &&
		object_find && sprite_find && ground_meeting && begin_animation && animation_code && epsilon && const_speed &&
		const_false && rng_state && rng_index && room_maxid;
	if (ok) {
		orig_step = gml_replace(table, "gml_Object_obj_enemycorpse_Step_0", corpse_step);
		orig_u14 = gml_replace(table, "gml_Object_obj_enemycorpse_Other_24", corpse_u14);
		ok = orig_step && orig_u14; // both exist in 1.1.10
	}
	installed = ok;
	if (debugMode) {
		verify = exp_flag("corpserestverify");
		fault = !verify ? 0 : exp_flag("corpserestfault1") ? 1 : exp_flag("corpserestfault2") ? 2 : 0;
		ab = exp_flag("corpserestab");
		classify = exp_flag("corpserestclass");
		disabled = classify; // classification times the original events
	}
	debugPrintf("bodies at rest: %s%s%s%s\n", ok ? "installed" : "NOT installed", verify ? ", verify" : "",
		ab ? ", A/B" : "", classify ? ", classify (originals)" : "");
}
