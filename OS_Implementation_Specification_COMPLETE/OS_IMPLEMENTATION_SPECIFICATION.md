# OS Implementation Specification

**Status:** Final consolidated development specification  
**Supersedes:** the earlier 500 design documents when conflicts exist.

---

## 1. What this specification fixes

The previous documents explored many alternatives. This specification makes one choice per subsystem and freezes the development-facing shape:

1. one kernel architecture;
2. one repository tree;
3. one `include/` layout;
4. one set of core structures;
5. one Kernel API/UAPI boundary;
6. one initialization order;
7. one ownership/concurrency model;
8. one coding order.

Historical documents remain useful for rationale but are no longer implementation authority.

---

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


---

# Conflict Resolution / Deprecated Designs

The entries below explicitly retire old alternatives.

| Old / conflicting idea | Final decision | Reason |
|---|---|---|
| Permanent physical=virtual identity mapping | High-half RAM direct map `phys + DIRECT_MAP_BASE`; identity mappings only during controlled early boot | Separates user/kernel layout and MMIO semantics |
| Always use 1 GiB kernel pages | Use largest safe page size per range; split for permissions/cache boundaries | W^X and device/cache correctness take priority |
| Store allocator/region type in arbitrary high PTE bits | VM metadata lives in VMA/VM object/page descriptors; only architecturally safe software bits may be used behind feature-aware helpers | Avoid reserved-bit faults and future CPU feature conflicts |
| Buddy represented as a parent/child node tree | Classic per-order free lists plus O(1) PFN→`page_t` database | Less metadata, simpler coalescing, proven hot path |
| Buddy has free and used lists | Free lists only; allocated blocks are absent from free lists | Avoid duplicated state |
| Free physical block using start with no recorded size | Allocation head stores `order`; `page_free(head)` derives size. Higher-level VM allocations track byte length separately | Fast free while preserving allocator invariants |
| Page tables tell Buddy the allocation size | Page tables and physical allocator are independent | Mapping granularity does not equal allocation ownership |
| Prebuild user page tables for reserved VA | Page-table levels allocated on demand | Avoid massive page-table overhead |
| General syscall replaced by a message/command queue | Direct x86_64 SYSCALL ABI; queueing is used only for asynchronous work | Lower complexity and predictable fast path |
| Every kernel structure is a generic Object | Only user-visible/security/waitable/named resources use full object headers | Avoid object-manager tax on hot internal data |
| Separate Interactive and Background scheduler classes | RT / FAIR / IDLE only; interactivity/background expressed inside FAIR | Avoid class boundary starvation/priority cliffs |
| Global scheduler runqueue | Per-CPU runqueues with targeted load balance | SMP scalability and cache locality |
| `volatile` used as synchronization | Atomics, locks, acquire/release barriers, DMA barriers | `volatile` is not a concurrency primitive |
| Hard IRQ performs heavy completion work | Hard IRQ only acknowledges/snapshots/queues deferred work; network/storage use bounded polling/completion | Bounded interrupt latency |
| Device gets CPU physical address directly for DMA | DMA API returns IOVA/device DMA address; IOMMU isolation by default for complex devices | Isolation and correctness |
| MMIO mapped through normal RAM direct map | `ioremap()`-style dedicated MMIO mapping with UC/WC policy | Correct cache semantics |
| Large IPC payload copied through message queue | Shared memory/object mapping for bulk, message only for control | Avoid repeated copies |
| Dynamic symbol resolution in kernel ELF loader | User-mode dynamic linker | Keeps kernel ABI small |
| Window placement/composition policy in kernel | User-mode Window Server/Compositor | Kernel exposes mechanism, userspace owns policy |
| Audio mixing in kernel | User-mode audio server + kernel DMA streams | Isolation and easier policy evolution |
| DHCP/DNS/Wi-Fi policy in kernel | User-mode Network Manager / resolver | Kernel networking stays mechanism-focused |
| Driver-specific firmware paths | Firmware Manager resolves logical firmware names | Packaging and versioning |
| Immediate global reboot on device hang | Abort → queue reset → function/device reset → offline; reboot only if system integrity is lost | Fault containment |
| Free backing page immediately after PTE removal | PTE update → TLB shootdown/ack → backing reuse | Prevent stale-TLB use-after-free |
| User pointer becomes safe after a pre-check | Exception-safe `copy_from_user`/`copy_to_user` with fixup | Mapping can change concurrently |
| SYSRET unconditionally used for Ring3 return | Validate frame; SYSRET fast path, IRETQ fallback | Canonicality/flags safety |
| Normal kernel stack used for every exceptional case | Dedicated IST for NMI/#DF/#MC | Survive corrupted/overflowed normal stack |
| Full NUMA design in first version | No NUMA; keep CPU topology and interfaces extensible | Scope control |


