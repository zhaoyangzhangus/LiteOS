# OS Implementation Specification — Authority and Scope

## 1. Authority

This directory is the **single implementation authority**.

The earlier 500 design documents are historical design inputs. If an earlier document conflicts with this specification, **this specification wins**.

Priority:

```text
OS_IMPLEMENTATION_SPECIFICATION.md
        ↓
include/ headers in this package
        ↓
docs/ subsystem rules in this package
        ↓
earlier 1–500 documents only as rationale/history
```

## 2. Target

- Architecture: x86_64 only.
- Firmware: UEFI only.
- Kernel: modular monolithic kernel.
- Multiprocessing: SMP.
- NUMA: explicitly not implemented in the first release.
- Paging: 4-level, 48-bit canonical VA for the first release.
- User ABI: ELF64 + stable syscall/UAPI.
- Desktop goal: GPU-accelerated compositor, input, audio, network, storage, package/security/service stack.

## 3. Design principle

```text
Correctness → Ownership → Observability → Concurrency
→ Security → Performance → Compatibility
```

No fast-path optimization may bypass a lifetime, ordering, permission, or recovery contract.
