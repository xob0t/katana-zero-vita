/*
 * held_native.c: exact native gml_Script_controller_check (held input), APK 1.1.10 runner, ARM AAPCS.
 *
 * The script runs 2-4 times per frame in every gameplay room (player_input, player_idle_ground) and cost
 * ~145 us per call: for each binding it scans the on-screen keyboard's key list through generic variable getters
 * (array length, element, assignedKey, isHeld per key). Lift: work/decomp/out/controller_check.gml (untracked).
 *
 * The native reads the same state once per call (global.mode, global.controller_threshold, the controller's
 * control_override / control / controller_slot, the keyboard's vKeyList with each key's isHeld and assignedKey,
 * the joystick's posValueX/Y) and then runs the original decision order with:
 *  - the original builtins through YYGML_CallLegacyFunction (instance_exists, mouse_check_button,
 *    gamepad_axis_value, gamepad_button_check, gamepad_button_value, angle_difference), in the original order,
 *    with the original arguments and stack-trace lines; YYGML_keyboard_check and YYGML_point_direction directly,
 *    like the original;
 *  - YYCompareVal semantics for every comparison (inlined only for two finite reals: d = a - b, 0 if |d| <= eps),
 *    the original flag (equality 0, ordering 1) and its -2 handling; `abs(angle_difference(..)) < 120` as the
 *    original's double arithmetic abs + -120 < -eps;
 *  - the vKeyList array reads that can assign the array's copy-on-write owner, at their original points: the
 *    element read of vKeyList[0] for key 32776, and per scanned binding one whole-array read (freed again) and
 *    one element read.
 * The result is written like the original (kind 13, 1.0 or 0.0, flags kept).
 *
 * Anything unusual runs the original script from the start (nothing written yet): global.mode == 1 (the
 * player-local controller), control_override set, missing controller / keyboard / needed joystick, value kinds
 * other than real / int32 / bool, list or binding counts outside 1..64 / 0..64, pending instance changes, a
 * result slot that overlaps an input, a managed result value.
 *
 * Differential test against the original ARM body: work/held/held_verify.py (untracked).
 * exp_heldnative0 keeps the original. exp_heldverify runs the original after every native call and counts
 * results that differ ([HELD] report line). exp_heldab (measurement) switches the native off and on every report
 * window.
 */
#include <string.h>
#include "main.h"
#include <stdint.h>
#include <math.h>
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#define ABI __attribute__((pcs("aapcs")))
typedef struct CInstance CInstance;
typedef struct __attribute__((packed, aligned(4))) {
	union { double d; uint64_t bits; uint32_t w[2]; } v;
	uint32_t flags, kind;
} RV;
typedef RV *ABI (*Script)(CInstance *, CInstance *, RV *, int, RV **);
typedef RV *ABI (*Read)(void *, int);
typedef int ABI (*Compare)(const RV *, const RV *, double, int);
typedef int ABI (*Bool)(const RV *);
typedef RV *ABI (*Legacy)(CInstance *, CInstance *, RV *, int, int, RV **);
typedef int ABI (*Get)(const RV *, int, int, RV *, int, int);
typedef void ABI (*Free)(RV *);
typedef float ABI (*Direction)(float, float, float, float);
typedef double ABI (*Real)(const RV *);
typedef double ABI (*Abs)(double);
typedef int ABI (*Keyboard)(int);
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;
typedef struct { const char *name; void *call; int argc; } Function;

unsigned held_native_calls, held_native_hits, held_native_fallbacks;
unsigned held_verify_calls, held_verify_mismatch;
unsigned held_stat_true, held_stat_analog, held_stat_stick, held_stat_button, held_stat_vkey;
uint32_t held_native_us;
int held_verify, held_ab, held_ab_off, held_timing;
static unsigned reject_line[16], reject_count[16];

