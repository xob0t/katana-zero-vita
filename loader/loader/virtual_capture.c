/* APK 1.0.10 ARM runner only. Install after Run_Start initializes the registry.
 * See virtual_capture.md. Keep the original GML script and event dispatch. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#ifdef VIRTUAL_CAPTURE_TEST
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#endif
#pragma GCC optimize ("unwind-tables")
#define AAPCS __attribute__((pcs("aapcs")))
#define MAX_QUADS 64
typedef struct { uint64_t bits; uint32_t flags, kind; } RV;
typedef void AAPCS (*script_fn)(void *, void *, RV *, int, RV **);
typedef int AAPCS (*target_fn)(int, int, int);
typedef int AAPCS (*reset_fn)(void);
typedef void AAPCS (*clear_fn)(uint32_t, float);
typedef void *AAPCS (*alloc_fn)(int, void *, int, int);
typedef void AAPCS (*surface_fn)(int, float, float, float, float, float, uint32_t, float);
typedef void *AAPCS (*get_surface_fn)(int);
typedef uint32_t *AAPCS (*colours_fn)(uint32_t, float);
typedef void *AAPCS (*texture_get_fn)(int, bool, bool, bool, bool);
typedef RV *AAPCS (*get_fn)(void *, int);
typedef void AAPCS (*matrix_fn)(int, float *);
typedef void *AAPCS (*frustum_fn)(void);
typedef void AAPCS (*lookat_fn)(float *, const float *, const float *, const float *);
typedef void AAPCS (*ortho_fn)(float *, float, float, float, float);
typedef void AAPCS (*multiply_fn)(float *, const float *, const float *);
typedef void AAPCS (*frustum_build_fn)(float *, const float *);
typedef void *AAPCS (*camera_fn)(void *, int);
typedef struct { const char *name; void *call; int argc; } Function;
typedef struct { float x, y, z; uint32_t colour; float u, v; } Vertex;
typedef struct { void *texture; Vertex vertices[6]; } Quad;
typedef struct {
    const char *name; uint32_t offset, prologue[2];
    unsigned relocation; uint32_t literal, pc; unsigned mode, mutation;
} HookSpec;
#include "virtual_capture_hooks.inc"
#include "virtual_capture_atlas.inc"
#include "virtual_capture_matrix.inc"
#define NHOOKS (sizeof specs / sizeof specs[0])
enum { VC_OPERATION, VC_CLIP, VC_CAPACITY, VC_FORMAT, VC_STATE,
       VC_ALPHA, VC_TRANSFORM, VC_NEW_CAPTURE, VC_PROTOCOL, VC_POLICY,
       VC_TEXEL, VC_REASON_COUNT };
unsigned virtual_capture_virtual_captures, virtual_capture_direct_replays;
/* Loader opt-in. Snapshot at target admission so a mode change cannot make
 * already relaxed geometry eligible for an exact consumer. */
int virtual_capture_relaxed;
unsigned virtual_capture_relaxed_replays, virtual_capture_clipped_quads;
unsigned virtual_capture_materializations, virtual_capture_quads_recorded;
unsigned virtual_capture_capture_fallbacks, virtual_capture_install_failures;
unsigned virtual_capture_materializations_by_reason[VC_REASON_COUNT];
unsigned virtual_capture_materializations_by_hook[NHOOKS];
struct { unsigned line, count; } virtual_capture_rejections[128];
unsigned virtual_capture_rejections_used;
/* Saved runner LR, including its instruction-state bit, minus ELF load bias.
 * Key includes hook so two operations at the same indirect call stay distinct. */
struct { unsigned offset, hook, count; } virtual_capture_callers[64];
unsigned virtual_capture_callers_used, virtual_capture_callers_dropped;
static uintptr_t entries[NHOOKS], trampolines[NHOOKS], bias;
static script_fn original_capture;
static target_fn original_target;
static reset_fn original_reset;
static clear_fn original_clear;
static alloc_fn original_alloc;
static surface_fn original_surface;
static get_surface_fn surface_get;
static get_surface_fn surface_texture_get;
static colours_fn get_colours;
static texture_get_fn original_texture_get;
static matrix_fn original_matrix;
static frustum_fn original_frustum;
static lookat_fn matrix_lookat;
static ortho_fn matrix_ortho;
static multiply_fn matrix_multiply;
static frustum_build_fn frustum_build;
static camera_fn camera_get;
static void *camera_manager;
static const int *default_camera_id;
static const int *render_target_active;
static float virtual_matrices[5][16] __attribute__((aligned(16)));
static float virtual_frustum[24] __attribute__((aligned(16)));
static int virtual_camera_ok, virtual_matrices_ready, virtual_frustum_ready;
static int virtual_projection_flip;
static void **global_object, **active_shader, **next_shader;
static int *native_shader, *next_native_shader, *texture_stage;
static int *surface_stack, *current_targets;
static uint8_t *graphics_initialized;
static uint8_t *mark_vertices;
static const int *render_stack;
static const uint8_t *render_states;
static const float *matrices, *draw_depth;
static const uint32_t *var_sprite, *var_rim, *var_slowdown;
static Function **functions;
static const uint32_t *clear_descriptor, *surface_descriptor;
static Quad quads[MAX_QUADS];
static unsigned char replay_clip[MAX_QUADS];
static int capture_relaxed;
static struct { void *texture; unsigned dirty; } atlas[sizeof atlas_pages / sizeof atlas_pages[0]];
static unsigned used, last_pending;
static unsigned quad_reason;
static int installed, in_script, virtual_target, pending, consumed, internal;
/* ARM gate adds this byte offset to PC without changing APSR or FPSCR.
 * A consumed list is still pending until its pixels exist in the real surface. */
