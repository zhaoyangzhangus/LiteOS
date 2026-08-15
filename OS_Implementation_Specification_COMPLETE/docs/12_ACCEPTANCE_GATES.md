# Mandatory Acceptance Gates

## Core

- Buddy randomized split/coalesce.
- SLAB double-free/redzone/poison.
- concurrent COW.
- concurrent `mmap/munmap/mprotect`.
- stale-TLB backing-page reuse stress.
- usercopy racing `munmap`.
- lost-wakeup / timeout / cancel race.
- PI inversion test.
- repeated process/thread/handle destruction.
- syscall return-frame fuzz.

本轮验证记录：`process_core_self_test()` 已扩展为 32 轮独立的父子进程、用户线程和句柄创建/销毁测试，并验证句柄二次关闭返回 `K_ENOENT`、线程取消后的等待对象信号和进程退出码；QEMU 双核通过 `LITEOS_PROCESS_CORE_OK`。

## Driver/DMA

- invalid IOVA fault is contained.
- device removal with I/O pending.
- queue/device reset.
- firmware load failure.
- MSI-X rebinding.
- descriptor/doorbell ordering stress.

## Storage

- NVMe queue wrap.
- reset under load.
- buffered+mmap+direct-I/O coherence.
- fsync + simulated power loss.
- journal replay.
- metadata checksum corruption.

## Network

- packet loss/reorder/duplicate.
- TCP long transfer.
- link down/up.
- NIC reset.
- RSS/multiqueue scaling.
- socket close vs async completion race.

## Graphics

- buffer lifetime until fence.
- process exits with GPU work pending.
- GPU reset/device-lost.
- compositor crash/restart.
- multi-monitor hotplug.
- missed-vblank tracing.

## 本轮已执行的验证

- `make -f makefile`：完整内核、UEFI loader、PE/ELF 产物构建通过，启用 `-Werror`。
- 普通 QEMU 双核启动：通过 `LITEOS_E1000_HW_OK`、`LITEOS_GPU_CORE_OK`、
  `LITEOS_DISPLAY_CORE_OK` 和 `QEMU UEFI handoff: OK`。
- QEMU NVMe 双核启动 90 秒：通过 `LITEOS_NVME_IO_OK`、
  `LITEOS_NVME_RESET_OK`、`LITEOS_JOURNAL_BLOCK_IO_OK`、
  `LITEOS_DMA_IO_CORE_OK` 和 `QEMU UEFI handoff: OK`。
- NVMe 异步完成路径：请求在提交后保留 request 引用和 DMA 映射，MSI-X
  仅投递 deferred CQ 消费，LAPIC 定时器提供有界轮询；QEMU 双核 NVMe
  回归通过 `LITEOS_NVME_IO_OK`、`LITEOS_NVME_RESET_OK`，普通双核启动也通过。
- IPv6 socket 数据报路径：新增 16 字节端点 ABI、IPv6 bind/connect/send/recv、
  deferred 异步发送和 loopback 自测；QEMU 回归通过
  `LITEOS_SOCKET_IPV6_OK`。
- DMA 描述符/门铃顺序：新增 `dma_wmb()`，已接入 NVMe、xHCI、e1000，普通与
  NVMe QEMU 回归通过。
- GPU 生命周期：覆盖 allocation pin、异步提交、进程引用释放、device-lost、
  fence timeline 完成/失败顺序；通过 `LITEOS_GPU_CORE_OK`。
- e1000 生命周期：reset、poll、link 查询使用独立生命周期锁；通过网卡自检和
  普通 QEMU 回归。
- e1000 RSS 降级路径：QEMU 的老式 e1000 明确报告一条硬件队列，并按 IPv4/IPv6 UDP
  五元组进入多核数量上限的软件队列；流内保持顺序，队列满载保留丢包计数，QEMU
  回归通过 `LITEOS_E1000_RSS_OK`。真实硬件 RSS/多 DMA 队列仍需在支持相应寄存器的
  NIC 上验收。
- 本轮网络回归：完整构建、ABI/宿主测试、QEMU 2 vCPU 25 秒启动和 2 vCPU 1000
  次含 Enter 的输入压力均通过；网络标记包含 `LITEOS_NET_ARP_OK`、
  `LITEOS_E1000_HW_OK`、`LITEOS_E1000_RSS_OK` 和 `QEMU UEFI handoff: OK`。
- Enter 输入压力：`tools/qemu_input_stress.ps1` 发送 1000 个按键并包含 Enter，
  通过 `QEMU input stress with Enter: OK (1000 keys)`。
- USB 根集线器热插拔：detach/attach 各执行一次，收到两次
  `LITEOS_USB_RUNTIME_ROOT_CHANGE`，通过 `QEMU USB root hotplug: OK`。