static Script original;
static Read read_var;
static Compare compare;
static Bool truth;
static Legacy legacy;
static Get get;
static Free free_value;
static Direction direction;
static Real real_value;
static Abs yyabs;
static Keyboard keyboard_check;
static void **global, **object_hash;
static uint32_t *id_hash, *changes, *activation;
static Trace **trace_start;
static double *epsilon;
static Function **functions;
static const RV *constants[8]; // 180, 0, 90, 270, 32785, 32786, 32787, 32788
static const char *script_name;

enum { MODE, THRESHOLD, OVERRIDE, LIST, HELD, ASSIGNED, POSX, POSY, CONTROL, SLOT, NV };
static const char *const variable_names[NV] = {
	"mode", "controller_threshold", "control_override", "vKeyList", "isHeld", "assignedKey",
	"posValueX", "posValueY", "control", "controller_slot" };
static const uint32_t *variables[NV];
enum { EXISTS, ANGLE, MOUSE, AXIS, BUTTON, VALUE, NF };
static const char *const function_names[NF] = {
	"instance_exists", "angle_difference", "mouse_check_button", "gamepad_axis_value",
	"gamepad_button_check", "gamepad_button_value" };
static const uint32_t *descriptors[NF];
enum { OBJ_CONTROLLER = 501, OBJ_KEYBOARD = 703, OBJ_JOYSTICK = 880 };
enum { C180, C0, C90, C270, AXIS_LH, AXIS_LV, AXIS_RH, AXIS_RV };

static uint32_t word(const void *p, unsigned offset) {
	uint32_t v;
	memcpy(&v, (const char *)p + offset, 4);
	return v;
}

static void reject(unsigned line) {
	held_native_fallbacks++;
	for (int i = 0; i < 16; i++)
		if (reject_line[i] == line || !reject_line[i]) {
			reject_line[i] = line;
			reject_count[i]++;
			return;
		}
}

static RV number(double d) { RV v; v.v.d = d; v.flags = 0; v.kind = 0; return v; }
static RV reference(unsigned id) { RV v; v.v.bits = UINT64_C(0x0100000000000000) | id; v.flags = 0; v.kind = 15; return v; }
static int scalar(const RV *v) {
	return v && (v->kind == 0 || v->kind == 7 || v->kind == 13) && (v->kind == 7 || isfinite(v->v.d));
}
static int disjoint(const void *a, size_t n, const RV *b) {
	uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
	return x >= y + 16 || y >= x + n;
}
static RV *slot(void *owner, int n) { return read_var(owner, (int)variables[n][1]); }
// The value as the original converts it before a float/int use: the payload double for kind 0, else REAL_RValue_Ex.
static double realv(const RV *v) { return (v->kind & 0xffffff) ? real_value(v) : v->v.d; }

// YYCompareVal(a, b, eps, flag). Two finite reals: d = a - b; 0 if |d| <= eps, else -1/1 (the runner's real path,
// see visibility_native.c). Everything else goes to the runner.
static int cmp(const RV *a, const RV *b, int flag) {
	if (a->kind == 0 && b->kind == 0) {
		double d = a->v.d - b->v.d, eps = *epsilon;
		if (d == d)
			return fabs(d) <= eps ? 0 : d < 0 ? -1 : 1;
	}
	return compare(a, b, *epsilon, flag);
}
static int eq(const RV *a, double b) { RV v = number(b); return cmp(a, &v, 0) == 0; }
// `a < b` and `a > b` as the original tests YYCompareVal's result (-2: not comparable).
static int less(const RV *a, const RV *b) { int r = cmp(a, b, 1); return r != -2 && r < 0; }
static int greater(const RV *a, const RV *b) { return cmp(a, b, 1) >= 1; }

