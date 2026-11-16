/* SPDX-License-Identifier: MIT */
/* The Flip's Look (port/miyoo_look.c, ADR-9002) against references written
 * here, pixel by pixel, from the definitions rather than from the renderer's
 * tables:
 *   a cell is the run of panel pixels whose centre maps to one source pixel,
 *     its first column and first row the gap;
 *   grid: the tone, and the gaps mixed toward black, in integers;
 *   dots: four shades by brightness, ink, the gaps' share of their
 *     neighbours' ink, the shadow sampled right and up, within 1 a channel;
 *   the picture lands turned 180 degrees, clipped, nothing written outside it;
 *   a few pixels worked out by hand;
 *   the bezel: its page, its name, the file read only when its header is
 *     the panel's size, decoded without asking for more memory than any
 *     752x560 PNG needs, and its lens laid back over the picture. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "miyoo_look.h"
#include "stb_image.h"
#include "stb_image_write.h"

/* vendor/stb/stb_image.c: its ceiling on one allocation, and the largest it
 * has handed out. */
extern const size_t stbi_diatom_alloc_max;
extern size_t stbi_diatom_largest;

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define FB_W 752
#define FB_H 560
#define SENTINEL 0x12345678u

static uint32_t rnd(void)
{
	static uint32_t x = 2463534242u;

	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return x;
}

static uint32_t rgb565_ref(uint16_t p)
{
	uint32_t r = p >> 11 & 31, g = p >> 5 & 63, b = p & 31;

	return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

typedef struct {
	int           w, h;
	diatom_pixfmt fmt;
	size_t        pitch;
	void         *px;
	uint32_t     *rgb;              /* the same picture, 0x00RRGGBB */
} image;

/* `grey`: four Game Boy shades only, as the launcher's palette gives; else
 * any colour, as a colour game, or a monochrome one with a tinted palette. */
static image make_image(int w, int h, diatom_pixfmt fmt, bool grey)
{
	static const uint16_t G565[4] = { 0xFFFF, 0xAD55, 0x52AA, 0x0000 };
	static const uint32_t G888[4] = { 0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000 };
	image im = { w, h, fmt, 0, NULL, NULL };
	int bpp = fmt == DIATOM_PIX_RGB565 ? 2 : 4;
	uint32_t run = 0;

	im.pitch = (size_t)(w + 5) * bpp;
	im.px = malloc(im.pitch * h);
	im.rgb = malloc((size_t)w * h * 4);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t v = rnd();

			if (x % 7 >= 3) v = run;    /* runs of one colour, as a game draws */
			run = v;
			if (bpp == 2) {
				uint16_t p = grey ? G565[v & 3] : (uint16_t)v;
				((uint16_t *)((uint8_t *)im.px + y * im.pitch))[x] = p;
				im.rgb[y * w + x] = rgb565_ref(p);
			} else {
				uint32_t p = grey ? G888[v & 3] | (v & 0xFF000000u) : v;   /* junk in the top byte */
				((uint32_t *)((uint8_t *)im.px + y * im.pitch))[x] = p;
				im.rgb[y * w + x] = p & 0xFFFFFF;
			}
		}
	return im;
}

static void free_image(image *im) { free(im->px); free(im->rgb); }

/* ---------- references ----------------------------------------------------- */

static int src_of(int x, int n_src, int n_dst) { return (int)floor((x + 0.5) * n_src / n_dst); }

/* Panel pixel x's place in its cell: k from the cell's start, and the cell's width. */
static void place(int x, int n_src, int n_dst, int *k, int *len)
{
	int s = src_of(x, n_src, n_dst), a = x, b = x;

	while (a > 0 && src_of(a - 1, n_src, n_dst) == s) a--;
	while (b < n_dst - 1 && src_of(b + 1, n_src, n_dst) == s) b++;
	*k = x - a;
	*len = b - a + 1;
}

static int ch(uint32_t p, int c) { return p >> (16 - 8 * c) & 255; }

/* A file of `size` bytes in `path` that starts with a PNG signature and an
 * IHDR for w x h RGBA of `depth` bits, and nothing a decoder could use after
 * it. `chunk` is the first chunk's type. */
static bool header_file(const char *path, const char *chunk, uint32_t w, uint32_t h, int depth, long size)
{
	uint8_t head[26] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13 };
	size_t n = size < (long)sizeof head ? (size_t)size : sizeof head;
	FILE *f = fopen(path, "wb");
	bool ok;

	memcpy(head + 12, chunk, 4);
	for (int i = 0; i < 4; i++) { head[16 + i] = (uint8_t)(w >> (24 - 8 * i)); head[20 + i] = (uint8_t)(h >> (24 - 8 * i)); }
	head[24] = (uint8_t)depth;
	head[25] = 6;
	if (!f) return false;
	ok = fwrite(head, 1, n, f) == n;
	ok &= fclose(f) == 0;
	return ok && (size <= (long)sizeof head || truncate(path, size) == 0);
}

/* ---- PNGs written by hand, for the decoder ---- */

typedef struct { uint8_t *p; size_t n, cap; uint32_t bits; int nbits; } bytes;

static void put(bytes *b, const void *d, size_t n)
{
	if (b->n + n > b->cap) {
		while (b->n + n > b->cap) b->cap = b->cap ? b->cap * 2 : 4096;
		b->p = realloc(b->p, b->cap);
	}
	memcpy(b->p + b->n, d, n);
	b->n += n;
}

static void put8(bytes *b, uint8_t v) { put(b, &v, 1); }
static void put32(bytes *b, uint32_t v) { uint8_t be[4] = { v >> 24, v >> 16, v >> 8, v }; put(b, be, 4); }

/* Deflate's bits, least significant first; its Huffman codes, most first. */
static void put_bits(bytes *b, uint32_t v, int n)
{
	b->bits |= v << b->nbits;
	for (b->nbits += n; b->nbits >= 8; b->nbits -= 8, b->bits >>= 8) put8(b, (uint8_t)b->bits);
}

