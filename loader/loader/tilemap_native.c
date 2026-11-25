/*
 * tilemap_native.c
 *
 * Native tile layer draw: DrawLayerTilemapElement (libyoyo.so SHA256 ab7014c1..., 0xef1b54, 2052 bytes) builds the
 * view's tile quads into the runner's vertex batch every frame. In the six rooms below 60 fps this costs 0.8-1.4 ms
 * per frame (frame budget audit, Nov 25: DrawRoomLayers). Per drawn tile the runner reloads the element and tileset
 * fields, converts the animation frame, splits the tile index into UV column and row with a software division
 * (__aeabi_uidiv: the Cortex-A9 has no divide instruction), does about 20 VFP operations and 36 stores.
 *
 * This file does the same draw with the same results:
 * - the same runner calls in the same order with the same arguments: Tileset_Exists, Tileset_Data,
 *   CalcLayerTilemapParams (same initial values), Graphics::GetColourArray(0xffffffff, 1.0),
 *   Graphics::GetMaxDynamicVertexCount, and the same Graphics::AllocVerts / Graphics::FreeVerts sequence (the
 *   reservation is min(remaining cells of the view window, max vertices / 6) tiles at the first drawn tile and
 *   whenever the reservation is used up; the unused tail is freed at the end);
 * - the same view window (the integer conversions are VFP vcvt, round toward zero, saturating);
 * - the same vertex bytes, in the same order: per tile six 24-byte vertices (x, y, depth, colour, u, v) TL, TR, BL,
 *   BL, TR, BR, with the runner's float operations (this file is built with -ffp-contract=off; VFP vmla rounds twice,
 *   like a C multiply then add). Mirror, flip and rotate pick the same UV corners.
 * What changes is only how the floats are reached: the per-tile UVs come from a table per tileset parameter set
 * (filled with the runner's operations on first use of a tile index), and the column positions are computed once per
 * draw, not once per tile.
 *
 * Admission (anything else runs the runner's function): the installed function's code matches the analysed one
 * (checksum of every non-bl word after the entry hook), CalcLayerTilemapParams takes its direct-texture path (all
 * tilesets of the game do), the tileset has at least one column, the window is at most 1024 columns wide.
 *
 * Switches: exp_tilenative0 off. exp_tilenativeab: the runner's code in every other report window (in-run A/B).
 * exp_tilenativeprof: time spent in tile layer draws per frame, both paths (sceKernelGetProcessTimeLow per call).
 * exp_tilenativeverify (debugMode): every draw runs the native path into scratch memory first (nothing drawn), then
 * the runner's function for real with Graphics::AllocVerts entry-hooked to capture its vertices; the submissions
 * (primitive, texture, stride, counts, freed tail, every vertex byte) are compared and mismatches counted.
 * exp_tilenativeverifyfault: the native x of the first vertex one float step off (negative control: every verified
 * draw with a tile must mismatch).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <arm_neon.h>
#include <psp2/kernel/processmgr.h>

#include "main.h"
#include "so_util.h"
#include "hooks.h"
#include "katana_internal.h"

#define ABI __attribute__((pcs("aapcs")))

typedef int ABI (*int_int_fn)(int);
typedef uint8_t *ABI (*data_fn)(int);
typedef void ABI (*params_fn)(void *ts, void **tex, float *, float *, float *, float *, float *, float *, float *,
	float *, uint8_t *);
typedef const uint32_t *ABI (*colour_fn)(uint32_t, float);
typedef int ABI (*int_void_fn)(void);
typedef void *ABI (*alloc_fn)(int prim, void *tex, int stride, int count);
typedef void ABI (*free_fn)(int count);
typedef struct {
	alloc_fn alloc;
	free_fn free;
} sink;
typedef void ABI (*draw_fn)(uint32_t rect, uint32_t layer, uint32_t elem, uint32_t x, uint32_t y, uint32_t depth);

static int_int_fn tileset_exists;
static data_fn tileset_data;
static params_fn calc_params;
static colour_fn colour_array;
static int_void_fn max_verts;
static alloc_fn alloc_verts;
static free_fn free_verts;
static const float *gr_depth;
static const uint32_t *tiledata_mask;
static const uint8_t *ignore_cull;
static int *vertex_count;
static draw_fn orig_draw;
static sink live; // the runner's Graphics::AllocVerts and FreeVerts

static int installed, ab_mode, prof, verify, fault;
static int ab_off; // exp_tilenativeab: this report window runs the runner's code

static unsigned n_calls, n_native, n_tiles, n_uv_direct, n_fallback[4];
static uint64_t prof_us[2];
static unsigned prof_calls[2];
static unsigned v_calls, v_bad, v_bytes, v_skipped;

#define FNV_EXPECT 0x442dc622u // DrawLayerTilemapElement words 2..512, bl words skipped (they differ after PLT patching)
#define MAX_COLS 1024

static inline int32_t vcvt_s32(float f) {
	float r;
	__asm__("vcvt.s32.f32 %0, %1" : "=t"(r) : "t"(f));
	int32_t i;
	memcpy(&i, &r, 4);
	return i;
}

static inline uint32_t fbits(float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

/* ---- UV table: per tile index the four (u, v) corner pairs TL, TR, BL, BR, per CalcLayerTilemapParams result and
 * tileset column count. Filled on first use with the runner's operations (u = u offset + column * u step, then + u
 * size; v likewise), so the bits are the runner's. ---- */
