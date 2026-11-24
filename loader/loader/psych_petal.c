/*
 * psych_petal.c
 *
 * Native Draw for obj_rosepetal, the rose petals the giant scatters in the last part of the secret Psychiatrist fight
 * (~105 at once). The Draw event (gml_Object_obj_rosepetal_Draw_0, APK 1.1.10) is:
 *
 *   if (dead) draw_sprite_ext(sprite_index, animation_index, x, y + 1, 2 * size, 2 * size, 0, col, 1);
 *   else {
 *       draw_sprite_ext(sprite_index, animation_index, x, y - (1 - spd) * 30, 2 * size, 2 * size, 0, col, 1);
 *       if (!collision)
 *           draw_sprite_ext(sprite_index, animation_index, x, y - (1 - spd) * 30, 2 * size, 2 * size, 0, c_black,
 *               1 - (size - 1) - 0.3);
 *   }
 *
 * compiled to direct YYGML_draw_sprite_ext calls: sprite (int)(int64)sprite_index, image (float)animation_index,
 * (float) of the double expressions above in that order of operations, colour INT64_RValue(col). The native reads the
 * same variables through the same getters and makes the same calls with the same arguments; it writes nothing, as the
 * event. Values that aren't reals run the original event. exp_psychpetal0 keeps the original; exp_psychpetalverify
 * (debug) runs the native with its calls recorded instead of submitted, then the original with its
 * YYGML_draw_sprite_ext calls recorded (an entry hook that is only active then), and compares them bit for bit
 * ([PSYCHPETAL] line); exp_psychpetalfault is a negative control (alpha + 0.001 on the shadow draw).
 */
#include <vitasdk.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

#define ABI __attribute__((pcs("aapcs")))
#define KIND(v) ((v)->kind & 0xffffff)
typedef void ABI (*draw_ext_fn)(void *self, int sprite, float image, float x, float y, float xs, float ys, float rot,
	int col, float alpha);
typedef int ABI (*bool_fn)(const gml_rvalue *v);
typedef int64_t ABI (*int64_fn)(const gml_rvalue *v);
typedef double ABI (*real_fn)(const gml_rvalue *v);
typedef void ABI (*getvalue_fn)(const gml_rvalue *target, int var, int index, gml_rvalue *out, int a, int b);

static event_func orig_petal_draw;
static draw_ext_fn draw_ext, orig_draw_ext;
static bool_fn bool_rvalue;
static int64_fn int64_rvalue;
static real_fn real_rvalue_ex;
static unsigned reject[4];

// As the compiled event reads a value it passes on: the double if it is a real, else REAL_RValue_Ex
static inline double as_real(const gml_rvalue *v) {
	return KIND(v) == 0 ? v->val : real_rvalue_ex(v);
}
static getvalue_fn get_value;
static int *v_dead, *v_collision, *v_animation_index, *v_spd, *v_size, *v_col, *v_sprite_index, *v_x, *v_y;
static int verify, fault, ab_skip, native_on;
static unsigned n_native, n_fallback, n_verified, n_mismatch, n_draws;


typedef struct { void *self; int sprite; float f[6]; int col; float alpha; } draw_call;
typedef struct { draw_call c[4]; int n; } call_list;
static call_list *cur;
static int submit = 1;

static void ABI record_draw(void *self, int sprite, float image, float x, float y, float xs, float ys, float rot,
		int col, float alpha) {
	if (cur && cur->n < 4) {
		draw_call *d = &cur->c[cur->n++];
		d->self = self;
		d->sprite = sprite;
		d->f[0] = image; d->f[1] = x; d->f[2] = y; d->f[3] = xs; d->f[4] = ys; d->f[5] = rot;
		d->col = col;
		d->alpha = alpha;
	} else if (cur)
		cur->n++;
}

// Entry hook on YYGML_draw_sprite_ext (verify mode only): records while a list is open, then draws
static void ABI draw_ext_hook(void *self, int sprite, float image, float x, float y, float xs, float ys, float rot,
		int col, float alpha) {
	record_draw(self, sprite, image, x, y, xs, ys, rot, col, alpha);
	orig_draw_ext(self, sprite, image, x, y, xs, ys, rot, col, alpha);
}

