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
 * (the four corner (u, v) pairs, filled with the runner's operations on first use of a tile index; the tables grow to
 * the highest index used, about 250 KB in the heavy rooms), the column positions are computed once per draw, not once
 * per tile, and each vertex is written as three 64-bit stores.
 * Nothing is kept across frames except those UV entries, which depend only on the tile index and the parameters they
 * are keyed on: tile edits, animation frames, layer moves and camera moves need no invalidation.
 *
 * Measured on the Vita (500 MHz, in-run A/B, ~1980 tiles/frame in 4-6 layers): the draw takes 1.05-1.33 ms/frame
 * with the runner's code, 0.81-1.09 with this one (-0.12 to -0.27 ms by room). What remains is the batch itself:
 * AllocVerts flushing the previous layer's quads (~0.26 ms) and writing 144 bytes per tile into the runner's vertex
 * batch (~0.8 ms for ~290 KB): building the tiles in a small cached buffer and copying them out with sceClibMemcpy was
 * slower (back to the runner's time).
 * Why not a vertex cache (build a static layer's quads once, copy them each frame): the same bytes still have to go
 * into the batch every frame, which is the part that remains, and the view-culled window changes with every camera move
 * (an earlier window-keyed cache, work/codex/tilemap_cache.c, got 0 hits).
 *
 * Admission (anything else runs the runner's function): the installed function's code matches the analysed one
 * (checksum of every non-bl word after the entry hook), CalcLayerTilemapParams takes its direct-texture path (all
 * tilesets of the game do), the tileset has at least one column, the window is at most 1024 columns wide.
 *
 * Four vertices per tile (exp_tile4v0 off). The six vertices of a tile are TL, TR, BL, BL, TR, BR: the fourth and fifth
 * repeat the third and second bit for bit (the runner writes them from the same registers). The native draw writes
 * only TL, TR, BL, BR, and the batch is drawn with glDrawRangeElements through a static index buffer of 0, 1, 2, 2,
 * 1, 3 per tile (vitaGL reads it in place and copies 4 vertices per tile instead of 6). The GPU gets the same
 * triangles with the same vertex values in the same order, so the pixels are the same; a third less is stored into
 * the batch and copied by vitaGL. To keep such a batch free of other quads, the runner's batch is flushed before the
 * layer's first tile when it isn't empty, and the layer's batch is flushed when the layer is done (Graphics::Flush:
 * at most two more draw calls, only where a sprite on the same texture page would have shared the batch). Splitting a
 * batch changes nothing drawn: Graphics::Flush draws with the GL state applied when the batch began and only then
 * applies pending render states (RenderStateManager::Flush, Shader_Commit); a pending render state already makes
 * AllocVerts flush at that same point, and uniform, matrix, shader, viewport and target setters flush first. Every
 * glDrawArrays the runner makes while such a batch is open (Graphics::Flush, AllocVerts filling up mid-layer) goes
 * through the index buffer; that happens in this file's glDrawArrays wrapper (dlsym, main.c), which otherwise calls
 * vitaGL's glDrawArrays directly. The index buffer is checked after creation (mapped and compared); without it the
 * six-vertex path runs. Measured on the Vita (Nov 26, 500 MHz, in-run A/B of the whole game-thread frame): bunker2_3
 * -0.66 +- 0.08 ms/frame, club_4_2 -0.57 +- 0.18, haunted_alien2 -0.50 +- 0.10.
 * exp_tile4ab (debugMode): six vertices per tile in every other report window (in-run A/B; the [TILEMAP] line names
 * the state of the window it reports). exp_tile4verify (with exp_tilenativeverify): the four-vertex pass goes into
 * scratch memory and is compared with the runner's six vertices at positions 0, 1, 2, 5, and the runner's own vertices
 * 3 and 4 are checked against 2 and 1.
 *
 * Switches: exp_tilenative0 off. exp_tilenativeab: the runner's code in every other report window (in-run A/B).
 * exp_tilenativeprof: time spent in tile layer draws per frame, both paths (sceKernelGetProcessTimeLow per call).
 * exp_tilenativeverify (debugMode): every draw runs the native path into scratch memory first (nothing drawn), then
 * the runner's function for real; its vertices are read back from the runner's batch and compared (texture, primitive,
 * vertex size, count, every byte); mismatches are counted in the report.
 * exp_tilenativeverifyfault: the native x of the first vertex one float step off (negative control: every verified
 * draw with a tile mismatches by one byte; draws with no tile in view have nothing to fault).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <arm_neon.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

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
typedef void ABI (*void_fn)(void);
static void_fn graphics_flush;
// four vertices per tile: on (exp_tile4v0 off), the index buffer is ready, a batch of 4-vertex tiles is open
static int quad4_on, quad4_ready, quad4_failed, quad4_batch, quad4_verify, quad4_ab;
static GLuint quad4_ibo, runner_ibo;
static uint32_t quad4_tex; // the open 4-vertex batch's texture
static void **last_texture; // the runner's batch: texture, primitive type, vertex size
static int *last_prim, *last_vsize;
static int quad4_maxv;
static void (*orig_gl_draw_arrays)(GLenum, GLint, GLsizei);
static void (*orig_gl_bind_buffer)(GLenum, GLuint);
static unsigned n_q4_layers, n_q4_draws, n_q4_bad, n_q4_preflush;
static unsigned v4_calls, v4_bad, v4_dup_bad;
static int ab_off; // exp_tilenativeab: this report window runs the runner's code

static unsigned n_calls, n_native, n_tiles, n_uv_direct, n_fallback[4];
static uint64_t prof_us[2], prof_alloc_us; // exp_tilenativeprof: draw time by path; the native path's AllocVerts
static unsigned prof_calls[2];
static unsigned v_calls, v_bad, v_bytes, v_skipped, v_faulted;

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
#define UV_MAX 4096 // tile indices from here on are computed per tile
typedef struct {
	uint32_t key[7]; // u offset, v offset, u size, v size, u step, v step (float bits), tileset columns
	int used;
	uint32_t cap;    // entries allocated (a power of two, grown on demand)
	uint8_t *valid;
	uint64_t (*uv)[4];
} uv_slot;
static uv_slot slots[UV_SLOTS];
static int next_slot;
static unsigned uv_max_t, uv_bytes;

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
	if (s->cap)
		memset(s->valid, 0, s->cap);
	s->used = 1;
	return s;
}

// A tile index not in the table yet: grow the table if needed, fill the entry; past UV_MAX (or without memory) the
// corners go to scratch
static __attribute__((noinline)) const uint64_t *uv_miss(uv_slot *s, uint32_t t, uint32_t cols, const float *p,
		uint64_t *scratch) {
	if (t > uv_max_t)
		uv_max_t = t;
	if (t >= s->cap && t < UV_MAX) {
		uint32_t cap = s->cap ? s->cap : 256;
		while (cap <= t)
			cap <<= 1;
		uint8_t *mem = malloc((size_t)cap * (sizeof(*s->uv) + 1));
		if (mem) {
			uint64_t (*uv)[4] = (uint64_t (*)[4])mem;
			uint8_t *valid = mem + (size_t)cap * sizeof(*s->uv);
			memset(valid, 0, cap);
			if (s->cap) {
				memcpy(uv, s->uv, (size_t)s->cap * sizeof(*s->uv));
				memcpy(valid, s->valid, s->cap);
				free(s->uv);
				uv_bytes -= s->cap * (sizeof(*s->uv) + 1);
			}
			uv_bytes += cap * (sizeof(*s->uv) + 1);
			s->uv = uv;
			s->valid = valid;
			s->cap = cap;
		}
	}
	if (t < s->cap) {
		uv_compute(s->uv[t], t, cols, p);
		s->valid[t] = 1;
		return s->uv[t];
	}
	n_uv_direct++;
	uv_compute(scratch, t, cols, p);
	return scratch;
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
		const sink *out_fns, int q4) {
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
	const int vpt = q4 ? 4 : 6; // vertices per tile
	const int maxper = maxv / vpt;
	int32_t remaining = (int32_t)((uint32_t)(ce - cs) * (uint32_t)(re - rs));
	int32_t left_tiles = 0;
	uint32_t *o = NULL;
	if (ce <= cs)
		return 1;

	const uint8_t *data = *(uint8_t **)(elem + 0x34);
	const uint32_t frame4 = (uint32_t)vcvt_s32(*(float *)(elem + 0x30)) << 2;
	const uint32_t fc = *(uint32_t *)(ts + 0x30);
	const uint8_t *anim = *(uint8_t **)(ts + 0x48);
	uv_slot *const slot = uv_slot_for(p, tscols);
	uint32_t cap = slot->cap;
	const uint8_t *valid = slot->valid;
	const uint64_t (*uvtab)[4] = (const uint64_t (*)[4])slot->uv;
	uint64_t direct_uv[4];
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
				if (q4 && !reserved && out_fns == &live) { // the batch must hold only this layer's 4-vertex tiles
					if (*vertex_count > 0) {
						graphics_flush();
						n_q4_preflush++;
					}
					quad4_batch = 1;
					quad4_tex = *(uint32_t *)tex;
				}
				reserved += left_tiles;
				const uint32_t ta = prof ? sceKernelGetProcessTimeLow() : 0;
				o = out_fns->alloc(4, (void *)*(uint32_t *)tex, 0x18, left_tiles * vpt);
				if (prof)
					prof_alloc_us += sceKernelGetProcessTimeLow() - ta;
			}
			const uint64_t *uv;
			if (t < cap && valid[t])
				uv = uvtab[t];
			else {
				uv = uv_miss(slot, t, tscols, p, direct_uv);
				cap = slot->cap;
				valid = slot->valid;
				uvtab = slot->uv;
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
			if (q4) { // TL, TR, BL, BR: the six-vertex order without its repeats (indices 0, 1, 2, 2, 1, 3)
				q[0] = top.val[0];  q[1] = zc0;  q[2] = t0;
				q[3] = top.val[1];  q[4] = zc1;  q[5] = t1;
				q[6] = bot.val[0];  q[7] = zc3;  q[8] = t2;
				q[9] = bot.val[1];  q[10] = zc2; q[11] = t5;
				o += 24;
			} else {
				q[0] = top.val[0];  q[1] = zc0;  q[2] = t0;
				q[3] = top.val[1];  q[4] = zc1;  q[5] = t1;
				q[6] = bot.val[0];  q[7] = zc3;  q[8] = t2;
				q[9] = bot.val[0];  q[10] = zc3; q[11] = t2;
				q[12] = top.val[1]; q[13] = zc1; q[14] = t1;
				q[15] = bot.val[1]; q[16] = zc2; q[17] = t5;
				o += 36;
			}
			left_tiles--;
		}
	}
	if (left_tiles > 0)
		out_fns->free(left_tiles * vpt);
	n_tiles += reserved - (left_tiles > 0 ? left_tiles : 0);
	if (quad4_batch) { // draw the layer's 4-vertex batch now, through the index buffer
		graphics_flush();
		quad4_batch = 0;
		n_q4_layers++;
	}
	return 1;
}

