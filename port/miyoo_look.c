/* SPDX-License-Identifier: MIT */
/* The Flip's Look renderers and bezel (ADR-9002); see miyoo_look.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miyoo_look.h"

#define DOT_GAP    0.38     /* share of each neighbour's ink a gap between dots takes */
#define DOT_SHADOW 0.30     /* share of a dot's ink its shadow carries */

#define XRGB(r, g, b) (0xFF000000u | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))

const diatom_look_screen diatom_look_screens[DIATOM_LOOK_SCREENS] = {
	/* Harlequin's dmg-3x. */
	{ "dots-dmg", DIATOM_LOOK_DOTS, XRGB(137, 137, 0),
	  .dots = { { 161, 161, 0 }, { 0, 90, 170 }, 0.05f, 2.6f, 0.7f, 0.85f, 0.9f, 0.9f } },
	/* gb-pocket-3x. */
	{ "dots-pocket", DIATOM_LOOK_DOTS, XRGB(149, 155, 119),
	  .dots = { { 166, 172, 132 }, { 18, 25, 34 }, 0.03f, 3.5f, 0.8f, 0.9f, 0.95f, 0.9f } },
	/* The Pocket's, with the panel and ink that put its white and black on TWB64's Light. */
	{ "dots-light", DIATOM_LOOK_DOTS, XRGB(0, 183, 130),
	  .dots = { { 0, 203, 144 }, { 0, 101, 80 }, 0.03f, 3.5f, 0.8f, 0.9f, 0.95f, 0.9f } },
	/* Fitted to the Authentic colour shaders over the core's colour-corrected picture,
	 * the grid's darkening theirs (lcd2.py, the approved mock-ups). */
	{ "grid-gbc", DIATOM_LOOK_GRID, XRGB(172, 189, 174),
	  .grid = { { 35, 80, 41 }, { 14, 25, 28 }, 96, 87, 147 } },
	{ "grid-gba", DIATOM_LOOK_GRID, XRGB(143, 149, 155),
	  .grid = { { 42, 54, 65 }, { 25, 27, 30 }, 134, 61, 25 } },
};

const diatom_look_screen *diatom_look_find(const char *path)
{
	const char *file;
	char want[32];

	if (!path) return NULL;
	file = strrchr(path, '/');
	file = file ? file + 1 : path;
	for (int i = 0; i < DIATOM_LOOK_SCREENS; i++) {
		snprintf(want, sizeof want, "flipfx-%s", diatom_look_screens[i].name);
		if (strstr(file, want)) return &diatom_look_screens[i];
	}
	return NULL;
}

/* The source pixel's cell on one axis: where it starts and how wide it is. */
static void axis(int n_src, int n_dst, uint16_t *start, uint16_t *len)
{
	int i = 0;

	for (int x = 0; x < n_dst; x++) {
		int s = (int)(((int64_t)(2 * x + 1) * n_src) / (2 * n_dst));
		while (i <= s) start[i++] = (uint16_t)x;
	}
	start[n_src] = (uint16_t)n_dst;
	for (i = 0; i < n_src; i++) len[i] = (uint16_t)(start[i + 1] - start[i]);
}

static int widest(int n_src, int n_dst)
{
	return (n_dst + n_src - 1) / n_src;
}

bool diatom_look_fits(const diatom_look_screen *s, int src_w, int src_h, diatom_rect dst)
{
	if (!s || src_w <= 0 || src_h <= 0 || src_w > 240 || src_h > 160) return false;
	if (dst.w < 2 * src_w || dst.h < 2 * src_h || dst.w > UINT16_MAX || dst.h > UINT16_MAX) return false;
	if (s->kind == DIATOM_LOOK_DOTS &&
	    (widest(src_w, dst.w) > DIATOM_LOOK_MAX_CELL || widest(src_h, dst.h) > DIATOM_LOOK_MAX_CELL))
		return false;
	return s->kind != DIATOM_LOOK_NONE;
}

void diatom_look_free(diatom_look *l)
{
	free(l->xm); free(l->xlen); free(l->ystart); free(l->ylen);
	free(l->row[0]); free(l->row[1]);
	free(l->tab); free(l->xcls); free(l->yrow); free(l->shade[0]); free(l->key);
	memset(l, 0, sizeof *l);
}

/* ---------- dots ---------------------------------------------------------- */