static inline void submit_draw(void *self, int sprite, float image, float x, float y, float xs, float ys, float rot,
		int col, float alpha) {
	if (cur)
		record_draw(self, sprite, image, x, y, xs, ys, rot, col, alpha);
	if (submit)
		draw_ext(self, sprite, image, x, y, xs, ys, rot, col, alpha);
}

static int native_draw(void *self) {
	gml_rvalue *dead = gml_vget(self, v_dead[1]);
	gml_rvalue *anim = gml_vget(self, v_animation_index[1]);
	gml_rvalue *size = gml_vget(self, v_size[1]);
	gml_rvalue *col = gml_vget(self, v_col[1]);
	// size and spd go through YYRValue operators: reals only (anything else runs the original)
	if (!dead || !anim || !size || !col || KIND(size) != 0) {
		reject[0]++;
		return 0;
	}
	int is_dead = bool_rvalue(dead);
	gml_rvalue *spd = NULL, *collision = NULL;
	if (!is_dead) {
		spd = gml_vget(self, v_spd[1]);
		collision = gml_vget(self, v_collision[1]);
		if (!spd || !collision || KIND(spd) != 0) {
			reject[1]++;
			return 0;
		}
	}
	gml_rvalue self_rv = {0, 0, 6}, spr = {0, 0, 5}, gx = {0, 0, 5}, gy = {0, 0, 5};
	memcpy(&self_rv.val, &self, sizeof(self));
	get_value(&self_rv, v_sprite_index[1], (int)0x80000000, &spr, 0, 0);
	get_value(&self_rv, v_x[1], (int)0x80000000, &gx, 0, 0);
	get_value(&self_rv, v_y[1], (int)0x80000000, &gy, 0, 0);
	if (KIND(&gy) != 0 || ((1u << (spr.kind & 31)) & 0x46) || ((1u << (gx.kind & 31)) & 0x46)) {
		reject[2]++; // y goes through YYRValue operators
		return 0;
	}
	int sprite = (int)(int64_t)as_real(&spr);
	float image = (float)as_real(anim), x = (float)as_real(&gx), scale = (float)(2.0 * size->val);
	int colour = (int)int64_rvalue(col);
	if (is_dead) {
		submit_draw(self, sprite, image, x, (float)(gy.val + 1.0), scale, scale, 0.0f, colour, 1.0f);
		return 1;
	}
	float y = (float)(gy.val - (1.0 - spd->val) * 30.0);
	submit_draw(self, sprite, image, x, y, scale, scale, 0.0f, colour, 1.0f);
	// The original reads collision after the first draw (which writes nothing)
	if (!bool_rvalue(collision)) {
		float alpha = (float)((1.0 - (size->val - 1.0)) - 0.3);
		if (fault)
			alpha += 0.001f;
		submit_draw(self, sprite, image, x, y, scale, scale, 0.0f, 0, alpha);
	}
	return 1;
}

static int same_call(const draw_call *a, const draw_call *b) {
	return a->self == b->self && a->sprite == b->sprite && !memcmp(a->f, b->f, sizeof(a->f)) && a->col == b->col &&
		!memcmp(&a->alpha, &b->alpha, sizeof(float));
}

// psychPetals=N in yyl.cfg (default 2, 1..4): only one petal in N is drawn (a visible cut, compromises.md). Every
// petal still exists and runs its Step: the Step draws random numbers (random(3) when a petal turns), so the fight's
// random sequence stays as it was; petals only fall and settle on blocks, nothing reads them to hurt or stop the
// player. The petal is chosen from its instance address, so it stays drawn or hidden for its whole life.
int psych_petals = 2;
extern int psych_cut_off; // exp_psychcutab: the cuts are off in this report window (psych_fix.c)
static unsigned n_hidden;

static inline int petal_hidden(void *self) {
	if (psych_petals <= 1 || psych_cut_off)
		return 0;
	uintptr_t a = (uintptr_t)self;
	return ((a >> 4) ^ (a >> 7) ^ (a >> 11) ^ (a >> 15)) % (unsigned)psych_petals != 0;
}

