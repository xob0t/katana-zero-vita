/* libyoyo.so ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * ARM softfp, no fast-math. Install after relocation/import resolution, before
 * execution. Function registry initialization is NOT required for installation.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifdef FLASH_MASK_STUB
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif
#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef RV *ABI (*script_fn)(void *, void *, RV *, int, RV **);
typedef RV *ABI (*get_fn)(void *, int);
typedef void ABI (*builtin_fn)(RV *, void *, void *, int, RV *);
typedef double ABI (*real_fn)(const RV *);
typedef int ABI (*bool_fn)(const RV *);
typedef void ABI (*event_fn)(void *, void *, int);
typedef int ABI (*new_fn)(void *, void **, void **, const RV *);
typedef int ABI (*next_fn)(void *, void **, void **);
typedef void ABI (*delete_fn)(void *, void **, void **);
typedef void ABI (*sprite_draw_fn)(void *, int, float, float, float,
                                   float, float, float, int, float);
typedef void ABI (*target_fn)(int, int);
typedef void ABI (*reset_fn)(void);
typedef const char *ABI (*name_fn)(int);
typedef void *ABI (*sprite_fn)(int);
typedef void ABI (*value_get_fn)(const RV *, int, int, RV *, bool, bool);
typedef void ABI (*free_fn)(RV *);
typedef int ABI (*object_ref_fn)(const RV *, int, int, int, void **, bool, bool);
enum { XVIEW, YVIEW, SURFACE, X, Y, XS, YS, OBJECT, NSLOTS };
static const char *const names[NSLOTS] = {
    "g_VAR_xview", "g_VAR_yview", "g_VAR_light_surface", "g_VAR_x",
    "g_VAR_y", "g_VAR_image_xscale", "g_VAR_image_yscale", "g_VAR_object_index"
};
static const uint32_t *slots[NSLOTS];
static void **global;
static const uint8_t *profile;
static get_fn standard_get, standard_ref;
static script_fn original, create;
static builtin_fn exists, clear, blend, composite, width, height;
static real_fn real;
static bool_fn boolean;
static event_fn event_user;
static new_fn new_with;
static next_fn next_with;
static delete_fn delete_with;
static sprite_draw_fn draw;
static target_fn set_target;
static reset_fn reset_target;
static name_fn object_name;
static sprite_fn sprite_data;
static value_get_fn variable_get;
static free_fn release;
static object_ref_fn object_ref;
static const int *object_count;
static bool installed;
unsigned flash_mask_hits, flash_mask_fallbacks, flash_mask_instances;
unsigned flash_mask_culled, flash_mask_draws, flash_mask_name_hits;
unsigned flash_mask_name_misses, flash_mask_slow_instances, flash_mask_unculled;
unsigned flash_mask_fail_line[32], flash_mask_fail_count[32];
static int reject(unsigned line) {
    for (unsigned i = 0; i < 32; ++i)
        if (!flash_mask_fail_line[i] || flash_mask_fail_line[i] == line) {
            flash_mask_fail_line[i] = line; ++flash_mask_fail_count[i]; break;
        }
    return 0;
}
static uint32_t word(const void *p, size_t off) {
    uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v;
}
static float scalar(const void *p, size_t off) {
    float v; memcpy(&v, (const uint8_t *)p + off, 4); return v;
}
static int slot(int n) { return (int)slots[n][1]; }
/* Admission is read-only; a rejected frame reaches the trampoline before any
 * runner call or result write. Builtins are NOT local hash-table entries. */
static int standard_object(void *p) {
    if (!p) return 0;
    const void *vt = (const void *)(uintptr_t)word(p, 0);
    return vt && word(vt, 8) == (uintptr_t)standard_get &&
           word(vt, 12) == (uintptr_t)standard_ref;
}
static int ordinary(void *p) {
    return standard_object(p) && !word(p, 0x70) && !word(p, 0x74);
}
static RV *peek(void *p, int n) {
    /* g_pGlobal may be a YYObjectBase, not a full CInstance. Do not read its
     * nonexistent CInstance fields when checking its scalar hash entries. */
    if (!standard_object(p)) return NULL;
    const void *ht = (const void *)(uintptr_t)word(p, 0x24);
    if (!ht) return NULL;
    uint32_t mask = word(ht, 8), key = ((uint32_t)slot(n) + 1) & 0x7fffffffu;
    const uint8_t *entries = (const uint8_t *)(uintptr_t)word(ht, 16);
    if (!key || !entries || mask > 65535 || (mask & (mask + 1))) return NULL;
    for (uint32_t i = key & mask, d = 0; d <= mask; ++d, i = (i + 1) & mask) {
        uint32_t k = word(entries + 12 * i, 8);
        if (k == key) return (RV *)(uintptr_t)word(entries + 12 * i, 0);
        if (!k || d > ((i - (k & mask)) & mask)) break;
    }
    return NULL;
}
static int numeric(const RV *v) { return v && v->kind == 0 && isfinite(v->value); }
/* Surface builtins validate this exact YYRef type. REAL_RValue_Ex converts
 * kind 15 to the signed low word, ignoring the type word, for target setup. */
