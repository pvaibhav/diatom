/* SPDX-License-Identifier: MIT */
/* The Flip's Look (TortOS plorpos-f1p, ADR-9002): a Game Boy, Game Boy Color
 * or Game Boy Advance picture drawn as that handheld's LCD, on the CPU, and
 * the lens image a bezel lays around it. Its own file, as miyoo_scale, so the
 * host test (test/fliplook_test.c) and tools/lookshot run the code the device
 * runs, with no framebuffer and no vendor library.
 *
 * A screen is one of five entries, found by the "flipfx-<name>" its shader
 * pass's file name carries (ADR-0041's chain, the launcher's contract):
 *   dots-dmg, dots-pocket, dots-light  the monochrome Game Boy's dots, the
 *       Authentic shader's model (ink from four shades, gaps that take a share
 *       of the ink on either side, the dots' shadow on the reflector);
 *   grid-gbc, grid-gba  a colour tone over each pixel and a darker grid.
 *
 * Cells: panel pixel x of a w-wide rect shows source pixel
 * floor((x + 0.5) * src_w / w), and the first panel pixel of each cell, on
 * each axis, is the gap: one panel pixel wide whatever the scale, the cells
 * uneven at fractional scales. The same down.
 *
 * Both renderers draw into a page of fb0 turned 180 degrees, as
 * diatom_scaler_draw does. diatom_look_prepare does every division and
 * allocation for a geometry; diatom_look_draw does none. */
#ifndef DIATOM_MIYOO_LOOK_H
#define DIATOM_MIYOO_LOOK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diatom_port.h"

typedef enum { DIATOM_LOOK_NONE, DIATOM_LOOK_DOTS, DIATOM_LOOK_GRID } diatom_look_kind;

/* The Authentic shader's constants for one Game Boy screen (authentic.py's
 * DOTS); GAP, SHADOW and the dot shadow's offset and strength are the
 * shader's own and the same for all three. */
typedef struct {
	uint8_t panel[3], ink[3];       /* the reflector lit, and the ink, 0..255 */
	float   baseline_alpha, grey_balance, contrast, screen_light, pixel_opacity, shadow_opacity;
} diatom_look_dots;

/* A colour screen: each channel c (0..255) drawn as the tone
 * t = (c_tone*a + c*(255-a) + 127) / 255, a gap as t mixed toward black by
 * ax (a column), ay (a row) or the larger (where they cross). */
typedef struct {
	uint8_t tone[3], black[3];
	uint8_t a, ax, ay;
} diatom_look_grid;

typedef struct {
	const char      *name;
	diatom_look_kind kind;
	uint32_t         panel;         /* xRGB: the lit panel with nothing drawn, the page under a bezel */
	union { diatom_look_dots dots; diatom_look_grid grid; };
} diatom_look_screen;

#define DIATOM_LOOK_SCREENS 5
extern const diatom_look_screen diatom_look_screens[DIATOM_LOOK_SCREENS];

/* The screen whose "flipfx-<name>" the file name of `path` contains, or NULL. */
const diatom_look_screen *diatom_look_find(const char *path);

/* The widest cell the dots draw: their table holds a cell's panel pixels for
 * every neighbourhood, so it grows with the square of this. Stretch on the
 * Flip is 4.7 across. */
#define DIATOM_LOOK_MAX_CELL 6

/* Whether the look draws this frame into this rect: a frame of at most
 * 240x160 at 2x or more on both axes (the contract with the launcher), and
 * for dots no cell wider than DIATOM_LOOK_MAX_CELL. Otherwise the port draws
 * the picture as it would with no look. */
bool diatom_look_fits(const diatom_look_screen *s, int src_w, int src_h, diatom_rect dst);

