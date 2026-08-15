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
