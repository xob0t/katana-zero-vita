/*
 * hud_sparks.c
 *
 * No battery-meter sparks on the HUD during slow motion (hudSparks=1 in yyl.cfg keeps them).
 *
 * While global.slowdown < 1 (and the game isn't paused), obj_hud's Draw spawns one obj_hudparticle spark a frame off
 * the battery meter (APK 1.1.10, gml_Object_obj_hud_Draw_0, GML lines 76-101 and 185-211):
 *
 *     zz = choose(0, 1, 2, 3);
 *     if (zz == 0) { bb = instance_create(150, 4 + offset + 51 - 32, obj_hudparticle);
 *                    with (bb) motion_add(random(360), random(10)); }   // and three more corners
 *
 * About 24 of them are alive at a time; each one Steps (gravity, friction, fade) and is drawn by the HUD itself
 * (line 299, `with (obj_hudparticle) event_user(15)`). Here they aren't created, and everything they change outside
 * themselves is done instead, so the game's state, its random numbers above all, stays as it was. In-run A/B on the
 * Vita (500 MHz, slow-mo held, exp_hudsparksab): club_4_2 fighting 20.90 -> 20.28 ms/frame over all slow-mo windows
 * (an earlier run 20.21 -> 19.76; -0.35..-0.71 ms between neighbouring windows of one slow-mo stretch), factory_3
 * 11.29 -> 10.53 ms in full slow-mo windows. What the skip reproduces:
 * - the instance id and the layer element id of each creation (room_maxid + 1, CLayerManager::m_CurrentElementID + 1;
 *   the skip only happens when the spark's depth layer exists, as in rain_skip.c), the same instance reference
 *   returned to bb, so with (bb) finds no instance as it finds none for an id that doesn't exist;
 * - the random numbers. The spark's Create draws three (direction = random(360), speed = random(5),
 *   size = 6 - random(2)), the with block two (motion_add's arguments), and its user event 15 one each time the HUD
 *   draws it (draw_set_alpha(random(..)), line 0 of the event, unconditional). The Step draws none. The skip draws the
 *   five at the creation and, at the HUD's line-299 with, one for each spark that would be alive (a call-site wrapper
 *   on that YYGML_NewWithIterator call, before the real iteration);
 * - the HUD's line-261 `instance_exists(obj_hudparticle)`, which decides whether that block (gpu_set_blendmode(1),
 *   the with, draw_set_alpha(1), the blend mode back) runs when nothing flickers: a call-site wrapper answers true
 *   while a spark would be alive.
 * Which sparks would be alive is simulated: each spark's direction, speed, hspeed, vspeed (CInstance floats), alpha,
 * size and pspeed with the runner's own arithmetic (CInstance::SetDirection, SetSpeed, SetHspeed, SetVspeed,
 * AddTo_Speed in float, the same libm calls through the runner's GOT, the GML statements in double, comparisons by
 * the runner's YYCompareVal with the live epsilon). The spark's Step (lines 1-37):
 *
 *     if (instance_exists(obj_gamepaused)) { if (speed != 0) pspeed = speed; speed = 0; }
 *     else { if (speed == 0) { speed = pspeed; pspeed = 0; }
 *            vspeed += 0.3; if (hspeed > 0) hspeed -= 0.1; if (hspeed < 0) hspeed += 0.1;
 *            speed -= 0.1; alpha -= 0.04;
 *            if (size <= 0 || alpha <= 0 || speed <= 0) instance_destroy(); }
 *
 * The simulated sparks step in obj_hud's Step (the pause deactivates every instance but a few masters, obj_hud and
 * the sparks included, and puts them back together at the resume). A new obj_hud instance (every room start and
 * restart; obj_hud isn't persistent) clears them: sparks aren't persistent either. obj_hudparticle has no sprite, no
 * mask, no parent and no Destroy event, and nothing else in the game reads it (the HUD's Draw is the only code that
 * names it, besides the hidden touch buttons' GUI Draw, whose sparks are still created and drawn).
 *
 * Not reproduced: the runner's bookkeeping (allocator, GC, instance lists), the sparks' trail positions (x, y,
 * xprev.., drawing only) and their draws.
 *
 * All of this is APK 1.1.10 code: the HUD Draw, the spark's Create, Step and user event 15 and gml_Script_instance_create
 * must match their fingerprints, and the two call sites their instructions, before anything is hooked.
 *
 * exp_hudsparksverify (debugMode): sparks are created as before, and the skip is checked against them: at each
 * creation the five random numbers are taken from a copy of the generator, and the id and layer counters and the
 * three draws of the Create are compared; a twin of each spark is stepped in the spark's own Step and compared with
 * it before and after (floats bit for bit, doubles exactly, destroyed or not); a second simulation stepped where the
 * skip steps (obj_hud's Step) is compared with the twins at the line-299 with, where the draws the iteration makes
 * must equal the simulated count, and at line 261. [HUDSPARKS] verify counts must stay 0 mismatches;
 * exp_hudsparksfault (with verify) leaves out one alpha decrement in the second simulation as a negative control.
 * exp_hudsparksab (debugMode) creates the sparks in every other report window (in-run A/B; the simulated and real
 * sparks of the other windows live on).
 *
 * The instance_create_depth hook goes into the runner's function registry, which is filled only by
 * katana_late_install (hud_sparks_late_install); the Step wrappers and the call sites are set up before.
 */