---

# Unique Final Repository Tree

```text
os/
├── boot/
│   └── uefi/
│       ├── entry.c
│       ├── memory_map.c
│       ├── framebuffer.c
│       ├── acpi.c
│       └── boot_info.h
├── arch/
│   └── x86_64/
│       ├── boot/
│       │   ├── entry.S
│       │   ├── early_paging.c
│       │   └── trampoline.S
│       ├── cpu/
│       │   ├── cpu.c
│       │   ├── feature.c
│       │   ├── gdt.c
│       │   ├── idt.c
│       │   ├── tss.c
│       │   ├── xsave.c
│       │   └── context_switch.S
│       ├── irq/
│       │   ├── exception.S
│       │   ├── exception.c
│       │   ├── apic.c
│       │   ├── ioapic.c
│       │   ├── msi.c
│       │   └── ipi.c
│       ├── mm/
│       │   ├── paging.c
│       │   ├── tlb.c
│       │   ├── pat.c
│       │   └── uaccess.S
│       ├── syscall/
│       │   ├── entry.S
│       │   └── return.c
│       └── time/
│           ├── tsc.c
│           ├── deadline.c
│           └── hpet.c
├── kernel/
│   ├── init/
│   │   ├── main.c
│   │   └── init_stage.c
│   ├── panic.c
│   ├── log.c
│   ├── trace.c
│   ├── irq/
│   │   ├── irq.c
│   │   └── deferred.c
│   ├── time/
│   │   ├── clock.c
│   │   ├── hrtimer.c
│   │   └── timer_queue.c
│   ├── sched/
│   │   ├── core.c
│   │   ├── fair.c
│   │   ├── rt.c
│   │   ├── balance.c
│   │   └── affinity.c
│   ├── process/
│   │   ├── process.c
│   │   ├── thread.c
│   │   ├── exec.c
│   │   └── exit.c
│   ├── object/
│   │   ├── object.c
│   │   ├── handle.c
│   │   └── namespace.c
│   ├── sync/
│   │   ├── spinlock.c
│   │   ├── mutex.c
│   │   ├── wait.c
│   │   ├── futex.c
│   │   └── rcu.c
│   ├── ipc/
│   │   ├── port.c
│   │   ├── shared_section.c
│   │   └── completion.c
│   └── syscall/
│       ├── table.c
│       └── handlers.c
├── mm/
│   ├── early.c
│   ├── physmap.c
│   ├── page_db.c
│   ├── buddy.c
│   ├── percpu_page.c
│   ├── slab.c
│   ├── vmalloc.c
│   ├── vm_space.c
│   ├── vma_tree.c
│   ├── vm_object.c
│   ├── fault.c
│   ├── cow.c
│   ├── page_cache.c
│   ├── reclaim.c
│   └── tlb_shootdown.c
├── io/
│   ├── request.c
│   ├── completion.c
│   └── cancel.c
├── drivers/
│   ├── core/
│   │   ├── bus.c
│   │   ├── device.c
│   │   ├── driver.c
│   │   ├── pnp.c
│   │   ├── power.c
│   │   ├── reset.c
│   │   ├── firmware.c
│   │   └── module.c
│   ├── acpi/
│   │   ├── tables.c
│   │   ├── madt.c
│   │   ├── mcfg.c
│   │   └── fadt.c
│   ├── iommu/
│   │   ├── core.c
│   │   ├── iova.c
│   │   └── x86_backend.c
│   ├── pci/
│   │   ├── ecam.c
│   │   ├── enumerate.c
│   │   ├── bar.c
│   │   └── msix.c
│   ├── nvme/
│   │   ├── controller.c
│   │   ├── admin.c
│   │   ├── queue.c
│   │   ├── io.c
│   │   └── reset.c
│   ├── usb/
│   │   ├── core/
│   │   └── xhci/
│   │       ├── controller.c
│   │       ├── ring.c
│   │       ├── command.c
│   │       ├── transfer.c
│   │       ├── hub.c
│   │       └── interrupt.c
│   ├── input/
│   │   ├── core.c
│   │   └── hid.c
│   ├── net/
│   │   └── <nic-driver>/
│   ├── audio/
│   │   ├── core.c
│   │   ├── hda/
│   │   └── usb_audio/
│   ├── bluetooth/
│   │   └── hci/
│   ├── gpu/
│   │   ├── core/
│   │   │   ├── device.c
│   │   │   ├── context.c
│   │   │   ├── vm.c
│   │   │   ├── scheduler.c
│   │   │   ├── fence.c
│   │   │   ├── memory.c
│   │   │   └── reset.c
│   │   └── vendor/
│   │       └── <vendor-backend>/
│   └── display/
│       ├── core.c
│       ├── atomic.c
│       └── connector.c
├── block/
│   ├── bio.c
│   ├── queue.c
│   ├── merge.c
│   └── flush.c
├── fs/
│   ├── vfs/
│   │   ├── path.c
│   │   ├── file.c
│   │   ├── vnode.c
│   │   ├── mount.c
│   │   └── mmap.c
│   ├── pagecache/
│   │   ├── cache.c
│   │   └── writeback.c
│   └── <nativefs>/
│       ├── super.c
│       ├── inode.c
│       ├── extent.c
│       ├── journal.c
│       └── recovery.c
├── net/
│   ├── core/
│   │   ├── buffer.c
│   │   ├── device.c
│   │   └── poll.c
│   ├── ethernet.c
│   ├── arp.c
│   ├── ipv4.c
│   ├── ipv6.c
│   ├── route.c
│   ├── neighbor.c
│   ├── icmp.c
│   ├── udp.c
│   ├── tcp/
│   │   ├── input.c
│   │   ├── output.c
│   │   ├── state.c
│   │   ├── timer.c
│   │   └── congestion.c
│   ├── socket.c
│   └── firewall.c
├── security/
│   ├── token.c
│   ├── acl.c
│   ├── capability.c
│   ├── random.c
│   ├── crypto.c
│   ├── audit.c
│   └── module_verify.c
├── lib/
│   ├── string.c
│   ├── rbtree.c
│   ├── bitmap.c
│   ├── crc.c
│   └── hash.c
├── include/
│   ├── arch/x86_64/
│   ├── kernel/
│   └── uapi/
├── user/
│   ├── ld/
│   ├── libsys/
│   ├── libc/
│   ├── libthread/
│   ├── libgraphics/
│   ├── libaudio/
│   ├── services/
│   │   ├── init/
│   │   ├── deviced/
│   │   ├── networkd/
│   │   ├── audiod/
│   │   ├── bluetoothd/
│   │   ├── securityd/
│   │   ├── sessiond/
│   │   └── windowd/
│   └── desktop/
├── tests/
│   ├── kernel/
│   ├── user/
│   ├── stress/
│   ├── fuzz/
│   └── hardware/
├── tools/
│   ├── mkimage/
│   ├── symbolize/
│   ├── trace/
│   └── abi-check/
└── build/
```