unsigned virtual_capture_armed; /* 0 = idle, 12 = deferred surface exists */
static int surface_id, expected_surface, cleared, candidate;
static int surface_texture_id;
static void *surface_texture;
static uint32_t clear_colour;
static float clear_alpha;
static uint32_t word(const void *p, size_t off) {
    uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v;
}
static void reject(unsigned line) {
    unsigned i;
    for (i = 0; i < virtual_capture_rejections_used; ++i)
        if (virtual_capture_rejections[i].line == line) {
            ++virtual_capture_rejections[i].count; return;
        }
    if (i < 128) {
        virtual_capture_rejections[i].line = line;
        virtual_capture_rejections[i].count = 1;
        ++virtual_capture_rejections_used;
    }
}
static void caller_hit(unsigned hook, uintptr_t lr) {
    unsigned i, offset = (unsigned)(lr - bias);
    for (i = 0; i < virtual_capture_callers_used; ++i)
        if (virtual_capture_callers[i].offset == offset && virtual_capture_callers[i].hook == hook) {
            ++virtual_capture_callers[i].count; return;
        }
    if (i == 64) { ++virtual_capture_callers_dropped; return; }
    virtual_capture_callers[i].offset = offset;
    virtual_capture_callers[i].hook = hook;
    virtual_capture_callers[i].count = 1;
    ++virtual_capture_callers_used;
}
static RV *global_value(const uint32_t *descriptor) {
    get_fn get;
    void *owner = *global_object;
    if (!owner) return NULL;
    memcpy(&get, (const void *)(uintptr_t)(word(owner, 0) + 8), sizeof get);
    return get ? get(owner, (int)descriptor[1]) : NULL;
}
static int numeric(const RV *v, double *out) {
    if (!v) return 0;
    if (v->kind == 0 || v->kind == 13) memcpy(out, &v->bits, 8);
    else if (v->kind == 7) *out = (int32_t)(uint32_t)v->bits;
    else if (v->kind == 10) *out = (int64_t)v->bits;
    else return 0;
    return isfinite(*out);
}
static int surface_value(const RV *v, int *out) {
    double id;
    if (v && v->kind == 15) {
        /* YYC REAL_RValue_Ex/INT32_RValue use the signed low payload word.
         * Surface builtins additionally require this exact YYRef type. */
        if ((uint32_t)(v->bits >> 32) != 0x08000004u) return 0;
        *out = (int32_t)(uint32_t)v->bits;
        return *out >= 0;
    }
    if (!numeric(v, &id) || id < 0 || id > INT32_MAX || id != (int)id) return 0;
    *out = (int)id;
    return 1;
}
static int policy_ok(void) {
    double a, b;
    return numeric(global_value(var_rim), &a) && a == 0 &&
           numeric(global_value(var_slowdown), &b) && b >= 1;
}
/* Inspect DESIRED state. g_SrcBlend and g_TextureFilterMin describe the last
 * flush and can lag an inlined GPU setter. These enums come from executing
 * RenderStateManager::Reset in the original ARM binary. */