static void put_code(bytes *b, uint32_t code, int n)
{
	while (n--) put_bits(b, code >> n & 1, 1);
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;

	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
	}
	return ~c;
}

/* A chunk of `n` data bytes whose length field says `declared`. */
static void put_chunk(bytes *b, const char *type, const void *data, size_t n, uint32_t declared)
{
	size_t at;

	put32(b, declared);
	at = b->n;
	put(b, type, 4);
	if (n) put(b, data, n);
	put32(b, crc32_of(b->p + at, n + 4));
}

/* The signature and IHDR. */
static void put_head(bytes *b, int depth, int interlace)
{
	uint8_t ihdr[13] = { 0, 0, FB_W >> 8, FB_W & 255, 0, 0, FB_H >> 8, FB_H & 255, (uint8_t)depth, 6, 0, 0, (uint8_t)interlace };

	put(b, "\x89PNG\r\n\x1A\n", 8);
	put_chunk(b, "IHDR", ihdr, sizeof ihdr, sizeof ihdr);
}

/* `raw` in a zlib stream of stored blocks, as IDAT, then IEND. */
static void put_stored(bytes *b, const uint8_t *raw, size_t n)
{
	bytes z = { 0 };
	uint32_t a = 1, s = 0;

	put8(&z, 0x78); put8(&z, 0x01);
	for (size_t at = 0; at < n || at == 0; at += 65535) {
		uint16_t len = n - at > 65535 ? 65535 : (uint16_t)(n - at);
		uint8_t hdr[5] = { at + len >= n, len & 255, len >> 8, ~len & 255, (uint16_t)~len >> 8 };

		put(&z, hdr, 5);
		if (len) put(&z, raw + at, len);
		if (!n) break;
	}
	for (size_t i = 0; i < n; i++) { a = (a + raw[i]) % 65521; s = (s + a) % 65521; }
	put32(&z, s << 16 | a);
	put_chunk(b, "IDAT", z.p, z.n, (uint32_t)z.n);
	put_chunk(b, "IEND", NULL, 0, 0);
	free(z.p);
}

static bool write_bytes(const char *path, const bytes *b)
{
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(b->p, 1, b->n, f) == b->n;

	return (f && fclose(f) == 0) && ok;
}

static void put_stbiw(void *ctx, void *data, int n)
{
	put(ctx, data, (size_t)n);
}

/* The file read as the port reads it, then decoded to RGBA; NULL if either
 * refuses, `largest` the biggest allocation stb was given. */
static uint8_t *decode(const char *path, size_t *largest, const char **why)
{
	size_t len;
	uint8_t *png, *px = NULL;
	int w, h, n;

	stbi_diatom_largest = 0;
	if ((png = diatom_bezel_read(path, FB_W, FB_H, &len, why))) {
		px = stbi_load_from_memory(png, (int)len, &w, &h, &n, 4);
		*why = px ? NULL : stbi_failure_reason();
		if (px && (w != FB_W || h != FB_H)) { stbi_image_free(px); px = NULL; *why = "decoded to another size"; }
		free(png);
	}
	*largest = stbi_diatom_largest;
	return px;
}

static uint32_t ref_grid(const diatom_look_grid *g, uint32_t p, bool gx, bool gy)
{
	int a = gx && gy ? (g->ax > g->ay ? g->ax : g->ay) : gx ? g->ax : gy ? g->ay : 0;
	uint32_t out = 0;

	for (int c = 0; c < 3; c++) {
		int t = (g->tone[c] * g->a + ch(p, c) * (255 - g->a) + 127) / 255;
		out |= (uint32_t)((g->black[c] * a + t * (255 - a) + 127) / 255) << (16 - 8 * c);
	}
	return out;
}

/* 0 lightest .. 3 darkest, by the mean of the channels. */
static int ref_shade(uint32_t p)
{
	double mean = (ch(p, 0) + ch(p, 1) + ch(p, 2)) / 3.0;

	return 3 - (int)floor(mean * 3 / 255 + 0.5);
}

static double ref_ink(const diatom_look_dots *d, double grey)
{
	return fmin(3.0 / d->grey_balance * (1 - grey) + d->baseline_alpha, 1.0);
}

static uint32_t ref_dots(const diatom_look_dots *d, const image *im, int sx, int sy,
                         int kx, int lx, int ky, int ly)
{
	/* Past the picture: no ink for a gap to take, shade 0 for the shadow. */
#define SH(x, y) ((x) < 0 || (y) < 0 || (x) >= im->w || (y) >= im->h ? 0 : ref_shade(im->rgb[(y) * im->w + (x)]))
#define INK(x, y) ((x) < 0 || (y) < 0 ? 0.0 : ref_ink(d, 1 - SH(x, y) / 3.0))
	double self = INK(sx, sy), a;
	double fx = (kx + 0.5) / lx, fy = (ky + 0.5) / ly;
	double wself_x = 1 - fx, wself_y = fy;
	double gs = 1 - SH(sx, sy) / 3.0, gr = 1 - SH(sx + 1, sy) / 3.0;
	double gu = 1 - SH(sx, sy - 1) / 3.0, gur = 1 - SH(sx + 1, sy - 1) / 3.0;
	double grey = wself_y * (wself_x * gs + (1 - wself_x) * gr) + (1 - wself_y) * (wself_x * gu + (1 - wself_x) * gur);
	double shadow;
	uint32_t out = 0;

	if (kx == 0 && ky == 0) a = 0;
	else if (kx == 0) a = 0.38 * (self + INK(sx - 1, sy));
	else if (ky == 0) a = 0.38 * (self + INK(sx, sy - 1));
	else a = self;
	shadow = fmin(ref_ink(d, grey) * d->contrast * d->shadow_opacity * 0.30 * (1 - 0.75 * a), 1.0);
	for (int c = 0; c < 3; c++) {
		double v = d->ink[c] / 255.0 * a * (1 - d->contrast * a * a) +
		           d->panel[c] / 255.0 * (1 - shadow) * (d->screen_light - d->contrast * d->pixel_opacity * a);
		out |= (uint32_t)lround(fmin(fmax(v, 0), 1) * 255) << (16 - 8 * c);
	}
	return out;
#undef SH
#undef INK
}

