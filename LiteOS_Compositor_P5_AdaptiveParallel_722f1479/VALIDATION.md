# P5 validation

## Correctness

P5 does not change scene composition or damage generation, but test:

- 32x32 client update
- several separate small dirty rectangles
- 1200x800 full-window repaint
- 1920x1080 full-window repaint
- 2560x1440 full-screen repaint
- window drag
- live resize
- cursor-only movement

Drag should remain on the single-writer path because
`WINDOW_COMPOSITOR_PARALLEL_DRAG` stays `0U`.

## Expected path selection

### 1200x800
960,000 pixels: below the P5 1,048,576 threshold -> serial.

### 1920x1080
2,073,600 pixels -> parallel if it is one damage rectangle.

### 2560x1440
3,686,400 pixels -> parallel if it is one damage rectangle.

## A/B measurement

Compare:

1. P4 baseline
2. P5 default
3. P5 `--disable-parallel-ordinary`
4. P5 `--disable-spin-wait`

The important metric is large ordinary repaint latency, not cursor or tiny
damage latency.

If P5 does not improve full-screen publication, the target is likely already
saturating the device/WC path on one CPU. In that case adding more CPU copy
workers is the wrong direction; move to native page flip / display-driver
scanout.