static int state_ok(int relaxed) {
    static const uint32_t normal[38] = {
        0,0,3,2,1,0,5,6,0,4,0,5,1,0,0,0,1,0,0x3f800000,
        0,0,0,0,3,15,0,1,1,1,8,0,0xffffffff,0xffffffff,0,5,6,0,0
    };
    unsigned i;
    if (*active_shader || *next_shader || *native_shader != -1 ||
        *next_native_shader != -1 || *texture_stage != 0 || *mark_vertices) return 0;
    for (i = 0; i < 38; ++i)
        if (word(render_states, 0x210 + i * 4) != normal[i]) return 0;
    /* Relaxed replay submits source textures through the sprite allocator
     * with the consumer's desired min/mag filters. Mipmaps stay disabled. */
    if ((!relaxed && (word(render_states, 0x2a8) || word(render_states, 0x2ac))) ||
        word(render_states, 0x2cc) != 2) return 0;
    /* Surface target changes the camera, not the world transform. A nonidentity
     * world matrix would be applied twice by the original two-pass draw. */
    for (i = 0; i < 16; ++i)
        if (matrices[32 + i] != (i % 5 == 0 ? 1.0f : 0.0f)) return 0;
    return 1;
}
static void emit_identity(void) {
    unsigned i;
    for (i = 0; i < used; ++i) {
        void *dest = original_alloc(4, quads[i].texture, 24, 6);
        memcpy(dest, quads[i].vertices, sizeof quads[i].vertices);
    }
}
static int binary_region(const Quad *q) {
    unsigned i, lo = 0, hi = sizeof binary_regions / sizeof binary_regions[0];
    float umin = q->vertices[0].u, umax = umin;
    float vmin = q->vertices[0].v, vmax = vmin;
    float x, y, w, h;
    uint64_t key;
    for (i = 1; i < 6; ++i) {
        float u = q->vertices[i].u, v = q->vertices[i].v;
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    for (i = 0; i < sizeof atlas / sizeof atlas[0]; ++i)
        if (atlas[i].texture == q->texture && !atlas[i].dirty) break;
    if (i == sizeof atlas / sizeof atlas[0]) return 0;
    x = umin * atlas_pages[i].width; y = vmin * atlas_pages[i].height;
    w = (umax - umin) * atlas_pages[i].width; h = (vmax - vmin) * atlas_pages[i].height;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > atlas_pages[i].width ||
        y + h > atlas_pages[i].height || x != (unsigned)x || y != (unsigned)y ||
        w != (unsigned)w || h != (unsigned)h) return 0;
    /* Removing a resampled intermediate image is exact only when capture
     * texels are integral replications of source texels. Integer destination
     * corners alone would still accept, for example, 16 source pixels into
     * 12 capture pixels, which samples differently on subsequent enlargement. */
    {
        const Vertex *v = q->vertices;
        float sx = fabsf(v[1].x - v[0].x) / w;
        float sy = fabsf(v[4].y - v[0].y) / h;
        if (sx < 1 || sy < 1 || sx != (unsigned)sx || sy != (unsigned)sy ||
            v[0].u != v[4].u || v[0].v != v[1].v ||
            v[1].u != v[2].u || v[2].v != v[4].v ||
            v[2].u != v[3].u || v[2].v != v[3].v ||
            v[0].u != v[5].u || v[0].v != v[5].v) return 0;
    }
    key = (((((uint64_t)i << 13) | (unsigned)x) << 13 | (unsigned)y) << 13 |
           (unsigned)w) << 13 | (unsigned)h;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (binary_regions[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < sizeof binary_regions / sizeof binary_regions[0] && binary_regions[lo] == key;
}
#include "virtual_capture_relaxed.inc"
/* Finalize the last returned writer buffer before considering any operation.
 * Its caller has finished writing by the next runner call or target reset. */
static int finish_quad(void) {
    unsigned i;
    Quad *q;
    if (!last_pending) return 1;
    last_pending = 0;
    q = &quads[used - 1];
    quad_reason = VC_CLIP;
    if (capture_relaxed) {
        int clip = relaxed_quad(q);
        if (clip < 0) return 0;
        replay_clip[used - 1] = (unsigned char)clip;
        if (clip == REPLAY_CLIP) ++virtual_capture_clipped_quads;
        ++virtual_capture_quads_recorded;
        return 1;
    }
    for (i = 0; i < 6; ++i) {
        Vertex *v = &q->vertices[i];
        if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) ||
            !isfinite(v->u) || !isfinite(v->v) || v->x < 0 || v->y < 0 ||
            v->x > 70 || v->y > 70) return 0;
    }
    /* Keep the task's visual guarantee. Fractional or rotated capture edges
     * would change rasterization when the intermediate 70x70 image is removed. */
    quad_reason = VC_TRANSFORM;
    for (i = 0; i < 6; ++i)
        if (q->vertices[i].x != (int)q->vertices[i].x ||
            q->vertices[i].y != (int)q->vertices[i].y) return 0;
    if (q->vertices[0].x != q->vertices[4].x ||
        q->vertices[0].y != q->vertices[1].y ||
        q->vertices[1].x != q->vertices[2].x ||
        q->vertices[2].y != q->vertices[4].y ||
        q->vertices[2].x != q->vertices[3].x ||
        q->vertices[2].y != q->vertices[3].y ||
        q->vertices[0].x != q->vertices[5].x ||
        q->vertices[0].y != q->vertices[5].y) return 0;
    quad_reason = VC_TEXEL;
    for (i = 0; i < 6; ++i) if (q->vertices[i].colour != 0xffffffffu) return 0;
    if (!binary_region(q)) return 0;
    ++virtual_capture_quads_recorded;
    return 1;
}
static void materialize(unsigned reason, unsigned line) {
    int still_target;
    if (internal || (!virtual_target && !pending)) return;
    (void)finish_quad();
    reject(line);
    ++virtual_capture_materializations;
    ++virtual_capture_materializations_by_reason[reason];
    still_target = virtual_target;
    /* Barriers run BEFORE mutations. All recorded quads have the current
     * shader/blend/filter state. Set/reset supply the original camera state. */
    internal = 1; virtual_capture_armed = 0;
    original_target(0, surface_id, surface_id);
    if (cleared) original_clear(clear_colour, clear_alpha);
    emit_identity();
    if (!still_target) original_reset();
    internal = 0;
    virtual_target = pending = last_pending = 0;
    candidate = 0;
}
static void barrier(unsigned reason, unsigned line) {
    if (internal) return;
    if (!finish_quad()) materialize(quad_reason, line);
    materialize(reason, line);
}
/* Called by generated entry stubs, with every argument register preserved. */
unsigned virtual_capture_armed_hits[NHOOKS]; /* loader diagnostic */
uintptr_t AAPCS virtual_capture_barrier(unsigned hook, const uint32_t *args) {
    unsigned i;
    unsigned before = virtual_capture_materializations;
    ++virtual_capture_armed_hits[hook];
    if (!internal && (virtual_target || pending)) {
        if (!finish_quad()) materialize(quad_reason, __LINE__);
        /* The rim script can be a no-op after reset under this policy. Its
         * actual surface operations still encounter their own barriers. */
        if ((virtual_target || pending) &&
            !(specs[hook].mode == 2 && !virtual_target && policy_ok())) {
            materialize(VC_OPERATION, __LINE__);
        }
    }
    if (before != virtual_capture_materializations) {
        ++virtual_capture_materializations_by_hook[hook];
        caller_hit(hook, args[5]); /* r0-r3, ip, runner LR saved by stub */
    }
    /* A modified or freed atlas never becomes eligible again just because its
     * id or native pointer gets reused. Materialize BEFORE invalidating it. */
    if (specs[hook].mutation) {
        for (i = 0; i < sizeof atlas / sizeof atlas[0]; ++i)
            if (args[0] == i || args[0] == (uint32_t)(uintptr_t)atlas[i].texture ||
                specs[hook].mutation == 2) atlas[i].dirty = 1;
    }
    return trampolines[hook];
}
/* Common ARM barrier entry. The stub already reserved 8 bytes for its target
 * and saved r0-r3/ip/lr. Preserve caller-saved VFP registers and flags too. */
