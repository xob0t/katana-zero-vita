/*
 * smokedraw_native.c
 *
 * Exact native obj_smokecloud Draw_0 (YYC event at 0x0490421c in libyoyo SHA256
 * ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701). club_4's broken pipe keeps 84 clouds alive and
 * their Draw cost 3.7 ms per frame. Much of that is the event's own variable traffic: about twenty hash lookups,
 * a dozen YYGML_Variable_GetValue -> Variable_GetValue_Direct -> GV_* chains, nine Variable_SetValue_Direct -> SV_*
 * chains, tagged operators and compares, and the copy/free bookkeeping of thirty stack RValues.
 *
 * Ported from the ARM listing, which differs from the readable lift (untracked work/decomp/out) in three places:
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
 * Visual settings (yyl.cfg; they need this native, so exp_smokedraw0 also restores the original look):
 * - smokeClouds=N (1..3, default 2; 1 = original; larger values count as 3): only clouds whose instance id is a
 *   multiple of N submit sprites, at sqrt(N) times their size, so the smoke covers about the same area with 1/N of the sprites (a burst from a smoke
 *   vial is 131 clouds). The other clouds still run the whole event except draw_sprite_recolored and the sprite
 *   submissions: update_light_array, inside_screen (in_screen gates the cloud's contacts in its Step), the
 *   x/y/image_index/image_angle/image_blend/image_alpha writes and every variable write happen as before (a skipped
 *   draw_self still does what draw_self does besides submitting: if Sprite_Data(sprite_index) exists, the +0x9c
 *   frame offset is consumed and ms_drawInstance ends at 0), and collision masks, positions, lifetimes and
 *   the random state are untouched. The drawn size goes through the draw arguments (draw_self: the instance's scale
 *   fields for the call, put back right after; raw field writes, so no bounding-box update). What it skips only
 *   writes draw_sprite_recolored's light variables on the cloud (light_col/r/g/b/alpha, prev_recolor_timer_val),
 *   which only that script and the base layer's colour read, and the rdist scratch value of the lights it visits
 *   (written before every read).
 * exp_smokelookab (debugMode) switches it off and on every report window ([SMOKELOOK] in the profiling report).
 * exp_smokeverify + exp_smokelookverify compares the original with the native at smokeClouds: everything above
 * except the submissions, draw_sprite_recolored's call and those light variables. smokeClouds is clamped to 1..3:
 * a large divisor would hide almost every cloud.
 *
 * smokeWobble (yyl.cfg, default 1; 0 = original): the wobble without the collision system. The event moves the cloud
 * by its wobble (lines 25/27), turns it to `angle` (32), back to 0 (72) or to `angle` again (77), and puts x and y
 * back (86/87). Through the setters (SV_X/SV_Y -> CInstance::SetPosition, SV_ImageAngle -> SetImageAngle) every one
 * of those writes sets the bounding-box flag (+0x7c bit 3) and calls CollisionMarkDirty, so each cloud on screen is
 * pushed onto the collision dirty list every frame and the next tree query's UpdateTree recomputes its box and
 * removes and reinserts it: ~2 ms per frame for a smoke vial's 131 clouds on the Vita (setters and tree updates).
 * The box (CInstance::Compute_BoundingBox) depends on x, y, the scales, image_angle and the sprite or mask, not on
 * image_index; at the end x and y hold the same floats as before, and image_angle changes for good only when it ends
 * at another value (the cloud's first frame on screen at a non-zero angle, the slowdown branch starting or ending).
 * With smokeWobble=1 these writes are the setters' field writes alone: (float) of the value, written if it differs as
 * a float (SetPosition / SetImageAngle compare first), and for image_angle the +0x7c bit 14 "plain draw" flag
 * recomputed as SetImageAngle does (blend, image_alpha and draw_self read it). Nothing in between reads the cloud's
 * box: the scripts and draws read the x/y fields (update_region, draw_sprite_recolored's light distances, the draw
 * arguments), which hold the same values as before. The one tree query that can run in between,
 * draw_sprite_recolored's update_light_array(true) (close_lights == -1 only), tests obj_door, not clouds. If
 * image_angle ends at another value, bit 3 is set and CollisionMarkDirty called, as SetImageAngle does. So the cloud's box, its rectangle in the tree, the dirty lists' other entries and
 * every variable and field come out as before; what differs is that a cloud whose box did not change is not taken
 * out of the tree and put back each frame, so the order of the tree's leaves (which instance_place meets first when
 * several instances overlap) is not reshuffled by the clouds. Used only for an ordinary CInstance without a skeleton
 * (SetPosition calls SkeletonAnimation, which could create one), with the audited setters in the builtin table.
 * exp_smokewobbleab (debugMode) switches it off and on every report window ([SMOKEWOBBLE]). exp_smokeverify +
 * exp_smokewobbleverify compares the original with the native at smokeWobble=1 (with exp_smokelookverify too, at
 * smokeClouds): everything the exact verify compares, except that with an unchanged angle the native must leave the
 * collision lists and the cloud's links as they were before the event (the original moves the cloud to the front of
 * them) and bit 3 as it was; the box fields (+0xec..+0xf8) must be untouched by both passes, and when bit 3 was clear
 * before the event, the box Compute_BoundingBox gives for the original's final state must be the box the cloud holds.
 * Compute_BoundingBox calls during either pass are recorded with the script calls. exp_smoketreecheck (debugMode)
 * walks the collision trees once a frame: every leaf of an instance with nothing pending (bit 3 clear, not on the
 * dirty list) must hold the instance's box, with each instance once ([SMOKETREE] line). Negative controls:
 * exp_smokeverifyfault3 marks every unmoved cloud (lists), exp_smokeverifyfault4 leaves the angle turned and unmarked
 * (fields, box). exp_smokecollog (smoke_collog.c) logs what the clouds' place_meeting/instance_place return.
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

#ifndef SMOKEDRAW_TEST
int smoke_clouds = 2;    // smokeClouds (yyl.cfg): one cloud in N draws, sqrt(N) times larger; 1 = original
#else
int smoke_clouds = 1;    // the fixture checks the exact event
#endif
static int look_ab, look_off, look_verify;
static float draw_scale = 1.0f, cloud_scale = 1.0f; // cloud_scale: sqrt(smokeClouds), set at install
static unsigned look_drawn, look_hidden;

#ifndef SMOKEDRAW_TEST
int smoke_wobble = 1; // smokeWobble (yyl.cfg): the wobble's x/y/image_angle writes without the collision system
#else
int smoke_wobble = 0; // the fixture switches it on for its wobble cases
#endif
static int wobble_ab, wobble_off, wobble_verify, wobble_ok;
static int raw, raw_marked;   // this event: wobble as field writes; image_angle changed, so the cloud was marked
static uint32_t raw_angle0;   // image_angle's bits when the event started
static int verify_fault;
static unsigned wobble_raw, wobble_exact, wobble_marked;
// What the setters run, which the field writes stand in for: must be the audited code (no entry or GOT hook)
enum { W_SV_X, W_SV_Y, W_SV_ANGLE, W_SETPOS, W_SETANGLE, W_MARK, W_SKELETON, NWOBBLE };
static const struct {
	const char *name;
	uint32_t addr, w0, w1;
} wobble_fns[NWOBBLE] = {
	{"_Z4SV_XP9CInstanceiP6RValue", 0xf84e6c, 0xe92d4c10, 0xe28db008},
	{"_Z4SV_YP9CInstanceiP6RValue", 0xf84ebc, 0xe92d4c10, 0xe28db008},
	{"_Z13SV_ImageAngleP9CInstanceiP6RValue", 0xf85678, 0xe92d4c10, 0xe28db008},
	{"_ZN9CInstance11SetPositionEff", 0xf297d4, 0xe92d4c10, 0xe28db008},
	{"_ZN9CInstance13SetImageAngleEf", 0xf27260, 0xe92d4c10, 0xe28db008},
	{"_Z18CollisionMarkDirtyP9CInstance", 0xf6bf40, 0xe5901068, 0xe3510000},
	{"_ZN9CInstance17SkeletonAnimationEv", 0xf2673c, 0xe92d4830, 0xe28db008},
};
static uintptr_t wobble_addr[NWOBBLE];
static uintptr_t *wobble_got[NWOBBLE];
typedef void ABI (*inst_fn)(void *inst);

// draw_self for this cloud: nothing if hidden, else at draw_scale times the instance's size (its scale fields for
// the call, then the same bits back)
static int hidden;
static void *(*sprite_data)(int sprite); // Sprite_Data, resolved at install
static void **draw_instance;           // CSkeletonSprite::ms_drawInstance
static void draw_cloud(void *self) {
	if (hidden) {
		// What YYGML_draw_self leaves besides the submission: nothing if Sprite_Data(sprite_index) is NULL; otherwise
		// the frame offset at +0x9c consumed (CInstance::GetImageIndex, called before that, only reads) and
		// ms_drawInstance, set for the submission, back at 0
		if (sprite_data(*(const int32_t *)((const uint8_t *)self + 0x88))) { // sprite_index
			memset((uint8_t *)self + 0x9c, 0, 4);
			*draw_instance = NULL;
		}
		return;
	}
	float k = draw_scale;
	if (k == 1.0f) {
		((draw_self_fn)calls[C_DRAW_SELF])(self);
		return;
	}
	uint8_t *inst = (uint8_t *)self;
	float xs, ys, sxs, sys;
	memcpy(&xs, inst + 0xa4, 4);
	memcpy(&ys, inst + 0xa8, 4);
	sxs = xs * k;
	sys = ys * k;
	memcpy(inst + 0xa4, &sxs, 4);
	memcpy(inst + 0xa8, &sys, 4);
	((draw_self_fn)calls[C_DRAW_SELF])(self);
	memcpy(inst + 0xa4, &xs, 4);
	memcpy(inst + 0xa8, &ys, 4);
}

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
static inline void put_field(void *inst, size_t o, float v) { memcpy((uint8_t *)inst + o, &v, 4); }
static inline void put_word(void *inst, size_t o, uint32_t v) { memcpy((uint8_t *)inst + o, &v, 4); }

// smokeWobble: an x, y or image_angle write of the wobble. The setter, or (raw) the fields it writes without the
// bounding-box flag and CollisionMarkDirty:
// - SV_X / SV_Y / SV_ImageAngle: (float) of the value, REAL_RValue_Ex unless the kind is 0;
// - CInstance::SetPosition(x, y): nothing if both equal the fields as floats, else both fields written;
// - CInstance::SetImageAngle(a): nothing if it equals the field as a float, else the field and +0x7c bit 14 (set if
//   image_alpha is 1, image_blend 0xffffff, both scales 1 and the angle 0). A NaN angle (the runner's YYError) takes
//   the setter.
static void wset(void *self, int n, RV *value) {
	if (!raw) {
		bset(self, n, value);
		return;
	}
	double d = KIND(value) == 0 ? value->v.d : ((real_fn)calls[C_REAL])(value);
	float f = (float)d;
	if (n == V_IMAGE_ANGLE) {
		if (field(self, 0xac) == f)
			return;
		if (f != f) {
			bset(self, n, value);
			return;
		}
		uint32_t flags = word(self, 0x7c) & ~0x4000u;
		if (field(self, 0xb0) == 1.0f && word(self, 0xb4) == 0xffffff && field(self, 0xa8) == 1.0f &&
				field(self, 0xa4) == 1.0f && f == 0.0f)
			flags |= 0x4000;
		put_field(self, 0xac, f);
		put_word(self, 0x7c, flags);
		return;
	}
	float x = n == V_X ? f : field(self, 0xb8), y = n == V_Y ? f : field(self, 0xbc);
	if (field(self, 0xb8) == x && field(self, 0xbc) == y)
		return;
	put_field(self, 0xb8, x);
	put_field(self, 0xbc, y);
}
// Whether this event's wobble can be field writes: the setters are the audited ones (validate) and the cloud has no
// skeleton and won't get one (CInstance::SkeletonAnimation, which SetPosition calls, creates one for a sprite of
// type 2 when +0x70 is NULL)
static inline int wobble_raw_ok(void *self) {
	if (word(self, 0x70))
		return 0;
	const void *sprite = sprite_data((int32_t)word(self, 0x88));
	return !sprite || word(sprite, 0x84) != 2;
}
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
	// smokeWobble: the setters in the table and the runner code they run are the audited ones (else the setters run)
	wobble_ok = sprite_data && set_fn[V_X] == (bvar_fn)wobble_addr[W_SV_X] &&
		set_fn[V_Y] == (bvar_fn)wobble_addr[W_SV_Y] && set_fn[V_IMAGE_ANGLE] == (bvar_fn)wobble_addr[W_SV_ANGLE];
	for (unsigned i = 0; i < NWOBBLE && wobble_ok; i++) {
		const uint32_t *code = (const uint32_t *)wobble_addr[i];
		wobble_ok = code && code[0] == wobble_fns[i].w0 && code[1] == wobble_fns[i].w1 &&
			(!wobble_got[i] || *wobble_got[i] == wobble_addr[i]);
	}
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

	// smokeWobble: the wobble as field writes; what x, y and image_angle hold now
	raw = smoke_wobble && wobble_ok && !wobble_off && (!smokedraw_verify || wobble_verify) && wobble_raw_ok(self);
	raw_marked = 0;
	uint32_t x0 = word(self, 0xb8), y0 = word(self, 0xbc);
	raw_angle0 = word(self, 0xac);

	// smokeClouds: whether this cloud makes its submissions (every other effect stays)
	hidden = 0;
	draw_scale = 1.0f;
	if (!look_off && (!smokedraw_verify || look_verify)) {
		if (smoke_clouds > 1) {
			if (word(self, 0x80) % (unsigned)smoke_clouds)
				hidden = 1;
			else
				draw_scale = cloud_scale;
		}
	}
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
	wset(self, V_Y, &ynew);

	tr.line = 26;
	RV *ax = ref(self, V_AX);
	bget_field(self, 0xb8, &b);
	store_var(self, ax, &b);

	tr.line = 27;
	RV *hoffset = get_std(self, V_HOFFSET);
	bget_field(self, 0xb8, &b);
	RV xnew;
	offset_coord(&b, hoffset, 2, &xnew);
	wset(self, V_X, &xnew);

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
		wset(self, V_IMAGE_ANGLE, &angle_copy);

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
			if (draw_scale != 1.0f) { // the light layer at the drawn size
				img[4].v.d = (double)(field(self, 0xa4) * draw_scale);
				img[5].v.d = (double)(field(self, 0xa8) * draw_scale);
			}
			bget_field(self, 0xac, &img[6]);
			for (int i = 0; i < 7; i++) {
				img[i].flags = 0;
				assign_rv(&arg[i], &img[i]);
			}
			assign_rv(&arg[7], const_false);
			if (!BUG(4) && !hidden)
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
				draw_cloud(self);
				tr.line = 52;
				blend = real(0.0);
				bset(self, V_IMAGE_BLEND, &blend);
				tr.line = 53;
				alpha = real(0.75);
				bset(self, V_IMAGE_ALPHA, &alpha);
				tr.line = 54;
				draw_cloud(self);
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
				if (!hidden)
					((draw_ext_fn)calls[C_DRAW_EXT])(self, sprite, (float)subimg, x, y, xs * draw_scale, ys * draw_scale,
						rot, colour, (float)(BUG(5) ? a.v.d * 0.5 : a.v.d));
			}
		} else {
			tr.line = 69;
			RV *ra = get_std(self, V_REAL_ALPHA);
			alpha_product(ra, get(other, V_ALPHA), &alpha);
			bset(self, V_IMAGE_ALPHA, &alpha);
			tr.line = 70;
			draw_cloud(self);
		}

		tr.line = 72;
		free_rv(&angle_copy);
		angle_copy = real(0.0);
		if (!(raw && verify_fault == 4)) // test only: the angle not put back
			wset(self, V_IMAGE_ANGLE, &angle_copy);
		tr.line = 75;
		RV one = real(1.0);
		int s = compare(g_slowdown, &one, 1);
		if (s != -2 && s <= -1 && truth(get_std(self, V_SLOWDOWN))) {
			tr.line = 77;
			assign_rv(&angle_copy, get_std(self, V_ANGLE));
			wset(self, V_IMAGE_ANGLE, &angle_copy);
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
			draw_cloud(self);
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
	wset(self, V_Y, &back);
	free_rv(&back);
	tr.line = 87;
	{
		RV tmp = *get_std(self, V_AX);
		copy_rv(&back, &tmp);
	}
	if (!BUG(3))
		wset(self, V_X, &back);
	free_rv(&back);
	if (raw) {
		// A position or angle that ends elsewhere: the box changes, so the cloud is marked as the setters mark it
		if ((word(self, 0xb8) != x0 || word(self, 0xbc) != y0 || word(self, 0xac) != raw_angle0) && verify_fault != 4) {
			put_word(self, 0x7c, word(self, 0x7c) | 8);
			((inst_fn)wobble_addr[W_MARK])(self);
			raw_marked = 1;
			wobble_marked++;
		} else if (verify_fault == 3) { // test only: marked although nothing moved
			((inst_fn)wobble_addr[W_MARK])(self);
		}
		wobble_raw++;
	} else {
		wobble_exact++;
	}
	free_rv(&result);
	free_rv(&ynew);
	free_rv(&xnew);
	free_rv(&index_copy);
	free_rv(&angle_copy);
	for (int i = 0; i < 8; i++)
		free_rv(&arg[i]);
	if (hidden)
		look_hidden++;
	else
		look_drawn++;
	*trace_start = tr.prev;
}

#ifndef SMOKEDRAW_TEST
#define NOW() sceKernelGetProcessTimeLow()
#else
#define NOW() 0u
#endif

static void verify_event(void *self, void *other);

/*
 * exp_smoketreecheck (debugMode): once a frame, before the first cloud's Draw, every collision tree is walked. Each leaf
 * entry of an instance with nothing pending (+0x7c bit 3 clear, not on the dirty list) must hold the rectangle
 * CollisionUpdate makes from the instance's box (min and max of the corners, its compares); no instance may be in the
 * trees twice. Counted for clouds and for everything else ([SMOKETREE]).
 */
