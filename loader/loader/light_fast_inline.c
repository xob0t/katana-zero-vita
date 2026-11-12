/* APK 1.10, SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * Inlined-helper variant of light_fast.c: the "static recompiler kill test" of
 * work/cpu_research/big_win.md section 3. Same admission and the same event
 * bodies as light_fast.c; the differences are:
 *   - YYCompareVal is replaced by cmp(): kind check, then the runner's REAL/BOOL
 *     path (e3ee1c..e3ee30, e3f31c..e3f338) in C; every other case calls the
 *     original function;
 *   - variable lookups, table set-up and truth tests are forced inline;
 *   - no rejection histogram scan and no hash-table shape validation. Checks
 *     that select a different runner code path (standard getters, +4 word,
 *     missing map, slot kinds) stay, because exactness needs them.
 * Resolve symbols after loader relocation. Descriptor IDs are read live.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#ifdef LIGHT_FAST_STUB
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif
#define ABI __attribute__((pcs("aapcs")))
#define INL static inline __attribute__((always_inline))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef void ABI (*Event)(void *, void *);
typedef RV *ABI (*Getter)(void *, int);
typedef int ABI (*Compare)(const RV *, const RV *, double, bool);
typedef bool ABI (*Setter)(void *, int, int, RV *);
typedef double ABI (*Random)(double);
typedef float ABI (*PointDirection)(float, float, float, float);
typedef void ABI (*LegacyCall)(void *, void *, RV *, int, int, RV **);
enum { ORANGE, BACK, NEVENTS };
enum { COLOR, REGION, HAS_BACK, HAS_LIGHTBACK, MOUSE, MOVING, SPEED,
       MX, MY, FR, PARENT, ALPHA, PREV_SPEED, BASE_X, BASE_Y, TIMER, X, Y, NVARS };
static const char *const names[NVARS] = {
    "g_VAR_color_override", "g_VAR_region", "g_VAR_has_back",
    "g_VAR_has_lightback_obj", "g_VAR_mouse", "g_VAR_moving", "g_VAR_speed",
    "g_VAR_mouse_xposition", "g_VAR_mouse_yposition", "g_VAR_fr",
    "g_VAR_parent", "g_VAR_alpha", "g_VAR_prev_speed", "g_VAR_base_x",
    "g_VAR_base_y", "g_VAR_collision_timer", "g_VAR_x", "g_VAR_y"
};
static const uint32_t *vars[NVARS];
static Event original[NEVENTS];
static Getter standard_get, standard_ref;
static Compare compare;
static Setter set_value;
static Random runner_random;
static PointDirection point_direction;
static LegacyCall call_legacy;
static const uint32_t *motion_var;
static void **functions;
static const int *function_count;
static uintptr_t motion_function, x_get, x_set, y_get, y_set;
static const RV *motion_amount;
static const double *epsilon;
static const uint8_t *profile, *managed;
static const int *managed_guard, *builtin_start;
static const void *builtins, *instance_hash;
static uintptr_t speed_get, speed_set;
static void **globals;
static bool installed;
/* Event indices: 0 orangelight Step, 1 lightback Step. One increment per call.
 * A rejection costs two stores on the cold path: reject_line[i] is the C source
 * line of check i, reject_count[event][i] how often it rejected. */
enum { LIGHT_FAST_INLINE_REJECTS = 48 };
unsigned light_fast_inline_hits[NEVENTS], light_fast_inline_fallbacks[NEVENTS];
unsigned light_fast_inline_reject_line[LIGHT_FAST_INLINE_REJECTS];
unsigned light_fast_inline_reject_count[NEVENTS][LIGHT_FAST_INLINE_REJECTS];
static __attribute__((noinline, cold)) void reject(unsigned event, unsigned code, unsigned line) {
    light_fast_inline_reject_line[code] = line;
    ++light_fast_inline_reject_count[event][code];
}
enum { REJECT_BASE = __COUNTER__ + 1 };
#define REJECT(event) (reject(event, __COUNTER__ - REJECT_BASE, __LINE__), 0)
INL uint32_t word(const void *p, size_t off) {
    uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v;
}
INL void *ptr(const void *p, size_t off) {
    return (void *)(uintptr_t)word(p, off);
}
INL int id(int n) { return (int)vars[n][1]; }
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
/* What the generic getter checks before it reaches the plain slot map. */
INL int table(const void *p, Table *out) {
    if (!p || word(p, 4)) return 0;
    const void *vt = ptr(p, 0);
    if (!vt || word(vt, 8) != (uintptr_t)standard_get) return 0;
    const void *ht = ptr(p, 0x24);
    if (!ht) return 0;
    out->mask = word(ht, 8); out->entries = ptr(ht, 16);
    return 1;
}
INL int lvalue_table(const void *p) {
    return word(ptr(p, 0), 12) == (uintptr_t)standard_ref;
}
/* Existing scalar locals return directly in InternalGetYYVarRef. Missing,
 * inherited or accessor entries must execute the original getter. */