/* The picture as the screen shows it, d.w x d.h, upright, 0x00RRGGBB. */
static uint32_t *reference(const diatom_look_screen *s, const image *im, diatom_rect d)
{
	uint32_t *o = malloc((size_t)d.w * d.h * 4);

	for (int y = 0; y < d.h; y++)
		for (int x = 0; x < d.w; x++) {
			int sx = src_of(x, im->w, d.w), sy = src_of(y, im->h, d.h), kx, lx, ky, ly;

			place(x, im->w, d.w, &kx, &lx);
			place(y, im->h, d.h, &ky, &ly);
			o[y * d.w + x] = s->kind == DIATOM_LOOK_GRID
			                 ? ref_grid(&s->grid, im->rgb[sy * im->w + sx], kx == 0, ky == 0)
			                 : ref_dots(&s->dots, im, sx, sy, kx, lx, ky, ly);
		}
	return o;
}

/* ---------- drawing and comparing ------------------------------------------ */

static uint32_t *page(void)
{
	uint32_t *fb = malloc((size_t)FB_W * FB_H * 4);

	for (int i = 0; i < FB_W * FB_H; i++) fb[i] = SENTINEL;
	return fb;
}

static uint32_t *draw(const diatom_look_screen *s, const image *im, diatom_rect d)
{
	diatom_look l;
	uint32_t *fb = page();

	memset(&l, 0, sizeof l);
	CHECK(diatom_look_prepare(&l, s, im->w, im->h, d, FB_W, FB_H), "%s %dx%d -> %dx%d: prepare failed",
	      s->name, im->w, im->h, d.w, d.h);
	diatom_look_draw(&l, im->px, im->pitch, im->fmt, fb, FB_W);
	diatom_look_free(&l);
	return fb;
}

/* The page against a logical reference: inside the visible rect within `tol`
 * a channel, outside it untouched. Returns the largest difference. */
static int compare(const uint32_t *fb, const uint32_t *ref, diatom_rect d, int tol, const char *what)
{
	int worst = 0, outside = 0;

	for (int fy = 0; fy < FB_H; fy++)
		for (int fx = 0; fx < FB_W; fx++) {
			int x = FB_W - 1 - fx - d.x, y = FB_H - 1 - fy - d.y;
			uint32_t got = fb[fy * FB_W + fx];

			if (x < 0 || y < 0 || x >= d.w || y >= d.h) {
				outside += got != SENTINEL;
				continue;
			}
			for (int c = 0; c < 3; c++) {
				int e = abs(ch(got, c) - ch(ref[y * d.w + x], c));
				if (e > worst) worst = e;
			}
		}
	CHECK(!outside, "%s: %d pixels written outside the rect", what, outside);
	CHECK(worst <= tol, "%s: off by %d (allowed %d)", what, worst, tol);
	return worst;
}

static const diatom_look_screen *screen(const char *name)
{
	char path[64];

	snprintf(path, sizeof path, "/mnt/SDCARD/TortOS/shaders/flipfx-%s.glsl", name);
	return diatom_look_find(path);
}

static void check_case(const char *name, int w, int h, diatom_pixfmt fmt, bool grey, diatom_rect d, const char *label)
{
	const diatom_look_screen *s = screen(name);
	image im = make_image(w, h, fmt, grey);
	uint32_t *ref = reference(s, &im, d), *fb = draw(s, &im, d);
	char what[160];
	int worst;

	snprintf(what, sizeof what, "%s %s %dx%d %s%s -> %dx%d%+d%+d", name, label, w, h,
	         fmt == DIATOM_PIX_RGB565 ? "rgb565" : "xrgb8888", grey ? " grey" : "", d.w, d.h, d.x, d.y);
	worst = compare(fb, ref, d, s->kind == DIATOM_LOOK_GRID ? 0 : 1, what);
	printf("  %-62s within %d\n", what, worst);
	free(ref);
	free(fb);
	free_image(&im);
}

/* The upright logical pixel (x, y) of the rect d on a turned page. */
static uint32_t at(const uint32_t *fb, diatom_rect d, int x, int y)
{
	return fb[(FB_H - 1 - (d.y + y)) * FB_W + (FB_W - 1 - (d.x + x))] & 0xFFFFFF;
}