- QEMU vCPU 矩阵：1/2/4/8 vCPU 均通过 `QEMU UEFI handoff: OK`；2 vCPU 延长到
  50 秒后同时通过 COW、VM、UAccess、display commit 和 SMP user dispatch 标记。

## Ethernet/ARP

- 增加 Ethernet/IPv4 ARP request/reply 的严格构造与解析；e1000 RX 学习 ARP
  sender IP/MAC 到驱动私有缓存，并在配置本机 IPv4 且目标命中时自动回复；通过
  PHY loopback 自测，内核启动输出 `LITEOS_NET_ARP_OK`。动态 IPv4 配置、完整
  网络管理器和外部网络长时稳定性仍需后续验收。
- 增加最小 `net_device_t` 二层设备边界，e1000 通过统一 transmit 回调提供帧发送，
  同步 MTU 和 link 状态；上层不再必须调用 e1000 私有发送实现。

## TCP wire 与 socket 基础

- 增加 IPv4/IPv6 TCP 无选项报文的构造、解析、伪首部校验和与损坏报文拒绝自测；
  内核启动输出 `LITEOS_NET_TCP_OK`。
- e1000 已将已建立 IPv4 TCP 的 ACK/PSH/FIN RX 接入四元组 stream，验证连续序号、
  重复/乱序包处理和 FIN EOF；被动 SYN/ACK/ACK 三次握手已接入 accept 队列，
  数据接收会按连续序号生成 ACK，并按 stream 缓冲余量通告窗口；PHY loopback
  已验证 ACK 经过统一 `net_device_t` 发送边界。
- 主动 IPv4 连接已接入 `SYN_SENT`，支持 SYN、SYN-ACK、最终 ACK、数据发送和统一
  重传轮询；socket 自测覆盖握手、数据确认和重传。
- IPv6 stream、外部网络长传、链路故障恢复、NIC reset 以及网络管理器仍需后续验收。

## Release

- QEMU 1/2/4/8 CPU matrix.
- 本轮已验证并发 COW：用户态载荷创建克隆进程和子线程，在克隆前建立共享同步页；父子进程分别写入同一私有虚拟地址并互相校验，内核输出 `LITEOS_COW_CONCURRENT_OK`。
- 本轮已验证并发 VM 操作：同一地址空间的主线程和 VM worker 在不同固定区域并发执行 `map`、缺页写入、`mprotect`、`munmap` 循环，内核输出 `LITEOS_VM_CONCURRENT_OK`。
- 本轮已验证并发 usercopy/munmap：主线程反复通过 `PORT_SEND` 对固定用户地址执行 `copy_from_user`，worker 同时循环映射、写入并撤销该地址；返回值限制为成功、`K_EACCES` 或队列满的 `K_EAGAIN`，内核输出 `LITEOS_UACCESS_CONCURRENT_OK`。
- QEMU 1、2、4、8 CPU 均通过 `LITEOS_COW_CONCURRENT_OK`、`LITEOS_VM_CONCURRENT_OK`、`LITEOS_UACCESS_CONCURRENT_OK`、`LITEOS_DISPLAY_COMMIT_OK`、`LITEOS_SMP_USER_DISPATCH_OK` 和 `QEMU UEFI handoff: OK`；其余验收门仍需分别完成。
- stale-TLB backing-page reuse：`vmalloc_tlb_reuse_self_test()` 在撤销旧映射并释放旧页后，分配器重新取得物理页并映射回相同虚拟地址，验证读值和新物理地址；内核输出 `LITEOS_TLB_REUSE_OK`。
- lost-wakeup/timeout race：用户态 worker 与主线程重复执行 64 次 futex 唤醒和 1 ms 超时竞争，主线程只接受成功、值不匹配或超时结果，并等待 worker 正常退出；内核输出 `LITEOS_WAIT_RACE_OK`。
- physical hardware matrix.
- filesystem recovery.
- long-run network.
- GPU/window tests.
- security/signature/rollback.
- performance regression.
- complete crash symbolization.

## 已验证证据

- 并发 COW：`kernel/process/user_test_blob.S` 创建克隆进程和子线程，在克隆前建立共享同步页；父子进程按协议分别写入同一私有虚拟地址并互相校验。内核输出 `LITEOS_COW_CONCURRENT_OK`。
- 并发 VM 操作：同一地址空间的主线程和 VM worker 在不同固定区域并发执行 `map`、缺页写入、`mprotect`、`munmap` 循环，worker 完成后写回同步标记。内核输出 `LITEOS_VM_CONCURRENT_OK`。
- QEMU CPU 矩阵：1、2、4、8 CPU 均通过 `LITEOS_COW_CONCURRENT_OK`、`LITEOS_DISPLAY_COMMIT_OK`、`LITEOS_SMP_USER_DISPATCH_OK` 和 `QEMU UEFI handoff: OK`。
- 该记录只覆盖并发 COW；其余验收门仍需分别完成，不能由本条证据替代。