__attribute__((naked, pcs("aapcs"))) static void barrier_entry(void) {
    __asm__ volatile(
        "vpush {d0-d7}\n vpush {d16-d31}\n"
        "mrs r1, apsr\n vmrs r2, fpscr\n push {r1,r2}\n"
        "add r1, sp, #200\n"
        "bl virtual_capture_barrier\n"
        "str r0, [sp, #224]\n"
        "pop {r1,r2}\n vmsr fpscr, r2\n msr apsr_nzcvq, r1\n"
        "vpop {d16-d31}\n vpop {d0-d7}\n"
        "pop {r0-r3,r12,lr}\n ldr pc, [sp], #8\n");
}
void AAPCS virtual_capture_script(void *self, void *other, RV *result, int argc, RV **argv) {
    int id;
    int outer = in_script;
    if (outer) {
        barrier(VC_NEW_CAPTURE, __LINE__);
        candidate = 0; ++in_script;
        original_capture(self, other, result, argc, argv);
        --in_script; return;
    }
    if (virtual_target || (pending && !consumed))
        barrier(VC_NEW_CAPTURE, __LINE__);
    candidate = !outer && policy_ok() && state_ok(virtual_capture_relaxed) &&
        surface_value(global_value(var_sprite), &id);
    expected_surface = candidate ? id : -1;
    if (!candidate) { reject(__LINE__); ++virtual_capture_capture_fallbacks; }
    in_script = outer + 1;
    original_capture(self, other, result, argc, argv);
    if (virtual_target) barrier(VC_PROTOCOL, __LINE__);
    in_script = outer;
    candidate = 0;
}
int AAPCS virtual_capture_target(int index, int id, int depth_id) {
    void *surface;
    if (!internal && candidate && in_script == 1 && !virtual_target && (!pending || consumed) &&
        index == 0 && id == expected_surface && depth_id == id && state_ok(virtual_capture_relaxed) && policy_ok() &&
        *graphics_initialized && *surface_stack >= 0 && *surface_stack < 17 &&
        *render_stack < 99 && current_targets[0] != id) {
        void *texture = NULL;
        unsigned was_armed = virtual_capture_armed;
        internal = 1; virtual_capture_armed = 0;
        surface = surface_get(id);
        if (surface) texture = surface_texture_get((int)word(surface, 4));
        internal = 0; virtual_capture_armed = was_armed;
        if (surface && texture && word(surface, 12) == 70 && word(surface, 16) == 70) {
            /* UpdateDefaultCamera uses this camera's 2D mode. A missing/3D
             * camera or texture creation needs original setup. Normal target
             * setup preserves target-active; the special FBO path sets -1. */
            void *camera = camera_get(camera_manager, *default_camera_id);
            virtual_camera_ok = camera && *((uint8_t *)camera + 0x1e0) &&
                (int)word(texture, 0x1c) != -1;
            if (!virtual_camera_ok) {
                reject(__LINE__); barrier(VC_OPERATION, __LINE__);
                return original_target(index, id, depth_id);
            }
            virtual_projection_flip = !word(texture, 0x20) && *render_target_active != -1;
            virtual_matrices_ready = virtual_frustum_ready = 0;
            surface_texture = texture;
            surface_texture_id = (int)word(surface, 4);
            surface_id = id; virtual_target = 1; pending = 0;
            capture_relaxed = virtual_capture_relaxed != 0;
            virtual_capture_armed = 12;
            used = last_pending = consumed = cleared = 0;
            return 1;
        }
        reject(__LINE__);
    }
    barrier(VC_OPERATION, __LINE__);
    return original_target(index, id, depth_id);
}
/* Same CPU builders and multiplication order as Build2DView, ApplyMatrices
 * and SetMatrices. Never write the GPU's current matrices or frustum cache. */
