/* SPDX-License-Identifier: MIT */
/* What does the Flip's CPU scaling cost a frame (ADR-9001)?
 *
 * Times port/miyoo_scale.c for the systems' frame sizes and the rects Aspect,
 * Stretch and Integer give on the Flip's 752x560 panel (src/scale.c, as
 * diatom computes them), and prints ms a frame, median and worst:
 *   - the CPU scaler with each filter. diatom uses it for Integer (nearest),
 *     for Crisp at whole factors (nearest), and for Crisp and Soft only when
 *     the blitter is missing. Its destination is malloc'd memory unless --fb
 *     is given; then it is a page of /dev/fb0 that is not on glass, which is
 *     what diatom writes to, with fb0's own caching. Nothing is panned, so
 *     the screen does not change.
 *   - Crisp's upscale, the CPU half of Crisp, into malloc'd memory (diatom
 *     writes the vendor's MMA buffer, which this cannot reach). The blit that
 *     scales it down to the rect is not included.
 *
 *   scalebench [--fb] [--frames N] [--plain]
 *
 * --plain times the plain C loops instead of NEON.
 * Built for the device with
 *   docker run --rm -v "$PWD:/work" -w /work miyoo-toolchain make PORT=miyoo scalebench
 * statically linked: the binary is all it needs. Times from any other CPU say
 * nothing about the Flip's. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fb.h>
#endif

#include "diatom.h"
#include "miyoo_scale.h"

#define PANEL_W 752
#define PANEL_H 560

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int by_value(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* The median and worst of t[0..frames), as a column. */
static void report(double *t, int frames)
{
	qsort(t, (size_t)frames, sizeof *t, by_value);
	printf(" %7.2f / %5.2f", t[frames / 2], t[frames - 1]);
}

