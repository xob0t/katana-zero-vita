/*
 * smokedraw_native.c
 *
 * Exact native obj_smokecloud Draw_0 (YYC event at 0x0490421c in libyoyo SHA256
 * ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701). club_4's broken pipe keeps 84 clouds alive and
 * their Draw cost 3.7 ms per frame. Much of that is the event's own variable traffic: about twenty hash lookups,
 * a dozen YYGML_Variable_GetValue -> Variable_GetValue_Direct -> GV_* chains, nine Variable_SetValue_Direct -> SV_*
 * chains, tagged operators and compares, and the copy/free bookkeeping of thirty stack RValues.
 *
 * Ported from the ARM listing, which differs from the readable lift (work/decomp/out) in three places:
 * - the coloured, non-blackout branch multiplies by self.alpha (line 60); only the plain branch uses other.alpha;
 * - the plain branch writes no image_blend before its draw_self;
 * - the slowdown branch ends with `angle = image_angle` (a write reference on angle, value from the image_angle
 *   getter), not `image_angle = angle`.
 *
 * What runs where:
 * - Every helper with effects is the runner's own code, reached through the same target as the event's `bl`
 *   (decoded from the event at install, so loader hooks on those targets still apply): update_light_array(false),
 *   inside_screen(50), update_region(), draw_sprite_recolored(...), draw_self, draw_sprite_ext, the SV_* setters,
 *   GV_ImageIndex, FREE_RValue__Pre, the event's RValue copy helper, REAL_RValue_Ex, INT64_RValue, __aeabi_d2lz, and
 *   BOOL_RValue / YYCompareVal / the YYRValue operators for anything but plain reals.
 * - Builtin variables: builtin_variables[id].set (and .get for image_index), the function Variable_SetValue_Direct /
 *   Variable_GetValue_Direct tail-calls for a CInstance (type 1) and an id below g_nStartGlobalVariables, after the
 *   same managed-object and read-only checks. The other builtin getters the event reads (x, y, image_xscale/yscale,
 *   image_angle, image_alpha, sprite_index) are three-instruction field reads, done inline when the table holds the
 *   audited GV_* functions.
 * - Instance and global variables: InternalGetYYVarRef(L)'s hash probe. A hit is what the getter returns; a miss
 *   (or, for the read getter, a kind-6 hit, which it may wrap) calls the getter.
 * - PushContextStack/PopContextStack around variable stores: inline, the runner's Push when the stack must grow.
 * - Two reals: YYCompareVal's subtract/epsilon path, BOOL_RValue's > 0.5, the operators' double arithmetic.
 * Every read happens where the original reads, so a helper that changes a variable is seen exactly as before.
 *
 * Not reproduced: the flags word of stack RValues the builtin getters fill (GV_* write value and kind only, so the
 * original copies stack garbage into the flags of ay, ax and, in the slowdown branch, angle, and into the flags of
 * draw_sprite_recolored's arguments; here it is 0). Nothing reads the flags of a real or an asset reference.
 *
 * Calls it can't take run the original event before any effect: self not an ordinary CInstance with the standard
 * variable getters, a managed object, builtin ids, getters or setters it doesn't expect, or a bypassed runner
 * helper that is hooked (checked once per frame).
 *
 * exp_smokedraw0 keeps the original event. Debug (profiling report, [SMOKEDRAW]): exp_smokeab switches the native
 * off and on every report window; exp_smokeprof times the event; exp_smokeverify (debug builds) runs the original with
 * its draws recorded but not submitted, puts the cloud's variables and image fields back, runs the native and compares
 * draws, variables, instance fields and the random state.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#ifdef SMOKEDRAW_TEST
#include "smokedraw_test.h"
#else
#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include "main.h"
#include "so_util.h"
extern int debugMode;
#endif
void *entry_hook(uintptr_t entry, void *replacement);

#define ABI __attribute__((pcs("aapcs")))
#ifdef SMOKEDRAW_TEST
int smokedraw_test_bug; // fixture only: deliberate faults the exp_smokeverify comparison must catch
#define BUG(n) (smokedraw_test_bug == (n))
#else
#define BUG(n) 0
#endif

typedef struct {
	union {
		double d;
		uint32_t w[2];
	} v;
	uint32_t flags, kind;
} RV;
typedef struct Trace {
	struct Trace *prev;
	const char *name;
	int line;
} Trace;

typedef void ABI (*event_fn)(void *self, void *other);
typedef RV *ABI (*script_fn)(void *self, void *other, RV *result, int argc, RV **args);
typedef RV *ABI (*var_fn)(void *owner, int id);
typedef int ABI (*bvar_fn)(void *inst, int index, RV *value);
typedef int ABI (*compare_fn)(const RV *a, const RV *b, double epsilon, int flag);
typedef int ABI (*bool_fn)(const RV *v);
typedef double ABI (*real_fn)(const RV *v);
typedef int64_t ABI (*int64_fn)(const RV *v);
typedef int64_t ABI (*d2l_fn)(double v);
typedef void ABI (*free_fn)(RV *v);
typedef void ABI (*copy_fn)(RV *dst, const RV *src);
typedef void ABI (*push_fn)(void *owner);
typedef void ABI (*mul_int_fn)(RV *ret, const RV *a, int b);
typedef RV *ABI (*op_fn)(RV *lhs, const RV *rhs);
typedef void ABI (*draw_self_fn)(void *inst);
typedef void ABI (*draw_ext_fn)(void *inst, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
	int colour, float alpha);

#define EVENT_ADDR 0x0490421cu
#define PLT_START 0x4ab0db0u
#define PLT_SIZE 0x2f130u
#define ARRAY_INDEX ((int)0x80000000)
#define OWNING 0x46u // RValue kinds 1 (string), 2 (array), 6 (object): the event's copy/free mask
#define KIND(v) ((v)->kind & 0xffffff)

// The event's call sites (ELF addresses): the native calls what each `bl` reaches.
enum {
	C_COMPARE, C_FREE, C_BOOL, C_COPY, C_LIGHTS, C_INSIDE, C_PUSH, C_POP, C_MUL_INT, C_ADD, C_REGION, C_MUL,
	C_DRAW_SELF, C_RECOLORED, C_SUB, C_REAL, C_D2L, C_INT64, C_DRAW_EXT, NCALLS
};
static const uint32_t call_sites[NCALLS] = {
	0x4904450, 0x4904470, 0x4904488, 0x4904524, 0x490455c, 0x490469c, 0x49046a8, 0x4904710, 0x4904890, 0x49048e4,
	0x4904e7c, 0x4905350, 0x4905414, 0x49058b4, 0x4905c48, 0x490622c, 0x4906648, 0x490672c, 0x490679c,
};
// What each site reaches in the audited ELF: the symbol (a PLT stub in the unpatched library) or a local function.
static const char *const call_symbols[NCALLS] = {
	"_Z12YYCompareValRK6RValueS1_db", "_Z16FREE_RValue__PreP6RValue", "_Z11BOOL_RValuePK6RValue", NULL,
	"_Z29gml_Script_update_light_arrayP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z24gml_Script_inside_screenP9CInstanceS0_R8YYRValueiPPS1_", "_Z16PushContextStackP12YYObjectBase",
	"_Z15PopContextStacki", "_ZmlRK8YYRValuei", "_ZN8YYRValuepLERKS_",
	"_Z24gml_Script_update_regionP9CInstanceS0_R8YYRValueiPPS1_", "_ZN8YYRValuemLERKS_",
	"_Z15YYGML_draw_selfP9CInstance", "_Z32gml_Script_draw_sprite_recoloredP9CInstanceS0_R8YYRValueiPPS1_",
	"_ZN8YYRValuemIERKS_", "_Z14REAL_RValue_ExPK6RValue", NULL, "_Z12INT64_RValuePK6RValue",
	"_Z21YYGML_draw_sprite_extP9CInstanceiffffffif",
};
static const uint32_t local_targets[NCALLS] = {[C_COPY] = 0x4906acc, [C_D2L] = 0x4aad78c};
static uintptr_t calls[NCALLS];

// Runner functions the native stands in for (whole or for plain reals): they must be the audited code (no entry or
// GOT hook), else the event runs as compiled. {symbol, ELF address, first two instruction words}
enum { B_GET, B_REF, B_GV_X, B_GV_Y, B_GV_XSCALE, B_GV_YSCALE, B_GV_ANGLE, B_GV_ALPHA, B_GV_SPRITE, B_GV_INDEX };
static const struct {
	const char *name;
	uint32_t addr, w0, w1;
} bypassed[] = {
	{"_ZN12YYObjectBase19InternalGetYYVarRefEi", 0xf21708, 0xe92d4df0, 0xe28db018},
	{"_ZN12YYObjectBase20InternalGetYYVarRefLEi", 0xf219b0, 0xe92d4bf0, 0xe28db018},
	{"_Z4GV_XP9CInstanceiP6RValue", 0xf844d4, 0xed900a2e, 0xe3a00000},
	{"_Z4GV_YP9CInstanceiP6RValue", 0xf844f0, 0xed900a2f, 0xe3a00000},
	{"_Z14GV_ImageXScaleP9CInstanceiP6RValue", 0xf84af4, 0xed900a29, 0xe3a00000},
	{"_Z14GV_ImageYScaleP9CInstanceiP6RValue", 0xf84b10, 0xed900a2a, 0xe3a00000},
	{"_Z13GV_ImageAngleP9CInstanceiP6RValue", 0xf84b2c, 0xed900a2b, 0xe3a00000},
	{"_Z13GV_ImageAlphaP9CInstanceiP6RValue", 0xf84b48, 0xed900a2c, 0xe3a00000},
	{"_Z14GV_SpriteIndexP9CInstanceiP6RValue", 0xf848e8, 0xe3a0100f, 0xe5900088},
	{"_Z13GV_ImageIndexP9CInstanceiP6RValue", 0xf84908, 0xe92d4c10, 0xe28db008}, // called, but must be this one
	{"_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb", 0xf89eb0, 0xe92d4ef0, 0xe28db018},
	{"_Z24Variable_GetValue_DirectP12YYObjectBaseiiP6RValuebb", 0xf891d0, 0xe92d4ff0, 0xe28db01c},
	{"_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue", 0xf8936c, 0xe92d4df0, 0xe28db018},
	{"_Z12YYCompareValRK6RValueS1_db", 0xe3e8f8, 0xe92d4970, 0xe28db010},
	{"_Z11BOOL_RValuePK6RValue", 0xec9c5c, 0xe92d4c10, 0xe28db008},
	{"_ZmlRK8YYRValuei", 0xbae3ac, 0xe92d4830, 0xe28db008},
	{"_ZN8YYRValuepLERKS_", 0xbaed0c, 0xe92d4830, 0xe28db008},
	{"_ZN8YYRValuemLERKS_", 0xbae6ac, 0xe92d48f0, 0xe28db010},
	{"_ZN8YYRValuemIERKS_", 0xbaeb4c, 0xe92d4830, 0xe28db008},
	{"_Z16PushContextStackP12YYObjectBase", 0xecc284, 0xe92d48f0, 0xe28db010},
	{"_Z15PopContextStacki", 0xecc33c, 0xe59f1014, 0xe79f1001},
};
#define NBYPASSED (sizeof(bypassed) / sizeof(*bypassed))
static uintptr_t bypassed_addr[NBYPASSED];
static uintptr_t *bypassed_got[NBYPASSED];

enum {
	V_UPDATE_LIGHTS, V_PARTICLE_COLORING, V_BLACKOUT, V_SLOWDOWN, V_UPDATE_LIGHT_TIMER, V_IN_SCREEN, V_AY, V_OFFSET,
	V_AX, V_HOFFSET, V_ANIMATION_INDEX, V_ANGLE, V_REGION, V_REAL_ALPHA, V_COLOR_ME, V_SPARK, V_ALPHA, V_LIGHT_COL,
	V_FIRST_BUILTIN,
	V_X = V_FIRST_BUILTIN, V_Y, V_IMAGE_XSCALE, V_IMAGE_YSCALE, V_IMAGE_ANGLE, V_IMAGE_ALPHA, V_SPRITE_INDEX,
	V_IMAGE_INDEX, V_IMAGE_BLEND, NVARS
};
static const char *const var_names[NVARS] = {
	"update_lights", "particle_coloring", "blackout", "slowdown", "update_light_timer", "in_screen", "ay", "offset",
	"ax", "hoffset", "animation_index", "angle", "region", "real_alpha", "color_me", "spark", "alpha", "light_col",
	"x", "y", "image_xscale", "image_yscale", "image_angle", "image_alpha", "sprite_index", "image_index",
	"image_blend",
};
static const int32_t *vars[NVARS]; // g_VAR_* descriptors; [1] is the id, filled at game start
static int id[NVARS];               // ids as validated
// Builtins written through their setter
static const uint8_t builtin_write[NVARS] = {[V_X] = 1, [V_Y] = 1, [V_IMAGE_INDEX] = 1, [V_IMAGE_ANGLE] = 1,
	[V_IMAGE_BLEND] = 1, [V_IMAGE_ALPHA] = 1};

static var_fn standard_get, standard_ref;
static void **global_obj;
static const double *epsilon;
static Trace **trace_start;
static const int *builtin_count;
static const uint8_t *builtin_variables; // 16-byte entries {name, get, set, can_set}
static const uint8_t *managed_objects, *garbage_collection;
static void ***context_stack;
static int *context_top, *context_max;
static const uint32_t *frame_counter;
static const RV *const_false, *const_50;
static bvar_fn set_fn[NVARS], get_image_index;
static event_fn original;
static int installed;
static uint32_t validated_frame;
static unsigned validated_generation;
static int validated;

int smokedraw_ab, smokedraw_ab_off, smokedraw_prof, smokedraw_verify;
unsigned smokedraw_native_calls, smokedraw_fallbacks;
unsigned smokedraw_reject_line[8], smokedraw_reject_count[8];
unsigned smokedraw_prof_us, smokedraw_prof_calls;
unsigned smokedraw_verified, smokedraw_mismatches, smokedraw_verify_records, smokedraw_verify_skipped;
unsigned smokedraw_verify_unrecorded;

static int reject(unsigned line) {
	for (int i = 0; i < 8; i++)
		if (!smokedraw_reject_line[i] || smokedraw_reject_line[i] == line) {
			smokedraw_reject_line[i] = line;
			smokedraw_reject_count[i]++;
			break;
		}
	return 0;
}
#define REQUIRE(c) do { if (!(c)) return reject(__LINE__); } while (0)

static inline uint32_t word(const void *p, size_t o) {
	uint32_t v;
	memcpy(&v, (const uint8_t *)p + o, 4);
	return v;
}
static inline float field(const void *inst, size_t o) {
	float v;
	memcpy(&v, (const uint8_t *)inst + o, 4);
	return v;
}
static inline int owning(const RV *v) { return (OWNING >> (v->kind & 31)) & 1; }
static inline var_fn vtable(void *owner, int slot) { return *(var_fn *)(uintptr_t)(word(owner, 0) + 4 * slot); }

// InternalGetYYVarRef(L)'s hash probe (0xf217a8, 0xf21a50): the slot of an existing variable, or NULL
static inline RV *peek(void *owner, int vid) {
	const uint8_t *ht = (const uint8_t *)(uintptr_t)word(owner, 0x24);
	if (!ht)
		return NULL;
	uint32_t mask = word(ht, 8), key = ((uint32_t)vid + 1) & 0x7fffffff, i = key & mask;
	const uint8_t *entries = (const uint8_t *)(uintptr_t)word(ht, 16);
	for (uint32_t d = 0; d <= mask; d++, i = (i + 1) & mask) {
		const uint8_t *e = entries + 12 * i;
		uint32_t k = word(e, 8);
		if (k == key)
			return (RV *)(uintptr_t)word(e, 0);
		if (!k || d > ((i - (k & mask)) & mask))
			return NULL;
	}
	return NULL;
}
// owner->vtable[2] (read reference) for an owner known to use InternalGetYYVarRef
static inline RV *get_std(void *owner, int n) {
	RV *v = peek(owner, id[n]);
	if (v && v->kind != 6)
		return v;
	return standard_get(owner, id[n]);
}
static RV *get(void *owner, int n) {
	var_fn fn = vtable(owner, 2);
	return fn == standard_get ? get_std(owner, n) : fn(owner, id[n]);
}
// self->vtable[3] (write reference), self known to use InternalGetYYVarRefL
static inline RV *ref(void *self, int n) {
	RV *v = peek(self, id[n]);
	return v ? v : standard_ref(self, id[n]);
}
static inline void bset(void *self, int n, RV *value) { set_fn[n](self, ARRAY_INDEX, value); }
// GV_X & co.: value (double)(float field), kind 0; the flags word is not written
static inline void bget_field(void *self, size_t o, RV *out) {
	out->v.d = (double)field(self, o);
	out->kind = 0;
}

static inline void free_rv(RV *v) {
	if (owning(v))
		((free_fn)calls[C_FREE])(v);
}
// The event's copy into an existing RValue: flags and kind, then a counted copy (helper) or the 8 value bytes
static inline void copy_rv(RV *dst, const RV *src) {
	dst->flags = src->flags;
	dst->kind = src->kind;
	if (owning(src))
		((copy_fn)calls[C_COPY])(dst, src);
	else
		dst->v = src->v;
}
// `slot = *value` as compiled: through a temporary, free, copy
static inline void assign_rv(RV *slot, const RV *value) {
	RV tmp = *value;
	free_rv(slot);
	copy_rv(slot, &tmp);
}
// PushContextStack(self) (0xecc284): with garbage collection on, append; the runner's own call when it must grow
static inline void push_context(void *self) {
	if (!*garbage_collection)
		return;
	int top = *context_top;
	if (top == *context_max) {
		((push_fn)calls[C_PUSH])(self);
		return;
	}
	(*context_stack)[top] = self;
	*context_top = top + 1;
}
// PopContextStack(1) (0xecc33c): top - 1, not below 0
static inline void pop_context(void) {
	int top = *context_top - 1;
	*context_top = top < 0 ? 0 : top;
}
// A store into an instance variable as compiled: inside Push/Pop, skipped when the value is the slot itself
static void store_var(void *self, RV *slot, const RV *value) {
	push_context(self);
	if (value != slot)
		assign_rv(slot, value);
	pop_context();
}

// YYCompareVal(a, b, epsilon, flag); two reals: d = a - b, 0 within epsilon (0xe3ee1c, 0xe3f31c; the flag is not
// read there). A NaN difference takes the runner's own path.
static int compare(const RV *a, const RV *b, int flag) {
	if (KIND(a) == 0 && KIND(b) == 0) {
		double d = a->v.d - b->v.d, eps = *epsilon;
		if (d == d)
			return fabs(d) <= eps ? 0 : (d < 0 ? -1 : 1);
	}
	return ((compare_fn)calls[C_COMPARE])(a, b, *epsilon, flag);
}
// BOOL_RValue: real and bool are value > 0.5 (0xec9ce0)
static inline int truth(const RV *v) {
	uint32_t k = KIND(v);
	if (k == 0 || k == 13)
		return v->v.d > 0.5;
	return ((bool_fn)calls[C_BOOL])(v) != 0;
}
// REAL of a builtin getter's result, as the event converts it (REAL_RValue_Ex unless the kind is 0)
static inline double real_of(const RV *v) {
	return KIND(v) == 0 ? v->v.d : ((real_fn)calls[C_REAL])(v);
}
// lhs *= rhs (YYRValue::operator*=); two reals: lhs = rhs * lhs (0xbae7ec)
static inline void mul_assign(RV *lhs, const RV *rhs) {
	if (lhs->kind == 0 && KIND(rhs) == 0)
		lhs->v.d = rhs->v.d * lhs->v.d;
	else
		((op_fn)calls[C_MUL])(lhs, rhs);
}
static inline RV real(double d) {
	RV r;
	r.v.d = d;
	r.flags = 0;
	r.kind = 0;
	return r;
}
static inline void script_result(RV *result) {
	free_rv(result);
	result->kind = 5;
	result->flags = 0;
	result->v.w[0] = 0;
}
// `v = coord + k * offset` (lines 25/27): operator*(offset, k), copy of coord, operator+=, copy of the sum
static void offset_coord(const RV *coord, const RV *offset, int k, RV *out) {
	if (offset->kind == 0) {
		*out = *coord;
		out->v.d = coord->v.d + (double)k * offset->v.d;
		return;
	}
	RV prod, sum;
	((mul_int_fn)calls[C_MUL_INT])(&prod, offset, k);
	sum = *coord;
	((op_fn)calls[C_ADD])(&sum, &prod);
	RV tmp = sum;
	copy_rv(out, &tmp);
	free_rv(&sum);
	free_rv(&prod);
}
// `image_alpha = real_alpha * alpha` (lines 60/69): copy of real_alpha, operator*=, copy of the product
static void alpha_product(const RV *ra, const RV *a, RV *alpha) {
	RV prod;
	prod.flags = ra->flags;
	prod.kind = ra->kind;
	if (owning(ra))
		((copy_fn)calls[C_COPY])(&prod, ra);
	else
		prod.v = ra->v;
	mul_assign(&prod, a);
	RV tmp = prod;
	free_rv(alpha);
	copy_rv(alpha, &tmp);
	free_rv(&prod);
}

// Static state, checked once per frame: variable ids, builtin table entries, helper code and GOT slots
static int validate(void) {
	int count = *builtin_count;
	REQUIRE(count > 0 && count <= 10000);
	for (int n = 0; n < NVARS; n++) {
		int v = vars[n][1];
		REQUIRE(v >= 0 && v < 0x7ffffffe);
		REQUIRE((n >= V_FIRST_BUILTIN) == (v < count));
		id[n] = v;
	}
	for (int n = V_FIRST_BUILTIN; n < NVARS; n++) {
		const uint8_t *e = builtin_variables + 16 * id[n];
		if (n <= V_SPRITE_INDEX)
			REQUIRE(word(e, 4) == bypassed_addr[B_GV_X + (n - V_X)]);
		if (n == V_IMAGE_INDEX) { // GV_ImageIndex (writes a real, never a value that needs freeing)
			REQUIRE(word(e, 4) == bypassed_addr[B_GV_INDEX]);
			get_image_index = (bvar_fn)bypassed_addr[B_GV_INDEX];
		}
		if (builtin_write[n]) {
			REQUIRE(e[12] && word(e, 8));
			set_fn[n] = (bvar_fn)(uintptr_t)word(e, 8);
		}
	}
	for (unsigned i = 0; i < NBYPASSED; i++) {
		const uint32_t *code = (const uint32_t *)bypassed_addr[i];
		REQUIRE(code[0] == bypassed[i].w0 && code[1] == bypassed[i].w1);
		REQUIRE(!bypassed_got[i] || *bypassed_got[i] == bypassed_addr[i]);
	}
	REQUIRE(*epsilon == *epsilon);
	return 1;
}

// Entry checks: nothing here has an effect. The static ones run again each frame and after any hook install
// (so_hook_generation: hook_addr/hook_arm/hook_thumb and so_got_slot bump it).
static inline int admit(void *self, void *other) {
	uint32_t frame = *frame_counter;
	unsigned generation = so_hook_generation;
	if (frame != validated_frame || generation != validated_generation || !validated) {
		validated_frame = frame;
		validated_generation = generation;
		validated = validate();
	}
	REQUIRE(validated);
	REQUIRE(self && other && *global_obj);
	REQUIRE(word(self, 0x50) == 1); // CInstance: Variable_Get/SetValue_Direct's builtin path
	REQUIRE(vtable(self, 2) == standard_get && vtable(self, 3) == standard_ref);
	if (*managed_objects) { // Variable_SetValue_Direct's managed-object path (0xf89398)
		const uint8_t *object = (const uint8_t *)(uintptr_t)word(self, 0x68);
		REQUIRE(object && !(object[0x50] & 0x80));
	}
	return 1;
}

static void native_event(void *self, void *other) {
	Trace tr = {*trace_start, "gml_Object_obj_smokecloud_Draw_0", 0};
	*trace_start = &tr;
	void *g = *global_obj;
	RV *update_lights = get(g, V_UPDATE_LIGHTS);
	RV *particle_coloring = get(g, V_PARTICLE_COLORING);
	RV *g_blackout = get(g, V_BLACKOUT);
	RV *g_slowdown = get(g, V_SLOWDOWN);
	// Stack RValues freed at the end, as in the original: script result and argument slots, image_angle's value
	RV result, arg[8], angle_copy;
	RV *args[8];
	result.v.w[0] = 0;
	result.kind = 0xffffff;
	for (int i = 0; i < 8; i++) {
		arg[i].v.w[0] = 0;
		arg[i].kind = 0xffffff;
		args[i] = &arg[i];
	}
	angle_copy.v.w[0] = 0;
	angle_copy.kind = 0xffffff;
	RV b; // builtin getter output (the original's per-variable stack slots; only value and kind are written)

	tr.line = 15;
	RV zero = real(0);
	int c = compare(get_std(self, V_UPDATE_LIGHT_TIMER), &zero, 1);
	if ((c != -2 && c < 1) || truth(update_lights)) {
		tr.line = 17;
		script_result(&result);
		assign_rv(&arg[0], const_false);
		if (!BUG(1))
			((script_fn)calls[C_LIGHTS])(self, other, &result, 1, args);
		tr.line = 18;
		RV *timer = ref(self, V_UPDATE_LIGHT_TIMER);
		free_rv(timer);
		timer->kind = 0;
		timer->v.d = 1.0;
	}

	tr.line = 21;
	script_result(&result);
	RV *in_screen = ref(self, V_IN_SCREEN);
	assign_rv(&arg[0], const_50);
	RV *r = ((script_fn)calls[C_INSIDE])(self, other, &result, 1, args);
	store_var(self, in_screen, r);

	tr.line = 24;
	RV *ay = ref(self, V_AY);
	b.flags = 0;
	bget_field(self, 0xbc, &b);
	store_var(self, ay, &b);

	tr.line = 25;
	RV *offset = get_std(self, V_OFFSET);
	bget_field(self, 0xbc, &b);
	RV ynew;
	offset_coord(&b, offset, 4, &ynew);
	bset(self, V_Y, &ynew);

	tr.line = 26;
	RV *ax = ref(self, V_AX);
	bget_field(self, 0xb8, &b);
	store_var(self, ax, &b);

	tr.line = 27;
	RV *hoffset = get_std(self, V_HOFFSET);
	bget_field(self, 0xb8, &b);
	RV xnew;
	offset_coord(&b, hoffset, 2, &xnew);
	bset(self, V_X, &xnew);

	tr.line = 28;
	RV index_copy;
	{
		RV tmp = *get_std(self, V_ANIMATION_INDEX);
		copy_rv(&index_copy, &tmp);
	}
	bset(self, V_IMAGE_INDEX, &index_copy);

	tr.line = 29;
	if (truth(get_std(self, V_IN_SCREEN))) {
		tr.line = 32;
		assign_rv(&angle_copy, get_std(self, V_ANGLE));
		bset(self, V_IMAGE_ANGLE, &angle_copy);

		tr.line = 33;
		RV minus_one = real(-1.0);
		if (compare(get_std(self, V_REGION), &minus_one, 0) == 0) {
			tr.line = 35;
			script_result(&result);
			((script_fn)calls[C_REGION])(self, other, &result, 0, NULL);
		}

		tr.line = 38;
		RV *real_alpha = ref(self, V_REAL_ALPHA);
		free_rv(real_alpha);
		real_alpha->kind = 0;
		tr.line = 41;
		real_alpha->v.d = 0.75;
		RV blend = real(0), alpha = real(0); // non-owning until the event's first assignment
		if (truth(particle_coloring) && truth(get_std(self, V_COLOR_ME))) {
			tr.line = 43;
			blend = real(16777215.0);
			bset(self, V_IMAGE_BLEND, &blend);
			tr.line = 44;
			alpha = real(1.0);
			bset(self, V_IMAGE_ALPHA, &alpha);

			tr.line = 46;
			script_result(&result);
			RV img[7];
			img[0].kind = 0; // GV_SpriteIndex: asset reference {id, 0x01000001}, kind 15
			img[0].v.w[0] = word(self, 0x88);
			img[0].v.w[1] = 0x01000001;
			img[0].kind = 15;
			img[1].kind = 0xffffff;
			get_image_index(self, ARRAY_INDEX, &img[1]);
			bget_field(self, 0xb8, &img[2]);
			bget_field(self, 0xbc, &img[3]);
			bget_field(self, 0xa4, &img[4]);
			bget_field(self, 0xa8, &img[5]);
			bget_field(self, 0xac, &img[6]);
			for (int i = 0; i < 7; i++) {
				img[i].flags = 0;
				assign_rv(&arg[i], &img[i]);
			}
			assign_rv(&arg[7], const_false);
			if (!BUG(4))
				((script_fn)calls[C_RECOLORED])(self, other, &result, 8, args);

			tr.line = 47;
			if ((truth(g_blackout) || truth(get_std(self, V_BLACKOUT))) && !truth(get_std(self, V_SPARK))) {
				tr.line = 49;
				blend = real(4210752.0);
				bset(self, V_IMAGE_BLEND, &blend);
				tr.line = 50;
				alpha = real(1.0);
				bset(self, V_IMAGE_ALPHA, &alpha);
				tr.line = 51;
				((draw_self_fn)calls[C_DRAW_SELF])(self);
				tr.line = 52;
				blend = real(0.0);
				bset(self, V_IMAGE_BLEND, &blend);
				tr.line = 53;
				alpha = real(0.75);
				bset(self, V_IMAGE_ALPHA, &alpha);
				tr.line = 54;
				((draw_self_fn)calls[C_DRAW_SELF])(self);
				tr.line = 55;
				alpha = real(1.0);
				bset(self, V_IMAGE_ALPHA, &alpha);
			} else {
				tr.line = 59;
				blend = real(16777215.0);
				bset(self, V_IMAGE_BLEND, &blend);
				tr.line = 60;
				RV *ra = get_std(self, V_REAL_ALPHA);
				alpha_product(ra, BUG(2) ? get(other, V_ALPHA) : get_std(self, V_ALPHA), &alpha);
				bset(self, V_IMAGE_ALPHA, &alpha);

				tr.line = 62;
				RV *light_col = get_std(self, V_LIGHT_COL);
				RV index, a;
				int32_t sprite_id = (int32_t)word(self, 0x88); // read before image_index, as the original does
				index.kind = 0xffffff;
				get_image_index(self, ARRAY_INDEX, &index);
				int sprite = (int)((d2l_fn)calls[C_D2L])((double)sprite_id);
				double subimg = real_of(&index);
				float x = field(self, 0xb8), y = field(self, 0xbc), xs = field(self, 0xa4), ys = field(self, 0xa8),
					rot = field(self, 0xac);
				int colour = (int)((int64_fn)calls[C_INT64])(light_col);
				bget_field(self, 0xb0, &a);
				((draw_ext_fn)calls[C_DRAW_EXT])(self, sprite, (float)subimg, x, y, xs, ys, rot, colour,
					(float)(BUG(5) ? a.v.d * 0.5 : a.v.d));
			}
		} else {
			tr.line = 69;
			RV *ra = get_std(self, V_REAL_ALPHA);
			alpha_product(ra, get(other, V_ALPHA), &alpha);
			bset(self, V_IMAGE_ALPHA, &alpha);
			tr.line = 70;
			((draw_self_fn)calls[C_DRAW_SELF])(self);
		}

		tr.line = 72;
		free_rv(&angle_copy);
		angle_copy = real(0.0);
		bset(self, V_IMAGE_ANGLE, &angle_copy);
		tr.line = 75;
		RV one = real(1.0);
		int s = compare(g_slowdown, &one, 1);
		if (s != -2 && s <= -1 && truth(get_std(self, V_SLOWDOWN))) {
			tr.line = 77;
			assign_rv(&angle_copy, get_std(self, V_ANGLE));
			bset(self, V_IMAGE_ANGLE, &angle_copy);
			tr.line = 78;
			blend = real(0.0);
			bset(self, V_IMAGE_BLEND, &blend);
			tr.line = 79;
			RV diff = real(1.0);
			if (KIND(g_slowdown) == 0)
				diff.v.d = 1.0 - g_slowdown->v.d;
			else
				((op_fn)calls[C_SUB])(&diff, g_slowdown);
			RV tmp = diff;
			free_rv(&alpha);
			copy_rv(&alpha, &tmp);
			free_rv(&diff);
			bset(self, V_IMAGE_ALPHA, &alpha);
			tr.line = 80;
			((draw_self_fn)calls[C_DRAW_SELF])(self);
			tr.line = 81;
			blend = real(16777215.0);
			bset(self, V_IMAGE_BLEND, &blend);
			tr.line = 82;
			free_rv(&alpha);
			alpha = real(1.0);
			bset(self, V_IMAGE_ALPHA, &alpha);
			tr.line = 83;
			RV *angle = ref(self, V_ANGLE);
			bget_field(self, 0xac, &b);
			store_var(self, angle, &b);
		}
		free_rv(&alpha);
	}

	tr.line = 86;
	RV back;
	{
		RV tmp = *get_std(self, V_AY);
		copy_rv(&back, &tmp);
	}
	bset(self, V_Y, &back);
	free_rv(&back);
	tr.line = 87;
	{
		RV tmp = *get_std(self, V_AX);
		copy_rv(&back, &tmp);
	}
	if (!BUG(3))
		bset(self, V_X, &back);
	free_rv(&back);
	free_rv(&result);
	free_rv(&ynew);
	free_rv(&xnew);
	free_rv(&index_copy);
	free_rv(&angle_copy);
	for (int i = 0; i < 8; i++)
		free_rv(&arg[i]);
	*trace_start = tr.prev;
}

#ifndef SMOKEDRAW_TEST
#define NOW() sceKernelGetProcessTimeLow()
#else
#define NOW() 0u
#endif

static void verify_event(void *self, void *other);

static void ABI smokedraw_event(void *self, void *other) {
	if (smokedraw_ab_off || !admit(self, other)) {
		smokedraw_fallbacks++;
		original(self, other);
		return;
	}
	smokedraw_native_calls++;
	if (smokedraw_verify) {
		verify_event(self, other);
	} else if (smokedraw_prof) {
		uint32_t t0 = NOW();
		native_event(self, other);
		smokedraw_prof_us += NOW() - t0;
		smokedraw_prof_calls++;
	} else {
		native_event(self, other);
	}
}

/*
 * exp_smokeverify (debug builds): both versions run on every cloud, from the same state, and are compared.
 * 1. Snapshot the cloud: every variable slot, instance fields 0x7c..0xc0 (position, image state, flags), RNG.
 * 2. The original runs. Its script calls (update_light_array, inside_screen, update_region, draw_sprite_recolored:
 *    arguments and returned value) and every sprite submission (CSprite::Draw arguments and the instance it draws
 *    for) are recorded. Submissions are not rendered; draw_self/draw_sprite_ext still run down to CSprite::Draw, so
 *    their state effects (the +0x9c frame offset, ms_drawInstance) happen.
 * 3. The snapshot is put back (variable values that own nothing, the fields, the RNG).
 * 4. The native runs with the same recording, rendered normally.
 * 5. Compared: the ordered records, every variable slot (the flags word of ay/ax/angle excepted: the original leaves
 *    stack bytes there), fields 0x7c..0xc0, RNG.
 * State outside the cloud that the original's scripts write (light rdist, close_light_array elements, room-seed
 * scratch fields) is not put back. The second pass can't depend on it: those scripts write each of these before
 * reading it, so the same calls with the same arguments (compared in 5) write the same values, and a script call the
 * native skips or adds shows up in the records.
 * Script calls and CSprite::Draw are recorded through entry hooks installed on the first verified event, after every
 * native has installed its own. An entry another native already hooked (ldr pc,[pc,#-4]; target: visibility,
 * light array) gets the recorder in front: the recorder calls that target, so the other native still runs. An unset or
 * undefined value is recorded by kind only: its value bytes are leftovers (update_light_array returns undefined in a
 * result slot whose high word the original never set).
 */
