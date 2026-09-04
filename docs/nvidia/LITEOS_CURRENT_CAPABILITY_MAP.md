# LiteOS 当前能力地图（Phase 0 基线）

> 本文件是实施规范要求的 Phase 0 产物，也是后续 NVIDIA 适配的能力基线。
> READY 只表示已有 LiteOS 原生基础设施可以作为适配器输入；它不表示 NVIDIA
> 硬件、OpenRM 或 Linux ABI 已经可用。

## 1. 基线身份与验证

| 项目 | 结果 |
|---|---|
| 实施规范 | C:/Users/Admin/Downloads/LiteOS_GB205_OpenRM_OpenGL_Implementation_Spec_v1.0.md |
| 规范 SHA-256 | 0F8E5C841F2FC2DD2F8D211D327C8E3727556492A0921D91A92C36F52B14DC4D |
| Git HEAD | f14eb07a27e6294e33359d29a55c9f0893647558 (f14eb07 1) |
| 分支 | main |
| 基线构建 | make：PASS，退出码 0；生成 build/esp，最后输出 ESP image prepared at build/esp |
| 基线测试 | make test：PASS，退出码 0；所有列出的 host-side 测试均输出 ok |
| NVIDIA vendor 源码 | 当前 LiteOS 工作树未发现 NVIDIA 595.84/OpenRM 源码；Phase 0 未导入、未编译、未修改 |
| QEMU/NVIDIA 硬件结论 | 本阶段未将其计入 Gate P0；没有对 10de:2f04 做硬件支持宣称 |

make test 实际覆盖：sha256、mm-api、memory-map、fat32、cache、
primitives、bluetooth、firmware、audiod 和 uapi-abi。

基线审计开始时工作树已有以下用户修改；Phase 0 未改动、未暂存、未覆盖：

.vscode/c_cpp_properties.json
.vscode/tasks.json
run-qemu-windows.ps1
tools/run-qemu-auto.ps1
tools/stop-qemu-auto-debug.ps1

## 2. 架构边界与当前实现归属

实施规范的所有权约束在当前基线中按以下方式落地：

| 所有者 | 当前/后续责任 |
|---|---|
| LiteOS | 系统 RAM、CPU 页表与 VM、PCI/ECAM、BAR 访问、IRQ/APIC、DMA/IOMMU、线程/调度、同步、VFS、原生 Ring3、原生窗口与显示 |
| NVIDIA OpenRM（后续只作为 vendor 代码接入） | GB205 HAL、RM 生命周期、GSP/RPC、PMA/VRAM、GPU VA/GMMU、FIFO/channel、CE copy 以及 NVIDIA GPU 状态 |
| NVIDIA 官方 userspace（后续） | GLVND、EGL、OpenGL/GLX/GLES 及其 Linux x86-64 userspace ABI |
| Linux compatibility personality（后续） | Linux syscall、/dev/nvidia*、Linux fd/mmap/futex/thread/signal 语义到 LiteOS 原语的翻译 |

因此，当前地图不把 LiteOS 的通用 GPU 对象当作 GB205 后端，也不新增与
OpenRM 竞争的 VRAM allocator、GPU VA、GMMU 或 GSP RPC owner。

## 3. 当前代码结构清单