## Dependency rule

Allowed direction:

```text
arch → kernel mechanisms
drivers → kernel public internal APIs
fs/net → kernel public internal APIs
uapi ← no kernel private headers
user ← uapi only
```

Forbidden:

- `include/uapi` including `include/kernel`.
- generic MM depending on a specific PCI/NVMe/GPU driver.
- VFS depending on a concrete filesystem.
- GPU core depending on one vendor backend.
- core scheduler depending on GUI/window policy.


---

# Unique Final `include/` Layout

```text
include/
├── arch/x86_64/
│   ├── context.h
│   ├── cpu.h
│   ├── interrupt.h
│   ├── paging.h
│   └── syscall.h
├── kernel/
│   ├── api.h
│   ├── base.h
│   ├── list.h
│   ├── rbtree.h
│   ├── refcount.h
│   ├── spinlock.h
│   ├── cpumask.h
│   ├── object.h
│   ├── handle.h
│   ├── wait.h
│   ├── mm.h
│   ├── vm.h
│   ├── sched.h
│   ├── process.h
│   ├── io.h
│   ├── device.h
│   ├── dma.h
│   ├── block.h
│   ├── vfs.h
│   ├── net.h
│   ├── gpu.h
│   ├── security.h
│   └── resource.h
└── uapi/
    ├── all.h
    ├── abi.h
    ├── syscall.h
    ├── mm.h
    ├── io.h
    ├── process.h
    ├── socket.h
    └── gpu.h
```