#define MAX_RECORDS 16
enum { R_SCRIPT = 1, R_SPRITE = 8 };
typedef struct {
	uint32_t kind; // R_SCRIPT + script index, or R_SPRITE
	uint32_t w[44];
} Record;
typedef void ABI (*sprite_draw_fn)(void *sprite, float subimg, float x, float y, float xs, float ys, float rot,
	uint32_t colour, float alpha);
static const char *const script_names[4] = {
	"_Z29gml_Script_update_light_arrayP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z24gml_Script_inside_screenP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z24gml_Script_update_regionP9CInstanceS0_R8YYRValueiPPS1_",
	"_Z32gml_Script_draw_sprite_recoloredP9CInstanceS0_R8YYRValueiPPS1_",
};
static script_fn script_orig[4];
static sprite_draw_fn orig_sprite_draw;
static void **draw_instance;
static uint32_t *rng_state, *rng_index;
static Record records[2][MAX_RECORDS];
static int record_count[2], capture = -1, capture_skip;

static Record *new_record(uint32_t kind) {
	int n = record_count[capture]++;
	if (n >= MAX_RECORDS)
		return NULL; // still counted: an overflow fails the comparison
	Record *r = &records[capture][n];
	memset(r, 0, sizeof(*r));
	r->kind = kind;
	return r;
}
// Value and kind of an argument or result; an unset (0xffffff) or undefined (5) value has no payload
static void record_value(uint32_t *w, const RV *v) {
	uint32_t kind = KIND(v);
	if (kind != 0xffffff && kind != 5)
		memcpy(w, &v->v, 8);
	w[2] = v->kind;
}
static RV *ABI record_script(int k, void *self, void *other, RV *result, int argc, RV **args) {
	Record *r = NULL;
	if (capture >= 0 && (r = new_record(R_SCRIPT + k)) != NULL) {
		r->w[0] = (uint32_t)(uintptr_t)self;
		r->w[1] = (uint32_t)(uintptr_t)other;
		r->w[2] = (uint32_t)argc;
		for (int i = 0; i < argc && i < 8 && args; i++) // value and kind (the flags word: see above)
			record_value(&r->w[3 + 4 * i], args[i]);
	}
	int which = capture;
	RV *out = script_orig[k](self, other, result, argc, args);
	if (r && which == capture) {
		record_value(&r->w[40], out);
		r->w[43] = 1;
	}
	return out;
}
static RV *ABI record_lights(void *s, void *o, RV *r, int c, RV **a) { return record_script(0, s, o, r, c, a); }
static RV *ABI record_inside(void *s, void *o, RV *r, int c, RV **a) { return record_script(1, s, o, r, c, a); }
static RV *ABI record_region(void *s, void *o, RV *r, int c, RV **a) { return record_script(2, s, o, r, c, a); }
static RV *ABI record_recolored(void *s, void *o, RV *r, int c, RV **a) { return record_script(3, s, o, r, c, a); }
static void *const script_recorders[4] = {(void *)record_lights, (void *)record_inside, (void *)record_region,
	(void *)record_recolored};