#include <vitasdk.h>
#include <kubridge.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

int hud_sparks = 0; // hudSparks (yyl.cfg): 1 keeps the game's sparks

uint32_t code_fingerprint(uintptr_t entry, uint32_t size); // fog_skip.c

#define ABI __attribute__((pcs("aapcs")))
typedef struct { double val; uint32_t flags, kind; } RV;
typedef struct stack_trace { struct stack_trace *prev; const char *name; int line; } stack_trace;
typedef int ABI (*new_fn)(void *iterator, void **self, void **other, const RV *target);
typedef void ABI (*delete_fn)(void *iterator, void **self, void **other);
typedef RV *ABI (*legacy_fn)(void *self, void *other, RV *result, int argc, int id, RV **args);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double epsilon, int throw_error);
typedef int ABI (*bool_fn)(const RV *value);

// APK 1.1.10 ELF addresses: obj_hud's Draw, its line-261 instance_exists call and line-299 with, their object
// operands (movw r0, #0x233: obj_hudparticle) and the PLT entries they call
enum { HUD_DRAW = 0x41afc38, EXISTS_OBJ = 0x41b931c, EXISTS_SITE = 0x41b936c, WITH_OBJ = 0x41bafb4, WITH_NEW = 0x41bafd8,
       WITH_DELETE = 0x41bb050, PLT_NEW = 0x4ab0e00, PLT_DELETE = 0x4ab0e40, PLT_LEGACY = 0x4ab0ff0 };
#define MOVW_R0_HUDPARTICLE 0xe3000233u

static builtin_fn orig_create_depth;
static new_fn orig_new_with;
static delete_fn orig_delete_with;
static legacy_fn orig_legacy;
static event_func orig_hud_step, orig_spark_step;
static compare_fn compare;
static bool_fn bool_value;
static double (*yy_random)(double);
static int (*instance_exists)(void *self, void *other, int object);
static void *(*layer_with_depth)(void *room, int depth, int dynamic_only);
static int (*get_int32)(const RV *args, int index);
static void (*sincosf_fn)(float, float *, float *);
static float (*atan2f_fn)(float, float), (*fmodf_fn)(float, float);
static long (*lrintf_fn)(float);
static stack_trace **trace_top;
static void **run_room;
static int *room_maxid, *element_id;
static uint32_t *rng_state, *rng_index, *id_map;
static const double *epsilon;
static int *var_alpha, *var_size, *var_pspeed;
// APK 1.1.10 object indices (the fingerprinted code and the checked movw operands name them); Object_Find confirms
// them at obj_hud's first Step, once the objects are loaded (it returns -1 while the loader installs)
enum { OBJ_HUDPARTICLE = 563, OBJ_GAMEPAUSED = 474 };
static int spark_object = OBJ_HUDPARTICLE, paused_object = OBJ_GAMEPAUSED;
static int (*object_find)(const char *);
static int objects_ok; // 0 not checked yet, 1 confirmed, -1 other objects: everything runs the original
static int verify, fault, ab, ab_off;

// ---- the spark model ----------------------------------------------------------------------------------------------

typedef struct {
	int id;
	float dir, speed, h, v; // CInstance +0xd0 direction, +0xd4 speed, +0xe4 hspeed, +0xe8 vspeed
	double alpha, size, pspeed;
	double m_dir, m_speed;  // motion_add's arguments (verify: applied before the first Step)
	int pending;
	unsigned n;             // creation number (fault injection)
} spark;

#define MAX_SPARKS 96
static spark sim[MAX_SPARKS], twin[MAX_SPARKS];
static int nsim, ntwin;
static int hud_id = -1;

static const float PI_F = 3.1415927410125732f, SNAP = 9.999999747378752e-05f;

static float snap(float x) {
	float r = (float)lrintf_fn(x);
	return fabsf(x - r) < SNAP ? r : x;
}