## Header rule

`kernel/` is an internal ABI and can evolve until code freeze.

`uapi/` is the external ABI and must remain self-contained. Once OS 1.0 ships, syscall numbers and published structure layouts are never silently reused.


---

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


---

# Final Kernel API Contract

Authoritative function prototypes live in `include/kernel`.

## Memory

```c
page_t *page_alloc(uint8_t order, page_alloc_flags_t flags);
void page_free(page_t *head);

page_t *phys_to_page(paddr_t pa);
paddr_t page_to_phys(const page_t *page);

void *kmalloc(size_t size, uint32_t flags);
void kfree(void *ptr);
void *vmalloc(size_t size);
void vfree(void *ptr);
```

Rules:

- `page_alloc()` returns a compound head.
- `page_free()` accepts only the allocation head.
- normal RAM direct-map conversion is invalid for MMIO.
- `kmalloc` is for small kernel objects; large virtually contiguous allocations use `vmalloc`.
- DMA does not use `page_to_phys()` as its final device address.

## Virtual memory

```c
kstatus_t vm_space_create(vm_space_t **out);
kstatus_t vm_map_object(...);
kstatus_t vm_unmap(...);
kstatus_t vm_protect(...);
kstatus_t vm_handle_fault(...);
```

`vm_unmap()` does not release old backing until required TLB invalidations are acknowledged.

## Scheduler

```c
void sched_enqueue(thread_t *);
void sched_wake(thread_t *);
void sched_block_current(void);
void schedule(void);
void sched_tick(uint64_t now_ns);
```

No caller directly edits runqueue membership or `thread->state`.

## Process / Thread

```c
kstatus_t process_create(...);
kstatus_t process_exec(...);
void process_exit(...);
kstatus_t thread_create_user(...);
void thread_exit(...);
```

## Object / Handle

```c
void object_get(void *);
void object_put(void *);
kstatus_t handle_create(...);
kstatus_t handle_lookup(...);
kstatus_t handle_close(...);
```

`handle_lookup()` returns an owned reference on success.

## Wait

```c
kstatus_t wait_on_queue(...);
uint32_t wake_one(...);
uint32_t wake_all(...);
```

Every wait is predicate-based; “check then sleep” without queue registration is forbidden.

## I/O

