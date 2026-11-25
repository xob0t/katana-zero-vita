/*
 * flashlight_native.c
 *
 * Native obj_flashlightmaster user event 2 (gml_Object_obj_flashlightmaster_Other_12, APK 1.1.10, 58 KB of YYC), the
 * mansion gangsters' flashlights. flash_mask.c's flashlight_draw calls it once per frame with the light surface as
 * the render target. The event, from the ARM listing (work/decomp/out/gml_Object_obj_flashlightmaster_Other_12.*):
 *
 *   with (obj_enemy) {
 *       if (fall_sprite != spr_gangsterfall) continue;
 *       aim_direction = point_direction(x, y - 30, lag_x, lag_y);        // then +- recoil * global.fr
 *       round_aim_direction = round(aim_direction / 5) * 5;
 *       arm, gun and laser positions (lengthdir_x/y, point_direction);
 *       ai_set_gunpoint();                                               // gunpoint_x/y/angle by sprite and frame
 *       flashlight_x/y/angle ease towards the gun point; center_x/y = beam origin;
 *       arc_width = 6; lights = 3; light_offset = 10 (simple: 1, 80);
 *       if (flashlight_light[0] == noone) create the lights, else for each light:
 *           flashlight_light[i].x/.y = center + lengthdir(...);
 *           with (flashlight_light[i]) if (collision_line(x, y, other.center_x, other.center_y, obj_block, 0, 0))
 *               { x = -10000; y = -10000; region = -1; update_timer = 0 }
 *           else if (--update_timer <= 0) { update_timer = 5; update_region(); }
 *       flashlight_light[0].y += 32 (simple: 16);
 *       if neither the beam's end nor its origin is in the view (10 px margin): continue;
 *       beam_range = 400, shortened by an obj_door on the beam (x distance) or by a 3-step search for obj_block;
 *       if the player is inside the beam and visible from the origin: beam_range = its distance, beam_alpha by angle;
 *       center_x/y = (center - view) / 2; two triangle fans of draw_vertex_colour (i from -6 and -7 in steps of 5).
 *   }
 *
 * It costs 1.8-2.2 ms per frame in mansion_2_2 and mansion_3 (frame audit, Nov 25), 1.6 ms of it in the event's own
 * code: ~210 variable lookups through the instance vtable, RValue temporaries and operator calls per gangster, and
 * ai_set_gunpoint's ~60 sprite/frame comparisons.
 *
 * The native makes the same runner calls with the same arguments in the same order: the with iterator over obj_enemy,
 * YYCompareVal for the sprite and instance comparisons, round / angle_difference / point_distance / instance_exists /
 * collision_line / draw_primitive_begin / draw_vertex_colour / draw_primitive_end through YYGML_CallLegacyFunction,
 * YYGML_point_direction and YYGML_lengthdir_x/y with the same float arguments, YYGML_sign / YYGML_abs,
 * Variable_SetValue for the lights' x/y, Variable_SetValue_Direct for x/y = -10000, YYGML_Variable_GetValue and
 * YYGML_ErrCheck_Variable_GetValue for x/y, hit.x and obj_player's x/y/invincible, gml_Script_update_region.
 * Comparisons of two reals are YYCompareVal's (|a - b| <= g_GMLMathEpsilon is equal); the arithmetic is in doubles in
 * the order of the compiled expressions (built with -ffp-contract=off). Variables are written as YYC writes them:
 * value and kind 0, flags kept (plain copies such as flashlight_x = gunpoint_x copy the whole RValue). Nothing here
 * draws a random number.
 *
 * A read-only pass first walks the with iterator's instances and checks every gangster: the variables the event
 * reads are reals, the ones it writes exist and hold no string/array/struct, flashlight_light is an array with the
 * lights, each light resolves to one instance with a real update_timer, and the globals are reals. Two cases run the
 * original event for the whole frame: a gangster whose lights don't exist yet (flashlight_light[0] == noone: the
 * create branch, the first frame), and one in state 5 or 9 on another sprite than spr_gangsterturn, where
 * ai_set_gunpoint calls the enemy's user event 14. Nothing after the check can change what it found: the event writes
 * only the gangster's own variables, its lights and (through update_region) obj_room_seed's.
 *
 * exp_flashlightnative0 runs the original event. Debug: exp_flashlightverify runs the native (draws recorded, not
 * submitted), puts back every variable of the gangsters, their lights and obj_room_seed and the lights' x/y (through
 * Variable_SetValue_Direct), runs the original, and compares the variables (kind and value; flags of reals counted
 * apart), the gangsters' CInstance fields, the lights' x/y, the random state, room_maxid and the lists of
 * collision_line calls (arguments and results) and draw calls ([FLASHLIGHT] report). The flags word of reals is
 * counted apart, not as a mismatch: where the original assigns an RValue temporary it copies that temporary's
 * uninitialised flags (0, 3, 0x78787878, ... from the stack); the native keeps the variable's (exp_flashlightflags
 * prints the first ones). exp_flashlightfault (with verify) adds 1e-9 to one gangster's center_y as a negative
 * control. exp_flashlightab switches the native off in every other report window; the report has the event's time
 * per frame (debugMode). exp_flashlightprof times the check pass and the event's parts.
 *
 * One more thing the original does: at the top of `with (flashlight_light[i])` YYC reads `i` in the light's scope
 * (and drops it). The instance getter creates the variable, unset, on a light that doesn't have it yet; the native
 * makes the same read.
 */
#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

#define ABI __attribute__((pcs("aapcs")))
typedef gml_rvalue RV;
typedef RV *ABI (*legacy_fn)(void *self, void *other, RV *result, int argc, int funcid, RV **argv);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double eps, int throw_error);
typedef int ABI (*bool_fn)(const RV *v);
typedef RV *ABI (*read_var_fn)(void *obj, int id);
typedef void ABI (*getvalue_fn)(const RV *target, int var, int index, RV *out, int a, int b);
typedef void ABI (*errcheck_fn)(const RV *target, int var, int index, RV *out, void *self, void *other);
typedef int ABI (*setvalue_fn)(const RV *target, int var, int index, RV *val);
typedef int ABI (*setdirect_fn)(void *obj, int var, int index, RV *val);
typedef int ABI (*new_with_fn)(void *it, void **self, void **other, const RV *target);
typedef int ABI (*next_with_fn)(void *it, void **self, void **other);
typedef void ABI (*delete_with_fn)(void *it, void **self, void **other);
typedef float ABI (*pdir_fn)(float x1, float y1, float x2, float y2);
typedef float ABI (*ldir_fn)(float len, float dir);
typedef double ABI (*dfn)(double v);
typedef void ABI (*free_fn)(RV *v);
typedef int ABI (*find_fn)(const char *name);
typedef RV *ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);

#define NO_INDEX ((int)0x80000000)
#define KIND(v) ((v)->kind & 0xffffff)
#define REFCOUNTED(k) ((1u << ((k) & 31)) & 0x46u) // string, array, object: what YYC frees before a write
#define MAX_INST 128

// The gangster's variables the event touches
#define EVARS(X) X(lag_x) X(lag_y) X(aim_direction) X(recoil) X(round_aim_direction) X(dir) X(leftarm_x) X(leftarm_y) \
	X(leftarm_angle) X(gun_x) X(gun_y) X(gun_angle) X(rightarm_x) X(rightarm_y) X(rightarm_angle) X(laser_x) X(laser_y) \
	X(gunpoint_dir) X(gunpoint_frame) X(gunpoint_angle) X(gunpoint_x) X(gunpoint_y) X(animation_index) X(state) \
	X(flashlight_x) X(flashlight_y) X(flashlight_angle) X(ad) X(center_x) X(center_y) X(arc_width) X(i) X(lights) \
	X(light_offset) X(flashlight_light)
