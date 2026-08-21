# LiteOS Compositor Retained-Drag Optimization v2

Baseline:

```text
40c242b066fc1a896f0933616cd20f9b749d2ae1
```

This replaces the previous v1 compositor experiment.

## Why v1 had little effect

v1 reduced some damage merging, but the live motion path still forced:

```c
g_window_server.drag_blit_valid = false;
```

so a moved window still re-rendered its complete new surface every frame.

## v2 changes

### 1. Clean retained scene

`composite_framebuffer` now contains only:

```text
desktop + windows + decorations
```

The software cursor is no longer baked into the retained scene.

### 2. Cursor follows HID rate

Pointer-only input no longer dirties the scene.

The Window worker immediately:

```text
restore old 24x24 cursor from clean WB scene
blend new 24x24 cursor directly to WC GOP framebuffer
```

So scene composition can remain 60Hz while the pointer follows the input rate.

### 3. Correct 2-D retained drag

The old retained-blit mixed window-local X offsets with framebuffer coordinates.
v2 copies:

```text
source = old screen position + local window offset
dest   = new screen position + same local window offset
```

and chooses row/column direction like a real 2-D `memmove`.

### 4. First frame full, later frames retained

The first frame of a drag intentionally performs a complete composition.
After that clean frame is committed and the captured window is topmost,
retained reuse is armed.

Subsequent small moves recompose only exposed regions plus four tiny
rounded-corner repair squares.

### 5. Additional safe copy changes

- parallel publication threshold: 32K -> 512K pixels;
- max helpers: 2;
- helper CPUs avoid the compositor creator CPU (normally CPU0);
- desktop-cache WB->WB restore stays on the cached path;
- a retained drag can still publish one final bounding scanout transaction.

## Applying over v1

If the script detects:

```text
LITEOS_COMPOSITOR_OPT_V1
```

it automatically restores:

```text
.compositor-opt-backup-40c242b/kernel/graphics/window_server.c
```

and then applies v2.

## Apply

```bash
python3 apply_compositor_v2.py ~/LiteOS --check-only

./apply.sh ~/LiteOS

cd ~/LiteOS
git diff -- kernel/graphics/window_server.c

make clean
make -j$(nproc)
make test
```

## Rollback

```bash
./rollback.sh ~/LiteOS
```

## What to test

1. Move mouse without dragging: pointer should feel much more immediate.
2. Drag a large 1200x800+ window.
3. First drag frame can be expensive; subsequent frames should be much cheaper.
4. Check for cursor trails.
5. Check all four rounded corners for stale pixels.
6. Test 1/2/4 vCPU with the same window size.

## Remaining ceiling

GOP still has no page flip or hardware cursor plane. The final scanout
publication of a large moved window can still be several MiB.

If v2 makes scene composition cheap but drag is still limited, the next
bottleneck is the GOP publication path itself, not the scene renderer.
