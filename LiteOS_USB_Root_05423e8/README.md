# LiteOS USB Root Boot Patch

Baseline:

```text
05423e88e3ccd716bf9972ceb10d7248f8b08b5d
```

This patch allows LiteOS to use a USB Mass Storage FAT volume as `/`, while
keeping the existing NVMe root path as fallback.

It modifies only:

```text
include/usb/storage.h
kernel/drivers/usb/storage.c
kernel/kernel_entry.c
```

## What it adds

- synchronous boot-stage `usb_msc_attach(slot)`;
- whole-device FAT detection;
- MBR primary-partition scanning;
- standard GPT partition scanning;
- `/init` + `/init-runtime` validation before accepting a USB root;
- USB-first, NVMe-fallback root selection.

Existing xHCI BOT/SCSI runtime/hotplug behavior is preserved.

## Apply

```bash
./apply.sh ~/LiteOS

cd ~/LiteOS
git diff -- include/usb/storage.h kernel/drivers/usb/storage.c kernel/kernel_entry.c

make clean
make -j$(nproc)
make test
```

## USB layout

Copy the CONTENTS of:

```text
build/esp/
```

to the root of a FAT volume on the USB disk.

Required files include:

```text
/EFI/BOOT/BOOTX64.EFI
/EFI/LITEOS/kernel.elf
/EFI/LITEOS/loader.conf
/init
/init-runtime
/sbin/...
/lib/...
```

Supported layouts:

```text
whole-disk FAT
MBR primary FAT partition
standard GPT FAT/ESP partition
```

Disable Secure Boot unless your EFI loader is signed/trusted.

## Expected log

```text
LITEOS_ROOT_USB_ATTACH SLOT=...
LITEOS_USB_MSC_BLOCK_OK
LITEOS_ROOT_USB_VOLUME_OK SLOT=...
LITEOS_ROOT_USB_OK
LITEOS_ROOT_SOURCE=USB
```

If USB root discovery fails but NVMe remains usable:

```text
LITEOS_ROOT_NVME_OK
LITEOS_ROOT_SOURCE=NVME
```

## Rollback

```bash
./rollback.sh ~/LiteOS
```

## Current limitations

- USB MSC BOT/SCSI transparent storage only;
- LUN 0 only (same as current LiteOS driver);
- no UAS;
- do not hot-unplug the active root USB;
- GPT support targets normal GPT entries (standard 128-byte entries work);
- UEFI BootInfo device-path hash is not yet mapped back to the xHCI route,
  so the root candidate is selected by presence of `/init` and `/init-runtime`.
