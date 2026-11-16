/* SPDX-License-Identifier: MIT */
/* Not upstream's: the one translation unit that holds stb_image's
 * implementation, compiled on its own flags like stb_image_write.c
 * (vendor/stb/README.md). PNG only, from memory - the other decoders and
 * stdio are compiled out. */
#include <stddef.h>
#include <stdlib.h>

/* No single request stb makes for a 752x560 PNG of at most 4 MB
 * (DIATOM_BEZEL_FILE_MAX) needs more than 8 MiB:
 *   the IDAT chunks, gathered into one buffer that doubles: under twice the
 *     4 MB file;
 *   the inflated scanlines, guessed as the 8-bit or 16-bit image plus a
 *     filter byte a row, and doubling when the data runs past the guess:
 *     Adam7's 1050 filter bytes against the guess's 560 push 16-bit RGBA from
 *     3,369,520 to 6,739,040;
 *   the pixels, 752*560*8 = 3,368,960 at 16 bits, the two filter scanlines
 *     12,032.
 * stb otherwise asks for what a file declares - 1 GiB for one IDAT chunk's
 * length, or a buffer that doubles for as long as the deflate stream keeps
 * inflating - and the device has 128 MB. A request over the ceiling fails as
 * stb's own out of memory. */
#define STBI_DIATOM_ALLOC_MAX ((size_t)8 << 20)

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