static double ink_of_grey(const diatom_look_dots *d, double grey)
{
	double a = 3.0 / d->grey_balance * (1.0 - grey) + d->baseline_alpha;

	return a < 1.0 ? a : 1.0;
}

static double ink_of(const diatom_look_dots *d, int shade) { return ink_of_grey(d, 1.0 - shade / 3.0); }

/* Panel pixel (k, r) of a cell lx by ly, k and r counted from its top-left,
 * for the neighbourhood `key`. A cell on the picture's left or top edge has
 * no neighbour for its gap to take ink from. The shadow samples grey half a
 * game pixel right and up, bilinearly, so it ramps across the cell from this
 * dot's shade to its neighbours': at three or four panel pixels a cell, a
 * whole pixel's offset left a hard-edged copy of the dot, not a shadow. */
static uint32_t dots_pixel(const diatom_look_dots *d, unsigned key, int k, int lx, bool x_edge,
                           int r, int ly, bool y_edge)
{
	int self = key & 3, left = key >> 2 & 3, up = key >> 4 & 3, right = key >> 6 & 3, ur = key >> 8 & 3;
	double a, fx = (k + 0.5) / lx, fy = (r + 0.5) / ly, wx, wy, grey, shadow;
	uint32_t out = 0xFF000000u;

	if (k == 0 && r == 0) a = 0.0;
	else if (k == 0) a = DOT_GAP * (ink_of(d, self) + (x_edge ? 0.0 : ink_of(d, left)));
	else if (r == 0) a = DOT_GAP * (ink_of(d, self) + (y_edge ? 0.0 : ink_of(d, up)));
	else a = ink_of(d, self);
	wx = 1.0 - fx;
	wy = fy;
	grey = 1.0 - (wy * (wx * self + (1 - wx) * right) + (1 - wy) * (wx * up + (1 - wx) * ur)) / 3.0;
	shadow = ink_of_grey(d, grey) * d->contrast * d->shadow_opacity * DOT_SHADOW * (1.0 - 0.75 * a);
	if (shadow > 1.0) shadow = 1.0;
	for (int c = 0; c < 3; c++) {
		double v = d->ink[c] / 255.0 * a * (1.0 - d->contrast * a * a) +
		           d->panel[c] / 255.0 * (1.0 - shadow) * (d->screen_light - d->contrast * d->pixel_opacity * a);
		int q = (int)(v * 255.0 + 0.5);

		out |= (uint32_t)(q < 0 ? 0 : q > 255 ? 255 : q) << (16 - 8 * c);
	}
	return out;
}

/* A cell class per source index on one axis: 0 for the first cell, then one
 * for each width the others have (two at most). Returns the class count and
 * each class's width in `clen`. */
static int classes(const uint16_t *len, int n, uint8_t *cls, int clen[3])
{
	int lo = 0, hi = 0;

	for (int i = 1; i < n; i++) {
		if (!lo || len[i] < lo) lo = len[i];
		if (len[i] > hi) hi = len[i];
	}
	clen[0] = len[0];
	if (cls) cls[0] = 0;
	if (n == 1) return 1;
	clen[1] = lo;
	clen[2] = hi;
	for (int i = 1; i < n; i++)
		if (cls) cls[i] = (uint8_t)(1 + len[i] - lo);
	return hi > lo ? 3 : 2;
}

