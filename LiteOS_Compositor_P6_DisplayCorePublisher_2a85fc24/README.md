# LiteOS Compositor P6 — Display-core GOP publisher

Exact GitHub base:

- commit: `769128984e463a961d4efd20fbfbe4347b334292`
- `kernel/graphics/window_server.c`:
  `2a85fc241e991ab5afd6133bf13728d135ebed5b`
- `kernel/core/display.c`:
  `8b13d71bb3b4b013d5ad253ff8608270193a1ce0`
- display ABI header:
  `54ebd9c9f44dcddb28687dfce8ef5a88f9c8cc1d`

P5 is already present in this base.

## What P6 changes

Before:

```text
window_server
    owns MOVDIR64B/MOVNTI GOP writer

display_commit_submit
    owns a separate per-pixel GOP writer
```

After:

```text
                    +-> window compositor P5
display core GOP <--|
publisher            +-> async DISPLAY_COMMIT
```

`display_core_publish_xrgb8888_span()` is now the single WB -> GOP WC
publication primitive.

For native XRGB GOP (`g_display.format == 1`) it uses:

- MOVDIR64B for aligned 64-byte bodies when CPUID allows it
- MOVNTI fallback
- integer/GPR loads only
- no XMM/YMM/FPU state

Other GOP formats keep `display_convert_pixel()`.

## Important: no compositor regression

P6 does **not** move these policies out of P5:

- damage rectangle selection
- 1M-pixel ordinary parallel threshold
- contiguous row slicing across compositor + helpers
- drag remains single writer
- cursor direct-present model
- transaction-level SFENCE

Only the leaf WB -> device-memory span implementation moves to display core.

## Async DISPLAY_COMMIT correctness improvement

`display_complete_commit()` may continue through the deferred queue on another
CPU. MOVNTI/MOVDIR64B ordering is CPU-local.

P6 therefore executes an SFENCE for each deferred 32-row chunk before dropping
the display lock. The existing final fence remains harmless and preserves the
public fence-signalling contract.

## Apply

```bash
unzip LiteOS_Compositor_P6_DisplayCorePublisher_2a85fc24.zip

./LiteOS_Compositor_P6_DisplayCorePublisher_2a85fc24/apply.sh --check

./LiteOS_Compositor_P6_DisplayCorePublisher_2a85fc24/apply.sh

git diff --check
git diff -- \
  kernel/graphics/window_server.c \
  kernel/core/display.c \
  OS_Implementation_Specification_COMPLETE/include/kernel/display.h
```

## Emit a normal unified patch

```bash
./LiteOS_Compositor_P6_DisplayCorePublisher_2a85fc24/apply.sh \
  --emit-patch /tmp/liteos-compositor-p6.patch

git apply --check /tmp/liteos-compositor-p6.patch
git apply /tmp/liteos-compositor-p6.patch
```

## Rollback

```bash
./LiteOS_Compositor_P6_DisplayCorePublisher_2a85fc24/rollback.sh
```
