/* SPDX-License-Identifier: MIT */
/* The Flip's CPU scaling (port/miyoo_scale.c, ADR-0050) against references
 * written here from the definitions, not from the scaler's maps:
 *   nearest samples each output pixel's centre;
 *   Crisp is nearest up by ceil(scale) on each axis, then bilinear down,
 *     within 1 per channel, and nearest exactly at whole factors;
 *   Soft is bilinear, within 1 per channel;
 *   the picture lands turned 180 degrees, clipped, nothing written outside it;
 *   RGB565 converts as the port always has;
 *   the whole-factor upscale repeats each source pixel kx by ky times, keeps
 *     the pixel format bit for bit and writes nothing past a row.
 * Where the host has NEON (arm64) every case runs twice, NEON and plain C,
 * and the two must agree bit for bit. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diatom.h"
#include "miyoo_scale.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define FB_W 752
#define FB_H 560
#define SENTINEL 0x12345678u

/* The port's conversion before the scaler had its own (port/miyoo.c). */
static uint32_t rgb565_ref(uint16_t p)
{
	uint32_t r = p >> 11 & 31, g = p >> 5 & 63, b = p & 31;

	return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static uint32_t rnd(void)
{
	static uint32_t x = 2463534242u;

	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return x;
}

typedef struct {
	int           w, h;
	diatom_pixfmt fmt;
	size_t        pitch;            /* bytes; wider than a row, to catch pitch bugs */
	void         *px;
	uint32_t     *xrgb;             /* the same picture as xRGB with alpha set */
} image;

static image make_image(int w, int h, diatom_pixfmt fmt)
{
	image im = { w, h, fmt, 0, NULL, NULL };
	int bpp = fmt == DIATOM_PIX_RGB565 ? 2 : 4;

	im.pitch = (size_t)(w + 3) * bpp;
	im.px = malloc(im.pitch * h);
	im.xrgb = malloc((size_t)w * h * 4);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t v = rnd();
			/* Runs of one colour beside edges, like a game's picture, and
			 * full-contrast neighbours, where rounding shows. */
			if (x % 5 == 1) v = x % 2 ? 0 : 0xFFFFFFFFu;
			if (bpp == 2) {
				((uint16_t *)((uint8_t *)im.px + y * im.pitch))[x] = (uint16_t)v;
				im.xrgb[y * w + x] = rgb565_ref((uint16_t)v);
			} else {
				((uint32_t *)((uint8_t *)im.px + y * im.pitch))[x] = v;   /* junk in the top byte */
				im.xrgb[y * w + x] = v | 0xFF000000u;
			}
		}
	return im;
}

static void free_image(image *im) { free(im->px); free(im->xrgb); }

/* The reference picture in logical (unturned) coordinates, d.w x d.h. */
static uint32_t *ref_nearest(const image *im, diatom_rect d)
{
	uint32_t *o = malloc((size_t)d.w * d.h * 4);

	for (int y = 0; y < d.h; y++)
		for (int x = 0; x < d.w; x++) {
			int sx = (int)floor((x + 0.5) * im->w / d.w), sy = (int)floor((y + 0.5) * im->h / d.h);
			o[y * d.w + x] = im->xrgb[sy * im->w + sx];
		}
	return o;
}