static int surface_value(const RV *v) {
    return v && ((v->kind == 15 && word(v, 4) == 0x08000004u) ||
                 (numeric(v) && v->value >= -2147483648.0 && v->value < 2147483648.0));
}
static RV number(double v) { RV r = {v, 0, 0}; return r; }
static RV undefined(void) { RV r = {0, 0, 5}; return r; }
static double value(const RV *v) { return v->kind == 0 ? v->value : real(v); }
static void discard(RV *v) {
    if ((0x46u >> (v->kind & 31)) & 1) release(v);
}
static RV *get(void *p, int n) {
    const void *vt = (const void *)(uintptr_t)word(p, 0);
    return ((get_fn)(uintptr_t)word(vt, 8))(p, slot(n));
}
enum { DEFAULT, SLOPE, SLOPE2, CEILING, DOOR };
/* Direct-mapped cache: collisions only cause misses. Names in this build are
 * immutable asset strings. Definition/name identity also invalidates a reused
 * index or replaced definition. Descendants never inherit an ancestor's case. */
typedef struct { int index, which; const void *definition, *name; } Case;
static Case cases[256];
static int select_case(void *p, int index) {
    const void *definition = (const void *)(uintptr_t)word(p, 0x68);
    const void *name = definition ? (const void *)(uintptr_t)word(definition, 0) : NULL;
    Case *c = &cases[(uint32_t)index & 255];
    if (definition && c->definition == definition && c->name == name && c->index == index) {
        ++flash_mask_name_hits; return c->which;
    }
    ++flash_mask_name_misses;
    const char *s = object_name(index);
    int which = s && !strcmp(s, "obj_slope") ? SLOPE :
                s && !strcmp(s, "obj_slope2") ? SLOPE2 :
                s && !strcmp(s, "obj_ceiling_tile") ? CEILING :
                s && !strcmp(s, "obj_door") ? DOOR : DEFAULT;
    c->index = index; c->which = which; c->definition = definition; c->name = name;
    return which;
}
typedef struct { float x, y, xs, ys; } Mask;
/* Test the actual float arguments submitted by YYC. The whitepixel is 1x1,
 * origin 0,0. Two target pixels cover filtering and bounded f32 edge rounding.
 * Nonfinite or huge geometry is always submitted. Negative scales reverse the
 * edges; zero scales stay conservative. Both slope records are tested. */
static int touches(const Mask *m) {
    double x = m->x, y = m->y, sx = m->xs, sy = m->ys;
    if (!isfinite(x) || !isfinite(y) || !isfinite(sx) || !isfinite(sy) ||
        fabs(x) > 1048576 || fabs(y) > 1048576 ||
        fabs(sx) > 1048576 || fabs(sy) > 1048576) return 1;
    double r = x + sx, b = y + sy;
    double l = x < r ? x : r, t = y < b ? y : b;
    if (x > r) r = x;
    if (y > b) b = y;
    return !(r < -2 || b < -2 || l > 646 || t > 366);
}
/* Unusual instance layouts use the runner's complete builtin lookup, with a
 * POINTER receiver. Never interpret kind 15's stale high word as a double. */