```c
kstatus_t io_submit(io_request_t *);
kstatus_t io_cancel(io_request_t *);
void io_complete(io_request_t *, kstatus_t, uint64_t);
```

A request can reach a terminal completion exactly once.

## Device / Driver

```c
kstatus_t device_register(device_t *);
kstatus_t driver_register(driver_t *);
```

Data-plane operations use cached `device_ops_t`.

## DMA

```c
kstatus_t dma_map_pages(...);
kstatus_t dma_unmap_checked(...);
void dma_unmap(...);
void dma_sync_for_device(...);
void dma_sync_for_cpu(...);
```

The returned mapping is the only valid device DMA address source.
`dma_unmap_checked()` 只有在 IOMMU 解除成功或设备 domain 已被回收时才返回
`K_OK`；失败时 mapping、pin、后备页和设备引用仍然有效，驱动必须保留它们并重试。

## VFS

```c
kstatus_t vfs_open(...);
kstatus_t vfs_read(...);
kstatus_t vfs_write(...);
kstatus_t vfs_fsync(...);
```

`read`, `write`, and `mmap` converge on the vnode’s single page-cache identity.

## Security

```c
kstatus_t security_check_access(...);
```

Permission checks are performed when creating/duplicating handles and at operations where rights can change or require a distinct privilege.


---

# Final User / Kernel ABI

## Raw x86_64 syscall ABI

```text
RAX = syscall number
RDI = arg0
RSI = arg1
RDX = arg2
R10 = arg3
R8  = arg4
R9  = arg5

RAX = result
RCX/R11 clobbered
```

Negative results are stable error codes. libc converts them to its public convention.

## Entry/return

```text
SYSCALL
→ swapgs/per-CPU state
→ switch to per-thread kernel stack
→ minimal syscall frame
→ O(1) table dispatch
→ return-to-user work
→ SYSRETQ if validated
→ otherwise IRETQ
```

Initial Ring3 entry always uses an IRETQ-compatible frame.

## User pointer rule

Range checks are only preliminary. All actual user memory access uses exception-safe UAccess routines backed by an exception-fixup table.

## Syscall namespace

The stable number ranges are declared in `include/uapi/syscall.h`.

Ranges are kept sparse so a subsystem can grow without renumbering another subsystem.

## Handle ABI

`os_handle_t` is opaque. Applications never interpret generation/index bits.

No kernel pointer, PA, IOVA, driver structure or page descriptor is exposed.

## Versioned structures

Every extensible UAPI input starts with:

```c
os_versioned_header_t {
    size;
    version;
    flags;
}
```

The kernel rejects structures smaller than the fields required by the requested version, ignores documented trailing extension bytes, and requires reserved fields to be zero where specified.

## Data transfer

```text
small arguments/results → usercopy
bulk persistent data    → mapping/shared object
async buffers           → pin/map for request lifetime
GPU data                → GPU allocation handles
```


---

# Final Initialization Order

The order below is authoritative.

## Stage 0 — UEFI loader

```text
Load kernel ELF
→ collect UEFI memory map
→ framebuffer
→ RSDP
→ RNG seed
→ boot device metadata
→ allocate boot_info
→ ExitBootServices
→ transfer to kernel entry
```

After `ExitBootServices`, no Boot Services allocation or console call is legal.

## Stage 1 — x86_64 early entry

```text
temporary stack
→ validate boot_info
→ early serial/framebuffer log
→ feature detection
→ build final GDT
→ IDT
→ TSS + IST
→ establish final kernel page tables
→ switch CR3
→ remove unnecessary identity mappings
```

## Stage 2 — physical memory

```text
normalize firmware ranges
→ reserve kernel/boot/ACPI/runtime/MMIO
→ initialize sparse page sections
→ buddy zones
→ per-CPU page caches for BSP
→ SLAB
→ vmalloc
```

## Stage 3 — time / interrupt core