static void *hash_find(const void *h, int id) {
	uintptr_t node;
	unsigned trips = 0;
	if (!h || !word(h, 0))
		return NULL;
	node = word((void *)(uintptr_t)word(h, 0), ((unsigned)id & word(h, 4)) * 8);
	while (node && trips++ < 4096) {
		if ((int)word((void *)node, 8) == id)
			return (void *)(uintptr_t)word((void *)node, 12);
		node = word((void *)node, 4);
	}
	return NULL;
}
static int ordinary(void *p) { return p && word(p, 0x50) == 1 && !(word(p, 0x7c) & 0x100003); }
// The instance an object-index receiver resolves to (input_native.c: the runner's OBJTYPE lookup).
static void *object_first(int id) {
	void *obj = hash_find(*object_hash, id);
	uintptr_t node;
	unsigned trips = 0;
	if (!obj)
		return NULL;
	node = word(obj, 0x44);
	while (node && trips++ < 4096) {
		void *p = (void *)(uintptr_t)word((void *)node, 8);
		if (!p)
			return NULL;
		if (!(word(p, 0x7c) & 0x100003))
			return ordinary(p) ? p : NULL;
		node = word((void *)node, 0);
	}
	return NULL;
}
static RV *array(const RV *v, unsigned *length) {
	void *h;
	int n;
	if (!v || v->kind != 2 || !v->v.w[0])
		return NULL;
	h = (void *)(uintptr_t)v->v.w[0];
	n = (int)word(h, 28);
	if (n < 0 || n > 64 || (n && !word(h, 4)))
		return NULL;
	*length = (unsigned)n;
	return (RV *)(uintptr_t)word(h, 4);
}

typedef struct {
	CInstance *self, *other;
	Trace *trace;
	RV out;
} Ctx;
static RV *call(Ctx *c, int line, int f, int n, const RV *a, const RV *b) {
	// The original passes its own copies; the dispatcher copies again for the builtin.
	RV x, y, *argv[2] = { &x, &y };
	if (a)
		x = *a;
	if (b)
		y = *b;
	c->trace->line = line;
	c->out.v.w[0] = 0;
	c->out.v.w[1] = 0;
	c->out.flags = 0;
	c->out.kind = 5;
	return legacy(c->self, c->other, &c->out, n, (int)descriptors[f][1], argv);
}
// abs(angle_difference(constant, dir)) < limit, as the original computes it.
static int angle_within(Ctx *c, int line, int k, const RV *dir, double minus_limit) {
	call(c, line, ANGLE, 2, constants[k], dir);
	return yyabs(realv(&c->out)) + minus_limit < -*epsilon;
}
static RV axis(Ctx *c, int line, const RV *slot_value, int k) {
	call(c, line, AXIS, 2, slot_value, constants[k]);
	return c->out;
}
static RV point_dir(const RV *x, const RV *y) {
	float d = direction(0, 0, (float)realv(x), (float)realv(y));
	return number((double)d);
}