#define UV_SLOTS 8
#define UV_N 1024
typedef struct {
	uint32_t key[7]; // u offset, v offset, u size, v size, u step, v step (float bits), tileset columns
	int used;
	uint8_t valid[UV_N];
	uint64_t (*uv)[4];
} uv_slot;
static uv_slot slots[UV_SLOTS];
static int next_slot;

static inline uint64_t pair(float u, float v) {
	return (uint64_t)fbits(u) | (uint64_t)fbits(v) << 32;
}

static void uv_compute(uint64_t *out, uint32_t t, uint32_t cols, const float *p) {
	uint32_t row = t / cols, col = t - row * cols;
	const float u0 = p[0] + (float)col * p[4], v0 = p[1] + (float)row * p[5];
	const float u1 = u0 + p[2], v1 = v0 + p[3];
	out[0] = pair(u0, v0);
	out[1] = pair(u1, v0);
	out[2] = pair(u0, v1);
	out[3] = pair(u1, v1);
}

static uv_slot *uv_slot_for(const float *p, uint32_t cols) {
	uint32_t key[7];
	for (int i = 0; i < 6; i++)
		key[i] = fbits(p[i]);
	key[6] = cols;
	for (int i = 0; i < UV_SLOTS; i++)
		if (slots[i].used && !memcmp(slots[i].key, key, sizeof(key)))
			return &slots[i];
	uv_slot *s = &slots[next_slot];
	next_slot = (next_slot + 1) % UV_SLOTS;
	memcpy(s->key, key, sizeof(key));
	memset(s->valid, 0, sizeof(s->valid));
	s->used = 1;
	return s;
}

/* ---- the draw ---- */

// Which of the UV table's corner pairs (0 TL, 1 TR, 2 BL, 3 BR) the vertices TL, TR, BL, BR get, by the cell's bits
// 28-30 (mirror, flip, rotate), as the runner picks them
static const uint8_t corner_order[8][4] = {
	{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 3, 0, 1}, {3, 2, 1, 0},
	{2, 0, 3, 1}, {3, 1, 2, 0}, {0, 2, 1, 3}, {1, 3, 0, 2},
};

