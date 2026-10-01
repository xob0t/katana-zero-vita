/* libyoyo.so ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * ARM softfp ABI. Install before execution, with 16 bytes of loader-owned RX.
 * Combined SetPosition and native momentum, guarded for ordinary instances.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifdef ROPE_STUB
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif
#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef RV *ABI (*script_fn)(void *, void *, RV *, int, RV **);
typedef RV *ABI (*ref_fn)(void *, int);
typedef int ABI (*compare_fn)(const RV *, const RV *, double, bool);
typedef void ABI (*position_fn)(void *, float, float);
typedef void ABI (*dirty_fn)(void *);
enum { FR, A, B, REST, DIST, DIR, NEG, MX, MY, STRETCH, X, Y, STATIC,
       HSP, VSP, POINT_DISTANCE, NSLOTS };
static const char *const names[NSLOTS] = {
    "g_VAR_fr", "g_VAR_vertA", "g_VAR_vertB", "g_VAR_argdist",
    "g_VAR_dist", "g_VAR_dir", "g_VAR_neg_dir", "g_VAR_midpoint_x",
    "g_VAR_midpoint_y", "g_VAR_stretch", "g_VAR_x", "g_VAR_y",
    "g_VAR_obStatic", "g_VAR_hsp", "g_VAR_vsp", "g_FUNC_point_distance"
};
static const uint32_t *slots[NSLOTS];
static void **global;
static const double *epsilon;
static const uint8_t *profile;
static const uint8_t *managed_enabled;
static const int *managed_guard;
static script_fn original;
static ref_fn standard_get, standard_ref;
static compare_fn compare;
static const void *instance_hash;
static position_fn position;
static dirty_fn dirty;
static const int *sprite_count;
static const void *sprite_items;
static bool installed;
unsigned rope_fast_hits, rope_fast_fallbacks;
static uint32_t word(const void *p, size_t o) {
    uint32_t v; memcpy(&v, (const uint8_t *)p + o, 4); return v;
}
static int slot(int n) { return (int)slots[n][1]; }
/* Read-only local hash lookup. Reject inherited/missing variables, managed
 * values, and nonstandard vtables before ANY original-observable operation.
 * Existing local scalar entries take the return at f21988 / f21ac8.
 */
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
/* Diagnostics: which admission check rejects (source line histogram) */
unsigned rf_fail_line[16], rf_fail_count[16];
static int rf_fail(unsigned line) {
    for (int i = 0; i < 16; i++) {
        if (rf_fail_line[i] == line || !rf_fail_line[i]) { rf_fail_line[i] = line; rf_fail_count[i]++; break; }
    }
    return 0;
}
static int table(void *p, Table *out) {
    if (!p) return rf_fail(__LINE__);
    const void *vt = (const void *)(uintptr_t)word(p, 0);
    if (word(vt, 8) != (uintptr_t)standard_get ||
        word(vt, 12) != (uintptr_t)standard_ref) return rf_fail(__LINE__);
    const void *ht = (const void *)(uintptr_t)word(p, 0x24);
    if (!ht) return rf_fail(__LINE__);
    out->mask = word(ht, 8);
    out->entries = (const uint8_t *)(uintptr_t)word(ht, 16);
    if (!out->entries || out->mask > 65535 || (out->mask & (out->mask + 1)))
        return rf_fail(__LINE__);
    return 1;
}
static RV *peek(const Table *t, int n) {
    uint32_t mask = t->mask, key = ((uint32_t)slot(n) + 1) & 0x7fffffff;
    const uint8_t *entries = t->entries;
    if (!key) return NULL;
    uint32_t i = key & mask;
    for (uint32_t d = 0; d <= mask; ++d, i = (i + 1) & mask) {
        uint32_t k = word(entries + 12 * i, 8);
        if (k == key) return (RV *)(uintptr_t)word(entries + 12 * i, 0);
        if (!k || d > ((i - (k & mask)) & mask)) return NULL;
    }
    return NULL;
}
static inline __attribute__((always_inline)) int numeric(const RV *v) {
    return v && (v->kind == 0 || v->kind == 13) &&
           (word(v, 4) & 0x7fffffffu) < 0x41300000u;
}
static int aliases(const RV *v, RV *result, RV **args) {
    return v && (v == result || v == args[0] || v == args[1] || v == args[2]);
}
/* FindInstance's ID-only hash branch f8a904..f8a968, without its error call. */
static void *find_id(int id) {
    const void *buckets = (const void *)(uintptr_t)word(instance_hash, 0);
    uint32_t mask = word(instance_hash, 4);
    if (!buckets || mask > 1048575 || (mask & (mask + 1))) return NULL;
    const void *p = (const void *)(uintptr_t)word(buckets, ((uint32_t)id & mask)*8);
    for (uint32_t count = 0; p && count < 1048576; ++count) {
        if (word(p, 8) == (uint32_t)id)
            return (void *)(uintptr_t)word(p, 12);
        p = (const void *)(uintptr_t)word(p, 4);
    }
    return NULL;
}
static int stack_value(const RV *v, const void *anchor) {
    /* YYC End Step passes temporary RVs above this frame on the ARM stack.
     * Restrict to that ownership convention, so parameters cannot alias the
     * runner's heap-backed instance variables. Heap/native callers fall back.
     */
    return ((uintptr_t)v & 3u) == 0 &&
           (uintptr_t)v > (uintptr_t)anchor &&
           (uintptr_t)v - (uintptr_t)anchor < 65536;
}
/* Node arguments arrive as instance references (kind 15, type word 0x04000001, ID in the
 * low word) in this build, or as REAL IDs. Returns the ID or -1. */