RV *ABI held_native(CInstance *self, CInstance *other, RV *result, int argc, RV **argv) {
	unsigned n = UINT32_MAX, m = UINT32_MAX, i, j, line = 0;
	RV *bindings, *list, *threshold, *mode, *override, *control_p, *slot_p;
	RV held[64], assigned[64], keys[64], control, padslot, dz, px = number(0), py = number(0);
	RV kb = reference(OBJ_KEYBOARD), ctl = reference(OBJ_CONTROLLER), tmp;
	void *keyboard, *controller, *joystick;
	Trace trace;
	Ctx c;
	int yes = 0, mode0, mode3, any_analog = 0;
	uint32_t t0 = held_timing ? sceKernelGetProcessTimeLow() : 0;
	held_native_calls++;
#define CHECK(x) do { if (!(x)) { line = __LINE__; goto fallback; } } while (0)
	CHECK(self && result && !((uintptr_t)result & 3));
	CHECK(argc == 1 && argv && argv[0]);
	CHECK(disjoint(argv[0], 16, result));
	CHECK(result->kind == 0 || result->kind == 7 || result->kind == 13 || result->kind == 5 || result->kind == 0xffffff);
	CHECK(*global && *functions && isfinite(*epsilon) && *epsilon >= 0 && *epsilon < 0.01);
	CHECK(changes[2] == 0 && activation[2] == 0);
	{
		// The original's two global reads, in its order, through g_pGlobal's vtable getter
		Read global_get = (*(Read **)*global)[2];
		mode = global_get(*global, (int)variables[MODE][1]);
		threshold = global_get(*global, (int)variables[THRESHOLD][1]);
	}
	CHECK(scalar(mode) && scalar(threshold));
	// The original writes the result (kind undefined) before it uses any of the values below
	CHECK(disjoint(mode, 16, result) && disjoint(threshold, 16, result));
	CHECK(!eq(mode, 1)); // global.mode 1: the player's own controller (object_get_name)
	bindings = array(argv[0], &n);
	CHECK(bindings || (argv[0]->kind == 2 && argv[0]->v.w[0] && n == 0));
	CHECK(disjoint((void *)(uintptr_t)argv[0]->v.w[0], 32, result));
	CHECK(!n || disjoint(bindings, 16 * n, result));
	for (i = 0; i < n; i++) {
		double k;
		CHECK(scalar(&bindings[i]));
		k = realv(&bindings[i]);
		CHECK(k > -2147483648.0 && k < 2147483648.0);
		keys[i] = bindings[i];
	}
	controller = object_first(OBJ_CONTROLLER);
	CHECK(controller);
	override = slot(controller, OVERRIDE);
	CHECK(scalar(override) && !truth(override) && disjoint(override, 16, result));
	control_p = slot(controller, CONTROL);
	slot_p = slot(controller, SLOT);
	CHECK(scalar(control_p) && scalar(slot_p) && disjoint(control_p, 16, result) && disjoint(slot_p, 16, result));
	control = *control_p;
	padslot = *slot_p;
	keyboard = object_first(OBJ_KEYBOARD);
	CHECK(keyboard);
	CHECK(slot(keyboard, LIST) && disjoint(slot(keyboard, LIST), 16, result));
	list = array(slot(keyboard, LIST), &m);
	CHECK(list && m >= 1 && disjoint(list, 16 * m, result));
	CHECK(disjoint((void *)(uintptr_t)slot(keyboard, LIST)->v.w[0], 32, result));
	for (j = 0; j < m; j++) {
		int id;
		void *key;
		RV *h, *a;
		CHECK(list[j].kind == 15 || list[j].kind == 7 || list[j].kind == 0);
		if (list[j].kind == 0) {
			CHECK(isfinite(list[j].v.d) && list[j].v.d >= 100000 && list[j].v.d <= 2147483647);
			id = (int)list[j].v.d;
		} else
			id = (int)list[j].v.w[0];
		CHECK(id >= 100000);
		key = hash_find(id_hash, id);
		CHECK(ordinary(key));
		h = slot(key, HELD);
		a = slot(key, ASSIGNED);
		CHECK(scalar(h) && scalar(a) && disjoint(h, 16, result) && disjoint(a, 16, result));
		held[j] = *h;
		assigned[j] = *a;
	}
	mode0 = eq(&control, 0);
	mode3 = !mode0 && eq(&control, 3);
	for (i = 0; i < n && !any_analog; i++)
		for (j = 0; j < 8; j++)
			if (eq(&keys[i], j == 7 ? 99970 : 9990 + (double)j))
				any_analog = 1;
	if (mode3 && any_analog) {
		// The original reads obj_screenJoystick without an instance_exists test
		joystick = object_first(OBJ_JOYSTICK);
		CHECK(joystick && scalar(slot(joystick, POSX)) && scalar(slot(joystick, POSY)));
		CHECK(disjoint(slot(joystick, POSX), 16, result) && disjoint(slot(joystick, POSY), 16, result));
		px = *slot(joystick, POSX);
		py = *slot(joystick, POSY);
	}
#undef CHECK

	held_native_hits++;
	trace.previous = *trace_start;
	trace.name = script_name;
	trace.line = 5;
	*trace_start = &trace;
	c.self = self;
	c.other = other;
	c.trace = &trace;
	tmp.v.bits = 0;
	tmp.flags = 0;
	tmp.kind = 5; // a GET frees what its destination held
	call(&c, 5, EXISTS, 1, &ctl, NULL);
	if (!truth(&c.out))
		goto done;
	for (i = 0; i < n; i++) {
		const RV *key = &keys[i];
		trace.line = 30;
		if (eq(key, 32776)) {
			trace.line = 32;
			get(&kb, (int)variables[LIST][1], 0, &tmp, 0, 0);
			if (truth(&held[0])) {
				held_stat_vkey++;
				yes = 1;
				break;
			}
		}
		call(&c, 50, EXISTS, 1, &kb, NULL);
		if (truth(&c.out)) {
			// Whole-array read (length test) and element read: copy-on-write owner, balanced references
			trace.line = 53;
			get(&kb, (int)variables[LIST][1], (int)0x80000000u, &tmp, 0, 0);
			free_value(&tmp);
			tmp.flags = 0;
			tmp.kind = 5; // FREE_RValue__Pre leaves the array tag
			trace.line = 55;
			get(&kb, (int)variables[LIST][1], 0, &tmp, 0, 0);
			for (j = 0; j < m; j++)
				if (cmp(&assigned[j], key, 0) == 0 && truth(&held[j])) {
					held_stat_vkey++;
					yes = 1;
					break;
				}
			if (yes)
				break;
		}
		trace.line = mode0 ? 62 : 80; // the control == 0 / control == 3 tests
		if (mode0 || mode3) {
			int base = mode0 ? 64 : 82; // mode 0: lines 64-75, mode 3: 82-93
			trace.line = base;
			if (eq(key, 1) || eq(key, 2) || eq(key, 3)) {
				call(&c, base + 2, MOUSE, 1, key, NULL);
				if (truth(&c.out)) {
					yes = 1;
					break;
				}
				if (mode0)
					continue;
			} else {
				trace.line = base + 7;
				if (eq(key, -100000)) {
					trace.line = base + 9;
					yes = keyboard_check(1) != 0;
					break;
				}
				trace.line = base + 11;
				if (keyboard_check((int)(int64_t)realv(key))) {
					yes = 1;
					break;
				}
				if (mode0)
					continue;
			}
			// Mode 3: on-screen joystick, then the pad's sticks or buttons
			int analog = 0;
			trace.line = 141;
			for (j = 0; j < 8 && !analog; j++)
				analog = eq(key, j == 7 ? 99970 : 9990 + (double)j);
			if (analog) {
				RV dir, lx, ly, rx, ry, neg;
				held_stat_analog++;
				trace.line = 145;
				dz = number(0.05);
				neg = number(-realv(&dz));
				dir = point_dir(&px, &py);
				if (eq(key, 9990) && less(&px, &neg) && angle_within(&c, 145, C180, &dir, -120.0))
					yes = 1;
				else if (eq(key, 9991) && greater(&px, &dz) && angle_within(&c, 145, C0, &dir, -120.0))
					yes = 1;
				else if (eq(key, 9992) && less(&py, &neg) && angle_within(&c, 145, C90, &dir, -60.0))
					yes = 1;
				else if (eq(key, 9993) && greater(&py, &dz) && angle_within(&c, 145, C270, &dir, -60.0))
					yes = 1;
				if (yes)
					break;
				trace.line = 151;
				dz = *threshold;
				neg = number(-realv(&dz));
				lx = axis(&c, 153, &padslot, AXIS_LH);
				ly = axis(&c, 154, &padslot, AXIS_LV);
				trace.line = 156;
				dir = point_dir(&lx, &ly);
				if (lx.v.d != 0 || ly.v.d != 0)
					held_stat_stick++;
				if (eq(key, 9990) && less(&lx, &neg) && angle_within(&c, 156, C180, &dir, -120.0))
					yes = 1;
				else if (eq(key, 9991) && greater(&lx, &dz) && angle_within(&c, 156, C0, &dir, -120.0))
					yes = 1;
				else if (eq(key, 9992) && less(&ly, &neg) && angle_within(&c, 156, C90, &dir, -60.0))
					yes = 1;
				else if (eq(key, 9993) && greater(&ly, &dz) && angle_within(&c, 156, C270, &dir, -60.0))
					yes = 1;
				if (yes)
					break;
				rx = axis(&c, 162, &padslot, AXIS_RH);
				ry = axis(&c, 163, &padslot, AXIS_RV);
				trace.line = 163;
				if ((eq(key, 9994) && less(&rx, &neg)) || (eq(key, 9995) && greater(&rx, &dz)) ||
					(eq(key, 9996) && less(&ry, &neg)) || (eq(key, 99970) && greater(&ry, &dz))) {
					yes = 1;
					break;
				}
			} else {
				held_stat_button++;
				call(&c, 174, BUTTON, 2, &padslot, key);
				if (truth(&c.out)) {
					trace.line = 176;
					if (eq(key, 32776) || eq(key, 32775)) {
						RV half = number(0.5);
						call(&c, 178, VALUE, 2, &padslot, key);
						trace.line = 179;
						if (cmp(&c.out, &half, 1) > 0) {
							yes = 1;
							break;
						}
					} else {
						yes = 1;
						break;
					}
				}
			}
		} else {
			// Other control modes: pad only
			int analog = 0;
			trace.line = 193;
			for (j = 0; j < 8 && !analog; j++)
				analog = eq(key, j == 7 ? 99970 : 9990 + (double)j);
			if (analog) {
				RV a, b, dir, neg, v;
				held_stat_analog++;
				trace.line = 197;
				dz = *threshold;
				neg = number(-realv(&dz));
				a = axis(&c, 198, &padslot, AXIS_LH);
				b = axis(&c, 198, &padslot, AXIS_LV);
				dir = point_dir(&a, &b);
				if (eq(key, 9990)) {
					v = axis(&c, 198, &padslot, AXIS_LH);
					yes = less(&v, &neg) && angle_within(&c, 198, C180, &dir, -120.0);
				}
				if (!yes && eq(key, 9991)) {
					v = axis(&c, 198, &padslot, AXIS_LH);
					yes = greater(&v, &dz) && angle_within(&c, 198, C0, &dir, -120.0);
				}
				if (!yes && eq(key, 9992)) {
					v = axis(&c, 198, &padslot, AXIS_LV);
					yes = less(&v, &neg) && angle_within(&c, 198, C90, &dir, -60.0);
				}
				if (!yes && eq(key, 9993)) {
					v = axis(&c, 198, &padslot, AXIS_LV);
					yes = greater(&v, &dz) && angle_within(&c, 198, C270, &dir, -60.0);
				}
				if (!yes && eq(key, 9994)) {
					v = axis(&c, 198, &padslot, AXIS_RH);
					yes = less(&v, &neg);
				}
				if (!yes && eq(key, 9995)) {
					v = axis(&c, 198, &padslot, AXIS_RH);
					yes = greater(&v, &dz);
				}
				if (!yes && eq(key, 9996)) {
					v = axis(&c, 198, &padslot, AXIS_RV);
					yes = less(&v, &neg);
				}
				if (!yes && eq(key, 99970)) {
					v = axis(&c, 198, &padslot, AXIS_RV);
					yes = greater(&v, &dz);
				}
				if (yes)
					break;
			} else {
				held_stat_button++;
				call(&c, 208, BUTTON, 2, &padslot, key);
				if (truth(&c.out)) {
					yes = 1;
					break;
				}
			}
		}
	}
done:
	result->v.d = yes ? 1.0 : 0.0;
	result->kind = 13;
	*trace_start = trace.previous;
	if (yes)
		held_stat_true++;
	if (held_timing)
		held_native_us += sceKernelGetProcessTimeLow() - t0;
	return result;
fallback:
	reject(line);
	return original(self, other, result, argc, argv);
}

