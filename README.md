# LiteOS UEFI x86_64

LiteOS 是一个面向 x86_64 的 UEFI 操作系统实验工程。当前启动链路不使用 initrd，而是由 UEFI 加载器直接读取并装载 ELF64 格式的 `kernel.elf`。

ESP 目录结构如下：

```text
EFI/BOOT/BOOTX64.EFI       UEFI 启动加载器
EFI/LITEOS/kernel.elf      ELF64 高半内核
EFI/LITEOS/loader.conf     加载器配置
```

## 当前状态

已经实现并可在 QEMU 中启动验证的部分包括：

- UEFI 启动、ELF64 PT_LOAD 装载、高半地址映射、SHA-256/RSA-2048 校验和 BootInfo 交接；
- UEFI 内存图整理，合并 `ConventionalMemory`、Boot Services 和 Loader 内存；
- 4 KiB Buddy、Page Descriptor、SLAB，以及启动保留区登记；
- GDT、IDT、TSS、LAPIC 定时器和独立内核栈；
- 对象管理、generation 句柄、调度队列、上下文切换测试；
- IRP、驱动/设备注册、PCI 枚举、NVMe 管理/多 I/O 队列与 xHCI 队列；NVMe 已覆盖队列环回、复位重建和复位后读盘自测；
- RAMFS、块设备层、固定容量 LRU 块缓存、FAT32 LFN 创建/删除与原位写入，以及带 journal/fsync/掉电回放和元数据校验的 LiteFS 原型；
- 软件 GPU 管理器、GOP framebuffer 窗口合成和输入事件队列；
- SYSCALL/SYSRET、固定大小 IPC、按需用户地址空间和 ring3 启动测试；
- 从 VFS 加载 `/init`，由用户态 init 创建并等待 `deviced`、`logd`、`audiod`、`crashd` 四个服务进程，验证句柄回收和地址空间释放；
- 已接入版本化设备 UAPI：`deviced` 先通过 `DEVICE_ENUMERATE` 发现公开设备，再执行 `DEVICE_OPEN/DEVICE_CONTROL(QUERY)` 并关闭 capability 句柄，设备对象类型与 socket 类型隔离；
- `audiod` 已作为独立 freestanding ELF 用户服务装载，使用用户态 mixer 混音后通过 `AUDIO_OPEN/AUDIO_CONTROL` 驱动 HDA BDL/DMA 或 USB Audio 等时端点；HDA 已完成 CORB/RIRB codec 探测及基本转换器 stream/tag/format 配置；
- VS Code 仅调试内核代码的 QEMU/GDB 配置。

这些模块中的部分是可运行的基础骨架，硬件驱动、服务的实际业务逻辑和完整桌面用户态仍在继续实现。

## 工具链与构建

当前 Makefile 默认使用 `x86_64-w64-mingw32-gcc`。UEFI Loader 产物是 PE/COFF，内核链接产物先生成 PE/COFF，再由 `objcopy` 转成 Loader 使用的 ELF64 文件。ABI 分工不同：UEFI 入口和 Loader 使用 Microsoft x64 ABI，Loader 跳转到内核时把 `BootInfo` 放入 `RDI`；内核及其内部调用统一使用 SysV AMD64 ABI。

`x86_64-elf-gcc` 是另一套面向 ELF/裸机的交叉工具链，Windows 上可以自行构建或通过其他交叉编译环境提供。当前 Loader 已实现 ELF64 程序头校验、PT_LOAD 装载、高半映射和入口跳转；若切换工具链，仍需保持 `kernel_entry(BootInfo *)` 的 SysV ABI、链接地址和节布局一致。

在 WSL 中建议安装目标交叉工具和主机测试工具：

```bash
sudo apt update
sudo apt install build-essential gcc-mingw-w64-x86-64 \
    binutils-mingw-w64-x86-64 gdb-multiarch \
    qemu-system-x86 qemu-utils ovmf
```

