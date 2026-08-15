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
kstatus_t device_get_by_id(uint64_t device_id, device_t **out);
kstatus_t device_reset(device_t *, uint32_t level);
kstatus_t driver_register(driver_t *);
```

`device_get_by_id()` 返回一个拥有引用的设备对象，调用者必须用 `object_put()` 释放。
Data-plane operations use cached `device_ops_t`; user mode only receives an opaque
handle and versioned numeric information, never a device pointer.

## Firmware Manager

```c
kstatus_t firmware_manager_init(firmware_manager_t *, firmware_resolve_fn,
                                firmware_release_fn, void *context);
kstatus_t firmware_request(firmware_manager_t *, const char *logical_name,
                           uint64_t minimum_version, firmware_blob_t *out);
void firmware_release(firmware_manager_t *, firmware_blob_t *blob);
```

Drivers use logical firmware names only. The provider resolves the name from the
package/filesystem layer; the manager requires a non-zero package identity,
version/generation, and a SHA-256 digest matching the returned bytes. Provider
callbacks run outside the manager spinlock because they may perform I/O. The
caller owns a successful blob until `firmware_release()` is called.

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