static double slow_value(void *p, int n) {
    RV receiver = undefined(), v = undefined();
    uint32_t lo = (uint32_t)(uintptr_t)p;
    memcpy(&receiver.value, &lo, 4); receiver.kind = 6;
    variable_get(&receiver, slot(n), (int)0x80000000u, &v, false, false);
    double result = value(&v); discard(&v); return result;
}
static int slow_index(void *p) {
    RV receiver = undefined(), v = undefined();
    uint32_t lo = (uint32_t)(uintptr_t)p;
    memcpy(&receiver.value, &lo, 4); receiver.kind = 6;
    variable_get(&receiver, slot(OBJECT), (int)0x80000000u, &v, false, false);
    /* Same YYGetRef overload/arguments as F_ObjectGetName: object asset type,
     * no array, out-of-range allowed, wrong types still report runner errors. */
    int result = object_ref(&v, 0, 0x01000000, *object_count, NULL, true, false);
    discard(&v); return result;
}
RV *ABI flashlight_draw_fast(void *self, void *other, RV *result, int argc, RV **args) {
    RV *vx = peek(*global, XVIEW), *vy = peek(*global, YVIEW), *surface = peek(self, SURFACE);
    if (*profile || !result || argc != 0 || !ordinary(self) || !numeric(vx) || !numeric(vy) ||
        !surface_value(surface)) {
        reject(__LINE__); ++flash_mask_fallbacks;
        return original(self, other, result, argc, args);
    }
    ++flash_mask_hits;
    /* Original 19bd2e4/2e8: only low payload word and kind are written. */
    uint32_t zero = 0; memcpy(result, &zero, 4); result->kind = 5;
    RV out = undefined(), argument = *surface;
    exists(&out, self, other, 1, &argument);
    int present = boolean(&out); discard(&out);
    if (!present) {
        out = undefined(); create(self, other, &out, 0, NULL); discard(&out);
        return result; /* Original 19bd460 jumps to cleanup, not target set! */
    }
    RV target_surface = *get(self, SURFACE);
    int target = (int)value(&target_surface);
    set_target(target, -1);
    RV clear_args[2] = {number(0), number(0)};
    out = undefined(); clear(&out, self, other, 2, clear_args); discard(&out);
    event_user(self, other, 2); /* Full original event dispatch, including overrides. */
    argument = number(3); out = undefined(); blend(&out, self, other, 1, &argument); discard(&out);
    /* Geometry guards do not replay user 2 on failure. They just disable culling. */
    /* Query the bound surface, retaining its type even if user 2 replaces the
     * variable. The composite separately reads the current variable below. */
    RV dimensions = target_surface;
    out = undefined(); width(&out, self, other, 1, &dimensions);
    int cull = value(&out) == 644; discard(&out);
    out = undefined(); height(&out, self, other, 1, &dimensions);
    cull = cull && value(&out) == 364; discard(&out);
    const void *sp = sprite_data(2184);
    cull = cull && sp && word(sp, 0x84) == 0 && word(sp, 0x5c) == 1 &&
           word(sp, 0x60) == 1 && word(sp, 0x64) == 0 && word(sp, 0x68) == 0;
    if (!cull) { reject(__LINE__); ++flash_mask_unculled; }
    void *s = self, *o = other;
    uint32_t iterator[4] = {0};
    RV blocks = undefined();
    uint32_t ref[2] = {57, 0x01000000};
    memcpy(&blocks.value, ref, 8); blocks.kind = 15;
    int remaining = new_with(iterator, &s, &o, &blocks);
    while (remaining > 0) {
        ++flash_mask_instances;
        int direct = ordinary(s) && word(s, 0x50) == 1;
        if (!direct) { reject(__LINE__); ++flash_mask_slow_instances; }
        int index = direct ? (int)word(s, 0x84) : slow_index(s);
        int which = select_case(s, index);
        /* ARM case 3 at 19bdde0 exits at L30. Exact obj_door, like ceiling,
         * is empty despite the earlier GML lift describing fallthrough. */
        if (which != CEILING && which != DOOR) {
            double x = direct ? (double)scalar(s, 0xb8) : slow_value(s, X);
            double y = direct ? (double)scalar(s, 0xbc) : slow_value(s, Y);
            /* Keep the references captured at entry, not copies of the camera. */
            double cx = value(vx), cy = value(vy);
            Mask m[2]; int count = 1;
            if (which == SLOPE || which == SLOPE2) {
                m[0] = (Mask){(float)((x + (which == SLOPE ? 16 : 0) - cx) / 2),
                              (float)((y - cy) / 2), 8, 16};
                m[1] = (Mask){(float)((x + (which == SLOPE2 ? 16 : 0) - cx) / 2),
                              (float)((y + 16 - cy) / 2), 8, 8};
                count = 2;
            } else {
                double sx = direct ? (double)scalar(s, 0xa4) : slow_value(s, XS);
                double sy = direct ? (double)scalar(s, 0xa8) : slow_value(s, YS);
                m[0] = (Mask){(float)((x - cx) / 2), (float)((y - cy) / 2),
                              (float)((sx * 32) / 2), (float)((sy * 32) / 2)};
            }
            for (int i = 0; i < count; ++i) {
                if (cull && !touches(&m[i])) { ++flash_mask_culled; continue; }
                ++flash_mask_draws;
                draw(s, 2184, 0, m[i].x, m[i].y, m[i].xs, m[i].ys, 0, 0, 1);
            }
        }
        remaining = next_with(iterator, &s, &o);
    }
    delete_with(iterator, &s, &o);
    reset_target();
    argument = number(1); out = undefined(); blend(&out, self, other, 1, &argument); discard(&out);
    RV comp[8] = {*get(self, SURFACE), *vx, *vy, number(2), number(2),
                  number(0), number(16777215), number(1)};
    out = undefined(); composite(&out, self, other, 8, comp); discard(&out);
    argument = number(0); out = undefined(); blend(&out, self, other, 1, &argument); discard(&out);
    return result;
}
int flash_mask_install(void *(*rx_alloc)(size_t)) {
    if (installed || !rx_alloc) return 0;
    uintptr_t entry = so_symbol(&yoyoloader_mod,
        "_Z26gml_Script_flashlight_drawP9CInstanceS0_R8YYRValueiPPS1_");
    if (!entry || (entry & 3) || word((void *)entry, 0) != 0xe92d4ff0u ||
        word((void *)entry, 4) != 0xe28db01cu) return 0;
    for (int n = 0; n < NSLOTS; ++n) {
        slots[n] = (const uint32_t *)so_symbol(&yoyoloader_mod, names[n]);
        if (!slots[n]) return 0;
    }
#define RESOLVE(v, type, name) do { v = (type)so_symbol(&yoyoloader_mod, name); if (!v) return 0; } while (0)
    RESOLVE(global, void **, "g_pGlobal");
    RESOLVE(profile, const uint8_t *, "g_bProfile");
    RESOLVE(standard_get, get_fn, "_ZN12YYObjectBase19InternalGetYYVarRefEi");
    RESOLVE(standard_ref, get_fn, "_ZN12YYObjectBase20InternalGetYYVarRefLEi");
    RESOLVE(create, script_fn, "_Z28gml_Script_flashlight_createP9CInstanceS0_R8YYRValueiPPS1_");
    RESOLVE(exists, builtin_fn, "_Z15F_SurfaceExistsR6RValueP9CInstanceS2_iPS_");
    RESOLVE(clear, builtin_fn, "_Z16F_DrawClearAlphaR6RValueP9CInstanceS2_iPS_");
    RESOLVE(blend, builtin_fn, "_Z17F_GPUSetBlendModeR6RValueP9CInstanceS2_iPS_");
    RESOLVE(composite, builtin_fn, "_Z16F_DrawSurfaceExtR6RValueP9CInstanceS2_iPS_");
    RESOLVE(width, builtin_fn, "_Z17F_SurfaceGetWidthR6RValueP9CInstanceS2_iPS_");
    RESOLVE(height, builtin_fn, "_Z18F_SurfaceGetHeightR6RValueP9CInstanceS2_iPS_");
    RESOLVE(real, real_fn, "_Z14REAL_RValue_ExPK6RValue");
    RESOLVE(boolean, bool_fn, "_Z11BOOL_RValuePK6RValue");
    RESOLVE(event_user, event_fn, "_Z16YYGML_event_userP9CInstanceS0_i");
    RESOLVE(new_with, new_fn, "_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
    RESOLVE(next_with, next_fn, "_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_");
    RESOLVE(delete_with, delete_fn, "_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_");
    RESOLVE(draw, sprite_draw_fn, "_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
    RESOLVE(set_target, target_fn, "_Z24YYGML_surface_set_targetii");
    RESOLVE(reset_target, reset_fn, "_Z26YYGML_surface_reset_targetv");
    RESOLVE(object_name, name_fn, "_Z11Object_Namei");
    RESOLVE(sprite_data, sprite_fn, "_Z11Sprite_Datai");
    RESOLVE(variable_get, value_get_fn, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
    RESOLVE(release, free_fn, "_Z16FREE_RValue__PreP6RValue");
    RESOLVE(object_ref, object_ref_fn, "_Z8YYGetRefPK6RValueiiiPPvbb");
    RESOLVE(object_count, const int *, "g_ObjectNumber");
#undef RESOLVE
    void *rx = rx_alloc(16);
    if (!rx || ((uintptr_t)rx & 3) || ((uintptr_t)rx < entry + 8 && (uintptr_t)rx + 16 > entry)) return 0;
    uint32_t code[4] = {0xe92d4ff0u, 0xe28db01cu, 0xe51ff004u, (uint32_t)(entry + 8)};
    kuKernelCpuUnrestrictedMemcpy(rx, code, sizeof(code));
    kuKernelFlushCaches(rx, sizeof(code)); original = (script_fn)rx;
    hook_addr(entry, (uintptr_t)flashlight_draw_fast);
    kuKernelFlushCaches((void *)entry, 8); installed = true;
    return 1;
}