// Entry hook target: A/B switch and the verify mode around the native.
static RV *ABI held_entry(CInstance *self, CInstance *other, RV *result, int argc, RV **argv) {
	if (held_ab_off)
		return original(self, other, result, argc, argv);
	if (!held_verify)
		return held_native(self, other, result, argc, argv);
	RV before = result ? *result : number(0);
	unsigned fallbacks = held_native_fallbacks;
	held_native(self, other, result, argc, argv);
	if (held_native_fallbacks == fallbacks) {
		RV check = before;
		original(self, other, &check, argc, argv);
		held_verify_calls++;
		if (check.kind != result->kind || check.v.bits != result->v.bits || check.flags != result->flags)
			held_verify_mismatch++;
	}
	return result;
}

void held_native_report(int frames) {
	if (!original || frames <= 0)
		return;
	debugPrintf("[HELD] controller_check native %.1f fallback %.1f per frame (true %u, vkey %u, button %u, analog %u, "
		"stick moved %u)%s\n", (float)held_native_hits / frames, (float)held_native_fallbacks / frames, held_stat_true,
		held_stat_vkey, held_stat_button, held_stat_analog, held_stat_stick,
		held_ab ? (held_ab_off ? " | A/B: off" : " | A/B: on") : "");
	if (held_timing)
		debugPrintf("[HELD] time %.3f ms per frame\n", held_native_us / 1000.0f / frames);
	if (held_verify)
		debugPrintf("[HELD] verify: %u calls, %u mismatches (session)\n", held_verify_calls, held_verify_mismatch);
	for (int i = 0; i < 16 && reject_line[i]; i++)
		debugPrintf("[HELD] fallback at held_native.c:%u x%u (session)\n", reject_line[i], reject_count[i]);
	held_native_calls = held_native_hits = held_native_fallbacks = 0;
	held_stat_true = held_stat_vkey = held_stat_button = held_stat_analog = held_stat_stick = 0;
	held_native_us = 0;
	if (held_ab)
		held_ab_off = !held_ab_off;
}