// Returns 0 when the runner's function must run instead (nothing was submitted then)
static __attribute__((noinline)) int draw_native(const float *rect, uint8_t *layer, uint8_t *elem, float fx, float fy, float fdepth,
		const sink *out_fns) {
	if (!tileset_exists(*(int *)(elem + 0x1c)))
		return 1;
	uint8_t *ts = tileset_data(*(int *)(elem + 0x1c));
	void *tex = NULL;
	// u offset, v offset, u size, v size, u step, v step, and two the direct path doesn't read
	float p[8] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f};
	uint8_t direct = 0;
	calc_params(ts, &tex, &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7], &direct);
	float xoff, yoff, depth;
	if (layer) {
		float ly = *(float *)(layer + 0xc);
		yoff = ly + *(float *)(elem + 0x24);
		xoff = *(float *)(layer + 8) + *(float *)(elem + 0x20);
		depth = *gr_depth;
	} else {
		xoff = fx;
		yoff = fy;
		depth = fdepth;
	}
	if (!tex)
		return 1;
	const uint32_t mask = *(uint32_t *)(elem + 0x38) & *tiledata_mask;
	if (!direct) {
		n_fallback[0]++;
		return 0;
	}
	const uint32_t tscols = *(uint32_t *)(ts + 0x2c);
	if (!tscols) {
		n_fallback[1]++;
		return 0;
	}
	const uint32_t *ca = colour_array(0xffffffffu, 1.0f);
	const uint32_t c0 = ca[0], c1 = ca[1], c2 = ca[2], c3 = ca[3];
	const int maxv = max_verts();
	const float tw = (float)*(int32_t *)(ts + 0x1c), th = (float)*(int32_t *)(ts + 0x20);
	const int32_t ew = *(int32_t *)(elem + 0x28), eh = *(int32_t *)(elem + 0x2c);
	int32_t rs, re, cs, ce;
	if (*ignore_cull) {
		rs = cs = 0;
		ce = ew;
		re = eh;
	} else {
		const float xi = (float)vcvt_s32(xoff), yi = (float)vcvt_s32(yoff);
		const float right = rect[2] - xi, left = rect[0] - xi, bottom = rect[3] - yi, top = rect[1] - yi;
		const float ce_f = right + tw, cs_f = left / tw, re_f = bottom + th, rs_f = top / th;
		re = vcvt_s32(re_f / th);
		ce = vcvt_s32(ce_f / tw);
		rs = vcvt_s32(rs_f);
		cs = vcvt_s32(cs_f);
		if (eh < re)
			re = eh;
		if (ew < ce)
			ce = ew;
		if (rs < 0)
			rs = 0;
		if (cs < 0)
			cs = 0;
	}
	if (re <= rs)
		return 1;
	if (ce > cs && ce - cs > MAX_COLS) {
		n_fallback[2]++;
		return 0;
	}
	const int maxper = maxv / 6;
	int32_t remaining = (int32_t)((uint32_t)(ce - cs) * (uint32_t)(re - rs));
	int32_t left_tiles = 0;
	uint32_t *o = NULL;
	if (ce <= cs)
		return 1;

	const uint8_t *data = *(uint8_t **)(elem + 0x34);
	const uint32_t frame4 = (uint32_t)vcvt_s32(*(float *)(elem + 0x30)) << 2;
	const uint32_t fc = *(uint32_t *)(ts + 0x30);
	const uint8_t *anim = *(uint8_t **)(ts + 0x48);
	uv_slot *slot = uv_slot_for(p, tscols);
	uint8_t *const valid = slot->valid;
	const uint32_t zb = fbits(depth);

	static uint32_t xs[2 * MAX_COLS]; // the column's left and right x (float bits)
	const int ncols = ce - cs;
	for (int i = 0; i < ncols; i++) {
		const float x0 = xoff + (float)(cs + i) * tw;
		xs[2 * i] = fbits(x0);
		xs[2 * i + 1] = fbits(x0 + tw);
	}
	// (depth, colour) of the corners: TL c0, TR c1, BR c2, BL c3
	const uint32x2_t zc0 = vcreate_u32((uint64_t)zb | (uint64_t)c0 << 32), zc1 = vcreate_u32((uint64_t)zb | (uint64_t)c1 << 32);
	const uint32x2_t zc2 = vcreate_u32((uint64_t)zb | (uint64_t)c2 << 32), zc3 = vcreate_u32((uint64_t)zb | (uint64_t)c3 << 32);
	unsigned reserved = 0;
	for (int32_t row = rs; row != re; row++) {
		const float y0f = yoff + (float)row * th;
		const uint32_t y0 = fbits(y0f), y1 = fbits(y0f + th);
		const uint32_t *cells = (const uint32_t *)(data + (uint32_t)ew * ((uint32_t)row * 4u)) + cs;
		const uint32x2_t y0v = vdup_n_u32(y0), y1v = vdup_n_u32(y1);
		for (int i = 0; i < ncols; i++, remaining--) {
			const uint32_t m = cells[i] & mask;
			const uint32_t idx = m & 0x7ffff;
			if (!idx)
				continue;
			const uint32_t t = *(const uint32_t *)(anim + ((idx * fc) << 2) + frame4);
			if (!t)
				continue;
			if (!left_tiles) {
				left_tiles = remaining < maxper ? remaining : maxper;
				reserved += left_tiles;
				o = out_fns->alloc(4, (void *)*(uint32_t *)tex, 0x18, left_tiles * 6);
			}
			const uint64_t *uv;
			uint64_t direct_uv[4];
			if (t < UV_N) {
				if (!valid[t]) {
					uv_compute(slot->uv[t], t, tscols, p);
					valid[t] = 1;
				}
				uv = slot->uv[t];
			} else {
				uv_compute(direct_uv, t, tscols, p);
				uv = direct_uv;
				n_uv_direct++;
			}
			// Corners: mirror (bit 28) swaps u, flip (bit 29) swaps v, rotate (bit 30) turns the corners
			uint32x2_t t0, t1, t2, t5;
			const uint32_t f = (m >> 28) & 7;
			if (!f) {
				t0 = vld1_u32((const uint32_t *)&uv[0]); t1 = vld1_u32((const uint32_t *)&uv[1]);
				t2 = vld1_u32((const uint32_t *)&uv[2]); t5 = vld1_u32((const uint32_t *)&uv[3]);
			} else {
				const uint8_t *k = corner_order[f];
				t0 = vld1_u32((const uint32_t *)&uv[k[0]]); t1 = vld1_u32((const uint32_t *)&uv[k[1]]);
				t2 = vld1_u32((const uint32_t *)&uv[k[2]]); t5 = vld1_u32((const uint32_t *)&uv[k[3]]);
			}
			const uint32x2_t xv = vld1_u32(&xs[2 * i]);
			const uint32x2x2_t top = vzip_u32(xv, y0v), bot = vzip_u32(xv, y1v); // (x0, y), (x1, y)
			uint32x2_t *q = (uint32x2_t *)o; // vstr d, [r, #imm]: 4-byte alignment is enough
			q[0] = top.val[0];  q[1] = zc0;  q[2] = t0;
			q[3] = top.val[1];  q[4] = zc1;  q[5] = t1;
			q[6] = bot.val[0];  q[7] = zc3;  q[8] = t2;
			q[9] = bot.val[0];  q[10] = zc3; q[11] = t2;
			q[12] = top.val[1]; q[13] = zc1; q[14] = t1;
			q[15] = bot.val[1]; q[16] = zc2; q[17] = t5;
			o += 36;
			left_tiles--;
		}
	}
	if (left_tiles > 0)
		out_fns->free(left_tiles * 6);
	n_tiles += reserved - (left_tiles > 0 ? left_tiles : 0);
	return 1;
}