typedef struct {
	/* The geometry the tables were built for. */
	const diatom_look_screen *screen;
	int         src_w, src_h, fb_w, fb_h;
	diatom_rect dst;
	bool        valid;

	/* Per source column, in the rect's logical order: where its cell starts
	 * in the row buffer (memory order: the turn reverses a row) and its
	 * width; per source row, its first logical row and height. */
	uint16_t *xm, *xlen;
	uint16_t *ystart, *ylen;
	/* The visible part of a row: logical columns vx0..vx1, landing at fb
	 * column fb_col. */
	int       vx0, vx1, fb_col;
	uint32_t *row[2];               /* row buffers, dst.w plus a cell of slack */

	/* Dots: a cell's panel pixels (kstride of them, memory order) for each
	 * class of panel row, class of column and 10-bit neighbourhood key
	 * (self, left, up, right, up-right; 2 bits each, a shade). A class is a
	 * cell width, or the picture's first cell, whose gap has no neighbour. */
	uint32_t *tab;
	int       kstride, ncol;
	uint8_t  *xcls;                 /* per source column */
	uint16_t *yrow;                 /* per source row: its first table row */
	uint8_t  *shade[2];             /* two source rows of shades, one of padding each side */
	uint16_t *key;
	uint8_t   sum_shade[766];       /* r + g + b to shade */

	/* Grid: per channel and part (cell, column gap, row gap, crossing),
	 * already shifted into place; from an 8-bit channel and from RGB565's
	 * 5- and 6-bit codes. */
	uint32_t  g8[4][3][256];
	uint32_t  g5r[4][32], g6g[4][64], g5b[4][32];
} diatom_look;

/* Build the tables for this geometry; a no-op when nothing changed. False
 * when the look does not fit (diatom_look_fits) or memory runs out, and then
 * nothing is drawn. */
bool diatom_look_prepare(diatom_look *l, const diatom_look_screen *s, int src_w, int src_h,
                         diatom_rect dst, int fb_w, int fb_h);

/* Draw one frame into the rect; nothing outside it is written. `fb` is the
 * page's first pixel, `fb_pitch` its row length in pixels. */
void diatom_look_draw(diatom_look *l, const void *src, size_t pitch, diatom_pixfmt fmt,
                      uint32_t *fb, size_t fb_pitch);

void diatom_look_free(diatom_look *l);

/* ---------- bezel ---------------------------------------------------------- */

/* A lens image's file name, "<lens>-<w>x<h>+<x>+<y>.png": the picture rect it
 * was drawn for, and where that rect's top-left corner goes. False for any
 * other name, another lens's included. */
bool diatom_bezel_parse(const char *file, const char *lens, diatom_rect *rect);

typedef struct { uint16_t r, g, b, na; } diatom_bezel_px;   /* c*a + 127 per channel, and 255 - a */
typedef struct { uint16_t row, col, len; uint32_t at; } diatom_bezel_span;

typedef struct {
	int                fb_w, fb_h;
	uint32_t          *bg;          /* fb_w x fb_h, turned: the panel colour with the lens over it */
	diatom_bezel_span *span;        /* the lens's pixels inside the picture with alpha > 0, per fb row */
	diatom_bezel_px   *px;
	int                nspan;
} diatom_bezel;

/* Build the page under the picture and the spans over it from an upright
 * RGBA image with straight alpha, which must be the panel's size. `pic` is
 * the picture's rect, already moved. False when the size is wrong or memory
 * runs out; then `b` holds nothing. */
bool diatom_bezel_prepare(diatom_bezel *b, const uint8_t *rgba, int w, int h, uint32_t panel,
                          diatom_rect pic, int fb_w, int fb_h);

/* The page under the picture, where the port would otherwise clear it. */
void diatom_bezel_fill(const diatom_bezel *b, uint32_t *fb, size_t fb_pitch);

/* After the picture: the lens's edge and shadow blended over it again. */
void diatom_bezel_reblend(const diatom_bezel *b, uint32_t *fb, size_t fb_pitch);

void diatom_bezel_free(diatom_bezel *b);

#endif
