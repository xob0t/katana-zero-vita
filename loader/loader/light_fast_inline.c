/* APK 1.10, SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * Inlined-helper variant of the hand native light_fast.c (removed Nov 2026, in git history): the kill test of
 * the untracked work/cpu_research/big_win.md, section 3. Same admission and the same event
 * bodies as that file had; the differences are:
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
#include <stdio.h>
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
typedef void ABI (*AddToSpeed)(void *, float, float);
typedef int ABI (*ObjectFind)(const char *);
static AddToSpeed add_to_speed;
static ObjectFind object_find;
static const uint32_t *frame_counter;

/* Lamp sway (lampSway=N in yyl.cfg, 1..4, default 4; 1 = the original every tick).
 * A free-floating obj_orangelight (moving, parent -1) steers toward a random point near its base every tick:
 * speed = prev_speed, motion_add(direction to base +- random(15), 0.1), prev_speed = speed. The runner moves it
 * by its speed every tick whatever the Step does. With lampSway=N the full Step runs on one tick in N (staggered by
 * instance id); on the others the light only draws its four random(15) values, in the same place of the event
 * order, so the global RNG stream is the original one. The next Step that runs first applies those deferred steering
 * impulses, with the same random targets, in order (speed = prev_speed, the impulse, prev_speed, collision_timer,
 * the speed cap: what the skipped Steps would have done), then the tick's own Step. So each lamp gets exactly the
 * original impulses, up to N-1 ticks late and aimed from where it is then. Simulated with the same pushes
 * (work/orangelight/swaydiff.py), a lamp at N=4 is ~3 px off the original's path after 1 s, ~5 px after 3 s, in a
 * 25-35 px sway; the paths drift apart over longer times, as two sessions' paths do (clock-seeded RNG).
 * Vita, room_haunted_cart2 (129 lamps), per-window A/B: N=4 -0.59 ms process per frame, N=2 -0.35..-0.5;
 * bunker2_3 (45) N=2 -0.1.
 * Only plain obj_orangelight instances whose last Step took the native motion path, didn't cap the speed and
 * won't reach the 15-tick wall check during the skip; never while global.fr is 0. Changes made by other code to
 * the light during a skipped tick (colour, region, parent) take effect up to N-1 ticks later; nothing in the game
 * logic reads a lamp's position or speed (only the lighting does).
 * exp_swayab: rate N and 1 alternating per report window (measurement only). */
int lamp_sway = 4;
int lamp_sway_ab, lamp_sway_ab_off;
unsigned lamp_sway_skips, lamp_sway_deferred, lamp_sway_dropped;
enum { SWAY_SLOTS = 512, SWAY_MAX = 4, SWAY_STALE = 120 };
typedef struct {
    uint32_t key, last;            /* key = instance id + 1 (0 = empty), last = tick of its last Step call */
    uint8_t eligible, pending;
    double timer;                  /* collision_timer after the last native motion Step */
    double r[SWAY_MAX - 1][4];     /* the four random(15) draws of each skipped tick */
} Sway;
static Sway sway[SWAY_SLOTS];
static int orange_object = -2;     /* obj_orangelight's index, resolved at the first Step (-1: not found) */

