# P6 design notes

## Why not route window_server through display_commit_submit yet?

The current API requires:

- `gpu_allocation_t`
- acquire/signal fences
- asynchronous deferred completion

and the current GOP implementation processes only 32 rows per continuation.

The Ring0 compositor already owns a retained WB scene and a tuned sparse-damage
publication scheduler. Routing it through DISPLAY_COMMIT now would throw away
P5 damage and parallel-copy policy.

P6 instead consolidates the **leaf GOP writer** first.

## Why this is the correct precursor to page flip

After P6 there is only one display-layer implementation of WB -> GOP WC stores.
A later native GPU backend can be added behind display core without leaving
MOVDIR64B/MOVNTI details embedded in the compositor.

The next structural step is to extend the existing display submit contract with
a native scanout/page-flip backend while keeping this P6 function as the GOP
fallback.

## Software cursor

P6 does not add a cursor save buffer.

The retained `composite_framebuffer` remains the clean cursor-free scene.
Cursor movement continues to restore only the old 24x24 area from that WB
scene and overlay the new cursor directly on scanout.