static int tree_check;
static void **collision_trees;
static uintptr_t dirty_sentinel;
static int cloud_object = -2;
static int (*object_find)(const char *name);
static uint32_t tree_check_frame;
static unsigned tc_frames, tc_entries, tc_clouds, tc_pending, tc_bad_cloud, tc_bad_other, tc_dup, tc_broken;
#define TC_SET 4096
static uintptr_t tc_seen[TC_SET];
static int tc_insert(uintptr_t p) {
	uint32_t h = ((uint32_t)p >> 3) * 2654435761u;
	for (int k = 0; k < TC_SET; k++) {
		uintptr_t *e = &tc_seen[(h + k) & (TC_SET - 1)];
		if (*e == p)
			return 0;
		if (!*e) {
			*e = p;
			return 1;
		}
	}
	return 1; // full: not checked
}
static void check_trees(void) {
	if (cloud_object == -2)
		cloud_object = object_find ? object_find("obj_smokecloud") : -1;
	memset(tc_seen, 0, sizeof(tc_seen));
	tc_frames++;
	for (int t = 0; t < 3; t++) {
		const uint8_t *tree = (const uint8_t *)collision_trees[t];
		if (!tree)
			continue;
		const uint8_t *stack[64];
		int sp = 0;
		const uint8_t *root = (const uint8_t *)(uintptr_t)word(tree, 0x14);
		if (root)
			stack[sp++] = root;
		while (sp) {
			const uint8_t *node = stack[--sp];
			uint32_t count = word(node, 0), level = word(node, 4);
			if (count > 6 || level > 16) {
				tc_broken++;
				break;
			}
			for (uint32_t i = 0; i < count; i++) {
				const uint8_t *b = node + 8 + 0x18 * i;
				if (level) {
					if (sp < 64)
						stack[sp++] = (const uint8_t *)(uintptr_t)word(b, 0x10);
					else
						tc_broken++;
					continue;
				}
				const uint8_t *inst = (const uint8_t *)(uintptr_t)word(b, 0x14);
				tc_entries++;
				int cloud = (int32_t)word(inst, 0x84) == cloud_object;
				tc_clouds += cloud;
				if (!tc_insert((uintptr_t)inst))
					tc_dup++;
				if ((word(inst, 0x7c) & 8) || word(inst, 0x170) == dirty_sentinel) {
					tc_pending++;
					continue;
				}
				// CollisionUpdate (0xf6c4e4..): x from +0xec/+0xf4, y from +0xf0/+0xf8
				float x0 = field(inst, 0xec), y0 = field(inst, 0xf0), x1 = field(inst, 0xf4), y1 = field(inst, 0xf8);
				float r[4] = {x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1, y0 > y1 ? y0 : y1};
				if (memcmp(r, b, 16)) {
					if (cloud)
						tc_bad_cloud++;
					else
						tc_bad_other++;
				}
			}
		}
	}
}