static void ABI record_sprite_draw(void *sprite, float subimg, float x, float y, float xs, float ys, float rot,
		uint32_t colour, float alpha) {
	if (capture >= 0) {
		Record *r = new_record(R_SPRITE);
		if (r) {
			float f[7] = {subimg, x, y, xs, ys, rot, alpha};
			r->w[0] = (uint32_t)(uintptr_t)sprite;
			r->w[1] = colour;
			memcpy(&r->w[2], f, sizeof(f));
			r->w[9] = (uint32_t)(uintptr_t)*draw_instance;
		}
		if (capture_skip)
			return;
	}
	orig_sprite_draw(sprite, subimg, x, y, xs, ys, rot, colour, alpha);
}

#define MAX_SLOTS 192
typedef struct {
	uint32_t key;
	RV *slot;
	RV value;
} Slot;
// The cloud's variables in table order, or -1 if there are more than MAX_SLOTS
static int snapshot_vars(void *self, Slot *out) {
	const uint8_t *ht = (const uint8_t *)(uintptr_t)word(self, 0x24);
	uint32_t n = word(ht, 8) + 1;
	const uint8_t *entries = (const uint8_t *)(uintptr_t)word(ht, 16);
	int count = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t k = word(entries + 12 * i, 8);
		RV *slot = (RV *)(uintptr_t)word(entries + 12 * i, 0);
		if (k && slot) {
			if (count == MAX_SLOTS)
				return -1;
			out[count].key = k;
			out[count].slot = slot;
			out[count].value = *slot;
			count++;
		}
	}
	return count;
}

