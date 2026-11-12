/* Draft for libyoyo.so SHA256 ab7014c1...bd4701 only.
 * No query replacement: retain the runner's tree ordering, dirty lists,
 * candidate callback, precise collision, argument conversion and restoration.
 * Compile with softfp, without fast-math, and with -ffp-contract=off.
 * The installer needs 16 bytes of caller-owned, aligned executable memory.
 * Keep that memory allocated until the runner is unloaded.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifdef COLLISION_STUB
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif

#define AAPCS __attribute__((pcs("aapcs")))
typedef struct { float left, top, right, bottom; } Rect;
typedef void AAPCS (*bbox_fn)(void *, bool);
typedef void *AAPCS (*sprite_fn)(int);
typedef void AAPCS (*update_fn)(void *, Rect *);
typedef long AAPCS (*round_fn)(float);
static bbox_fn original_bbox;
static sprite_fn sprite_data;
static update_fn collision_update;
static round_fn runner_lrintf;
static const uint8_t *compat_mode, *profile_mode;
static bool installed;
unsigned collision_fast_hits, collision_fallback_hits;

/* memcpy avoids aliasing CInstance/CSprite through invented C++ layouts. */
static uint32_t word(const void *p, size_t off) {
    uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v;
}
static int integer(const void *p, size_t off) {
    int v; memcpy(&v, (const uint8_t *)p + off, 4); return v;
}
static float scalar(const void *p, size_t off) {
    float v; memcpy(&v, (const uint8_t *)p + off, 4); return v;
}
static void put_word(void *p, size_t off, uint32_t v) {
    memcpy((uint8_t *)p + off, &v, 4);
}
static int bounded(float v) { return isfinite(v) && fabsf(v) < 1048576.0f; }

/* Rebuild from sprite-local edges. Translating a rounded world box loses
 * fractional phase and can change lrintf ties and single-precision additions.
 * This also works for ordinary precise/rotated-rectangle sprite mask modes:
 * only bbox construction is replaced, never the narrow-phase test.
 */
static int common_box(void *inst, Rect *out, uint32_t *flags) {
    if (!inst || *profile_mode || word(inst, 0x70) || word(inst, 0x74) ||
        scalar(inst, 0xac) != 0.0f || scalar(inst, 0xa4) != 1.0f ||
        scalar(inst, 0xa8) != 1.0f)
        return 0;
    int draw = integer(inst, 0x88), mask = integer(inst, 0x14c);
    void *sp = sprite_data(mask >= 0 ? mask : draw);
    if (!sp || integer(sp, 0x84) != 0) return 0; /* ordinary bitmap sprite only */
    int mode = integer(sp, 0x8c);
    if (mode < 0 || mode > 2) return 0; /* no skeleton mask or unknown mode */
    /* With an explicit mask, Compute_BoundingBox does not call
     * SkeletonAnimation. SetPosition and candidate collision remain original.
     * With no mask, the selected draw sprite above is ordinary and both
     * skeleton pointers are null, so SkeletonAnimation has no side effects.
     */
    const uint8_t *bitmap = (const uint8_t *)(uintptr_t)word(sp, 0x38);
    if (bitmap && bitmap[0x6c]) return 0; /* GetScaledBoundingBox path */
    float x = scalar(inst, 0xb8), y = scalar(inst, 0xbc);
    float l = scalar(sp, 0x48), t = scalar(sp, 0x4c);
    float r = scalar(sp, 0x50), b = scalar(sp, 0x54);
    int ox = integer(sp, 0x64), oy = integer(sp, 0x68);
    if (!bounded(x) || !bounded(y) || !bounded(l) || !bounded(t) ||
        !bounded(r) || !bounded(b) || ox <= -1048576 || ox >= 1048576 ||
        oy <= -1048576 || oy >= 1048576) return 0;
    /* Volatile stages force the same f32 operation boundaries as VFP vmla
     * on this ARMv7 runner. Its vmla is not a fused multiply-add.
     */
    volatile float w = r - l, h = b - t;
    volatile float dx = l - (float)ox, dy = t - (float)oy;
    volatile float ww = w + 1.0f, hh = h + 1.0f;
    volatile float left = x + dx, right = left + ww;
    volatile float top = y + dy, bottom = top + hh;
    Rect q = {left, top, right, bottom};
    if (!bounded(q.left) || !bounded(q.top) || !bounded(q.right) ||
        !bounded(q.bottom)) return 0;
    if (*compat_mode) {
        /* Call the loader-resolved runner import, including its rounding
         * behavior. so_symbol on an undefined import is not its function.
         */
        q.left = (float)runner_lrintf(q.left);
        q.right = (float)runner_lrintf(q.right);
        q.top = (float)runner_lrintf(q.top);
        q.bottom = (float)runner_lrintf(q.bottom);
    }
    if (q.left > q.right) { float tmp = q.left; q.left = q.right; q.right = tmp; }
    if (q.top > q.bottom) { float tmp = q.top; q.top = q.bottom; q.bottom = tmp; }
    if (*compat_mode) { q.right -= 1.0f; q.bottom -= 1.0f; }
    *flags = (word(inst, 0x7c) & ~(8u | 0x200u)) |
             (mode == 1 ? 0x200u : 0u);
    *out = q;
    return 1;
}