static int node_id(const RV *v) {
    if (!v) return -1;
    if (v->kind == 15 && word(v, 4) == 0x04000001u)
        return (int)word(v, 0);
    if (v->kind == 0 && v->value >= 100000 && v->value < 1048576 && v->value == (int)v->value)
        return (int)v->value;
    return -1;
}
typedef struct {
    RV *fr, *scratch[9], *stat[2], *hsp[2], *vsp[2];
    void *node[2];
} Admission;
static int admit(void *self, RV *result, int argc, RV **args, Admission *out) {
    Table caller, globals;
    int anchor;
    if (*profile || !self || !result || argc != 3 || !args || !table(*global, &globals) ||
        !isfinite(*epsilon) || *epsilon < 0) return rf_fail(__LINE__);
    RV *fr = peek(&globals, FR);
    if (!numeric(fr) || result == fr) return rf_fail(__LINE__);
    out->fr = fr;
    /* REAL/REAL zero comparison e3ee1c..e3f338 is abs(fr) <= epsilon.
     * This only selects the short guard. The runner still performs the test.
     * Frozen calls never read arguments or self's variable table.
     */
    if (fr->value == 0.0 || (fr->kind == 0 && fabs(fr->value) <= *epsilon)) return 1;
    if (*profile || !self || argc != 3 || !args) return rf_fail(__LINE__);
    int id_a = node_id(args[0]), id_b = node_id(args[1]);
    if (id_a < 100000 || id_b < 100000 || !numeric(args[2])) return rf_fail(__LINE__);
    if (args[2]->kind != 0) return rf_fail(__LINE__);
    if (!stack_value(result, &anchor)) return rf_fail(__LINE__);
    if (!stack_value(args[0], &anchor) || !stack_value(args[1], &anchor)) return rf_fail(__LINE__);
    if (!stack_value(args[2], &anchor)) return rf_fail(__LINE__);
    if (args[2]->value < 0) return rf_fail(__LINE__);
    if (!table(self, &caller)) return rf_fail(__LINE__);
    for (int n = A; n <= STRETCH; ++n) {
        RV *v = peek(&caller, n);
        if (!v || (v->kind != 0 && !((n == A || n == B) && v->kind == 15)) || aliases(v, result, args))
            return rf_fail(__LINE__);
        out->scratch[n - A] = v;
    }
    /* Kind 15 references are inline, unmanaged payloads. The YYC release mask
     * excludes bit 15, so copying/replacing vertA/B needs no retain/release.
     * Missing or owned scratch falls back once to create/release the slots. */
    if (aliases(fr, result, args) || result == args[0] ||
        result == args[1] || result == args[2]) return rf_fail(__LINE__);
    void *a = find_id(id_a), *b = find_id(id_b);
    if (!a || !b || a == self || b == self) return rf_fail(__LINE__);
    void *nodes[2] = {a, b};
    out->node[0] = a; out->node[1] = b;
    for (int i = 0; i < 2; ++i) {
        void *p = nodes[i];
        Table vars;
        if (word(p, 4) || word(p, 0x70) || word(p, 0x74) ||
            word(p, 0x50) != 1 || (word(p, 0x7c) & 0x100007u) != 4u ||
            (word(p, 0xb8) & 0x7fffffffu) >= 0x49800000u ||
            (word(p, 0xbc) & 0x7fffffffu) >= 0x49800000u ||
            !table(p, &vars)) return rf_fail(__LINE__);
        const void *definition = (const void *)(uintptr_t)word(p, 0x68);
        /* f8938c..f893b0 and f89478..f89564: marked definitions only change
         * REAL builtin writes when checking is enabled and guard >= 1.
         * Guard < 1 checks kind 15 RHS references, but our builtin RHSs are
         * REAL. Existing local scalar getter/lvalue slots are unaffected. */
        if (*managed_enabled && *managed_guard >= 1 && definition &&
            (word(definition, 0x50) & 0x80u)) return rf_fail(__LINE__);
        int image = (int)word(p, 0x88);
        if (image >= 0 && image < *sprite_count) {
            const void *items = (const void *)(uintptr_t)word(sprite_items, 4);
            if (!items) return rf_fail(__LINE__);
            const void *sp = (const void *)(uintptr_t)word(items, (size_t)image * 4);
            if (sp && word(sp, 0x84) == 2) return rf_fail(__LINE__);
        }
        for (size_t o = 0xd0; o <= 0xe8; o += 4)
            if ((word(p, o) & 0x7fffffffu) >= 0x49800000u) return rf_fail(__LINE__);
        for (int n = STATIC; n <= VSP; ++n) {
            const RV *v = peek(&vars, n);
            if (!numeric(v) || aliases(v, result, args)) return rf_fail(__LINE__);
            if (n == STATIC) out->stat[i] = (RV *)v;
            if (n == HSP) out->hsp[i] = (RV *)v;
            if (n == VSP) out->vsp[i] = (RV *)v;
        }
    }
    return 1;
}