/* Bilinear from an image u x v (pixels by `at`), edges clamped. */
static uint32_t *bilinear(const uint32_t *in, int u, int v, diatom_rect d)
{
	uint32_t *o = malloc((size_t)d.w * d.h * 4);

	for (int y = 0; y < d.h; y++)
		for (int x = 0; x < d.w; x++) {
			double fx = (x + 0.5) * u / d.w - 0.5, fy = (y + 0.5) * v / d.h - 0.5;
			int x0 = (int)floor(fx), y0 = (int)floor(fy);
			double ax = fx - x0, ay = fy - y0;
			int xa = x0 < 0 ? 0 : x0 > u - 1 ? u - 1 : x0, xb = x0 + 1 > u - 1 ? u - 1 : x0 + 1 < 0 ? 0 : x0 + 1;
			int ya = y0 < 0 ? 0 : y0 > v - 1 ? v - 1 : y0, yb = y0 + 1 > v - 1 ? v - 1 : y0 + 1 < 0 ? 0 : y0 + 1;
			uint32_t p = 0;

			for (int c = 0; c < 32; c += 8) {
				double tl = in[ya * u + xa] >> c & 255, tr = in[ya * u + xb] >> c & 255;
				double bl = in[yb * u + xa] >> c & 255, br = in[yb * u + xb] >> c & 255;
				double t = tl + (tr - tl) * ax, b = bl + (br - bl) * ax;
				p |= (uint32_t)floor(t + (b - t) * ay + 0.5) << c;
			}
			o[y * d.w + x] = p;
		}
	return o;
}

static uint32_t *ref_soft(const image *im, diatom_rect d) { return bilinear(im->xrgb, im->w, im->h, d); }

static uint32_t *ref_crisp(const image *im, diatom_rect d)
{
	int kx = d.w > im->w ? (d.w + im->w - 1) / im->w : 1, ky = d.h > im->h ? (d.h + im->h - 1) / im->h : 1;
	int u = im->w * kx, v = im->h * ky;
	uint32_t *up = malloc((size_t)u * v * 4), *o;

	for (int y = 0; y < v; y++)
		for (int x = 0; x < u; x++) up[y * u + x] = im->xrgb[(y / ky) * im->w + x / kx];
	o = bilinear(up, u, v, d);
	free(up);
	return o;
}

/* A page the size of the panel, everything SENTINEL. */
static uint32_t *page(void)
{
	uint32_t *fb = malloc((size_t)FB_W * FB_H * 4);

	for (int i = 0; i < FB_W * FB_H; i++) fb[i] = SENTINEL;
	return fb;
}

static uint32_t *draw(const image *im, diatom_rect d, diatom_filter f, bool plain)
{
	diatom_scaler s;
	uint32_t *fb = page();

	memset(&s, 0, sizeof s);
	s.plain = plain;
	if (!diatom_scaler_prepare(&s, im->w, im->h, d, f, FB_W, FB_H)) {
		CHECK(0, "prepare failed");
		diatom_scaler_free(&s);
		return fb;
	}
	diatom_scaler_draw(&s, im->px, im->pitch, im->fmt, fb, FB_W);
	diatom_scaler_free(&s);
	return fb;
}

/* fb against a logical reference: inside the visible rect within `tol` per
 * channel, outside it untouched. Returns the largest difference seen. */
static int compare(const uint32_t *fb, const uint32_t *ref, diatom_rect d, int tol, const char *what)
{
	int worst = 0, bad = 0;

	for (int fy = 0; fy < FB_H; fy++)
		for (int fx = 0; fx < FB_W; fx++) {
			int x = FB_W - 1 - fx - d.x, y = FB_H - 1 - fy - d.y;   /* turned back */
			uint32_t got = fb[fy * FB_W + fx];

			if (x < 0 || y < 0 || x >= d.w || y >= d.h) {
				if (got != SENTINEL && !bad++) CHECK(0, "%s: wrote outside the rect at fb %d,%d", what, fx, fy);
				continue;
			}
			for (int c = 0; c < 32; c += 8) {
				int e = abs((int)(got >> c & 255) - (int)(ref[y * d.w + x] >> c & 255));
				if (e > worst) worst = e;
			}
		}
	CHECK(worst <= tol, "%s: off by %d (allowed %d)", what, worst, tol);
	return worst;
}

static const char *const NAME[] = { "nearest", "crisp", "soft" };