#ifndef SMOKEDRAW_TEST
void smoke_collog_enable(void), smoke_collog_frame(void), smoke_collog_report(int frames); // smoke_collog.c
#else
static void smoke_collog_enable(void) {}
static void smoke_collog_frame(void) {}
static void smoke_collog_report(int frames) { (void)frames; }
#endif
static int collog;

static void ABI smokedraw_event(void *self, void *other) {
	if (collog)
		smoke_collog_frame();
	if (tree_check && *frame_counter != tree_check_frame) {
		tree_check_frame = *frame_counter;
		check_trees();
	}
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
 *    Also snapshotted: the runner's collision lists (s_dirty_list, s_test_list: every node in order with its links,
 *    the sentinels' first/last, the cloud's own links at +0x15c..+0x174) and the elements of the cloud's array
 *    variables (close_light_array).
 * 3. The snapshot is put back (variable values that own nothing, the fields, the RNG, array elements that own
 *    nothing when the array is the same one at the same length). The collision lists go back too when the original
 *    only moved the cloud's own nodes (the x/y and image_angle setters' CollisionMarkDirty takes the cloud out of each
 *    list and pushes it to the front), so a native that skipped that update would leave them differing; otherwise
 *    (another node added or removed, a list over MAX_LIST) they are left as the original left them and not compared
 *    ("lists unchecked" in the report).
 * 4. The native runs with the same recording, rendered normally.
 * 5. Compared: the ordered records, every variable slot (the flags word of ay/ax/angle excepted: the original leaves
 *    stack bytes there), fields 0x7c..0xc0, RNG, the collision lists and the cloud's links, array elements.
 * State outside the cloud that the original's scripts write (light rdist, room-seed scratch fields) is not put back.
 * The second pass can't depend on it: those scripts write each of these before reading it, so the same calls with the
 * same arguments (compared in 5) write the same values, and a script call the native skips or adds shows up in the
 * records. update_light_array rewrites close_light_array's entries with the same values each call, so a skipped call
 * shows in the records, not in the elements; the element compare catches different values written.
 * exp_smokeverifyfault1 (negative control) puts the collision lists back after the native pass, as a native that
 * skipped CollisionMarkDirty would leave them; exp_smokeverifyfault2 flips a value bit of the first element of an
 * array variable. Every event with a moved node, or with an array element, must then count as a mismatch.
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
// exp_smokelookverify: the comparison with smokeClouds on. Everything is compared except the
// submissions, draw_sprite_recolored's call and the cloud variables only it writes.
static const char *const light_var_names[6] = {"g_VAR_light_col", "g_VAR_light_r", "g_VAR_light_g", "g_VAR_light_b",
	"g_VAR_light_alpha", "g_VAR_prev_recolor_timer_val"};
