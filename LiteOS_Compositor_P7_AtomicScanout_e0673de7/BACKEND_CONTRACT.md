# P7 native scanout backend contract

P7 does not invent GB205 display registers.

A real display driver registers one kernel-lifetime backend:

```c
static kstatus_t my_atomic_commit(
    void *context,
    const display_scanout_request_t *request,
    display_scanout_complete_fn complete,
    void *completion_context)
{
    /*
     * Validate the allocation for scanout.
     * Program inactive scanout state.
     * Arm one atomic flip.
     * Save complete + completion_context in the driver's flip record.
     */
    return K_OK;
}

static const display_scanout_backend_t backend = {
    .context = &device_display_state,
    .atomic_commit = my_atomic_commit,
};

display_core_register_scanout_backend(0U, &backend);
```

Completion path:

```text
display/vblank IRQ
    -> ACK hardware
    -> queue GPU display bottom-half
    -> return

GPU display bottom-half
    -> complete(completion_context, K_OK)
```

Failure:

```c
complete(completion_context, K_EDEVLOST);
```

Rules:

- `atomic_commit() == K_OK`: backend owns the request asynchronously and must
  invoke `complete()` exactly once.
- non-K_OK: backend did not take ownership and must not invoke `complete()`.
- `complete(K_OK)` means the new surface is latched and the old surface is no
  longer fetched.
- callback may be synchronous or deferred, but not hard-IRQ context.

## VRAM-ready validation

P7 removes `buffer->backing != NULL` from generic DISPLAY_COMMIT shape
validation.

That is required because a native scanout allocation can live in VRAM and have
no CPU mapping. The GOP fallback still checks `pending->buffer->backing` before
copying, so GOP behavior remains safe.