/* ---- four vertices per tile: the index buffer and the draw ---- */
// The runner's glDrawArrays (resolved through dlsym): a batch of 4-vertex tiles is drawn through the index buffer
static void q4_gl_draw_arrays(GLenum mode, GLint first, GLsizei count) {
	// Only the batch of 4-vertex tiles: its texture, triangle list, 24-byte vertices (anything else the runner draws
	// meanwhile, say a quad another hook drew inside AllocVerts, flushed the tiles first and has its own texture)
	if (!quad4_batch || count <= 0 || (uint32_t)*last_texture != quad4_tex || *last_prim != 4 || *last_vsize != 0x18) {
		orig_gl_draw_arrays(mode, first, count);
		return;
	}
	if (mode != GL_TRIANGLES || first != 0 || (count & 3) || count > quad4_maxv) {
		n_q4_bad++; // can't happen: the batch holds whole 4-vertex tiles (reported; drawn as they are, never past the batch)
		orig_gl_draw_arrays(mode, first, count);
		return;
	}
	n_q4_draws++;
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad4_ibo);
	glDrawRangeElements(GL_TRIANGLES, 0, (GLuint)count - 1, count / 4 * 6, GL_UNSIGNED_SHORT, NULL);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, runner_ibo);
}

// The runner's glBindBuffer: its element buffer binding, put back after each indexed tile draw
static void q4_gl_bind_buffer(GLenum target, GLuint buffer) {
	if (target == GL_ELEMENT_ARRAY_BUFFER)
		runner_ibo = buffer;
	orig_gl_bind_buffer(target, buffer);
}

