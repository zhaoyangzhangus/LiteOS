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
