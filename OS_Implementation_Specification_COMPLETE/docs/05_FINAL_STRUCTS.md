# Final Core Structures

The actual authoritative definitions are in `include/kernel/*.h`. This document explains why each exists.

## `page_t`

One descriptor per present physical page, stored through the sparse page database.

Important semantics:

- `refs` is ownership/reference lifetime.
- `mapcount` counts CPU mappings where required.
- `order` is valid on a free buddy head and on an allocated compound head.
- Free pages are present in exactly one buddy free list.
- Allocated pages are not kept in a parallel “used list”.
- Tail pages of multi-page allocations point to the compound head.
- `page_free(head)` derives the order from the head descriptor.

## `vm_area_t`

Software mapping contract:

```text
[start,end)
protection
flags
object
object_offset
```

An augmented RB tree ordered by `start` is the only VMA index. The ordered list is retained for cheap neighbor traversal and teardown.

## `vm_object_t`

Backing ownership is explicit:

```text
ANON
FILE
SHARED
DEVICE
```

COW is expressed by VMA/object semantics. It is not inferred solely from a read-only PTE.

## `vm_space_t`

Owns:

- page-table root
- PCID
- VMA tree
- per-mm page-table lock stripes
- active CPU mask
- TLB generation
- memory accounting

## `thread_t`

Owns:

- kernel object identity
- scheduler state
- kernel stack
- x86 switch state + XSAVE pointer
- CPU affinity
- process membership

The trap frame is built on the kernel stack at an exception/syscall boundary; it is not permanently duplicated in `thread_t`.

## `process_t`

Owns:

- address space
- handle table
- token
- Job membership
- thread list
- process lifetime/exit state

## `object_header_t`

Only full executive objects embed it.

Hot internal nodes do not.

## `handle_table_t`

Chunked per-process table. Each handle encodes `generation:index`.

A lookup must:

```text
decode
→ lock target chunk
→ generation check
→ rights check
→ acquire object reference
→ unlock
```

## `io_request_t`

Every async I/O request has a single atomic state machine. Device, file, process/buffer resources remain referenced until completion or cancellation reaches the terminal state.

## `device_t` / `driver_t`

A `device_t` is an instance. A `driver_t` is an implementation.

Device lifetime exists independently from the driver private object, which makes hot-unplug and reset possible.

## `dma_mapping_t`

A DMA mapping owns:

- device
- pinned pages
- direction
- IOVA/segments
- mapping lifetime

No driver stores a CPU physical address and calls it a DMA address.

## `vnode_t` / `file_t`

`vnode_t` represents the filesystem object and owns unified page-cache identity.

`file_t` is an opened instance with position/rights/flags.

## `socket_t`

One socket core supports blocking, nonblocking and asynchronous completion. These are I/O modes, not separate socket implementations.

## `gpu_context_t` / `gpu_allocation_t` / `gpu_fence_t`

The GPU core exports generic lifetime and scheduling objects; vendor-private firmware/channel data remains behind `backend`.

## `security_token_t`, `job_t`, `session_t`

Resource hierarchy:

```text
Session
 ↓
Job
 ↓
Process
 ↓
Thread
```

Authorization uses Token + object security descriptor + requested rights/capability.