static void check_case(int w, int h, diatom_pixfmt fmt, diatom_rect d, const char *label)
{
	image im = make_image(w, h, fmt);
	char what[160];
	int integer = d.w % w == 0 && d.h % h == 0;

	for (int f = 0; f < 3; f++) {
		uint32_t *ref = f == 0 ? ref_nearest(&im, d) : f == 1 ? ref_crisp(&im, d) : ref_soft(&im, d);
		uint32_t *c = draw(&im, d, (diatom_filter)f, true);

		snprintf(what, sizeof what, "%s %dx%d %s -> %dx%d%+d%+d %s", label, w, h,
		         fmt == DIATOM_PIX_RGB565 ? "rgb565" : "xrgb8888", d.w, d.h, d.x, d.y, NAME[f]);
		compare(c, ref, d, f == 0 ? 0 : 1, what);
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		{
			uint32_t *n = draw(&im, d, (diatom_filter)f, false);
			CHECK(!memcmp(n, c, (size_t)FB_W * FB_H * 4), "%s: NEON and C differ", what);
			free(n);
		}
#endif
		if (f == 1 && integer) {
			uint32_t *nearest = draw(&im, d, DIATOM_FILTER_NEAREST, true);
			CHECK(!memcmp(nearest, c, (size_t)FB_W * FB_H * 4), "%s: Crisp is not nearest at a whole factor", what);
			free(nearest);
		}
		free(ref);
		free(c);
	}
	free_image(&im);
}

/* diatom_upscale of a w x h image by kx, ky into a buffer with padded rows,
 * against src[y / ky][x / kx]; the padding must stay untouched. */
static void check_upscale(int w, int h, diatom_pixfmt fmt, int kx, int ky)
{
	image im = make_image(w, h, fmt);
	int bpp = fmt == DIATOM_PIX_RGB565 ? 2 : 4, u = w * kx, v = h * ky;
	size_t row = (size_t)u * bpp, pitch = row + 12;
	uint8_t *out[2], *line = malloc(row);

	for (int plain = 0; plain < 2; plain++) {
		int bad = 0, pad = 0;

		out[plain] = malloc(pitch * v);
		memset(out[plain], 0xA5, pitch * v);
		diatom_upscale(im.px, w, h, im.pitch, fmt, kx, ky, out[plain], pitch, line, plain);
		for (int y = 0; y < v; y++) {
			const uint8_t *o = out[plain] + (size_t)y * pitch;
			const uint8_t *in = (const uint8_t *)im.px + (size_t)(y / ky) * im.pitch;

			for (int x = 0; x < u; x++) bad += memcmp(o + (size_t)x * bpp, in + (size_t)(x / kx) * bpp, bpp) != 0;
			for (size_t k = row; k < pitch; k++) pad += o[k] != 0xA5;
		}
		CHECK(!bad && !pad, "upscale %dx%d %s by %dx%d, %s: %d pixels wrong, %d padding bytes written",
		      w, h, fmt == DIATOM_PIX_RGB565 ? "rgb565" : "xrgb8888", kx, ky, plain ? "C" : "NEON", bad, pad);
	}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	CHECK(!memcmp(out[0], out[1], pitch * v), "upscale %dx%d by %dx%d: NEON and C differ", w, h, kx, ky);
#endif
	free(out[0]);
	free(out[1]);
	free(line);
	free_image(&im);
}