/* ---- exp_tilenativeverify ---- */
#define CAP_CHUNKS 16
#define CAP_BYTES (1024 * 1024)
typedef struct {
	int n, freed, overflow;
	int prim[CAP_CHUNKS], stride[CAP_CHUNKS], count[CAP_CHUNKS];
	uint32_t tex[CAP_CHUNKS];
	uint32_t off[CAP_CHUNKS];
	uint8_t *ptr[CAP_CHUNKS]; // original pass: where the runner's chunk is
	uint32_t used;
	uint8_t *buf;
} capture;
static capture cap_native, cap_orig;
static int capturing; // the runner's function runs with AllocVerts captured into cap_orig
static int cur_after_alloc;
static alloc_fn orig_alloc_hooked;

static void *ABI scratch_alloc(int prim, void *tex, int stride, int count) {
	capture *c = &cap_native;
	uint32_t bytes = (uint32_t)stride * (uint32_t)count;
	if (c->n >= CAP_CHUNKS || c->used + bytes > CAP_BYTES) {
		// keep writing somewhere harmless (one reservation is at most 0x4000 vertices, 384 KB); not compared
		c->overflow = 1;
		c->n = CAP_CHUNKS - 1;
		c->used = 0;
	}
	int i = c->n++;
	c->prim[i] = prim;
	c->tex[i] = (uint32_t)tex;
	c->stride[i] = stride;
	c->count[i] = count;
	c->off[i] = c->used;
	void *r = c->buf + c->used;
	c->used += bytes;
	return r;
}