// The tail of CInstance::SetHspeed / SetVspeed / AddTo_Speed / Compute_Speed1: direction and speed from h, v
static void speed_from_hv(spark *s) {
	float h = s->h, v = s->v, d;
	if (h != 0.0f) {
		float a = atan2f_fn(v, h);
		a = a * 180.0f;
		a = a / PI_F;
		d = a > 0.0f ? 360.0f - a : -a;
	} else if (v > 0.0f) {
		d = 270.0f;
	} else if (v < 0.0f) {
		d = 90.0f;
	} else {
		d = s->dir;
	}
	float sq = v * v;
	sq = sq + h * h;
	float sp = sqrtf(sq);
	d = snap(d);
	sp = snap(sp);
	s->dir = fmodf_fn(d, 360.0f);
	s->speed = sp;
}

// The tail of CInstance::SetSpeed / SetDirection / Compute_Speed2: h, v from direction and speed
static void hv_from_dir(spark *s) {
	float rad = s->dir * PI_F, sn, cs;
	rad = rad / 180.0f;
	sincosf_fn(rad, &sn, &cs);
	float h = cs * s->speed, v = -(s->speed * sn);
	s->h = snap(h);
	s->v = snap(v);
}

static void set_direction(spark *s, float value) {
	float d = fmodf_fn(value, 360.0f);
	if (d < 0.0f)
		d = d + 360.0f;
	s->dir = d;
	hv_from_dir(s);
}

static void set_speed(spark *s, float value) {
	if (s->speed == value)
		return;
	s->speed = value;
	hv_from_dir(s);
}

static void set_hspeed(spark *s, float value) {
	if (s->h == value)
		return;
	s->h = value;
	speed_from_hv(s);
}

static void set_vspeed(spark *s, float value) {
	if (s->v == value)
		return;
	s->v = value;
	speed_from_hv(s);
}

static void add_to_speed(spark *s, float dir, float spd) {
	float rad = dir * PI_F, sn, cs;
	rad = rad / 180.0f;
	sincosf_fn(rad, &sn, &cs);
	float h = s->h, v = s->v;
	h = h + spd * cs;
	v = v - spd * sn;
	s->h = h;
	s->v = v;
	speed_from_hv(s);
}

static int cmp(double a, double b, int throw_error) {
	RV x = {a, 0, 0}, y = {b, 0, 0};
	return compare(&x, &y, *epsilon, throw_error);
}

// The Create (lines 1-10) with its three random numbers, then motion_add (YYGetFloat of each argument)
static void spark_create(spark *s, int id, const double r[5]) {
	static unsigned created;
	memset(s, 0, sizeof(*s));
	s->id = id;
	s->n = created++;
	set_direction(s, (float)r[0]);
	set_speed(s, (float)r[1]);
	s->size = 6.0 - r[2];
	s->alpha = 1.0;
	s->pspeed = 0.0;
	s->m_dir = r[3];
	s->m_speed = r[4];
	s->pending = 1;
}

static void spark_motion(spark *s) {
	if (s->pending) {
		add_to_speed(s, (float)s->m_dir, (float)s->m_speed);
		s->pending = 0;
	}
}

// The Step; returns 0 when the spark destroys itself
static int spark_step(spark *s, int paused, int faulty) {
	spark_motion(s);
	if (paused) {
		if (cmp((double)s->speed, 0.0, 0) != 0)
			s->pspeed = (double)s->speed;
		set_speed(s, 0.0f);
		return 1;
	}
	if (cmp((double)s->speed, 0.0, 0) == 0) {
		set_speed(s, (float)s->pspeed);
		s->pspeed = 0.0;
	}
	set_vspeed(s, (float)((double)s->v + 0.3));
	if (cmp((double)s->h, 0.0, 1) >= 1)
		set_hspeed(s, (float)((double)s->h - 0.1));
	int c = cmp((double)s->h, 0.0, 1);
	if (c != -2 && c <= -1)
		set_hspeed(s, (float)((double)s->h + 0.1));
	set_speed(s, (float)((double)s->speed - 0.1));
	if (!faulty)
		s->alpha = s->alpha - 0.04;
	c = cmp(s->size, 0.0, 1);
	if (c != -2 && c < 1)
		return 0;
	c = cmp(s->alpha, 0.0, 1);
	if (c != -2 && c < 1)
		return 0;
	c = cmp((double)s->speed, 0.0, 1);
	return c == -2 || c > 0;
}

static void step_list(spark *list, int *count, int paused, int with_fault) {
	int k = 0;
	for (int i = 0; i < *count; i++)
		if (spark_step(&list[i], paused, with_fault && list[i].n % 7 == 3))
			list[k++] = list[i];
	*count = k;
}

// ---- runner state -------------------------------------------------------------------------------------------------

