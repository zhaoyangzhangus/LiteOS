# LiteOS Window / Mouse Performance Optimization Implementation Specification

Baseline: current analyzed LiteOS HEAD `3531bf0efe573f7d128ad913fcb2818dda7f8ab7`

## Goal

- Eliminate visible mouse stutter/jumpiness.
- Reduce input-to-cursor latency.
- Reduce unnecessary compositor and framebuffer work.
- Preserve keyboard/button ordering, Window ABI, Z-order, focus, drag/resize semantics.
- Keep the current Ring0 Window Server architecture.
- Keep Step5 allocator and CPU-local preemption changes intact.

## 0. Confirmed current problems

Current path:

```text
xHCI IRQ
 -> deferred xHCI worker
 -> HID report decode
 -> input_core_push()
 -> global input queue
 -> window_server_pump_input()
 -> Window routing / pointer update
 -> damage
 -> compositor snapshot
 -> WB composite framebuffer
 -> WC GOP framebuffer commit
```

Confirmed issues:

1. `window_server_pump_input()` currently runs from the idle loop after `sti; hlt`.
   Window input processing therefore depends on the CPU reaching/returning from idle.
   Input can accumulate while other user/kernel work is runnable.

2. Relative mouse motion is coalesced twice:
   - `kernel/core/input.c`
   - `kernel/graphics/window_server.c`

   This preserves total displacement but can destroy temporal resolution and turn many
   small movements into one visibly large jump.

3. QEMU `usb-mouse` is normally about 125 Hz. This limits smoothness but should not
   by itself cause severe stutter.

4. `compositor_commit_snapshot()` disables preemption while publishing damage to the
   GOP framebuffer. Tiny cursor damage is fine; large move/resize damage can create a
   noticeable non-preemptible interval.

5. Existing Window Server optimizations must be preserved:
   - up to 16 damage rectangles;
   - damage coalescing;
   - immutable compositor snapshot;
   - expensive render outside `window_lock`;
   - retained WB composite framebuffer;
   - WC/MOVNTI framebuffer publication;
   - one SFENCE per commit;
   - pointer motion batching;
   - bottom->top Z-order and top->bottom hit testing;
   - topmost full-cover optimization;
   - per-window compositor surface page cache.

# Phase M1 — Dedicated Window input/compositor kernel worker

Priority: CRITICAL

## Problem

Current runtime concept:

```c
for (;;) {
    __asm__ volatile("sti; hlt");
    window_server_pump_input();
    net_manager_poll();
}
```

This is opportunistic instead of event driven.

## Required design

Create a persistent Ring0 Window worker:

```text
xHCI IRQ
 -> deferred worker
 -> input_core_push()
 -> wake Window worker
 -> drain input
 -> update pointer/window state
 -> accumulate damage
 -> compositor
```

The worker must BLOCK when no work exists. No busy polling.

Suggested files:

- `kernel/core/input.c`
- `include/kernel/input.h`
- `kernel/graphics/window_server.c`
- `include/kernel/window_server.h`
- scheduler/kernel-thread bootstrap code as needed
- `kernel/kernel_entry.c`

Add an internal notification/wait mechanism. Possible APIs:

```c
bool input_core_has_pending(void);
kstatus_t input_core_wait(uint64_t timeout_ns);
```

or a direct Window-worker wake mechanism.

Use a lost-wakeup-safe BLOCKED/READY handshake similar to the existing deferred worker.

After the worker is operational, remove normal runtime ownership of
`window_server_pump_input()` from the idle loop. There must never be two concurrent
Window consumers.

Acceptance:
- mouse progresses while a CPU-bound Ring3 thread is continuously runnable;
- no dependency on idle/HLT;
- no lost wakeup;
- exactly one Window compositor/input worker;
- key/button ordering unchanged;
- existing tests pass.

# Phase M2 — Pressure-only input motion coalescing

Priority: CRITICAL

Current `input_core_push()` coalesces relative motion too aggressively.

Normal queue pressure must preserve original timing/order.

Suggested threshold:

```c
#define INPUT_COALESCE_THRESHOLD (INPUT_CORE_CAPACITY * 3U / 4U)
```

Only do:

```c
if (incoming.type == INPUT_EVENT_RELATIVE &&
    g_input.count >= INPUT_COALESCE_THRESHOLD) {
    if (input_coalesce_relative(&incoming)) {
        ...
    }
}
```

When below threshold, append motion normally.

At full queue:
- never block device/deferred producer;
- prefer dropping/coalescing stale relative motion;
- preserve newest pointer state;
- preserve key/button ordering barriers.