/* The page to write: fb0's first page that is not on glass, or a malloc'd one. */
static uint32_t *destination(bool fb, size_t *pitch_px)
{
#ifdef __linux__
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint8_t *mem;
	unsigned pages, shown;
	int fd;
#endif

	*pitch_px = PANEL_W;
	if (!fb) return calloc((size_t)PANEL_W * PANEL_H, 4);
#ifdef __linux__
	if ((fd = open("/dev/fb0", O_RDWR)) < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
		perror("/dev/fb0");
		return NULL;
	}
	if (var.xres != PANEL_W || var.yres != PANEL_H || var.bits_per_pixel != 32 || var.yres_virtual < 2 * var.yres) {
		fprintf(stderr, "fb0 is %ux%u, %u bpp, %u rows: not the Flip's\n",
		        var.xres, var.yres, var.bits_per_pixel, var.yres_virtual);
		return NULL;
	}
	if ((mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		perror("mmap fb0");
		return NULL;
	}
	pages = var.yres_virtual / var.yres;
	shown = var.yoffset / var.yres;
	*pitch_px = fix.line_length / 4;
	return (uint32_t *)(mem + (size_t)((shown + 1) % pages) * var.yres * fix.line_length);
#else
	fprintf(stderr, "--fb needs Linux's /dev/fb0\n");
	return NULL;
#endif
}

int main(int argc, char **argv)
{
	static const struct { int w, h; diatom_pixfmt fmt; double aspect; const char *what; } SRC[] = {
		{ 160, 144, DIATOM_PIX_RGB565,   10.0 / 9, "GB" },
		{ 240, 160, DIATOM_PIX_RGB565,   1.5,      "GBA" },
		{ 256, 224, DIATOM_PIX_RGB565,   1.3061,   "SNES" },
		{ 256, 240, DIATOM_PIX_XRGB8888, 4.0 / 3,  "xrgb" },
		{ 320, 224, DIATOM_PIX_RGB565,   1.4286,   "Genesis" },
	};
	static const struct { diatom_scale_mode m; const char *name; } MODE[] = {
		{ DIATOM_SCALE_ASPECT_FIT, "aspect" }, { DIATOM_SCALE_STRETCH, "stretch" },
		{ DIATOM_SCALE_INTEGER, "integer" },
	};
	bool fb = false, plain = false;
	int frames = 300;
	size_t pitch_px;
	uint32_t *dst;
	double *t;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--fb")) fb = true;
		else if (!strcmp(argv[i], "--plain")) plain = true;
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: %s [--fb] [--frames N] [--plain]\n", argv[0]);
			return 2;
		}
	}
	if (frames < 1) frames = 1;
	if (!(dst = destination(fb, &pitch_px)) || !(t = malloc(sizeof *t * (size_t)frames))) return 1;

	printf("scalebench: %d frames a case, %s loops; ms a frame, median / worst\n", frames,
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	       plain ? "plain C" : "NEON"
#else
	       "plain C (no NEON on this CPU)"
#endif
	       );
	printf("CPU scaler into %s; Crisp upscale into malloc'd memory, without the blit after it\n",
	       fb ? "a hidden fb0 page" : "malloc'd memory");
	printf("%-17s %-8s %-14s %15s %15s %15s %20s\n", "source", "mode", "rect",
	       "CPU nearest", "CPU Crisp", "CPU Soft", "Crisp upscale");
	for (size_t i = 0; i < sizeof SRC / sizeof *SRC; i++) {
		int bpp = SRC[i].fmt == DIATOM_PIX_RGB565 ? 2 : 4;
		size_t pitch = (size_t)SRC[i].w * bpp;
		uint8_t *src = malloc(pitch * SRC[i].h);
		char name[32];

		for (size_t k = 0; k < pitch * SRC[i].h; k++) src[k] = (uint8_t)(rand() >> 7);
		snprintf(name, sizeof name, "%dx%d %s", SRC[i].w, SRC[i].h, SRC[i].what);
		for (size_t m = 0; m < sizeof MODE / sizeof *MODE; m++) {
			diatom_rect r = diatom_scale_rect(MODE[m].m, SRC[i].w, SRC[i].h, SRC[i].aspect, PANEL_W, PANEL_H);
			char rect[32];

			snprintf(rect, sizeof rect, "%dx%d+%d+%d", r.w, r.h, r.x, r.y);
			printf("%-17s %-8s %-14s", name, MODE[m].name, rect);
			for (int f = 0; f < 3; f++) {
				diatom_scaler s;
				/* Integer is drawn with nearest whatever the setting (main.c,
				 * ADR-9001), so its other columns are what nearest costs. */
				diatom_filter filter = MODE[m].m == DIATOM_SCALE_INTEGER ? DIATOM_FILTER_NEAREST : (diatom_filter)f;

				memset(&s, 0, sizeof s);
				s.plain = plain;
				if (!diatom_scaler_prepare(&s, SRC[i].w, SRC[i].h, r, filter, PANEL_W, PANEL_H)) return 1;
				for (int k = 0; k < 10; k++)   /* warm the caches */
					diatom_scaler_draw(&s, src, pitch, SRC[i].fmt, dst, pitch_px);
				for (int k = 0; k < frames; k++) {
					double t0 = now_ms();
					diatom_scaler_draw(&s, src, pitch, SRC[i].fmt, dst, pitch_px);
					t[k] = now_ms() - t0;
				}
				report(t, frames);
				diatom_scaler_free(&s);
			}
			/* Crisp's upscale, where diatom does one (port/miyoo.c, choose_path). */
			{
				int kx = (r.w + SRC[i].w - 1) / SRC[i].w, ky = (r.h + SRC[i].h - 1) / SRC[i].h;
				size_t row = (size_t)SRC[i].w * kx * bpp, bytes = row * SRC[i].h * ky;

				if (MODE[m].m == DIATOM_SCALE_INTEGER)
					printf(" %20s", "-");
				else if (r.w == kx * SRC[i].w && r.h == ky * SRC[i].h)
					printf(" %20s", "whole: CPU nearest");
				else if (bytes > (size_t)PANEL_W * PANEL_H * 4)
					printf(" %14dx%d big", kx, ky);
				else {
					uint8_t *up = malloc(bytes), *line = malloc(row);

					if (!up || !line) return 1;
					for (int k = 0; k < 10; k++)
						diatom_upscale(src, SRC[i].w, SRC[i].h, pitch, SRC[i].fmt, kx, ky, up, row, line, plain);
					for (int k = 0; k < frames; k++) {
						double t0 = now_ms();
						diatom_upscale(src, SRC[i].w, SRC[i].h, pitch, SRC[i].fmt, kx, ky, up, row, line, plain);
						t[k] = now_ms() - t0;
					}
					printf("  %dx%d", kx, ky);
					report(t, frames);
					free(up);
					free(line);
				}
			}
			printf("\n");
			fflush(stdout);
		}
		free(src);
	}
	printf("integer: diatom draws whole pixels whatever the filter, so all three CPU columns are nearest\n");
	printf("Crisp upscale: the CPU half of Crisp only; the hardware blit that scales it down is not included.\n"
	       "\"big\": over a panel page, so diatom draws Soft instead\n");
	return 0;
}