#define FIELDS_FROM 0x7c
#define FIELDS_TO 0xc0
static int verify_hooks(void);
static int verify_hooked;
static void verify_event(void *self, void *other) {
	static Slot before[MAX_SLOTS], after_gml[MAX_SLOTS], after_native[MAX_SLOTS];
	if (!verify_hooked && !(verify_hooked = verify_hooks())) {
		smokedraw_verify = 0;
		native_event(self, other);
		return;
	}
	uint8_t fields_before[FIELDS_TO - FIELDS_FROM], fields_gml[FIELDS_TO - FIELDS_FROM];
	uint32_t rng_before[17], rng_gml[17];
	int nb = snapshot_vars(self, before);
	if (nb < 0) { // too many variables to put back: just run the native
		smokedraw_verify_skipped++;
		native_event(self, other);
		return;
	}
	memcpy(fields_before, (uint8_t *)self + FIELDS_FROM, sizeof(fields_before));
	memcpy(rng_before, rng_state, 64);
	rng_before[16] = *rng_index;

	record_count[0] = record_count[1] = 0;
	capture = 0;
	capture_skip = 1;
	original(self, other);
	capture = -1;
	capture_skip = 0;
	int ng = snapshot_vars(self, after_gml);
	memcpy(fields_gml, (uint8_t *)self + FIELDS_FROM, sizeof(fields_gml));
	memcpy(rng_gml, rng_state, 64);
	rng_gml[16] = *rng_index;

	for (int i = 0; i < nb; i++)
		if (!owning(&before[i].value) && !owning(before[i].slot))
			*before[i].slot = before[i].value;
	memcpy((uint8_t *)self + FIELDS_FROM, fields_before, sizeof(fields_before));
	memcpy(rng_state, rng_before, 64);
	*rng_index = rng_before[16];

	capture = 1;
	native_event(self, other);
	capture = -1;

	int nn = snapshot_vars(self, after_native);
	int records_ok = record_count[0] == record_count[1] && record_count[0] <= MAX_RECORDS &&
		!memcmp(records[0], records[1], sizeof(Record) * record_count[0]);
	int fields_ok = !memcmp((uint8_t *)self + FIELDS_FROM, fields_gml, sizeof(fields_gml));
	int rng_ok = !memcmp(rng_state, rng_gml, 64) && *rng_index == rng_gml[16];
	int bad_slot = ng < 0 || ng != nn ? -2 : -1;
	for (int i = 0; bad_slot == -1 && i < ng; i++) {
		const Slot *a = &after_gml[i], *n = &after_native[i];
		int loose = a->key == (uint32_t)id[V_AY] + 1 || a->key == (uint32_t)id[V_AX] + 1 ||
			a->key == (uint32_t)id[V_ANGLE] + 1;
		if (a->key != n->key || a->slot != n->slot || a->value.kind != n->value.kind ||
				memcmp(&a->value.v, &n->value.v, 8) || (!loose && a->value.flags != n->value.flags))
			bad_slot = i;
	}
	if (records_ok && fields_ok && rng_ok && bad_slot == -1) {
		smokedraw_verified++;
		smokedraw_verify_records += record_count[1];
		return;
	}
	if (smokedraw_mismatches++ < 8) {
		debugPrintf("[SMOKEDRAW] mismatch: records %d/%d %s, fields %s, rng %s, variables %s (slot %d key %u)\n",
			record_count[0], record_count[1], records_ok ? "same" : "differ", fields_ok ? "same" : "differ",
			rng_ok ? "same" : "differs", bad_slot == -1 ? "same" : "differ", bad_slot,
			bad_slot >= 0 ? after_gml[bad_slot].key : 0);
		for (int k = 0; k < record_count[0] && k < record_count[1] && k < MAX_RECORDS; k++)
			if (memcmp(&records[0][k], &records[1][k], sizeof(Record)))
				debugPrintf("[SMOKEDRAW]   record %d (kind %u/%u) differs\n", k, records[0][k].kind, records[1][k].kind);
		for (int o = FIELDS_FROM; o < FIELDS_TO; o += 4)
			if (word(self, o) != word(fields_gml, o - FIELDS_FROM))
				debugPrintf("[SMOKEDRAW]   field 0x%x gml %08x native %08x\n", o, word(fields_gml, o - FIELDS_FROM),
					word(self, o));
	}
}

