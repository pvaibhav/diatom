# stb_image_write and stb_image, vendored

`stb_image_write.h` is the screenshot hotkey's PNG encoder (`src/shot.c`,
plorpos-gkd.86.2); `stb_image.h` decodes the Flip's bezel images (ADR-9002)
and `tools/lookshot`'s frames. Neither device image ships a libpng diatom
could count on, and one header compiled in costs less than shipping and
locating a shared object on every port.

- Upstream: https://github.com/nothings/stb
- Version: `stb_image_write.h` v1.16 and `stb_image.h` v2.30, both at commit
  `2c980bb59875b0d32144a71867fbdebb2f77cd20`, unmodified.
- **Public domain (Unlicense) or MIT**, the reader's choice - the license text
  is at the end of each header. Taken as MIT here, like the rest of diatom's
  permissive code; brings no copyleft in.

`stb_image_write.c` is this repo's, not upstream's: the single translation unit
that defines `STB_IMAGE_WRITE_IMPLEMENTATION`, compiled on its own flags (the
header is not written to this project's warnings) and with stdio compiled out -
`shot.c` writes through `stbi_write_png_to_func` so it can sync before the
rename.

`stb_image.c` is the same for the decoder, on the same terms: PNG only
(`STBI_ONLY_PNG`), from memory (`STBI_NO_STDIO`), no float API
(`STBI_NO_LINEAR`). Only the Flip's build links it.
