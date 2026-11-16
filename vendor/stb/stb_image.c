/* SPDX-License-Identifier: MIT */
/* Not upstream's: the one translation unit that holds stb_image's
 * implementation, compiled on its own flags like stb_image_write.c
 * (vendor/stb/README.md). PNG only, from memory - the other decoders and
 * stdio are compiled out. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#include "stb_image.h"