// The function `replacement` should call to reach what entry did: a new trampoline, or the target of a hook already
// at entry (ldr pc,[pc,#-4]; target), which is then redirected to replacement. NULL if entry can't be hooked.
static void *hook_or_chain(uintptr_t entry, void *replacement) {
	if (!entry || (entry & 3))
		return NULL;
	uint32_t *w = (uint32_t *)entry;
	if (w[0] == 0xe51ff004) {
		void *next = (void *)(uintptr_t)w[1];
		uint32_t target = (uint32_t)(uintptr_t)replacement;
		kuKernelCpuUnrestrictedMemcpy(&w[1], &target, 4);
		kuKernelFlushCaches(w, 8);
		return next;
	}
	return entry_hook(entry, replacement);
}

// exp_smokeverify at install: what the comparison needs. 0 if the RNG or the draw instance is missing.
static int verify_install(void) {
	rng_state = (uint32_t *)so_symbol(&yoyoloader_mod, "state");
	rng_index = (uint32_t *)so_symbol(&yoyoloader_mod, "g_RndIndex");
	draw_instance = (void **)so_symbol(&yoyoloader_mod, "_ZN15CSkeletonSprite15ms_drawInstanceE");
	return rng_state && rng_index && draw_instance && so_symbol(&yoyoloader_mod, "_ZN7CSprite4DrawEffffffjf");
}

