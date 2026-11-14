/* SPDX-License-Identifier: MIT */
/* The Flip's CPU scaler (TortOS plorpos-f1p, ADR-0050): a core's frame scaled
 * into a page of fb0, turned 180 degrees because the panel is mounted upside
 * down. Its own file so the host tests (test/flipscale_test.c) and the
 * benchmark (tools/scalebench.c) run the code the device runs, with no
 * framebuffer and no vendor library.
 *
 * Filters, by the port interface's names:
 *   NEAREST  "Sharp": nearest neighbour.
 *   SHARP    "Crisp": nearest up to the next whole factor, then bilinear down
 *            to the rect. Equal to NEAREST at a whole factor.
 *   LINEAR   "Soft": bilinear.
 *
 * All the arithmetic per geometry (maps, weights) happens in
 * diatom_scaler_prepare; drawing a frame does no allocation and no division. */
#ifndef DIATOM_MIYOO_SCALE_H
#define DIATOM_MIYOO_SCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diatom_port.h"

typedef struct {
	/* The geometry the maps were built for. */
	int           src_w, src_h, fb_w, fb_h;
	diatom_rect   dst;
	diatom_filter filter;
	bool          valid;

	/* Visible columns (n) and rows (rows) of dst, after clipping to the panel.
	 * Columns are in MEMORY order: column 0 is the leftmost in fb memory,
	 * which is the rightmost visible logical column. */
	int       n, rows;
	int       fb_x, fb_y;     /* fb memory column of column 0; fb row of visible logical row 0 */
	int       scan_top;       /* fb row where dst's turned rect starts: scanline phase */
	uint16_t *cidx;           /* source column per output column */
	uint8_t  *cw4;            /* its weight towards cidx + 1, 0..255, repeated per byte */
	int16_t  *ridx;           /* source row per visible output row */
	uint8_t  *rw;             /* its weight towards ridx + 1 */
	bool      cblend, rblend; /* any weight on that axis is nonzero */

	/* Scratch, sized by prepare. */
	uint32_t *line;           /* a source row as xRGB, one pixel of padding */
	int16_t  *hrow[2];        /* source rows scaled across, 4 channels x n, 15-bit */
	int       hsrc[2];        /* which source row each holds, -1 for none */
	uint32_t *out;            /* the output row, before it is copied to fb rows */

	bool      plain;          /* the plain C loops even where NEON is built (tests) */
} diatom_scaler;

/* Build the maps for this geometry; a no-op when nothing changed. False only
 * when memory runs out, and then nothing is drawn. */
bool diatom_scaler_prepare(diatom_scaler *s, int src_w, int src_h, diatom_rect dst,
                           diatom_filter filter, int fb_w, int fb_h);

/* Draw one frame. `fb` is the page's first pixel, `fb_pitch` its row length
 * in pixels. With `scan`, every third fb row of the rect (the scanline
 * filter's dark rows) is skipped: the caller cleared it with the page. */
void diatom_scaler_draw(diatom_scaler *s, const void *src, size_t pitch, diatom_pixfmt fmt,
                        uint32_t *fb, size_t fb_pitch, bool scan);

void diatom_scaler_free(diatom_scaler *s);

/* RGB565 to xRGB with the top bits repeated into the bottom, so white is
 * 0xFFFFFFFF, not 0xFFF8FCF8. */
static inline uint32_t diatom_rgb565(uint16_t p)
{
	uint32_t r = p >> 11 & 31, g = p >> 5 & 63, b = p & 31;

	return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

#endif