int held_native_install(void *(*rx_alloc)(size_t)) {
	uintptr_t entry;
	uint32_t code[4];
	void *rx;
	unsigned i;
	char name[96];
	if (original || !rx_alloc)
		return 0;
#define RESOLVE(t, n) ((t)so_symbol(&yoyoloader_mod, n))
	entry = RESOLVE(uintptr_t, "_Z27gml_Script_controller_checkP9CInstanceS0_R8YYRValueiPPS1_");
	if (!entry || (entry & 3) || word((void *)entry, 0) != 0xe92d4ff0 || word((void *)entry, 4) != 0xe28db01c)
		return 0;
	read_var = RESOLVE(Read, "_ZN12YYObjectBase17InternalReadYYVarEi");
	compare = RESOLVE(Compare, "_Z12YYCompareValRK6RValueS1_db");
	truth = RESOLVE(Bool, "_Z11BOOL_RValuePK6RValue");
	legacy = RESOLVE(Legacy, "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	get = RESOLVE(Get, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	free_value = RESOLVE(Free, "_Z16FREE_RValue__PreP6RValue");
	direction = RESOLVE(Direction, "_Z21YYGML_point_directionffff");
	real_value = RESOLVE(Real, "_Z14REAL_RValue_ExPK6RValue");
	yyabs = RESOLVE(Abs, "_Z9YYGML_absd");
	keyboard_check = RESOLVE(Keyboard, "_Z20YYGML_keyboard_checki");
	global = RESOLVE(void **, "g_pGlobal");
	object_hash = RESOLVE(void **, "g_ObjectHash");
	id_hash = RESOLVE(uint32_t *, "_ZN9CInstance14ms_ID2InstanceE");
	changes = RESOLVE(uint32_t *, "g_InstanceChangeArray");
	activation = RESOLVE(uint32_t *, "g_InstanceActivateDeactive");
	trace_start = RESOLVE(Trace **, "_ZN13SYYStackTrace8s_pStartE");
	epsilon = RESOLVE(double *, "g_GMLMathEpsilon");
	functions = RESOLVE(Function **, "the_functions");
	for (i = 0; i < 8; i++) {
		strcpy(name, "gs_constArg0_4D95CF18");
		name[11] = (char)('0' + i);
		constants[i] = RESOLVE(const RV *, name);
		if (!constants[i])
			return 0;
	}
	for (i = 0; i < NV; i++) {
		strcpy(name, "g_VAR_");
		strcat(name, variable_names[i]);
		variables[i] = RESOLVE(const uint32_t *, name);
		if (!variables[i])
			return 0;
	}
	for (i = 0; i < NF; i++) {
		strcpy(name, "g_FUNC_");
		strcat(name, function_names[i]);
		descriptors[i] = RESOLVE(const uint32_t *, name);
		if (!descriptors[i])
			return 0;
	}
#undef RESOLVE
	if (!read_var || !compare || !truth || !legacy || !get || !free_value || !direction || !real_value || !yyabs ||
		!keyboard_check || !global || !object_hash || !id_hash || !changes || !activation || !trace_start ||
		!epsilon || !functions || !*functions)
		return 0;
	for (i = 0; i < NF; i++)
		if (!(*functions)[descriptors[i][1]].call)
			return 0;
	// The constant arguments the original passes (gs_constArg1 is 0.0 in .bss)
	static const double expect[8] = { 180, 0, 90, 270, 32785, 32786, 32787, 32788 };
	for (i = 0; i < 8; i++)
		if (constants[i]->kind != 0 || constants[i]->v.d != expect[i])
			return 0;
	// The stack-trace literal from the entry's PC-relative ADD (ldr r0, [pc, #0xff0] at +0x30, add r0, pc at +0x38)
	// Check both instructions before trusting the literal, and that the whole name lies in a mapped runner segment
	static const char want_name[] = "gml_Script_controller_check";
	if (word((void *)entry, 0x30) != 0xe59f0ff0 || word((void *)entry, 0x38) != 0xe08f0000)
		return 0;
	uintptr_t name_addr = entry + 0x40 + (int32_t)word((void *)entry, 0x1028);
	int mapped = name_addr >= yoyoloader_mod.text_base &&
		name_addr + sizeof want_name <= yoyoloader_mod.text_base + yoyoloader_mod.text_size;
	for (i = 0; !mapped && i < MAX_DATA_SEG; i++)
		mapped = yoyoloader_mod.data_size[i] && name_addr >= yoyoloader_mod.data_base[i] &&
			name_addr + sizeof want_name <= yoyoloader_mod.data_base[i] + yoyoloader_mod.data_size[i];
	if (!mapped)
		return 0;
	script_name = (const char *)name_addr;
	if (memcmp(script_name, want_name, sizeof want_name))
		return 0;
	rx = rx_alloc(16);
	if (!rx || ((uintptr_t)rx & 3))
		return 0;
	code[0] = 0xe92d4ff0;
	code[1] = 0xe28db01c;
	code[2] = 0xe51ff004;
	code[3] = (uint32_t)(entry + 8);
	kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof code);
	kuKernelFlushCaches(rx, sizeof code);
	original = (Script)rx;
	// Both need the debug report (it prints mismatches and flips the A/B): debugMode=1 only
	extern int debugMode;
	held_verify = debugMode && exp_flag("heldverify");
	held_ab = debugMode && exp_flag("heldab");
	extern int heavy_profiling;
	held_timing = heavy_profiling;
	hook_addr(entry, (uintptr_t)held_entry);
	kuKernelFlushCaches((void *)entry, 8);
	return 1;
}