void *tilemap_native_glwrap(const char *symbol, void *func) {
	if (!func || exp_flag("tile4v0") || exp_flag("tilenative0"))
		return func;
	if (!strcmp(symbol, "glDrawArrays")) {
		orig_gl_draw_arrays = func;
		return (void *)q4_gl_draw_arrays;
	}
	if (!strcmp(symbol, "glBindBuffer")) {
		orig_gl_bind_buffer = func;
		return (void *)q4_gl_bind_buffer;
	}
	return func;
}

// First tile draw (game thread, GL context): the static index buffer for the largest batch AllocVerts allows
static int quad4_prepare(void) {
	if (quad4_ready || quad4_failed)
		return quad4_ready;
	quad4_failed = 1;
	if (!orig_gl_draw_arrays || !orig_gl_bind_buffer || !graphics_flush) {
		debugPrintf("tile layer native: 4 vertices per tile NOT used (glDrawArrays/glBindBuffer not wrapped)\n");
		return 0;
	}
	int maxv = max_verts() / 4 * 4;
	if (maxv <= 0 || maxv > 0x10000) {
		debugPrintf("tile layer native: 4 vertices per tile NOT used (max batch %d vertices)\n", maxv);
		return 0;
	}
	const int n = maxv / 4 * 6;
	uint16_t *idx = malloc(n * sizeof(uint16_t));
	if (!idx)
		return 0;
	for (int t = 0; t < maxv / 4; t++) {
		uint16_t b = (uint16_t)(t * 4);
		uint16_t *d = idx + t * 6;
		d[0] = b; d[1] = b + 1; d[2] = b + 2; d[3] = b + 2; d[4] = b + 1; d[5] = b + 3;
	}
	glGenBuffers(1, &quad4_ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad4_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * sizeof(uint16_t), idx, GL_STATIC_DRAW);
	// vitaGL is built without error reporting: map the buffer back to see it was allocated and filled
	const void *m = quad4_ibo ? glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_READ_ONLY) : NULL;
	int ok = m && !memcmp(m, idx, n * sizeof(uint16_t));
	if (m)
		glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, runner_ibo);
	free(idx);
	if (!ok) {
		debugPrintf("tile layer native: 4 vertices per tile NOT used (index buffer %s)\n", m ? "content differs" : "not allocated");
		return 0;
	}
	quad4_maxv = maxv;
	quad4_failed = 0;
	quad4_ready = 1;
	debugPrintf("tile layer native: 4 vertices per tile, index buffer of %d indices (batches up to %d vertices)\n", n, maxv);
	return 1;
}