static void *instance_by_id(int id) {
	if (!id_map || !id_map[0])
		return NULL;
	const uint32_t *e = (const uint32_t *)(uintptr_t)((const uint32_t *)(uintptr_t)id_map[0])[(id_map[1] & (uint32_t)id) * 2];
	for (; e; e = (const uint32_t *)(uintptr_t)e[1])
		if ((int)e[2] == id)
			return (void *)(uintptr_t)e[3];
	return NULL;
}

static int at_hud_site(void) {
	stack_trace *t = *trace_top;
	return t && t->name && !strcmp(t->name, "gml_Script_instance_create") && (t = t->prev) && t->name &&
		!strcmp(t->name, "gml_Object_obj_hud_Draw_0");
}

static int object_arg(const RV *v) {
	if ((v->kind & 0xffffff) == 15) {
		uint32_t w[2];
		memcpy(w, &v->val, sizeof(w));
		return (int)w[0];
	}
	return (v->kind & 0xffffff) == 0 ? (int)v->val : -1;
}

// WELL512 as fYYRandom advances it (state[16], g_RndIndex)
typedef struct { uint32_t s[16], i; } rng_t;

static void rng_read(rng_t *r) {
	memcpy(r->s, rng_state, sizeof(r->s));
	r->i = *rng_index;
}

static void rng_write(const rng_t *r) {
	memcpy(rng_state, r->s, sizeof(r->s));
	*rng_index = r->i;
}

static void rng_step(rng_t *r) {
	uint32_t i = r->i & 15, a = r->s[i], c = r->s[(i + 13) & 15], b = r->s[(i + 9) & 15];
	uint32_t z1 = a ^ (a << 16) ^ c ^ (c << 15), z2 = b ^ (b >> 11), n = z1 ^ z2;
	r->s[i] = n;
	uint32_t j = (i - 1) & 15, d = r->s[j];
	r->s[j] = d ^ (d << 2) ^ (z1 << 18) ^ (z2 << 28) ^ (0xda442d20u & (n << 5)) ^ z2;
	r->i = j;
}

// Draws between a snapshot and now, -1 if none up to max
static int rng_draws(const rng_t *from, int max) {
	rng_t r = *from, now;
	rng_read(&now);
	for (int k = 0; k <= max; k++) {
		if (r.i == now.i && !memcmp(r.s, now.s, sizeof(r.s)))
			return k;
		rng_step(&r);
	}
	return -1;
}

static int read_bool_slot(void *self, int *var, double *out) {
	gml_rvalue *v = var ? gml_vget(self, var[1]) : NULL;
	if (!v || (v->kind & 0xffffff) != 0)
		return 0;
	*out = v->val;
	return 1;
}

// ---- report counters ----------------------------------------------------------------------------------------------

static unsigned skipped, fallbacks, with_draws, sim_max, sim_sum, frames_sim;
static unsigned v_creates, v_create_bad, v_twin_steps, v_twin_bad, v_shadow_checks, v_shadow_bad, v_with, v_with_bad,
	v_exists, v_exists_bad, v_lost;
static int v_logged;

static void v_log(const char *fmt, ...) {
	if (v_logged >= 12)
		return;
	v_logged++;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	debugPrintf("[HUDSPARKS] %s\n", buf);
}

static int skipping(void) {
	return !verify && !(ab && ab_off);
}

// ---- hooks ----------------------------------------------------------------------------------------------------------

static int real_matches(void *inst, const spark *s) {
	const uint8_t *p = inst;
	double alpha = NAN, size = NAN, pspeed = NAN;
	read_bool_slot(inst, var_alpha, &alpha);
	read_bool_slot(inst, var_size, &size);
	read_bool_slot(inst, var_pspeed, &pspeed);
	return !memcmp(p + 0xd0, &s->dir, 4) && !memcmp(p + 0xd4, &s->speed, 4) && !memcmp(p + 0xe4, &s->h, 4) &&
		!memcmp(p + 0xe8, &s->v, 4) && !memcmp(&alpha, &s->alpha, 8) && !memcmp(&size, &s->size, 8) &&
		!memcmp(&pspeed, &s->pspeed, 8);
}

static spark *find(spark *list, int count, int id) {
	for (int i = 0; i < count; i++)
		if (list[i].id == id)
			return &list[i];
	return NULL;
}

