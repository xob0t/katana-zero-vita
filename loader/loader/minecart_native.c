/* libyoyo.so ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * Compile for ARM softfp, without fast-math. Includes native master Draw and generic fastline entry hooks.
 * rx_alloc must return 64 aligned executable bytes, retained until unload.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifdef RAIL_TEST
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
typedef void ABI (*setter_fn)(void *, int, int, RV *);
typedef float ABI (*direction_fn)(float, float, float, float);
typedef float ABI (*length_fn)(float, float);
typedef double ABI (*clamp_fn)(double, double, double);
typedef void *ABI (*sprite_fn)(int);
typedef void ABI (*draw_fn)(void *, int, float, float, float, float, float,
                           float, int, float);
typedef void ABI (*builtin_fn)(RV *, void *, void *, int, RV *);
typedef struct { const char *name; builtin_fn call; int argc; } Function;
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;
typedef struct { double l, t, r, b; } View;
typedef struct { int w, h, ox, oy; } Sprite;
enum { X, Y, N, RX, RY, RANGLE, CUTOFF, XV, YV, WV, HV, SET, INDEX, COUNTER, PANGLE, NODE, NUMBER, NVARS };
static const char *const var_names[NVARS] = {"x", "y", "n", "rx", "ry",
    "rangle", "cutoff", "xview", "yview", "wview", "hview",
    "set", "i", "rail_counter", "pangle", "node", "number"};
static const uint32_t *vars[NVARS];
enum { DISTANCE, ALPHA, ANGLEDIFF, FLOOR, PART, WIDTH, NFUNCS };
static const char *const fn_names[NFUNCS] = {"point_distance", "draw_get_alpha",
    "angle_difference", "floor", "draw_sprite_part_ext", "sprite_get_width"};
static const char *const fn_symbols[NFUNCS] = {
    "_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_",
    "_Z14F_DrawGetAlphaR6RValueP9CInstanceS2_iPS_",
    "_Z21F_MathAngleDifferenceR6RValueP9CInstanceS2_iPS_",
    "_Z7F_FloorR6RValueP9CInstanceS2_iPS_",
    "_Z19F_DrawSpritePartExtR6RValueP9CInstanceS2_iPS_",
    "_Z16F_SpriteGetWidthR6RValueP9CInstanceS2_iPS_"};
static const uint32_t *descriptors[NFUNCS];
static builtin_fn builtins[NFUNCS];
static Function **functions, **current_function;
static void **global;
static Trace **trace_start;
static const double *epsilon;
static const uint8_t *profile, *managed_enabled;
static const int *managed_guard, *global_start;
static const void *const *active_shader;
static const float *matrices;
static const void *builtin_variables;
static uintptr_t xy_get[2], xy_set[2];
static ref_fn standard_get, standard_ref;
static compare_fn compare;
static setter_fn setter;
static direction_fn direction;
static length_fn length_x, length_y;
static clamp_fn clamp;
static sprite_fn sprite_data;
static draw_fn draw_sprite;
static script_fn original_rail, original_bottom, inside;
static int installed;
unsigned minecart_rail_native_hits, minecart_rail_native_bottom_hits, minecart_rail_native_fallbacks;
unsigned minecart_rail_native_submitted, minecart_rail_native_culled, minecart_rail_native_samples;
unsigned minecart_native_reject_line[64], minecart_native_reject_count[64];
unsigned minecart_native_reject_overflow;

static uint32_t word(const void *p, size_t o) {
    uint32_t v; memcpy(&v, (const uint8_t *)p + o, 4); return v;
}
static float scalar(const void *p, size_t o) {
    float v; memcpy(&v, (const uint8_t *)p + o, 4); return v;
}
static int reject(unsigned line) {
    for (int i = 0; i < 64; ++i)
        if (!minecart_native_reject_line[i] || minecart_native_reject_line[i] == line) {
            minecart_native_reject_line[i] = line; ++minecart_native_reject_count[i];
            return 0;
        }
    ++minecart_native_reject_overflow; return 0;
}
#define REQUIRE(c) do { if (!(c)) return reject(__LINE__); } while (0)
static RV real(double v) { RV r = {v, 0, 0}; return r; }
static RV asset(int id) {
    RV r = {0, 0, 15}; uint32_t bits[2] = {(uint32_t)id, 0x01000001};
    memcpy(&r.value, bits, 8); return r;
}
static int unmanaged_result(const RV *v) {
    /* Original generated code releases string/array/object results before
     * writing undefined. Ownership-bearing or unknown result tags delegate. */
    return v && (v->kind == 0 || v->kind == 5 || v->kind == 7 ||
                 v->kind == 10 || v->kind == 13);
}
static int numeric(const RV *v) {
    return v && v->kind == 0 && isfinite(v->value) && fabs(v->value) < 1048576;
}
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
static int table(void *owner, Table *t) {
    REQUIRE(owner);
    const void *vt = (const void *)(uintptr_t)word(owner, 0);
    REQUIRE(vt && word(vt, 8) == (uintptr_t)standard_get &&
            word(vt, 12) == (uintptr_t)standard_ref);
    const void *ht = (const void *)(uintptr_t)word(owner, 0x24);
    REQUIRE(ht);
    t->mask = word(ht, 8);
    t->entries = (const uint8_t *)(uintptr_t)word(ht, 16);
    REQUIRE(t->entries && t->mask <= 65535 && !(t->mask & (t->mask + 1)));
    return 1;
}
static const RV *peek(const Table *t, int n) {
    uint32_t key = (vars[n][1] + 1) & 0x7fffffff, i = key & t->mask;
    if (!key) return NULL;
    for (uint32_t d = 0; d <= t->mask; ++d, i = (i + 1) & t->mask) {
        const uint8_t *e = t->entries + 12 * i;
        uint32_t k = word(e, 8);
        if (k == key) return (const RV *)(uintptr_t)word(e, 0);
        if (!k || d > ((i - (k & t->mask)) & t->mask)) return NULL;
    }
    return NULL;
}
/* Registry initialization is intentionally deferred to invocation. Install
 * can run before registration; all descriptors and call pointers are live.
 * Ordinary YYC descriptor IDs are valid indices once the registry exists.
 */
