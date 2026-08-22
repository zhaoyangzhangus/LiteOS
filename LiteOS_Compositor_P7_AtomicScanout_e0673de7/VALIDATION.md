# P7 validation

```bash
./LiteOS_Compositor_P7_AtomicScanout_e0673de7/apply.sh --check
./LiteOS_Compositor_P7_AtomicScanout_e0673de7/apply.sh
git diff --check
```

Then run the normal LiteOS `make kernel`/boot tests.

P7 adds no object file and requires no makefile change.

## GOP regression

With no backend registered:

- `display_core_has_native_scanout(0)` is false;
- DISPLAY_COMMIT still uses the P6 GOP copy path;
- MOVDIR64B/MOVNTI behavior is unchanged;
- window compositor source is unchanged.

## Native backend invariants

1. Release fence is not signaled merely because `atomic_commit()` returned.
2. On first successful completion, submitted buffer becomes current scanout.
3. On the next successful completion, old scanout is unpinned only then.
4. Failed flip leaves old scanout untouched and fails the new fence.
5. Acquire fence must complete before `atomic_commit()` is invoked.
6. Only one display commit is pending at a time in P7.