static const int32_t *light_vars[6];
static int is_light_var(uint32_t key) {
	for (int i = 0; i < 6; i++)
		if (light_vars[i] && key == (uint32_t)light_vars[i][1] + 1)
			return 1;
	return 0;
}
// The runner's collision lists, as CollisionMarkDirty keeps them: circular through the sentinel {first, last, node
// offset in the instance}; a node is {next, prev, owning list (sentinel or 0)}
#define MAX_LIST 512 // a longer list is reported as unchecked
typedef struct { uintptr_t node, w[3]; } ListNode;
typedef struct { uintptr_t head[2]; int n; ListNode nodes[MAX_LIST]; } ListSnap;
static uintptr_t *coll_list[2]; // s_dirty_list, s_test_list
#define SELF_LINKS 0x15c        // the cloud's test-list node (+0x15c) and dirty-list node (+0x168)
#define SELF_LINKS_SIZE 24
unsigned smokedraw_verify_lists_unchecked, smokedraw_verify_arrays_unchecked;
unsigned smokedraw_verify_nodes, smokedraw_verify_arrays, smokedraw_verify_elements; // compared (session)

// The list in order; 0 if it is longer than MAX_LIST or a link is NULL
static int list_take(int l, ListSnap *s) {
	const uintptr_t *sentinel = coll_list[l];
	s->head[0] = sentinel[0];
	s->head[1] = sentinel[1];
	s->n = 0;
	uintptr_t n = sentinel[0];
	while (n != (uintptr_t)sentinel) {
		if (!n || s->n == MAX_LIST)
			return 0;
		ListNode *e = &s->nodes[s->n++];
		e->node = n;
		memcpy(e->w, (const void *)n, sizeof(e->w));
		n = e->w[0];
	}
	return 1;
}
// Whether a and b hold the same nodes in the same order once `skip` (the cloud's own node) is left out
static int same_others(const ListSnap *a, const ListSnap *b, uintptr_t skip) {
	int i = 0, j = 0;
	for (;;) {
		while (i < a->n && a->nodes[i].node == skip)
			i++;
		while (j < b->n && b->nodes[j].node == skip)
			j++;
		if (i == a->n || j == b->n)
			return i == a->n && j == b->n;
		if (a->nodes[i++].node != b->nodes[j++].node)
			return 0;
	}
}
static void list_restore(int l, const ListSnap *s) {
	for (int i = 0; i < s->n; i++)
		memcpy((void *)s->nodes[i].node, s->nodes[i].w, sizeof(s->nodes[i].w));
	coll_list[l][0] = s->head[0];
	coll_list[l][1] = s->head[1];
}
static int list_same(const ListSnap *a, const ListSnap *b) {
	return a->n == b->n && a->head[0] == b->head[0] && a->head[1] == b->head[1] &&
		!memcmp(a->nodes, b->nodes, sizeof(ListNode) * a->n);
}