static int registry(void) {
    REQUIRE(*functions);
    for (int i = 0; i < NFUNCS; ++i) {
        REQUIRE(descriptors[i][1] < 16384);
        REQUIRE((*functions)[descriptors[i][1]].call == builtins[i]);
    }
    return 1;
}
static void builtin(int n, RV *out, void *self, void *other, int argc, RV *args) {
    Function *saved = *current_function;
    uint32_t zero = 0;
    memcpy(&out->value, &zero, 4); out->flags = 0; out->kind = 5;
    *current_function = *functions + descriptors[n][1];
    builtins[n](out, self, other, argc, args);
    *current_function = saved;
}
static int sprite(int id, Sprite *out) {
    const void *sp = sprite_data(id);
    REQUIRE(sp && word(sp, 0x84) == 0); /* ordinary bitmap */
    out->w = (int)word(sp, 0x5c); out->h = (int)word(sp, 0x60);
    out->ox = (int)word(sp, 0x64); out->oy = (int)word(sp, 0x68);
    REQUIRE(out->w > 0 && out->w < 65536 && out->h > 0 && out->h < 65536 &&
            out->ox > -65536 && out->ox < 65536 &&
            out->oy > -65536 && out->oy < 65536);
    return 1;
}
static int view(void *self, RV *result, View *v) {
    Table t;
    REQUIRE(self && unmanaged_result(result) && !*profile && !*active_shader &&
            isfinite(*epsilon) && *epsilon >= 0 && *epsilon < 1);
    /* m_CurrentMatrix holds view, projection, world (matrix_view/projection/world = 0/1/2);
     * checked on the Vita: world is identity, view/projection carry the camera. A changed
     * world transform can move an otherwise off-view piece onto the screen, so delegate it. */
    for (int i = 0; i < 16; ++i)
        REQUIRE(matrices[32 + i] == ((i % 5 == 0) ? 1.0f : 0.0f));
    REQUIRE(registry() && table(*global, &t));
    const RV *a[4];
    for (int i = 0; i < 4; ++i) {
        a[i] = peek(&t, XV + i); REQUIRE(numeric(a[i]) && a[i] != result);
    }
    REQUIRE(a[2]->value > 0 && a[3]->value > 0);
    v->l = a[0]->value; v->t = a[1]->value;
    v->r = v->l + a[2]->value; v->b = v->t + a[3]->value;
    /* Loader addition: xview/yview need not be the camera actually used (shake, zoom). Also
     * cover the world rectangle the current view/projection matrices show, plus a margin. */
    const float *vm = matrices, *pm = matrices + 16;
    REQUIRE(vm[1] == 0 && vm[4] == 0 && pm[1] == 0 && pm[4] == 0 && vm[0] != 0 && vm[5] != 0 &&
            pm[0] != 0 && pm[5] != 0);
    double x0 = ((-1 - pm[12]) / pm[0] - vm[12]) / vm[0], x1 = ((1 - pm[12]) / pm[0] - vm[12]) / vm[0];
    double y0 = ((-1 - pm[13]) / pm[5] - vm[13]) / vm[5], y1 = ((1 - pm[13]) / pm[5] - vm[13]) / vm[5];
    REQUIRE(isfinite(x0) && isfinite(x1) && isfinite(y0) && isfinite(y1));
    v->l = fmin(v->l, fmin(x0, x1)) - 32; v->r = fmax(v->r, fmax(x0, x1)) + 32;
    v->t = fmin(v->t, fmin(y0, y1)) - 32; v->b = fmax(v->b, fmax(y0, y1)) + 32;
    return 1;
}
/* A one-source-pixel filtering apron plus four world pixels covers f32
 * vertex/trig rounding in the admitted coordinate domain. Rotated sprites
 * use an L1 radius about their actual origin, valid for every rotation.
 * Angle-zero sprites use the tighter signed-scale rectangle. These are full
 * image bounds, never collision-mask bounds or a test of the anchor alone.
 */