static void create_depth_hook(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (objects_ok != 1 || argc != 4 || object_arg((RV *)&args[3]) != spark_object || !at_hud_site() || !*run_room) {
		orig_create_depth(result, self, other, argc, args);
		return;
	}
	if (!layer_with_depth(*run_room, get_int32((RV *)args, 2), 1) || nsim >= MAX_SPARKS || ntwin >= MAX_SPARKS) {
		fallbacks++;
		orig_create_depth(result, self, other, argc, args);
		return;
	}
	// The five random numbers this creation and its with block draw, in order
	rng_t before, after3;
	double r[5];
	rng_read(&before);
	for (int k = 0; k < 5; k++) {
		r[k] = yy_random(k == 0 || k == 3 ? 360.0 : k == 1 ? 5.0 : k == 2 ? 2.0 : 10.0);
		if (k == 2)
			rng_read(&after3);
	}
	int id = *room_maxid + 1;
	if (skipping()) {
		*room_maxid = id;
		++*element_id;
		spark_create(&sim[nsim++], id, r);
		uint32_t w[2] = {(uint32_t)id, 0x04000001}; // the reference F_InstanceCreateDepth returns
		memcpy(&result->val, w, sizeof(w));
		result->flags = 0;
		result->kind = 15;
		skipped++;
		return;
	}
	rng_write(&before);
	int element = *element_id + 1;
	orig_create_depth(result, self, other, argc, args);
	if (!verify)
		return;
	v_creates++;
	rng_t now;
	rng_read(&now);
	uint32_t w[2];
	memcpy(w, &result->val, sizeof(w));
	if ((result->kind & 0xffffff) != 15 || (int)w[0] != id || w[1] != 0x04000001 || *room_maxid != id ||
			*element_id != element || now.i != after3.i || memcmp(now.s, after3.s, sizeof(now.s))) {
		v_create_bad++;
		v_log("creation mismatch: id %d/%d (%08x, kind %u), maxid %d, element %d/%d, rng %s", (int)w[0], id, w[1],
			result->kind, *room_maxid, *element_id, element, now.i == after3.i && !memcmp(now.s, after3.s, 64) ? "same" : "differs");
	}
	spark_create(&twin[ntwin++], id, r);
	spark_create(&sim[nsim++], id, r);
}

// obj_hudparticle Step (verify): the twin steps with the real spark
static void spark_step_hook(void *self, void *other) {
	spark *t = verify && objects_ok == 1 ? find(twin, ntwin, *(int *)((uint8_t *)self + 0x80)) : NULL;
	if (!t) {
		orig_spark_step(self, other);
		return;
	}
	spark_motion(t);
	if (!real_matches(self, t)) {
		v_twin_bad++;
		v_log("spark %d before its Step: dir %a/%a speed %a/%a h %a/%a v %a/%a", t->id, *(float *)((uint8_t *)self + 0xd0),
			t->dir, *(float *)((uint8_t *)self + 0xd4), t->speed, *(float *)((uint8_t *)self + 0xe4), t->h,
			*(float *)((uint8_t *)self + 0xe8), t->v);
	}
	int alive = spark_step(t, instance_exists(self, other, paused_object), 0);
	orig_spark_step(self, other);
	v_twin_steps++;
	int marked = *(uint32_t *)((uint8_t *)self + 0x7c) & 1;
	if (marked == alive || (alive && !real_matches(self, t))) {
		v_twin_bad++;
		v_log("spark %d after its Step: %s/%s, speed %a/%a alpha %a/%a", t->id, marked ? "destroyed" : "alive",
			alive ? "alive" : "destroyed", *(float *)((uint8_t *)self + 0xd4), t->speed, NAN, t->alpha);
	}
	if (!alive)
		*t = twin[--ntwin];
}

// obj_hud Step: the simulated sparks step where the real ones would
static void hud_step_hook(void *self, void *other) {
	if (!objects_ok) {
		int s = object_find("obj_hudparticle"), p = object_find("obj_gamepaused");
		objects_ok = s == OBJ_HUDPARTICLE && p == OBJ_GAMEPAUSED ? 1 : -1;
		debugPrintf("hud sparks: %s (obj_hudparticle %d, obj_gamepaused %d)\n", objects_ok == 1 ? "objects confirmed" :
			"NOT active, other objects", s, p);
	}
	if (objects_ok != 1) {
		orig_hud_step(self, other);
		return;
	}
	int id = *(int *)((uint8_t *)self + 0x80);
	if (id != hud_id) {
		hud_id = id;
		nsim = 0;
	}
	if (nsim)
		step_list(sim, &nsim, instance_exists(self, other, paused_object), verify && fault);
	orig_hud_step(self, other);
}

// Twins whose spark is gone without a Step that destroyed it (room end, restart)
static void drop_lost_twins(void) {
	for (int i = 0; i < ntwin;) {
		void *inst = instance_by_id(twin[i].id);
		if (!inst || (*(uint32_t *)((uint8_t *)inst + 0x7c) & 1)) {
			v_lost++;
			twin[i] = twin[--ntwin];
		} else {
			i++;
		}
	}
}