int main(void)
{
	static const struct { int w, h; diatom_pixfmt fmt; double aspect; } SRC[] = {
		{ 160, 144, DIATOM_PIX_RGB565, 10.0 / 9 }, { 240, 160, DIATOM_PIX_RGB565, 1.5 },
		{ 256, 224, DIATOM_PIX_RGB565, 1.3061 }, { 256, 240, DIATOM_PIX_XRGB8888, 4.0 / 3 },
		{ 320, 224, DIATOM_PIX_RGB565, 1.4286 },
	};
	static const struct { diatom_scale_mode m; const char *name; } MODE[] = {
		{ DIATOM_SCALE_ASPECT_FIT, "aspect" }, { DIATOM_SCALE_STRETCH, "stretch" },
		{ DIATOM_SCALE_INTEGER, "integer" },
	};

	printf("flipscale: the panel's modes\n");
	for (size_t i = 0; i < sizeof SRC / sizeof *SRC; i++)
		for (size_t k = 0; k < sizeof MODE / sizeof *MODE; k++)
			check_case(SRC[i].w, SRC[i].h, SRC[i].fmt,
			           diatom_scale_rect(MODE[k].m, SRC[i].w, SRC[i].h, SRC[i].aspect, FB_W, FB_H), MODE[k].name);

	printf("flipscale: odd sizes, downscales, whole factors, clipping\n");
	{
		static const struct { int w, h; diatom_rect d; const char *label; } ODD[] = {
			{ 7, 5, { 100, 50, 23, 17 }, "odd" },
			{ 13, 11, { 3, 9, 41, 30 }, "odd" },
			{ 300, 200, { 10, 10, 100, 77 }, "down" },
			{ 64, 48, { 0, 0, 64, 48 }, "1x" },
			{ 64, 48, { 31, 17, 192, 96 }, "3x2" },
			{ 160, 144, { -40, -20, 832, 600 }, "cropped" },
			{ 160, 144, { 700, 500, 320, 288 }, "corner" },
			{ 9, 7, { 5, 5, 18, 7 }, "2x1" },
		};
		for (size_t i = 0; i < sizeof ODD / sizeof *ODD; i++) {
			check_case(ODD[i].w, ODD[i].h, DIATOM_PIX_RGB565, ODD[i].d, ODD[i].label);
			check_case(ODD[i].w, ODD[i].h, DIATOM_PIX_XRGB8888, ODD[i].d, ODD[i].label);
		}
	}

	printf("flipscale: the turn\n");
	{
		/* 1x at an offset: source (x, y) is glass (W-1-(dx+x), H-1-(dy+y)). */
		image im = make_image(5, 3, DIATOM_PIX_XRGB8888);
		diatom_rect d = { 10, 20, 5, 3 };
		uint32_t *fb = draw(&im, d, DIATOM_FILTER_NEAREST, true);
		int ok = 1;

		for (int y = 0; y < 3; y++)
			for (int x = 0; x < 5; x++)
				ok &= fb[(FB_H - 1 - (20 + y)) * FB_W + (FB_W - 1 - (10 + x))] == im.xrgb[y * 5 + x];
		CHECK(ok, "source pixel (x, y) at fb (W-1-(dx+x), H-1-(dy+y))");
		CHECK(fb[(FB_H - 21) * FB_W + (FB_W - 11)] == im.xrgb[0], "top-left of the picture at the bottom-right of memory");
		free(fb);
		free_image(&im);
	}

	printf("flipscale: rgb565\n");
	{
		image im = { 256, 256, DIATOM_PIX_RGB565, 512, malloc(65536 * 2), NULL };
		diatom_rect d = { 0, 0, 256, 256 };
		int bad[2] = { 0, 0 };

		for (int i = 0; i < 65536; i++) ((uint16_t *)im.px)[i] = (uint16_t)i;
		for (int plain = 0; plain < 2; plain++) {
			uint32_t *fb = draw(&im, d, DIATOM_FILTER_NEAREST, plain);
			for (int i = 0; i < 65536; i++) {
				int x = i % 256, y = i / 256;
				bad[plain] += fb[(FB_H - 1 - y) * FB_W + (FB_W - 1 - x)] != rgb565_ref((uint16_t)i);
			}
			free(fb);
		}
		CHECK(!bad[0] && !bad[1], "all 65536 values as rgb565() had them (NEON %d off, C %d off)", bad[0], bad[1]);
		for (int i = 0; i < 65536; i++) bad[0] += diatom_rgb565((uint16_t)i) != rgb565_ref((uint16_t)i);
		CHECK(!bad[0], "diatom_rgb565 is the old rgb565");
		free(im.px);
	}

	printf("flipscale: rects off the panel, frames too big, one scaler reused\n");
	{
		static const diatom_rect OFF[] = { { 10, 600, 23, 17 }, { 800, 10, 23, 17 }, { -40, 10, 23, 17 }, { 10, -40, 23, 17 } };
		image im = make_image(7, 5, DIATOM_PIX_RGB565), big = make_image(240, 160, DIATOM_PIX_RGB565);
		diatom_scaler s;
		uint32_t *fb = page();
		bool clean = true;

		/* Nothing of the rect is on the panel: prepare may refuse or accept, and
		 * a draw after either writes nothing. */
		memset(&s, 0, sizeof s);
		for (size_t i = 0; i < sizeof OFF / sizeof *OFF; i++) {
			diatom_scaler_prepare(&s, im.w, im.h, OFF[i], DIATOM_FILTER_SHARP, FB_W, FB_H);
			diatom_scaler_draw(&s, im.px, im.pitch, im.fmt, fb, FB_W);
		}
		for (int i = 0; i < FB_W * FB_H; i++) clean &= fb[i] == SENTINEL;
		CHECK(clean, "a rect wholly off the panel draws nothing");
		CHECK(!diatom_scaler_prepare(&s, 40000, 5, (diatom_rect){ 0, 0, 100, 50 }, DIATOM_FILTER_NEAREST, FB_W, FB_H),
		      "a frame wider than the column map holds is refused");
		CHECK(!diatom_scaler_prepare(&s, 5, 40000, (diatom_rect){ 0, 0, 100, 50 }, DIATOM_FILTER_NEAREST, FB_W, FB_H),
		      "a frame taller than the row map holds is refused");

		/* The same scaler across a size, rect and filter change, as a game
		 * that changes resolution does: the second draw matches a fresh one. */
		{
			diatom_rect d = { 0, 29, 752, 501 };
			uint32_t *fresh = draw(&big, d, DIATOM_FILTER_LINEAR, false), *again = page();
			bool same = true;

			CHECK(diatom_scaler_prepare(&s, im.w, im.h, (diatom_rect){ 100, 50, 23, 17 }, DIATOM_FILTER_SHARP, FB_W, FB_H),
			      "prepare after a refusal");
			diatom_scaler_draw(&s, im.px, im.pitch, im.fmt, fb, FB_W);
			CHECK(diatom_scaler_prepare(&s, big.w, big.h, d, DIATOM_FILTER_LINEAR, FB_W, FB_H), "prepare for the new size");
			diatom_scaler_draw(&s, big.px, big.pitch, big.fmt, again, FB_W);
			for (int i = 0; i < FB_W * FB_H; i++) same &= again[i] == fresh[i];
			CHECK(same, "a reused scaler draws what a fresh one does");
			free(fresh);
			free(again);
		}
		diatom_scaler_free(&s);
		free(fb);
		free_image(&im);
		free_image(&big);
	}

	printf("flipscale: whole-factor upscale\n");
	{
		/* Widths below, at and past a NEON vector (8 RGB565, 4 xRGB pixels),
		 * odd ones with a tail, and the Game Boy's. */
		static const struct { int w, h; } SIZE[] = { { 1, 1 }, { 3, 2 }, { 7, 5 }, { 8, 3 }, { 13, 11 }, { 37, 4 }, { 160, 9 } };

		for (size_t i = 0; i < sizeof SIZE / sizeof *SIZE; i++)
			for (int kx = 1; kx <= 5; kx++)
				for (int ky = 1; ky <= 5; ky++) {
					check_upscale(SIZE[i].w, SIZE[i].h, DIATOM_PIX_RGB565, kx, ky);
					check_upscale(SIZE[i].w, SIZE[i].h, DIATOM_PIX_XRGB8888, kx, ky);
				}
	}

	if (failures) { printf("flipscale: %d FAILED\n", failures); return 1; }
	printf("flipscale: ok\n");
	return 0;
}