static bool dots_prepare(diatom_look *l)
{
	const diatom_look_dots *d = &l->screen->dots;
	int sw = l->src_w, sh = l->src_h, ycl[3], xcl[3], ny, nrows = 0, base[3];
	uint8_t *ycls = malloc((size_t)sh);
	size_t per_row;

	if (!ycls) return false;
	l->xcls = malloc((size_t)sw);
	l->yrow = malloc((size_t)sh * sizeof *l->yrow);
	l->shade[0] = calloc((size_t)(sw + 2) * 3, 1);
	l->key = malloc((size_t)sw * sizeof *l->key);
	if (!l->xcls || !l->yrow || !l->shade[0] || !l->key) { free(ycls); return false; }
	l->shade[1] = l->shade[0] + (sw + 2);

	l->ncol = classes(l->xlen, sw, l->xcls, xcl);
	ny = classes(l->ylen, sh, ycls, ycl);
	for (int c = 0; c < ny; c++) { base[c] = nrows; nrows += ycl[c]; }
	for (int y = 0; y < sh; y++) l->yrow[y] = (uint16_t)base[ycls[y]];
	free(ycls);
	l->kstride = 0;
	for (int c = 0; c < l->ncol; c++)
		if (xcl[c] > l->kstride) l->kstride = xcl[c];

	per_row = (size_t)l->ncol * 1024 * l->kstride;
	l->tab = malloc((size_t)nrows * per_row * sizeof *l->tab);
	if (!l->tab) return false;
	for (int yc = 0; yc < ny; yc++)
		for (int r = 0; r < ycl[yc]; r++) {
			uint32_t *t = l->tab + (size_t)(base[yc] + r) * per_row;

			for (int xc = 0; xc < l->ncol; xc++)
				for (unsigned key = 0; key < 1024; key++) {
					uint32_t *e = t + ((size_t)xc * 1024 + key) * l->kstride;
					int lx = xcl[xc];

					/* Memory order: the turn puts the cell's gap last. */
					for (int k = 0; k < lx; k++)
						e[lx - 1 - k] = dots_pixel(d, key, k, lx, xc == 0, r, ycl[yc], yc == 0);
					for (int k = lx; k < l->kstride; k++) e[k] = e[lx - 1];
				}
		}
	for (int s = 0; s < 766; s++) l->sum_shade[s] = (uint8_t)(3 - (2 * s + 255) / 510);
	return true;
}

static inline unsigned e5(unsigned v) { return v << 3 | v >> 2; }
static inline unsigned e6(unsigned v) { return v << 2 | v >> 4; }

static void dots_shades(const diatom_look *l, const void *row, diatom_pixfmt fmt, uint8_t *out)
{
	if (fmt == DIATOM_PIX_RGB565) {
		const uint16_t *p = row;

		for (int x = 0; x < l->src_w; x++)
			out[x] = l->sum_shade[e5(p[x] >> 11) + e6(p[x] >> 5 & 63) + e5(p[x] & 31)];
	} else {
		const uint32_t *p = row;

		for (int x = 0; x < l->src_w; x++)
			out[x] = l->sum_shade[(p[x] >> 16 & 255) + (p[x] >> 8 & 255) + (p[x] & 255)];
	}
}

static void dots_draw(diatom_look *l, const uint8_t *src, size_t pitch, diatom_pixfmt fmt,
                      uint32_t *fb, size_t fb_pitch)
{
	const size_t per_row = (size_t)l->ncol * 1024 * l->kstride, kbytes = (size_t)l->kstride * 4;
	const size_t n = (size_t)(l->vx1 - l->vx0) * 4;
	const int sw = l->src_w, from = l->dst.w - l->vx1;
	const uint8_t *zero = l->shade[0] + 2 * (sw + 2) + 1;   /* the row above the picture: shade 0 */
	uint32_t *o = l->row[0];

	for (int y = 0; y < l->src_h; y++) {
		uint8_t *cur = l->shade[y & 1] + 1;
		const uint8_t *up = y ? l->shade[(y + 1) & 1] + 1 : zero;
		int y0 = l->dst.y + l->ystart[y], n_r = l->ylen[y];

		dots_shades(l, src + (size_t)y * pitch, fmt, cur);
		if (y0 + n_r <= 0 || y0 >= l->fb_h) continue;
		/* cur[-1] and cur[sw] stay 0: past the picture is shade 0. */
		for (int x = 0; x < sw; x++)
			l->key[x] = (uint16_t)(cur[x] | cur[x - 1] << 2 | up[x] << 4 | cur[x + 1] << 6 | up[x + 1] << 8);
		for (int r = 0; r < n_r; r++) {
			const uint32_t *t = l->tab + (size_t)(l->yrow[y] + r) * per_row;

			if (y0 + r < 0 || y0 + r >= l->fb_h) continue;
			/* Lowest memory first: each cell's slack is the next cell's start. */
			for (int x = sw - 1; x >= 0; x--)
				memcpy(o + l->xm[x], t + ((size_t)l->xcls[x] * 1024 + l->key[x]) * l->kstride, kbytes);
			memcpy(fb + (size_t)(l->fb_h - 1 - (y0 + r)) * fb_pitch + l->fb_col, o + from, n);
		}
	}
}

/* ---------- grid ---------------------------------------------------------- */