int main(void)
{
	static const diatom_pixfmt FMT[] = { DIATOM_PIX_RGB565, DIATOM_PIX_XRGB8888 };
	static const struct { const char *label; diatom_rect d; } GB[] = {
		{ "integer", { 136, 64, 480, 432 } }, { "aspect", { 65, 0, 622, 560 } }, { "stretch", { 0, 0, 752, 560 } },
		{ "bezel", { 136, 72, 480, 432 } },
	}, GBA[] = {
		{ "integer", { 16, 40, 720, 480 } }, { "aspect", { 0, 29, 752, 501 } }, { "stretch", { 0, 0, 752, 560 } },
		{ "bezel", { 0, 4, 752, 501 } },
	};

	printf("fliplook: the screens\n");
	CHECK(screen("dots-dmg") && screen("dots-pocket") && screen("dots-light") && screen("grid-gbc") && screen("grid-gba"),
	      "all five found by their file names");
	CHECK(!diatom_look_find("/mnt/SDCARD/TortOS/shaders/flipfx-scanlines.glsl") &&
	      !diatom_look_find("/mnt/SDCARD/TortOS/shaders/flipfx-bezel-dmg.glsl") &&
	      !diatom_look_find("/x/flipfx-grid-gbc/other.glsl") && !diatom_look_find(NULL),
	      "no screen for scanlines, a bezel, a directory's name, or nothing");
	for (int i = 0; i < DIATOM_LOOK_SCREENS; i++) {
		const diatom_look_screen *s = &diatom_look_screens[i];
		uint32_t bare = 0;

		/* The panel with nothing drawn: no ink and no shadow; a white cell for a grid. */
		for (int c = 0; c < 3; c++) {
			int v = s->kind == DIATOM_LOOK_DOTS
			        ? (int)lround(s->dots.panel[c] * s->dots.screen_light)
			        : (s->grid.tone[c] * s->grid.a + 255 * (255 - s->grid.a) + 127) / 255;
			bare |= (uint32_t)v << (16 - 8 * c);
		}
		CHECK((s->panel & 0xFFFFFF) == bare, "%s: panel %06x, its formula gives %06x", s->name,
		      (unsigned)(s->panel & 0xFFFFFF), (unsigned)bare);
	}

	printf("fliplook: when it draws\n");
	{
		const diatom_look_screen *dots = screen("dots-dmg"), *grid = screen("grid-gba");

		CHECK(diatom_look_fits(dots, 160, 144, (diatom_rect){ 0, 0, 320, 288 }), "2x fits");
		CHECK(!diatom_look_fits(dots, 160, 144, (diatom_rect){ 0, 0, 319, 560 }), "under 2x across does not");
		CHECK(!diatom_look_fits(grid, 240, 160, (diatom_rect){ 0, 0, 752, 319 }), "under 2x down does not");
		CHECK(!diatom_look_fits(grid, 241, 160, (diatom_rect){ 0, 0, 752, 560 }), "a frame wider than 240 does not");
		CHECK(!diatom_look_fits(grid, 240, 161, (diatom_rect){ 0, 0, 752, 560 }), "a frame taller than 160 does not");
		CHECK(!diatom_look_fits(dots, 100, 100, (diatom_rect){ 0, 0, 700, 500 }), "dots wider than the table's cells do not");
		CHECK(diatom_look_fits(grid, 100, 100, (diatom_rect){ 0, 0, 700, 500 }), "a grid of any cell does");
		CHECK(!diatom_look_fits(NULL, 160, 144, (diatom_rect){ 0, 0, 480, 432 }), "no screen, no look");
		CHECK(!diatom_look_fits(grid, 1, 1, (diatom_rect){ 0, 0, 65536, 2 }) &&
		      !diatom_look_fits(grid, 1, 1, (diatom_rect){ 0, 0, 2, 65536 }),
		      "a rect longer than the maps' 16 bits does not");
	}

	printf("fliplook: what prepare refuses\n");
	{
		static const struct { const char *what; diatom_rect d; } REFUSED[] = {
			{ "wider than twice the panel", { 0, 0, 2 * FB_W + 1, 4 } },
			{ "taller than twice the panel", { 0, 0, 4, 2 * FB_H + 1 } },
			{ "the widest the maps hold", { 0, 0, 65535, 2 } },
			{ "starting past the panel's right", { FB_W + 1, 0, 4, 4 } },
			{ "starting past the panel's bottom", { 0, FB_H + 1, 4, 4 } },
			{ "ending before the panel's left", { -5, 0, 4, 4 } },
			{ "ending above the panel's top", { 0, -5, 4, 4 } },
			{ "at the most negative x", { -2147483647 - 1, 0, 4, 4 } },
		};
		const diatom_look_screen *grid = screen("grid-gbc");
		image im = make_image(1, 1, DIATOM_PIX_XRGB8888, false);
		diatom_look l;
		uint32_t *fb = page();
		bool untouched = true;

		memset(&l, 0, sizeof l);
		for (size_t i = 0; i < sizeof REFUSED / sizeof *REFUSED; i++) {
			CHECK(!diatom_look_prepare(&l, grid, 1, 1, REFUSED[i].d, FB_W, FB_H), "a rect %s is refused", REFUSED[i].what);
			diatom_look_draw(&l, im.px, im.pitch, im.fmt, fb, FB_W);
		}
		for (int i = 0; i < FB_W * FB_H; i++) untouched &= fb[i] == SENTINEL;
		CHECK(untouched, "and draws nothing");
		CHECK(!diatom_look_prepare(&l, grid, 1, 1, (diatom_rect){ 0, 0, 4, 4 }, INT16_MAX + 1, FB_H),
		      "a panel wider than 15 bits is refused");
		CHECK(diatom_look_prepare(&l, grid, 160, 144, (diatom_rect){ -8, -8, 2 * FB_W, 2 * FB_H }, FB_W, FB_H),
		      "a rect twice the panel, cropped, is not");
		diatom_look_free(&l);
		free(fb);
		free_image(&im);
	}

	printf("fliplook: Game Boy dots and Game Boy Color grid, the panel's modes\n");
	for (size_t m = 0; m < sizeof GB / sizeof *GB; m++)
		for (size_t f = 0; f < 2; f++) {
			check_case("dots-dmg", 160, 144, FMT[f], true, GB[m].d, GB[m].label);
			check_case("grid-gbc", 160, 144, FMT[f], false, GB[m].d, GB[m].label);
		}
	check_case("dots-pocket", 160, 144, DIATOM_PIX_RGB565, true, GB[1].d, "aspect");
	check_case("dots-light", 160, 144, DIATOM_PIX_XRGB8888, true, GB[0].d, "integer");
	check_case("dots-dmg", 160, 144, DIATOM_PIX_RGB565, false, GB[1].d, "aspect, tinted");

	printf("fliplook: Game Boy Advance grid, the panel's modes\n");
	for (size_t m = 0; m < sizeof GBA / sizeof *GBA; m++)
		for (size_t f = 0; f < 2; f++) check_case("grid-gba", 240, 160, FMT[f], false, GBA[m].d, GBA[m].label);

	printf("fliplook: odd sizes and clipping\n");
	{
		static const struct { int w, h; diatom_rect d; const char *label; } ODD[] = {
			{ 7, 5, { 100, 50, 23, 17 }, "odd" },
			{ 13, 11, { 3, 9, 41, 30 }, "odd" },
			{ 2, 2, { 0, 0, 4, 4 }, "2x2" },
			{ 160, 144, { -40, -20, 640, 576 }, "cropped" },
			{ 160, 144, { 500, 400, 480, 432 }, "corner" },
		};
		for (size_t i = 0; i < sizeof ODD / sizeof *ODD; i++)
			for (size_t f = 0; f < 2; f++) {
				check_case("dots-pocket", ODD[i].w, ODD[i].h, FMT[f], true, ODD[i].d, ODD[i].label);
				check_case("grid-gba", ODD[i].w, ODD[i].h, FMT[f], false, ODD[i].d, ODD[i].label);
			}
	}

	printf("fliplook: by hand\n");
	{
		image white = make_image(160, 144, DIATOM_PIX_XRGB8888, true), black = make_image(160, 144, DIATOM_PIX_XRGB8888, true);
		diatom_rect d = { 136, 64, 480, 432 };
		uint32_t *fb;

		for (int i = 0; i < 160 * 144; i++) {
			int x = i % 160, y = i / 160;
			((uint32_t *)((uint8_t *)white.px + y * white.pitch))[x] = 0xFFFFFF;
			((uint32_t *)((uint8_t *)black.px + y * black.pitch))[x] = 0x000000;
		}
		/* dots-dmg, all white, 3x: a cell pixel has the white dot's ink, 0.05,
		 * and its shadow 0.05 * 0.7 * 0.9 * 0.30 * (1 - 0.75 * 0.05);
		 * R 161 * (1 - 0.00910) * (0.85 - 0.63 * 0.05) = 130.58,
		 * G 90 * 0.05 * (1 - 0.7 * 0.0025) + the same = 135.07, B 170 * 0.04991 = 8.49.
		 * A crossing has no ink: R 161 * (1 - 0.00945) * 0.85 = 135.56, B 0. */
		fb = draw(screen("dots-dmg"), &white, d);
		CHECK(at(fb, d, 4, 4) == 0x838708, "dots-dmg white cell %06x, by hand 838708", (unsigned)at(fb, d, 4, 4));
		CHECK(at(fb, d, 3, 3) == 0x888800, "dots-dmg white crossing %06x, by hand 888800", (unsigned)at(fb, d, 3, 3));
		free(fb);
		/* grid-gbc: white is the panel, (35*96 + 255*159 + 127)/255 = 172,
		 * (80*96 + 255*159 + 127)/255 = 189, (41*96 + 255*159 + 127)/255 = 174;
		 * its column gap (14*87 + 172*168 + 127)/255 = 118,
		 * (25*87 + 189*168 + 127)/255 = 133, (28*87 + 174*168 + 127)/255 = 124;
		 * black (35*96 + 127)/255 = 13,
		 * (80*96 + 127)/255 = 30, (41*96 + 127)/255 = 15. */
		fb = draw(screen("grid-gbc"), &white, d);
		CHECK(at(fb, d, 4, 4) == 0xACBDAE, "grid-gbc white cell %06x, by hand acbdae", (unsigned)at(fb, d, 4, 4));
		CHECK(at(fb, d, 3, 4) == 0x76857C, "grid-gbc white column gap %06x, by hand 76857c", (unsigned)at(fb, d, 3, 4));
		free(fb);
		fb = draw(screen("grid-gbc"), &black, d);
		CHECK(at(fb, d, 1, 1) == 0x0D1E0F, "grid-gbc black cell %06x, by hand 0d1e0f", (unsigned)at(fb, d, 1, 1));
		free(fb);
		/* dots-dmg, white but for a black pixel at (2, 1), 3x: the bottom-right
		 * panel pixel of (1, 1)'s cell is 5/6 across and down it, so the shadow
		 * takes 5/6 * 5/6 = 25/36 of the black dot to its right and the rest from
		 * white: ink 3/2.6 * 25/36 + 0.05 = 0.8513, shadow that * 0.63 * 0.30 *
		 * (1 - 0.75 * 0.05) = 0.15486; R 161 * (1 - 0.15486) * 0.8185 = 111.37,
		 * G 4.49 + the same = 115.86, B 8.49. A whole pixel's offset would take
		 * the black dot in full there. */
		((uint32_t *)((uint8_t *)white.px + white.pitch))[2] = 0x000000;
		fb = draw(screen("dots-dmg"), &white, d);
		CHECK(at(fb, d, 5, 5) == 0x6F7408, "dots-dmg shadow beside a black dot %06x, by hand 6f7408", (unsigned)at(fb, d, 5, 5));
		free(fb);
		free_image(&white);
		free_image(&black);
	}

	printf("fliplook: the turn\n");
	{
		/* 2x at an offset: source (x, y)'s cell is logical (dx + 2x .. +1,
		 * dy + 2y .. +1), at fb (W-1-that, H-1-that); its gap the cell's
		 * top-left, the largest fb column and row of the four. */
		image im = make_image(5, 3, DIATOM_PIX_XRGB8888, false);
		const diatom_look_screen *s = screen("grid-gba");
		diatom_rect d = { 10, 20, 10, 6 };
		uint32_t *fb = draw(s, &im, d);
		int ok = 1;

		for (int y = 0; y < 3; y++)
			for (int x = 0; x < 5; x++) {
				uint32_t p = im.rgb[y * 5 + x];
				int fx = FB_W - 1 - (10 + 2 * x), fy = FB_H - 1 - (20 + 2 * y);

				ok &= (fb[fy * FB_W + fx] & 0xFFFFFF) == ref_grid(&s->grid, p, true, true);
				ok &= (fb[fy * FB_W + fx - 1] & 0xFFFFFF) == ref_grid(&s->grid, p, false, true);
				ok &= (fb[(fy - 1) * FB_W + fx] & 0xFFFFFF) == ref_grid(&s->grid, p, true, false);
				ok &= (fb[(fy - 1) * FB_W + fx - 1] & 0xFFFFFF) == ref_grid(&s->grid, p, false, false);
			}
		CHECK(ok, "source pixel (x, y)'s cell at fb (W-1-(dx+2x), H-1-(dy+2y)), its gap there");
		free(fb);
		free_image(&im);
	}

	printf("fliplook: one renderer reused\n");
	{
		image gb = make_image(160, 144, DIATOM_PIX_RGB565, true), gba = make_image(240, 160, DIATOM_PIX_XRGB8888, false);
		diatom_look l;
		uint32_t *a = page(), *c = page(), *ref;
		bool same = true;

		memset(&l, 0, sizeof l);
		CHECK(!diatom_look_prepare(&l, screen("dots-dmg"), 160, 144, (diatom_rect){ 0, 0, 300, 288 }, FB_W, FB_H),
		      "prepare refuses under 2x");
		diatom_look_draw(&l, gb.px, gb.pitch, gb.fmt, a, FB_W);
		for (int i = 0; i < FB_W * FB_H; i++) same &= a[i] == SENTINEL;
		CHECK(same, "a refused look draws nothing");
		CHECK(diatom_look_prepare(&l, screen("dots-dmg"), 160, 144, GB[1].d, FB_W, FB_H), "dots");
		diatom_look_draw(&l, gb.px, gb.pitch, gb.fmt, a, FB_W);
		ref = reference(screen("dots-dmg"), &gb, GB[1].d);
		compare(a, ref, GB[1].d, 1, "a refused renderer reused for dots");
		free(ref);
		CHECK(diatom_look_prepare(&l, screen("grid-gba"), 240, 160, GBA[1].d, FB_W, FB_H), "then a grid");
		diatom_look_draw(&l, gba.px, gba.pitch, gba.fmt, c, FB_W);
		ref = reference(screen("grid-gba"), &gba, GBA[1].d);
		compare(c, ref, GBA[1].d, 0, "the dots' renderer reused for a grid");
		free(ref);
		diatom_look_free(&l);
		free(a); free(c);
		free_image(&gb);
		free_image(&gba);
	}

	printf("fliplook: bezel names\n");
	{
		diatom_rect r = { 0, 0, 0, 0 };

		CHECK(diatom_bezel_parse("dmg-480x432+136+72.png", "dmg", &r) && r.w == 480 && r.h == 432 && r.x == 136 && r.y == 72,
		      "dmg-480x432+136+72.png");
		CHECK(diatom_bezel_parse("gba-752x501+0+4.png", "gba", &r) && r.w == 752 && r.h == 501 && r.x == 0 && r.y == 4,
		      "gba-752x501+0+4.png");
		CHECK(!diatom_bezel_parse("gba-752x501+0+4.png", "gbc", &r), "another lens's");
		CHECK(!diatom_bezel_parse("pocket-480x432+136+72.png", "pocket-480", &r), "a lens name that is a prefix");
		CHECK(!diatom_bezel_parse("dmg-480x432+136+72.png.bak", "dmg", &r) && !diatom_bezel_parse("dmg-480x432+136.png", "dmg", &r) &&
		      !diatom_bezel_parse("dmg-480x432-136+72.png", "dmg", &r) && !diatom_bezel_parse("dmg-x432+1+2.png", "dmg", &r) &&
		      !diatom_bezel_parse("dmg-0x432+1+2.png", "dmg", &r) && !diatom_bezel_parse("dmg-480x432+136+72.PNG", "dmg", &r) &&
		      !diatom_bezel_parse("dmgx-480x432+136+72.png", "dmg", &r),
		      "nothing else");
	}

	printf("fliplook: bezel files\n");
	{
		static const struct { const char *what, *chunk; uint32_t w, h; int depth; long size; const char *why; } FILES[] = {
			{ "an 8192x8192 PNG of 261 KB", "IHDR", 8192, 8192, 8, 261 * 1024, "not the panel's size" },
			{ "a 640x480 PNG", "IHDR", 640, 480, 8, 4096, "not the panel's size" },
			{ "a 752x560 PNG over 4 MB", "IHDR", FB_W, FB_H, 8, DIATOM_BEZEL_FILE_MAX + 1, "larger than 4 MB" },
			{ "a PNG whose first chunk is not IHDR", "CgBI", FB_W, FB_H, 8, 4096, "not a PNG" },
			{ "a file shorter than a PNG header", "IHDR", FB_W, FB_H, 8, 20, "not a PNG" },
			{ "a file that ends before IHDR's bit depth", "IHDR", FB_W, FB_H, 8, 24, "not a PNG" },
			{ "a 752x560 16-bit PNG", "IHDR", FB_W, FB_H, 16, 4096, "not 8 bits deep" },
			{ "a 752x560 4-bit PNG", "IHDR", FB_W, FB_H, 4, 4096, "not 8 bits deep" },
			{ "a 752x560 PNG", "IHDR", FB_W, FB_H, 8, 4096, NULL },
			{ "a 752x560 PNG of exactly 4 MB", "IHDR", FB_W, FB_H, 8, DIATOM_BEZEL_FILE_MAX, NULL },
		};
		const char *tmp = getenv("TMPDIR");
		char path[512];
		int fd;

		snprintf(path, sizeof path, "%s/fliplook-XXXXXX", tmp && *tmp ? tmp : "/tmp");
		CHECK((fd = mkstemp(path)) >= 0, "a scratch file");
		if (fd >= 0) close(fd);
		/* The files hold a header and nothing to decode: what is refused is
		 * refused from the header and the file's length, before any image is
		 * decoded, and what passes is only read. */
		for (size_t i = 0; fd >= 0 && i < sizeof FILES / sizeof *FILES; i++) {
			const char *why = "unset";
			size_t len = 0;
			uint8_t *png;

			CHECK(header_file(path, FILES[i].chunk, FILES[i].w, FILES[i].h, FILES[i].depth, FILES[i].size), "%s: written", FILES[i].what);
			png = diatom_bezel_read(path, FB_W, FB_H, &len, &why);
			if (FILES[i].why)
				CHECK(!png && why && !strcmp(why, FILES[i].why), "%s: refused as %s, got %s", FILES[i].what, FILES[i].why,
				      png ? "the bytes" : why ? why : "no reason");
			else
				CHECK(png && !why && len == (size_t)FILES[i].size && !memcmp(png, "\x89PNG", 4), "%s: read whole", FILES[i].what);
			free(png);
		}
		if (fd >= 0) {
			const char *why = NULL;
			size_t len = 0;

			unlink(path);
			CHECK(!diatom_bezel_read(path, FB_W, FB_H, &len, &why) && why && !strcmp(why, "unreadable"), "no file: unreadable");
		}
	}

	printf("fliplook: bezel decode\n");
	{
		const char *tmp = getenv("TMPDIR"), *why;
		char path[512];
		size_t largest;
		uint8_t *px;
		int fd;

		/* No allocation is to pass twice the largest file: what the IDAT
		 * buffer's doubling needs (vendor/stb/stb_image.c). */
		snprintf(path, sizeof path, "%s/fliplook-XXXXXX", tmp && *tmp ? tmp : "/tmp");
		CHECK((fd = mkstemp(path)) >= 0, "a scratch file");
		if (fd >= 0) {
			close(fd);

			/* A 752x560 file of 0.8 MB whose one IDAT inflates to 128 MiB of
			 * zeros: a literal, then copies of 258 at distance 1 in fixed
			 * Huffman codes. stb takes the data past the scanlines, so all it
			 * would stop on is the memory. */
			{
				bytes b = { 0 }, z = { 0 };
				uint32_t k = (128u << 20) / 258, total = 1 + 258 * k;

				put8(&z, 0x78); put8(&z, 0x01);
				put_bits(&z, 1, 1); put_bits(&z, 1, 2);    /* the last block, fixed codes */
				put_code(&z, 0x30, 8);                     /* literal 0 */
				for (uint32_t i = 0; i < k; i++) { put_code(&z, 0xC5, 8); put_code(&z, 0, 5); }   /* 285, distance 1 */
				put_code(&z, 0, 7);                        /* end of block */
				if (z.nbits) put8(&z, (uint8_t)z.bits);
				put32(&z, (total % 65521) << 16 | 1);
				put_head(&b, 8, 0);
				put_chunk(&b, "IDAT", z.p, z.n, (uint32_t)z.n);
				put_chunk(&b, "IEND", NULL, 0, 0);
				CHECK(b.n < DIATOM_BEZEL_FILE_MAX && write_bytes(path, &b), "a zip bomb: written");
				px = decode(path, &largest, &why);
				CHECK(!px && why && !strcmp(why, "outofmem"), "a zip bomb of %zu bytes: refused as outofmem, got %s", b.n,
				      px ? "an image" : why ? why : "no reason");
				CHECK(largest <= stbi_diatom_alloc_max && largest <= 2 * DIATOM_BEZEL_FILE_MAX,
				      "a zip bomb: given %zu bytes at once (ceiling %zu)", largest, stbi_diatom_alloc_max);
				stbi_image_free(px);
				free(b.p); free(z.p);
			}

			/* 41 bytes: a 752x560 IHDR, then an IDAT that says 1 GiB and
			 * ends at its type. */
			{
				bytes b = { 0 };

				put_head(&b, 8, 0);
				put32(&b, 1u << 30);
				put(&b, "IDAT", 4);
				CHECK(b.n == 41 && write_bytes(path, &b), "a 1 GiB IDAT: written");
				px = decode(path, &largest, &why);
				CHECK(!px && why && !strcmp(why, "outofmem"), "a 1 GiB IDAT in 41 bytes: refused as outofmem, got %s",
				      px ? "an image" : why ? why : "no reason");
				CHECK(largest <= stbi_diatom_alloc_max && largest <= 2 * DIATOM_BEZEL_FILE_MAX,
				      "a 1 GiB IDAT: given %zu bytes at once (ceiling %zu)", largest, stbi_diatom_alloc_max);
				stbi_image_free(px);
				free(b.p);
			}

			/* stb_image_write's 752x560 RGBA, of noise: decoded, every pixel. */
			{
				uint8_t *rgba = malloc((size_t)FB_W * FB_H * 4);
				bytes b = { 0 };

				for (size_t i = 0; i < (size_t)FB_W * FB_H * 4; i++) rgba[i] = (uint8_t)rnd();
				CHECK(stbi_write_png_to_func(put_stbiw, &b, FB_W, FB_H, 4, rgba, FB_W * 4) && write_bytes(path, &b),
				      "an RGBA PNG: written");
				px = decode(path, &largest, &why);
				CHECK(px && !memcmp(px, rgba, (size_t)FB_W * FB_H * 4), "stb_image_write's 752x560 RGBA of %zu bytes: decoded, got %s",
				      b.n, px ? "other pixels" : why ? why : "no reason");
				printf("  %-62s largest allocation %zu\n", "stb_image_write's 752x560 RGBA", largest);
				stbi_image_free(px);
				free(b.p); free(rgba);
			}

			/* 8-bit RGBA, Adam7, stored: the scanlines a bezel can need,
			 * its passes' 1050 filter bytes running past the 560 of stb's
			 * guess and doubling its output (vendor/stb/stb_image.c). */
			{
				static const int P[7][4] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
				                             { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
				bytes b = { 0 }, raw = { 0 };
				int bad = 0;

				for (int p = 0; p < 7; p++)
					for (int y = P[p][1]; y < FB_H; y += P[p][3]) {
						if (P[p][0] >= FB_W) break;
						put8(&raw, 0);
						for (int x = P[p][0]; x < FB_W; x += P[p][2])
							for (int c = 0; c < 4; c++) put8(&raw, (uint8_t)(x * 131 + y * 71 + c * 67));
					}
				put_head(&b, 8, 1);
				put_stored(&b, raw.p, raw.n);
				CHECK(b.n <= DIATOM_BEZEL_FILE_MAX && write_bytes(path, &b), "an Adam7 PNG: written");
				px = decode(path, &largest, &why);
				for (int y = 0; px && y < FB_H; y++)
					for (int x = 0; x < FB_W; x++)
						for (int c = 0; c < 4; c++)
							bad += px[((size_t)y * FB_W + x) * 4 + c] != (uint8_t)(x * 131 + y * 71 + c * 67);
				CHECK(px && !bad, "a 752x560 RGBA Adam7 PNG of %zu bytes: decoded, got %s", b.n,
				      px ? "other pixels" : why ? why : "no reason");
				CHECK(largest == 2 * ((size_t)FB_W * FB_H * 4 + FB_H), "an Adam7 PNG: given %zu bytes at once, twice stb's guess",
				      largest);
				printf("  %-62s largest allocation %zu\n", "a 752x560 RGBA Adam7 PNG", largest);
				stbi_image_free(px);
				free(b.p); free(raw.p);
			}
			unlink(path);
		}
	}

	printf("fliplook: bezel\n");
	{
		/* A lens with every kind of alpha, over the picture and off it. */
		uint8_t *rgba = malloc((size_t)FB_W * FB_H * 4);
		uint32_t panel = screen("dots-pocket")->panel, *fb = page(), *want = malloc((size_t)FB_W * FB_H * 4);
		diatom_rect pic = { 136, 72, 480, 432 };
		diatom_bezel b;
		int bad_bg = 0, bad = 0, written = 0;

		for (int i = 0; i < FB_W * FB_H; i++) {
			int x = i % FB_W, y = i / FB_W;
			uint32_t v = rnd();
			bool window = x >= pic.x + 4 && x < pic.x + pic.w - 4 && y >= pic.y + 4 && y < pic.y + pic.h - 4;

			rgba[4 * i] = v; rgba[4 * i + 1] = v >> 8; rgba[4 * i + 2] = v >> 16;
			rgba[4 * i + 3] = window ? ((v >> 24) < 200 ? 0 : v >> 24) : (v >> 24) < 40 ? 255 : (v >> 24) < 60 ? 0 : v >> 24;
		}
		memset(&b, 0, sizeof b);
		CHECK(!diatom_bezel_prepare(&b, rgba, 640, 480, panel, pic, FB_W, FB_H), "an image not the panel's size is refused");
		CHECK(diatom_bezel_prepare(&b, rgba, FB_W, FB_H, panel, pic, FB_W, FB_H), "prepare");
		diatom_bezel_fill(&b, fb, FB_W);
		for (int y = 0; y < FB_H; y++)
			for (int x = 0; x < FB_W; x++) {
				const uint8_t *s = rgba + ((size_t)y * FB_W + x) * 4;
				uint32_t p = 0;

				for (int c = 0; c < 3; c++)
					p |= (uint32_t)((s[c] * s[3] + ch(panel, c) * (255 - s[3]) + 127) / 255) << (16 - 8 * c);
				want[y * FB_W + x] = p;
				bad_bg += (fb[(FB_H - 1 - y) * FB_W + (FB_W - 1 - x)] & 0xFFFFFF) != p;
			}
		CHECK(!bad_bg, "the page: the panel colour with the lens over it, turned (%d pixels wrong)", bad_bg);
		/* The picture, then the lens again where it covers the picture. */
		for (int y = pic.y; y < pic.y + pic.h; y++)
			for (int x = pic.x; x < pic.x + pic.w; x++) {
				uint32_t v = rnd() & 0xFFFFFF, p = 0;
				const uint8_t *s = rgba + ((size_t)y * FB_W + x) * 4;

				fb[(FB_H - 1 - y) * FB_W + (FB_W - 1 - x)] = 0xFF000000u | v;
				for (int c = 0; c < 3; c++)
					p |= (uint32_t)((s[c] * s[3] + ch(v, c) * (255 - s[3]) + 127) / 255) << (16 - 8 * c);
				want[y * FB_W + x] = s[3] ? p : v;
				written += s[3] != 0;
			}
		diatom_bezel_reblend(&b, fb, FB_W);
		for (int y = 0; y < FB_H; y++)
			for (int x = 0; x < FB_W; x++) bad += (fb[(FB_H - 1 - y) * FB_W + (FB_W - 1 - x)] & 0xFFFFFF) != want[y * FB_W + x];
		CHECK(!bad, "the lens over the picture's edge, and nothing else touched (%d pixels wrong)", bad);
		{
			int n = 0;
			for (int i = 0; i < b.nspan; i++) n += b.span[i].len;
			CHECK(n == written, "the spans hold the %d pixels with alpha over the picture, and only them (%d)", written, n);
		}
		diatom_bezel_free(&b);
		diatom_bezel_free(&b);
		free(rgba);
		free(fb);
		free(want);
	}

	if (failures) { printf("fliplook: %d FAILED\n", failures); return 1; }
	printf("fliplook: ok\n");
	return 0;
}
