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
