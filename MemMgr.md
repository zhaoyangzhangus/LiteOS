# LiteOS 物理内存管理

当前物理内存路径为：

```text
UEFI Memory Map -> Buddy -> Page Descriptor -> SLAB
```

## 1. Loader 合并可用内存

Loader 在最后一次成功调用 `ExitBootServices` 后整理内存图。以下类型会作为操作系统可用内存处理：

- `EfiConventionalMemory`
- `EfiBootServicesCode`
- `EfiBootServicesData`
- `EfiLoaderCode`
- `EfiLoaderData`

只有物理地址连续、虚拟地址连续（或都为零）且属性相同的相邻描述符才会合并。合并后的类型统一写为 `EfiConventionalMemory`。内核镜像、BootInfo、内存图缓冲区、命令行、Loader 名称、framebuffer、Runtime Services、System Table 和 BootstrapStack 会在 Buddy 初始化时重新登记为保留区。

## 2. Buddy 分配器

`order 0` 对应一个 4 KiB 物理页，`order n` 的块大小为：

```text
4096 << n
```

同阶 Buddy 的地址通过异或计算：

```text
buddy_address = address ^ block_size(order)
```

初始化时，分配器遍历所有可用内存范围，并按 4 KiB 对齐切分。每段内存从高阶到低阶加入空闲链表；已经登记的保留区先被排除，因此不会把内核或启动数据交给普通分配请求。

分配流程：

1. 根据请求阶数查找不小于目标阶数的非空链表；
2. 从高阶块逐级拆分，右半块加入对应低阶空闲链表；
3. 返回左半块，并记录原始地址、阶数和占用状态。

释放流程：

1. 校验块地址、阶数和分配状态；
2. 计算同阶 Buddy；
3. 若 Buddy 空闲且属于同一内存范围，则从链表摘除并合并；
4. 重复向更高阶合并，最后加入空闲链表。

`liteos_buddy_alloc_bytes` 会向上取整到最小的 2 的幂次块，释放时必须传入原始的 `LITEOS_PHYSICAL_BLOCK`，不能自行修改地址或阶数。

## 3. 启动分页

内核首先建立覆盖低 4 GiB 的恒等映射，每个页目录项映射一个 2 MiB 大页，供启动代码、BootInfo、framebuffer 和低地址物理页访问。当前启动页表不是最终虚拟内存系统，后续应替换为完整的物理映射和 4 KiB PTE 管理。

用户地址空间测试使用 1 TiB 虚拟基址和 2 MiB 按需映射：

- 低地址内核映射保持 `U/S=0`，用户态不可访问；
- 用户区使用 `U/S=1`、`R/W=1`；
- 首次访问由页故障处理程序建立映射；
- 非用户区或权限错误不恢复，直接进入故障路径。

正式实现还需要 VMA、4 KiB 页面、COW、按页回收、NX/W^X 和完整的地址空间复制策略。

## 4. Page Descriptor

每个可管理物理页对应一个描述符，记录物理地址、存在状态、保留状态、元数据状态、引用计数、映射计数和私有计数。描述符本身由 Buddy 提供的内存建立，并标记为元数据保留区，避免描述符覆盖自身。

当前通过 UEFI 内存范围线性查找页描述符；随着物理内存规模扩大，应改为二分查找或物理页号索引。

## 5. SLAB

SLAB 从 Buddy 获取页块，把固定大小的内核对象组织为空闲链表，适合 Thread、Object Header、IRP 等高频对象。当前对象管理器和 I/O 管理器使用 SLAB，并提供显式销毁接口，避免自测或服务重启时遗留整页。

## 6. ABI 与栈

UEFI 入口和 Loader 内部使用 Microsoft x64 ABI。Loader 在装载内核时分配并切换到独立的 2 MiB BootstrapStack，随后把 `BootInfo` 放入 `RDI`，以 SysV AMD64 ABI 调用内核入口。内核内部统一使用 SysV ABI，不预留 Microsoft ABI 的 shadow space。

内核 GDT/TSS 还会从 BootstrapStack 的低 64 KiB 划出中断栈，剩余高地址区域作为启动 C 栈，以避免用户态运行期间 LAPIC 中断破坏用户栈。

## 7. 公共接口

定义位于 `include/buddy.h`、`include/page.h` 和 `include/slab.h`，主要接口为：

```c
BOOLEAN liteos_buddy_init(const LITEOS_BOOT_INFO *boot_info);
BOOLEAN liteos_buddy_alloc(UINT32 order, LITEOS_PHYSICAL_BLOCK *block);
BOOLEAN liteos_buddy_alloc_bytes(UINT64 bytes, LITEOS_PHYSICAL_BLOCK *block);
BOOLEAN liteos_buddy_free(LITEOS_PHYSICAL_BLOCK *block);
BOOLEAN liteos_page_init(const LITEOS_BOOT_INFO *boot_info);
BOOLEAN liteos_slab_init(LITEOS_SLAB_CACHE *cache, UINT32 object_size);
```

QEMU 启动测试会依次输出 `LITEOS_BUDDY_OK`、`LITEOS_PAGING_OK`、`LITEOS_PAGE_OK`、`LITEOS_SLAB_OK` 和 `LITEOS_ADDRESS_SPACE_OK`。

文件系统使用 `include/cache.h` 提供的固定容量块缓存。缓存以 LBA 为键，采用最近最少使用替换；写入先标记脏块，缓存淘汰或文件系统卸载时回写底层块设备。

## 8. 当前虚拟内存后备对象

正式 VMA 实现位于 `kernel/mm/vm_space.c`，匿名对象和文件对象都采用按页缺页建立映射：

- `VM_OBJECT_ANON` 的页由匿名页表按对象页号索引，首次访问时分配零页；
- 共享文件映射直接使用 VFS 的 vnode 页缓存，写入后由页缓存脏标记和 `fsync` 回写；
- 私有可写文件映射拥有独立的匿名 `private_object`。首次写入时复制文件页，之后只修改 shadow；
- `fork` 会克隆匿名对象和文件 shadow，并把父子页设为 COW。写缺页完成复制后，才重新安装可写 PTE；
- VMA 拆分、`mprotect`、`munmap` 和地址空间销毁都会同步增加或释放 shadow 对象引用；
- PTE 修改必须经过 TLB shootdown，收到全部在线 CPU 的确认后才允许释放或复用后备页。

因此，私有文件映射的写入不会污染文件页缓存；父子进程也不会因为关闭文件句柄或退出进程而提前释放仍在异步路径中使用的后备页。