/* ---- exp_tilenativeverify ----
 * The native pass writes into scratch memory (nothing drawn), then the runner's function draws for real. Its vertices
 * are read back from the runner's batch (SrcVerts): a tile layer draw that reserves once ends its quads at
 * g_CurrentVertexCount (a triangle-list AllocVerts appends, or flushes and starts at 0; FreeVerts drops the unused
 * tail). Compared: the texture, primitive and vertex size AllocVerts recorded, the vertex count added, and every byte.
 * No hook on Graphics::AllocVerts (virtual_capture.c owns its entry). Draws with more than one reservation (over
 * 2730 tiles) are skipped. */
#define SCRATCH_BYTES (0x4000 * 24)
static uint8_t *scratch_buf;
static int sc_n, sc_freed, sc_count;
static uint32_t sc_tex, sc_prim, sc_stride;
static uint8_t **src_verts;

static void *ABI scratch_alloc(int prim, void *tex, int stride, int count) {
	sc_n++;
	sc_prim = prim;
	sc_tex = (uint32_t)tex;
	sc_stride = stride;
	sc_count = count;
	return (uint32_t)stride * (uint32_t)count <= SCRATCH_BYTES ? scratch_buf : scratch_buf + SCRATCH_BYTES; // the
	// second half is a sink for an oversized reservation (not compared)
}