// Elements of the cloud's array variables (a 1-D array: header +4 items, +0x1c length)
#define MAX_ARRAYS 4
#define MAX_ELEMS 64
typedef struct { int slot; uintptr_t header; uint32_t n; RV e[MAX_ELEMS]; } ArrayCopy;
typedef struct { int n; ArrayCopy a[MAX_ARRAYS]; } Arrays;
// 0 if there are more arrays or elements than fit
static int arrays_take(const Slot *slots, int count, Arrays *out) {
	out->n = 0;
	for (int i = 0; i < count; i++) {
		if (KIND(&slots[i].value) != 2)
			continue;
		const void *header = (const void *)(uintptr_t)word(&slots[i].value, 0);
		if (!header || out->n == MAX_ARRAYS)
			return 0;
		ArrayCopy *a = &out->a[out->n++];
		a->slot = i;
		a->header = (uintptr_t)header;
		a->n = word(header, 0x1c);
		const RV *items = (const RV *)(uintptr_t)word(header, 4);
		if (a->n > MAX_ELEMS || (a->n && !items))
			return 0;
		memcpy(a->e, items, sizeof(RV) * a->n);
	}
	return 1;
}
// Put back elements that own nothing (in the snapshot and now), if the variable still holds the same array
static void arrays_restore(const Slot *slots, const Arrays *s) {
	for (int k = 0; k < s->n; k++) {
		const ArrayCopy *a = &s->a[k];
		const RV *v = slots[a->slot].slot;
		if (KIND(v) != 2 || word(v, 0) != a->header || word((const void *)a->header, 0x1c) != a->n)
			continue;
		RV *items = (RV *)(uintptr_t)word((const void *)a->header, 4);
		for (uint32_t i = 0; i < a->n; i++)
			if (!owning(&a->e[i]) && !owning(&items[i]))
				items[i] = a->e[i];
	}
}
// Value and kind (an unset or undefined element by kind only), as the records compare them
static int same_element(const RV *a, const RV *b) {
	uint32_t kind = KIND(a);
	return kind == KIND(b) && (kind == 0xffffff || kind == 5 || !memcmp(&a->v, &b->v, 8));
}
static int arrays_same(const Arrays *a, const Arrays *b) {
	if (a->n != b->n)
		return 0;
	for (int k = 0; k < a->n; k++) {
		const ArrayCopy *x = &a->a[k], *y = &b->a[k];
		if (x->slot != y->slot || x->header != y->header || x->n != y->n)
			return 0;
		for (uint32_t i = 0; i < x->n; i++)
			if (!same_element(&x->e[i], &y->e[i]))
				return 0;
	}
	return 1;
}

// smokeWobble verify: CInstance::Compute_BoundingBox calls for the verified cloud during each pass (recorded), and
// the box check (Compute_BoundingBox(false) on the final state: box and +0x7c written, no tree update; both put back)
typedef void ABI (*bbox_fn)(void *inst, int update);
static bbox_fn compute_bbox, orig_compute_bbox;
static void *verify_self;
static unsigned bbox_calls[2];
unsigned smokedraw_wobble_events, smokedraw_wobble_unmoved, smokedraw_wobble_box_checked, smokedraw_wobble_box_bad;
unsigned smokedraw_wobble_flag_bad, smokedraw_wobble_box_window, smokedraw_wobble_list_bad;
static void ABI record_bbox(void *inst, int update) {
	if (capture >= 0 && inst == verify_self)
		bbox_calls[capture]++;
	orig_compute_bbox(inst, update);
}
#define BOX 0xec // the bounding box, 4 floats

