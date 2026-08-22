# LiteOS P7 — Native Atomic Scanout

Exact P6-applied base:

- `kernel/core/display.c`
  - `e0673de76be150792fb779dc06e21e866a130852`
- `OS_Implementation_Specification_COMPLETE/include/kernel/display.h`
  - `693ebfde10ed78741a1b33dd7b741b0b9a5f0e2d`

## New display path

```text
DISPLAY_COMMIT
     |
 acquire fence
     |
     +---- native backend registered ----+
     |                                   |
     | atomic_commit(buffer)             |
     |        |                          |
     |     page flip                     |
     |        |                          |
     |  flip-done/vblank                 |
     |        |                          |
     | complete(K_OK)                    |
     |        |                          |
     | install current_scanout           |
     | release old scanout               |
     | signal release fence              |
     |                                   |
     +-----------------------------------+
     |
     +---- no native backend ------------+
          existing P6 GOP copy fallback
```

## Ownership

On submit, the new allocation gets one object reference and one pin, exactly as
the existing GPU async submission model does.

For native scanout, that same reference+pin transfers into
`g_display.current_scanout` only after successful flip completion. The old
scanout is unpinned only after the backend reports that the new surface is
latched.

Therefore the signal fence means **scanout lifetime ended**, not merely
**flip command queued**.

## Apply

```bash
unzip LiteOS_Compositor_P7_AtomicScanout_e0673de7.zip

./LiteOS_Compositor_P7_AtomicScanout_e0673de7/apply.sh --check
./LiteOS_Compositor_P7_AtomicScanout_e0673de7/apply.sh

git diff --check
```

## Emit unified diff

```bash
./LiteOS_Compositor_P7_AtomicScanout_e0673de7/apply.sh   --emit-patch /tmp/liteos-p7.patch

git apply --check /tmp/liteos-p7.patch
```

## Roll back

```bash
./LiteOS_Compositor_P7_AtomicScanout_e0673de7/rollback.sh
```