static void build_virtual_matrices(void) {
    static const float eye[3] = {35,35,-16000}, at[3] = {35,35,0}, up[3] = {-0.0f,1,0};
    static const float flip[16] = {1,0,0,0,0,-1,0,0,0,0,1,0,0,0,0,1};
    float projection[16] __attribute__((aligned(16)));
    if (virtual_matrices_ready) return;
    matrix_lookat(virtual_matrices[0], eye, at, up);
    matrix_ortho(projection, 70, 70, 0, 32000);
    if (virtual_projection_flip) matrix_multiply(virtual_matrices[1], projection, flip);
    else memcpy(virtual_matrices[1], projection, 64);
    memcpy(virtual_matrices[2], matrices + 32, 64);
    matrix_multiply(virtual_matrices[3], virtual_matrices[2], virtual_matrices[0]);
    matrix_multiply(virtual_matrices[4], virtual_matrices[3], virtual_matrices[1]);
    virtual_matrices_ready = 1;
}
static void observation_fallback(unsigned hook, uintptr_t lr, unsigned line) {
    unsigned before = virtual_capture_materializations;
    barrier(VC_OPERATION, line);
    if (before != virtual_capture_materializations) {
        ++virtual_capture_materializations_by_hook[hook]; caller_hit(hook, lr);
    }
}
static void finish_observation(unsigned hook, uintptr_t lr, unsigned line) {
    if (!finish_quad()) {
        unsigned before = virtual_capture_materializations;
        materialize(quad_reason, line);
        if (before != virtual_capture_materializations) {
            ++virtual_capture_materializations_by_hook[hook]; caller_hit(hook, lr);
        }
    }
}
void AAPCS virtual_capture_matrix(int matrix, float *out) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    if (!internal && virtual_target) {
        finish_observation(H_MATRIX, lr, __LINE__);
        if (virtual_target && matrix != 2) {
            if (virtual_camera_ok && (unsigned)matrix < 5) {
                build_virtual_matrices(); memcpy(out, virtual_matrices[matrix], 64); return;
            }
            observation_fallback(H_MATRIX, lr, __LINE__);
        }
    }
    /* Target setup leaves world unchanged. After reset all matrices belong
     * to the current real target, even while captured pixels remain pending. */
    original_matrix(matrix, out);
}
void *AAPCS virtual_capture_frustum(void) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    uintptr_t caller = lr - bias;
    int sprite_cull = caller == 0x00d616f8 || caller == 0x00d63a64 ||
        caller == 0x00d64104 || caller == 0x00d64458 || caller == 0x00d64e08;
    if (!internal && virtual_target) {
        finish_observation(H_FRUSTUM, lr, __LINE__);
        /* These callers only read the planes in IntersectsSphere. Other users
         * could retain the real cache pointer, which a private copy cannot mimic. */
        if (virtual_target && virtual_camera_ok && sprite_cull) {
            if (!virtual_frustum_ready) {
                float vp[16] __attribute__((aligned(16)));
                build_virtual_matrices();
                matrix_multiply(vp, virtual_matrices[0], virtual_matrices[1]);
                frustum_build(virtual_frustum, vp); virtual_frustum_ready = 1;
            }
            return virtual_frustum;
        }
        observation_fallback(H_FRUSTUM, lr, __LINE__);
    }
    return original_frustum();
}
int AAPCS virtual_capture_reset(void) {
    if (!internal && virtual_target) {
        if (!finish_quad()) materialize(quad_reason, __LINE__);
        if (virtual_target && cleared) {
            virtual_target = 0; pending = 1;
            ++virtual_capture_virtual_captures;
            return 1;
        }
        materialize(VC_PROTOCOL, __LINE__);
    } else barrier(VC_OPERATION, __LINE__);
    return original_reset();
}
void AAPCS virtual_capture_clear(uint32_t colour, float alpha) {
    if (!internal && virtual_target && !cleared && !used && colour == 0 && alpha == 0) {
        cleared = 1; clear_colour = colour; clear_alpha = alpha; return;
    }
    barrier(VC_PROTOCOL, __LINE__);
    original_clear(colour, alpha);
}
static int sprite_writer(uintptr_t return_address) {
    /* ELF ranges for every GR_Texture_Draw* writer and Graphics::DrawQuad.
     * Generated from dynsym; text/surface entries have earlier barriers. */
    uintptr_t p = return_address - bias;
    return (p > 0x00eafa20 && p < 0x00eb1bfc) ||
           (p > 0x00e878ac && p < 0x00e87980);
}
void *AAPCS virtual_capture_alloc(int type, void *texture, int stride, int count) {
    if (!internal && virtual_target) {
        if (!finish_quad()) materialize(quad_reason, __LINE__);
        if (virtual_target && cleared && type == 4 && stride == 24 && count == 6 &&
            texture && texture != surface_texture &&
            sprite_writer((uintptr_t)__builtin_return_address(0)) && used < MAX_QUADS) {
            Quad *q = &quads[used++];
            q->texture = texture;
            memset(q->vertices, 0, sizeof q->vertices);
            last_pending = 1;
            return q->vertices;
        }
        materialize(used == MAX_QUADS ? VC_CAPACITY : VC_FORMAT, __LINE__);
    } else if (!internal && pending && texture == surface_texture)
        barrier(VC_OPERATION, __LINE__);
    return original_alloc(type, texture, stride, count);
}
static uint32_t multiply_colour(uint32_t a, uint32_t b) {
    uint32_t out = a & 0xff000000u;
    unsigned shift;
    for (shift = 0; shift < 24; shift += 8)
        out |= ((((a >> shift) & 255) * ((b >> shift) & 255)) / 255) << shift;
    return out;
}
void *AAPCS virtual_capture_texture_get(int id, bool a, bool b, bool c, bool d) {
    if (!internal && (virtual_target || pending) && id == surface_texture_id)
        barrier(VC_OPERATION, __LINE__);
    void *out = original_texture_get(id, a, b, c, d);
    if (out && id >= 0 && (unsigned)id < sizeof atlas / sizeof atlas[0]) {
        float iw, ih;
        memcpy(&iw, (const uint8_t *)out + 8, 4);
        memcpy(&ih, (const uint8_t *)out + 12, 4);
        if (iw == 1.0f / atlas_pages[id].width && ih == 1.0f / atlas_pages[id].height)
            atlas[id].texture = (void *)(uintptr_t)word(out, 0);
    }
    return out;
}
static int aligned_transform(float x, float y, float xs, float ys, float angle) {
    return isfinite(x) && isfinite(y) && isfinite(xs) && isfinite(ys) && angle == 0 &&
        fabsf(x) <= 1048576 && fabsf(y) <= 1048576 && fabsf(xs) <= 64 && fabsf(ys) <= 64 &&
        x == (int)x && y == (int)y && xs == (int)xs && ys == (int)ys && xs != 0 && ys != 0;
}
void AAPCS virtual_capture_surface(int id, float x, float y, float xs, float ys,
                                   float angle, uint32_t colour, float alpha) {
    unsigned i, j;
    float sn, cs;
    int relaxed = virtual_capture_relaxed != 0;
    int transform_ok = relaxed ? relaxed_transform(x,y,xs,ys,angle) :
                                 aligned_transform(x,y,xs,ys,angle);
    if (!internal && pending && id == surface_id && alpha == 1 &&
        transform_ok && relaxed == capture_relaxed &&
        state_ok(relaxed) && policy_ok()) {
        /* GR_Surface_Draw converts degrees to radians with this float literal.
         * Map the captured 70x70 coordinates through the same rotation sign. */
        float radians = angle * 0.01745329238474369049f;
        if (fabsf(radians) < 0.0001f) { sn = 0; cs = 1; }
        else { sn = sinf(radians); cs = cosf(radians); }
        (void)get_colours(colour, alpha);
        internal = 1; virtual_capture_armed = 0;
        for (i = 0; i < used; ++i) {
            Vertex clipped[30], *out;
            const Vertex *vertices = quads[i].vertices;
            unsigned count = 6;
            if (relaxed && replay_clip[i] == REPLAY_DROP) continue;
            if (relaxed && replay_clip[i] == REPLAY_CLIP) {
                count = clip_quad(vertices, clipped);
                vertices = clipped;
                if (!count) continue;
            }
            out = original_alloc(4, quads[i].texture, 24, (int)count);
            memcpy(out, vertices, count * sizeof *out);
            for (j = 0; j < count; ++j) {
                float px = out[j].x * xs, py = out[j].y * ys;
                out[j].x = x + cs * px + sn * py;
                out[j].y = y - sn * px + cs * py;
                out[j].z = *draw_depth;
                out[j].colour = multiply_colour(out[j].colour, colour);
            }
        }
        internal = 0; consumed = 1; virtual_capture_armed = 12;
        ++virtual_capture_direct_replays;
        if (relaxed) ++virtual_capture_relaxed_replays;
        return;
    }
    barrier(alpha != 1 ? VC_ALPHA : !transform_ok ? VC_TRANSFORM : VC_STATE,
            __LINE__);
    original_surface(id, x, y, xs, ys, angle, colour, alpha);
}
/* These helpers relocate the audited eight-byte ARM prologues. */
static uint32_t mov16(unsigned top, unsigned reg, uint32_t value) {
    return (top ? 0xe3400000u : 0xe3000000u) | (reg << 12) |
        ((value & 0xf000u) << 4) | (value & 0xfffu);
}
static unsigned load_constant(uint32_t *code, unsigned reg, uint32_t v) {
    code[0] = mov16(0, reg, v); code[1] = mov16(1, reg, v >> 16); return 2;
}
static int install_fail(unsigned line) {
    reject(line); ++virtual_capture_install_failures;
    debugPrintf("virtual_capture_install: check at line %u failed\n", line);
    return 0;
}
int virtual_capture_install(void *(*rx_alloc)(size_t)) {
    unsigned i, j, pos = 0;
    uint32_t code[NHOOKS * 20];
    uintptr_t replacements[NHOOKS];
    unsigned starts[NHOOKS], stubs[NHOOKS];
    uint8_t *rx;
    if (installed || !rx_alloc) return install_fail(__LINE__);
#define RESOLVE(type, s) ((type)so_symbol(&yoyoloader_mod, s))
    for (i = 0; i < NHOOKS; ++i) {
        entries[i] = RESOLVE(uintptr_t, specs[i].name);
        if (!entries[i] || (entries[i] & 3) || entries[i] < specs[i].offset ||
            word((void *)entries[i], 0) != specs[i].prologue[0] ||
            word((void *)entries[i], 4) != specs[i].prologue[1]) return install_fail(__LINE__);
        if (!i) bias = entries[i] - specs[i].offset;
        else if (entries[i] - specs[i].offset != bias) return install_fail(__LINE__);
    }
    for (i = 0; i < sizeof matrix_helpers / sizeof matrix_helpers[0]; ++i) {
        uintptr_t p = RESOLVE(uintptr_t, matrix_helpers[i].name);
        if (p != bias + matrix_helpers[i].offset ||
            word((void *)p, 0) != matrix_helpers[i].prologue[0] ||
            word((void *)p, 4) != matrix_helpers[i].prologue[1]) return install_fail(__LINE__);
    }
    global_object = RESOLVE(void **, "g_pGlobal");
    active_shader = RESOLVE(void **, "g_ActiveUserShader");
    next_shader = RESOLVE(void **, "g_NextActiveUserShader");
    native_shader = RESOLVE(int *, "g_ActiveUserShaderM");
    next_native_shader = RESOLVE(int *, "g_NextActiveUserShaderM");
    texture_stage = RESOLVE(int *, "g_DefaultTextureStage");
    render_states = RESOLVE(const uint8_t *, "g_States");
    matrices = RESOLVE(const float *, "m_CurrentMatrix");
    draw_depth = RESOLVE(const float *, "GR_Depth");
    var_sprite = RESOLVE(const uint32_t *, "g_VAR_my_sprite");
    var_rim = RESOLVE(const uint32_t *, "g_VAR_rim_shading");
    var_slowdown = RESOLVE(const uint32_t *, "g_VAR_slowdown");
    functions = RESOLVE(Function **, "the_functions");
    clear_descriptor = RESOLVE(const uint32_t *, "g_FUNC_draw_clear_alpha");
    surface_descriptor = RESOLVE(const uint32_t *, "g_FUNC_draw_surface_ext");
    get_colours = RESOLVE(colours_fn, "_ZN8Graphics14GetColourArrayEjf");
    surface_get = RESOLVE(get_surface_fn, "_Z14GR_Surface_Geti");
    surface_texture_get = RESOLVE(get_surface_fn, "_Z22GR_Texture_Get_Surfacei");
    camera_manager = RESOLVE(void *, "g_CM");
    default_camera_id = RESOLVE(const int *, "g_DefaultCameraID");
    render_target_active = RESOLVE(const int *, "g_RenderTargetActive");
    camera_get = RESOLVE(camera_fn, "_ZN14CCameraManager9GetCameraEi");
    matrix_lookat = RESOLVE(lookat_fn, "_ZN8yyMatrix8LookAtLHEPS_P9yyVector3S2_S2_");
    matrix_ortho = RESOLVE(ortho_fn, "_ZN8yyMatrix7OrthoLHEPS_ffff");
    matrix_multiply = RESOLVE(multiply_fn, "_ZN8yyMatrix8MultiplyEPS_S0_S0_");
    frustum_build = RESOLVE(frustum_build_fn, "_ZN9yyFrustum15FromViewProjMatERK8yyMatrix");
    graphics_initialized = RESOLVE(uint8_t *, "g_GraphicsInitialised");
    mark_vertices = RESOLVE(uint8_t *, "g_MarkVerts");
    surface_stack = RESOLVE(int *, "StackSP");
    current_targets = RESOLVE(int *, "currenttargets");
    render_stack = (const int *)(bias + 0x04e4a610u);
    if (!global_object || !*global_object || !active_shader || !next_shader || !native_shader ||
        !next_native_shader || !texture_stage || !render_states || !matrices || !draw_depth ||
        !var_sprite || !var_rim || !var_slowdown ||
        !functions || !*functions || !clear_descriptor || !surface_descriptor || !get_colours || !surface_get ||
        !surface_texture_get || !graphics_initialized || !surface_stack || !current_targets || !mark_vertices ||
        !camera_manager || !default_camera_id || !render_target_active || !camera_get || !matrix_lookat ||
        !matrix_ortho || !matrix_multiply || !frustum_build)
        return install_fail(__LINE__);
    if ((*functions)[clear_descriptor[1]].call != RESOLVE(void *, "_Z16F_DrawClearAlphaR6RValueP9CInstanceS2_iPS_") ||
        (*functions)[surface_descriptor[1]].call != RESOLVE(void *, "_Z16F_DrawSurfaceExtR6RValueP9CInstanceS2_iPS_"))
        return install_fail(__LINE__);
#undef RESOLVE
    /* Validate everything before allocating or changing any runner entry. */
    rx = rx_alloc(sizeof code);
    if (!rx || ((uintptr_t)rx & 3) || (uintptr_t)rx > UINTPTR_MAX - sizeof code ||
        ((uintptr_t)rx < bias + 0x04e80000u && (uintptr_t)rx + sizeof code > bias))
        return install_fail(__LINE__);
    for (i = 0; i < NHOOKS; ++i) {
        const HookSpec *h = &specs[i];
        starts[i] = pos;
        if (h->relocation == 2) {
            uint32_t value = word((void *)(bias + h->pc + h->literal), 0);
            pos += load_constant(code + pos, (h->prologue[0] >> 12) & 15, value);
        } else for (j = 0; j < 2; ++j) {
            if (h->relocation == 4 && j == 1) {
                /* Preserve the original condition, jump to an absolute ARM
                 * address if true, and skip the literal if false. */
                code[pos++] = (h->prologue[j] & 0xf0000000u) | 0x059ff000u;
                code[pos++] = 0xea000000u;
                code[pos++] = (uint32_t)(bias + h->literal);
            } else if ((h->relocation == 1 && j == 0) || (h->relocation == 3 && j == 1))
                pos += load_constant(code + pos, (h->prologue[j] >> 12) & 15, h->literal);
            else code[pos++] = h->prologue[j];
        }
        code[pos++] = 0xe51ff004; code[pos++] = (uint32_t)(entries[i] + 8);
        stubs[i] = pos;
        trampolines[i] = (uintptr_t)(rx + 4 * starts[i]);
        replacements[i] = (uintptr_t)(rx + 4 * stubs[i]);
        /* Capture/target admission and texture discovery must run while idle.
         * Atlas mutation hooks also retain their lifetime audit while idle.
         * All other entries bypass C, VFP saves and flag saves in six ARM
         * instructions, preserving ip, all arguments and the original stack. */
        if (i != H_CAPTURE && i != H_TARGET && i != H_TEXTURE_GET && !h->mutation) {
            code[pos++] = 0xe52dc004; /* push {ip} */
            code[pos++] = 0xe59fc004; /* ldr ip,[pc,#4] */
            code[pos++] = 0xe59cc000; /* ldr ip,[ip] */
            code[pos++] = 0xe08ff00c; /* add pc,pc,ip (no flags) */
            code[pos++] = (uint32_t)(uintptr_t)&virtual_capture_armed;
            code[pos++] = 0xe49dc004; /* pop {ip} */
            code[pos++] = 0xe51ff004; /* ldr pc,[pc,#-4] */
            code[pos++] = (uint32_t)trampolines[i];
            code[pos++] = 0xe49dc004; /* armed: pop {ip} */
        }
        uintptr_t specialized = i == H_RESET ? (uintptr_t)virtual_capture_reset :
            i == H_CLEAR ? (uintptr_t)virtual_capture_clear :
            i == H_ALLOC ? (uintptr_t)virtual_capture_alloc :
            i == H_CONSUME ? (uintptr_t)virtual_capture_surface :
            i == H_MATRIX ? (uintptr_t)virtual_capture_matrix :
            i == H_FRUSTUM ? (uintptr_t)virtual_capture_frustum : 0;
        if (specialized) {
            code[pos++] = 0xe51ff004;
            code[pos++] = (uint32_t)specialized;
        } else {
            code[pos++] = 0xe24dd008; /* sub sp,sp,#8 */
            code[pos++] = 0xe92d500f; /* push {r0-r3,ip,lr} */
            code[pos++] = mov16(0, 0, i); /* movw r0,#hook */
            code[pos++] = 0xe51ff004;
            code[pos++] = (uint32_t)(uintptr_t)barrier_entry;
        }
    }
    kuKernelCpuUnrestrictedMemcpy(rx, code, pos * 4);
    kuKernelFlushCaches(rx, pos * 4);
    original_capture = (script_fn)trampolines[H_CAPTURE];
    original_target = (target_fn)trampolines[H_TARGET];
    original_reset = (reset_fn)trampolines[H_RESET];
    original_clear = (clear_fn)trampolines[H_CLEAR];
    original_alloc = (alloc_fn)trampolines[H_ALLOC];
    original_surface = (surface_fn)trampolines[H_CONSUME];
    original_texture_get = (texture_get_fn)trampolines[H_TEXTURE_GET];
    original_matrix = (matrix_fn)trampolines[H_MATRIX];
    original_frustum = (frustum_fn)trampolines[H_FRUSTUM];
    for (i = 0; i < NHOOKS; ++i)
        if (strcmp(specs[i].name, "_Z14GR_Surface_Geti") == 0)
            surface_get = (get_surface_fn)trampolines[i];
    replacements[H_CAPTURE] = (uintptr_t)virtual_capture_script;
    replacements[H_TARGET] = (uintptr_t)virtual_capture_target;
    replacements[H_TEXTURE_GET] = (uintptr_t)virtual_capture_texture_get;
    for (i = 0; i < NHOOKS; ++i) {
        hook_addr(entries[i], replacements[i]);
        kuKernelFlushCaches((void *)entries[i], 8);
    }
    installed = 1; return 1;
}

/* Keep loader report accessors in the standalone implementation too. */
int virtual_capture_hook_count(void) { return NHOOKS; }
const char *virtual_capture_hook_name(int i) { return i >= 0 && i < (int)NHOOKS ? specs[i].name : "?"; }