Keep Window-level motion batching initially. Re-benchmark it after M1.

# Phase M3 — One HID mouse report = one pointer transaction

Priority: HIGH

Current boot mouse report contains:

```text
buttons + dx + dy + wheel
```

but X/Y become separate events and are reconstructed later.

Prefer a kernel-internal transaction:

```c
typedef struct input_pointer_motion {
    uint64_t timestamp;
    uint32_t device_id;
    uint16_t flags;
    int32_t dx;
    int32_t dy;
    int32_t wheel;
} input_pointer_motion_t;
```

Do not change the public Ring3 input ABI initially.

One USB report should become one pointer state transition:

```c
pointer_x += dx;
pointer_y += dy;
```

Buttons from the same report must preserve ordering.

# Phase M4 — Reduce large non-preemptible GOP publication

Priority: HIGH

Keep current design where scene composition happens in preemptible WB memory.

Instrument first:

```text
compositor_commit_count
compositor_commit_pixels
compositor_commit_max_pixels
compositor_commit_max_tsc
compositor_nonpreempt_max_tsc
```

Then optimize large commits.

Possible paths:

1. Ordinary unrelated damage: bounded row chunks with scheduling points.
2. Drag/move transaction: keep atomic publication until a better page-flip design exists.
3. Long-term: render to alternate scanout buffer and page flip.

Do not introduce visible half-moved frames.

# Phase W1 — Precise application damage

Priority: CRITICAL for overall Window performance

Kernel already supports local `WINDOW_UPDATE(x,y,width,height)` damage.

Applications must stop submitting whole-window damage for small changes.

For gshell:
- typing one character: redraw only changed glyph/input-line region;
- caret movement: damage old and new caret rectangles;
- command output: damage changed terminal rows;
- resize: full redraw is acceptable.

Apply the same rule to notes/file manager/network UI.

Acceptance:
typing one character must not submit the whole client surface.

# Phase W2 — Frame scheduler / damage accumulation

Priority: HIGH

Separate:

```text
input state update frequency
```

from:

```text
display composition frequency
```

Input must update pointer state immediately.
Rendering should happen at a frame deadline/VBlank.

Support configurable targets, not hardcoded 60 Hz:

```text
60Hz  = 16.67ms
120Hz = 8.33ms
144Hz = 6.94ms
240Hz = 4.17ms
```

Future GPU/display driver should replace timer pacing with real VBlank/page-flip completion.

# Phase W3 — Damage overflow -> tile damage, not fullscreen

Priority: HIGH

Current behavior after more than 16 damage rects must not become automatic fullscreen damage.

Use a hybrid:

```text
0..16 rects -> existing rect path
overflow    -> convert to tile bitmap
```

Recommended initial tile size:

```text
64x64 pixels
```

4K requires about:

```text
60 x 34 = 2040 tiles ~= 255 bytes
```

Mark dirty tiles with bit operations and later convert runs into spans or render tiles directly.

# Phase W4 — Tile/region occlusion

Priority: MEDIUM/HIGH

Upgrade current single-window full-cover optimization.

For each dirty tile/span:

```text
visible = dirty
for windows top -> bottom:
    draw intersection(window, visible)
    if opaque:
        visible -= covered
if visible:
    draw desktop
```

Do not build a complex polygon region system yet. Tiles are enough.

# Phase W5 — Per-window event wait queue

Priority: MEDIUM

Current windows have independent event rings but share a global Window wait queue.

Move toward:

```c
typedef struct window_server_window {
    ...
    wait_queue_t event_waitq;
    ...
} window_server_window_t;
```

Target events should use:

```c
wake_one(&window->event_waitq);
```

`window_server_event_read()` should wait only on that Window.

Process/window teardown must wake blocked readers safely.

# Phase W6 — O(1) identifier lookup

Priority: MEDIUM/LOW at current 64-window limit

Replace linear `find_window_locked()` lookup with slot+generation identifier:

```text
high bits = generation
low 6 bits = slot
```

Lookup:

```c
slot = identifier & 63U;
window = window_slots[slot];
if (!window || window->identifier != identifier)
    fail;
```

Keep a separate ordered array for Z-order.

# Phase W7 — Pixel copy tuning only after reducing pixel count

Priority: LOW initially

Current code already has:
- WB composite buffer;
- REP MOVSQ;
- MOVNTI WC framebuffer stores;
- one SFENCE/commit;
- no kernel SIMD dependency.

Do not prematurely add AVX.

After W1-W4 benchmark on target CPU:

```text
REP MOVSB
REP MOVSQ
64-bit integer copy
AVX2 NT copy only after defining kernel SIMD/FPU rules
```

Test by span-size buckets.

# Long-term GPU compositor

Future architecture:

```text
Window shared surface
 -> GPU surface/texture
 -> GPU BLIT/composite
 -> scanout buffer
 -> page flip
```

Add hardware cursor plane:

```text
mouse motion -> cursor-plane position update
```

Then ordinary pointer movement does not damage the main framebuffer.

# Required instrumentation

Input:
```text
hid_reports
input_events_pushed
input_motion_coalesced
input_motion_dropped
input_queue_max_depth
window_worker_wake_count
window_worker_wake_to_run_max_tsc
window_worker_wake_to_run_avg
```

Pointer:
```text
pointer_transactions
pointer_total_dx
pointer_total_dy
pointer_max_batch_reports
pointer_max_batch_dx
pointer_max_batch_dy
```

Compositor:
```text
frames
damage_rects_total
damage_pixels_total
full_damage_frames
tile_damage_frames
windows_considered
windows_drawn
occluded_windows_skipped
surface_pixels_read
composite_pixels_written
scanout_pixels_written
```

Timing:
```text
HID completion -> input_core_push
input_core_push -> Window worker run
Window worker run -> pointer state update
pointer update -> compositor begin
compositor begin -> WB render complete
WB render -> GOP publication complete
```

# Mouse-specific tests

## A. Raw pointer stream

Inject 1000 reports:

```text
dx=1, dy=0
```

Verify final displacement is exactly 1000 and no drops occur without pressure.

## B. Ordering

```text
move +5
move +4
left press
move +3
left release
```

Button ordering must remain exact.

## C. Queue pressure

Push motion faster than consumer.

Verify:
- bounded queue;
- stale motion may be merged/dropped;
- newest pointer position converges correctly;
- no stuck key/button;
- dropped/coalesced telemetry increments.

## D. CPU load

Run a CPU-bound Ring3 thread continuously.

Mouse must remain responsive. This specifically tests removal of idle-loop dependency.

## E. Drag

Drag a large 1200x800 window.

Record:
- max non-preemptible GOP commit;
- max input queue depth;
- max motion batch size;
- frame duration.

## F. Many windows

Test 1/8/16/32/64 windows and measure hit-test, snapshot and compositor cost.

# Required implementation order

```text
M1 dedicated Window worker
M2 pressure-only input coalescing
M3 pointer transaction cleanup
instrument + benchmark mouse
M4 reduce large non-preemptible GOP commit

W1 precise app damage
W2 frame scheduler
W3 tile damage overflow
W4 tile occlusion
W5 per-window wait queue
W6 O(1) identifier lookup
W7 copy micro-optimization

GPU compositor / hardware cursor later
```

Do not combine everything into one giant patch.

# Constraints

Must preserve:
- public Window ABI unless explicitly extended;
- keyboard/button ordering;
- Z-order/focus semantics;
- drag/resize behavior;
- damage correctness;
- snapshot object lifetime/reference safety;
- shared-section ownership;
- Step5 allocator;
- CPU-local preemption;
- xHCI HID hotplug correctness;
- exactly one active compositor.

Do NOT:
- move expensive rendering back under `window_lock`;
- busy-poll input;
- add timer polling to xHCI HID;
- leave idle loop and Window worker both pumping input;
- drop key/button events just to smooth motion;
- make 17 damage rects force fullscreen;
- add kernel AVX before SIMD/FPU policy exists;
- remove WC/MOVNTI without benchmark evidence.

# Expected immediate result from M1 + M2

Before:

```text
HID reports
 -> unconditional input coalesce
 -> wait until idle loop runs
 -> second Window batch
 -> one large pointer step
```

After:

```text
HID report
 -> input queue
 -> wake Window worker immediately
 -> small ordered batch
 -> prompt pointer update
```

Expected visible result:
- temporally more uniform pointer movement;
- fewer large cursor jumps;
- CPU-bound workloads no longer stall mouse until idle;
- drag/GOP publication becomes the next bottleneck if still visible.

# Codex execution instructions

1. Inspect current repository HEAD before editing.
2. Do not rely on line numbers in this document.
3. Implement M1 only first.
4. Compile with normal repository `-Wall -Wextra -Werror`.
5. Run ABI sanity and existing tests.
6. Add a focused mouse scheduling test.
7. Show diff and diagnostic/benchmark output.
8. Only after M1 passes, implement M2.
9. Continue one phase at a time.
10. Never silently change public ABI or remove correctness paths.
