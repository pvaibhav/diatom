# 0050. On the Miyoo Mini Flip, scale on the CPU with three filters, and keep Integer whole pixels

- **Status:** Accepted
- **Date:** 2026-11-15
- **Supersedes:** -
- **Superseded by:** -

## Context

Until now the Flip port sent every frame through the vendor's 2D engine (`MI_GFX_BitBlit`), which scaled it, converted
it to xRGB and turned it 180 degrees into fb0 in one call. The port ignored the filter it was passed.

The engine smooths whenever it scales, so the picture was soft in every display mode, Integer included, where it
should be exact. The vendor library has no call to turn that off: `libmi_gfx.so` exports BitBlit, QuickFill, DrawLine
and the palette and alpha-threshold calls, and the kernel's nearest-mode setter is never called by `mi_gfx.ko`. Those
findings are the owner's, from the binaries; they were not re-checked for this record.

The CPU path that existed as a fallback was measured at 29.6 ms a frame (TortOS plorpos-f1p.6). It converted and
looked up every output pixel, and it copied repeated rows by reading them back out of fb0. The new scaler does not
read from fb0, and it computes each distinct output row once.

The Flip has two Cortex-A7 cores at 1.2 GHz with NEON and no GPU. A frame is up to 752x560 pixels, 1.7 MB to write.
The comment on the blitter path in `port/miyoo.c` records about 5 ms a blit, waited for in present.

## Options considered

### Option A - keep the blitter
Costs no CPU and no code. But the picture stays soft, Integer included, and nothing in the vendor's interface changes
that.

### Option B - the blitter for scaling, after a CPU nearest prescale to a whole factor
This gives the Crisp look in hardware: the CPU scales up by a whole factor, then the blitter scales down. But the
filter the blitter uses to scale down is unknown and cannot be changed. The prescaled frame needs MMA memory several
times the frame's size. Integer still needs the CPU. Not measured.

### Option C - the CPU everywhere, three filters
Sharp is nearest neighbour. Crisp is a nearest upscale to the next whole factor (the ceiling of the scale, per axis),
then a bilinear downscale to the rect. Soft is bilinear. Each frame's cost is the CPU's, and it is unknown on the
device until `tools/scalebench` runs there. Every pixel is under our control, and the result can be tested on the
host.

## Decision

Option C, in `port/miyoo_scale.c`:

- The maps and weights are built once per geometry. Each source row is converted once, scaled across once and kept
  for the rows that use it. Output rows are built in a cached buffer and copied to every fb row that shows them, and
  fb0 is never read. NEON does the blends and the RGB565 conversion. A plain C loop does the same arithmetic, and the
  host test requires the two to match bit for bit.
- The port interface gets `DIATOM_FILTER_LINEAR`, and the protocol's `filter=` takes `linear`. Sharp, Crisp and Soft
  are `nearest`, `sharp` and `linear`. Diatom's existing SHARP (sharp-bilinear) is Crisp's name there. On the Flip its
  definition is the two-step one above, which equals NEAREST at a whole factor.
- `RUN` takes `filter=` as it takes `mode=`, so the first frame is drawn with it. Absent, the start filter.
- In `integer` and `integer-over` the host passes NEAREST, whatever the setting, so Integer is always whole pixels. It
  does this on every port.
- The same scaler draws the scanline filter. It skips every third panel row of the rect, and those rows stay black from
  the page clear.
- The blitter path stays, off by default. `DIATOM_BLIT=hw` or `TortOS/diatom.hwblit` turns it on for a comparison on
  the device. It ignores the filter. It is not in the menu, because a fourth filter value would put one device's
  hardware in the port interface.
- The launcher's default is Crisp. At a whole factor it is identical to Sharp. At the Flip's usual 2.5x to 4.7x it
  keeps every source pixel the same size, which Sharp cannot, and it does not blur the picture as Soft does.

## Consequences

- The picture is exact in Integer, and the player chooses the look in every other mode.
- Present costs CPU time that the blitter did not, before the game's next frame. How much is not yet known on the
  device. The game-end log line now names the filter and pixel format with the blit time, so a game that slows can be
  traced to it.
- Every port has to accept LINEAR. The Brick and H700 build their maps without sharp-bilinear's steepening for it, the
  desktop and GKD draw it with linear sampling, and the GKD already drew SHARP that way.
- Integer is now nearest on every port, also where sharp or linear was asked for. Before this, the desktop and the GKD
  blended at an integer factor when the filter was sharp.

## Revisit if

- `tools/scalebench --fb` on the Flip shows a Crisp or Soft median over 4 ms for any case in its table. Then Crisp is
  too expensive as the default, or its loops need another pass.
- A game that held 60 fps on the blitter (`DIATOM_BLIT=hw`) loses frames with the CPU scaler, as shown by `lock stood
  down` or `never shown` in the game-end line.
- A way to put the blitter in nearest mode is found.