```text
local APIC/x2APIC
→ IOAPIC routing
→ clocksource calibration
→ invariant TSC if valid
→ TSC-deadline timer if valid
→ hrtimer
→ deferred IRQ work
```

## Stage 4 — SMP

```text
allocate AP stacks/per-CPU areas
→ trampoline
→ AP feature validation
→ GDT/TSS/IDT per CPU
→ local APIC
→ scheduler runqueue
→ online cpumask
```

All CPUs must agree on features that affect shared kernel state.

## Stage 5 — scheduler / core objects

```text
scheduler
→ idle threads
→ object manager
→ handle tables
→ wait/futex
→ process/thread core
→ RCU
```

## Stage 6 — VM / user boundary

```text
user vm_space
→ page fault engine
→ COW
→ UAccess exception fixups
→ syscall MSRs
→ ELF loader
→ create first user process
```

## Stage 7 — platform / device framework

```text
ACPI tables
→ PCI ECAM
→ bus/device/driver core
→ IOMMU
→ MSI/MSI-X
→ firmware manager
```

## Stage 8 — storage

```text
NVMe admin queue
→ I/O queues
→ block layer
→ page cache
→ VFS
→ root filesystem
→ journal recovery
```

## Stage 9 — essential userspace

```text
/init
→ service manager
→ device manager service
→ logging/crash service
```

## Stage 10 — I/O devices

```text
xHCI
→ USB HID/input
→ NIC/network stack
→ network manager
```

## Stage 11 — graphics/desktop

```text
GPU core
→ vendor backend
→ display core
→ window server
→ compositor
→ session/login
→ desktop shell
```

## Stage 12 — media/security/power

```text
audio
→ Bluetooth
→ credential/security services
→ package/update service
→ full power management
→ safe-mode/recovery integration
```


---

# Concurrency, Ownership and Lifetime Rules

## Lock hierarchy

Base order:

```text
CPU-local
→ Scheduler
→ Process/Thread
→ Object/Handle
→ VM/MM
→ VFS/Page Cache
→ I/O
→ Device/Driver
```

A subsystem may define finer local ranks, but it must not invert this global direction.

## Primitive choice

- spinlock: short, non-sleeping state.
- mutex: sleeping ownership; PI-enabled only on selected latency-sensitive paths.
- rwlock: medium-duration read-mostly maps where RCU is not suitable.
- RCU: read-mostly registries/lists with explicit grace-period reclamation.
- atomics: counters/state transitions, not an excuse to create undocumented lock-free algorithms.
- per-CPU: first choice for high-rate counters, queues and caches.

## Lifetime

Every async path follows:

```text
submit
→ take required references
→ publish request
→ hardware/wait
→ complete or cancel
→ unmap/unpin
→ release references
```

## TLB lifetime rule

```text
remove/replace PTE
→ publish page-table change
→ targeted remote invalidation
→ wait for required acknowledgements
→ reuse/free old physical backing
```

## DMA lifetime rule

```text
pin pages
→ create IOMMU mapping
→ publish descriptors
→ dma_wmb
→ doorbell
→ completion
→ stop device access
→ unmap IOMMU
→ unpin
```

## Wait rule

All blocking is predicate based.

Timeout, cancellation and normal wake compete through one atomic waiter state so only one terminal transition succeeds.

## IRQ rule

Hard IRQ:

```text
acknowledge
snapshot minimum status
queue deferred work/poll
return
```

No sleeping, unbounded loop, filesystem work, allocation with blocking semantics, or normal mutex.

## NMI/#DF/#MC

Use dedicated IST stacks and preallocated diagnostics. Normal locks/loggers are not NMI-safe.

## Debug requirements

Debug kernel enables:

- lock rank checking
- refcount underflow/overflow checks
- page/slab poisoning
- stack guards
- VMA overlap checks
- runqueue consistency
- TLB-generation tracing
- double-completion detection
- waiter state validation


---

# Final I/O / Driver / DMA Model

## Control plane vs data plane