#define E_ENUM(n) E_##n,
enum { EVARS(E_ENUM) NEV };
// Read before the event writes them: must be finite reals
#define READ_VARS (1ull << E_lag_x | 1ull << E_lag_y | 1ull << E_recoil | 1ull << E_dir | 1ull << E_gunpoint_x | \
	1ull << E_gunpoint_y | 1ull << E_animation_index | 1ull << E_state | 1ull << E_flashlight_x | 1ull << E_flashlight_y | \
	1ull << E_flashlight_angle)
#define OTHER_VARS(X) X(fall_sprite) X(x) X(y) X(sprite_index) X(simple) X(flashlight_col) X(fr) X(xview) X(yview) \
	X(wview) X(hview) X(region) X(update_timer) X(invincible)
#define V_DECL(n) static int *v_##n;
EVARS(V_DECL)
OTHER_VARS(V_DECL)
static int *const *const evar_ids[NEV] = {
#define V_ADDR(n) &v_##n,
	EVARS(V_ADDR)
};
#define ID(n) (v_##n[1])

static event_func orig_user2;
static legacy_fn legacy;
static compare_fn compare;
static bool_fn yy_bool;
static read_var_fn read_var;
static getvalue_fn get_value;
static errcheck_fn errcheck_get;
static setvalue_fn set_value;
static setdirect_fn set_direct;
static new_with_fn new_with;
static next_with_fn next_with;
static delete_with_fn delete_with;
static pdir_fn point_direction;
static ldir_fn lengthdir_x, lengthdir_y;
static dfn yy_sign, yy_abs;
static free_fn yy_free;
static script_fn update_region;
static const double *epsilon;
static const RV *const_false;
static uint32_t *rng_state, *rng_index;
static int *room_maxid;
static const int *g_func[8];
enum { F_ROUND, F_ANGLE_DIFFERENCE, F_POINT_DISTANCE, F_INSTANCE_EXISTS, F_COLLISION_LINE, F_PRIM_BEGIN, F_VERTEX_COLOUR,
	F_PRIM_END };
static int fid[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static const char *const fnames[8] = {"round", "angle_difference", "point_distance", "instance_exists", "collision_line",
	"draw_primitive_begin", "draw_vertex_colour", "draw_primitive_end"};

// Objects and sprites (resolved by name at install, checked against the listing's constants)
static int obj_enemy, obj_block, obj_door, obj_player, obj_room_seed;
static find_fn object_find, sprite_find;
enum { S_IDLE, S_WALK, S_WHIP, S_HURTFLY, S_FALL, S_HURTGROUND, S_STUNNED, S_TURN, S_RUN, NSPR };
static const char *const sprite_names[NSPR] = {"spr_gangsteridle", "spr_gangsterwalk", "spr_gangsterwhip",
	"spr_gangsterhurtfly", "spr_gangsterfall", "spr_gangsterhurtground", "spr_gangsterstunned", "spr_gangsterturn",
	"spr_gangsterrun"};
static RV sprite_ref[NSPR];

// ai_set_gunpoint's frame tables (work/decomp/out/_lift_D_gunpoint.json, the script run in Unicorn): frame, x, y,
// angle (NAN: not written). hurtfly / fall / hurtground / stunned write x = y = -100000 for any frame.
typedef struct { signed char frame; signed char x, y; short angle; } gp_entry;
static const gp_entry gp_idle[] = {{0, 20, -25, -1}, {1, 20, -25, -1}, {2, 20, -24, -1}, {3, 18, -23, -1},
	{4, 20, -22, -1}, {5, 18, -23, -1}, {6, 20, -24, -1}, {7, 20, -25, -1}};
static const gp_entry gp_walk[] = {{0, 9, -21, -1}, {1, 9, -20, -1}, {2, 8, -19, -1}, {3, 5, -20, -1},
	{4, 4, -22, -1}, {5, 4, -21, -1}, {6, 5, -19, -1}, {7, 9, -19, -1}};
static const gp_entry gp_whip[] = {{0, 15, -25, -1}, {1, 7, -23, -1}, {2, 2, -23, 202}, {3, 0, -18, 217},
	{4, 12, -17, 270}, {5, 11, -22, 340}};
static const gp_entry gp_turn[] = {{0, -17, -26, 172}, {1, -15, -28, 150}, {2, -6, -35, 118}, {3, 7, -29, 45},
	{4, 13, -21, -1}, {5, 15, -24, -1}};
static const gp_entry gp_run[] = {{0, 15, -24, -1}, {1, 14, -21, -1}, {2, 9, -18, -1}, {3, 7, -20, -1},
	{4, 8, -22, -1}, {5, 6, -23, -1}, {6, 5, -22, -1}, {7, 5, -19, -1}, {8, 8, -18, -1}, {9, 12, -21, -1}};

typedef struct {
	void *inst;
	int gangster;
	RV *v[NEV];
	double x, y;
	unsigned spr_match; // bit per S_*
	int nlights;
	RV *elem[3];        // flashlight_light[k]
	void *light[3];
	RV *light_ut[3], *light_region[3];
} plan;
static plan plans[MAX_INST];
static int nplans;

static int installed, verify, fault, ab, ab_off, debug_flags, prof;
// exp_flashlightprof: time per frame of the check pass and of the event's parts (arms and gun point, lights, beam
// tests, fans)
static uint32_t t_plan, t_part[4], t_mark;
#define MARK(k) do { if (prof) { uint32_t now_ = sceKernelGetProcessTimeLow(); if ((k) >= 0) t_part[k] += now_ - t_mark; \
	t_mark = now_; } } while (0)
static unsigned n_native, n_orig, n_gangsters, n_visible, n_lightcalls;
static unsigned why_create, why_dispatch, why_line;
// branch coverage (session): dir -1, state 3, blocked lights, update_region calls, door hits, block searches, player lit
static unsigned c_dirm1, c_state3, c_blocked, c_region, c_door, c_search, c_lit;
static uint32_t ev_us, ev_frames;
#define FL_LINES 16
static unsigned reject_line[FL_LINES], reject_count[FL_LINES];

static int reject(unsigned line) {
	why_line = line;
	for (int k = 0; k < FL_LINES; k++)
		if (!reject_line[k] || reject_line[k] == line) {
			reject_line[k] = line;
			reject_count[k]++;
			break;
		}
	return 0;
}
#define NO(c) do { if (c) return reject(__LINE__); } while (0)