static void grid_prepare(diatom_look *l)
{
	const diatom_look_grid *g = &l->screen->grid;
	const unsigned mix[4] = { 0, g->ax, g->ay, g->ax > g->ay ? g->ax : g->ay };

	for (int part = 0; part < 4; part++)
		for (int ch = 0; ch < 3; ch++)
			for (unsigned c = 0; c < 256; c++) {
				unsigned t = (g->tone[ch] * g->a + c * (255u - g->a) + 127) / 255;
				unsigned v = (g->black[ch] * mix[part] + t * (255u - mix[part]) + 127) / 255;

				l->g8[part][ch][c] = v << (16 - 8 * ch) | (ch == 0 ? 0xFF000000u : 0);
			}
	for (int part = 0; part < 4; part++) {
		for (unsigned c = 0; c < 32; c++) {
			l->g5r[part][c] = l->g8[part][0][e5(c)];
			l->g5b[part][c] = l->g8[part][2][e5(c)];
		}
		for (unsigned c = 0; c < 64; c++) l->g6g[part][c] = l->g8[part][1][e6(c)];
	}
}

static void grid_draw(diatom_look *l, const uint8_t *src, size_t pitch, diatom_pixfmt fmt,
                      uint32_t *fb, size_t fb_pitch)
{
	const size_t n = (size_t)(l->vx1 - l->vx0) * 4;
	const int from = l->dst.w - l->vx1;
	uint32_t *gap = l->row[0], *cell = l->row[1];

	for (int y = 0; y < l->src_h; y++) {
		int y0 = l->dst.y + l->ystart[y], n_r = l->ylen[y];

		if (y0 + n_r <= 0 || y0 >= l->fb_h) continue;
		for (int x = l->src_w - 1; x >= 0; x--) {
			uint32_t c[4];
			int w = l->xlen[x];
			uint32_t *a = gap + l->xm[x], *b = cell + l->xm[x];

			if (fmt == DIATOM_PIX_RGB565) {
				unsigned p = ((const uint16_t *)(src + (size_t)y * pitch))[x], r = p >> 11, g = p >> 5 & 63, bl = p & 31;

				for (int k = 0; k < 4; k++) c[k] = l->g5r[k][r] | l->g6g[k][g] | l->g5b[k][bl];
			} else {
				uint32_t p = ((const uint32_t *)(src + (size_t)y * pitch))[x];

				for (int k = 0; k < 4; k++)
					c[k] = l->g8[k][0][p >> 16 & 255] | l->g8[k][1][p >> 8 & 255] | l->g8[k][2][p & 255];
			}
			for (int k = 0; k < w - 1; k++) { a[k] = c[2]; b[k] = c[0]; }
			a[w - 1] = c[3];
			b[w - 1] = c[1];
		}
		for (int r = 0; r < n_r; r++) {
			if (y0 + r < 0 || y0 + r >= l->fb_h) continue;
			memcpy(fb + (size_t)(l->fb_h - 1 - (y0 + r)) * fb_pitch + l->fb_col, (r ? cell : gap) + from, n);
		}
	}
}

/* ---------- both ---------------------------------------------------------- */