static void AAPCS collision_bbox(void *inst, bool update_tree) {
    Rect box, old;
    uint32_t flags;
    if (!common_box(inst, &box, &flags)) {
        ++collision_fallback_hits;
        original_bbox(inst, update_tree);
        return;
    }
    memcpy(&old, (uint8_t *)inst + 0xec, sizeof(old));
    memcpy((uint8_t *)inst + 0xec, &box, sizeof(box));
    put_word(inst, 0x7c, flags);
    ++collision_fast_hits;
    /* Original f2608c..f260a0: update even if the box was already clean or
     * numerically equal. Preserve removal/insertion and resulting leaf order.
     */
    if (update_tree) collision_update(inst, &old);
}

/* Call once after relocation/import resolution and before runner execution.
 * This is an entry hook, so direct calls produced by patch_plt_calls are caught.
 * rx must be aligned RX memory with at least 16 bytes, disjoint from the runner.
 * No PC-relative instructions occur in the two displaced ARM words.
 * Return 1 on success, 0 if any binary/loader precondition is unmet.
 */
int collision_install(void *rx, size_t rx_size) {
    const char *sym = "_ZN9CInstance19Compute_BoundingBoxEb";
    if (installed || !rx || ((uintptr_t)rx & 3u) || rx_size < 16) return 0;
    uintptr_t entry = so_symbol(&yoyoloader_mod, sym);
    if (!entry || (entry & 3u) || word((void *)entry, 0) != 0xe92d4ff0u ||
        word((void *)entry, 4) != 0xe28db01cu) return 0;
    sprite_data = (sprite_fn)so_symbol(&yoyoloader_mod, "_Z11Sprite_Datai");
    collision_update = (update_fn)so_symbol(&yoyoloader_mod,
                                           "_Z15CollisionUpdateP9CInstanceR9tagYYRECT");
    compat_mode = (const uint8_t *)so_symbol(&yoyoloader_mod,
                                            "g_Collision_Compatibility_Mode");
    profile_mode = (const uint8_t *)so_symbol(&yoyoloader_mod, "g_bProfile");
    uintptr_t *round_slot = so_got_slot(&yoyoloader_mod, "lrintf");
    if (!sprite_data || !collision_update || !compat_mode || !profile_mode ||
        !round_slot || !*round_slot) return 0;
    runner_lrintf = (round_fn)*round_slot;
    uint32_t trampoline[4] = {0xe92d4ff0u, 0xe28db01cu,
                             0xe51ff004u, (uint32_t)(entry + 8)};
    kuKernelCpuUnrestrictedMemcpy(rx, trampoline, sizeof(trampoline));
    kuKernelFlushCaches(rx, sizeof(trampoline));
    original_bbox = (bbox_fn)rx;
    hook_addr(entry, (uintptr_t)collision_bbox);
    kuKernelFlushCaches((void *)entry, 8);
    installed = true;
    return 1;
}