static int overlap(const View *v, double x, double y, double l, double t,
                   double r, double b) {
    return x + r + 4 >= v->l && x + l - 4 <= v->r &&
           y + b + 4 >= v->t && y + t - 4 <= v->b;
}
static int visible(const View *v, const Sprite *s, float x, float y,
                   float sx, float sy, float angle) {
    double l = (-s->ox - 1.0) * sx, r = (s->w - s->ox + 1.0) * sx;
    double t = (-s->oy - 1.0) * sy, b = (s->h - s->oy + 1.0) * sy;
    if (l > r) { double q = l; l = r; r = q; }
    if (t > b) { double q = t; t = b; b = q; }
    if (angle != 0) {
        double radius = fmax(fabs(l), fabs(r)) + fmax(fabs(t), fabs(b));
        l = t = -radius; r = b = radius;
    }
    return overlap(v, x, y, l, t, r, b);
}
static void emit(void *self, const View *v, const Sprite *s, int id, float frame,
                 float x, float y, float sx, float sy, float angle,
                 int colour, float alpha) {
    if (!visible(v, s, x, y, sx, sy, angle)) { ++minecart_rail_native_culled; return; }
    ++minecart_rail_native_submitted;
    draw_sprite(self, id, frame, x, y, sx, sy, angle, colour, alpha);
}
static void undefined(RV *result) {
    uint32_t zero = 0; memcpy(&result->value, &zero, 4); result->kind = 5;
}
static int rail_admit(void *self, RV *result, int argc, RV **args, View *v,
                      Sprite *rail, Sprite *white) {
    REQUIRE(argc >= 4 && args && view(self, result, v));
    for (int i = 0; i < 4; ++i) REQUIRE(numeric(args[i]) && args[i] != result);
    REQUIRE(word(self, 0x50) == 1 &&
            isfinite(scalar(self, 0xb8)) && isfinite(scalar(self, 0xbc)));
    for (int i = 0; i < 2; ++i) {
        REQUIRE(vars[i][1] < 1024 && (int)vars[i][1] < *global_start);
        const uint8_t *cell = (const uint8_t *)builtin_variables + 16 * vars[i][1];
        REQUIRE(word(cell, 4) == xy_get[i] && word(cell, 8) == xy_set[i] && word(cell, 12) != 0);
    }
    const void *definition = (const void *)(uintptr_t)word(self, 0x68);
    REQUIRE(!(*managed_enabled && *managed_guard >= 1 && definition &&
             (word(definition, 0x50) & 0x80)));
    REQUIRE(sprite(1664, rail) && rail->w == 6 && sprite(2184, white));
    return 1;
}
RV *ABI minecart_rail_native_draw(void *self, void *other, RV *result, int argc, RV **args) {
    View v; Sprite rail, white;
    if (!rail_admit(self, result, argc, args, &v, &rail, &white)) {
        ++minecart_rail_native_fallbacks; return original_rail(self, other, result, argc, args);
    }
    RV coords[4]; for (int i = 0; i < 4; ++i) coords[i] = *args[i];
    Trace trace = {*trace_start, "gml_Script_draw_rail", 12}; *trace_start = &trace;
    undefined(result); ++minecart_rail_native_hits;
    double angle = direction((float)coords[0].value, (float)coords[1].value,
                             (float)coords[2].value, (float)coords[3].value);
    RV distance = real(0); trace.line = 14;
    builtin(DISTANCE, &distance, self, other, 4, coords);
    RV saved_x = real(scalar(self, 0xb8)), saved_y = real(scalar(self, 0xbc));
    RV travelled = real(0), a = real(angle), zero = real(0), ninety = real(90);
    double dx = length_x(12, (float)angle), dy = length_y(12, (float)angle);
    int horizontal = compare(&a, &zero, *epsilon, false) == 0;
    int low = compare(&a, &ninety, *epsilon, true) < 0;
    for (;;) {
        trace.line = 20;
        int cmp = compare(&travelled, &distance, *epsilon, true);
        if (cmp >= 0 || cmp == -2) break;
        ++minecart_rail_native_samples;
        trace.line = 22; setter(self, (int)vars[X][1], INT32_MIN, &coords[0]);
        trace.line = 23; setter(self, (int)vars[Y][1], INT32_MIN, &coords[1]);
        double x = coords[0].value, y = coords[1].value;
        if (!horizontal) {
            double x2 = x + (low ? -40 : 40);
            trace.line = low ? 36 : 43;
            emit(self, &v, &rail, 1664, 0, (float)x2, (float)y, 2, -2,
                 (float)angle, 0xffffff, 1);
            /* Conversion of x and x +/- 40 can change the f32 distance near
             * exponent boundaries. Recompute with the actual runner builtin,
             * rather than hardcoding length 40 or assuming a fixed offset.
             */
            Trace line = {*trace_start, "gml_Script_draw_fastline_width_col", 16};
            *trace_start = &line;
            RV endpoints[4] = {real(x), real(y), real(x2), real(y)};
            RV dist = real(0), alpha = real(0);
            builtin(DISTANCE, &dist, self, other, 4, endpoints);
            line.line = 17;
            double dir = direction((float)x, (float)y, (float)x2, (float)y);
            float px = (float)(x + (double)length_x(1, (float)(dir + 90)));
            float py = (float)(y + (double)length_y(1, (float)(dir + 90)));
            line.line = 18;
            builtin(ALPHA, &alpha, self, other, 0, NULL);
            emit(self, &v, &white, 2184, 0, px, py, (float)dist.value, 2,
                 (float)dir, 4210752, (float)alpha.value);
            *trace_start = line.previous;
        }
        trace.line = horizontal ? 31 : low ? 38 : 45;
        emit(self, &v, &rail, 1664, 0, (float)x, (float)y, 2, 2,
             (float)angle, 0xffffff, 1);
        /* The double additions retain every original sample's rounding phase. */
        coords[0].value += dx; coords[1].value += dy; travelled.value += 12;
    }
    trace.line = 53; setter(self, (int)vars[X][1], INT32_MIN, &saved_x);
    trace.line = 54; setter(self, (int)vars[Y][1], INT32_MIN, &saved_y);
    *trace_start = trace.previous; return result;
}
static int bottom_admit(void *self, RV *result, View *v, Sprite *sp,
                        int *count, const RV **arrays) {
    Table t;
    REQUIRE(view(self, result, v) && table(self, &t) && sprite(1663, sp));
    const RV *n = peek(&t, N);
    REQUIRE(numeric(n) && n != result && n->value >= 0 && n->value <= 8192 &&
            n->value == (int)n->value);
    *count = (int)n->value;
    if (!*count) return 1;
    for (int i = 0; i < 4; ++i) {
        const RV *a = peek(&t, RX + i);
        REQUIRE(a && a != result && a->kind == 2);
        const void *h = (const void *)(uintptr_t)word(a, 0);
        REQUIRE(h && word(h, 28) >= (uint32_t)*count);
        arrays[i] = (const RV *)(uintptr_t)word(h, 4); REQUIRE(arrays[i]);
        for (int j = 0; j < *count; ++j)
            REQUIRE(numeric(arrays[i] + j) && arrays[i] + j != result);
    }
    return 1;
}
RV *ABI minecart_rail_native_bottom(void *self, void *other, RV *result, int argc, RV **args) {
    View v; Sprite sp; int count = 0; const RV *arrays[4];
    if (!bottom_admit(self, result, &v, &sp, &count, arrays)) {
        ++minecart_rail_native_fallbacks; return original_bottom(self, other, result, argc, args);
    }
    Trace trace = {*trace_start, "gml_Script_draw_rail_bottom", 7};
    *trace_start = &trace; undefined(result); ++minecart_rail_native_bottom_hits;
    RV zero = real(0), ninety = real(90);
    for (int i = 0; i < count; ++i) {
        RV points[3] = {arrays[0][i], arrays[1][i], real(150)}, accepted = real(0);
        RV *pointers[3] = {points, points + 1, points + 2};
        trace.line = 9; inside(self, other, &accepted, 3, pointers);
        if (accepted.value == 0) continue;
        RV ad_args[2] = {arrays[2][i], real(0)}, difference = real(0), f = real(0);
        int clipped = compare(arrays[3] + i, &zero, *epsilon, true) >= 0;
        int low = compare(arrays[2] + i, &ninety, *epsilon, true) < 0;
        trace.line = clipped ? (low ? 15 : 19) : (low ? 26 : 30);
        builtin(ANGLEDIFF, &difference, self, other, 2, ad_args);
        RV quotient = real(fabs(difference.value) / 13);
        builtin(FLOOR, &f, self, other, 1, &quotient);
        double frame = clamp(4 - f.value, 0, 4);
        float x = (float)arrays[0][i].value, y = (float)arrays[1][i].value;
        if (clipped) {
            double h = arrays[3][i].value / 2;
            /* draw_sprite_part_ext ignores the sprite origin, has no rotation,
             * and both original angle branches have xscale -2. */
            if (!overlap(&v, x, y, -2.0 * (sp.w + 1), -2,
                         2, 2.0 * ((float)h + 1))) { ++minecart_rail_native_culled; continue; }
            RV part[12] = {asset(1663), real(frame), real(0), real(0), real(sp.w),
                real(h), arrays[0][i], arrays[1][i], real(-2), real(2),
                real(0xffffff), real(1)}, out = real(0);
            ++minecart_rail_native_submitted; builtin(PART, &out, self, other, 12, part);
        } else {
            emit(self, &v, &sp, 1663, (float)(int)frame, x, y, low ? -2 : 2,
                 2, 0, 0xffffff, 1);
        }
    }
    *trace_start = trace.previous; return result;
}


