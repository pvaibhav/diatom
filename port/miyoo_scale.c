/* SPDX-License-Identifier: MIT */
/* See miyoo_scale.h.
 *
 * A frame is drawn a row at a time into `out`, a cached buffer, and copied to
 * every fb row that shows it: fb memory is only ever written, never read, and
 * a row that repeats (most of them, scaling up) is one memcpy.
 *
 * Bilinear is separable. Each source row the output needs is scaled across
 * once, into `hrow` (15 bits per channel), and output rows blend two of those.
 * A row whose weight is 0 is one of them narrowed to 8 bits. Crisp is the
 * same code with different maps, and most of its weights are 0.
 *
 * Every loop has a plain C form, and the NEON form computes the same bits.
 * test/flipscale_test.c compares the two. */
#include "miyoo_scale.h"

#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define SCALER_NEON 1
#endif

/* ---------- maps, once per geometry ----------------------------------------- */

static int64_t floor_div(int64_t a, int64_t b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* Output pixel i of d, on an axis of s source pixels: the source pixel it
 * takes, and the weight (0..255, of 256) of the one after it.
 *
 * Bilinear samples output pixel i's centre, (i + 0.5) * u / d - 0.5 in an
 * image u pixels wide, from its two neighbours. Soft samples the source
 * itself (u = s). Crisp samples the source scaled up by k = ceil(d / s) with
 * nearest, so u = s * k: two neighbours there are one source pixel unless
 * they straddle a source pixel's edge, and only then is there a weight.
 * Integers throughout: the sample point is ((2i + 1) * u - d) / 2d. */
static void tap(int i, int s, int d, diatom_filter f, int *idx, int *weight)
{
	int64_t k = 1, u, num, q, rem, a, b, w;

	if (f == DIATOM_FILTER_NEAREST) {
		*idx = (int)((2 * (int64_t)i + 1) * s / (2 * (int64_t)d));
		*weight = 0;
		return;
	}
	if (f == DIATOM_FILTER_SHARP && d > s) k = (d + s - 1) / s;
	u   = s * k;
	num = (2 * (int64_t)i + 1) * u - d;
	q   = floor_div(num, 2 * (int64_t)d);
	rem = num - q * 2 * d;
	a   = clamp64(q, 0, u - 1) / k;     /* past an edge, the edge pixel */
	b   = clamp64(q + 1, 0, u - 1) / k;
	w   = a == b ? 0 : (rem * 256 + d) / (2 * (int64_t)d);
	if (w == 256) { a = b; w = 0; }
	*idx = (int)a;
	*weight = (int)w;
}

static bool resize(void *pp, size_t bytes)
{
	void **p = pp, *n = realloc(*p, bytes);

	if (!n) return false;
	*p = n;
	return true;
}

bool diatom_scaler_prepare(diatom_scaler *s, int src_w, int src_h, diatom_rect dst,
                           diatom_filter filter, int fb_w, int fb_h)
{
	int x0, x1, y0, y1;

	if (s->valid && s->src_w == src_w && s->src_h == src_h && s->filter == filter &&
	    s->fb_w == fb_w && s->fb_h == fb_h && !memcmp(&s->dst, &dst, sizeof dst))
		return true;
	s->valid = false;
	if (src_w <= 0 || src_h <= 0 || dst.w <= 0 || dst.h <= 0) return false;

	/* The part of dst on the panel: a mode may crop (the port interface). */
	x0 = dst.x < 0 ? -dst.x : 0;
	x1 = dst.x + dst.w > fb_w ? fb_w - dst.x : dst.w;
	y0 = dst.y < 0 ? -dst.y : 0;
	y1 = dst.y + dst.h > fb_h ? fb_h - dst.y : dst.h;
	s->n    = x1 > x0 ? x1 - x0 : 0;
	s->rows = y1 > y0 ? y1 - y0 : 0;

	/* One more than needed of each: the last source pixel's padding, and no
	 * zero-size allocation for an empty rect. */
	{
		size_t n = (size_t)s->n + 1, rows = (size_t)s->rows + 1;

		if (!resize(&s->cidx, n * sizeof *s->cidx) || !resize(&s->cw4, 4 * n) ||
		    !resize(&s->hrow[0], 4 * n * sizeof **s->hrow) ||
		    !resize(&s->hrow[1], 4 * n * sizeof **s->hrow) ||
		    !resize(&s->out, n * sizeof *s->out) ||
		    !resize(&s->ridx, rows * sizeof *s->ridx) || !resize(&s->rw, rows) ||
		    !resize(&s->line, ((size_t)src_w + 1) * sizeof *s->line))
			return false;
	}

	/* Memory column m is logical column x1 - 1 - m: the panel is upside down. */
	s->cblend = false;
	for (int m = 0; m < s->n; m++) {
		int idx, w;

		tap(x1 - 1 - m, src_w, dst.w, filter, &idx, &w);
		s->cidx[m] = (uint16_t)idx;
		memset(s->cw4 + 4 * m, w, 4);
		s->cblend |= w != 0;
	}
	s->rblend = false;
	for (int j = 0; j < s->rows; j++) {
		int idx, w;

		tap(y0 + j, src_h, dst.h, filter, &idx, &w);
		s->ridx[j] = (int16_t)idx;
		s->rw[j] = (uint8_t)w;
		s->rblend |= w != 0;
	}
	s->fb_x     = fb_w - dst.x - x1;
	s->fb_y     = fb_h - 1 - (dst.y + y0);
	s->scan_top = fb_h - dst.y - dst.h;

	s->src_w = src_w; s->src_h = src_h; s->dst = dst; s->filter = filter;
	s->fb_w = fb_w; s->fb_h = fb_h;
	s->valid = true;
	return true;
}

void diatom_scaler_free(diatom_scaler *s)
{
	free(s->cidx); free(s->cw4); free(s->ridx); free(s->rw);
	free(s->line); free(s->hrow[0]); free(s->hrow[1]); free(s->out);
	memset(s, 0, sizeof *s);
}

/* ---------- the row loops --------------------------------------------------- */

/* A source row as xRGB, alpha set, into `o` (w pixels). */
static void convert_c(uint32_t *o, const void *src, int from, int w, diatom_pixfmt fmt)
{
	if (fmt == DIATOM_PIX_RGB565) {
		const uint16_t *in = src;
		for (int x = from; x < w; x++) o[x] = diatom_rgb565(in[x]);
	} else {
		const uint32_t *in = src;
		for (int x = from; x < w; x++) o[x] = in[x] | 0xFF000000u;
	}
}

static void gather_c(uint32_t *o, const uint32_t *ln, const uint16_t *ci, int n)
{
	for (int m = 0; m < n; m++) o[m] = ln[ci[m]];
}

/* Across: per channel a * (256 - w) + b * w, halved with rounding to fit 15
 * bits (0..32640), the form the vertical blend multiplies in. */
static void hscale_c(int16_t *h, const uint32_t *ln, const uint16_t *ci, const uint8_t *cw4,
                     int from, int n)
{
	for (int m = from; m < n; m++) {
		uint32_t a = ln[ci[m]], b = ln[ci[m] + 1];
		unsigned w = cw4[4 * m];

		for (int c = 0; c < 4; c++) {
			unsigned av = a >> 8 * c & 255, bv = b >> 8 * c & 255;
			h[4 * m + c] = (int16_t)((av * (256 - w) + bv * w + 1) >> 1);
		}
	}
}

/* Down: h0 + (h1 - h0) * w / 256, then back to 8 bits. The rounding is
 * VQRDMULH's and VQRSHRUN's, so the NEON form matches bit for bit. */
static inline uint32_t vpixel(const int16_t *h0, const int16_t *h1, int wq)
{
	uint32_t p = 0;

	for (int c = 0; c < 4; c++) {
		int v = h0[c];

		if (wq) v += (2 * (h1[c] - h0[c]) * wq + 32768) >> 16;
		v = (v + 64) >> 7;
		p |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << 8 * c;
	}
	return p;
}

static void vblend_c(uint32_t *o, const int16_t *h0, const int16_t *h1, int w, int from, int n)
{
	for (int m = from; m < n; m++) o[m] = vpixel(h0 + 4 * m, h1 + 4 * m, w << 7);
}

#ifdef SCALER_NEON
/* RGB565: each channel's top bits repeated into its bottom, as diatom_rgb565. */
static void convert_neon(uint32_t *o, const void *src, int w, diatom_pixfmt fmt)
{
	int x = 0;

	if (fmt == DIATOM_PIX_RGB565) {
		const uint16_t *in = src;
		for (; x + 8 <= w; x += 8) {
			uint16x8_t p = vld1q_u16(in + x);
			uint8x8x4_t q;
			uint8x8_t r = vshrn_n_u16(p, 8), g = vshrn_n_u16(p, 3), b = vshl_n_u8(vmovn_u16(p), 3);

			q.val[0] = vsri_n_u8(b, b, 5);
			q.val[1] = vsri_n_u8(g, g, 6);
			q.val[2] = vsri_n_u8(r, r, 5);
			q.val[3] = vdup_n_u8(0xFF);
			vst4_u8((uint8_t *)(o + x), q);
		}
	} else {
		const uint32_t *in = src;
		uint32x4_t alpha = vdupq_n_u32(0xFF000000u);
		for (; x + 4 <= w; x += 4) vst1q_u32(o + x, vorrq_u32(vld1q_u32(in + x), alpha));
	}
	convert_c(o, src, x, w, fmt);
}

static void hscale_neon(int16_t *h, const uint32_t *ln, const uint16_t *ci, const uint8_t *cw4, int n)
{
	int m = 0;

	for (; m + 4 <= n; m += 4) {
		/* Four (a, b) pairs, then a = the four firsts, b = the four seconds. */
		uint32x4_t p01 = vcombine_u32(vld1_u32(ln + ci[m]), vld1_u32(ln + ci[m + 1]));
		uint32x4_t p23 = vcombine_u32(vld1_u32(ln + ci[m + 2]), vld1_u32(ln + ci[m + 3]));
		uint32x4x2_t ab = vuzpq_u32(p01, p23);
		uint8x16_t a = vreinterpretq_u8_u32(ab.val[0]), b = vreinterpretq_u8_u32(ab.val[1]);
		uint8x16_t w = vld1q_u8(cw4 + 4 * m);
		/* a * 256 + b * w - a * w: wraps in between, exact at the end. */
		uint16x8_t lo = vshll_n_u8(vget_low_u8(a), 8), hi = vshll_n_u8(vget_high_u8(a), 8);

		lo = vmlsl_u8(vmlal_u8(lo, vget_low_u8(b), vget_low_u8(w)), vget_low_u8(a), vget_low_u8(w));
		hi = vmlsl_u8(vmlal_u8(hi, vget_high_u8(b), vget_high_u8(w)), vget_high_u8(a), vget_high_u8(w));
		vst1q_s16(h + 4 * m, vreinterpretq_s16_u16(vrshrq_n_u16(lo, 1)));
		vst1q_s16(h + 4 * m + 8, vreinterpretq_s16_u16(vrshrq_n_u16(hi, 1)));
	}
	hscale_c(h, ln, ci, cw4, m, n);
}

static void vblend_neon(uint32_t *o, const int16_t *h0, const int16_t *h1, int w, int n)
{
	int16x8_t wq = vdupq_n_s16((int16_t)(w << 7));
	int m = 0;

	for (; m + 4 <= n; m += 4) {
		int16x8_t a0 = vld1q_s16(h0 + 4 * m), a1 = vld1q_s16(h0 + 4 * m + 8);

		if (w) {
			a0 = vaddq_s16(a0, vqrdmulhq_s16(vsubq_s16(vld1q_s16(h1 + 4 * m), a0), wq));
			a1 = vaddq_s16(a1, vqrdmulhq_s16(vsubq_s16(vld1q_s16(h1 + 4 * m + 8), a1), wq));
		}
		vst1q_u8((uint8_t *)(o + m), vcombine_u8(vqrshrun_n_s16(a0, 7), vqrshrun_n_s16(a1, 7)));
	}
	vblend_c(o, h0, h1, w, m, n);
}
#endif

static void convert(diatom_scaler *s, const void *src, size_t pitch, diatom_pixfmt fmt, int r)
{
	const void *in = (const uint8_t *)src + (size_t)r * pitch;

#ifdef SCALER_NEON
	if (!s->plain) convert_neon(s->line, in, s->src_w, fmt);
	else
#endif
		convert_c(s->line, in, 0, s->src_w, fmt);
	s->line[s->src_w] = s->line[s->src_w - 1];   /* b of the last column: weight 0 */
}

/* Source row r scaled across, in a slot that does not hold row `keep`. */
static const int16_t *hrow(diatom_scaler *s, const void *src, size_t pitch, diatom_pixfmt fmt,
                           int r, int keep)
{
	int k;

	if (s->hsrc[0] == r) return s->hrow[0];
	if (s->hsrc[1] == r) return s->hrow[1];
	k = s->hsrc[0] == keep ? 1 : 0;
	convert(s, src, pitch, fmt, r);
#ifdef SCALER_NEON
	if (!s->plain) hscale_neon(s->hrow[k], s->line, s->cidx, s->cw4, s->n);
	else
#endif
		hscale_c(s->hrow[k], s->line, s->cidx, s->cw4, 0, s->n);
	s->hsrc[k] = r;
	return s->hrow[k];
}

static void make_row(diatom_scaler *s, const void *src, size_t pitch, diatom_pixfmt fmt, int r, int w)
{
	const int16_t *h0, *h1;

	if (!s->cblend && !s->rblend) {
		convert(s, src, pitch, fmt, r);
		gather_c(s->out, s->line, s->cidx, s->n);
		return;
	}
	h0 = hrow(s, src, pitch, fmt, r, w ? r + 1 : -1);
	h1 = w ? hrow(s, src, pitch, fmt, r + 1, r) : h0;
#ifdef SCALER_NEON
	if (!s->plain) vblend_neon(s->out, h0, h1, w, s->n);
	else
#endif
		vblend_c(s->out, h0, h1, w, 0, s->n);
}

void diatom_scaler_draw(diatom_scaler *s, const void *src, size_t pitch, diatom_pixfmt fmt,
                        uint32_t *fb, size_t fb_pitch, bool scan)
{
	uint32_t *row = fb + (size_t)s->fb_y * fb_pitch + (size_t)s->fb_x;
	int last_r = -1, last_w = -1;

	if (!s->valid || !s->n) return;
	s->hsrc[0] = s->hsrc[1] = -1;          /* a new frame: nothing cached is current */
	for (int j = 0; j < s->rows; j++, row -= fb_pitch) {
		int r = s->ridx[j], w = s->rw[j];

		/* The scanline filter's dark rows: every third fb row of the rect. */
		if (scan && (s->fb_y - j - s->scan_top) % 3 == 2) continue;
		if (r != last_r || w != last_w) {
			make_row(s, src, pitch, fmt, r, w);
			last_r = r;
			last_w = w;
		}
		memcpy(row, s->out, (size_t)s->n * 4);
	}
}