bool diatom_look_prepare(diatom_look *l, const diatom_look_screen *s, int src_w, int src_h,
                         diatom_rect dst, int fb_w, int fb_h)
{
	bool ok;

	if (l->valid && l->screen == s && l->src_w == src_w && l->src_h == src_h && l->fb_w == fb_w &&
	    l->fb_h == fb_h && !memcmp(&l->dst, &dst, sizeof dst))
		return true;
	diatom_look_free(l);
	if (!diatom_look_fits(s, src_w, src_h, dst) || fb_w <= 0 || fb_h <= 0 || fb_w > INT16_MAX || fb_h > INT16_MAX)
		return false;
	/* A mode crops a little at most. Twice the panel keeps every cell start
	 * and length in the maps' 16 bits and the arithmetic on dst.x and dst.y
	 * in an int. */
	if (dst.w > 2 * fb_w || dst.h > 2 * fb_h || dst.x < -dst.w || dst.x > fb_w || dst.y < -dst.h || dst.y > fb_h)
		return false;
	l->screen = s;
	l->src_w = src_w; l->src_h = src_h; l->dst = dst; l->fb_w = fb_w; l->fb_h = fb_h;

	l->xm = malloc((size_t)(src_w + 1) * sizeof *l->xm);
	l->xlen = malloc((size_t)src_w * sizeof *l->xlen);
	l->ystart = malloc((size_t)(src_h + 1) * sizeof *l->ystart);
	l->ylen = malloc((size_t)src_h * sizeof *l->ylen);
	/* A cell of slack: the dots write whole table entries. */
	l->row[0] = malloc((size_t)(dst.w + widest(src_w, dst.w)) * 4);
	l->row[1] = malloc((size_t)dst.w * 4);
	if (!l->xm || !l->xlen || !l->ystart || !l->ylen || !l->row[0] || !l->row[1]) goto fail;
	axis(src_w, dst.w, l->xm, l->xlen);
	for (int x = 0; x < src_w; x++) l->xm[x] = (uint16_t)(dst.w - l->xm[x] - l->xlen[x]);
	axis(src_h, dst.h, l->ystart, l->ylen);

	l->vx0 = dst.x < 0 ? -dst.x : 0;
	l->vx1 = fb_w - dst.x < dst.w ? fb_w - dst.x : dst.w;
	if (l->vx1 < l->vx0) l->vx1 = l->vx0;
	l->fb_col = fb_w - dst.x - l->vx1;

	ok = true;
	if (s->kind == DIATOM_LOOK_DOTS) ok = dots_prepare(l);
	else grid_prepare(l);
	if (!ok) goto fail;
	l->valid = true;
	return true;
fail:
	diatom_look_free(l);
	return false;
}

void diatom_look_draw(diatom_look *l, const void *src, size_t pitch, diatom_pixfmt fmt,
                      uint32_t *fb, size_t fb_pitch)
{
	if (!l->valid || !src || l->vx1 <= l->vx0) return;
	if (l->screen->kind == DIATOM_LOOK_DOTS) dots_draw(l, src, pitch, fmt, fb, fb_pitch);
	else grid_draw(l, src, pitch, fmt, fb, fb_pitch);
}

/* ---------- bezel --------------------------------------------------------- */

static const char *number(const char *p, int *out)
{
	long v = 0;

	if (*p < '0' || *p > '9') return NULL;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p++ - '0');
		if (v > 0xFFFF) return NULL;
	}
	*out = (int)v;
	return p;
}

bool diatom_bezel_parse(const char *file, const char *lens, diatom_rect *rect)
{
	size_t n = strlen(lens);
	const char *p = file;
	diatom_rect r;

	if (!n || strncmp(p, lens, n) != 0 || p[n] != '-') return false;
	p += n + 1;
	if (!(p = number(p, &r.w)) || *p++ != 'x' || !(p = number(p, &r.h)) || *p++ != '+' ||
	    !(p = number(p, &r.x)) || *p++ != '+' || !(p = number(p, &r.y)) || strcmp(p, ".png") != 0)
		return false;
	if (r.w <= 0 || r.h <= 0) return false;
	*rect = r;
	return true;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint8_t *diatom_bezel_read(const char *path, int fb_w, int fb_h, size_t *len, const char **why)
{
	/* The signature, then IHDR's length and type, then its width, height
	 * and bit depth. */
	static const uint8_t png_head[16] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
	uint8_t *png = NULL;
	FILE *f = fopen(path, "rb");
	long n;

	*why = "unreadable";
	if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) goto out;
	*why = "larger than 4 MB";
	if (n > DIATOM_BEZEL_FILE_MAX) goto out;
	*why = "not a PNG";
	if (n < (long)sizeof png_head + 9) goto out;
	*why = "unreadable, or out of memory";
	if (!(png = malloc((size_t)n)) || fread(png, 1, (size_t)n, f) != (size_t)n) goto refuse;
	/* Checked in the bytes the decoder is handed, not in an earlier read the
	 * file could have changed after. */
	*why = "not a PNG";
	if (memcmp(png, png_head, sizeof png_head) != 0) goto refuse;
	*why = "not the panel's size";
	if (fb_w <= 0 || fb_h <= 0 || be32(png + 16) != (uint32_t)fb_w || be32(png + 20) != (uint32_t)fb_h) goto refuse;
	/* stb leaks the 16-bit pixels when their conversion to 8 bits finds no
	 * memory, and no bezel needs more than 8. */
	*why = "not 8 bits deep";
	if (png[24] != 8) goto refuse;
	*len = (size_t)n;
	*why = NULL;
	goto out;
refuse:
	free(png);
	png = NULL;
out:
	if (f) fclose(f);
	return png;
}