static void ABI scratch_free(int count) {
	sc_freed += count;
}

static void verify_draw(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
	float fx, fy, fd;
	memcpy(&fx, &d, 4);
	memcpy(&fy, &e, 4);
	memcpy(&fd, &f, 4);
	sc_n = sc_freed = sc_count = 0;
	static const sink scratch = {scratch_alloc, scratch_free};
	const int native_ok = draw_native((const float *)a, (uint8_t *)b, (uint8_t *)c, fx, fy, fd, &scratch, quad4_verify);
	const int used = sc_count - sc_freed;
	if (native_ok && fault && sc_n == 1 && used > 0) {
		((uint32_t *)scratch_buf)[0] ^= 1;
		v_faulted++;
	}
	const int before = *vertex_count;
	orig_draw(a, b, c, d, e, f);
	const int after = *vertex_count;
	if (!native_ok || sc_n > 1 || (uint32_t)sc_stride * (uint32_t)sc_count > SCRATCH_BYTES) {
		v_skipped++;
		return;
	}
	v_calls++;
	int bad;
	if (!sc_n)
		bad = after != before;
	else {
		bad = (uint32_t)*last_texture != sc_tex || (uint32_t)*last_prim != sc_prim || (uint32_t)*last_vsize != sc_stride;
		if (!bad && quad4_verify) { // 4 native vertices per tile against the runner's 6: 0, 1, 2, 5 equal; its 3 = 2, 4 = 1
			const int tiles = used / 4, rused = tiles * 6;
			bad = used < 0 || (used & 3) || after < rused || (after - before != rused && after != rused);
			if (!bad) {
				const uint8_t *x = scratch_buf, *y = *src_verts + (uint32_t)(after - rused) * sc_stride;
				for (int t = 0; t < tiles; t++, x += 4 * 24, y += 6 * 24) {
					if (memcmp(x, y, 3 * 24) || memcmp(x + 3 * 24, y + 5 * 24, 24)) {
						bad = 1;
						for (int k = 0; k < 3 * 24; k++)
							v_bytes += x[k] != y[k];
						for (int k = 0; k < 24; k++)
							v_bytes += x[3 * 24 + k] != y[5 * 24 + k];
					}
					if (memcmp(y + 3 * 24, y + 2 * 24, 24) || memcmp(y + 4 * 24, y + 24, 24))
						v4_dup_bad++;
				}
			}
			v4_calls++;
			if (bad)
				v4_bad++;
		} else if (!bad)
			bad = used < 0 || after < used || (after - before != used && after != used);
		if (!bad && !quad4_verify) {
			const uint8_t *x = scratch_buf, *y = *src_verts + (uint32_t)(after - used) * sc_stride;
			const uint32_t bytes = (uint32_t)used * sc_stride;
			if (memcmp(x, y, bytes)) {
				bad = 1;
				for (uint32_t k = 0; k < bytes; k++)
					v_bytes += x[k] != y[k];
			}
		}
	}
	if (bad && v_bad++ < 5)
		debugPrintf("[TILEMAP] verify mismatch: reservations %d, vertices %d (native) / %d (runner, %d -> %d), tex %08x/%08x prim %u/%d size %u/%d\n",
			sc_n, used, after - before, before, after, sc_tex, (uint32_t)*last_texture, sc_prim, *last_prim, sc_stride,
			*last_vsize);
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
		native = draw_native((const float *)a, (uint8_t *)b, (uint8_t *)c, fx, fy, fd, &live, quad4_on && quad4_prepare());
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
		debugPrintf("[TILEMAP] layer draws %.1f/frame, native %.1f, tiles %.0f/frame | fallback direct %u cols %u wide %u | uv direct %u, table %u KB, top tile %u%s\n",
			(float)n_calls / frames, (float)n_native / frames, (float)n_tiles / frames, n_fallback[0], n_fallback[1],
			n_fallback[2], n_uv_direct, uv_bytes >> 10, uv_max_t, ab_mode ? (ab_off ? " | A/B: runner this window" : " | A/B: native this window") : "");
	if (prof && (prof_calls[0] || prof_calls[1]))
		debugPrintf("[TILEMAP] time %s %.3f ms/frame (%u draws), of it AllocVerts %.3f\n", prof_calls[1] ? "native" : "runner",
			(float)(prof_us[0] + prof_us[1]) / 1000.0f / frames, prof_calls[0] + prof_calls[1],
			(float)prof_alloc_us / 1000.0f / frames);
	if (quad4_ready)
		debugPrintf("[TILEMAP] %s vertices per tile: %.1f layers/frame, %.1f indexed draws/frame, %.2f pre-flushes/frame, wrong draws %u\n",
			quad4_on ? "4" : "6 (A/B)", (float)n_q4_layers / frames, (float)n_q4_draws / frames, (float)n_q4_preflush / frames, n_q4_bad);
	n_q4_layers = n_q4_draws = n_q4_preflush = 0;
	if (verify && quad4_verify && v4_calls)
		debugPrintf("[TILEMAP] verify 4 vertices per tile: %u draws, %u mismatched, runner repeats differing %u\n", v4_calls,
			v4_bad, v4_dup_bad);
	if (verify && (v_calls || v_skipped))
		debugPrintf("[TILEMAP] verify: %u draws compared, %u mismatched (%u bytes), %u skipped | fault injected into %u draws (exp_tilenativeverifyfault: each must mismatch by 1 byte)\n",
			v_calls, v_bad, v_bytes, v_skipped, v_faulted);
	n_calls = n_native = n_tiles = n_uv_direct = 0;
	memset(n_fallback, 0, sizeof(n_fallback));
	prof_us[0] = prof_us[1] = prof_alloc_us = 0;
	prof_calls[0] = prof_calls[1] = 0;
	if (ab_mode)
		ab_off = !ab_off;
	if (quad4_ab) // exp_tile4ab: 6 vertices per tile in every other report window
		quad4_on = !quad4_on;
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
	R(graphics_flush, void_fn, "_ZN8Graphics5FlushEv");
	R(last_texture, void **, "g_pLastTexture");
	R(last_prim, int *, "g_LastPrimType");
	R(last_vsize, int *, "g_LastVertexSize");
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
	verify = debugMode && exp_flag("tilenativeverify");
	if (verify) {
		fault = exp_flag("tilenativeverifyfault");
		scratch_buf = malloc(2 * SCRATCH_BYTES);
		src_verts = (uint8_t **)so_symbol(&yoyoloader_mod, "SrcVerts");
		if (!scratch_buf || !src_verts) {
			debugPrintf("tile layer native: verify NOT available (memory or symbols)\n");
			verify = 0;
		}
	}
	quad4_on = !exp_flag("tile4v0");
	quad4_verify = verify && exp_flag("tile4verify");
	quad4_ab = debugMode && quad4_on && exp_flag("tile4ab");
	ab_mode = debugMode && exp_flag("tilenativeab");
	prof = debugMode && exp_flag("tilenativeprof");
	return 1;
}

static void announce(const char *how) {
	installed = 1;
	debugPrintf("tile layer native: installed (%s)%s%s%s%s%s\n", how, quad4_on ? ", 4 vertices per tile" : ", 6 vertices per tile (exp_tile4v0)",
		verify ? (quad4_verify ? ", verify (4 vertices)" : ", verify") : "", fault ? " with fault" : "",
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