static int shadow_matches_twins(void) {
	if (nsim != ntwin)
		return 0;
	for (int i = 0; i < nsim; i++) {
		spark a = sim[i], *b = find(twin, ntwin, a.id), bb;
		if (!b)
			return 0;
		bb = *b;
		spark_motion(&a);
		spark_motion(&bb);
		if (memcmp(&a.dir, &bb.dir, 16) || memcmp(&a.alpha, &bb.alpha, 24))
			return 0;
	}
	return 1;
}

// Line 261: if (... || instance_exists(obj_hudparticle))
RV *ABI __attribute__((used)) hud_exists_site(void *self, void *other, RV *result, int argc, int id, RV **args) {
	RV *r = orig_legacy(self, other, result, argc, id, args);
	if (objects_ok == 1 && argc == 1 && object_arg(args[0]) == spark_object) {
		if (verify) {
			drop_lost_twins();
			v_exists++;
			if (bool_value(result) != (nsim > 0)) {
				v_exists_bad++;
				v_log("line 261: instance_exists %d, simulated %d", bool_value(result), nsim);
			}
		} else if (nsim > 0 && !bool_value(result)) {
			result->val = 1.0;
			result->flags = 0;
			result->kind = 0;
		}
	}
	return r;
}

// Line 299: with (obj_hudparticle) event_user(15). One draw per spark (user event 15, line 0).
static rng_t with_rng;
static int with_open;

int ABI __attribute__((used)) hud_with_new_site(void *iterator, void **self, void **other, const RV *target) {
	if (objects_ok == 1 && object_arg(target) == spark_object) {
		if (verify) {
			drop_lost_twins();
			v_shadow_checks++;
			if (!shadow_matches_twins()) {
				v_shadow_bad++;
				v_log("line 299: simulated %d, twins %d (or their state differs)", nsim, ntwin);
			}
			rng_read(&with_rng);
			with_open = 1;
		} else {
			for (int i = 0; i < nsim; i++)
				yy_random(1.0);
			with_draws += nsim;
		}
		if (nsim > sim_max)
			sim_max = nsim;
		sim_sum += nsim;
		frames_sim++;
	}
	return orig_new_with(iterator, self, other, target);
}

void ABI __attribute__((used)) hud_with_delete_site(void *iterator, void **self, void **other) {
	orig_delete_with(iterator, self, other);
	if (verify && with_open) {
		with_open = 0;
		int draws = rng_draws(&with_rng, 256);
		v_with++;
		if (draws != nsim) {
			v_with_bad++;
			v_log("line 299: the iteration drew %d, simulated sparks %d", draws, nsim);
		}
	}
}

void hud_sparks_report(int frames) {
	if (!orig_create_depth)
		return;
	if (ab) {
		debugPrintf("[HUDSPARKS] A/B window: sparks %s\n", ab_off ? "created (game)" : "simulated (skip)");
		ab_off = !ab_off;
	}
	if (!skipped && !fallbacks && !v_creates && !frames_sim)
		return;
	debugPrintf("[HUDSPARKS] %u sparks not created, %u created (no layer or list full), %u draws at the HUD's with; "
		"simulated alive %.1f (max %u) over %u HUD frames\n", skipped, fallbacks, with_draws,
		frames_sim ? (float)sim_sum / frames_sim : 0.0f, sim_max, frames_sim);
	if (verify)
		debugPrintf("[HUDSPARKS] verify: creations %u (%u mismatch), twin steps %u (%u mismatch), HUD with %u "
			"(simulation %u mismatch), its draws %u (%u mismatch), line 261 %u (%u mismatch), sparks gone without a "
			"Step %u%s\n", v_creates, v_create_bad, v_twin_steps, v_twin_bad, v_shadow_checks, v_shadow_bad, v_with,
			v_with_bad, v_exists, v_exists_bad, v_lost, fault ? " (fault: every 7th simulated spark keeps its alpha)" : "");
	skipped = fallbacks = with_draws = sim_max = sim_sum = frames_sim = 0;
	(void)frames;
}

// ---- install --------------------------------------------------------------------------------------------------------

static int bl_reaches(uintptr_t site, uintptr_t target) {
	int64_t delta = (int64_t)target - (int64_t)site - 8;
	return !(delta & 3) && delta >= -(1 << 25) && delta < (1 << 25);
}

static int check_call(uintptr_t site, uintptr_t plt, uintptr_t direct) {
	uint32_t ins = *(uint32_t *)site;
	uintptr_t target = site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
	return (ins >> 24) == 0xeb && (target == plt || target == direct);
}

static void patch_call(uintptr_t site, void *wrapper, uint32_t *veneer) {
	int32_t off = (int32_t)((uintptr_t)veneer - (site + 8));
	uint32_t code[2] = {0xe51ff004, (uint32_t)wrapper}; // ldr pc, [pc, #-4]; .word wrapper
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
}