static void petal_draw(void *self, void *other) {
	if (petal_hidden(self)) {
		n_hidden++;
		return;
	}
	if (ab_skip || !native_on) {
		orig_petal_draw(self, other);
		return;
	}
	if (!verify) {
		if (native_draw(self))
			n_native++;
		else {
			n_fallback++;
			orig_petal_draw(self, other);
		}
		return;
	}
	call_list a = {0}, b = {0};
	cur = &a;
	submit = 0;
	int ok = native_draw(self);
	submit = 1;
	cur = ok ? &b : NULL;
	orig_petal_draw(self, other);
	cur = NULL;
	if (!ok) {
		n_fallback++;
		return;
	}
	n_native++;
	n_verified++;
	n_draws += a.n;
	int bad = a.n != b.n || a.n > 4;
	for (int k = 0; !bad && k < a.n; k++)
		bad = !same_call(&a.c[k], &b.c[k]);
	if (bad) {
		if (n_mismatch < 10)
			debugPrintf("[PSYCHPETAL] verify: native %d draws, original %d; first alpha %f / %f\n", a.n, b.n,
				a.n ? a.c[a.n - 1].alpha : 0.0f, b.n ? b.c[b.n - 1].alpha : 0.0f);
		n_mismatch++;
	}
}

void psych_petal_report(int frames, int ab_off) {
	ab_skip = ab_off;
	if (!debugMode || !(n_native + n_fallback + n_hidden))
		return;
	debugPrintf("[PSYCHPETAL] native %.1f, original %.1f, hidden %.1f per frame (rejects %u %u %u)", (double)n_native / frames,
		(double)n_fallback / frames, (double)n_hidden / frames, reject[0], reject[1], reject[2]);
	if (verify)
		debugPrintf(" | verify: %u Draws (%u draw calls) compared, %u mismatches (total)%s\n", n_verified, n_draws,
			n_mismatch, fault ? " FAULT INJECTED" : "");
	else
		debugPrintf("\n");
	n_native = n_fallback = n_hidden = 0;
}

void *entry_hook(uintptr_t entry, void *replacement);

// native: the native Draw (else the original draws the petals that are shown)
int psych_petal_install(gml_func_entry *table, int native) {
	if (psych_petals < 1)
		psych_petals = 1;
	if (psych_petals > 4)
		psych_petals = 4;
	native_on = native;
#define SYM(n) so_symbol(&yoyoloader_mod, n)
	draw_ext = (draw_ext_fn)SYM("_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
	bool_rvalue = (bool_fn)SYM("_Z11BOOL_RValuePK6RValue");
	int64_rvalue = (int64_fn)SYM("_Z12INT64_RValuePK6RValue");
	real_rvalue_ex = (real_fn)SYM("_Z14REAL_RValue_ExPK6RValue");
	get_value = (getvalue_fn)SYM("_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	struct { int **v; const char *n; } vars[] = {
		{&v_dead, "g_VAR_dead"}, {&v_collision, "g_VAR_collision"}, {&v_animation_index, "g_VAR_animation_index"},
		{&v_spd, "g_VAR_spd"}, {&v_size, "g_VAR_size"}, {&v_col, "g_VAR_col"}, {&v_sprite_index, "g_VAR_sprite_index"},
		{&v_x, "g_VAR_x"}, {&v_y, "g_VAR_y"},
	};
	for (unsigned k = 0; k < sizeof(vars) / sizeof(vars[0]); k++)
		if (!(*vars[k].v = (int *)SYM(vars[k].n)))
			return 0;
	if (!draw_ext || !bool_rvalue || !int64_rvalue || !get_value || !real_rvalue_ex)
		native_on = native = 0;
	verify = native && debugMode && exp_flag("psychpetalverify");
	if (verify) {
		fault = exp_flag("psychpetalfault");
		orig_draw_ext = (draw_ext_fn)entry_hook((uintptr_t)draw_ext, (void *)draw_ext_hook);
		if (!orig_draw_ext) {
			debugPrintf("[PSYCHPETAL] verify: draw hook NOT installed\n");
			verify = 0;
		} else
			draw_ext = orig_draw_ext; // the native's own draws bypass the recorder (they record themselves)
	}
	event_func o = gml_replace(table, "gml_Object_obj_rosepetal_Draw_0", petal_draw);
	if (!o)
		return 0;
	orig_petal_draw = o;
	return 1;
}