INL RV *peek(const Table *t, int n) {
    uint32_t key = ((uint32_t)id(n) + 1) & 0x7fffffffu;
    if (!key) return NULL;
    uint32_t i = key & t->mask;
    for (uint32_t d = 0; d <= t->mask; ++d, i = (i + 1) & t->mask) {
        const void *e = t->entries + 12*i;
        uint32_t k = word(e, 8);
        if (k == key) return ptr(e, 0);
        if (!k || d > ((i - (k & t->mask)) & t->mask)) break;
    }
    return NULL;
}
INL int numeric(const RV *v) {
    return v && (v->kind == 0 || v->kind == 13) && isfinite(v->value);
}
#define SCALAR(t, n, v, event) (numeric(*(v) = peek(t, n)) || REJECT(event))
/* BOOL_RValue jump-table entries 0 and 13 both target ec9ce0. Admission
 * excludes every other kind before using this exact double threshold. */
INL int truth(const RV *v) { return v->value > 0.5; }
/* YYCompareVal, jump-table entries (0|13, 0|13) -> e3ee1c: d = a - b; unless d
 * is NaN, e3f31c returns 0 for |d| <= epsilon (false for a NaN epsilon, as
 * "ls" is), else -1 for d < 0, else 1. NaN differences and all other kind
 * pairs run the original. */
