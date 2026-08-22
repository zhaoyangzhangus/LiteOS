# P6 validation

## Build

```bash
git diff --check
# then run the normal LiteOS build
```

The current compiler/assembler already accepts MOVDIR64B because P4 uses the
same instruction in `window_server.c`.

## Window compositor regression matrix

Test:

- cursor-only movement
- tiny text/caret damage
- several sparse damage rectangles
- 1200x800 repaint
- 1920x1080 repaint
- 2560x1440 repaint
- window drag
- live resize

Expected: P5 path selection is unchanged.

## DISPLAY_COMMIT path

Run the existing display self-test and any GPU/display submission diagnostics.

Native format 1 should now perform bulk WB -> WC copies instead of one
`display_convert_pixel()` call per pixel.

At 2560x1440:

- pixels: 3,686,400
- bytes: 14,745,600
- aligned 64-byte body: 230,400 cache-line transfers

The exact instruction mix also includes scanline alignment prefixes/tails.

## Required correctness condition

The async deferred path now fences every copied chunk. Do not remove that fence
unless the deferred executor is permanently pinned to one CPU and the ordering
contract is proven another way.
