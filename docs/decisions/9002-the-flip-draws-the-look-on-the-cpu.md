# 9002. On the Miyoo Mini Flip, draw the Game Boy, Color and Advance Look and its bezel on the CPU, picked by the shader chain's names

- **Status:** Accepted
- **Date:** 2026-11-16
- **Supersedes:** -
- **Superseded by:** -

## Context

The launcher gives Game Boy, Game Boy Color and Game Boy Advance games a **Look** row on the Flip: Clean (today's
picture), Retro (the handheld's LCD) and Retro with Bezels (Retro inside a lens image that may move the picture). The
launcher's contract with diatom fixes the wire, the screens and the bezel files; this record is diatom's side of it.

Retro on the other ports is a GLSL shader. The Flip has no GPU (ADR-9001). Its vendor blitter scales, converts and
turns frames and can alpha-blend one surface over another, and that is all it can do.

The two kinds of LCD differ in what a panel pixel depends on:

- A monochrome Game Boy screen is the Authentic shader's model: dots of ink from four shades, gaps between dots that
  take a share of the ink on either side, and each dot's shadow on the reflector below and to the left. A panel pixel
  depends on its game pixel and four neighbours.
- A colour screen is a tone over each game pixel and a darker grid between them. A panel pixel depends on its game
  pixel alone.

A spike benchmark drew each look on the Flip into memory, 400 frames of noise, medians in ms:

| | Integer | Aspect |
|---|---|---|
| Game Boy dots | 2.27 | 3.42 |
| Game Boy Color grid | 1.72 | 2.08 |
| Game Boy Advance grid | 2.90 | 2.88 |
| Game Boy Advance, plain nearest, the same row builder | 2.08 | 2.02 |

The Soft blit those games use today takes 5.2 ms.

## Options considered

### Option A - the blitter's picture with a static overlay
The blitter scales the frame and blends a fixed ARGB image over it: the tone and the grid. This draws the colour grid,
but not dots: a dot's shadow and a gap's ink depend on the picture, which a fixed image cannot know.

### Option B - the CPU draws the look, the blitter blends the bezel
The look on the CPU, then a hardware alpha blend of the lens over the page. The blend is not needed: the lens covers
the picture only along its edge, a few thousand pixels, and everywhere else the page under it never changes.

### Option C - the CPU draws the look and the bezel
Every renderer is under the 5.2 ms of the blit it replaces. The bezel's page is built once, copied where the page
would be cleared, and only the lens pixels over the picture are blended each frame.

### The wire
A new protocol message would be one more thing for both sides to version. ADR-0041 already sends a shader chain whose
file names the Flip reads for `flipfx-scanlines`; the Look's screens and lenses are names in the same chain.

## Decision

Option C, picked by names in the existing chain.

- Pass 1's file name containing `flipfx-scanlines` is the scanline filter, as before; `flipfx-dots-dmg`,
  `flipfx-dots-pocket`, `flipfx-dots-light`, `flipfx-grid-gbc` or `flipfx-grid-gba` is that screen. An optional pass 2
  containing `flipfx-bezel-<lens>` (`dmg`, `pocket`, `gbc`, `gba`) is Retro with Bezels, its images in `bezels/` beside
  the pass 2 path. Anything else is refused whole, the previous chain left drawing. A change resets the pages.
- The look draws when the frame is at most 240x160 and the rect at least 2x the frame on both axes; otherwise the
  picture is drawn as Clean would be. It is a path of its own in `choose_path`, named in the game-end line as, for
  example, `Look/dots-dmg` or `Look/grid-gba+bezel`. The filter the host asks for is ignored.
- The renderers are `port/miyoo_look.c`, built like the CPU scaler: tables and maps once per geometry, no division or
  allocation in a frame, rows built once and copied to every page row that shows them, turned 180 degrees, nothing
  outside the rect written.
- Cells: panel pixel x of a w-wide rect shows source pixel `floor((x + 0.5) * src_w / w)`, as the Integer scaler, and
  the first panel pixel of each cell on each axis is the gap. Every gap is one panel pixel; at fractional scales cells
  are uneven (3 and 4 wide at 3.89x).
- Grid: per channel, the tone `(C*A + c*(255-A) + 127)/255`, and a gap that tone mixed toward the screen's black by
  the column's, the row's or, where they cross, the larger amount. Integer arithmetic, from tables.
- Dots: each pixel's shade by brightness, and a table per class of panel row, class of column and 10-bit neighbourhood
  (self, left, up, right, up-right) holding the cell's panel pixels. A class is a cell width, or the picture's first
  cell, whose gap has no neighbour to take ink from. The shadow is the grey sampled half a game pixel right and up of
  the panel pixel's centre, bilinearly, which reaches exactly those four texels (self, right, up, up-right), at 0.30 of
  a dot's ink. The shader's own is a whole pixel and 0.45: at three or four panel pixels a cell that drew a dark,
  hard-edged copy of each dot, not a shadow, so the Flip's is nearer and fainter.
  Cells wider than 6 panel pixels are not drawn as dots (the table grows with the square of the width); the Flip's
  widest is Stretch's 5.
- Bezel: on each accepted chain the port lists the lens's images (`<lens>-<w>x<h>+<x>+<y>.png`). For a rect whose size
  has one, it decodes that image (`vendor/stb/stb_image.h`, PNG only), keeps it as one opaque page (the screen's bare
  panel colour with the lens over it, turned) and as spans of the lens pixels with alpha over the picture, and moves
  the picture to `x,y`. The page is copied wherever the port would otherwise clear one; the spans are blended after
  each frame's picture. No image for the size, or a file over 4 MB or whose PNG header is not 752x560 at 8 bits deep (both
  checked on the bytes handed to the decoder, before it runs), or one that will not decode or whose decode asks for
  more than 4 MiB at once (`vendor/stb/stb_image.c`; no such PNG an encoder writes needs more): Retro without a bezel,
  said once in the log at info.
- `tools/lookshot` runs the same file on a PNG frame and writes the page, to compare with the mock-ups the launcher's
  design was approved on. `make check-fliplook` checks every renderer against per-pixel references written from the
  definitions above.

## Consequences

- The look costs 2 to 3.5 ms a frame of CPU, against the 5.2 ms blit those games paid before, by the spike above. The
  renderers in the port are not yet timed on the device.
- Grid matches the mock-ups within one level a channel. Dots differ from the mock-ups, which ran the real shader, where
  the shadow's fold and cell-centred weights differ from the shader's sampling: for Picross on the DMG screen a mean
  of 0.26 levels a channel at Integer and 0.51 at Aspect, at most 24 and 30. Two columns at Aspect are exact ties of
  the cell formula, gaps here and cell pixels in the shader's float arithmetic.
- Memory: the dots' table is about 144 KiB at Integer, 528 KiB at Aspect and 660 KiB at Stretch; a bezel's page is
  1645 KiB, its spans a few KiB, and decoding it needs as much again while it runs. All of it is freed on the next
  chain and at shutdown.
- A bezel is decoded on the first frame at its size: a one-time pause at the start of a game, not measured.
- The colour grid relies on the core's colour correction, and the dots on a greyscale palette; the launcher sets both
  (the contract). A tinted palette still works: shades go by brightness.

## Revisit if

- The port's game-end line shows a Look path losing frames (`lock stood down`, `never shown`) where Soft did not.
- A dots screen is compared on the device and the shadow approximation shows.
- A system other than these three wants a Look: a picture over 240x160 needs a new renderer, not these.