INL int cmp(const RV *a, const RV *b, double eps, bool throws) {
    if ((a->kind == 0 || a->kind == 13) && (b->kind == 0 || b->kind == 13)) {
        double d = a->value - b->value;
        if (d == d) return fabs(d) <= eps ? 0 : d < 0 ? -1 : 1;
    }
    return compare(a, b, eps, throws);
}
INL int local_id(int n) {
    /* Variable_GetValue_Direct checks unsigned id > 9999 before builtin
     * dispatch. Those IDs are locals too, even above builtin_start. */
    return id(n) >= 0 && (id(n) > 9999 || id(n) >= *builtin_start);
}
INL float field(const void *self, size_t off) {
    float f; memcpy(&f, (const uint8_t *)self + off, 4); return f;
}
INL int builtin(int n, uintptr_t get, uintptr_t set) {
    int i = id(n);
    if (i < 0 || i >= *builtin_start || i > 9999) return 0;
    const uint8_t *b = (const uint8_t *)builtins + 16*i;
    return word(b, 4) == get && word(b, 8) == set && b[12];
}
INL int writable(void *self) {
    if (!*managed) return 1;
    const void *def = ptr(self, 0x68);
    return def && !(*managed_guard >= 1 && (word(def, 0x50) & 0x80u));
}
INL void *find_id(int ident) {
    const void *buckets = ptr(instance_hash, 0);
    if (!buckets) return NULL;
    const void *node = ptr(buckets, ((uint32_t)ident & word(instance_hash, 4))*8);
    for (; node; node = ptr(node, 4))
        if (word(node, 8) == (uint32_t)ident) return ptr(node, 12);
    return NULL;
}
INL void *parent_instance(const RV *parent) {
    int ident;
    if (parent && parent->kind == 15 && word(parent, 4) == 0x04000001u)
        ident = (int)word(parent, 0);
    else if (parent && parent->kind == 0 && parent->value >= 100000 &&
             parent->value <= 2147483647.0 && parent->value == (int)parent->value)
        ident = (int)parent->value;
    else return NULL;
    if (ident < 100000) return NULL;
    void *owner = find_id(ident);
    return owner && word(owner, 0x80) == (uint32_t)ident &&
           word(owner, 0x50) == 1 ? owner : NULL;
}
typedef struct {
    enum { QUIET, MOTION, ATTACHED } path;
    bool cap;
    RV *prev, *timer, *base_x, *base_y;
    void *owner;
    int motion_id;
} Orange;
INL int orange_admit(void *self, Orange *plan) {
    Table t, g;
    const double eps = *epsilon;
    plan->path = QUIET;
    if (*profile) return REJECT(ORANGE);
    if (!isfinite(eps) || eps < 0) return REJECT(ORANGE);
    if (!self || word(self, 0x50) != 1) return REJECT(ORANGE);
    if (!table(self, &t) || !table(*globals, &g)) return REJECT(ORANGE);
    RV *v, *fr;
    /* The original unconditionally gets these globals before its first if.
     * Requiring present scalar entries prevents skipping getter side effects. */
    if (!SCALAR(&g, MX, &v, ORANGE) || !SCALAR(&g, MY, &v, ORANGE) ||
        !SCALAR(&g, FR, &fr, ORANGE)) return 0;
    const RV zero = {0, 0, 0}, minus_one = {-1, 0, 0}, ten = {10, 0, 0};
    if (!SCALAR(&t, COLOR, &v, ORANGE)) return 0;
    if (cmp(v, &zero, eps, false) != 0) return REJECT(ORANGE);
    if (!SCALAR(&t, REGION, &v, ORANGE)) return 0;
    if (cmp(v, &minus_one, eps, false) == 0) return REJECT(ORANGE);
    if (!SCALAR(&t, HAS_BACK, &v, ORANGE)) return 0;
    RV *has_lightback;
    if (!SCALAR(&t, HAS_LIGHTBACK, &has_lightback, ORANGE)) return 0;
    if (truth(v)) {
        if (!truth(has_lightback)) return REJECT(ORANGE);
    }
    if (!SCALAR(&t, MOUSE, &v, ORANGE)) return 0;
    if (truth(v)) return REJECT(ORANGE);
    if (!SCALAR(&t, MOVING, &v, ORANGE)) return 0;
    bool moving = truth(v);
    if (!builtin(SPEED, speed_get, speed_set))
        return REJECT(ORANGE);
    float f = field(self, 0xd4);
    if (!isfinite(f)) return REJECT(ORANGE);
    RV speed = {(double)f, 0, 0};
    plan->cap = cmp(&speed, &ten, eps, true) >= 1;
    /* Leave errors in the real event frame. Definition 0x80 is the managed
     * rollback marker, not a reason to reject ordinary read-only calls. */
    if ((moving || plan->cap) && !writable(self))
        return REJECT(ORANGE);
    if (moving) {
        RV *parent = peek(&t, PARENT);
        if (!parent || (!numeric(parent) && parent->kind != 15))
            return REJECT(ORANGE);
        if (!builtin(X, x_get, x_set) || !builtin(Y, y_get, y_set))
            return REJECT(ORANGE);
        /* The original comparison accepts reference and numeric parents. */
        if (cmp(parent, &minus_one, eps, false) != 0) {
            plan->owner = parent_instance(parent);
            if (!plan->owner || !isfinite(field(plan->owner, 0xb8)) ||
                !isfinite(field(plan->owner, 0xbc))) return REJECT(ORANGE);
            plan->path = ATTACHED;
        } else {
            if (cmp(fr, &zero, eps, false) == 0)
                return REJECT(ORANGE);
            if (!lvalue_table(self)) return REJECT(ORANGE);
            if (!local_id(PREV_SPEED) || !local_id(TIMER))
                return REJECT(ORANGE);
            if (!SCALAR(&t, PREV_SPEED, &plan->prev, ORANGE) ||
                !SCALAR(&t, BASE_X, &plan->base_x, ORANGE) ||
                !SCALAR(&t, BASE_Y, &plan->base_y, ORANGE) ||
                !SCALAR(&t, TIMER, &plan->timer, ORANGE)) return 0;
            if (plan->timer->kind != 0 || plan->prev == plan->timer)
                return REJECT(ORANGE);
            RV next = {plan->timer->value - 1.0, 0, 0};
            /* L40 is epsilon-aware, after all four random calls in the
             * original. Reject the whole event using its pre-state. */
            if (cmp(&next, &zero, eps, true) <= 0)
                return REJECT(ORANGE);
            if (!isfinite(field(self, 0xb8)) || !isfinite(field(self, 0xbc)) ||
                !isfinite(field(self, 0xd0)) || !isfinite((float)plan->prev->value) ||
                !isfinite((float)plan->base_x->value) ||
                !isfinite((float)plan->base_y->value)) return REJECT(ORANGE);
            plan->motion_id = (int)motion_var[1];
            if (!*functions || plan->motion_id < 0 || plan->motion_id >= *function_count ||
                word(*functions, 12*(unsigned)plan->motion_id + 4) != motion_function)
                return REJECT(ORANGE);
            if (!motion_amount || motion_amount->kind != 0 || motion_amount->value != 0.1)
                return REJECT(ORANGE);
            plan->path = MOTION;
        }
    }
    return 1;
}
static void ABI __attribute__((naked, noinline))
orange_fallback(void *self __attribute__((unused)), void *other __attribute__((unused)),
                uint32_t flags __attribute__((unused)), Event event __attribute__((unused))) {
    __asm__ volatile("mov ip, sp\n\tsub.w ip, ip, #0x2b0\n\tstr r2, [ip]\n\tbx r3");
}
static void ABI __attribute__((used, noinline))
orange_inline_entry(void *self, void *other, uint32_t speed_flags) {
    Orange plan;
    if (!orange_admit(self, &plan)) {
        ++light_fast_inline_fallbacks[ORANGE];
        orange_fallback(self, other, speed_flags, original[ORANGE]);
        return;
    }
    ++light_fast_inline_hits[ORANGE];
    const RV ten = {10, 0, 0};
    if (plan.path == MOTION) {
        RV old = *plan.prev;
        set_value(self, id(SPEED), (int)0x80000000u, &old);
        /* Explicit statements preserve x+, x-, y+, y- RNG ordering and
         * double arithmetic before the original float argument rounding. */
        double bx = plan.base_x->value + runner_random(15.0);
        bx = bx - runner_random(15.0);
        double by = plan.base_y->value + runner_random(15.0);
        by = by - runner_random(15.0);
        float direction = point_direction(field(self, 0xb8), field(self, 0xbc),
                                          (float)bx, (float)by);
        RV angle = {(double)direction, 0, 0}, amount = *motion_amount;
        RV result = {0, 0, 5};
        RV *args[2] = {&angle, &amount};
        call_legacy(self, other, &result, 2, plan.motion_id, args);
        plan.prev->value = (double)field(self, 0xd4);
        plan.prev->flags = speed_flags; plan.prev->kind = 0;
        plan.timer->value -= 1.0;
    } else if (plan.path == ATTACHED) {
        RV x = {(double)field(plan.owner, 0xb8), 0, 0};
        set_value(self, id(X), (int)0x80000000u, &x);
        RV y = {(double)field(plan.owner, 0xbc), 0, 0};
        set_value(self, id(Y), (int)0x80000000u, &y);
    }
    if (plan.path != QUIET) {
        RV speed = {(double)field(self, 0xd4), 0, 0};
        plan.cap = cmp(&speed, &ten, *epsilon, true) >= 1;
    }
    if (plan.cap) {
        RV cap = ten;
        set_value(self, id(SPEED), (int)0x80000000u, &cap);
    }
}
/* GV_Speed leaves flags untouched. L38 copies the original event's stale
 * temporary flags into prev_speed: incoming SP - 0x440 + 0x190. Capture
 * that word before our frame or runner calls can overwrite that stack area.
 * Tail-branch to C so fallback can restore the incoming stack exactly. */