typedef void ABI (*event_fn)(void *, void *);
typedef void *ABI (*find_fn)(int);
typedef bool ABI (*bool_fn)(const RV *);
typedef RV *ABI (*increment_fn)(RV *, int);
static event_fn original_master;
static script_fn original_fastline;
static find_fn find_instance;
static bool_fn to_bool;
static increment_fn increment;
unsigned minecart_native_hits, minecart_native_fallbacks, minecart_native_segments;
unsigned minecart_native_accepted, minecart_native_visibility_calls;
unsigned minecart_native_fastline_hits, minecart_native_fastline_fallbacks;
/* All diagnostics, including helper rejections, use this file's line numbers. */

static void *node_instance(const RV *ref) {
    if (!ref || ref->kind != 15 || word(ref, 4) != 0x04000001 ||
        word(ref, 0) < 100000 || word(ref, 0) > INT32_MAX) return NULL;
    return find_instance((int)word(ref, 0));
}
static int node_ok(const RV *ref, void *self) {
    void *p = node_instance(ref);
    REQUIRE(p && p != self && word(p, 0x50) == 1);
    REQUIRE(isfinite(scalar(p, 0xb8)) && fabs(scalar(p, 0xb8)) < 1048576 &&
            isfinite(scalar(p, 0xbc)) && fabs(scalar(p, 0xbc)) < 1048576);
    return 1;
}
static void coordinates(const RV *nodes, int i, RV *c) {
    void *a = node_instance(nodes + i), *b = node_instance(nodes + i + 1);
    c[0] = real(scalar(a, 0xb8)); c[1] = real(scalar(a, 0xbc));
    c[2] = real(scalar(b, 0xb8)); c[3] = real(scalar(b, 0xbc));
}
/* The ARM assigns low, high, kind separately and keeps flags. Do not merge
 * these stores: the verifier checks each persistent write transaction. */