static void verify_event(void *self, void *other) {
	static Slot before[MAX_SLOTS], after_gml[MAX_SLOTS], after_native[MAX_SLOTS];
	static ListSnap lists_before[2], lists_gml[2], lists_native[2], lists_real[2];
	static Arrays arrays_before, arrays_gml, arrays_native;
	uint8_t links_before[SELF_LINKS_SIZE], links_gml[SELF_LINKS_SIZE], links_real[SELF_LINKS_SIZE];
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
	memcpy(links_before, (uint8_t *)self + SELF_LINKS, sizeof(links_before));
	int lists_ok = list_take(0, &lists_before[0]) && list_take(1, &lists_before[1]);
	int arrays_ok = arrays_take(before, nb, &arrays_before);
	uint8_t box_before[16], box_gml[16];
	memcpy(box_before, (uint8_t *)self + BOX, 16);
	verify_self = self;
	bbox_calls[0] = bbox_calls[1] = 0;

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
	memcpy(links_gml, (uint8_t *)self + SELF_LINKS, sizeof(links_gml));
	memcpy(box_gml, (uint8_t *)self + BOX, 16);
	// The lists go back only if the original moved nothing but the cloud's own nodes
	for (int l = 0; l < 2 && lists_ok; l++)
		lists_ok = list_take(l, &lists_gml[l]) &&
			same_others(&lists_before[l], &lists_gml[l], (uintptr_t)self + coll_list[l][2]);
	if (ng < 0 || !arrays_take(after_gml, ng, &arrays_gml))
		arrays_ok = 0;

	for (int i = 0; i < nb; i++)
		if (!owning(&before[i].value) && !owning(before[i].slot))
			*before[i].slot = before[i].value;
	memcpy((uint8_t *)self + FIELDS_FROM, fields_before, sizeof(fields_before));
	memcpy((uint8_t *)self + BOX, box_before, 16);
	memcpy(rng_state, rng_before, 64);
	*rng_index = rng_before[16];
	if (lists_ok) {
		list_restore(0, &lists_before[0]);
		list_restore(1, &lists_before[1]);
		memcpy((uint8_t *)self + SELF_LINKS, links_before, sizeof(links_before));
	}
	arrays_restore(before, &arrays_before);

	capture = 1;
	native_event(self, other);
	capture = -1;
	verify_self = NULL;
	// smokeWobble: a cloud that ends where it started is not marked, so the lists, its links and +0x7c bit 3 must be
	// as before the event; one that moved or turned is marked like the original
	int unmoved = raw && !raw_marked;
	const ListSnap *lists_expect = unmoved ? lists_before : lists_gml;
	const uint8_t *links_expect = unmoved ? links_before : links_gml;
	if (unmoved)
		put_word(fields_gml, 0x7c - FIELDS_FROM, (word(fields_gml, 0x7c - FIELDS_FROM) & ~8u) |
			(word(fields_before, 0x7c - FIELDS_FROM) & 8u));

	// exp_smokeverifyfault1: the lists as if the native had skipped CollisionMarkDirty; the real ones go back after
	// the compare
	int fault_lists = verify_fault == 1 && lists_ok && list_take(0, &lists_real[0]) && list_take(1, &lists_real[1]);
	if (fault_lists) {
		memcpy(links_real, (uint8_t *)self + SELF_LINKS, sizeof(links_real));
		list_restore(0, &lists_before[0]);
		list_restore(1, &lists_before[1]);
		memcpy((uint8_t *)self + SELF_LINKS, links_before, sizeof(links_before));
	}
	int nn = snapshot_vars(self, after_native);
	uint32_t *faulted = NULL; // exp_smokeverifyfault2: a wrong element written (one value bit), undone after the compare
	if (verify_fault == 2 && nn > 0 && arrays_take(after_native, nn, &arrays_native))
		for (int k = 0; k < arrays_native.n && !faulted; k++) {
			RV *items = (RV *)(uintptr_t)word((const void *)arrays_native.a[k].header, 4);
			for (uint32_t i = 0; i < arrays_native.a[k].n && !faulted; i++)
				if (!owning(&items[i]) && KIND(&items[i]) != 0xffffff && KIND(&items[i]) != 5)
					faulted = (uint32_t *)&items[i].v;
		}
	if (faulted)
		*faulted ^= 1;
	int collision_ok = 1;
	if (lists_ok) {
		lists_ok = list_take(0, &lists_native[0]) && list_take(1, &lists_native[1]);
		collision_ok = lists_ok && list_same(&lists_expect[0], &lists_native[0]) &&
			list_same(&lists_expect[1], &lists_native[1]) &&
			!memcmp((uint8_t *)self + SELF_LINKS, links_expect, sizeof(links_gml));
	}
	if (fault_lists) {
		list_restore(0, &lists_real[0]);
		list_restore(1, &lists_real[1]);
		memcpy((uint8_t *)self + SELF_LINKS, links_real, sizeof(links_real));
	}
	if (!lists_ok)
		smokedraw_verify_lists_unchecked++;
	if (arrays_ok)
		arrays_ok = nn >= 0 && arrays_take(after_native, nn, &arrays_native);
	int elements_ok = !arrays_ok || arrays_same(&arrays_gml, &arrays_native);
	if (faulted)
		*faulted ^= 1;
	if (!arrays_ok)
		smokedraw_verify_arrays_unchecked++;
	if (look_verify) // the look settings change only the light layer script call and the submissions
		for (int c = 0; c < 2; c++) {
			int n = 0, total = record_count[c] < MAX_RECORDS ? record_count[c] : MAX_RECORDS;
			for (int k = 0; k < total; k++)
				if (records[c][k].kind != R_SPRITE && records[c][k].kind != R_SCRIPT + 3)
					records[c][n++] = records[c][k];
			record_count[c] = record_count[c] > MAX_RECORDS ? MAX_RECORDS + 1 : n;
		}
	int records_ok = record_count[0] == record_count[1] && record_count[0] <= MAX_RECORDS &&
		!memcmp(records[0], records[1], sizeof(Record) * record_count[0]);
	int fields_ok = !memcmp((uint8_t *)self + FIELDS_FROM, fields_gml, sizeof(fields_gml));
	// The box: the native leaves it as the original does (raw: as before the event; the original changes it only if
	// something in the event ran UpdateTree, counted). Raw with the box current before the event (bit 3 clear): the
	// box Compute_BoundingBox gives for the final state is the box the cloud holds (one that moved is marked).
	int box_ok = !memcmp((uint8_t *)self + BOX, raw ? box_before : box_gml, 16), recompute_ok = 1;
	if (raw) {
		smokedraw_wobble_events++;
		smokedraw_wobble_unmoved += unmoved;
		if (memcmp(box_gml, box_before, 16) || bbox_calls[0] || bbox_calls[1])
			smokedraw_wobble_box_window++;
		if (unmoved && (word(self, 0x7c) & 8) != (word(fields_before, 0x7c - FIELDS_FROM) & 8))
			smokedraw_wobble_flag_bad++;
		if (unmoved && !(word(fields_before, 0x7c - FIELDS_FROM) & 8) && compute_bbox) {
			uint32_t flags = word(self, 0x7c);
			uint8_t box_now[16];
			memcpy(box_now, (uint8_t *)self + BOX, 16);
			compute_bbox(self, 0);
			recompute_ok = !memcmp((uint8_t *)self + BOX, box_before, 16);
			memcpy((uint8_t *)self + BOX, box_now, 16);
			put_word(self, 0x7c, flags);
			smokedraw_wobble_box_checked++;
			smokedraw_wobble_box_bad += !recompute_ok;
		}
		smokedraw_wobble_list_bad += !collision_ok;
	}
	box_ok = box_ok && recompute_ok;
	int rng_ok = !memcmp(rng_state, rng_gml, 64) && *rng_index == rng_gml[16];
	int bad_slot = ng < 0 || ng != nn ? -2 : -1;
	for (int i = 0; bad_slot == -1 && i < ng; i++) {
		const Slot *a = &after_gml[i], *n = &after_native[i];
		int loose = a->key == (uint32_t)id[V_AY] + 1 || a->key == (uint32_t)id[V_AX] + 1 ||
			a->key == (uint32_t)id[V_ANGLE] + 1;
		if (look_verify && is_light_var(a->key))
			continue; // written by draw_sprite_recolored only, read by it and the base layer's colour

		if (a->key != n->key || a->slot != n->slot || a->value.kind != n->value.kind ||
				memcmp(&a->value.v, &n->value.v, 8) || (!loose && a->value.flags != n->value.flags))
			bad_slot = i;
	}
	if (records_ok && fields_ok && rng_ok && bad_slot == -1 && collision_ok && elements_ok && box_ok) {
		smokedraw_verified++;
		smokedraw_verify_records += record_count[1];
		if (lists_ok)
			smokedraw_verify_nodes += lists_gml[0].n + lists_gml[1].n;
		for (int k = 0; arrays_ok && k < arrays_gml.n; k++) {
			smokedraw_verify_arrays++;
			smokedraw_verify_elements += arrays_gml.a[k].n;
		}
		return;
	}
	if (smokedraw_mismatches++ < 8) {
		debugPrintf("[SMOKEDRAW] mismatch: records %d/%d %s, fields %s, rng %s, variables %s (slot %d key %u), "
			"collision lists %s, array elements %s, box %s%s\n",
			record_count[0], record_count[1], records_ok ? "same" : "differ", fields_ok ? "same" : "differ",
			rng_ok ? "same" : "differs", bad_slot == -1 ? "same" : "differ", bad_slot,
			bad_slot >= 0 ? after_gml[bad_slot].key : 0, collision_ok ? "same" : "differ",
			elements_ok ? "same" : "differ", box_ok ? "same" : "differs",
			raw ? (unmoved ? " (wobble)" : " (wobble, marked)") : "");
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
	for (int i = 0; i < 6; i++)
		light_vars[i] = (const int32_t *)so_symbol(&yoyoloader_mod, light_var_names[i]);
	rng_state = (uint32_t *)so_symbol(&yoyoloader_mod, "state");
	rng_index = (uint32_t *)so_symbol(&yoyoloader_mod, "g_RndIndex");
	draw_instance = (void **)so_symbol(&yoyoloader_mod, "_ZN15CSkeletonSprite15ms_drawInstanceE");
	coll_list[0] = (uintptr_t *)so_symbol(&yoyoloader_mod, "s_dirty_list");
	coll_list[1] = (uintptr_t *)so_symbol(&yoyoloader_mod, "s_test_list");
	verify_fault = exp_flag("smokeverifyfault1") ? 1 : exp_flag("smokeverifyfault2") ? 2 :
		exp_flag("smokeverifyfault3") ? 3 : exp_flag("smokeverifyfault4") ? 4 : 0;
	return rng_state && rng_index && draw_instance && coll_list[0] && coll_list[1] &&
		coll_list[0][2] == 0x168 && coll_list[1][2] == SELF_LINKS &&
		so_symbol(&yoyoloader_mod, "_ZN7CSprite4DrawEffffffjf");
}

// On the first verified event: the recording hooks. 0 (verify off) if CSprite::Draw can't be hooked.
static int verify_hooks(void) {
	orig_sprite_draw = (sprite_draw_fn)hook_or_chain(so_symbol(&yoyoloader_mod, "_ZN7CSprite4DrawEffffffjf"),
		(void *)record_sprite_draw);
	if (!orig_sprite_draw) {
		debugPrintf("smoke cloud Draw verify: CSprite::Draw can't be hooked, verify off\n");
		return 0;
	}
	compute_bbox = (bbox_fn)so_symbol(&yoyoloader_mod, "_ZN9CInstance19Compute_BoundingBoxEb");
	if (compute_bbox && !(orig_compute_bbox = (bbox_fn)hook_or_chain((uintptr_t)compute_bbox, (void *)record_bbox)))
		compute_bbox = NULL; // no box checks
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
			"%u skipped; %u list nodes, %u arrays with %u elements compared%s\n", smokedraw_verified,
			smokedraw_verify_records, smokedraw_mismatches, smokedraw_verify_skipped, smokedraw_verify_nodes,
			smokedraw_verify_arrays, smokedraw_verify_elements,
			smokedraw_verify_unrecorded ? ", some scripts not recorded" : "");
	if (smokedraw_verify && (smokedraw_verify_lists_unchecked || smokedraw_verify_arrays_unchecked))
		debugPrintf("[SMOKEDRAW] verify: collision lists unchecked in %u events, array elements in %u\n",
			smokedraw_verify_lists_unchecked, smokedraw_verify_arrays_unchecked);
	if (smoke_clouds != 1 || look_ab)
		debugPrintf("[SMOKELOOK] smokeClouds %d: drawn %.1f hidden %.1f per frame%s\n", smoke_clouds,
			(float)look_drawn / frames, (float)look_hidden / frames,
			look_ab ? (look_off ? " | A/B: off" : " | A/B: on") : "");
	look_drawn = look_hidden = 0;
	if (look_ab)
		look_off = !look_off;
	if ((smoke_wobble || wobble_ab) && (wobble_raw || wobble_exact))
		debugPrintf("[SMOKEWOBBLE] smokeWobble %d: field writes %.1f setters %.1f marked %.1f per frame%s\n", smoke_wobble,
			(float)wobble_raw / frames, (float)wobble_exact / frames, (float)wobble_marked / frames,
			wobble_ab ? (wobble_off ? " | A/B: off" : " | A/B: on") : "");
	if (wobble_verify)
		debugPrintf("[SMOKEWOBBLE] verify: %u wobble events (%u unmoved, %u marked), lists differ %u, bit 3 differs %u, "
			"box checked %u differs %u, box computed in the event %u\n", smokedraw_wobble_events,
			smokedraw_wobble_unmoved, smokedraw_wobble_events - smokedraw_wobble_unmoved, smokedraw_wobble_list_bad,
			smokedraw_wobble_flag_bad, smokedraw_wobble_box_checked, smokedraw_wobble_box_bad,
			smokedraw_wobble_box_window);
	wobble_raw = wobble_exact = wobble_marked = 0;
	if (collog)
		smoke_collog_report(frames);
	if (tree_check && tc_frames)
		debugPrintf("[SMOKETREE] check: %u frames, per frame %.1f entries (%.1f clouds, %.1f pending); rectangle differs: "
			"clouds %u, others %u; in twice %u, broken %u\n", tc_frames, (float)tc_entries / tc_frames,
			(float)tc_clouds / tc_frames, (float)tc_pending / tc_frames, tc_bad_cloud, tc_bad_other, tc_dup, tc_broken);
	tc_frames = tc_entries = tc_clouds = tc_pending = 0;
	if (wobble_ab)
		wobble_off = !wobble_off;
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
	look_ab = debugMode && exp_flag("smokelookab");
	if (smoke_clouds < 1)
		smoke_clouds = 1;
	if (smoke_clouds > 3)
		smoke_clouds = 3;
	draw_instance = (void **)so_symbol(&yoyoloader_mod, "_ZN15CSkeletonSprite15ms_drawInstanceE");
	sprite_data = (void *(*)(int))so_symbol(&yoyoloader_mod, "_Z11Sprite_Datai");
	if (!draw_instance || !sprite_data) // a hidden cloud's draw_self couldn't be matched: the original look
		smoke_clouds = 1;
	cloud_scale = (float)smoke_clouds; // sqrt by Newton steps (no libm call)
	for (int i = 0; i < 20; i++)
		cloud_scale = 0.5f * (cloud_scale + (float)smoke_clouds / cloud_scale);
	debugPrintf("smoke look: smokeClouds %d\n", smoke_clouds);
	for (unsigned i = 0; i < NWOBBLE; i++) { // smokeWobble's runner functions (validate checks them every frame)
		wobble_addr[i] = so_symbol(&yoyoloader_mod, wobble_fns[i].name);
		if (wobble_addr[i] && wobble_addr[i] != bias + wobble_fns[i].addr)
			wobble_addr[i] = 0;
		wobble_got[i] = so_got_slot(&yoyoloader_mod, wobble_fns[i].name);
		if (!wobble_addr[i])
			smoke_wobble = 0;
	}
	wobble_ab = debugMode && exp_flag("smokewobbleab");
	if (debugMode && exp_flag("smokecollog")) {
		collog = 1;
		smoke_collog_enable();
	}
	if (debugMode && exp_flag("smoketreecheck")) {
		collision_trees = (void **)so_symbol(&yoyoloader_mod, "g_collision_trees");
		dirty_sentinel = so_symbol(&yoyoloader_mod, "s_dirty_list");
		object_find = (int (*)(const char *))so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
		tree_check = collision_trees && dirty_sentinel;
		debugPrintf("smoke tree check: %s\n", tree_check ? "on" : "NOT available");
	}
	debugPrintf("smoke wobble: %s\n", smoke_wobble ? "field writes" : "setters (original)");
	if (debugMode && exp_flag("smokeverify")) {
		smokedraw_verify = verify_install();
		look_verify = smokedraw_verify && exp_flag("smokelookverify");
		wobble_verify = smokedraw_verify && exp_flag("smokewobbleverify");
		debugPrintf("smoke cloud Draw verify: %s\n", smokedraw_verify ? "on" : "NOT available");
	}
	hook_raw(entry, (uintptr_t)smokedraw_event);
	kuKernelFlushCaches((void *)entry, 8);
	installed = 1;
	return 1;
}
