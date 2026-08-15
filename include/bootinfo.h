#ifndef LITEOS_BOOTINFO_H
#define LITEOS_BOOTINFO_H

#include "uefi.h"
#include "update_state.h"

#define LITEOS_BOOTINFO_MAGIC 0x4C4954454F534249ULL /* ASCII 字符串 "LITEOSBI" */
#define LITEOS_BOOTINFO_VERSION 5U
#define LITEOS_BOOTSTRAP_STACK_SIZE (2ULL * 1024ULL * 1024ULL)

enum {
    LITEOS_BOOTINFO_HAS_FRAMEBUFFER = 1U << 0,
    LITEOS_BOOTINFO_HAS_ACPI        = 1U << 1,
    LITEOS_BOOTINFO_HAS_SMBIOS      = 1U << 2,
    LITEOS_BOOTINFO_HAS_RNG         = 1U << 3,
    LITEOS_BOOTINFO_SECURE_BOOT     = 1U << 4,
    LITEOS_BOOTINFO_KERNEL_VERIFIED = 1U << 5,
    LITEOS_BOOTINFO_HAS_UPDATE_STATE = 1U << 6,
    LITEOS_BOOTINFO_UPDATE_PENDING  = 1U << 7,
    LITEOS_BOOTINFO_UPDATE_SAFE_MODE = 1U << 8,
    /* BootInfo 中包含实际装载镜像的 SHA-256，供内核和崩溃记录关联发布产物。 */
    LITEOS_BOOTINFO_HAS_KERNEL_HASH = 1U << 9,
    /* 内核原始镜像通过 Loader 内置 RSA-2048 信任根验证。 */
    LITEOS_BOOTINFO_KERNEL_SIGNED = 1U << 10,
    /* BootInfo 末尾包含退出 UEFI 后仍可使用的启动设备描述。 */
    LITEOS_BOOTINFO_HAS_BOOT_DEVICE = 1U << 11,
};

enum {
    LITEOS_PIXEL_RGBR = 0,
    LITEOS_PIXEL_BGRR = 1,
    LITEOS_PIXEL_BITMASK = 2,
    LITEOS_PIXEL_BLT_ONLY = 3,
};

typedef struct {
    UINT64 Magic;
    UINT32 Version;
    UINT32 Size;
    UINT64 Flags;

    UINT64 KernelPhysicalBase; /* ELF 镜像实际装载的物理起始地址 */
    UINT64 KernelVirtualBase;  /* ELF PT_LOAD 段的高半虚拟起始地址 */
    UINT64 KernelSize;         /* 所有 PT_LOAD 段覆盖的页对齐大小 */
    UINT64 KernelEntry;        /* 高半内核入口虚拟地址 */

    UINT64 MemoryMap;
    UINT64 MemoryMapSize;
    UINT64 MemoryDescriptorSize;
    UINT32 MemoryDescriptorVersion;
    UINT32 Reserved0;

    UINT64 FrameBufferBase;
    UINT64 FrameBufferSize;
    UINT32 FrameBufferWidth;
    UINT32 FrameBufferHeight;
    UINT32 FrameBufferPixelsPerScanLine;
    UINT32 FrameBufferFormat;
    UINT32 FrameBufferMask[4];
    UINT32 Reserved1;

    UINT64 AcpiRsdp;
    UINT64 Smbios;
    UINT64 Smbios3;
    UINT64 RuntimeServices;
    UINT64 SystemTable;

    UINT8  RandomSeed[32];
    UINT64 CommandLine;
    UINT64 LoaderName;
    UINT64 LoaderNameSize;

    /* 内核启动期间必须继续保留的启动分配区域。 */
    UINT64 MemoryMapBufferSize;
    UINT64 CommandLineSize;
    UINT64 BootInfoPhysicalBase;
    UINT64 BootstrapStackBase;
    UINT64 BootstrapStackSize;
    UINT64 BootstrapStackTop;
    UINT64 LoaderImageBase;
    UINT64 LoaderImageSize;
    UINT64 PageDatabasePhysicalBase;
    UINT64 PageDatabaseSize;

    /* AP 的 SIPI 入口必须位于 1 MiB 以下，并在内核完成 SMP 启动前保持保留。 */
    UINT64 ApTrampolineBase;
    UINT64 ApTrampolineSize;

    /* A/B 更新状态；内核早期成功启动后将 pending 槽提交为 active。 */
    UINT32 UpdateActiveSlot;
    UINT32 UpdateBootSlot;
    UINT32 UpdateBootAttempts;
    UINT32 Reserved2;
    UINT64 UpdateVersion;
    UINT64 UpdateGeneration;

    /* 追加字段保持旧版 BootInfo 的已有偏移不变。 */
    UINT8  KernelImageHash[32]; /* EFI 文件内容的 SHA-256，不是重定位后的内存摘要 */

    /*
     * 启动设备元数据是 Loader 在 ExitBootServices 前复制出的值。
     * BootDeviceHandle 只能用于日志关联，内核不得把它当作 UEFI 指针解引用。
     * 设备路径哈希覆盖完整 EFI_DEVICE_PATH，分区字段来自硬盘节点。
     */
    UINT64 BootDeviceHandle;
    UINT32 BootDevicePathSize;
    UINT32 BootDevicePartitionNumber;
    UINT64 BootDevicePartitionStartLba;
    UINT64 BootDevicePartitionSizeLba;
    UINT8  BootDevicePartitionSignature[16];
    UINT8  BootDevicePathHash[32];
    UINT8  BootDevicePartitionMbrType;
    UINT8  BootDevicePartitionSignatureType;
    UINT16 Reserved3;
} LITEOS_BOOT_INFO;

#endif