static int code_is_1110(uintptr_t hud) {
	static const struct { const char *symbol; uint32_t size, fingerprint; } code[] = {
		{"_Z35gml_Object_obj_hudparticle_Create_0P9CInstanceS0_", 0x7fc, 0xaadeb60d},  // three random draws
		{"_Z33gml_Object_obj_hudparticle_Step_0P9CInstanceS0_", 0xdfc, 0x5788a451},    // none
		{"_Z35gml_Object_obj_hudparticle_Other_25P9CInstanceS0_", 0x1238, 0x7dc31c31}, // one, at line 0
		{"_Z26gml_Script_instance_createP9CInstanceS0_R8YYRValueiPPS1_", 0x59c, 0x049bde56},
	};
	for (unsigned i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
		uint32_t f = code_fingerprint(so_symbol(&yoyoloader_mod, code[i].symbol), code[i].size);
		if (f != code[i].fingerprint) {
			debugPrintf("hud sparks: %s fingerprint %08x, expected %08x\n", code[i].symbol, f, code[i].fingerprint);
			return 0;
		}
	}
	// The HUD Draw is 0x113cc bytes, fingerprinted in two parts (code_fingerprint takes up to 64 KB)
	uint32_t f1 = code_fingerprint(hud, 0x10000), f2 = code_fingerprint(hud + 0x10000, 0x13cc);
	if (f1 != 0x8819756e || f2 != 0x150db4bf) {
		debugPrintf("hud sparks: obj_hud Draw fingerprint %08x %08x\n", f1, f2);
		return 0;
	}
	return 1;
}

enum { NSITES = 3 };
static uintptr_t sites[NSITES];

// The three call sites share one veneer: the code cave past the runner's .text (288 bytes) is the only space within
// bl range of them, and other modules need it too. The return address tells the sites apart; r12 and the flags are
// free at a call (the PLT stubs these calls went to use r12 too), so arguments and stack stay as the site left them.
uintptr_t hud_ret_exists, hud_ret_with_new;
void ABI __attribute__((naked, target("arm"))) hud_site_dispatch(void) {
	__asm__ volatile(
		"ldr r12, =hud_ret_exists\n"
		"ldr r12, [r12]\n"
		"cmp lr, r12\n"
		"ldreq r12, =hud_exists_site\n"
		"bxeq r12\n"
		"ldr r12, =hud_ret_with_new\n"
		"ldr r12, [r12]\n"
		"cmp lr, r12\n"
		"ldreq r12, =hud_with_new_site\n"
		"bxeq r12\n"
		"ldr r12, =hud_with_delete_site\n"
		"bx r12\n"
		".ltorg");
}
static runner_function **functions;
static int *function_count, late_pending;

