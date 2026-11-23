/*
 * gm_qoi.h
 *
 * Decoder for GameMaker's QOI variant ("fioq" texture pages), written to produce the same pixels as the runner's
 * ReadQOIFFile (libyoyo.so 1.1.10 at 0xe80414): pixels in stream channel order, starting from (0, 0, 0, 255) with a
 * zeroed 64-entry index; only colour and diff ops update the index (runs and index ops don't). Unlike the runner it
 * never writes past the image: a run is cut at the last pixel, and a stream that ends early repeats the last pixel.
 * Shared by the loader (qoi_pages.c) and the host check (scripts/gm_qoi_check.py).
 */
#ifndef GM_QOI_H
#define GM_QOI_H

#include <stdint.h>

// Decodes npx pixels of the op stream [p, end) into out. Returns the number of stream bytes left over (0 for a
// well-formed page), or -1 if the stream ended before the image was full.
static inline int gm_qoi_decode(const uint8_t *p, const uint8_t *end, uint32_t *out, uint32_t npx) {
	uint32_t index[64];
	for (int i = 0; i < 64; i++)
		index[i] = 0;
	uint32_t px = 0xff000000u;
	uint32_t *o = out, *const oend = out + npx;
	while (o < oend) {
		if (p >= end) {
			while (o < oend)
				*o++ = px;
			return -1;
		}
		uint32_t b1 = *p++;
		if (b1 < 0x40) { // INDEX
			px = index[b1];
			*o++ = px;
			continue;
		}
		if (b1 < 0x80) { // RUN_8 (1-32 pixels) / RUN_16 (33-8224 pixels)
			uint32_t n;
			if (b1 & 0x20) {
				if (p >= end) {
					while (o < oend)
						*o++ = px;
					return -1;
				}
				n = (((b1 & 0x1f) << 8) | *p++) + 33;
			} else
				n = (b1 & 0x1f) + 1;
			if (n > (uint32_t)(oend - o))
				n = oend - o;
			uint32_t *stop = o + n;
			while (o < stop)
				*o++ = px;
			continue;
		}
		uint32_t r = px & 0xff, g = (px >> 8) & 0xff, b = (px >> 16) & 0xff, a = px >> 24;
		if (b1 < 0xc0) { // DIFF_8: signed 2-bit r, g, b
			r += ((int32_t)(b1 << 26) >> 30);
			g += ((int32_t)(b1 << 28) >> 30);
			b += ((int32_t)(b1 << 30) >> 30);
		} else if (b1 < 0xe0) { // DIFF_16: signed 5-bit r, 4-bit g, 4-bit b
			if (p >= end)
				goto short_stream;
			uint32_t b2 = *p++;
			r += ((int32_t)(b1 << 27) >> 27);
			g += ((int32_t)(b2 << 24) >> 28);
			b += ((int32_t)(b2 << 28) >> 28);
		} else if (b1 < 0xf0) { // DIFF_24: signed 5-bit r, g, b, a
			if (end - p < 2)
				goto short_stream;
			uint32_t m = (b1 << 16) | (p[0] << 8) | p[1];
			p += 2;
			r += ((int32_t)(m << 12) >> 27);
			g += ((int32_t)(m << 17) >> 27);
			b += ((int32_t)(m << 22) >> 27);
			a += ((int32_t)(m << 27) >> 27);
		} else { // COLOR: the channels whose flag is set follow
			if (end - p < (int)(((b1 >> 3) & 1) + ((b1 >> 2) & 1) + ((b1 >> 1) & 1) + (b1 & 1)))
				goto short_stream;
			if (b1 & 8)
				r = *p++;
			if (b1 & 4)
				g = *p++;
			if (b1 & 2)
				b = *p++;
			if (b1 & 1)
				a = *p++;
		}
		px = (r & 0xff) | ((g & 0xff) << 8) | ((b & 0xff) << 16) | ((a & 0xff) << 24);
		index[(r ^ g ^ b ^ a) & 0x3f] = px;
		*o++ = px;
	}
	return (int)(end - p);
short_stream:
	while (o < oend)
		*o++ = px;
	return -1;
}

#endif