void ABI __attribute__((naked)) light_fast_inline_orangelight(void *self __attribute__((unused)),
                                                            void *other __attribute__((unused))) {
    __asm__ volatile("mov r2, sp\n\tsub.w r2, r2, #0x2b0\n\tldr r2, [r2]\n\tb orange_inline_entry");
}
INL int back_admit(void *self, RV **src, RV **dst) {
    Table t, p;
    if (*profile) return REJECT(BACK);
    if (!self || word(self, 0x50) != 1) return REJECT(BACK);
    if (!local_id(ALPHA)) return REJECT(BACK);
    if (!table(self, &t) || !lvalue_table(self)) return REJECT(BACK);
    const RV *parent = peek(&t, PARENT);
    void *owner = parent_instance(parent);
    if (!owner) return REJECT(BACK);
    if (!table(owner, &p)) return REJECT(BACK);
    if (!SCALAR(&t, ALPHA, dst, BACK) || !SCALAR(&p, ALPHA, src, BACK)) return 0;
    return 1;
}
void ABI light_fast_inline_lightback(void *self, void *other) {
    RV *src, *dst;
    if (!back_admit(self, &src, &dst)) {
        ++light_fast_inline_fallbacks[BACK]; original[BACK](self, other); return;
    }
    ++light_fast_inline_hits[BACK];
    /* GET_RValue and the event copy all 16 bytes, including flags. Neither
     * existing scalar local assignment nor getter tests the managed guard. */
    RV copy = *src; *dst = copy;
}
_Static_assert(__COUNTER__ - REJECT_BASE <= LIGHT_FAST_INLINE_REJECTS, "reject table too small");
int light_fast_inline_install(void *(*rx_alloc)(size_t)) {
    static const char *const events[NEVENTS] = {
        "_Z33gml_Object_obj_orangelight_Step_0P9CInstanceS0_",
        "_Z31gml_Object_obj_lightback_Step_0P9CInstanceS0_"
    };
    static const uint32_t prologue[NEVENTS][2] = {
        {0xe92d4ff0, 0xe28db01c}, {0xe92d4df0, 0xe28db018}
    };
    Event replacements[NEVENTS] = {light_fast_inline_orangelight, light_fast_inline_lightback};
    uintptr_t entries[NEVENTS];
    if (installed || !rx_alloc) return 0;
    for (int i = 0; i < NEVENTS; ++i) {
        entries[i] = so_symbol(&yoyoloader_mod, events[i]);
        if (!entries[i] || (entries[i] & 3) ||
            word((void *)entries[i], 0) != prologue[i][0] ||
            word((void *)entries[i], 4) != prologue[i][1]) return 0;
    }
    for (int i = 0; i < NVARS; ++i) {
        vars[i] = (const uint32_t *)so_symbol(&yoyoloader_mod, names[i]);
        if (!vars[i]) return 0;
    }
#define RESOLVE(v, type, name) do { \
    v = (type)so_symbol(&yoyoloader_mod, name); if (!v) return 0; \
} while (0)
    RESOLVE(standard_get, Getter, "_ZN12YYObjectBase19InternalGetYYVarRefEi");
    RESOLVE(standard_ref, Getter, "_ZN12YYObjectBase20InternalGetYYVarRefLEi");
    RESOLVE(compare, Compare, "_Z12YYCompareValRK6RValueS1_db");
    RESOLVE(set_value, Setter, "_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
    RESOLVE(runner_random, Random, "_Z12YYGML_randomd");
    RESOLVE(point_direction, PointDirection, "_Z21YYGML_point_directionffff");
    RESOLVE(call_legacy, LegacyCall, "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
    RESOLVE(motion_var, const uint32_t *, "g_FUNC_motion_add");
    RESOLVE(functions, void **, "the_functions");
    RESOLVE(function_count, const int *, "the_numb");
    RESOLVE(motion_function, uintptr_t, "_Z11F_MotionAddR6RValueP9CInstanceS2_iPS_");
    RESOLVE(motion_amount, const RV *, "gs_constArg0_87AEB8EE");
    RESOLVE(x_get, uintptr_t, "_Z4GV_XP9CInstanceiP6RValue");
    RESOLVE(x_set, uintptr_t, "_Z4SV_XP9CInstanceiP6RValue");
    RESOLVE(y_get, uintptr_t, "_Z4GV_YP9CInstanceiP6RValue");
    RESOLVE(y_set, uintptr_t, "_Z4SV_YP9CInstanceiP6RValue");
    RESOLVE(epsilon, const double *, "g_GMLMathEpsilon");
    RESOLVE(profile, const uint8_t *, "g_bProfile");
    RESOLVE(managed, const uint8_t *, "g_enableManagedObjects");
    RESOLVE(managed_guard, const int *, "g_nManagedObjectGuard");
    RESOLVE(builtin_start, const int *, "g_nStartGlobalVariables");
    RESOLVE(builtins, const void *, "builtin_variables");
    RESOLVE(speed_get, uintptr_t, "_Z8GV_SpeedP9CInstanceiP6RValue");
    RESOLVE(speed_set, uintptr_t, "_Z8SV_SpeedP9CInstanceiP6RValue");
    RESOLVE(instance_hash, const void *, "_ZN9CInstance14ms_ID2InstanceE");
    RESOLVE(globals, void **, "g_pGlobal");
#undef RESOLVE
    uint8_t *rx = rx_alloc(32);
    if (!rx || ((uintptr_t)rx & 3) || (uintptr_t)rx > UINTPTR_MAX - 32) return 0;
    for (int i = 0; i < NEVENTS; ++i)
        if ((uintptr_t)rx < entries[i] + 8 && entries[i] < (uintptr_t)rx + 32) return 0;
    for (int i = 0; i < NEVENTS; ++i) {
        uint32_t code[4] = {prologue[i][0], prologue[i][1], 0xe51ff004,
                            (uint32_t)(entries[i] + 8)};
        kuKernelCpuUnrestrictedMemcpy(rx + 16*i, code, sizeof(code));
        original[i] = (Event)(rx + 16*i);
    }
    kuKernelFlushCaches(rx, 32);
    for (int i = 0; i < NEVENTS; ++i) {
        hook_addr(entries[i], (uintptr_t)replacements[i]);
        kuKernelFlushCaches((void *)entries[i], 8);
    }
    installed = true; return 1;
}