`CC/LD/OBJCOPY/OBJDUMP` 继续使用 `x86_64-w64-mingw32-*` 生成 UEFI、内核和用户服务；
`HOSTCC` 默认使用 WSL 的 `gcc`，只用于 `build-id`、ABI 检查和单元测试，因此测试可以直接在
WSL 执行，不依赖 Wine。工程可以直接放在 WSL 的 Linux 文件系统中，例如 `/home/zzy/LiteOS`：

```bash
cd /home/zzy/LiteOS
./build-wsl.sh -B esp DEBUG=1
./build-wsl.sh test
```

`DEBUG=1` 生成 `-O0 -g3` 的调试镜像；默认构建使用 `-O2`。生成的 ESP 位于 `build/esp`。

`GNUmakefile` 是默认入口；`make`、编译器、测试工具、QEMU 和 GDB 均来自 WSL。

## QEMU 验证

图形模式启动：

```bash
./build-wsl.sh -B esp DEBUG=2
./run-qemu.sh --keep-open
```

WSLg 会显示 Linux QEMU 的图形窗口。无 KVM 权限时脚本会自动退回 TCG 软件模拟；获得
`/dev/kvm` 权限后会自动使用 KVM。

运行统一通过 VS Code 的 F5 启动；`LiteOS: QEMU (F5 auto detect)` 会按当前环境优先选择
Windows 原生 QEMU，缺少原生 QEMU 时回退到 WSL QEMU。内核源码调试仍可选择旁边的
`LiteOS: Kernel debug (WSL QEMU)` 配置。

无图形模式运行固定时间并查看串口日志：

```bash
./run-qemu.sh --headless --seconds 5
```

`run-qemu.sh` 默认把 `build/esp` 作为可启动的 USB Mass Storage FAT 卷，固件和
LiteOS 内核都会从 USB 盘启动，内核串口应显示 `LITEOS_ROOT_SOURCE=USB`。需要对照
旧路径时可使用 `./run-qemu.sh --nvme-root`。

脚本使用 `/usr/bin/qemu-system-x86_64` 和 `/usr/share/OVMF/OVMF_CODE_4M.fd`。串口日志保存在
`build/qemu-serial.log`，QEMU guest error
日志保存在 `build/qemu.log`。

正常启动时会依次看到 `LITEOS_KERNEL_OK`、`LITEOS_BUDDY_OK`、`LITEOS_FAT32_OK`、`LITEOS_WINDOW_OK`、`LITEOS_SYSCALL_OK`、`LITEOS_USERMODE_OK` 和 `QEMU UEFI handoff: OK`。

## loader.conf

```ini
kernel=\EFI\LITEOS\kernel.elf
cmdline=console=framebuffer loglevel=info
kernel_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
# kernel_signature=<512 个十六进制字符，RSA-2048 PKCS#1 v1.5 签名>
verify=required
```

`verify=required` 时必须同时提供所选内核原始文件的 SHA-256 和 RSA-2048 PKCS#1 v1.5 签名；公钥固定在 Loader 内，不从 ESP 配置读取。Secure Boot 仍由固件验证 `BOOTX64.EFI` 的签名；加载器不会伪造或绕过 Secure Boot。

## BootInfo

`include/bootinfo.h` 定义了 Loader 到内核的公共 ABI，包含：

- 内核物理基址、恒等映射地址、镜像大小和入口地址；
- UEFI 内存图、描述符大小/版本和最终内存图缓冲区；
- GOP framebuffer 的地址、尺寸、stride、像素格式和 bit mask；
- ACPI、SMBIOS、Runtime Services、System Table 和固件 RNG 信息；
- 命令行、Loader 名称、Secure Boot/内核校验状态；
- BootInfo、内存图、命令行、Loader 名称、framebuffer 和 2 MiB BootstrapStack 的保留信息。

退出 `ExitBootServices` 后，内核不得再调用 Boot Services。Loader 最后切换到私有 2 MiB 启动栈，并以 SysV ABI 调用 `kernel_entry(BootInfo)`。

## VS Code 一键调试

安装 Microsoft C/C++ 扩展后，在“运行和调试”中选择 `LiteOS: QEMU (F5 auto detect)` 并按 F5。配置会：

