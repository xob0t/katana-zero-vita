/*
 * hazard_native.c: exact natives of the room hazards' per-frame events (APK 1.1.10).
 *
 * obj_ceiling_laser Step (0x3699b90, 29.8 KB of YYC). On the Vita a laser's Step cost 38 us in bunker2_3 (21 lasers,
 * 0.80 ms per frame) and 12.6 us in club_4_2 (17 lasers, 0.22 ms), of which the three place_meeting commands were
 * 13 us and 0 us: the rest is the event's own variable lookups, RValue temporaries and calls. From the ARM listing,
 * for a laser that doesn't move (moving false):
 *
 *   attack = false; image_speed = 0;                           // L1, L2
 *   if (!active) active_timer = 0;                             // L3, L6
 *   if (global.fr != 0 && active && !first_frame) {           // L8
 *       active_timer += global.fr;                             // L10
 *       image_index = 1;                                       // L73
 *       if (place_meeting(x, y, obj_player) && ...) ...        // L74: the player vaporized
 *       if (place_meeting(x, y, obj_enemy) && ...) ...         // L107: an enemy vaporized
 *       if (place_meeting(x, y, obj_enemycorpse) && ...) ...   // L122: a body vaporized
 *       if (attack) ...                                        // L137
 *   }
 *   if (first_frame) first_frame = false;                      // L154
 *   if (!bypass_blocks && (x != xprevious || instance_exists(obj_movingblock) && abs(...) < 300))
 *       { image_yscale = 1; i = 0; while (...) i += 1; image_yscale = i; }   // L160..L168: the beam length
 *   if (!anim_override)                                        // L174
 *       begin_animation(active ? spr_ceiling_laser_on : spr_ceiling_laser_off, 0.3, true);   // L195 / L184
 *   if (moving && ...) animation_code();                       // L200
 *
 * The native does this for a laser with moving and anim_override false (plain truth values) and no beam-length
 * update this frame: bypass_blocks true, or x equal to xprevious and no obj_movingblock instance (instance_exists is a
 * pure instance walk, and nothing on this path creates or destroys instances, so it is asked before anything is
 * written). In order, with the runner's own functions:
 *   - attack and active_timer are the laser's variable slots (non-refcounted kinds, written as the event writes them:
 *     attack value 0 kind 13; active_timer value 0 kind 0, or YYRValue::operator+='s real sum, kind 13 -> 0; the flags
 *     words aren't written by the event's code either, except attack's, which gets a stack word: not compared);
 *   - image_speed and image_index through Variable_SetValue_Direct with the values the event passes (SV_ImageSpeed /
 *     SV_ImageIndex only store +0xa0 / +0x98 and clear +0x9c);
 *   - the three place_meeting calls as the Command_InstancePlace calls F_PlaceMeeting makes for them (PerformColTest's
 *     object-reference path), with the floats of the event's arguments (GV_X / GV_Y of the laser);
 *   - BOOL_RValue / YYCompareVal semantics (live epsilon) for every test;
 *   - begin_animation: when sprite_index already equals the sprite (YYCompareVal of GV_SpriteIndex's value and the
 *     argument, as the script compares them), the script only copies argument1 (the event's constant 0.3) whole into
 *     animation_speed; that copy is made here. Otherwise the script runs with the event's arguments.
 * If a place_meeting answers true (a hit: sounds, screen shake, random choices, instances created and destroyed), the
 * native puts back what it wrote (attack and active_timer slots, +0x98..+0xa3) and runs the original event. The
 * queries it made are the ones the original makes first, at the same position: the first drains the collision dirty
 * list and computes a stale box exactly as the original's first query would; a repeated query at an unchanged x/y only
 * searches (no SetPosition, no dirty mark), so the original's own queries then find the same state.
 * Anything else (a moving laser, a missing or non-plain variable, global.fr not a real, a beam-length update, a
 * profiler on) runs the original before anything is written.
 *
 * Measured on the Vita (in-run A/B, the whole event timed per call in both states, enemies alive): bunker2_3 (21
 * active lasers) 0.700 -> 0.423 ms/frame, 33.3 -> 20.1 us per laser; club_4_2 (17 lasers, off) 0.222 -> 0.143 ms.
 * What is left is mostly the three collision commands (~13 us per active laser).
 *
 * Switches: exp_hzlaser0 (original event); exp_hzlaserverify (debugMode: each native call runs again as the original
 * from the same starting state, the laser's variables and fields 0x7c..0x150 put back in between; compares every
 * variable (kind and value; flags words aren't compared), the fields, the random state, room_maxid, the global.fr
 * value and the collision dirty list; the game keeps the original's result. A hit's rollback is checked against the
 * state before the native ran, and a hit that changed nothing else (no random draw, no new instance) is also run
 * again as the original alone and compared); exp_hzlaserfault (with verify, negative controls: 1 native call in 64
 * leaves active_timer 1e-9 off, 1 rollback in 4 doesn't put active_timer back); exp_hzlaserab (native off every
 * other report window, event time per call); exp_hzlaserprof (event time per call, and the instance_exists, query
 * and begin_animation parts). Report: [HZLASER].
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <psp2/kernel/processmgr.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

#define ABI __attribute__((pcs("aapcs")))
#define KIND(v) ((v)->kind & 0xffffff)
#define REFCOUNTED(k) ((1u << ((k) & 31)) & 0x46u)
#define ARRAY_NONE ((int)0x80000000)

typedef gml_rvalue *ABI (*ref_fn)(void *, int);
typedef int ABI (*compare_fn)(const gml_rvalue *, const gml_rvalue *, double, int);
typedef int ABI (*bool_fn)(const gml_rvalue *);
typedef int ABI (*set_fn)(void *, int, int, gml_rvalue *);
typedef int ABI (*getter_fn)(void *, int, gml_rvalue *);
typedef void ABI (*free_fn)(gml_rvalue *);
typedef void ABI (*bi_fn)(gml_rvalue *, void *, void *, int, gml_rvalue *);
typedef void *ABI (*script_fn)(void *, void *, gml_rvalue *, int, gml_rvalue **);
typedef struct { const char *name; void *call; int argc; } Function;
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;
typedef struct { const char *name; void *get; void *set; int canset; } BuiltinVar;

static ref_fn get_ref, get_ref_l;
static compare_fn yy_compare;
static bool_fn yy_bool;
static set_fn set_direct;
static free_fn yy_free;
static bi_fn f_instance_exists;
typedef void *ABI (*place_fn)(void *, float, float, int, void *);
static place_fn instance_place;
static script_fn begin_animation;
static const int *fid_instance_exists;
static Function **functions, **current_function;
static Trace **trace_start;
static const uint8_t *profile;
static const double *epsilon;
static void **global;
static const BuiltinVar *builtin_vars; // the registry array itself
static const int *start_globals;
static const gml_rvalue *const_speed, *const_true; // gs_constArg10_12D1E91B (0.3), gs_constTrue
static uint32_t *rng_state, *rng_index;
static int *room_maxid;
static uint32_t *dirty_list;

#define LV(X) X(attack) X(image_speed) X(active) X(active_timer) X(first_frame) X(moving) X(image_index) X(x) X(y) \
	X(xprevious) X(bypass_blocks) X(anim_override) X(fr) X(sprite_index) X(animation_speed)
#define LV_ENUM(n) L_##n,
enum { LV(LV_ENUM) NLV };
#define LV_NAME(n) "g_VAR_" #n,
static const char *const lv_names[NLV] = { LV(LV_NAME) };
static const int *lv[NLV];
static inline int lid(int n) { return lv[n][1]; } // g_VAR_* ids are filled at game start: read them live

enum { OBJ_PLAYER = 745, OBJ_ENEMY = 407, OBJ_CORPSE = 412, OBJ_MOVINGBLOCK = 666, SPR_LASER_OFF = 288, SPR_LASER_ON = 289 };

static event_func laser_orig;
static int laser_on, verify, fault, ab, ab_off, prof;
// per report window
static unsigned n_native, n_hit, n_orig, n_inactive;
static uint32_t ev_us, ev_n, sec_us[3];
// session
static unsigned why_line[16], why_count[16];
static unsigned v_rbcmp, v_rb, v_rb_bad, v_calls, v_bad, v_skipped, v_bad_vars, v_bad_fields, v_bad_rng, v_bad_maxid, v_bad_fr, v_bad_dirty;

static void why(unsigned line) {
	for (int i = 0; i < 16; i++) {
		if (why_line[i] == line || !why_line[i]) {
			why_line[i] = line;
			why_count[i]++;
			return;
		}
	}
}
#define NO(cond) do { if (cond) { why(__LINE__); return 0; } } while (0)

// The instance's variable map (YYObjectBase +0x24): open addressing, key id + 1, as laser_native.c reads it
static gml_rvalue *peek(void *self, int id) {
	const uint8_t *ht = *(const uint8_t **)((char *)self + 0x24);
	if (!ht)
		return NULL;
	uint32_t mask = *(const uint32_t *)(ht + 8);
	const uint8_t *e = *(const uint8_t **)(ht + 16);
	if (!e || mask > 65535 || (mask & (mask + 1)))
		return NULL;
	uint32_t key = ((uint32_t)id + 1) & 0x7fffffff, i = key & mask;
	if (!key)
		return NULL;
	for (uint32_t d = 0; d <= mask; d++, i = (i + 1) & mask) {
		uint32_t k = *(const uint32_t *)(e + 12 * i + 8);
		if (k == key)
			return *(gml_rvalue **)(e + 12 * i);
		if (!k || d > ((i - (k & mask)) & mask))
			return NULL;
	}
	return NULL;
}

static inline int truth_kind(const gml_rvalue *v) {
	unsigned k = KIND(v);
	return k == 0 || k == 7 || k == 10 || k == 13;
}

// YYCompareVal for two reals: d = a - b; 0 if |d| <= epsilon, else -1 / 1; a NaN difference takes the runner's path
static int compare_real(double a, double b) {
	double d = a - b, eps = *epsilon;
	if (d != d) {
		gml_rvalue x = { a, 0, 0 }, y = { b, 0, 0 };
		return yy_compare(&x, &y, eps, 0);
	}
	int r = d < 0 ? -1 : 1;
	return fabs(d) <= eps ? 0 : r;
}

// A builtin called as YYGML_CallLegacyFunction calls it: result undefined, g_pFunction the function's entry
static void call_builtin(bi_fn fn, const int *fid, gml_rvalue *out, void *self, void *other, int argc, gml_rvalue *args) {
	Function *saved = *current_function;
	memset(&out->val, 0, 4); // the payload's low word only, as YYGML_CallLegacyFunction
	out->flags = 0;
	out->kind = 5;
	*current_function = *functions + fid[1];
	fn(out, self, other, argc, args);
	*current_function = saved;
}

static int builtin_get(void *self, int id, gml_rvalue *out) {
	out->val = 0;
	out->flags = 0;
	out->kind = 0;
	return ((getter_fn)builtin_vars[id].get)(self, ARRAY_NONE, out);
}

// place_meeting(x, y, obj) for an object reference: F_PlaceMeeting -> PerformColTest, whose kind-15 object path is
// Command_InstancePlace(self, YYGetFloat(x), YYGetFloat(y), YYGetInt32(obj), NULL) != NULL (YYGetFloat of a real is
// its float). Called directly with the same floats.
static int place_meeting(void *self, double x, double y, int object) {
	return instance_place(self, (float)x, (float)y, object, NULL) != NULL;
}

static void set_real(void *self, int id, double v) {
	gml_rvalue r = { v, 0, 0 };
	set_direct(self, id, ARRAY_NONE, &r);
}

// begin_animation(sprite, 0.3, true). When sprite_index already equals the sprite (YYCompareVal of GV_SpriteIndex's
// value and the argument, as the script compares them) the script only copies argument1 into animation_speed (its
// line 18: the slot through the writable getter, old value freed, the RValue copied whole); that is done here for an
// existing non-refcounted slot. Otherwise the script runs.
static unsigned n_anim_inline;
static void call_begin_animation(void *self, void *other, int sprite) {
	gml_rvalue a0 = { 0, 0, 15 }, a1 = *const_speed, a2 = *const_true;
	uint32_t ref[2] = { (uint32_t)sprite, 0x1000001 };
	memcpy(&a0.val, ref, 8);
	gml_rvalue cur;
	builtin_get(self, lid(L_sprite_index), &cur);
	gml_rvalue *speed = peek(self, lid(L_animation_speed));
	if (!REFCOUNTED(KIND(&cur)) && speed && !REFCOUNTED(KIND(speed)) && yy_compare(&cur, &a0, *epsilon, 0) == 0) {
		*speed = a1;
		n_anim_inline++;
		return;
	}
	gml_rvalue *argv[3] = { &a0, &a1, &a2 };
	gml_rvalue result = { 0, 0, 5 };
	begin_animation(self, other, &result, 3, argv);
	if (REFCOUNTED(KIND(&result)))
		yy_free(&result);
}

// What the native writes before its first query, for the rollback on a hit
typedef struct { gml_rvalue *attack, *timer; gml_rvalue attack_v, timer_v; uint8_t fields[12]; } Undo;

// 1: handled; 0: run the original (nothing written); 2: a hit: written state put back, run the original
static int laser_step(void *self, void *other) {
	NO(!self || *(uint32_t *)((char *)self + 0x50) != 1 || *profile);
	void *g = *global;
	NO(!g);
	for (int n = L_image_speed; n <= L_y; n++)
		if (n == L_image_speed || n == L_image_index || n == L_x || n == L_y)
			NO(lid(n) >= *start_globals);
	NO(lid(L_xprevious) >= *start_globals || lid(L_sprite_index) >= *start_globals);
	gml_rvalue *attack = peek(self, lid(L_attack)), *active = peek(self, lid(L_active));
	gml_rvalue *timer = peek(self, lid(L_active_timer)), *first = peek(self, lid(L_first_frame));
	gml_rvalue *moving = peek(self, lid(L_moving)), *bypass = peek(self, lid(L_bypass_blocks));
	gml_rvalue *anim = peek(self, lid(L_anim_override));
	NO(!attack || !active || !timer || !first || !moving || !bypass || !anim);
	NO(REFCOUNTED(KIND(attack)) || !truth_kind(active) || (KIND(timer) != 0 && KIND(timer) != 13));
	NO(!truth_kind(first) || !truth_kind(moving) || !truth_kind(bypass) || !truth_kind(anim));
	NO(yy_bool(moving) || yy_bool(anim));
	gml_rvalue *fr = (*(ref_fn *)(*(void ***)g + 2))(g, lid(L_fr)); // the event's own read of global.fr (getter)
	NO(!fr || KIND(fr) != 0);
	// L160: the beam length is measured this frame only with blocks on and x moved or a moving block somewhere
	int on_blocks = !yy_bool(bypass);
	gml_rvalue gx, gy;
	if (on_blocks) {
		gml_rvalue gxp;
		builtin_get(self, lid(L_x), &gx);
		builtin_get(self, lid(L_xprevious), &gxp);
		NO(KIND(&gx) != 0 || KIND(&gxp) != 0);
		NO(compare_real(gx.val, gxp.val) != 0);
		gml_rvalue arg = { 0, 0, 15 }, out;
		uint32_t ref[2] = { OBJ_MOVINGBLOCK, 0x1000000 };
		memcpy(&arg.val, ref, 8);
		uint32_t te = prof ? sceKernelGetProcessTimeLow() : 0;
		call_builtin(f_instance_exists, fid_instance_exists, &out, self, other, 1, &arg);
		if (prof)
			sec_us[0] += sceKernelGetProcessTimeLow() - te;
		NO(yy_bool(&out));
	}
	Undo u = { attack, timer, *attack, *timer };
	memcpy(u.fields, (char *)self + 0x98, 12);
	Trace trace = { *trace_start, "gml_Object_obj_ceiling_laser_Step_0", 1 };
	*trace_start = &trace;
	// L1: attack = false (value 0, kind 13)
	memset(&attack->val, 0, 8);
	attack->kind = 13;
	trace.line = 2;
	set_real(self, lid(L_image_speed), 0);
	trace.line = 3;
	int is_active = yy_bool(active);
	if (!is_active) {
		memset(&timer->val, 0, 8);
		timer->kind = 0;
	}
	trace.line = 8;
	if (compare_real(fr->val, 0) != 0 && is_active && !yy_bool(first)) {
		trace.line = 10;
		timer->kind = 0; // YYRValue::operator+=: a bool becomes a real; real + real
		timer->val = fr->val + timer->val;
		if (fault && (v_calls & 63) == 7)
			timer->val += 1e-9;
		trace.line = 73;
		set_real(self, lid(L_image_index), 1);
		builtin_get(self, lid(L_x), &gx);
		builtin_get(self, lid(L_y), &gy);
		static const int objects[3] = { OBJ_PLAYER, OBJ_ENEMY, OBJ_CORPSE };
		static const int lines[3] = { 74, 107, 122 };
		for (int k = 0; k < 3; k++) {
			trace.line = lines[k];
			uint32_t tq = prof ? sceKernelGetProcessTimeLow() : 0;
			int hit = place_meeting(self, gx.val, gy.val, objects[k]);
			if (prof)
				sec_us[1] += sceKernelGetProcessTimeLow() - tq;
			if (hit) {
				*trace_start = trace.previous;
				*u.attack = u.attack_v;
				if (!(fault && (v_rb & 3) == 1))
					*u.timer = u.timer_v;
				memcpy((char *)self + 0x98, u.fields, 12);
				return 2;
			}
		}
	}
	trace.line = 154;
	if (yy_bool(first)) {
		trace.line = 156;
		memset(&first->val, 0, 8);
		first->kind = 13;
	}
	trace.line = 174;
	// anim_override false (checked); L176 reads active again
	int on = yy_bool(active);
	trace.line = on ? 195 : 184;
	uint32_t ta = prof ? sceKernelGetProcessTimeLow() : 0;
	call_begin_animation(self, other, on ? SPR_LASER_ON : SPR_LASER_OFF);
	if (prof)
		sec_us[2] += sceKernelGetProcessTimeLow() - ta;
	*trace_start = trace.previous;
	n_inactive += !is_active;
	return 1;
}

// exp_hzlaserverify: what the event can change on the laser, and a few runner globals
#define SNAP_VARS 256
typedef struct { gml_rvalue *v; int key; uint32_t hash; } var_elem;
typedef struct { int size, used, mask, grow; var_elem *elems; } var_map;
typedef struct {
	int n, overflow;
	int key[SNAP_VARS];
	gml_rvalue *ptr[SNAP_VARS];
	gml_rvalue val[SNAP_VARS];
	uint8_t fields[0x150 - 0x7c];
	uint32_t rng[16], rng_i, dirty[3];
	int maxid;
	double fr;
} Snap;
static Snap s_before, s_native, s_orig;

static void take(void *self, Snap *s) {
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
	memcpy(s->fields, (char *)self + 0x7c, sizeof(s->fields));
	memcpy(s->rng, rng_state, sizeof(s->rng));
	s->rng_i = *rng_index;
	s->maxid = *room_maxid;
	memcpy(s->dirty, dirty_list, sizeof(s->dirty));
	void *g = *global;
	gml_rvalue *fr = g ? (*(ref_fn *)(*(void ***)g + 2))(g, lid(L_fr)) : NULL;
	s->fr = fr && KIND(fr) == 0 ? fr->val : -12345.0;
}

static int put_back(void *self, const Snap *s) {
	for (int i = 0; i < s->n; i++) {
		gml_rvalue *v = s->ptr[i];
		if (REFCOUNTED(KIND(v)) || REFCOUNTED(KIND(&s->val[i]))) {
			if (memcmp(v, &s->val[i], sizeof(*v)))
				return 0;
			continue;
		}
		*v = s->val[i];
	}
	memcpy((char *)self + 0x7c, s->fields, sizeof(s->fields));
	return 1;
}

static int same_value(const gml_rvalue *a, const gml_rvalue *b) {
	if (KIND(a) != KIND(b))
		return 0;
	if (KIND(a) == 0 || KIND(a) == 13)
		return !memcmp(&a->val, &b->val, 8);
	return !memcmp(a, b, sizeof(*a));
}

static int same(const Snap *a, const Snap *b) {
	int ok = 1;
	if (a->n != b->n || a->overflow || b->overflow) {
		v_bad_vars++;
		ok = 0;
	} else {
		for (int i = 0; i < a->n; i++) {
			if (a->key[i] != b->key[i] || !same_value(&a->val[i], &b->val[i])) {
				if (v_bad_vars < 8)
					debugPrintf("[HZLASER] verify: variable %d kind %u/%u\n", a->key[i], KIND(&a->val[i]), KIND(&b->val[i]));
				v_bad_vars++;
				ok = 0;
				break;
			}
		}
	}
	if (memcmp(a->fields, b->fields, sizeof(a->fields))) {
		if (v_bad_fields < 8)
			for (unsigned o = 0; o < sizeof(a->fields); o += 4)
				if (memcmp(a->fields + o, b->fields + o, 4)) {
					debugPrintf("[HZLASER] verify: field +0x%x\n", 0x7c + o);
					break;
				}
		v_bad_fields++;
		ok = 0;
	}
	if (memcmp(a->rng, b->rng, sizeof(a->rng)) || a->rng_i != b->rng_i)
		v_bad_rng++, ok = 0;
	if (a->maxid != b->maxid)
		v_bad_maxid++, ok = 0;
	if (memcmp(&a->fr, &b->fr, 8))
		v_bad_fr++, ok = 0;
	if (memcmp(a->dirty, b->dirty, sizeof(a->dirty)))
		v_bad_dirty++, ok = 0;
	return ok;
}

static void ABI laser_event(void *self, void *other) {
	if (ab_off) {
		laser_orig(self, other);
		return;
	}
	if (!verify) {
		int r = laser_step(self, other);
		if (r == 1) {
			n_native++;
			return;
		}
		n_hit += r == 2;
		n_orig += r == 0;
		laser_orig(self, other);
		return;
	}
	take(self, &s_before);
	int r = laser_step(self, other);
	if (r == 2) {
		// The rollback: every variable and +0x98..+0xa3 as they were before the native ran
		take(self, &s_native);
		v_rb++;
		int bad = s_native.n != s_before.n ||
			memcmp(s_native.fields + (0x98 - 0x7c), s_before.fields + (0x98 - 0x7c), 12);
		for (int i = 0; !bad && i < s_before.n; i++)
			bad = s_native.key[i] != s_before.key[i] || !same_value(&s_native.val[i], &s_before.val[i]);
		v_rb_bad += bad;
	}
	if (r != 1) {
		n_hit += r == 2;
		n_orig += r == 0;
		laser_orig(self, other);
		// A hit that changed nothing outside the laser (no random draw, no new instance: a body or player on another
		// draw layer, or already vaporized): (native queries + rollback + original) against the original alone
		if (r == 2 && *room_maxid == s_before.maxid && !memcmp(rng_state, s_before.rng, sizeof(s_before.rng)) &&
				*rng_index == s_before.rng_i) {
			take(self, &s_native);
			if (!put_back(self, &s_before)) {
				v_skipped++;
				return;
			}
			laser_orig(self, other);
			take(self, &s_orig);
			v_rbcmp++;
			v_calls++;
			if (!same(&s_native, &s_orig))
				v_bad++;
		}
		return;
	}
	n_native++;
	take(self, &s_native);
	if (!put_back(self, &s_before)) {
		v_skipped++;
		laser_orig(self, other);
		return;
	}
	laser_orig(self, other);
	take(self, &s_orig);
	v_calls++;
	if (!same(&s_native, &s_orig))
		v_bad++;
}

static void ABI laser_timed(void *self, void *other) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	laser_event(self, other);
	ev_us += sceKernelGetProcessTimeLow() - t0;
	ev_n++;
}

void hazard_native_report(int frames) {
	if (!laser_on || frames <= 0)
		return;
	float f = (float)frames;
	debugPrintf("[HZLASER] native %.1f/frame (inactive %.1f, begin_animation inline %.1f), hit -> original %.1f, original %.1f%s\n", n_native / f,
		n_inactive / f, n_anim_inline / f, n_hit / f, n_orig / f, ab ? (ab_off ? " | A/B off" : " | A/B on") : "");
	if ((prof || ab) && ev_n)
		debugPrintf("[HZLASER] event %.3f ms/frame, %.2f us/call (%s) | instance_exists %.3f, queries %.3f, "
			"begin_animation %.3f ms/frame\n", ev_us / 1000.0f / f, (float)ev_us / ev_n, ab_off ? "off" : "on",
			sec_us[0] / 1000.0f / f, sec_us[1] / 1000.0f / f, sec_us[2] / 1000.0f / f);
	if (verify)
		debugPrintf("[HZLASER] verify: %u calls, %u mismatches (vars %u, fields %u, rng %u, maxid %u, fr %u, dirty list %u), "
			"%u skipped; hit rollbacks %u, %u not restored, %u of the checks after a rollback (session)%s\n", v_calls,
			v_bad, v_bad_vars, v_bad_fields, v_bad_rng, v_bad_maxid, v_bad_fr, v_bad_dirty, v_skipped, v_rb, v_rb_bad,
			v_rbcmp, fault ? " FAULT INJECTED" : "");
	for (int i = 0; i < 16 && why_line[i]; i++)
		debugPrintf("[HZLASER] original at hazard_native.c:%u x%u (session)\n", why_line[i], why_count[i]);
	n_native = n_hit = n_orig = n_inactive = n_anim_inline = 0;
	ev_us = ev_n = 0;
	memset(sec_us, 0, sizeof(sec_us));
	if (ab)
		ab_off = !ab_off;
}

int hazard_native_install(gml_func_entry *table) {
	if (exp_flag("hzlaser0"))
		return 0;
#define SYM(t, n) ((t)so_symbol(&yoyoloader_mod, n))
	get_ref = SYM(ref_fn, "_ZN12YYObjectBase19InternalGetYYVarRefEi");
	get_ref_l = SYM(ref_fn, "_ZN12YYObjectBase20InternalGetYYVarRefLEi");
	yy_compare = SYM(compare_fn, "_Z12YYCompareValRK6RValueS1_db");
	yy_bool = SYM(bool_fn, "_Z11BOOL_RValuePK6RValue");
	set_direct = SYM(set_fn, "_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
	yy_free = SYM(free_fn, "_Z16FREE_RValue__PreP6RValue");
	instance_place = SYM(place_fn, "_Z21Command_InstancePlaceP9CInstanceffiP8CDS_List");
	f_instance_exists = SYM(bi_fn, "_Z16F_InstanceExistsR6RValueP9CInstanceS2_iPS_");
	fid_instance_exists = SYM(const int *, "g_FUNC_instance_exists");
	begin_animation = SYM(script_fn, "_Z26gml_Script_begin_animationP9CInstanceS0_R8YYRValueiPPS1_");
	functions = SYM(Function **, "the_functions");
	current_function = SYM(Function **, "g_pFunction");
	trace_start = SYM(Trace **, "_ZN13SYYStackTrace8s_pStartE");
	profile = SYM(const uint8_t *, "g_bProfile");
	epsilon = SYM(const double *, "g_GMLMathEpsilon");
	global = SYM(void **, "g_pGlobal");
	builtin_vars = SYM(const BuiltinVar *, "builtin_variables");
	start_globals = SYM(const int *, "g_nStartGlobalVariables");
	const_speed = SYM(const gml_rvalue *, "gs_constArg10_12D1E91B");
	const_true = SYM(const gml_rvalue *, "gs_constTrue");
	rng_state = SYM(uint32_t *, "state");
	rng_index = SYM(uint32_t *, "g_RndIndex");
	room_maxid = SYM(int *, "room_maxid");
	dirty_list = SYM(uint32_t *, "s_dirty_list");
#undef SYM
	if (!get_ref || !get_ref_l || !yy_compare || !yy_bool || !set_direct || !yy_free || !instance_place ||
			!f_instance_exists || !fid_instance_exists || !begin_animation || !functions ||
			!current_function || !trace_start || !profile || !epsilon || !global || !builtin_vars || !start_globals ||
			!const_speed || !const_true || !rng_state || !rng_index || !room_maxid) {
		debugPrintf("[HZLASER] runner symbols missing\n");
		return 0;
	}
	for (int i = 0; i < NLV; i++)
		if (!(lv[i] = (const int *)so_symbol(&yoyoloader_mod, lv_names[i]))) {
			debugPrintf("[HZLASER] no %s\n", lv_names[i]);
			return 0;
		}
	if (debugMode) {
		verify = exp_flag("hzlaserverify") && dirty_list;
		fault = verify && exp_flag("hzlaserfault");
		ab = exp_flag("hzlaserab");
		prof = exp_flag("hzlaserprof"); // section timers inside the native
	}
	// exp_hzlaserab and exp_hzlaserprof time the whole event per call (same two clock reads in both A/B states)
	event_func o = gml_replace(table, "gml_Object_obj_ceiling_laser_Step_0", prof || ab ? laser_timed : laser_event);
	if (!o)
		return 0;
	laser_orig = o;
	laser_on = 1;
	return 1;
}
