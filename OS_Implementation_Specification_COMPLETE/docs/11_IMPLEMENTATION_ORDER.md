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