Control plane may use Device/Object/PnP abstractions.

Data plane must collapse to:

```text
io_request
→ cached device ops
→ driver queue
→ DMA/MMIO
→ hardware
```

## PnP state

```text
DISCOVERED
→ ENUMERATED
→ BOUND
→ ACTIVE
→ SUSPENDED
→ RECOVERING
→ REMOVING
→ REMOVED
```

`FAILED` is an explicit terminal/recovery state.

## Surprise removal

Every request path must be able to return `K_EDEVREMOVED`.

The device object may outlive physical presence until outstanding references and requests drain.

## Reset escalation

```text
abort request
→ reset queue/engine
→ reset function/device
→ bus recovery if supported
→ mark device offline
```

Unknown-completion writes are never blindly replayed.

## MMIO

- Register BAR: UC.
- Prefetchable framebuffer/VRAM aperture: WC only when the backend permits.
- Normal RAM: WB.
- The same PA must not have conflicting cache aliases.

## Doorbell ordering

```text
descriptor/data writes
→ DMA visibility barrier
→ MMIO doorbell
```

## IOMMU

Complex DMA-capable devices receive a domain by default.

Future ATS/PASID/PRI support is behind IOMMU interfaces; it does not change generic driver DMA APIs.


---

# Final Implementation Order

This is the coding order. Do not implement later desktop features before earlier correctness gates pass.

## Phase 0 — Repository / toolchain

Deliver:

- freestanding cross build
- UEFI image
- linker script
- serial log
- build-id
- QEMU launch script

Gate: boots to a deterministic marker.

## Phase 1 — CPU entry / exceptions

Deliver:

- final GDT/IDT/TSS
- #PF/#GP/#UD diagnostics
- NMI/#DF/#MC IST
- panic/unwind addresses
- feature detection

Gate: synthetic exceptions are correctly classified.

## Phase 2 — Paging / physical memory

Deliver:

- high-half kernel
- RAM direct map
- sparse page database
- Buddy
- compound-head order metadata
- page-table helpers
- PAT/cache helpers

Gate: randomized alloc/free conservation test and map/unmap self-test.

## Phase 3 — SLAB / vmalloc / UAccess base

Deliver:

- SLAB size classes
- kmalloc/kfree
- vmalloc/vfree
- exception table infrastructure

Gate: poison/redzone tests and invalid-access tests.

## Phase 4 — APIC / time / SMP

Deliver:

- APIC/x2APIC
- IOAPIC
- TSC clock
- deadline timer fallback chain
- AP bring-up
- per-CPU areas

Gate: 1/2/4/8 CPU boot matrix.

## Phase 5 — Scheduler / wait

Deliver:

- per-CPU RT/FAIR/IDLE runqueue
- context switch
- blocking/wakeup
- mutex/PI selected paths
- futex
- deferred work

Gate: wait/wake/timeout/cancel stress.

## Phase 6 — VM / process

Deliver:

- vm_space
- augmented VMA tree
- anonymous faults
- COW
- file-backed object abstraction
- PCID/TLB generation/shootdown
- process/thread lifetime

Gate: fork/COW, mmap/mprotect/munmap SMP stress.

## Phase 7 — Syscall / ELF / runtime entry

Deliver:

- SYSCALL entry
- UAccess copy fixup
- return-to-user checkpoint
- SYSRET validated fast path + IRET fallback
- ELF64
- AUXV
- TLS FS.base

Gate: malicious pointer/frame fuzz cannot panic kernel.

## Phase 8 — Object / Handle / Security core

Deliver:

- object refs
- generation handles
- token/ACL/capability
- Job/Session primitives
- shared sections
- completion ports

Gate: stale-handle, access-rights and lifetime stress.

## Phase 9 — Device / PCI / IOMMU / DMA

Deliver:

- Device/Driver/Bus
- ACPI MCFG/MADT
- ECAM
- BAR
- MSI-X
- IOMMU/IOVA
- DMA mapping

