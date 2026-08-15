# Final Architecture

## Kernel form

Final choice:

```text
High-performance modular monolithic kernel
```

Drivers run primarily in Ring 0. Policy-heavy services remain in userspace.

## Final subsystem layering

```text
UEFI
 ↓
arch/x86_64
 ↓
Kernel Core
 ├─ MM / VM
 ├─ Scheduler / Process
 ├─ Object / Handle / Wait
 ├─ I/O / Device / DMA
 ├─ VFS / Block
 ├─ Network
 ├─ GPU / Display
 └─ Security
 ↓ stable UAPI
System Services
 ↓
Runtime / Libraries
 ↓
Applications
```

## Memory

```text
UEFI Memory Map
 ↓
Early Allocator
 ↓
Sparse Page Database
 ↓
Buddy (free lists by order)
 ↓
Per-CPU page caches + SLAB
 ↓
VM / Page Cache / DMA
```

Key final decisions:

- No allocator tree with parent/child descriptors.
- Buddy keeps free lists only.
- The allocated compound head stores allocation `order`, so `page_free(head)` can infer block size.
- Physical RAM has a high-half direct map. It is not a permanent identity map.
- MMIO is never treated as normal RAM.
- File `read/write/mmap` share one page cache.
- VMA metadata lives in `vm_area_t`/`vm_object_t`, not arbitrary PTE high bits.

## Scheduler

Final classes:

```text
RT
FAIR
IDLE
```

“Interactive” and “background” are **not separate hard classes**. They are expressed as FAIR weights, nice values, latency hints, and wakeup placement. This avoids priority cliffs.

Each CPU owns a runqueue. Normal placement favors locality; load balancing is periodic or triggered by imbalance.

## Object and Handle model

Only resources that need user visibility, security, handles, waitability, naming, or independent lifetime become full kernel objects.

Internal hot-path structures do **not** automatically inherit generic object machinery.

Handle:

```text
generation:32 | index:32
```

A per-process chunked handle table performs type/rights/generation validation.

## Syscalls and IPC

General syscall mechanism:

```text
SYSCALL → O(1) dispatch → service → validated return
```

A command queue does **not** replace syscalls.

IPC roles are separated:

```text
small control          → message port
bulk data              → shared memory / mapping
uncontended sync       → userspace atomics
contended sync         → futex / wait object
async I/O completion   → completion port
```

## Drivers

```text
Bus → Device → Driver → I/O Manager → Hardware
```

Drivers must implement normal removal and surprise removal. Device reset is an expected recovery path.

## DMA

CPU VA, PA, IOVA and GPU VA are separate address domains.

Devices receive DMA addresses only from the DMA/IOMMU API.

## Storage

```text
VFS → Filesystem → Unified Page Cache
→ BIO → Block Multi-Queue → NVMe
```

`fsync()` is a durability contract, not merely “write commands submitted”.

## Graphics

```text
Application
 ↓
User graphics runtime
 ↓
Window Server / Compositor
 ↓
GPU UAPI
 ↓
GPU Core
 ↓
Vendor Backend
 ↓
Firmware/Hardware
```

Display modesetting is separated from 3D submission but belongs to the same graphics subsystem.

## User runtime

Kernel loads ELF segments and the interpreter. Symbol lookup, relocation, libc, malloc, high-level threading, TLS object management and policy live in userspace.