| 子系统 | 当前实现与关键入口 | 基线判断 |
|---|---|---|
| MM/CPU VM | include/kernel/mm.h；kernel/mm/phys/page_alloc.c、page_db.c、percpu_page.c、kmalloc.c、vmalloc.c、physmap.c；include/kernel/vm.h；kernel/mm/map.c、protection.c、fault.c、vm_space.c、object.c | 真实的 buddy/page/slab、direct map、CPU 页表、VMA、fault、COW 和 device mapping；不是 NVIDIA VRAM/GMMU |
| DMA/IOMMU | include/kernel/dma.h、include/kernel/iommu.h；kernel/drivers/core/dma.c；kernel/drivers/iommu/core.c | 已有页 pin、物理段、可选 Intel VT-d IOMMU、方向/访问检查和同步屏障；没有 NVIDIA GPU DMA/RM adapter |
| PCI/BAR | include/kernel/pci.h；kernel/drivers/pci/enumerate.c | 已有 ACPI ECAM 枚举、BAR sizing、设备/class 查找、memory/bus-master；ROM/VBIOS API 不存在 |
| MMIO | include/arch/x86_64/paging.h；xHCI/IOMMU/display/NVMe 各自的 UC/WC 映射与 volatile accessor | 有页级 cache mode 和 MMIO 映射原语；没有统一的 nv_port_mmio_* BAR0/BAR1 适配层 |
| IRQ/deferred | include/kernel/irq.h、kernel/arch/x86_64/irq/irq.c；include/kernel/deferred.h、kernel/irq/deferred.c；APIC 在 kernel/arch/x86_64/irq/apic.c | 有向量注册/注销、EOI、持久 Ring0 bottom-half worker、IRQ-safe deferred 队列；没有 NVIDIA IRQ 生命周期封装 |
| 调度/线程 | include/kernel/sched.h、kernel/sched/*、include/kernel/process.h、kernel/process/thread.c | 有 SMP 调度、阻塞/唤醒、内核/用户线程及线程终止；没有 Linux kthread/clone personality |
| 同步/时间 | include/kernel/spinlock.h、mutex.h、wait.h、timer.h；kernel/sync/*、kernel/time/hrtimer.c | 有 C11 atomic、spinlock、PI mutex、wait queue、completion、futex、TSC timer；rwlock 只有类型，semaphore 无实现 |
| VFS | include/kernel/vfs.h；kernel/fs/vfs/*、kernel/fs/nativefs/*、kernel/fs/pagecache/* | 有原生 VFS、backend file、FAT32、page cache、kernel/user I/O；没有 Linux namespace 或 /dev/nvidia* |
| Ring3/ELF | include/kernel/process.h、elf_loader.h；kernel/process/process.c、thread.c、exec.c；user/runtime/ld.c | 有原生 Ring3 生命周期、ELF64/PT_LOAD/PT_INTERP、栈和 auxv 子集；其 ABI 是 LiteOS 自有 ABI |
| syscall | include/uapi/syscall.h；kernel/syscall/handlers.c 及各 handler | 有原生 syscall 表、VM/handle/futex/file/window/GPU 等入口；没有 Linux syscall number/personality dispatch |
| window/buffer | include/kernel/window_server.h、display.h、shared_section.h、vm.h；kernel/graphics/window.c、buffer.c、compositor.c、render.c | 有 CPU shared-section surface、窗口映射、合成和 GOP/display 提交；没有 GPU-backed HBUFFER 或 glReadPixels bridge |

## 4. nv_port、Linux 与 Window 能力矩阵

状态含义：

- READY：已有原生基础设施可以直接作为适配器输入；不表示 NVIDIA 硬件或 Linux ABI 已完成。
- PARTIAL：已有部分原语，但目标所需的适配层、语义或覆盖范围尚未闭合。
- MISSING：当前没有可用实现。

| 能力 | 状态 | 现有 LiteOS 原语与精确位置 | 缺口及边界 |
|---|---|---|---|
| 系统 RAM page/heap | READY | include/kernel/mm.h:71-82；page_alloc/page_free；kmalloc/kzalloc/kfree；phys_to_page/page_to_phys/phys_to_direct | 可作为 RM host-memory adapter 的输入；不用于创建 VRAM owner |
| memory descriptor、连续/对齐/zero/cache | PARTIAL | PAGE_ALLOC_ZERO、PAGE_ALLOC_DMA32、order allocation；include/arch/x86_64/paging.h:17-47 的 WB/WC/UC | 没有规范要求的 nv_port_mem descriptor、统一 cache/lifetime contract；页 pin 也不是公开 MM API |
| DMA / scatter-gather / IOMMU | PARTIAL | include/kernel/dma.h；kernel/drivers/core/dma.c:136-241、:393-500；iommu_map_pages/iommu_unmap_pages | 现有接口以页数组为输入，IOMMU 是系统 VT-d；缺少 NVIDIA 设备 DMA domain、coherent allocation、完整 SG adapter |
| PCI 枚举、配置、BAR | PARTIAL | pci_ecam_init、pci_find_device、pci_find_class；kernel/drivers/pci/enumerate.c:229-357；BAR 解析 :105-177 | 可支撑通用设备发现；尚无 NVIDIA 设备 probe/driver 绑定、PCI ROM 读取和 NV 专用 port API |
| MMIO UC/WC 映射与读写 | PARTIAL | x86_map_page、x86_unmap_page、x86_translate_page；xHCI 示例 kernel/drivers/usb/xhci/lifecycle.c:116-169 | 有页级基础但没有统一 nv_port_mmio_map/read32/read64/write API；BAR0/BAR1 属性和生命周期尚未定义 |
| MSI / MSI-X | PARTIAL | pci_msix_configure、pci_msix_mask、pci_msix_disable、pci_msi_configure；kernel/drivers/pci/enumerate.c:555-673 | 现有实现可配单个 MSI/MSI-X entry；无 NVIDIA vector allocator、per-device routing/synchronize/mask lifecycle |
| IRQ / deferred work | PARTIAL | irq_register/irq_unregister/x86_irq_dispatch；kernel/arch/x86_64/irq/irq.c:46-121；deferred_schedule 系列和 worker kernel/irq/deferred.c:275-520 | 通用 hard-IRQ/bottom-half 已有；缺少 NVIDIA top-half ACK、teardown drain、同步和多 vector ownership adapter |
| atomic | READY | include/kernel/base.h 引入 stdatomic.h；C11 atomic_* 在 MM、DMA、调度、同步和对象代码中使用 | 原子语义可复用；vendor ABI 所需 wrapper/命名尚未闭合 |
| spin / mutex / event(completion) | READY | include/kernel/spinlock.h:5-37；kmutex_*；wait_queue_*、completion_*；kernel/sync/mutex.c、wait.c | 现有 wait/lock 可作为适配输入；需在后续逐个落实 vendor sleep/IRQ/lifetime 语义 |
| rwlock / semaphore | PARTIAL | rwlock_t 出现在 include/kernel/spinlock.h 和 handle/VFS 对象结构；wait queue 可构成阻塞等待 | 未发现 rwlock_* 公共操作；未发现 semaphore header/API/实现，不能宣称 READY |
| timer / delay | PARTIAL | timer_create/timer_cancel/timer_poll；kernel/time/hrtimer.c:97-170；x86_read_tsc、x86_timeout_ns_to_tsc、x86_tsc_to_ns | 有 TSC deadline/可等待 timer；未发现独立 udelay/mdelay port API，vendor 忙等/可睡眠分类待后续定义 |
| kernel threads / worker | PARTIAL | sched_start_thread、sched_publish_blocked、sched_wake；thread_create_user 系列；deferred 持久 worker | LiteOS scheduler/thread substrate 已有；缺少与 NV 生命周期匹配的 generic kthread create/stop/join/drain adapter |
| VM map / protect / device map | PARTIAL | include/kernel/vm.h:121-186；vm_map_object、vm_unmap、vm_protect、vm_handle_fault、vm_object_create_device | CPU VM 和 device physical mapping 已有；没有 Linux mmap 语义，也不能用于代替 GPU VA/GMMU |
| user copy | READY | include/arch/x86_64/uaccess.h；x86_user_range_valid、copy_from_user、copy_to_user；kernel/arch/x86_64/mm/uaccess.c 与 fixup 表 | 有范围校验、SMAP/fault fixup 和 u32 accessor，可供 syscall/port 使用 |
| pin pages / unpin pages | PARTIAL | kernel/drivers/core/dma.c:93-101、:164-168 使用 refs、private_data 和 PAGE_PINNED | 仅 DMA core 内部 pin；无公开、可供 vendor 直接调用的 pin_pages/unpin_pages API |
| firmware loader | PARTIAL | include/kernel/firmware.h；firmware_manager_init、firmware_request、firmware_release；kernel/drivers/core/firmware.c；tests/kernel/firmware_test.c | 有 provider/版本/包 ID/释放约束和测试；没有 NVIDIA GSP firmware 路径、签名/版本策略及具体 provider |
| VBIOS / PCI option ROM | MISSING | 当前 include/kernel/pci.h 与 PCI driver 目录没有 ROM/VBIOS 读取入口 | 后续必须接入真实 VBIOS 来源；不能以 QEMU VGA ROM 或 framebuffer 推断 GB205 VBIOS 能力 |
| VFS / firmware file access | PARTIAL | vfs_open、vfs_read、vfs_close；vfs_mount_fat32；vfs_vm_file_ops；include/kernel/vfs.h:41-114 | 原生文件读取可复用；缺少 /compat/linux、Linux mount/path namespace、/lib/firmware 和 /dev/nvidia* 语义 |
| Ring3 process | READY | process_create、process_fork、process_abort、process_exec；include/kernel/process.h:68-90；process/thread/VM/handle 绑定 | 原生 Ring3 生命周期真实存在；READY 不表示 Linux process ABI 已存在 |
| ELF loader | PARTIAL | process_load_elf_image、process_exec_from_vfs；kernel/process/exec.c:212-365、:651-829；include/kernel/elf_loader.h | ELF64/PT_LOAD 及权限/边界校验真实存在；只保证 LiteOS loader contract，Linux ELF/glibc contract 仍缺 |
| PT_INTERP | PARTIAL | validate_elf 识别并校验 ELF_PT_INTERP：kernel/process/exec.c:238-258；读取/校验/映射 interpreter：:583-603；self-test 使用 /lib/ld-liteos.so.1：:928-978 | 只支持当前 LiteOS 的单级 interpreter 方式；没有 Linux loader 搜索、Linux ABI 或官方 userspace 运行环境 |
| auxv | PARTIAL | build_initial_stack 写入 AT_PHDR/PHENT/PHNUM/PAGESZ/BASE/ENTRY/RANDOM 与 OS_AUX_LITEOS_FD_MAP：kernel/process/exec.c:492-502；user/runtime/ld.c:489-545 读取核心项 | 已有真实初始栈/auxv 子集；不是完整 Linux/glibc auxv contract，fd map 仍是 LiteOS 扩展 |
| Linux syscall personality | MISSING | 当前仅有 OS_SYS_* 原生号码：include/uapi/syscall.h:5-121；原生固定表：kernel/syscall/handlers.c:87-321 | 没有 Linux syscall number dispatch、errno/ABI personality、arch_prctl、ioctl/openat 等 Linux 入口 |
| fd table | PARTIAL | Kernel handle_table_*：include/kernel/handle.h、kernel/object/handle.c；exec 可传 os_exec_fd_map_t：include/uapi/process.h | 原生 handle table 和 userspace descriptor map 存在；没有 Linux per-process integer-fd kernel personality、poll/dup/close-on-exec 全语义 |
| mmap | PARTIAL | VM_MAP/UNMAP/PROTECT 原生 syscall；user/libc/mman.c；kernel/syscall/vm.c | 原生地址空间映射可复用；缺少 Linux flags、fd/offset、MAP_SHARED/private、VMA error/errno 兼容语义 |
| futex | PARTIAL | futex_wait/futex_wake：include/kernel/futex.h、kernel/sync/futex.c；原生入口 OS_SYS_FUTEX_WAIT/WAKE | 物理 user-word key、超时和唤醒已实现；缺少 Linux futex op 集合、robust list、PI/requeue/time64 等语义 |
| clone / TLS | PARTIAL | thread_create_user 系列、thread_start、thread_register_user_stack：include/kernel/process.h:125-140；kernel/process/thread.c:39-185；FS base 在 kernel/arch/x86_64/cpu/cpu.c:391-393 | 原生用户线程和 FS/TLS base 存在；没有 Linux clone/clone3 flags、set_tid、TLS/arch_prctl ABI |
| signals | PARTIAL | 原生 1..64 signal、action/mask/send/return：include/uapi/signal.h、kernel/syscall/process.c、handlers.c:177-180 | 有原生 signal frame、mask、restorer；没有 Linux rt_sigaction/rt_sigprocmask/完整 signal frame 和线程组语义 |
| Window HBUFFER（CPU readback 目标） | PARTIAL | shared_section_t：include/kernel/shared_section.h；window_server_create/window_server_map 和 VM_OBJECT_FLAG_WINDOW_SURFACE：kernel/graphics/window.c:418-730；CPU compositor：kernel/graphics/buffer.c、render.c | 有可映射 CPU WB shared surface 和现有 compositor；没有 glReadPixels → HBUFFER bridge、GPU-backed buffer backend 或 fence 绑定 |
| generic GPU allocation/fence（面向真实 NVIDIA） | MISSING | kernel/drivers/gpu/core/core.c:157-206 分配系统 RAM 并递增软件 GPU VA；:358-408 仅把 submission 放入 deferred 后 signal fence | 这是通用软件对象/时序自测，不是 GB205 后端；不能作为硬件成功或 NVIDIA capability 证据 |
| OpenRM / HAL / GSP / RPC | MISSING | 当前工作树无 NVIDIA/OpenRM 源码或对应 kernel/drivers/gpu/nvidia/port 层 | Phase 1 才做 595.84 source inventory；Phase 0 不导入、不编译、不改 vendor |
| PMA / VRAM / GPU VA / GMMU | MISSING（且禁止在 LiteOS 侧自实现） | 当前只有 CPU VM 的 gpu_va_t 类型和 generic GPU test object | 所有权必须留给 OpenRM；LiteOS 只能提供 host primitives 和调用边界，不能新增竞争 allocator/GMMU |
| EGL / OpenGL / GLVND | MISSING | 当前未发现 NVIDIA userspace stack 或 /dev/nvidia* Linux ABI | 必须等待 OpenRM、Linux personality 和后续 EGL/GL phases；Phase 0 不验证 triangle/readback |

## 5. 关键结论

1. LiteOS 的系统级底座足以作为后续 nv_port_* 适配的输入：真实 page/VM、DMA/IOMMU、PCI、IRQ/deferred、同步、VFS、Ring3 和原生 ELF 均已存在。
2. “存在底座”与“完成 NVIDIA/Linux 能力”严格分开。尤其是 kernel/drivers/gpu/core 当前只做系统 RAM backing、软件 GPU VA 和 deferred fence completion，不能计为 GB205 硬件路径。
3. Linux compatibility 是缺口最大的边界：Linux syscall personality、Linux fd/mmap/futex/clone/TLS/signal 语义目前均未闭合。
4. Window 侧已经有 CPU shared surface 和 compositor，但规范要求的 glReadPixels → HBUFFER → existing compositor 尚未实现；后续应扩展通用 buffer reference，不把 NVIDIA private pointer 暴露给 Window/Ring3。
5. VBIOS、OpenRM、GSP/RPC、PMA/VRAM、GPU VA/GMMU、NVIDIA EGL/OpenGL 均是当前 MISSING；本阶段没有用 fake-success 方式填充这些状态。

## 6. Phase 0 边界与 Gate P0

Phase 0 已明确不做以下工作：

- 不修改 scheduler、MM、VM、window/compositor 的算法或所有权；
- 不导入、编译或修改 NVIDIA 595.84 vendor 源码；
- 不在 LiteOS 侧实现 VRAM PMA、GPU VA、GMMU 或 GSP RPC；
- 不把 generic GPU 软件 completion、自测或 QEMU VGA 当作 NVIDIA 硬件成功；
- 不提前进入 Phase 1 vendor import/build graph 或后续 port/ABI/OpenGL/window phases。

Gate P0 记录：

- [x] baseline build PASS：make
- [x] baseline tests PASS：make test
- [x] capability map 已作为唯一 Phase 0 交付物提交
