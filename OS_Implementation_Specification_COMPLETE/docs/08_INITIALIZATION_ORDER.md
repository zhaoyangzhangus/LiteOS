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

The loader copies boot-device metadata before leaving Boot Services. The
handoff contains the device-path byte length and SHA-256, plus the partition
number/LBA range and partition signature when a hard-drive device-path node is
present. The kernel must use these copied values and must not dereference a
UEFI handle or protocol after `ExitBootServices`.

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