// On the first verified event: the recording hooks. 0 (verify off) if CSprite::Draw can't be hooked.
static int verify_hooks(void) {
	orig_sprite_draw = (sprite_draw_fn)hook_or_chain(so_symbol(&yoyoloader_mod, "_ZN7CSprite4DrawEffffffjf"),
		(void *)record_sprite_draw);
	if (!orig_sprite_draw) {
		debugPrintf("smoke cloud Draw verify: CSprite::Draw can't be hooked, verify off\n");
		return 0;
	}
	for (int k = 0; k < 4; k++) {
		script_orig[k] = (script_fn)hook_or_chain(so_symbol(&yoyoloader_mod, script_names[k]), script_recorders[k]);
		if (!script_orig[k])
			smokedraw_verify_unrecorded |= 1u << k;
	}
	if (smokedraw_verify_unrecorded)
		debugPrintf("smoke cloud Draw verify: script calls not recorded (entry can't be hooked), mask %x\n",
			smokedraw_verify_unrecorded);
	return 1;
}

void smokedraw_native_report(int frames) {
	if (!installed)
		return;
	if (smokedraw_native_calls || smokedraw_fallbacks)
		debugPrintf("[SMOKEDRAW] native %.1f fallback %.1f per frame%s\n", (float)smokedraw_native_calls / frames,
			(float)smokedraw_fallbacks / frames, smokedraw_ab ? (smokedraw_ab_off ? " | A/B: off" : " | A/B: on") : "");
	for (int i = 0; i < 8 && smokedraw_reject_line[i]; i++)
		debugPrintf("[SMOKEDRAW] reject at smokedraw_native.c:%u x%u\n", smokedraw_reject_line[i],
			smokedraw_reject_count[i]);
	if (smokedraw_prof && smokedraw_prof_calls)
		debugPrintf("[SMOKEDRAW] native event %.3f ms/frame in %.1f calls\n", smokedraw_prof_us / 1000.0f / frames,
			(float)smokedraw_prof_calls / frames);
	if (smokedraw_verify)
		debugPrintf("[SMOKEDRAW] verify: %u events compared (%u script calls and sprite draws), %u mismatches, "
			"%u skipped%s\n", smokedraw_verified, smokedraw_verify_records, smokedraw_mismatches,
			smokedraw_verify_skipped, smokedraw_verify_unrecorded ? ", some scripts not recorded" : "");
	smokedraw_native_calls = smokedraw_fallbacks = smokedraw_prof_us = smokedraw_prof_calls = 0;
	memset(smokedraw_reject_line, 0, sizeof(smokedraw_reject_line));
	memset(smokedraw_reject_count, 0, sizeof(smokedraw_reject_count));
	if (smokedraw_ab)
		smokedraw_ab_off = !smokedraw_ab_off;
}