static void reset_real(RV *r) {
    volatile uint32_t *p = (volatile uint32_t *)r;
    p[0] = 0; p[1] = 0; p[3] = 0;
}
static void angle_write(RV *r, double a) {
    ((volatile uint32_t *)r)[3] = 0;
    *(volatile double *)&r->value = a;
}
static int master_admit(void *self, RV **state, const RV **nodes, int *count) {
    Table t; View v; Sprite rail, white, bottom;
    RV result = real(0), dummy[4] = {real(0), real(0), real(0), real(0)};
    RV *args[4] = {dummy, dummy + 1, dummy + 2, dummy + 3};
    int supports; const RV *arrays[4];
    REQUIRE(self && !*profile && table(self, &t));
    state[0] = (RV *)peek(&t, SET);
    REQUIRE(state[0] && (state[0]->kind == 0 || state[0]->kind == 13) &&
            isfinite(state[0]->value));
    if (!to_bool(state[0])) return 1;
    REQUIRE(rail_admit(self, &result, 4, args, &v, &rail, &white) &&
            bottom_admit(self, &result, &v, &bottom, &supports, arrays));
    for (int k = 1; k <= 3; ++k) {
        state[k] = (RV *)peek(&t, INDEX + k - 1);
        REQUIRE(numeric(state[k]));
        for (int j = 0; j < k; ++j) REQUIRE(state[k] != state[j]);
    }
    const RV *number = peek(&t, NUMBER), *a = peek(&t, NODE);
    REQUIRE(numeric(number) && number->value >= 0 && number->value <= 8192 &&
            number->value == (int)number->value);
    REQUIRE(a && a->kind == 2 && word(a, 0));
    const void *header = (const void *)(uintptr_t)word(a, 0);
    *count = (int)number->value - 1;
    int needed = *count < 1 ? 2 : *count + 1;
    REQUIRE(word(header, 28) >= (unsigned)needed);
    *nodes = (const RV *)(uintptr_t)word(header, 4);
    REQUIRE(*nodes);
    for (int k = 0; k < needed; ++k) REQUIRE(node_ok(*nodes + k, self));
    /* Ordinary local hash values own distinct cells. Reject fabricated aliases
     * with state, arrays, or camera cells before the first persistent write. */
    for (int k = 1; k <= 3; ++k) {
        REQUIRE(state[k] != number && state[k] != a);
        for (int j = 0; j < NVARS; ++j)
            if (j != INDEX + k - 1) REQUIRE(peek(&t, j) != state[k]);
        for (int j = 0; j < needed; ++j) REQUIRE(*nodes + j != state[k]);
        for (int j = 0; j < 4; ++j)
            for (int n = 0; n < supports; ++n) REQUIRE(arrays[j] + n != state[k]);
    }
    return 1;
}
static int accepted_point(void *self, void *other, double x, double y) {
    RV args[3] = {real(x), real(y), real(150)}, out = real(0);
    RV *p[3] = {args, args + 1, args + 2};
    ++minecart_native_visibility_calls;
    inside(self, other, &out, 3, p);
    return to_bool(&out);
}
void ABI minecart_native_draw(void *self, void *other) {
    RV *state[4]; const RV *nodes = NULL; int count = 0;
    if (!master_admit(self, state, &nodes, &count)) {
        ++minecart_native_fallbacks; original_master(self, other); return;
    }
    ++minecart_native_hits;
    Trace trace = {*trace_start, "gml_Object_obj_minecart_master_Draw_0", 1};
    *trace_start = &trace;
    if (!to_bool(state[0])) { *trace_start = trace.previous; return; }
    RV coords[4], result = real(0);
    RV *args[4] = {coords, coords + 1, coords + 2, coords + 3};
    trace.line = 4; reset_real(state[1]);
    trace.line = 5; reset_real(state[2]);
    trace.line = 6; coordinates(nodes, 0, coords);
    angle_write(state[3], direction((float)coords[0].value, (float)coords[1].value,
                                   (float)coords[2].value, (float)coords[3].value));
    trace.line = 9; minecart_rail_native_bottom(self, other, &result, 0, NULL);
    trace.line = 14; reset_real(state[1]);
    trace.line = 15; coordinates(nodes, 0, coords);
    angle_write(state[3], direction((float)coords[0].value, (float)coords[1].value,
                                   (float)coords[2].value, (float)coords[3].value));
    for (int i = 0; i < count; ++i) {
        ++minecart_native_segments; trace.line = 18;
        coordinates(nodes, i, coords);
        if (accepted_point(self, other, coords[0].value, coords[1].value) ||
            accepted_point(self, other, coords[2].value, coords[3].value) ||
            accepted_point(self, other, (coords[0].value + coords[2].value) / 2,
                                       (coords[1].value + coords[3].value) / 2)) {
            ++minecart_native_accepted; trace.line = 20;
            minecart_rail_native_draw(self, other, &result, 4, args);
        }
        trace.line = 24; increment(state[1], 1);
    }
    *trace_start = trace.previous;
}
static int fastline_admit(void *self, RV *result, int argc, RV **args) {
    /* This public one-sprite helper preserves the complete draw stream. Camera
     * union admission costs more than the tagged arithmetic it replaces. Rail
     * crossbars still use the included rail helper's conservative culling. */
    REQUIRE(self && unmanaged_result(result) && argc >= 6 && args && !*profile &&
            !*active_shader && registry());
    for (int i = 0; i < 5; ++i) REQUIRE(numeric(args[i]) && args[i] != result);
    REQUIRE(args[5] && args[5] != result && args[5]->kind == 0 &&
            isfinite(args[5]->value) && args[5]->value >= INT32_MIN &&
            args[5]->value <= INT32_MAX);
    /* Nonnegative bounded widths keep the inherited sprite bound domain. */
    REQUIRE(args[4]->value >= 0 && args[4]->value <= 65536);
    return 1;
}
RV *ABI minecart_native_fastline(void *self, void *other, RV *result, int argc, RV **args) {
    if (!fastline_admit(self, result, argc, args)) {
        ++minecart_native_fastline_fallbacks;
        return original_fastline(self, other, result, argc, args);
    }
    ++minecart_native_fastline_hits; undefined(result);
    Trace trace = {*trace_start, "gml_Script_draw_fastline_width_col", 16};
    *trace_start = &trace;
    RV c[4]; for (int i = 0; i < 4; ++i) c[i] = *args[i];
    RV distance = real(0), alpha = real(0);
    builtin(DISTANCE, &distance, self, other, 4, c);
    trace.line = 17;
    double angle = direction((float)c[0].value, (float)c[1].value,
                             (float)c[2].value, (float)c[3].value);
    trace.line = 18;
    double half = args[4]->value / 2;
    float x = (float)(c[0].value + (double)length_x((float)half, (float)(angle + 90)));
    float y = (float)(c[1].value + (double)length_y((float)half, (float)(angle + 90)));
    builtin(ALPHA, &alpha, self, other, 0, NULL);
    ++minecart_rail_native_submitted;
    draw_sprite(self, 2184, 0, x, y, (float)distance.value,
                (float)args[4]->value, (float)angle, (int)args[5]->value, (float)alpha.value);
    *trace_start = trace.previous; return result;
}