// YYCompareVal of two reals: equal within g_GMLMathEpsilon, else the sign of a - b
static inline int cmp(double a, double b) {
	double d = a - b;
	if (fabs(d) <= *epsilon)
		return 0;
	return d < 0 ? -1 : 1;
}
static inline int finite_real(const RV *v) {
	return v && KIND(v) == 0 && isfinite(v->val);
}
static inline int writable(const RV *v) {
	return v && !REFCOUNTED(KIND(v));
}
static inline RV real_rv(double d) {
	RV r = {d, 0, 0};
	return r;
}
static inline RV obj_ref(int obj) {
	RV v;
	uint32_t w[4] = {(uint32_t)obj, 0x1000000, 0, 15};
	memcpy(&v, w, sizeof(v));
	return v;
}
static inline RV inst_rv(void *inst) {
	RV v = {0, 0, 6};
	memcpy(&v.val, &inst, sizeof(inst));
	return v;
}
static inline void W(RV *slot, double d) { // value and kind; flags kept
	slot->val = d;
	slot->kind = 0;
}
static inline void release(RV *v) {
	if (REFCOUNTED(KIND(v)))
		yy_free(v);
}
static inline RV call(void *self, void *other, int f, int argc, RV **argv) {
	RV r = {0, 0, 5};
	legacy(self, other, &r, argc, fid[f], argv);
	return r;
}
static inline int is_noone(const RV *v) {
	RV noone = real_rv(-4.0);
	return compare(v, &noone, *epsilon, 0) == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The read-only pass

// An instance's variable map holds pointers to its values, which stay where they are for the instance's life, so the
// pointers are looked up once per instance (pointer and id) and kept, as in corpse_rest.c. The values are checked
// every frame.
#define CACHE_SLOTS 256
static struct { void *inst; uint32_t id; RV *fs; int full; RV *v[NEV]; } cache[CACHE_SLOTS];
static unsigned cache_fills;
static RV **cached_vars(void *e, RV **fs) {
	uint32_t id;
	memcpy(&id, (char *)e + 0x80, 4);
	unsigned slot = (id * 2654435761u) >> 24;
	if (cache[slot].inst != e || cache[slot].id != id) {
		cache[slot].inst = NULL;
		cache[slot].full = 0;
		RV *f = read_var(e, ID(fall_sprite));
		if (!f) {
			*fs = NULL;
			return NULL;
		}
		cache[slot].fs = f;
		cache[slot].inst = e;
		cache[slot].id = id;
		cache_fills++;
	}
	*fs = cache[slot].fs;
	if (!cache[slot].full) {
		int full = 1;
		for (int k = 0; k < NEV; k++)
			full &= (cache[slot].v[k] = read_var(e, (*evar_ids[k])[1])) != NULL;
		cache[slot].full = full;
		if (!full)
			return NULL;
	}
	return cache[slot].v;
}

static int plan_frame(void *self, RV **g, int *simple) {
	for (int k = 0; k < 8; k++)
		NO(fid[k] < 0);
	int *gv[5] = {v_fr, v_xview, v_yview, v_wview, v_hview};
	for (int k = 0; k < 5; k++) {
		g[k] = gml_vget(*global_obj_ptr, gv[k][1]);
		NO(!finite_real(g[k]));
	}
	RV *rs = read_var(self, ID(simple)), *col = read_var(self, ID(flashlight_col));
	NO(!rs || !col || REFCOUNTED(KIND(col)));
	*simple = yy_bool(rs) ? 1 : 0;
	int nl = *simple ? 1 : 3;
	// The with iterator's instances, in its order
	nplans = 0;
	{
		uint32_t it[16] = {0};
		void *s = self, *o = NULL;
		RV target = obj_ref(obj_enemy);
		int ok = 1;
		if (new_with(it, &s, &o, &target) > 0) {
			do {
				if (nplans == MAX_INST) {
					ok = 0;
					break;
				}
				plans[nplans++].inst = s;
			} while (next_with(it, &s, &o));
		}
		delete_with(it, &s, &o);
		NO(!ok);
	}
	for (int n = 0; n < nplans; n++) {
		plan *p = &plans[n];
		void *e = p->inst;
		RV *fs, **cv = cached_vars(e, &fs);
		NO(!fs); // the original's read of an unset variable is an error
		p->gangster = compare(fs, &sprite_ref[S_FALL], *epsilon, 0) == 0;
		if (!p->gangster)
			continue;
		NO(!cv); // a variable the event writes doesn't exist yet
		for (int k = 0; k < NEV; k++) {
			RV *v = p->v[k] = cv[k];
			if (READ_VARS >> k & 1)
				NO(!finite_real(v));
			else if (k != E_flashlight_light)
				NO(!writable(v));
		}
		RV erv = inst_rv(e), out = {0, 0, 5};
		get_value(&erv, ID(x), NO_INDEX, &out, 0, 0);
		NO(!finite_real(&out));
		p->x = out.val;
		out = (RV){0, 0, 5};
		get_value(&erv, ID(y), NO_INDEX, &out, 0, 0);
		NO(!finite_real(&out));
		p->y = out.val;
		RV spr = {0, 0, 5};
		get_value(&erv, ID(sprite_index), NO_INDEX, &spr, 0, 0);
		p->spr_match = 0;
		for (int k = 0; k < NSPR; k++) // nine different sprites: one at most is equal
			if (compare(&spr, &sprite_ref[k], *epsilon, 0) == 0) {
				p->spr_match |= 1u << k;
				break;
			}
		release(&spr);
		// ai_set_gunpoint calls user event 14 here: the original runs
		if (!(p->spr_match >> S_TURN & 1) && (cmp(p->v[E_state]->val, 5.0) == 0 || cmp(p->v[E_state]->val, 9.0) == 0)) {
			why_dispatch++;
			return 0;
		}
		RV *arr = p->v[E_flashlight_light];
		NO(KIND(arr) != 2);
		uint8_t *a;
		memcpy(&a, &arr->val, sizeof(a));
		NO(!a);
		int len = *(int *)(a + 0x1c);
		RV *el = *(RV **)(a + 4);
		NO(len < 1 || !el);
		if (is_noone(&el[0])) { // the create branch
			why_create++;
			return 0;
		}
		NO(len < nl);
		p->nlights = nl;
		for (int k = 0; k < nl; k++) {
			p->elem[k] = &el[k];
			NO(REFCOUNTED(KIND(&el[k])));
			uint32_t it[16] = {0};
			void *s = e, *o = NULL, *light = NULL;
			int count = 0;
			if (new_with(it, &s, &o, &el[k]) > 0) {
				do {
					light = s;
					count++;
				} while (next_with(it, &s, &o));
			}
			delete_with(it, &s, &o);
			NO(count != 1);
			p->light[k] = light;
			p->light_ut[k] = read_var(light, ID(update_timer));
			p->light_region[k] = read_var(light, ID(region));
			NO(!finite_real(p->light_ut[k]) || !writable(p->light_region[k]));
		}
	}
	return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// The event, per gangster

static RV *col_line(void *self, void *other, RV *x1, RV *y1, RV *x2, RV *y2, int obj, RV *out) {
	RV o = obj_ref(obj), f1 = *const_false, f2 = *const_false;
	RV *argv[7] = {x1, y1, x2, y2, &o, &f1, &f2};
	*out = call(self, other, F_COLLISION_LINE, 7, argv);
	n_lightcalls++;
	return out;
}

static void vertex(void *e, void *master, double x, double y, const RV *col, double alpha) {
	RV a0 = real_rv(x), a1 = real_rv(y), a2 = *col, a3 = real_rv(alpha);
	RV *argv[4] = {&a0, &a1, &a2, &a3};
	RV r = call(e, master, F_VERTEX_COLOUR, 4, argv);
	release(&r);
}

static void prim(void *e, void *master, int begin) {
	RV a0 = real_rv(6.0);
	RV *argv[1] = {&a0};
	RV r = begin ? call(e, master, F_PRIM_BEGIN, 1, argv) : call(e, master, F_PRIM_END, 0, argv);
	release(&r);
}

static void gunpoint_block(RV **v, const gp_entry *t, int n, double anim) {
	for (int k = 0; k < n; k++)
		if (cmp(anim, t[k].frame) == 0) {
			if (t[k].angle >= 0)
				W(v[E_gunpoint_angle], t[k].angle);
			W(v[E_gunpoint_x], t[k].x);
			W(v[E_gunpoint_y], t[k].y);
			return;
		}
}

// ai_set_gunpoint without the user event 14 branch (plan_frame sent those frames to the original)
static void set_gunpoint(plan *p) {
	RV **v = p->v;
	W(v[E_gunpoint_dir], 1.0);
	W(v[E_gunpoint_frame], 0.0);
	W(v[E_gunpoint_angle], 0.0);
	double anim = v[E_animation_index]->val;
	unsigned m = p->spr_match;
	if (m >> S_IDLE & 1)
		gunpoint_block(v, gp_idle, sizeof(gp_idle) / sizeof(gp_idle[0]), anim);
	if (m >> S_WALK & 1)
		gunpoint_block(v, gp_walk, sizeof(gp_walk) / sizeof(gp_walk[0]), anim);
	if (m >> S_WHIP & 1)
		gunpoint_block(v, gp_whip, sizeof(gp_whip) / sizeof(gp_whip[0]), anim);
	for (int s = S_HURTFLY; s <= S_STUNNED; s++)
		if (m >> s & 1) {
			W(v[E_gunpoint_x], -100000.0);
			W(v[E_gunpoint_y], -100000.0);
		}
	if (m >> S_TURN & 1)
		gunpoint_block(v, gp_turn, sizeof(gp_turn) / sizeof(gp_turn[0]), anim);
	if (m >> S_RUN & 1)
		gunpoint_block(v, gp_run, sizeof(gp_run) / sizeof(gp_run[0]), anim);
}

static void gangster(void *master, plan *p, RV **g, int simple, const RV *col) {
	MARK(-1);
	void *e = p->inst;
	RV **v = p->v;
	const double dir = v[E_dir]->val;
	// aim
	double aim = (double)point_direction((float)p->x, (float)(p->y - 30.0), (float)v[E_lag_x]->val,
		(float)v[E_lag_y]->val);
	W(v[E_aim_direction], aim);
	if (cmp(aim, 90.0) == 1 && cmp(aim, 270.0) == -1)
		aim = aim - v[E_recoil]->val * g[0]->val;
	else
		aim = aim + v[E_recoil]->val * g[0]->val;
	W(v[E_aim_direction], aim);
	{
		RV a0 = v[E_aim_direction][0];
		a0.val = aim / 5.0;
		RV *argv[1] = {&a0};
		RV r = call(e, master, F_ROUND, 1, argv);
		W(v[E_round_aim_direction], r.val * 5.0);
		release(&r);
	}
	// arms, gun, laser
	const double rad = v[E_round_aim_direction]->val;
	const double lax = -12.0 * dir;
	W(v[E_leftarm_x], lax);
	W(v[E_leftarm_y], -42.0);
	const double la = (rad + 90.0) - 90.0 * dir;
	W(v[E_leftarm_angle], la);
	const double t1 = la - 18.0 * dir;
	const double gx = lax + (double)lengthdir_x(20.0f, (float)t1) * dir;
	W(v[E_gun_x], gx);
	const double gy = -42.0 + (double)lengthdir_y(20.0f, (float)t1) * dir;
	W(v[E_gun_y], gy);
	const double ga = (la + 90.0) - 90.0 * dir;
	W(v[E_gun_angle], ga);
	const double rax = 6.0 * dir;
	W(v[E_rightarm_x], rax);
	W(v[E_rightarm_y], -44.0);
	const double t2 = la + 20.0 * dir;
	const double hx = gx + (double)lengthdir_x(25.0f, (float)t2) * dir;
	const double hy = gy + (double)lengthdir_y(25.0f, (float)t2) * dir;
	// point_direction(...) + 90 - 90 * dir: YYC adds the 90 in single precision
	const float rpd = point_direction((float)rax, -44.0f, (float)hx, (float)hy);
	W(v[E_rightarm_angle], (double)(rpd + 90.0f) - 90.0 * dir);
	const double t3 = ga + 50.0 * dir;
	const double lsx = (p->x + gx) + (double)lengthdir_x(10.0f, (float)t3);
	W(v[E_laser_x], lsx);
	const double lsy = (p->y + gy) + (double)lengthdir_y(10.0f, (float)t3);
	W(v[E_laser_y], lsy);
	set_gunpoint(p);
	// flashlight easing
	if (cmp(dir, -1.0) == 0) {
		c_dirm1++;
		double gpa = v[E_gunpoint_angle]->val;
		gpa = gpa + (90.0 - gpa) * 2.0;
		W(v[E_gunpoint_angle], gpa);
		if (cmp(gpa, 360.0) == 1)
			W(v[E_gunpoint_angle], gpa - 360.0);
		gpa = v[E_gunpoint_angle]->val;
		if (cmp(gpa, 0.0) == -1)
			W(v[E_gunpoint_angle], gpa + 360.0);
	}
	if ((cmp(v[E_flashlight_x]->val, 0.0) == 0 && cmp(v[E_flashlight_y]->val, 0.0) == 0) ||
			cmp(v[E_gunpoint_x]->val, -100000.0) == 0) {
		*v[E_flashlight_x] = *v[E_gunpoint_x];
		*v[E_flashlight_y] = *v[E_gunpoint_y];
		*v[E_flashlight_angle] = *v[E_gunpoint_angle];
	}
	if (cmp(v[E_gunpoint_x]->val, 0.0) == 1)
		W(v[E_gunpoint_x], v[E_gunpoint_x]->val - 5.0);
	else
		W(v[E_gunpoint_x], v[E_gunpoint_x]->val + 5.0);
	W(v[E_gunpoint_y], v[E_gunpoint_y]->val + 1.0);
	const double gpx = v[E_gunpoint_x]->val, gpy = v[E_gunpoint_y]->val;
	if (cmp(v[E_flashlight_x]->val, gpx) == -1)
		W(v[E_flashlight_x], v[E_flashlight_x]->val + yy_abs(v[E_flashlight_x]->val - gpx) / 3.0);
	if (cmp(v[E_flashlight_x]->val, gpx) == 1)
		W(v[E_flashlight_x], v[E_flashlight_x]->val - yy_abs(v[E_flashlight_x]->val - gpx) / 3.0);
	if (cmp(v[E_flashlight_y]->val, gpy) == -1)
		W(v[E_flashlight_y], v[E_flashlight_y]->val + yy_abs(v[E_flashlight_y]->val - gpy) / 3.0);
	if (cmp(v[E_flashlight_y]->val, gpy) == 1)
		W(v[E_flashlight_y], v[E_flashlight_y]->val - yy_abs(v[E_flashlight_y]->val - gpy) / 3.0);
	{
		RV a0 = *v[E_gunpoint_angle], a1 = *v[E_flashlight_angle];
		RV *argv[2] = {&a0, &a1};
		RV r = call(e, master, F_ANGLE_DIFFERENCE, 2, argv);
		W(v[E_ad], r.val);
		release(&r);
	}
	const double adv = v[E_ad]->val;
	W(v[E_flashlight_angle], v[E_flashlight_angle]->val + yy_sign(adv) * (yy_abs(adv) * 0.5));
	if (cmp(v[E_state]->val, 3.0) == 0) {
		c_state3++;
		*v[E_flashlight_angle] = *v[E_aim_direction];
		*v[E_center_x] = *v[E_laser_x];
		*v[E_center_y] = *v[E_laser_y];
	} else {
		W(v[E_center_x], p->x + (v[E_flashlight_x]->val * dir) * 2.0);
		W(v[E_center_y], p->y + v[E_flashlight_y]->val * 2.0);
	}
	W(v[E_arc_width], 6.0);
	W(v[E_i], 0.0);
	W(v[E_lights], 3.0);
	W(v[E_light_offset], 10.0);
	if (simple) {
		W(v[E_lights], 1.0);
		W(v[E_light_offset], 80.0);
	}
	MARK(0);
	// the lights (flashlight_light[0] exists: checked)
	W(v[E_i], 0.0);
	const double fa = v[E_flashlight_angle]->val, lo = v[E_light_offset]->val, nl = v[E_lights]->val;
	for (int k = 0; k < p->nlights; k++) {
		const double ii = v[E_i]->val;
		const double len = ((400.0 - lo) / nl) * ii + lo;
		RV lx = real_rv(v[E_center_x]->val + (double)lengthdir_x((float)len, (float)fa));
		set_value(p->elem[k], ID(x), NO_INDEX, &lx);
		RV ly = real_rv(v[E_center_y]->val + (double)lengthdir_y((float)len, (float)fa));
		set_value(p->elem[k], ID(y), NO_INDEX, &ly);
		void *light = p->light[k];
		// YYC reads `i` in the light's scope at the top of the with body (and drops it): the instance's getter creates
		// the variable (unset) on a light that doesn't have it yet
		gml_vget(light, ID(i));
		RV lrv = inst_rv(light), x1 = {0, 0, 5}, y1 = {0, 0, 5};
		get_value(&lrv, ID(x), NO_INDEX, &x1, 0, 0);
		get_value(&lrv, ID(y), NO_INDEX, &y1, 0, 0);
		RV cx = *v[E_center_x], cy = *v[E_center_y], hit;
		col_line(light, e, &x1, &y1, &cx, &cy, obj_block, &hit);
		int blocked = !is_noone(&hit);
		release(&hit);
		release(&x1);
		release(&y1);
		if (blocked) {
			c_blocked++;
			RV far = real_rv(-10000.0);
			set_direct(light, ID(x), NO_INDEX, &far);
			far = real_rv(-10000.0);
			set_direct(light, ID(y), NO_INDEX, &far);
			W(p->light_region[k], -1.0);
			W(p->light_ut[k], 0.0);
		} else {
			W(p->light_ut[k], p->light_ut[k]->val - 1.0);
			if (cmp(p->light_ut[k]->val, 0.0) <= 0) {
				W(p->light_ut[k], 5.0);
				c_region++;
				RV r = {0, 0, 5};
				update_region(light, e, &r, 0, NULL);
				release(&r);
			}
		}
		W(v[E_i], v[E_i]->val + 1.0);
	}
	{
		RV y0 = {0, 0, 5};
		errcheck_get(p->elem[0], ID(y), NO_INDEX, &y0, e, master);
		y0.val = y0.val + (simple ? 16.0 : 32.0);
		set_value(p->elem[0], ID(y), NO_INDEX, &y0);
	}
	MARK(1);
	// the beam: only when its end or its origin is in the view
	const double vx = g[1]->val, vy = g[2]->val, vw = g[3]->val, vh = g[4]->val;
	double cxv = v[E_center_x]->val, cyv = v[E_center_y]->val;
	int vis = 0;
	{
		const double ex = cxv + (double)lengthdir_x(400.0f, (float)fa);
		if (cmp(ex, vx - 10.0) == 1 && cmp(ex, (vx + vw) + 10.0) == -1) {
			const double ey = cyv + (double)lengthdir_y(400.0f, (float)fa);
			if (cmp(ey, vy - 10.0) == 1 && cmp(ey, (vy + vh) + 10.0) == -1)
				vis = 1;
		}
		if (!vis && cmp(cxv, vx - 10.0) == 1 && cmp(cxv, (vx + vw) + 10.0) == -1 && cmp(cyv, vy - 10.0) == 1 &&
				cmp(cyv, (vy + vh) + 10.0) == -1)
			vis = 1;
	}
	if (!vis) {
		MARK(2);
		return;
	}
	n_visible++;
	double br = 400.0;
	{
		RV a0 = *v[E_center_x], a1 = *v[E_center_y];
		RV a2 = real_rv(cxv + (double)lengthdir_x(400.0f, (float)fa));
		RV a3 = real_rv(cyv + (double)lengthdir_y(400.0f, (float)fa)), hit;
		col_line(e, master, &a0, &a1, &a2, &a3, obj_door, &hit);
		if (!is_noone(&hit)) {
			c_door++;
			RV hx = {0, 0, 5}, z0 = real_rv(0.0), z1 = real_rv(0.0), c0 = *v[E_center_x];
			errcheck_get(&hit, ID(x), NO_INDEX, &hx, e, master);
			RV *argv[4] = {&c0, &z0, &hx, &z1};
			RV pd = call(e, master, F_POINT_DISTANCE, 4, argv);
			br = pd.val * yy_sign(400.0);
			release(&pd);
			release(&hx);
		}
		release(&hit);
	}
	if (cmp(br, 400.0) == 0) {
		RV a0 = *v[E_center_x], a1 = *v[E_center_y];
		RV a2 = real_rv(cxv + (double)lengthdir_x((float)br, (float)fa));
		RV a3 = real_rv(cyv + (double)lengthdir_y((float)br, (float)fa)), hit;
		col_line(e, master, &a0, &a1, &a2, &a3, obj_block, &hit);
		int blocked = !is_noone(&hit);
		release(&hit);
		if (blocked) {
			c_search++;
			double low = 0.0, high = br;
			for (int k = 0; k < 3; k++) {
				const double mid = (high + low) / 2.0;
				RV b0 = real_rv(cxv + (double)lengthdir_x((float)low, (float)fa));
				RV b1 = real_rv(cyv + (double)lengthdir_y((float)low, (float)fa));
				RV b2 = real_rv(cxv + (double)lengthdir_x((float)mid, (float)fa));
				RV b3 = real_rv(cyv + (double)lengthdir_y((float)mid, (float)fa)), c;
				col_line(e, master, &b0, &b1, &b2, &b3, obj_block, &c);
				if (is_noone(&c))
					low = mid;
				else
					high = mid;
				release(&c);
			}
			br = high + 16.0;
			if (cmp(br, 400.0) == 1)
				br = 400.0;
		}
	}
	double beam_alpha = 1.0;
	{
		RV pl = obj_ref(obj_player);
		RV *argv1[1] = {&pl};
		RV ie = call(e, master, F_INSTANCE_EXISTS, 1, argv1);
		int exists = yy_bool(&ie);
		release(&ie);
		int invincible = 1;
		if (exists) {
			RV inv = {0, 0, 5};
			get_value(&pl, ID(invincible), NO_INDEX, &inv, 0, 0);
			invincible = yy_bool(&inv);
			release(&inv);
		}
		if (exists && !invincible) {
			RV px = {0, 0, 5}, py = {0, 0, 5};
			errcheck_get(&pl, ID(x), NO_INDEX, &px, e, master);
			errcheck_get(&pl, ID(y), NO_INDEX, &py, e, master);
			RV c0 = *v[E_center_x], c1 = *v[E_center_y], py40 = py;
			py40.val = py.val - 40.0;
			RV *argv[4] = {&c0, &c1, &px, &py40};
			RV pd = call(e, master, F_POINT_DISTANCE, 4, argv);
			int closer = cmp(pd.val, br) == -1;
			release(&pd);
			if (closer) {
				const double pdir = (double)point_direction((float)cxv, (float)cyv, (float)px.val, (float)(py.val - 40.0));
				RV d0 = real_rv(pdir), d1 = *v[E_flashlight_angle];
				RV *argv2[2] = {&d0, &d1};
				RV adr = call(e, master, F_ANGLE_DIFFERENCE, 2, argv2);
				const double d = yy_abs(adr.val) - (v[E_arc_width]->val + 6.0);
				release(&adr);
				if (cmp(d, *epsilon) != 1) {
					RV c2 = *v[E_center_x], c3 = *v[E_center_y], py41 = py, c;
					py41.val = py.val - 40.0;
					col_line(e, master, &c2, &c3, &px, &py41, obj_block, &c);
					int clear = is_noone(&c);
					release(&c);
					if (clear) {
						c_lit++;
						RV c4 = *v[E_center_x], c5 = *v[E_center_y], py42 = py;
						py42.val = py.val - 40.0;
						RV *argv3[4] = {&c4, &c5, &px, &py42};
						RV pd2 = call(e, master, F_POINT_DISTANCE, 4, argv3);
						br = pd2.val;
						release(&pd2);
						const double pdir2 = (double)point_direction((float)cxv, (float)cyv, (float)px.val,
							(float)(py.val - 40.0));
						RV f0 = *v[E_flashlight_angle], f1 = real_rv(pdir2);
						RV *argv4[2] = {&f0, &f1};
						RV ad2 = call(e, master, F_ANGLE_DIFFERENCE, 2, argv4);
						beam_alpha = (1.0 - yy_abs(ad2.val / (v[E_arc_width]->val + 6.0))) * 0.5;
						release(&ad2);
					}
				}
			}
			release(&px);
			release(&py);
		}
	}
	MARK(2);
	// surface coordinates (written back to the gangster) and the two fans
	W(v[E_center_x], (v[E_center_x]->val - g[1]->val) / 2.0);
	W(v[E_center_y], (v[E_center_y]->val - g[2]->val) / 2.0);
	if (fault)
		W(v[E_center_y], v[E_center_y]->val + 1e-9);
	cxv = v[E_center_x]->val;
	cyv = v[E_center_y]->val;
	W(v[E_i], -6.0);
	const double rim = (0.1 + 0.9 * ((400.0 - br) / 400.0)) * beam_alpha;
	for (int fan = 0; fan < 2; fan++) {
		const double a = fan ? 0.6 : 0.9, half = fan ? 7.0 : 6.0;
		if (fan)
			W(v[E_i], -7.0);
		prim(e, master, 1);
		vertex(e, master, cxv, cyv, col, a);
		while (cmp(v[E_i]->val, half) != 1) {
			const double ang = fa + v[E_i]->val;
			const double px = cxv + (double)lengthdir_x((float)(br / 2.0), (float)ang);
			const double py = cyv + (double)lengthdir_y((float)(br / 2.0), (float)ang);
			vertex(e, master, px, py, col, rim * a);
			W(v[E_i], v[E_i]->val + 5.0);
		}
		vertex(e, master, cxv, cyv, col, a);
		prim(e, master, 0);
	}
	MARK(3);
}

static int native(void *self, void *other) {
	RV *g[5];
	int simple;
	uint32_t t0 = prof ? sceKernelGetProcessTimeLow() : 0;
	if (!plan_frame(self, g, &simple))
		return 0;
	if (prof)
		t_plan += sceKernelGetProcessTimeLow() - t0;
	RV col = *read_var(self, ID(flashlight_col));
	for (int n = 0; n < nplans; n++)
		if (plans[n].gangster) {
			gangster(self, &plans[n], g, simple, &col);
			n_gangsters++;
		}
	return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// exp_flashlightverify

typedef struct { int f; RV a[7]; RV r; } call_rec;
#define MAX_CALLS 1024
typedef struct { call_rec c[MAX_CALLS]; int n, overflow; } call_log;
static call_log *logs; // [2]
static call_log *cur_log;
static int submit_draws = 1;
static runner_function *rf[8];
static void *real_fn[8];

static void record(int f, RV *result, int argc, RV *args) {
	if (!cur_log)
		return;
	if (cur_log->n == MAX_CALLS) {
		cur_log->overflow = 1;
		return;
	}
	call_rec *c = &cur_log->c[cur_log->n++];
	c->f = f;
	memset(c->a, 0, sizeof(c->a));
	memcpy(c->a, args, (argc < 7 ? argc : 7) * sizeof(RV));
	c->r = result ? *result : (RV){0, 0, 0};
}
static void rec_collision(RV *result, void *self, void *other, int argc, RV *args) {
	((builtin_fn)real_fn[F_COLLISION_LINE])(result, self, other, argc, args);
	record(F_COLLISION_LINE, result, argc, args);
}
#define REC_DRAW(name, F) \
	static void name(RV *result, void *self, void *other, int argc, RV *args) { \
		record(F, NULL, argc, args); \
		if (submit_draws) \
			((builtin_fn)real_fn[F])(result, self, other, argc, args); \
	}
REC_DRAW(rec_begin, F_PRIM_BEGIN)
REC_DRAW(rec_vertex, F_VERTEX_COLOUR)
REC_DRAW(rec_end, F_PRIM_END)

typedef struct { RV *v; int key; uint32_t hash; } var_elem;
typedef struct { int size, used, mask, grow; var_elem *elems; } var_map;
#define SNAP_INST 160
#define SNAP_VARS 16384
typedef struct {
	int ninst, nvars, overflow;
	void *inst[SNAP_INST];
	int first[SNAP_INST + 1];
	int kind[SNAP_INST]; // 0 gangster/enemy, 1 light, 2 seed, 3 master
	double lx[SNAP_INST], ly[SNAP_INST];
	uint8_t fields[SNAP_INST][0x150 - 0x84];
	int key[SNAP_VARS];
	RV *ptr[SNAP_VARS];
	RV val[SNAP_VARS];
	uint32_t rng[16], rng_i;
	int maxid;
} snapshot;
static snapshot *snaps; // [3]: before, native, original
static unsigned v_frames, v_bad, v_skipped, bad_vars, bad_flags, bad_fields, bad_xy, bad_rng, bad_calls, v_calls;

static void snap_inst(snapshot *s, void *inst, int kind) {
	if (s->ninst == SNAP_INST) {
		s->overflow = 1;
		return;
	}
	int n = s->ninst++;
	s->inst[n] = inst;
	s->kind[n] = kind;
	s->first[n] = s->nvars;
	var_map *m = *(var_map **)((char *)inst + 0x24);
	for (int i = 0; m && i < m->size; i++) {
		if (!m->elems[i].hash)
			continue;
		if (s->nvars == SNAP_VARS) {
			s->overflow = 1;
			break;
		}
		s->key[s->nvars] = m->elems[i].key;
		s->ptr[s->nvars] = m->elems[i].v;
		s->val[s->nvars] = *m->elems[i].v;
		s->nvars++;
	}
	s->first[n + 1] = s->nvars;
	memcpy(s->fields[n], (char *)inst + 0x84, sizeof(s->fields[n]));
	s->lx[n] = s->ly[n] = 0;
	if (kind == 1) {
		RV rv = inst_rv(inst), o = {0, 0, 5};
		get_value(&rv, ID(x), NO_INDEX, &o, 0, 0);
		s->lx[n] = o.val;
		o = (RV){0, 0, 5};
		get_value(&rv, ID(y), NO_INDEX, &o, 0, 0);
		s->ly[n] = o.val;
	}
}

static void take(snapshot *s, void *master) {
	s->ninst = s->nvars = s->overflow = 0;
	for (int n = 0; n < nplans; n++) {
		snap_inst(s, plans[n].inst, 0);
		if (plans[n].gangster)
			for (int k = 0; k < plans[n].nlights; k++)
				snap_inst(s, plans[n].light[k], 1);
	}
	uint32_t it[16] = {0};
	void *a = master, *o = NULL;
	RV t = obj_ref(obj_room_seed);
	if (new_with(it, &a, &o, &t) > 0) {
		do
			snap_inst(s, a, 2);
		while (next_with(it, &a, &o));
	}
	delete_with(it, &a, &o);
	snap_inst(s, master, 3);
	memcpy(s->rng, rng_state, sizeof(s->rng));
	s->rng_i = *rng_index;
	s->maxid = *room_maxid;
}

// Plain values back; lights' x/y through the runner's setter. 0 if a string/array/struct changed.
static int put_back(const snapshot *s) {
	int ok = 1;
	for (int n = 0; n < s->ninst; n++) {
		for (int j = s->first[n]; j < s->first[n + 1]; j++) {
			RV *v = s->ptr[j];
			if (REFCOUNTED(KIND(v)) || REFCOUNTED(KIND(&s->val[j]))) {
				if (memcmp(v, &s->val[j], sizeof(*v)))
					ok = 0;
				continue;
			}
			*v = s->val[j];
		}
		if (s->kind[n] == 1) {
			RV x = real_rv(s->lx[n]), y = real_rv(s->ly[n]);
			set_direct(s->inst[n], ID(x), NO_INDEX, &x);
			set_direct(s->inst[n], ID(y), NO_INDEX, &y);
		}
	}
	memcpy(rng_state, s->rng, sizeof(s->rng));
	*rng_index = s->rng_i;
	return ok;
}

static int same_snap(const snapshot *a, const snapshot *b) {
	int ok = 1;
	if (a->ninst != b->ninst || a->nvars != b->nvars || a->overflow || b->overflow) {
		if (bad_vars < 12) {
			debugPrintf("[FLASHLIGHT] verify: snapshots differ: instances %d/%d variables %d/%d overflow %d/%d\n",
				a->ninst, b->ninst, a->nvars, b->nvars, a->overflow, b->overflow);
			for (int n = 0; n < a->ninst && n < b->ninst; n++)
				if (a->inst[n] != b->inst[n] || a->first[n + 1] - a->first[n] != b->first[n + 1] - b->first[n]) {
					debugPrintf("[FLASHLIGHT] verify: inst %d (kind %d) %p/%p has %d/%d variables\n", n, a->kind[n],
						a->inst[n], b->inst[n], a->first[n + 1] - a->first[n], b->first[n + 1] - b->first[n]);
					for (int j = b->first[n]; j < b->first[n + 1]; j++) {
						int found = 0;
						for (int k = a->first[n]; k < a->first[n + 1] && !found; k++)
							found = a->key[k] == b->key[j];
						if (!found)
							debugPrintf("[FLASHLIGHT] verify:   only the original has var %d = %.17g/%u\n", b->key[j],
								b->val[j].val, KIND(&b->val[j]));
					}
					for (int j = a->first[n]; j < a->first[n + 1]; j++) {
						int found = 0;
						for (int k = b->first[n]; k < b->first[n + 1] && !found; k++)
							found = b->key[k] == a->key[j];
						if (!found)
							debugPrintf("[FLASHLIGHT] verify:   only the native has var %d = %.17g/%u\n", a->key[j],
								a->val[j].val, KIND(&a->val[j]));
					}
					break;
				}
		}
		bad_vars++;
		return 0;
	}
	for (int n = 0; n < a->ninst; n++) {
		for (int j = a->first[n]; j < a->first[n + 1]; j++) {
			const RV *x = &a->val[j], *y = &b->val[j];
			if (a->key[j] != b->key[j] || KIND(x) != KIND(y) || memcmp(&x->val, &y->val, 8)) {
				if (bad_vars < 12)
					debugPrintf("[FLASHLIGHT] verify: inst %d (kind %d) var %d native %.17g/%u original %.17g/%u\n", n,
						a->kind[n], a->key[j], x->val, KIND(x), y->val, KIND(y));
				bad_vars++;
				ok = 0;
			} else if (x->flags != y->flags && KIND(x) != 0 && KIND(x) != 13) {
				if (bad_vars < 12)
					debugPrintf("[FLASHLIGHT] verify: inst %d (kind %d) var %d kind %u flags native %08x original %08x\n",
						n, a->kind[n], a->key[j], KIND(x), x->flags, y->flags);
				bad_vars++;
				ok = 0;
			} else if (x->flags != y->flags) {
				if (bad_flags < 16 && debug_flags)
					debugPrintf("[FLASHLIGHT] verify: inst %d (kind %d) var %d = %.17g flags native %08x original %08x "
						"before %08x\n", n, a->kind[n], a->key[j], x->val, x->flags, y->flags, snaps[0].val[j].flags);
				bad_flags++; // reals and bools: counted apart
			}
		}
		if (a->kind[n] == 0 && memcmp(a->fields[n], b->fields[n], sizeof(a->fields[n]))) {
			bad_fields++;
			ok = 0;
		}
		if (a->kind[n] == 1 && (memcmp(&a->lx[n], &b->lx[n], 8) || memcmp(&a->ly[n], &b->ly[n], 8))) {
			if (bad_xy < 12)
				debugPrintf("[FLASHLIGHT] verify: light %d x/y native %.9g,%.9g original %.9g,%.9g\n", n, a->lx[n],
					a->ly[n], b->lx[n], b->ly[n]);
			bad_xy++;
			ok = 0;
		}
	}
	if (memcmp(a->rng, b->rng, sizeof(a->rng)) || a->rng_i != b->rng_i || a->maxid != b->maxid) {
		bad_rng++;
		ok = 0;
	}
	return ok;
}

static int same_calls(const call_log *a, const call_log *b) {
	if (a->overflow || b->overflow || a->n != b->n) {
		if (bad_calls < 12)
			debugPrintf("[FLASHLIGHT] verify: calls native %d original %d (overflow %d %d)\n", a->n, b->n, a->overflow,
				b->overflow);
		bad_calls++;
		return 0;
	}
	for (int k = 0; k < a->n; k++) {
		const call_rec *x = &a->c[k], *y = &b->c[k];
		int diff = x->f != y->f || KIND(&x->r) != KIND(&y->r) || memcmp(&x->r.val, &y->r.val, 8);
		for (int j = 0; j < 7; j++)
			diff |= KIND(&x->a[j]) != KIND(&y->a[j]) || memcmp(&x->a[j].val, &y->a[j].val, 8);
		if (diff) {
			if (bad_calls < 12)
				debugPrintf("[FLASHLIGHT] verify: call %d %s/%s args %.17g %.17g %.17g %.17g | %.17g %.17g %.17g %.17g\n",
					k, fnames[x->f], fnames[y->f], x->a[0].val, x->a[1].val, x->a[2].val, x->a[3].val, y->a[0].val,
					y->a[1].val, y->a[2].val, y->a[3].val);
			bad_calls++;
			return 0;
		}
	}
	return 1;
}

static void verify_frame(void *self, void *other) {
	RV *g[5];
	int simple;
	if (!plan_frame(self, g, &simple)) {
		n_orig++;
		orig_user2(self, other);
		return;
	}
	snapshot *before = &snaps[0], *nat = &snaps[1], *org = &snaps[2];
	take(before, self);
	logs[0].n = logs[0].overflow = logs[1].n = logs[1].overflow = 0;
	cur_log = &logs[0];
	submit_draws = 0;
	RV col = *read_var(self, ID(flashlight_col));
	for (int n = 0; n < nplans; n++)
		if (plans[n].gangster) {
			gangster(self, &plans[n], g, simple, &col);
			n_gangsters++;
		}
	submit_draws = 1;
	cur_log = NULL;
	take(nat, self);
	if (!put_back(before)) {
		v_skipped++;
		n_orig++;
		orig_user2(self, other);
		return;
	}
	cur_log = &logs[1];
	orig_user2(self, other);
	cur_log = NULL;
	take(org, self);
	n_native++;
	v_frames++;
	v_calls += logs[1].n;
	int ok = same_snap(nat, org);
	ok &= same_calls(&logs[0], &logs[1]);
	if (!ok)
		v_bad++;
}

// ---------------------------------------------------------------------------------------------------------------

static void flashlight_user2(void *self, void *other) {
	uint32_t t0 = debugMode ? sceKernelGetProcessTimeLow() : 0;
	if (ab_off)
		orig_user2(self, other), n_orig++;
	else if (verify && snaps && logs)
		verify_frame(self, other);
	else if (native(self, other))
		n_native++;
	else {
		n_orig++;
		orig_user2(self, other);
	}
	if (debugMode) {
		ev_us += sceKernelGetProcessTimeLow() - t0;
		ev_frames++;
	}
}

void flashlight_native_report(int frames) {
	if (!installed || frames <= 0)
		return;
	if (debugMode && (n_native || n_orig)) {
		float f = (float)frames;
		debugPrintf("[FLASHLIGHT] native %u original %u (session: create %u, user 14 %u, last check line %u) | "
			"gangsters %.1f, beams %.1f, collision_line %.1f per frame | event %.3f ms/frame%s\n", n_native, n_orig,
			why_create, why_dispatch, why_line, n_gangsters / f, n_visible / f, n_lightcalls / f,
			ev_frames ? ev_us / 1000.0f / f : 0.0f, ab ? (ab_off ? " | A/B: off" : " | A/B: on") : "");
		if (verify)
			debugPrintf("[FLASHLIGHT] verify: %u frames compared, %u with mismatches, %u not compared | vars %u fields "
				"%u light x/y %u rng/maxid %u calls %u (%u original calls), real flags differing %u | covered: dir -1 %u, "
				"state 3 %u, lights blocked %u, update_region %u, door %u, block search %u, player lit %u (session)%s\n",
				v_frames, v_bad, v_skipped, bad_vars, bad_fields, bad_xy, bad_rng, bad_calls, v_calls, bad_flags, c_dirm1,
				c_state3, c_blocked, c_region, c_door, c_search, c_lit, fault ? " | FAULT INJECTED" : "");
		if (prof)
			debugPrintf("[FLASHLIGHT] prof ms/frame: check %.3f, arms+gunpoint %.3f, lights %.3f, beam tests %.3f, "
				"fans %.3f | variable lookups %u instances (session)\n", t_plan / 1000.0f / f, t_part[0] / 1000.0f / f,
				t_part[1] / 1000.0f / f, t_part[2] / 1000.0f / f, t_part[3] / 1000.0f / f, cache_fills);
		for (int k = 0; k < FL_LINES && reject_line[k]; k++)
			debugPrintf("[FLASHLIGHT] original at flashlight_native.c:%u x%u (session)\n", reject_line[k],
				reject_count[k]);
	}
	n_native = n_orig = n_gangsters = n_visible = n_lightcalls = 0;
	ev_us = ev_frames = 0;
	t_plan = t_part[0] = t_part[1] = t_part[2] = t_part[3] = 0;
	if (ab)
		ab_off = !ab_off;
}

#define SYM(n) so_symbol(&yoyoloader_mod, n)

// patch_performance: the event table entry
int flashlight_native_install(gml_func_entry *table) {
#define V_LOOKUP(n) v_##n = (int *)SYM("g_VAR_" #n);
	EVARS(V_LOOKUP)
	OTHER_VARS(V_LOOKUP)
#define V_CHECK(n) if (!v_##n) { debugPrintf("[FLASHLIGHT] no g_VAR_%s\n", #n); return 0; }
	EVARS(V_CHECK)
	OTHER_VARS(V_CHECK)
	legacy = (legacy_fn)SYM("_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	compare = (compare_fn)SYM("_Z12YYCompareValRK6RValueS1_db");
	yy_bool = (bool_fn)SYM("_Z11BOOL_RValuePK6RValue");
	read_var = (read_var_fn)SYM("_ZN12YYObjectBase17InternalReadYYVarEi");
	get_value = (getvalue_fn)SYM("_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	errcheck_get = (errcheck_fn)SYM("_Z32YYGML_ErrCheck_Variable_GetValueRK8YYRValueiiP6RValueP9CInstanceS5_");
	set_value = (setvalue_fn)SYM("_Z17Variable_SetValueRK8YYRValueiiP6RValue");
	set_direct = (setdirect_fn)SYM("_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
	new_with = (new_with_fn)SYM("_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	next_with = (next_with_fn)SYM("_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
	delete_with = (delete_with_fn)SYM("_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_");
	point_direction = (pdir_fn)SYM("_Z21YYGML_point_directionffff");
	lengthdir_x = (ldir_fn)SYM("_Z17YYGML_lengthdir_xff");
	lengthdir_y = (ldir_fn)SYM("_Z17YYGML_lengthdir_yff");
	yy_sign = (dfn)SYM("_Z10YYGML_signd");
	yy_abs = (dfn)SYM("_Z9YYGML_absd");
	yy_free = (free_fn)SYM("_Z16FREE_RValue__PreP6RValue");
	update_region = (script_fn)SYM("_Z24gml_Script_update_regionP9CInstanceS0_R8YYRValueiPPS1_");
	epsilon = (const double *)SYM("g_GMLMathEpsilon");
	const_false = (const RV *)SYM("gs_constFalse");
	rng_state = (uint32_t *)SYM("state");
	rng_index = (uint32_t *)SYM("g_RndIndex");
	room_maxid = (int *)SYM("room_maxid");
	object_find = (find_fn)SYM("_Z11Object_FindPKc");
	sprite_find = (find_fn)SYM("_Z11Sprite_FindPKc");
	for (int k = 0; k < 8; k++) {
		char name[48];
		snprintf(name, sizeof(name), "g_FUNC_%s", fnames[k]);
		if (!(g_func[k] = (const int *)SYM(name))) {
			debugPrintf("[FLASHLIGHT] no %s\n", name);
			return 0;
		}
	}
	if (!legacy || !compare || !yy_bool || !read_var || !get_value || !errcheck_get || !set_value || !set_direct ||
			!new_with || !next_with || !delete_with || !point_direction || !lengthdir_x || !lengthdir_y || !yy_sign ||
			!yy_abs || !yy_free || !update_region || !epsilon || !const_false || !rng_state || !rng_index ||
			!room_maxid || !object_find || !sprite_find || !global_obj_ptr) {
		debugPrintf("[FLASHLIGHT] runner symbols missing\n");
		return 0;
	}
	event_func o = gml_replace(table, "gml_Object_obj_flashlightmaster_Other_12", flashlight_user2);
	if (!o)
		return 0;
	orig_user2 = o;
	installed = 1;
	if (debugMode) {
		verify = exp_flag("flashlightverify");
		fault = verify && exp_flag("flashlightfault");
		debug_flags = verify && exp_flag("flashlightflags");
		ab = exp_flag("flashlightab");
		prof = exp_flag("flashlightprof");
	}
	return 1;
}

// katana_late_install: the builtins' ids (and the verify mode's call recorders) need the function registry
void flashlight_native_late_install(void) {
	if (!installed)
		return;
	// The objects and sprites, by name (loaded by Startup); the listing's constants where it has them
	obj_enemy = object_find("obj_enemy");
	obj_block = object_find("obj_block");
	obj_door = object_find("obj_door");
	obj_player = object_find("obj_player");
	obj_room_seed = object_find("obj_room_seed");
	if (obj_enemy != 407 || obj_block != 57 || obj_door != 343 || obj_player != 745 || obj_room_seed < 0) {
		debugPrintf("[FLASHLIGHT] unexpected object indices %d %d %d %d %d\n", obj_enemy, obj_block, obj_door, obj_player,
			obj_room_seed);
		fid[0] = -1;
		return;
	}
	for (int k = 0; k < NSPR; k++) {
		int s = sprite_find(sprite_names[k]);
		if (s < 0) {
			debugPrintf("[FLASHLIGHT] no %s\n", sprite_names[k]);
			fid[0] = -1;
			return;
		}
		uint32_t w[4] = {(uint32_t)s, 0x1000001, 0, 15};
		memcpy(&sprite_ref[k], w, sizeof(RV));
	}
	uint32_t fall_index, idle_index;
	memcpy(&fall_index, &sprite_ref[S_FALL], 4);
	memcpy(&idle_index, &sprite_ref[S_IDLE], 4);
	if (fall_index != 0x2b1 || idle_index != 0x2b5) {
		debugPrintf("[FLASHLIGHT] unexpected sprite indices\n");
		fid[0] = -1;
		return;
	}
	runner_function **functions = (runner_function **)SYM("the_functions");
	int *count = (int *)SYM("the_numb");
	for (int k = 0; k < 8; k++) {
		fid[k] = g_func[k][1];
		if (!functions || !*functions || !count || fid[k] < 0 || fid[k] >= *count ||
				strcmp((*functions)[fid[k]].name, fnames[k])) {
			debugPrintf("[FLASHLIGHT] builtin %s not found: native off\n", fnames[k]);
			fid[0] = -1;
			return;
		}
		rf[k] = &(*functions)[fid[k]];
	}
	if (!verify)
		return;
	snaps = malloc(3 * sizeof(snapshot));
	logs = malloc(2 * sizeof(call_log));
	if (!snaps || !logs) {
		debugPrintf("[FLASHLIGHT] verify: no memory\n");
		free(snaps);
		free(logs);
		snaps = NULL;
		logs = NULL;
		verify = 0;
		return;
	}
	static const int recorded[4] = {F_COLLISION_LINE, F_PRIM_BEGIN, F_VERTEX_COLOUR, F_PRIM_END};
	static void *const recorders[4] = {(void *)rec_collision, (void *)rec_begin, (void *)rec_vertex, (void *)rec_end};
	for (int k = 0; k < 4; k++) {
		real_fn[recorded[k]] = rf[recorded[k]]->func;
		rf[recorded[k]]->func = recorders[k];
	}
	debugPrintf("[FLASHLIGHT] verify on%s\n", fault ? " (fault injected)" : "");
}
