# 0050. On the Miyoo Mini Flip, scale with the blitter, give Crisp a CPU upscale, and draw Integer on the CPU

- **Status:** Accepted
- **Date:** 2026-11-15
- **Supersedes:** -
- **Superseded by:** -

## Context

The Flip port sends frames through the vendor's 2D engine (`MI_GFX_BitBlit`). One call scales the frame, converts it to
xRGB and turns it 180 degrees into fb0, at almost no CPU cost. The comment on the blitter path in `port/miyoo.c` records
about 5 ms a blit, waited for in present.

The engine smooths whenever it scales. The vendor library has no call to turn that off: `libmi_gfx.so` exports BitBlit,
QuickFill, DrawLine and the palette and alpha-threshold calls, and the kernel's nearest-mode setter is never called by
`mi_gfx.ko`. Those findings are the owner's, from the binaries; they were not re-checked for this record. So Integer,
which should be exact, is soft, and at the Flip's usual factors of 2.5x to 4.7x the edge of every source pixel is
blended across the whole of it.

The Flip has two Cortex-A7 cores at 1.2 GHz with NEON and no GPU. CPU time spent in present is taken from the core
before its next frame. A frame on the panel is up to 752x560 pixels, 1645 KB in xRGB.

## Options considered

### Option A - the blitter for everything
Costs no CPU and no code. Integer stays soft, and no sharper look is offered.

### Option B - the CPU for everything
Nearest, a two-step Crisp and bilinear, all drawn by the CPU, with every pixel under our control and testable on the
host. But every game pays CPU time for every frame, also the many for which the blitter's picture is fine, and CPU time
is what the Flip has least of.

### Option C - the blitter, and the CPU only where the blitter cannot make the picture
Soft is the blitter alone. Crisp enlarges the frame on the CPU by whole factors with nearest neighbour, and the blitter
scales that down to the rect with its bilinear filter. Each source pixel is then several pixels wide and high, so the
blending is confined to the edges between source pixels. Integer is drawn on the CPU with nearest neighbour, because
any blit blurs it. Crisp costs a CPU upscale every frame and a bigger blit; Soft costs nothing new. The look of Crisp
depends on the blitter's filter when it scales down, which is assumed to be bilinear and has not been compared with a
reference on the device.

## Decision

Option C.

- The port interface's filters are `NEAREST`, `SHARP` and `LINEAR`, and the protocol's `filter=` takes `nearest`,
  `sharp` and `linear`. On the Flip, `sharp` is Crisp and `linear` is Soft. `RUN` takes `filter=` as it takes `mode=`,
  so the first frame is drawn with it; absent, the start filter.
- In `integer` and `integer-over`, and for one DS screen at `fill=integer`, the host passes `NEAREST` whatever the
  setting (`present_filter` in `src/main.c`). It does this on every port.
- The Flip port picks a path for each geometry (`choose_path` in `port/miyoo.c`):

  | Case | Path |
  |---|---|
  | Scanline filter on (rect at least 2x the frame's height) | the blitter, two blits into every third row, whatever the filter |
  | `NEAREST` (Integer) | the CPU scaler, nearest neighbour, straight into the page |
  | Crisp, rect a whole multiple of the frame on both axes | the CPU scaler, nearest neighbour: nothing to scale down |
  | Crisp | the upscale into the blitter's buffer, then the blitter scales it down |
  | Crisp, upscale bigger than a panel page | as Soft, logged once per geometry |
  | Soft | the blitter |
  | No blitter (`MI_GFX_Open` failed) | the CPU scaler with the filter asked for |

  A blit that fails draws that frame on the CPU, and the game-end log line counts it. That line also names the path,
  for example `Soft/hardware`, `Crisp/upscale 3x3` or `Integer/CPU`.
- Crisp's factors are `kx = ceil(rect width / frame width)` and `ky` the same for heights. The upscaled frame keeps the
  frame's pixel format, so RGB565 stays 16-bit. The port writes it into the vendor's MMA buffer that the blitter
  reads, and that buffer only grows. Each source row is widened once (NEON stores for factors 2 to 4, plain C above)
  and copied `ky` times. The port never reads the buffer back, and flushes the cache over the bytes written, as it does
  before a plain blit.
- Crisp's limit is one panel page of bytes, 752x560x4 = 1645 KB. Integer at full screen writes that much a frame, so
  Crisp never writes more than the CPU path already may. The common RGB565 frames fit: a 160x144 Game Boy at 5x4 is
  900 KB, a 320x240 PlayStation frame at 3x3 1350 KB, a 368x240 one 1553 KB. A 640x480 frame at 2x2 (2400 KB) and a
  256x240 XRGB8888 frame at 3x3 (2160 KB) do not, and are drawn as Soft.
- The CPU scaler (`port/miyoo_scale.c`) builds its maps once per geometry, draws a row once and copies it to every page
  row that shows it, and never reads fb0. NEON does the blends and the RGB565 conversion. Every loop, the upscale's
  included, has a plain C form, and the host test (`make check-flipscale`) requires the two to match bit for bit.
- `tools/scalebench` times the CPU scaler for each filter and Crisp's upscale for the common frame sizes and modes.
  The blit that follows the upscale is not in its times.
- The launcher offers Crisp and Soft, per game and per system. Soft is the default.

## Consequences

- Soft, the default, costs no CPU time for scaling, because the blitter does it.
- Integer is exact on the Flip, and on every port. Before this, the desktop and the GKD blended at an integer factor
  when the filter was `sharp`.
- Integer costs CPU time on the Flip: a nearest-neighbour draw of the rect. Crisp costs an upscale each frame, and the
  blitter reads up to 1645 KB a frame instead of the frame itself. Neither is measured on the device yet. The simulator's
  times say nothing about the device.
- The blitter draws the scanline filter in every mode. With it on, Integer and Crisp are as soft as Soft.
- Every port accepts `LINEAR`. The Brick and H700 build their maps without sharp-bilinear's steepening for it, and the
  desktop and GKD sample it linearly.
- Widening by 5 or more runs in plain C. A Game Boy in Stretch is 5x4 across and down.
- Unverified on the device: the largest source surface the blitter accepts. Of the frames named here, Crisp's widest
  upscale is a 368-wide PlayStation frame at 3x, 1104 pixels. If the blitter refuses it, every frame of that geometry is drawn on the CPU and counted in
  the game-end line.

## Revisit if

- `tools/scalebench` on the Flip shows a Crisp upscale median over 2 ms, or an Integer (CPU nearest) median over 4 ms,
  for any case in its table.
- A game that holds full speed on Soft loses frames on Crisp, as shown by `lock stood down` or `never shown` in the
  game-end line.
- The game-end line reports failed blits for Crisp on the device.
- A way to put the blitter in nearest mode is found. Then Integer and Crisp can be drawn by the blitter alone.