void hud_sparks_install(gml_func_entry *table) {
	if (hud_sparks) {
		debugPrintf("hud sparks: the game's (hudSparks=1)\n");
		return;
	}
#define HS_RESOLVE(var, sym) var = (void *)so_symbol(&yoyoloader_mod, sym)
	uintptr_t hud = so_symbol(&yoyoloader_mod, "_Z25gml_Object_obj_hud_Draw_0P9CInstanceS0_");
	HS_RESOLVE(trace_top, "_ZN13SYYStackTrace8s_pStartE");
	HS_RESOLVE(run_room, "Run_Room");
	HS_RESOLVE(room_maxid, "room_maxid");
	HS_RESOLVE(element_id, "_ZN13CLayerManager18m_CurrentElementIDE");
	HS_RESOLVE(id_map, "_ZN9CInstance14ms_ID2InstanceE");
	HS_RESOLVE(rng_state, "state");
	HS_RESOLVE(rng_index, "g_RndIndex");
	HS_RESOLVE(layer_with_depth, "_ZN13CLayerManager17GetLayerWithDepthEP5CRoomib");
	HS_RESOLVE(get_int32, "_Z10YYGetInt32PK6RValuei");
	HS_RESOLVE(yy_random, "_Z12YYGML_randomd");
	HS_RESOLVE(compare, "_Z12YYCompareValRK6RValueS1_db");
	HS_RESOLVE(bool_value, "_Z11BOOL_RValuePK6RValue");
	HS_RESOLVE(epsilon, "g_GMLMathEpsilon");
	HS_RESOLVE(instance_exists, "_Z21YYGML_instance_existsP9CInstanceS0_i");
	HS_RESOLVE(orig_new_with, "_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
	HS_RESOLVE(orig_delete_with, "_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_");
	HS_RESOLVE(orig_legacy, "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	HS_RESOLVE(var_alpha, "g_VAR_alpha");
	HS_RESOLVE(var_size, "g_VAR_size");
	HS_RESOLVE(var_pspeed, "g_VAR_pspeed");
	HS_RESOLVE(object_find, "_Z11Object_FindPKc");
	HS_RESOLVE(functions, "the_functions");
	HS_RESOLVE(function_count, "the_numb");
#undef HS_RESOLVE
	// The libm functions the runner's CInstance setters call (its GOT slots)
	uintptr_t *s_sincos = so_got_slot(&yoyoloader_mod, "sincosf"), *s_atan2 = so_got_slot(&yoyoloader_mod, "atan2f"),
		*s_fmod = so_got_slot(&yoyoloader_mod, "fmodf"), *s_lrint = so_got_slot(&yoyoloader_mod, "lrintf");
	if (!hud || !trace_top || !run_room || !room_maxid || !element_id || !id_map || !rng_state || !rng_index ||
			!layer_with_depth || !get_int32 || !yy_random || !compare || !bool_value || !epsilon || !instance_exists ||
			!orig_new_with || !orig_delete_with || !orig_legacy || !var_alpha || !var_size || !var_pspeed || !object_find ||
			!functions || !function_count || !s_sincos || !s_atan2 || !s_fmod || !s_lrint || !table) {
		debugPrintf("hud sparks: NOT installed (symbols)\n");
		return;
	}
	sincosf_fn = (void *)*s_sincos;
	atan2f_fn = (void *)*s_atan2;
	fmodf_fn = (void *)*s_fmod;
	lrintf_fn = (void *)*s_lrint;
	if (!code_is_1110(hud)) {
		debugPrintf("hud sparks: NOT installed (not the APK 1.1.10 code)\n");
		return;
	}
#define AT(a) (hud + (a) - HUD_DRAW)
	if (*(uint32_t *)AT(EXISTS_OBJ) != MOVW_R0_HUDPARTICLE || *(uint32_t *)AT(WITH_OBJ) != MOVW_R0_HUDPARTICLE ||
			!check_call(AT(EXISTS_SITE), AT(PLT_LEGACY), (uintptr_t)orig_legacy) ||
			!check_call(AT(WITH_NEW), AT(PLT_NEW), (uintptr_t)orig_new_with) ||
			!check_call(AT(WITH_DELETE), AT(PLT_DELETE), (uintptr_t)orig_delete_with)) {
		debugPrintf("hud sparks: NOT installed (call sites)\n");
		return;
	}
	sites[0] = AT(EXISTS_SITE);
	sites[1] = AT(WITH_NEW);
	sites[2] = AT(WITH_DELETE);
#undef AT
	verify = debugMode && exp_flag("hudsparksverify");
	fault = verify && exp_flag("hudsparksfault");
	ab = debugMode && !verify && exp_flag("hudsparksab");
	if (!(orig_hud_step = gml_replace(table, "gml_Object_obj_hud_Step_0", hud_step_hook)) ||
			(verify && !(orig_spark_step = gml_replace(table, "gml_Object_obj_hudparticle_Step_0", spark_step_hook)))) {
		debugPrintf("hud sparks: NOT installed (Step events)\n");
		return;
	}
	late_pending = 1;
}

// katana_late_install, after rain_skip_install: the runner's function registry is filled by then. The veneers are
// allocated here too, after fog_skip.c has taken the code cave space its call site needs. Nothing is patched or
// hooked unless all of it can be: skipped sparks need the HUD's call sites.
void hud_sparks_late_install(void) {
	if (!late_pending)
		return;
	late_pending = 0;
	uint32_t *block = (uint32_t *)so_alloc_near(&yoyoloader_mod, sites[0], 8);
	for (int k = 0; block && k < NSITES; k++)
		if (!bl_reaches(sites[k], (uintptr_t)block))
			block = NULL;
	if (!block) {
		debugPrintf("hud sparks: NOT installed (no veneer in range of the HUD's call sites)\n");
		return;
	}
	for (int i = 0; *functions && i < *function_count; i++) {
		runner_function *f = &(*functions)[i];
		if (f->name && !strcmp(f->name, "instance_create_depth")) {
			orig_create_depth = builtin_chain((void **)&f->func, create_depth_hook);
			// (chained: see hooks.c)
			break;
		}
	}
	if (!orig_create_depth) {
		debugPrintf("hud sparks: NOT installed (instance_create_depth)\n");
		return;
	}
	hud_ret_exists = sites[0] + 4;
	hud_ret_with_new = sites[1] + 4;
	patch_call(sites[0], hud_site_dispatch, block); // writes the veneer
	for (int k = 1; k < NSITES; k++)
		patch_call(sites[k], hud_site_dispatch, block);
	debugPrintf("hud sparks: installed%s%s%s\n", verify ? " (verify mode)" : "", fault ? " (with faults)" : "",
		ab ? " (A/B per report window)" : "");
}
