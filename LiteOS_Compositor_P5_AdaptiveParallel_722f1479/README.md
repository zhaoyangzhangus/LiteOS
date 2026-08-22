# LiteOS Compositor P5 — adaptive parallel ordinary publication

Base:

- `kernel/graphics/window_server.c`
- Git blob: `722f147920039fc4722e9053ad3a4ee9f7af6a9e`

P1–P4 are already present in this base.

## Current bottleneck targeted

The compositor already owns two persistent copy workers, but ordinary client
repaints still always use the single-CPU `compositor_publish_rect()` path.

P5 uses the existing disjoint-row workers when all of these are true:

- not a drag/move transaction
- exactly one damage rectangle
- rectangle is larger than 1,048,576 pixels (~4 MiB XRGB8888)
- persistent copy workers are available
- compositor is still preemptible

Small and fragmented damage stays exactly as before.

## Why one rectangle only

Spatial damage should stay cheap.

Starting a helper generation for every tiny rectangle can cost more than the
copy itself and can turn a good sparse-damage workload into wake/scheduler
overhead.

Full-window and full-screen repaints naturally tend to be one large rectangle,
which is the workload this optimization targets.

## Hybrid worker completion wait

Before P5, participant zero does:

```text
finish local slice
    ↓
if helper not finished:
    schedule() immediately
```

A remote helper that is only a few microseconds behind can therefore cause a
full scheduler round trip.

P5 instead:

```text
up to 1024 x PAUSE
    ↓ still incomplete
schedule()
    ↓
repeat
```

This keeps the wait bounded while avoiding unnecessary context switching in
the common case.

## Apply

```bash
unzip LiteOS_Compositor_P5_AdaptiveParallel_722f1479.zip

./LiteOS_Compositor_P5_AdaptiveParallel_722f1479/apply.sh --check

./LiteOS_Compositor_P5_AdaptiveParallel_722f1479/apply.sh

git diff --check
git diff -- kernel/graphics/window_server.c
```

## Emit standard patch

```bash
./LiteOS_Compositor_P5_AdaptiveParallel_722f1479/apply.sh \
  --emit-patch /tmp/liteos-compositor-p5.patch

git apply --check /tmp/liteos-compositor-p5.patch
git apply /tmp/liteos-compositor-p5.patch
```

## A/B — disable ordinary multi-core path

```bash
./LiteOS_Compositor_P5_AdaptiveParallel_722f1479/rollback.sh

./LiteOS_Compositor_P5_AdaptiveParallel_722f1479/apply.sh \
  --disable-parallel-ordinary
```

## A/B — keep multi-core but restore immediate schedule wait

```bash
./LiteOS_Compositor_P5_AdaptiveParallel_722f1479/rollback.sh

./LiteOS_Compositor_P5_AdaptiveParallel_722f1479/apply.sh \
  --disable-spin-wait
```

## Rollback

```bash
./LiteOS_Compositor_P5_AdaptiveParallel_722f1479/rollback.sh
```