// The GOT slot a PLT stub jumps through (add ip, pc, #a; add ip, ip, #b; ldr pc, [ip, #c]!), or NULL
static uintptr_t *plt_slot(uintptr_t stub) {
	const uint32_t *w = (const uint32_t *)stub;
	if ((w[0] & 0xfffff000) != 0xe28fc000 || (w[1] & 0xfffff000) != 0xe28cc000 || (w[2] & 0xfffff000) != 0xe5bcf000)
		return NULL;
	uint32_t a = w[0] & 0xff, ra = ((w[0] >> 8) & 0xf) * 2, b = w[1] & 0xff, rb = ((w[1] >> 8) & 0xf) * 2;
	a = ra ? (a >> ra) | (a << (32 - ra)) : a;
	b = rb ? (b >> rb) | (b << (32 - rb)) : b;
	return (uintptr_t *)(stub + 8 + a + b + (w[2] & 0xfff));
}

// Target of the `bl` at site, or 0
static uintptr_t bl_target(uintptr_t site) {
	uint32_t ins = *(const uint32_t *)site;
	if ((ins & 0xff000000) != 0xeb000000)
		return 0;
	int32_t off = (int32_t)(ins << 8) >> 6;
	return site + 8 + off;
}

int smokedraw_native_install(void *(*rx_alloc)(size_t)) {
	if (installed || !rx_alloc)
		return 0;
#define RESOLVE(v, t, n) do { v = (t)so_symbol(&yoyoloader_mod, n); if (!v) return 0; } while (0)
	uintptr_t entry;
	RESOLVE(entry, uintptr_t, "_Z32gml_Object_obj_smokecloud_Draw_0P9CInstanceS0_");
	if ((entry & 3) || entry < EVENT_ADDR)
		return 0;
	uintptr_t bias = entry - EVENT_ADDR;
	if (word((void *)entry, 0) != 0xe92d4ff0 || word((void *)entry, 4) != 0xe28db01c)
		return 0;
	// The library is the audited one: every call site is a bl to the expected function (direct, as after
	// patch_plt_calls.py, or its PLT stub)
	for (int i = 0; i < NCALLS; i++) {
		uintptr_t target = bl_target(bias + call_sites[i]);
		if (!target)
			return 0;
		if (call_symbols[i]) {
			uintptr_t sym = so_symbol(&yoyoloader_mod, call_symbols[i]);
			if (!sym)
				return 0;
			if (target != sym) { // the symbol's own PLT stub
				if (target < bias + PLT_START || target >= bias + PLT_START + PLT_SIZE)
					return 0;
				uintptr_t *slot = plt_slot(target);
				if (!slot || slot != so_got_slot(&yoyoloader_mod, call_symbols[i]))
					return 0;
			}
		} else if (target != bias + local_targets[i]) {
			return 0;
		}
		calls[i] = target;
	}
	if (word((void *)calls[C_COPY], 0) != 0xe92d4c10 || word((void *)calls[C_COPY], 4) != 0xe28db008)
		return 0;
	for (unsigned i = 0; i < NBYPASSED; i++) {
		RESOLVE(bypassed_addr[i], uintptr_t, bypassed[i].name);
		if (bypassed_addr[i] != bias + bypassed[i].addr)
			return 0;
		bypassed_got[i] = so_got_slot(&yoyoloader_mod, bypassed[i].name);
	}
	standard_get = (var_fn)bypassed_addr[B_GET];
	standard_ref = (var_fn)bypassed_addr[B_REF];
	RESOLVE(global_obj, void **, "g_pGlobal");
	RESOLVE(epsilon, const double *, "g_GMLMathEpsilon");
	RESOLVE(trace_start, Trace **, "_ZN13SYYStackTrace8s_pStartE");
	RESOLVE(builtin_count, const int *, "g_nStartGlobalVariables");
	RESOLVE(builtin_variables, const uint8_t *, "builtin_variables");
	RESOLVE(managed_objects, const uint8_t *, "g_enableManagedObjects");
	RESOLVE(garbage_collection, const uint8_t *, "g_fGarbageCollection");
	RESOLVE(context_stack, void ***, "g_ContextStack");
	RESOLVE(context_top, int *, "g_ContextStackTop");
	RESOLVE(context_max, int *, "g_ContextStackMax");
	RESOLVE(frame_counter, const uint32_t *, "g_IOFrameCount");
	RESOLVE(const_false, const RV *, "gs_constFalse");
	RESOLVE(const_50, const RV *, "gs_constArg0_C48BBD16");
	char name[64];
	for (int n = 0; n < NVARS; n++) {
		strcpy(name, "g_VAR_");
		strcat(name, var_names[n]);
		RESOLVE(vars[n], const int32_t *, name);
	}
#undef RESOLVE
	void *rx = rx_alloc(16);
	if (!rx || ((uintptr_t)rx & 3))
		return 0;
	uint32_t code[4] = {0xe92d4ff0, 0xe28db01c, 0xe51ff004, (uint32_t)(entry + 8)};
	kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof(code));
	kuKernelFlushCaches(rx, sizeof(code));
	original = (event_fn)rx;
	// Debug only: the A/B switch flips in the profiling report, which runs with debugMode
	smokedraw_ab = debugMode && exp_flag("smokeab");
	smokedraw_prof = debugMode && exp_flag("smokeprof");
	if (debugMode && exp_flag("smokeverify")) {
		smokedraw_verify = verify_install();
		debugPrintf("smoke cloud Draw verify: %s\n", smokedraw_verify ? "on" : "NOT available");
	}
	hook_addr(entry, (uintptr_t)smokedraw_event);
	kuKernelFlushCaches((void *)entry, 8);
	installed = 1;
	return 1;
}
