# P7 design notes

- Existing `display_commit_submit()` remains the single public submission API.
- No second userspace page-flip syscall is introduced.
- Native backend registration is one-shot; no unregister race is introduced.
- After native registration, backend errors do **not** silently fall back to
  GOP, because that would make scanout ownership ambiguous.
- `gpu_allocation_t` already contains `backend`, `gpu_va`, `backing_phys`,
  `pin_count`, and the generic object reference needed by a real GPU backend.
- P7 does not yet convert the Ring0 compositor's retained `kzalloc` scene into
  scanout-capable buffers. It establishes the correct display-core atomic
  commit/fence/ownership semantics first.
