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