static void ABI scratch_free(int count) {
	cap_native.freed += count;
}

static void cap_close_last(int count) {
	capture *c = &cap_orig;
	if (!c->n || c->overflow)
		return;
	int i = c->n - 1;
	uint32_t bytes = (uint32_t)c->stride[i] * (uint32_t)count;
	if (c->used + bytes > CAP_BYTES) {
		c->overflow = 1;
		return;
	}
	c->off[i] = c->used;
	memcpy(c->buf + c->used, c->ptr[i], bytes);
	c->used += bytes;
}

static void *ABI alloc_capture(int prim, void *tex, int stride, int count) {
	if (!capturing)
		return orig_alloc_hooked(prim, tex, stride, count);
	capture *c = &cap_orig;
	if (c->n)
		cap_close_last(c->count[c->n - 1]); // a flush in this call may reuse the previous chunk's memory
	void *r = orig_alloc_hooked(prim, tex, stride, count);
	if (c->n >= CAP_CHUNKS) {
		c->overflow = 1;
		return r;
	}
	int i = c->n++;
	c->prim[i] = prim;
	c->tex[i] = (uint32_t)tex;
	c->stride[i] = stride;
	c->count[i] = count;
	c->ptr[i] = r;
	cur_after_alloc = *vertex_count;
	return r;
}

static void verify_draw(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
	float fx, fy, fd;
	memcpy(&fx, &d, 4);
	memcpy(&fy, &e, 4);
	memcpy(&fd, &f, 4);
	cap_native.n = cap_native.freed = cap_native.overflow = 0;
	cap_native.used = 0;
	static const sink scratch = {scratch_alloc, scratch_free};
	int native_ok = draw_native((const float *)a, (uint8_t *)b, (uint8_t *)c, fx, fy, fd, &scratch);
	if (native_ok && fault && cap_native.n && cap_native.count[0] > 0) {
		uint32_t *x = (uint32_t *)(cap_native.buf + cap_native.off[0]);
		x[0] ^= 1;
	}
	cap_orig.n = cap_orig.freed = cap_orig.overflow = 0;
	cap_orig.used = 0;
	capturing = 1;
	orig_draw(a, b, c, d, e, f);
	capturing = 0;
	if (cap_orig.n && !cap_orig.overflow) {
		int freed = cur_after_alloc - *vertex_count;
		cap_orig.freed = freed;
		int last = cap_orig.count[cap_orig.n - 1] - freed;
		cap_close_last(last > 0 ? last : 0);
	}
	if (!native_ok || cap_native.overflow || cap_orig.overflow) {
		v_skipped++;
		return;
	}
	v_calls++;
	const capture *n = &cap_native, *g = &cap_orig;
	int bad = n->n != g->n || n->freed != g->freed;
	for (int i = 0; !bad && i < n->n; i++) {
		if (n->prim[i] != g->prim[i] || n->tex[i] != g->tex[i] || n->stride[i] != g->stride[i] ||
				n->count[i] != g->count[i]) {
			bad = 1;
			break;
		}
		int cnt = i == n->n - 1 ? n->count[i] - n->freed : n->count[i];
		uint32_t bytes = (uint32_t)n->stride[i] * (uint32_t)(cnt > 0 ? cnt : 0);
		const uint8_t *x = n->buf + n->off[i], *y = g->buf + g->off[i];
		if (memcmp(x, y, bytes)) {
			bad = 1;
			for (uint32_t k = 0; k < bytes; k++)
				v_bytes += x[k] != y[k];
		}
	}
	if (bad) {
		if (v_bad++ < 5) {
			debugPrintf("[TILEMAP] verify mismatch: chunks %d/%d freed %d/%d", n->n, g->n, n->freed, g->freed);
			if (n->n && g->n)
				debugPrintf(" first count %d/%d tex %08x/%08x", n->count[0], g->count[0], n->tex[0], g->tex[0]);
			debugPrintf("\n");
		}
	}
}