1. 自动检测 Windows 原生 QEMU 或 WSL QEMU；
2. 在 Windows 原生 QEMU 模式下使用 WSL 自动编译工程；
3. 启动带 USB 键盘和 USB 鼠标的 LiteOS 图形环境。

如需断点调试，再选择 `LiteOS: Kernel debug (WSL QEMU)`，它会使用 WSL 的 QEMU/GDB
连接并加载 `kernel.elf` 符号。

## 已知限制与后续工作

- QEMU 下的 NVMe 队列环回/复位、xHCI、HDA/e1000 reset/link 检查和 VT-d 路径已有硬件仿真验证，真实硬件仍需按控制器、固件和 ACPI 差异补齐验证矩阵；
- xHCI 已覆盖直连设备清单、独立设备 context、重复枚举、slot 禁用、活动端点 Stop Endpoint、端点 DMA 释放和重新配置，并能识别 Hub、读取 Hub 描述符、控制下游端口以及用 route string 枚举下游设备；运行期已接入根口事件、Hub 下游连接状态轮询、Hub 中断状态端点、设备树级 slot/DMA 回收和重连，仍只调度一个活动数据设备，多活动类设备调度仍需继续实现，因此 USB 键盘与 USB Audio 的回归需要分别启动；
- FAT32 已支持 LFN 读取与创建、跨簇原位写入、文件/目录创建与删除、LFN 元数据整组回收、空目录回收、文件扩展和删除事务回滚；LiteFS 已覆盖 fsync、模拟掉电 journal 回放及 superblock/inode 校验，FAT32 仍需继续补齐真实介质掉电时序验证；
- 蓝牙已完成 HCI/L2CAP/GATT/HID 协议核心，并接入 xHCI 的 USB Bluetooth HCI 控制、事件和 ACL DMA 路径；真实适配仍需按具体控制器补充固件和硬件矩阵；
- 音频已具备 DMA/PCM/HDA BDL 控制器、UAPI 音频流句柄、codec 基本路由、USB Audio 等时端点后端和独立 audiod 混音服务；复杂 pin/mixer 路由、USB Audio class control 以及多 alternate/rate 协商仍需完成；
- 电源管理已实现设备事务、超时、回滚、挂起/恢复循环和 ACPI S3/S4 请求，具体平台的唤醒固件路径仍需实机验证；
- Loader 使用的 RSA-2048 公钥是开发配置，发布版应替换为项目正式公钥并通过 Secure Boot 签名链发布；
- A/B 更新已具备版本/代数约束、启动尝试计数、安全模式和持久化失败回滚；真实发布仍需接入原子 slot 写入、正式签名包和发布密钥；
- Firmware Manager 已接入逻辑名称解析、版本/代数约束、包标识和 SHA-256 固件完整性校验；真实发布仍需把 provider 接到签名包和设备固件仓库；
- audiod mixer 支持多客户端环形缓冲、S16/S24/S32 饱和混音、欠载统计及设备断开恢复，并由独立 ELF 服务接入 AUDIO_OPEN/AUDIO_CONTROL syscall；
- 窗口层已支持焦点恢复、合成器重启、输出热插拔、VBlank 序列和丢帧计数；当前仍是 GOP 软件合成，真实 GPU 原子提交、硬件 VBlank 和物理多显示器矩阵需要继续验证；
- `/init` 已同时接通启动自测和常驻监督路径：正式运行态会创建四个独立用户进程并保持其生命周期，监督进程异常退出时由内核有限次数重启；但服务的实际设备策略、日志后端、崩溃守护、窗口服务器和桌面环境仍未完成，不应把当前启动自检视为可发布的 OS 1.0。

代码注释统一使用中文，模块公共接口集中在 `include/`，实现按 `kernel/arch`、`kernel/core`、`kernel/mm`、`kernel/drivers`、`kernel/fs` 和 `kernel/graphics` 分层组织。


重复性的工作写成脚本可以减少后期工作量
临时文件及时删除