static float scalar(const void *p, size_t o) {
    float f; memcpy(&f, (const uint8_t *)p + o, 4); return f;
}
static void put_float(void *p, size_t o, float f) {
    memcpy((uint8_t *)p + o, &f, 4);
}
/* Reproduce the runner's explicit f32 stages and 0.0001 snapping. These
 * boundaries prevent repeated constraints from drifting at integer axes.
 * Double arithmetic remains in caller expressions and midpoint addition.
 */
static float radians(float d) {
    volatile float product = d * 3.1415927410125732421875f;
    return product / 180.0f;
}
static float angle(float x, float y, float previous) {
    if (x == 0) return y > 0 ? 270.0f : y < 0 ? 90.0f : previous;
    volatile float product = atan2f(y, x) * 180.0f;
    float a = product / 3.1415927410125732421875f;
    return a > 0 ? 360.0f - a : -a;
}
static float snap(float x) {
    float near = (float)lrintf(x);
    return fabsf(x - near) < 0.0001f ? near : x;
}
static float projection(float length, float trig) {
    float x = length * trig;
    /* lengthdir uses modff, not nearest-integer snapping. */
    float integral = truncf(x);
    return fabsf(x - integral) < 0.0001f ? integral : x;
}
static float magnitude(float x, float y) {
    volatile float yy = y * y, xx = x * x;
    return sqrtf(yy + xx);
}
static void add_momentum(void *node, RV *hsp, RV *vsp, float dir, float amount,
                         int position_changed) {
    float old_h = scalar(node, 0xe4), old_v = scalar(node, 0xe8);
    float d = scalar(node, 0xd0);
    float input_h = (float)hsp->value, input_v = (float)vsp->value;
    float rad = radians(dir);
    volatile float dx = amount * cosf(rad), dy = amount * sinf(rad);
    float h = input_h + dx, v = input_v - dy;
    /* Intermediate SetHspeed/SetVspeed directions matter only when the
     * final vector is zero. Their speed values are overwritten by AddTo_Speed. */
    if (h == 0 && v == 0) {
        if (old_h != input_h) d = fmodf(snap(angle(input_h, old_v, d)), 360.0f);
        if (old_v != input_v) d = fmodf(snap(angle(input_h, input_v, d)), 360.0f);
    }
    d = fmodf(snap(angle(h, v, d)), 360.0f);
    hsp->value = (double)h; hsp->flags = 0; hsp->kind = 0;
    vsp->value = (double)v; vsp->flags = 0; vsp->kind = 0;
    /* AddTo_Speed snaps speed to zero below 0.0001. SetSpeed(0) then
     * returns without changing tiny components. For larger speed it snaps
     * both computed zero components to zero. */
    if (fabsf(h) >= 0.0001f || fabsf(v) >= 0.0001f ||
        magnitude(h, v) >= 0.0001f) { h = 0; v = 0; }
    put_float(node, 0xd0, d); put_float(node, 0xd4, 0);
    put_float(node, 0xe4, h); put_float(node, 0xe8, v);
    uint32_t flags = word(node, 0x7c) | 8u;
    memcpy((uint8_t *)node + 0x7c, &flags, 4);
    if (!position_changed) dirty(node);
}
static void assign(RV *dst, double value, int clear_flags) {
    dst->value = value; dst->kind = 0;
    if (clear_flags) dst->flags = 0;
}
RV *ABI rope_constraint_fast(void *self, void *other, RV *result,
                              int argc, RV **args) {
    Admission path; path.node[0] = NULL; path.node[1] = NULL;
    if (!admit(self, result, argc, args, &path)) {
        ++rope_fast_fallbacks;
        return original(self, other, result, argc, args);
    }
    ++rope_fast_hits;
    RV zero = {0, 0, 0};
    uint32_t z = 0; memcpy(result, &z, 4); result->kind = 5;
    if (compare(path.fr, &zero, *epsilon, false) == 0) return result;
    void *a = path.node[0], *b = path.node[1];
    *path.scratch[A - A] = *args[0]; *path.scratch[B - A] = *args[1];
    *path.scratch[REST - A] = *args[2];
    double ax = scalar(a, 0xb8), ay = scalar(a, 0xbc);
    double bx = scalar(b, 0xb8), by = scalar(b, 0xbc);
    float dx = (float)(bx - ax), dy = (float)(by - ay);
    double dist = magnitude(dx, dy), rest = args[2]->value;
    double dir = angle(dx, dy, 0), neg = angle(-dx, -dy, 0);
    float rad = radians((float)dir), nr = radians((float)neg);
    float cx = cosf(rad), sy = -sinf(rad), ncx = cosf(nr), nsy = -sinf(nr);
    double mx = ax + (double)projection((float)(dist / 2), cx);
    double my = ay + (double)projection((float)(dist / 2), sy);
    double stretch = (fabs(rest - dist) / 10.0) * 3.0;
    assign(path.scratch[DIST - A], dist, 1);
    assign(path.scratch[DIR - A], dir, 0); assign(path.scratch[NEG - A], neg, 0);
    assign(path.scratch[MX - A], mx, 1); assign(path.scratch[MY - A], my, 1);
    assign(path.scratch[STRETCH - A], stretch, 0);
    if (compare(path.scratch[DIST - A], path.scratch[REST - A], *epsilon, true) < 1)
        return result;
    RV bool_zero = {0, 0, 13};
    int sa = compare(path.stat[0], &bool_zero, *epsilon, false) != 0;
    int sb = compare(path.stat[1], &bool_zero, *epsilon, false) != 0;
    if (!sa) {
        float len = (float)(sb ? rest : rest / 2);
        float x = (float)((sb ? bx : mx) + (double)projection(len, ncx));
        float y = (float)((sb ? by : my) + (double)projection(len, nsy));
        int changed = x != scalar(a, 0xb8) || y != scalar(a, 0xbc);
        position(a, x, y);
        add_momentum(a, path.hsp[0], path.vsp[0], (float)dir, (float)stretch, changed);
    }
    if (!sb) {
        float len = (float)(sa ? rest : rest / 2);
        /* Fresh A coordinates match the original second-end pass. */
        float x = (float)((sa ? (double)scalar(a, 0xb8) : mx) + (double)projection(len, cx));
        float y = (float)((sa ? (double)scalar(a, 0xbc) : my) + (double)projection(len, sy));
        int changed = x != scalar(b, 0xb8) || y != scalar(b, 0xbc);
        position(b, x, y);
        add_momentum(b, path.hsp[1], path.vsp[1], (float)neg, (float)stretch, changed);
    }
    return result;
}
int rope_fast_install(void *rx, size_t rx_size) {
    if (installed || !rx || ((uintptr_t)rx & 3) || rx_size < 16) return 0;
    uintptr_t entry = so_symbol(&yoyoloader_mod,
        "_Z37gml_Script_softbody_constraint_strongP9CInstanceS0_R8YYRValueiPPS1_");
    if (!entry || (entry & 3) || word((void *)entry, 0) != 0xe92d4ff0 ||
        word((void *)entry, 4) != 0xe28db01c) return 0;
    for (int n = 0; n < NSLOTS; ++n) {
        slots[n] = (const uint32_t *)so_symbol(&yoyoloader_mod, names[n]);
        if (!slots[n]) return 0;
    }
#define RESOLVE(var, type, name) do { \
    var = (type)so_symbol(&yoyoloader_mod, name); if (!var) return 0; \
} while (0)
    RESOLVE(global, void **, "g_pGlobal");
    RESOLVE(epsilon, const double *, "g_GMLMathEpsilon");
    RESOLVE(profile, const uint8_t *, "g_bProfile");
    RESOLVE(managed_enabled, const uint8_t *, "g_enableManagedObjects");
    RESOLVE(managed_guard, const int *, "g_nManagedObjectGuard");
    RESOLVE(standard_get, ref_fn, "_ZN12YYObjectBase19InternalGetYYVarRefEi");
    RESOLVE(standard_ref, ref_fn, "_ZN12YYObjectBase20InternalGetYYVarRefLEi");
    RESOLVE(compare, compare_fn, "_Z12YYCompareValRK6RValueS1_db");
    RESOLVE(instance_hash, const void *, "_ZN9CInstance14ms_ID2InstanceE");
    RESOLVE(position, position_fn, "_ZN9CInstance11SetPositionEff");
    RESOLVE(dirty, dirty_fn, "_Z18CollisionMarkDirtyP9CInstance");
    RESOLVE(sprite_count, const int *, "g_NumberOfSprites");
    RESOLVE(sprite_items, const void *, "g_SpriteItems");
#undef RESOLVE
    uint32_t code[4] = {0xe92d4ff0, 0xe28db01c, 0xe51ff004, (uint32_t)(entry+8)};
    kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof(code));
    kuKernelFlushCaches(rx, sizeof(code)); original = (script_fn)rx;
    hook_addr(entry, (uintptr_t)rope_constraint_fast);
    kuKernelFlushCaches((void *)entry, 8); installed = true;
    return 1;
}