void diatom_bezel_free(diatom_bezel *b)
{
	free(b->bg);
	free(b->span);
	free(b->px);
	memset(b, 0, sizeof *b);
}

/* Whether logical pixel (lx, ly) is one the picture covers and the lens is
 * not clear over. */
static bool over_picture(const uint8_t *rgba, int w, diatom_rect pic, int lx, int ly)
{
	return lx >= pic.x && lx < pic.x + pic.w && ly >= pic.y && ly < pic.y + pic.h &&
	       rgba[((size_t)ly * w + lx) * 4 + 3] != 0;
}

bool diatom_bezel_prepare(diatom_bezel *b, const uint8_t *rgba, int w, int h, uint32_t panel,
                          diatom_rect pic, int fb_w, int fb_h)
{
	const unsigned pr = panel >> 16 & 255, pg = panel >> 8 & 255, pb = panel & 255;
	size_t npx = 0;
	int nspan = 0;

	diatom_bezel_free(b);
	if (!rgba || w != fb_w || h != fb_h || fb_w <= 0 || fb_h <= 0 || fb_w > 0xFFFF || fb_h > 0xFFFF)
		return false;
	b->fb_w = fb_w; b->fb_h = fb_h;
	if (!(b->bg = malloc((size_t)fb_w * fb_h * 4))) goto fail;
	for (int fy = 0; fy < fb_h; fy++)
		for (int fx = 0; fx < fb_w; fx++) {
			int lx = fb_w - 1 - fx, ly = fb_h - 1 - fy;
			const uint8_t *s = rgba + ((size_t)ly * w + lx) * 4;
			unsigned a = s[3], na = 255 - a;

			b->bg[(size_t)fy * fb_w + fx] = XRGB((s[0] * a + pr * na + 127) / 255, (s[1] * a + pg * na + 127) / 255,
			                                     (s[2] * a + pb * na + 127) / 255);
			if (over_picture(rgba, w, pic, lx, ly)) {
				npx++;
				nspan += fx == 0 || !over_picture(rgba, w, pic, lx + 1, ly);
			}
		}
	if (npx) {
		b->span = malloc((size_t)nspan * sizeof *b->span);
		b->px = malloc(npx * sizeof *b->px);
		if (!b->span || !b->px) goto fail;
	}
	npx = 0;
	for (int fy = 0; fy < fb_h; fy++)
		for (int fx = 0; fx < fb_w; fx++) {
			int lx = fb_w - 1 - fx, ly = fb_h - 1 - fy;
			const uint8_t *s = rgba + ((size_t)ly * w + lx) * 4;

			if (!over_picture(rgba, w, pic, lx, ly)) continue;
			if (fx == 0 || !over_picture(rgba, w, pic, lx + 1, ly))
				b->span[b->nspan++] = (diatom_bezel_span){ (uint16_t)fy, (uint16_t)fx, 0, (uint32_t)npx };
			b->span[b->nspan - 1].len++;
			b->px[npx++] = (diatom_bezel_px){ (uint16_t)(s[0] * s[3] + 127), (uint16_t)(s[1] * s[3] + 127),
			                                  (uint16_t)(s[2] * s[3] + 127), (uint16_t)(255 - s[3]) };
		}
	return true;
fail:
	diatom_bezel_free(b);
	return false;
}

void diatom_bezel_fill(const diatom_bezel *b, uint32_t *fb, size_t fb_pitch)
{
	if (!b->bg) return;
	for (int y = 0; y < b->fb_h; y++)
		memcpy(fb + (size_t)y * fb_pitch, b->bg + (size_t)y * b->fb_w, (size_t)b->fb_w * 4);
}

void diatom_bezel_reblend(const diatom_bezel *b, uint32_t *fb, size_t fb_pitch)
{
	for (int i = 0; i < b->nspan; i++) {
		const diatom_bezel_span *s = &b->span[i];
		const diatom_bezel_px *p = b->px + s->at;
		uint32_t *d = fb + (size_t)s->row * fb_pitch + s->col;

		for (int k = 0; k < s->len; k++) {
			uint32_t v = d[k];

			d[k] = XRGB((p[k].r + (v >> 16 & 255) * p[k].na) / 255, (p[k].g + (v >> 8 & 255) * p[k].na) / 255,
			            (p[k].b + (v & 255) * p[k].na) / 255);
		}
	}
}
