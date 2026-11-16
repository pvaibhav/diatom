/* SPDX-License-Identifier: MIT */
/* Not upstream's: the one translation unit that holds stb_image's
 * implementation, compiled on its own flags like stb_image_write.c
 * (vendor/stb/README.md). PNG only, from memory - the other decoders and
 * stdio are compiled out. */
#include <stddef.h>
#include <stdlib.h>

/* No single request stb makes for a bezel diatom_bezel_read passes - a
 * 752x560 PNG 8 bits deep, of at most 4 MB (DIATOM_BEZEL_FILE_MAX) - needs
 * more than 4 MiB when its IDAT holds what an encoder writes for it:
 *   the inflated scanlines, guessed as the image at the file's channels plus
 *     a filter byte a row, and doubling when the data runs past the guess:
 *     RGBA is the most, and Adam7's 1050 filter bytes against the guess's
 *     560 push it from 1,685,040 to 3,370,080;
 *   the IDAT chunks, gathered into one buffer that doubles: under twice
 *     their total however they are split, and RGBA's is 1,685,666 bytes
 *     stored, 1.9 MB at most in fixed Huffman codes (9 bits a byte);
 *   the pixels, palette, grey and RGB expanded to RGBA, 752*560*4 =
 *     1,684,480, an Adam7 pass less, the two filter scanlines 6,016.
 * That leaves 824,224 bytes (24%) over the scanlines, and admits any IDAT
 * total up to 2 MiB, 10% over the 1.9 MB. stb otherwise asks for what a file
 * declares - 1 GiB for one IDAT chunk's length, or a buffer that doubles for
 * as long as the deflate stream keeps inflating - and the device has 128 MB.
 * A request over the ceiling fails as stb's own out of memory. */
#define STBI_DIATOM_ALLOC_MAX ((size_t)4 << 20)

const size_t stbi_diatom_alloc_max = STBI_DIATOM_ALLOC_MAX;
size_t stbi_diatom_largest;          /* the largest allocation handed out, for the tests */

static void *bounded_realloc(void *p, size_t n)
{
	if (n > STBI_DIATOM_ALLOC_MAX) return NULL;
	if (n > stbi_diatom_largest) stbi_diatom_largest = n;
	return realloc(p, n);
}

#define STBI_MALLOC(n)                bounded_realloc(NULL, n)
#define STBI_REALLOC_SIZED(p, old, n) bounded_realloc(p, n)
#define STBI_FREE(p)                  free(p)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#include "stb_image.h"