/* exp_lightparts: time of the event's parts, microseconds (process time), all calls of the report window */
int light_parts;
uint32_t light_parts_us[9];
unsigned light_parts_n[9];
#ifdef LIGHT_FAST_STUB
#define PART_NOW() 0u
#else
#include <psp2/kernel/processmgr.h>
#define PART_NOW() (light_parts ? sceKernelGetProcessTimeLow() : 0u)
#endif
#define PART(i, t) do { if (light_parts) { uint32_t now_ = PART_NOW(); light_parts_us[i] += now_ - (t); \
    ++light_parts_n[i]; (t) = now_; } } while (0)
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
INL int orange_admit(void *self, Orange *plan, int pending) {
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
            /* Lamp sway: the deferred ticks' decrements come first (each
             * stayed above 0, checked when its tick was skipped). */
            RV next = {plan->timer->value - 1.0 - (double)pending, 0, 0};
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
/* The slot of a plain obj_orangelight (object index at +0x84, id at +0x80), NULL for anything else or when the
 * table is full. Entries unseen for SWAY_STALE ticks are reused; ids aren't reused within a session. */
static Sway *sway_slot(void *self, uint32_t tick) {
    if (orange_object == -2)
        orange_object = object_find ? object_find("obj_orangelight") : -1;
    if (orange_object < 0 || (int)word(self, 0x84) != orange_object) return NULL;
    uint32_t key = word(self, 0x80) + 1, i = (key * 2654435761u) % SWAY_SLOTS;
    Sway *free_slot = NULL;
    for (int n = 0; n < SWAY_SLOTS; ++n, i = (i + 1) % SWAY_SLOTS) {
        Sway *s = &sway[i];
        if (s->key == key) return s;
        if (!s->key) { if (!free_slot) free_slot = s; break; }
        if (!free_slot && tick - s->last > SWAY_STALE) free_slot = s;
    }
    if (free_slot) { memset(free_slot, 0, sizeof(*free_slot)); free_slot->key = key; free_slot->last = tick - 2; }
    return free_slot;
}
/* At 30 ticks/s (combat30.c) half the rate in ticks, rounded up: the same rate in real time */
extern int c30_active;
INL int sway_rate(void) {
    if (lamp_sway_ab_off) return 1;
    int n = lamp_sway < 1 ? 1 : lamp_sway > SWAY_MAX ? SWAY_MAX : lamp_sway;
    return c30_active ? (n + 1) / 2 : n;
}
/* Whether this tick's Step may be skipped: see the lamp sway comment. */
INL int sway_skip(const Sway *s, uint32_t tick, int rate) {
    if (rate < 2 || !s->eligible || tick - s->last != 1 || s->pending >= rate - 1 || (tick + s->key) % rate == 0)
        return 0;
    /* L40 on the skipped tick: collision_timer would be timer - (pending + 1); it must stay above 0 (epsilon). */
    double d = s->timer - (double)(s->pending + 1);
    if (!(d == d) || !(d > *epsilon)) return 0;
    Table g;
    RV *fr;
    if (!table(*globals, &g) || !numeric(fr = peek(&g, FR))) return 0;
    const RV zero = {0, 0, 0};
    return cmp(fr, &zero, *epsilon, false) != 0;
}
/* The skipped ticks' motion steps (L36-L39 and L67), before this tick's Step. Lights no longer in that state
 * drop them. */
static void sway_apply(void *self, Sway *s, uint32_t speed_flags) {
    Table t;
    RV *prev, *base_x, *base_y, *timer, *v;
    const RV minus_one = {-1, 0, 0}, ten = {10, 0, 0};
    const double eps = *epsilon;
    int n = s->pending;
    s->pending = 0;
    if (!table(self, &t) || !lvalue_table(self) || !writable(self) || !builtin(SPEED, speed_get, speed_set) ||
        !local_id(PREV_SPEED) || !local_id(TIMER) || !add_to_speed || !motion_amount || motion_amount->kind != 0 ||
        !numeric(v = peek(&t, MOVING)) || !truth(v) || !(v = peek(&t, PARENT)) || !numeric(v) ||
        cmp(v, &minus_one, eps, false) != 0 ||
        !numeric(prev = peek(&t, PREV_SPEED)) || !numeric(base_x = peek(&t, BASE_X)) ||
        !numeric(base_y = peek(&t, BASE_Y)) || !numeric(timer = peek(&t, TIMER)) || timer->kind != 0 || prev == timer) {
        lamp_sway_dropped += n;
        return;
    }
    for (int k = 0; k < n; ++k) {
        const double *r = s->r[k];
        RV old = *prev;
        set_value(self, id(SPEED), (int)0x80000000u, &old);
        double bx = base_x->value + r[0];
        bx = bx - r[1];
        double by = base_y->value + r[2];
        by = by - r[3];
        float direction = point_direction(field(self, 0xb8), field(self, 0xbc), (float)bx, (float)by);
        /* motion_add's builtin: F_MotionAdd = AddTo_Speed(YYGetFloat(arg0), YYGetFloat(arg1)) */
        add_to_speed(self, direction, (float)motion_amount->value);
        prev->value = (double)field(self, 0xd4);
        prev->flags = speed_flags; prev->kind = 0;
        timer->value -= 1.0;
        RV speed = {(double)field(self, 0xd4), 0, 0};
        if (cmp(&speed, &ten, eps, true) >= 1) {
            RV cap = ten;
            set_value(self, id(SPEED), (int)0x80000000u, &cap);
        }
    }
    lamp_sway_deferred += n;
}
static void ABI __attribute__((naked, noinline))
orange_fallback(void *self __attribute__((unused)), void *other __attribute__((unused)),
                uint32_t flags __attribute__((unused)), Event event __attribute__((unused))) {
    __asm__ volatile("mov ip, sp\n\tsub.w ip, ip, #0x2b0\n\tstr r2, [ip]\n\tbx r3");
}
static void ABI __attribute__((used, noinline))
orange_inline_entry(void *self, void *other, uint32_t speed_flags) {
    Orange plan = {0};
    uint32_t t = PART_NOW(), tick = frame_counter ? *frame_counter : 0;
    Sway *s = frame_counter ? sway_slot(self, tick) : NULL;
    int pending = 0;
    if (s) {
        if (sway_skip(s, tick, sway_rate())) {
            /* L37's four draws, in its order */
            double *r = s->r[s->pending++];
            r[0] = runner_random(15.0); r[1] = runner_random(15.0);
            r[2] = runner_random(15.0); r[3] = runner_random(15.0);
            s->last = tick;
            ++lamp_sway_skips;
            PART(6, t);
            return;
        }
        if (s->pending && tick - s->last != 1) { lamp_sway_dropped += s->pending; s->pending = 0; }
        pending = s->pending;
        s->last = tick;
        s->eligible = 0;
    }
    int admitted = orange_admit(self, &plan, pending);
    if (pending && (!admitted || plan.path != MOTION)) {
        /* Not this tick's native motion path: the deferred steps on their own, then the event as usual */
        sway_apply(self, s, speed_flags);
        pending = 0;
        PART(7, t);
        admitted = orange_admit(self, &plan, 0);
    }
    if (!admitted) {
        ++light_fast_inline_fallbacks[ORANGE];
        orange_fallback(self, other, speed_flags, original[ORANGE]);
        PART(5, t);
        return;
    }
    ++light_fast_inline_hits[ORANGE];
    const RV ten = {10, 0, 0};
    if (plan.path == MOTION) {
        PART(0, t);
        RV old = *plan.prev;
        set_value(self, id(SPEED), (int)0x80000000u, &old);
        PART(1, t);
        /* Lamp sway: the skipped ticks' impulses, in order. Between them the original's speed = prev_speed is a
         * no-op (prev_speed was just set from speed; SetSpeed returns at once for an equal value), and a speed cap
         * in between would be undone by it, so only the impulses and timer decrements remain. */
        for (int k = 0; k < pending; ++k) {
            const double *r = s->r[k];
            double bx = plan.base_x->value + r[0];
            bx = bx - r[1];
            double by = plan.base_y->value + r[2];
            by = by - r[3];
            float direction = point_direction(field(self, 0xb8), field(self, 0xbc), (float)bx, (float)by);
            add_to_speed(self, direction, (float)motion_amount->value);
            plan.timer->value -= 1.0;
        }
        if (pending) {
            lamp_sway_deferred += pending;
            s->pending = 0;
            PART(7, t);
        }
        /* Explicit statements preserve x+, x-, y+, y- RNG ordering and
         * double arithmetic before the original float argument rounding. */
        double bx = plan.base_x->value + runner_random(15.0);
        bx = bx - runner_random(15.0);
        double by = plan.base_y->value + runner_random(15.0);
        by = by - runner_random(15.0);
        PART(2, t);
        float direction = point_direction(field(self, 0xb8), field(self, 0xbc),
                                          (float)bx, (float)by);
        PART(3, t);
        if (add_to_speed) {
            /* motion_add through YYGML_CallLegacyFunction is F_MotionAdd: AddTo_Speed(YYGetFloat(arg0),
             * YYGetFloat(arg1)) with two reals; the legacy wrapper only copies the arguments, sets the
             * (discarded) result to undefined and g_pFunction around the call. */
            add_to_speed(self, direction, (float)motion_amount->value);
        } else {
            RV angle = {(double)direction, 0, 0}, amount = *motion_amount;
            RV result = {0, 0, 5};
            RV *args[2] = {&angle, &amount};
            call_legacy(self, other, &result, 2, plan.motion_id, args);
        }
        PART(4, t);
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
    if (s && plan.path == MOTION && !plan.cap) {
        s->eligible = 1;
        s->timer = plan.timer->value;
    }
    if (plan.path == MOTION) PART(8, t);
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
    /* Lamp sway only: without these every Step runs (lampSway=1) */
    add_to_speed = (AddToSpeed)so_symbol(&yoyoloader_mod, "_ZN9CInstance11AddTo_SpeedEff");
    object_find = (ObjectFind)so_symbol(&yoyoloader_mod, "_Z11Object_FindPKc");
    frame_counter = (const uint32_t *)so_symbol(&yoyoloader_mod, "g_IOFrameCount");
    if (!add_to_speed || !object_find || !frame_counter) frame_counter = NULL;
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
        hook_raw(entries[i], (uintptr_t)replacements[i]);
        kuKernelFlushCaches((void *)entries[i], 8);
    }
    installed = true; return 1;
}

/* Profile report line (katana_prof.c, debugMode): per frame of the window; exp_swayab flips the rate after it */
#ifndef LIGHT_FAST_STUB
void lamp_sway_report(int frames) {
    static unsigned last_skips, last_deferred, last_dropped;
    static const char *const parts[9] = {"admit", "speed=prev", "random x4", "point_direction", "motion_add",
                                         "fallback", "skip", "deferred", "tail"};
    if (frames <= 0) return;
    if (lamp_sway_skips != last_skips || lamp_sway_deferred != last_deferred || lamp_sway_ab)
        debugPrintf("[LAMPSWAY] lampSway %d: skipped %.1f, deferred impulses %.1f, dropped %.1f per frame%s\n",
            sway_rate(), (float)(lamp_sway_skips - last_skips) / frames, (float)(lamp_sway_deferred - last_deferred) / frames,
            (float)(lamp_sway_dropped - last_dropped) / frames, lamp_sway_ab ? (lamp_sway_ab_off ? " | A/B: off" : " | A/B: on") : "");
    last_skips = lamp_sway_skips; last_deferred = lamp_sway_deferred; last_dropped = lamp_sway_dropped;
    if (light_parts) {
        char line[512];
        int len = snprintf(line, sizeof(line), "[LIGHTPARTS] ms/frame (calls/frame, us/call):");
        uint32_t total = 0;
        for (int i = 0; i < 9; ++i) {
            total += light_parts_us[i];
            if (light_parts_n[i] && len < (int)sizeof(line))
                len += snprintf(line + len, sizeof(line) - len, " %s %.3f (%.1f, %.2f)", parts[i],
                    light_parts_us[i] / 1000.0f / frames, (float)light_parts_n[i] / frames,
                    (float)light_parts_us[i] / light_parts_n[i]);
            light_parts_us[i] = light_parts_n[i] = 0;
        }
        debugPrintf("%s | total %.3f\n", line, total / 1000.0f / frames);
    }
    if (lamp_sway_ab) lamp_sway_ab_off = !lamp_sway_ab_off;
}
#endif