/* ---- entry ---- */
static void tilemap_draw(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
	n_calls++;
	uint32_t t0 = prof ? sceKernelGetProcessTimeLow() : 0;
	int native = 0;
	if (verify)
		verify_draw(a, b, c, d, e, f);
	else if (!ab_off) {
		float fx, fy, fd;
		memcpy(&fx, &d, 4);
		memcpy(&fy, &e, 4);
		memcpy(&fd, &f, 4);
		native = draw_native((const float *)a, (uint8_t *)b, (uint8_t *)c, fx, fy, fd, &live);
		if (native)
			n_native++;
		else
			orig_draw(a, b, c, d, e, f);
	} else
		orig_draw(a, b, c, d, e, f);
	if (prof) {
		int k = !verify && !ab_off;
		prof_us[k] += sceKernelGetProcessTimeLow() - t0;
		prof_calls[k]++;
	}
}

void tilemap_native_report(int frames) {
	if (!installed || frames <= 0)
		return;
	if (n_calls)
		debugPrintf("[TILEMAP] layer draws %.1f/frame, native %.1f, tiles %.0f/frame | fallback direct %u cols %u wide %u | uv direct %u%s\n",
			(float)n_calls / frames, (float)n_native / frames, (float)n_tiles / frames, n_fallback[0], n_fallback[1],
			n_fallback[2], n_uv_direct, ab_mode ? (ab_off ? " | A/B: runner this window" : " | A/B: native this window") : "");
	if (prof && (prof_calls[0] || prof_calls[1]))
		debugPrintf("[TILEMAP] time %s %.3f ms/frame (%u draws)\n", prof_calls[1] ? "native" : "runner",
			(float)(prof_us[0] + prof_us[1]) / 1000.0f / frames, prof_calls[0] + prof_calls[1]);
	if (verify && (v_calls || v_skipped))
		debugPrintf("[TILEMAP] verify: %u draws compared, %u mismatched (%u bytes), %u skipped%s\n", v_calls, v_bad, v_bytes,
			v_skipped, fault ? " (fault injected: all must mismatch)" : "");
	n_calls = n_native = n_tiles = n_uv_direct = 0;
	memset(n_fallback, 0, sizeof(n_fallback));
	prof_us[0] = prof_us[1] = 0;
	prof_calls[0] = prof_calls[1] = 0;
	if (ab_mode)
		ab_off = !ab_off;
}

static int resolve(void) {
#define R(var, type, name) if (!(var = (type)so_symbol(&yoyoloader_mod, name))) { debugPrintf("tile layer native: no %s\n", name); return 0; }
	R(tileset_exists, int_int_fn, "_Z14Tileset_Existsi");
	R(tileset_data, data_fn, "_Z12Tileset_Datai");
	R(calc_params, params_fn, "_Z22CalcLayerTilemapParamsP8CTilesetPP8RTextureRfS4_S4_S4_S4_S4_S4_S4_Rb");
	R(colour_array, colour_fn, "_ZN8Graphics14GetColourArrayEjf");
	R(max_verts, int_void_fn, "_ZN8Graphics24GetMaxDynamicVertexCountEv");
	R(alloc_verts, alloc_fn, "_ZN8Graphics10AllocVertsE9ePrimTypePvii");
	R(free_verts, free_fn, "_ZN8Graphics9FreeVertsEi");
	R(gr_depth, const float *, "GR_Depth");
	R(tiledata_mask, const uint32_t *, "_ZN13CLayerManager14m_TiledataMaskE");
	R(ignore_cull, const uint8_t *, "_ZN7CSprite13ms_ignoreCullE");
	R(vertex_count, int *, "g_CurrentVertexCount");
#undef R
	live.alloc = alloc_verts;
	live.free = free_verts;
	return 1;
}

