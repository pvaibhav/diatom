/* SPDX-License-Identifier: MIT */
/* The page the Flip's Look draws for one frame (ADR-9002), written upright as
 * a PNG, through the port's own renderer and bezel code (port/miyoo_look.c):
 *
 *   lookshot [--rgb565] FRAME.png LOOK WxH+X+Y OUT.png [BEZEL.png]
 *
 * LOOK is a screen's name (dots-dmg, dots-pocket, dots-light, grid-gbc,
 * grid-gba); the rect is the display rect the port is handed. With a bezel
 * whose file name gives this rect's size the picture moves to the name's x,y,
 * as on the device; with any other, it is drawn with no bezel, as there.
 * --rgb565 hands the renderer the frame as a core in RGB565 would. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miyoo_look.h"
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#define FB_W 752
#define FB_H 560

static uint8_t *load_png(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf = NULL, *px = NULL;
	long len;
	int n;

	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
	    (buf = malloc((size_t)len)) && fread(buf, 1, (size_t)len, f) == (size_t)len)
		px = stbi_load_from_memory(buf, (int)len, w, h, &n, 4);
	fclose(f);
	free(buf);
	return px;
}

static void put(void *ctx, void *data, int size) { fwrite(data, 1, (size_t)size, ctx); }

int main(int argc, char **argv)
{
	bool rgb565 = argc > 1 && !strcmp(argv[1], "--rgb565");
	const diatom_look_screen *s;
	diatom_rect d;
	diatom_look l;
	diatom_bezel b;
	bool bezel = false;
	char want[64];
	uint8_t *frame, *art = NULL, *rgb;
	uint32_t *fb;
	void *src;
	size_t pitch;
	int w, h, aw = 0, ah = 0;
	FILE *out;

	argv += rgb565; argc -= rgb565;
	if (argc < 5 || argc > 6 || sscanf(argv[3], "%dx%d%d%d", &d.w, &d.h, &d.x, &d.y) != 4) {
		fprintf(stderr, "usage: lookshot [--rgb565] FRAME.png LOOK WxH+X+Y OUT.png [BEZEL.png]\n");
		return 2;
	}
	snprintf(want, sizeof want, "flipfx-%s", argv[2]);
	if (!(s = diatom_look_find(want))) { fprintf(stderr, "lookshot: no screen %s\n", argv[2]); return 2; }
	if (!(frame = load_png(argv[1], &w, &h))) { fprintf(stderr, "lookshot: cannot read %s\n", argv[1]); return 1; }

	/* The frame as a core hands it over. */
	pitch = (size_t)w * (rgb565 ? 2 : 4);
	src = malloc(pitch * h);
	for (int i = 0; i < w * h; i++) {
		const uint8_t *p = frame + 4 * i;

		if (rgb565) ((uint16_t *)src)[i] = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
		else ((uint32_t *)src)[i] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
	}
	stbi_image_free(frame);

	fb = malloc((size_t)FB_W * FB_H * 4);
	for (int i = 0; i < FB_W * FB_H; i++) fb[i] = 0xFF000000u;
	memset(&b, 0, sizeof b);
	if (argc == 6) {
		const char *file = strrchr(argv[5], '/'), *dash;
		char lens[16];
		diatom_rect r;

		file = file ? file + 1 : argv[5];
		dash = strchr(file, '-');
		snprintf(lens, sizeof lens, "%.*s", dash ? (int)(dash - file) : 0, file);
		if (!diatom_bezel_parse(file, lens, &r) || r.w != d.w || r.h != d.h)
			fprintf(stderr, "lookshot: %s is not art for %dx%d: no bezel\n", file, d.w, d.h);
		else if (!(art = load_png(argv[5], &aw, &ah)) ||
		         !diatom_bezel_prepare(&b, art, aw, ah, s->panel, r, FB_W, FB_H))
			fprintf(stderr, "lookshot: %s will not do (unreadable, or not %dx%d): no bezel\n", argv[5], FB_W, FB_H);
		else {
			bezel = true;
			d = r;
			diatom_bezel_fill(&b, fb, FB_W);
		}
		if (art) stbi_image_free(art);
	}

	memset(&l, 0, sizeof l);
	if (!diatom_look_prepare(&l, s, w, h, d, FB_W, FB_H)) {
		fprintf(stderr, "lookshot: %s does not draw %dx%d into %dx%d\n", s->name, w, h, d.w, d.h);
		return 1;
	}
	diatom_look_draw(&l, src, pitch, rgb565 ? DIATOM_PIX_RGB565 : DIATOM_PIX_XRGB8888, fb, FB_W);
	if (bezel) diatom_bezel_reblend(&b, fb, FB_W);

	/* Turned back upright. */
	rgb = malloc((size_t)FB_W * FB_H * 3);
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++) {
			uint32_t p = fb[(size_t)(FB_H - 1 - y) * FB_W + (FB_W - 1 - x)];
			uint8_t *o = rgb + ((size_t)y * FB_W + x) * 3;

			o[0] = p >> 16; o[1] = p >> 8; o[2] = p;
		}
	if (!(out = fopen(argv[4], "wb")) || !stbi_write_png_to_func(put, out, FB_W, FB_H, 3, rgb, FB_W * 3)) {
		fprintf(stderr, "lookshot: cannot write %s\n", argv[4]);
		return 1;
	}
	fclose(out);
	printf("lookshot: %s %dx%d -> %dx%d%+d%+d%s: %s\n", s->name, w, h, d.w, d.h, d.x, d.y, bezel ? " with bezel" : "", argv[4]);
	diatom_look_free(&l);
	diatom_bezel_free(&b);
	free(src); free(fb); free(rgb);
	return 0;
}