Gate: DMA fault isolation and hot-remove simulation.

## Phase 10 — NVMe / Block / VFS

Deliver:

- NVMe admin and multi-I/O queues
- BIO
- block multiqueue
- unified page cache
- VFS
- native filesystem minimal journal
- fsync/flush semantics

Gate: power-cut journal simulation and I/O reset tests.

## Phase 11 — USB / Input

Deliver:

- xHCI
- hubs
- HID
- unified input core

Gate: repeated hotplug and pending-transfer removal.

## Phase 12 — Network

Deliver:

- NIC multiqueue/RSS
- Ethernet/ARP
- IPv4/IPv6
- UDP/TCP
- sockets
- async completion
- firewall hooks

Gate: loss/reorder/reset and multi-core throughput tests.

## Phase 13 — GPU / Display

Deliver:

- GPU object model
- GPU VM/context/queue/fence
- vendor backend
- display atomic commit
- reset/device-lost
- userspace graphics runtime

Gate: command-buffer lifetime, fence and reset stress.

## Phase 14 — Window system

Deliver:

- window protocol
- compositor
- damage/occlusion
- VBlank frame scheduling
- input/focus
- multi-monitor

Gate: input-to-photon trace and compositor restart.

## Phase 15 — Audio / Bluetooth

Deliver:

- audio DMA stream
- userspace audio server
- HDA
- USB audio
- HCI + basic L2CAP/GATT/HID

Gate: underrun/hotplug/controller-reset recovery.

## Phase 16 — Services / package / update / security

Deliver:

- service manager
- session/login
- credential service
- package identity
- signed update
- Secure Boot chain
- audit

Gate: rollback/safe-mode recovery test.

## Phase 17 — Power / recovery

Deliver:

- device suspend/resume
- system suspend
- watchdog
- safe boot
- crash dump

Gate: suspend/resume loop and injected device hang.

## Phase 18 — Hardening / performance

Deliver:

- SMEP/SMAP/NX/W^X
- release signing
- RCU hot registries
- per-CPU caches
- IPI batching
- storage/network/GPU batching
- latency telemetry

Gate: no correctness regression; benchmark evidence required for every fast-path optimization.

## Phase 19 — ABI freeze

Deliver:

- syscall/UAPI compatibility suite
- symbol/build-id archive
- driver API version
- old-app/new-kernel matrix

## Phase 20 — OS 1.0

Only ship when all release acceptance gates pass.


---

# Mandatory Acceptance Gates

## Core

- Buddy randomized split/coalesce.
- SLAB double-free/redzone/poison.
- concurrent COW.
- concurrent `mmap/munmap/mprotect`.
- stale-TLB backing-page reuse stress.
- usercopy racing `munmap`.
- lost-wakeup / timeout / cancel race.
- PI inversion test.
- repeated process/thread/handle destruction.
- syscall return-frame fuzz.

## Driver/DMA

- invalid IOVA fault is contained.
- device removal with I/O pending.
- queue/device reset.
- firmware load failure.
- MSI-X rebinding.
- descriptor/doorbell ordering stress.

## Storage

- NVMe queue wrap.
- reset under load.
- buffered+mmap+direct-I/O coherence.
- fsync + simulated power loss.
- journal replay.
- metadata checksum corruption.

## Network

- packet loss/reorder/duplicate.
- TCP long transfer.
- link down/up.
- NIC reset.
- RSS/multiqueue scaling.
- socket close vs async completion race.

## Graphics

- buffer lifetime until fence.
- process exits with GPU work pending.
- GPU reset/device-lost.
- compositor crash/restart.
- multi-monitor hotplug.
- missed-vblank tracing.

## Release

- QEMU 1/2/4/8 CPU matrix.
- physical hardware matrix.
- filesystem recovery.
- long-run network.
- GPU/window tests.
- security/signature/rollback.
- performance regression.
- complete crash symbolization.