static uintptr_t draw_entry(void) {
	return so_symbol(&yoyoloader_mod, "_Z23DrawLayerTilemapElementP9tagYYRECTP6CLayerP20CLayerTilemapElementfff");
}

// The function body after the entry hook's two words, bl words skipped (patch_plt_calls.py retargets them)
static int code_matches(uintptr_t fn) {
	const uint32_t *w = (const uint32_t *)fn;
	uint32_t h = 0x811c9dc5u;
	for (int i = 2; i < 513; i++) {
		if ((w[i] >> 24) == 0xeb)
			continue;
		h ^= w[i];
		h *= 0x01000193u;
	}
	if (h != FNV_EXPECT)
		debugPrintf("tile layer native: code checksum %08x, expected %08x\n", h, FNV_EXPECT);
	return h == FNV_EXPECT;
}

static int prepare(void) {
	if (installed)
		return 0;
	if (exp_flag("tilenative0")) {
		debugPrintf("tile layer native: off (exp_tilenative0)\n");
		return 0;
	}
	uintptr_t fn = draw_entry();
	if (!fn || !resolve() || !code_matches(fn)) {
		debugPrintf("tile layer native: NOT installed\n");
		return 0;
	}
	uint64_t (*mem)[4] = malloc(sizeof(*mem) * UV_N * UV_SLOTS); // 256 KB
	if (!mem) {
		debugPrintf("tile layer native: NOT installed (no memory)\n");
		return 0;
	}
	for (int i = 0; i < UV_SLOTS; i++)
		slots[i].uv = mem + i * UV_N;
	verify = debugMode && exp_flag("tilenativeverify");
	if (verify) {
		fault = exp_flag("tilenativeverifyfault");
		cap_native.buf = malloc(CAP_BYTES);
		cap_orig.buf = malloc(CAP_BYTES);
		uintptr_t av = so_symbol(&yoyoloader_mod, "_ZN8Graphics10AllocVertsE9ePrimTypePvii");
		if (!cap_native.buf || !cap_orig.buf || !(orig_alloc_hooked = entry_hook(av, alloc_capture))) {
			debugPrintf("tile layer native: verify NOT available (memory or AllocVerts hook)\n");
			verify = 0;
		}
	}
	ab_mode = debugMode && exp_flag("tilenativeab");
	prof = debugMode && exp_flag("tilenativeprof");
	return 1;
}

static void announce(const char *how) {
	installed = 1;
	debugPrintf("tile layer native: installed (%s)%s%s%s%s\n", how, verify ? ", verify" : "", fault ? " with fault" : "",
		ab_mode ? ", A/B by report window" : "", prof ? ", timed" : "");
}

// pause_bg.c owns the entry hook: it calls what this returns in place of the runner's function (orig)
void *tilemap_native_wrap(void *orig) {
	if (!orig || !prepare())
		return orig;
	orig_draw = (draw_fn)orig;
	announce("behind the paused-tile skip");
	return (void *)tilemap_draw;
}

// patch_katana, after pause_bg_install: its own entry hook when pause_bg didn't take the function
void tilemap_native_install(void) {
	if (installed || exp_flag("tilenative0"))
		return;
	if (!prepare())
		return;
	if (!(orig_draw = (draw_fn)entry_hook(draw_entry(), tilemap_draw))) {
		debugPrintf("tile layer native: NOT installed (entry hook)\n");
		return;
	}
	announce("own entry hook");
}