int minecart_native_install(void *(*rx_alloc)(size_t)) {
    if (installed || !rx_alloc) return 0;
#define RESOLVE(var, type, name) do { \
    var = (type)so_symbol(&yoyoloader_mod, name); if (!var) return 0; \
} while (0)
    uintptr_t rail, bottom, master, fastline;
    RESOLVE(rail, uintptr_t, "_Z20gml_Script_draw_railP9CInstanceS0_R8YYRValueiPPS1_");
    RESOLVE(bottom, uintptr_t, "_Z27gml_Script_draw_rail_bottomP9CInstanceS0_R8YYRValueiPPS1_");
    if ((rail & 3) || (bottom & 3) || rail < 0x01934824 ||
        rail - 0x01934824 != bottom - 0x01931af8 ||
        word((void *)rail, 0) != 0xe92d4ff0 || word((void *)rail, 4) != 0xe28db01c ||
        word((void *)bottom, 0) != 0xe92d4ff0 || word((void *)bottom, 4) != 0xe28db01c)
        return 0;
    RESOLVE(master, uintptr_t, "_Z37gml_Object_obj_minecart_master_Draw_0P9CInstanceS0_");
    RESOLVE(fastline, uintptr_t, "_Z34gml_Script_draw_fastline_width_colP9CInstanceS0_R8YYRValueiPPS1_");
    if ((master & 3) || (fastline & 3) || master - 0x04389280 != rail - 0x01934824 ||
        fastline - 0x018fa988 != rail - 0x01934824 ||
        word((void *)master, 0) != 0xe92d4ff0 || word((void *)master, 4) != 0xe28db01c ||
        word((void *)fastline, 0) != 0xe92d4ff0 || word((void *)fastline, 4) != 0xe28db01c)
        return 0;
    RESOLVE(find_instance, find_fn, "_Z18YYGML_FindInstancei");
    RESOLVE(to_bool, bool_fn, "_Z11BOOL_RValuePK6RValue");
    RESOLVE(increment, increment_fn, "_ZN8YYRValuepLEi");
    RESOLVE(global, void **, "g_pGlobal");
    RESOLVE(epsilon, const double *, "g_GMLMathEpsilon");
    RESOLVE(profile, const uint8_t *, "g_bProfile");
    RESOLVE(managed_enabled, const uint8_t *, "g_enableManagedObjects");
    RESOLVE(managed_guard, const int *, "g_nManagedObjectGuard");
    RESOLVE(global_start, const int *, "g_nStartGlobalVariables");
    RESOLVE(active_shader, const void *const *, "g_ActiveUserShader");
    RESOLVE(matrices, const float *, "m_CurrentMatrix");
    RESOLVE(builtin_variables, const void *, "builtin_variables");
    RESOLVE(xy_get[0], uintptr_t, "_Z4GV_XP9CInstanceiP6RValue");
    RESOLVE(xy_get[1], uintptr_t, "_Z4GV_YP9CInstanceiP6RValue");
    RESOLVE(xy_set[0], uintptr_t, "_Z4SV_XP9CInstanceiP6RValue");
    RESOLVE(xy_set[1], uintptr_t, "_Z4SV_YP9CInstanceiP6RValue");
    RESOLVE(standard_get, ref_fn, "_ZN12YYObjectBase19InternalGetYYVarRefEi");
    RESOLVE(standard_ref, ref_fn, "_ZN12YYObjectBase20InternalGetYYVarRefLEi");
    RESOLVE(compare, compare_fn, "_Z12YYCompareValRK6RValueS1_db");
    RESOLVE(setter, setter_fn, "_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
    RESOLVE(direction, direction_fn, "_Z21YYGML_point_directionffff");
    RESOLVE(length_x, length_fn, "_Z17YYGML_lengthdir_xff");
    RESOLVE(length_y, length_fn, "_Z17YYGML_lengthdir_yff");
    RESOLVE(clamp, clamp_fn, "_Z11YYGML_clampddd");
    RESOLVE(sprite_data, sprite_fn, "_Z11Sprite_Datai");
    RESOLVE(draw_sprite, draw_fn, "_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
    RESOLVE(inside, script_fn, "_Z30gml_Script_inside_screen_pointP9CInstanceS0_R8YYRValueiPPS1_");
    RESOLVE(functions, Function **, "the_functions");
    RESOLVE(current_function, Function **, "g_pFunction");
    RESOLVE(trace_start, Trace **, "_ZN13SYYStackTrace8s_pStartE");
    char name[64];
    for (int i = 0; i < NVARS; ++i) {
        strcpy(name, "g_VAR_"); strcat(name, var_names[i]);
        RESOLVE(vars[i], const uint32_t *, name);
    }
    for (int i = 0; i < NFUNCS; ++i) {
        strcpy(name, "g_FUNC_"); strcat(name, fn_names[i]);
        RESOLVE(descriptors[i], const uint32_t *, name);
        RESOLVE(builtins[i], builtin_fn, fn_symbols[i]);
    }
#undef RESOLVE
    void *rx = rx_alloc(64);
    uintptr_t bias = rail - 0x01934824;
    if (!rx || ((uintptr_t)rx & 3) || (uintptr_t)rx > UINTPTR_MAX - 64 ||
        ((uintptr_t)rx < bias + 0x04e70000 && (uintptr_t)rx + 64 > bias)) return 0;
    uint32_t code[16] = {0xe92d4ff0, 0xe28db01c, 0xe51ff004, (uint32_t)(rail + 8),
                       0xe92d4ff0, 0xe28db01c, 0xe51ff004, (uint32_t)(bottom + 8),
                       0xe92d4ff0, 0xe28db01c, 0xe51ff004, (uint32_t)(master + 8),
                       0xe92d4ff0, 0xe28db01c, 0xe51ff004, (uint32_t)(fastline + 8)};
    kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof(code));
    kuKernelFlushCaches(rx, sizeof(code));
    original_rail = (script_fn)rx; original_bottom = (script_fn)((uint8_t *)rx + 16);
    original_master = (event_fn)((uint8_t *)rx + 32);
    original_fastline = (script_fn)((uint8_t *)rx + 48);
    hook_addr(master, (uintptr_t)minecart_native_draw);
    hook_addr(fastline, (uintptr_t)minecart_native_fastline);
    hook_addr(rail, (uintptr_t)minecart_rail_native_draw);
    hook_addr(bottom, (uintptr_t)minecart_rail_native_bottom);
    kuKernelFlushCaches((void *)rail, 8); kuKernelFlushCaches((void *)bottom, 8);
    kuKernelFlushCaches((void *)master, 8); kuKernelFlushCaches((void *)fastline, 8);
    installed = 1; return 1;
}
