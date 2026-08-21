#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

EXPECTED_HEAD = "05423e88e3ccd716bf9972ceb10d7248f8b08b5d"
MARKER = "LITEOS_USB_ROOT_PATCH_V1"

FILES = [
    Path("include/usb/storage.h"),
    Path("kernel/drivers/usb/storage.c"),
    Path("kernel/kernel_entry.c"),
]

def die(message):
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)

def read(path):
    return path.read_text(encoding="utf-8")

def write(path, data):
    path.write_text(data, encoding="utf-8", newline="\n")

def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        die(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)

def git_head(repo):
    try:
        return subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", "HEAD"],
            text=True
        ).strip()
    except Exception as exc:
        die(f"cannot read git HEAD: {exc}")

def backup(repo):
    backup_root = repo / f".usbroot-backup-{EXPECTED_HEAD[:8]}"
    if backup_root.exists():
        return backup_root
    for rel in FILES:
        src = repo / rel
        if not src.exists():
            die(f"missing {rel}")
        dst = backup_root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    return backup_root

def rollback(repo):
    backup_root = repo / f".usbroot-backup-{EXPECTED_HEAD[:8]}"
    if not backup_root.exists():
        die(f"backup not found: {backup_root}")
    for rel in FILES:
        src = backup_root / rel
        dst = repo / rel
        if not src.exists():
            die(f"backup missing {rel}")
        shutil.copy2(src, dst)
    print("USB root patch rolled back.")

def patch_storage_header(text):
    if MARKER in text:
        return text
    old = 'bool usb_msc_schedule_attach(uint8_t slot);\n'
    new = (
        f'/* {MARKER}: boot-stage synchronous MSC attach. */\n'
        'bool usb_msc_attach(uint8_t slot);\n'
        'bool usb_msc_schedule_attach(uint8_t slot);\n'
    )
    return replace_once(text, old, new, "storage.h")

def patch_storage_c(text):
    if MARKER in text:
        return text
    anchor = 'bool usb_msc_schedule_attach(uint8_t slot) {\n'
    insert = f'''/*
 * {MARKER}
 *
 * Boot can need the same USB Mass Storage device that UEFI used to load
 * BOOTX64.EFI before the persistent deferred worker has started.
 *
 * The normal hotplug path remains deferred.  This helper reuses the exact
 * same SCSI/BOT attach routine synchronously during root discovery.
 */
bool usb_msc_attach(uint8_t slot) {{
    if (slot == 0U) return false;
    usb_msc_attach_work((void *)(uintptr_t)slot);
    return usb_msc_present(slot);
}}

bool usb_msc_schedule_attach(uint8_t slot) {{
'''
    return replace_once(text, anchor, insert, "storage.c")

def patch_kernel_entry(text):
    if MARKER in text:
        return text

    text = replace_once(
        text,
        '#include "usb.h"\n',
        '#include "usb.h"\n#include <usb/storage.h>\n',
        "kernel_entry include"
    )

    globals_anchor = '''static LITEOS_BLOCK_MANAGER g_nvme_root_block_manager;
static LITEOS_BLOCK_DEVICE *g_nvme_root_block_device;
static LITEOS_FAT32 g_nvme_root_filesystem;
static nvme_fat32_backend_t g_nvme_root_backend;
'''
    globals_new = globals_anchor + f'''
/*
 * {MARKER}
 *
 * USB root is a lightweight partition view over the MSC whole-disk block
 * device. FAT code sees sector zero of the selected volume.
 */
typedef struct {{
    LITEOS_BLOCK_DEVICE *parent;
    UINT64 start_lba;
    UINT64 block_count;
}} usb_root_backend_t;

static LITEOS_BLOCK_DEVICE g_usb_root_block_device;
static LITEOS_FAT32 g_usb_root_filesystem;
static usb_root_backend_t g_usb_root_backend;

/* USB-root probing runs before the serial helper definitions below. */
static void serial_write(const CHAR8 *text);
static void serial_write_u32(UINT32 value);
'''
    text = replace_once(text, globals_anchor, globals_new, "kernel_entry globals")

    load_anchor = '''static UINT32 load_u32(const UINT8 *source) {
    return (UINT32)source[0] | ((UINT32)source[1] << 8) |
           ((UINT32)source[2] << 16) | ((UINT32)source[3] << 24);
}
'''

    helpers = load_anchor + r'''
static UINT64 usb_root_load_u64(const UINT8 *source) {
    return (UINT64)source[0] |
           ((UINT64)source[1] << 8) |
           ((UINT64)source[2] << 16) |
           ((UINT64)source[3] << 24) |
           ((UINT64)source[4] << 32) |
           ((UINT64)source[5] << 40) |
           ((UINT64)source[6] << 48) |
           ((UINT64)source[7] << 56);
}

static VOID usb_root_zero(VOID *memory, UINTN size) {
    UINT8 *bytes = (UINT8 *)memory;
    if (bytes == 0) return;
    for (UINTN index = 0U; index < size; ++index) bytes[index] = 0U;
}

static BOOLEAN usb_root_read(VOID *context, UINT64 lba,
                             UINT32 count, VOID *buffer) {
    usb_root_backend_t *backend = (usb_root_backend_t *)context;
    if (backend == 0 || backend->parent == 0 || buffer == 0 ||
        count == 0U || lba > backend->block_count ||
        count > backend->block_count - lba ||
        backend->start_lba > UINT64_MAX - lba) {
        return 0;
    }
    return liteos_block_read(backend->parent,
                             backend->start_lba + lba,
                             count, buffer);
}

static BOOLEAN usb_root_write(VOID *context, UINT64 lba,
                              UINT32 count, const VOID *buffer) {
    usb_root_backend_t *backend = (usb_root_backend_t *)context;
    if (backend == 0 || backend->parent == 0 || buffer == 0 ||
        count == 0U || lba > backend->block_count ||
        count > backend->block_count - lba ||
        backend->start_lba > UINT64_MAX - lba) {
        return 0;
    }
    return liteos_block_write(backend->parent,
                              backend->start_lba + lba,
                              count, buffer);
}

static BOOLEAN usb_root_flush(VOID *context) {
    usb_root_backend_t *backend = (usb_root_backend_t *)context;
    return backend != 0 && backend->parent != 0 &&
           liteos_block_flush(backend->parent);
}

static VOID usb_root_prepare_slice(LITEOS_BLOCK_DEVICE *parent,
                                   UINT64 start_lba,
                                   UINT64 block_count) {
    usb_root_zero(&g_usb_root_backend, sizeof(g_usb_root_backend));
    usb_root_zero(&g_usb_root_block_device, sizeof(g_usb_root_block_device));

    g_usb_root_backend.parent = parent;
    g_usb_root_backend.start_lba = start_lba;
    g_usb_root_backend.block_count = block_count;

    g_usb_root_block_device.Name[0] = 'u';
    g_usb_root_block_device.Name[1] = 's';
    g_usb_root_block_device.Name[2] = 'b';
    g_usb_root_block_device.Name[3] = 'r';
    g_usb_root_block_device.Name[4] = 'o';
    g_usb_root_block_device.Name[5] = 'o';
    g_usb_root_block_device.Name[6] = 't';
    g_usb_root_block_device.Name[7] = 0;
    g_usb_root_block_device.BlockSize = parent->BlockSize;
    g_usb_root_block_device.BlockCount = block_count;
    g_usb_root_block_device.Read = usb_root_read;
    g_usb_root_block_device.Write = usb_root_write;
    g_usb_root_block_device.Flush = usb_root_flush;
    g_usb_root_block_device.Context = &g_usb_root_backend;
    g_usb_root_block_device.Registered = 1;
}

static BOOLEAN usb_root_has_required_files(LITEOS_FAT32 *filesystem) {
    os_file_info_t info = {0};

    if (filesystem == 0 ||
        !liteos_fat32_stat_path(filesystem, "/init", &info) ||
        info.type != OS_FILE_TYPE_REGULAR) {
        return 0;
    }

    info = (os_file_info_t){0};
    return liteos_fat32_stat_path(filesystem, "/init-runtime", &info) &&
           info.type == OS_FILE_TYPE_REGULAR;
}

static BOOLEAN usb_root_try_slice(LITEOS_BLOCK_DEVICE *parent,
                                  UINT64 start_lba,
                                  UINT64 block_count) {
    if (parent == 0 || !parent->Registered ||
        parent->BlockSize < 512U || parent->BlockSize > 4096U ||
        start_lba >= parent->BlockCount ||
        block_count == 0U ||
        block_count > parent->BlockCount - start_lba) {
        return 0;
    }

    usb_root_prepare_slice(parent, start_lba, block_count);
    usb_root_zero(&g_usb_root_filesystem, sizeof(g_usb_root_filesystem));

    if (!liteos_fat32_init(&g_usb_root_filesystem,
                            &g_usb_root_block_device)) {
        return 0;
    }

    if (!usb_root_has_required_files(&g_usb_root_filesystem)) {
        (void)liteos_fat32_destroy(&g_usb_root_filesystem);
        usb_root_zero(&g_usb_root_filesystem,
                      sizeof(g_usb_root_filesystem));
        return 0;
    }

    if (vfs_mount_fat32("/", &g_usb_root_filesystem) != K_OK) {
        (void)liteos_fat32_destroy(&g_usb_root_filesystem);
        usb_root_zero(&g_usb_root_filesystem,
                      sizeof(g_usb_root_filesystem));
        return 0;
    }

    return 1;
}

static BOOLEAN usb_root_try_gpt(LITEOS_BLOCK_DEVICE *device) {
    UINT8 sector[4096];
    UINT64 entries_lba;
    UINT32 entry_count;
    UINT32 entry_size;
    UINT32 entries_per_block;
    UINT64 loaded_lba = UINT64_MAX;

    if (device == 0 || device->BlockSize > sizeof(sector) ||
        device->BlockCount <= 2U ||
        !liteos_block_read(device, 1U, 1U, sector)) {
        return 0;
    }

    static const UINT8 signature[8] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    for (UINT32 index = 0U; index < sizeof(signature); ++index) {
        if (sector[index] != signature[index]) return 0;
    }

    entries_lba = usb_root_load_u64(sector + 72U);
    entry_count = load_u32(sector + 80U);
    entry_size = load_u32(sector + 84U);

    if (entries_lba == 0U || entries_lba >= device->BlockCount ||
        entry_count == 0U || entry_size < 128U ||
        entry_size > device->BlockSize ||
        device->BlockSize % entry_size != 0U) {
        return 0;
    }

    entries_per_block = device->BlockSize / entry_size;
    if (entry_count > 128U) entry_count = 128U;

    for (UINT32 index = 0U; index < entry_count; ++index) {
        UINT64 entry_block =
            entries_lba + (UINT64)(index / entries_per_block);
        UINT32 entry_offset =
            (index % entries_per_block) * entry_size;
        UINT8 *entry;
        BOOLEAN type_nonzero = 0;

        if (entry_block >= device->BlockCount) break;
        if (loaded_lba != entry_block) {
            if (!liteos_block_read(device, entry_block, 1U, sector)) {
                return 0;
            }
            loaded_lba = entry_block;
        }

        entry = sector + entry_offset;
        for (UINT32 byte = 0U; byte < 16U; ++byte) {
            if (entry[byte] != 0U) {
                type_nonzero = 1;
                break;
            }
        }
        if (!type_nonzero) continue;

        UINT64 first_lba = usb_root_load_u64(entry + 32U);
        UINT64 last_lba = usb_root_load_u64(entry + 40U);
        if (first_lba == 0U || last_lba < first_lba ||
            first_lba >= device->BlockCount ||
            last_lba >= device->BlockCount) {
            continue;
        }

        if (usb_root_try_slice(device, first_lba,
                               last_lba - first_lba + 1U)) {
            return 1;
        }
    }

    return 0;
}

static BOOLEAN usb_root_try_device(LITEOS_BLOCK_DEVICE *device) {
    UINT8 sector[4096];
    BOOLEAN protective_gpt = 0;

    if (device == 0 || !device->Registered ||
        device->BlockSize < 512U ||
        device->BlockSize > sizeof(sector) ||
        device->BlockCount == 0U) {
        return 0;
    }

    if (usb_root_try_slice(device, 0U, device->BlockCount)) {
        return 1;
    }

    if (!liteos_block_read(device, 0U, 1U, sector) ||
        sector[510] != 0x55U || sector[511] != 0xAAU) {
        return 0;
    }

    for (UINT32 partition = 0U; partition < 4U; ++partition) {
        UINT32 offset = 446U + partition * 16U;
        if (sector[offset + 4U] == 0xEEU) {
            protective_gpt = 1;
            break;
        }
    }

    if (protective_gpt && usb_root_try_gpt(device)) {
        return 1;
    }

    for (UINT32 partition = 0U; partition < 4U; ++partition) {
        UINT32 offset = 446U + partition * 16U;
        UINT8 type = sector[offset + 4U];
        UINT64 start_lba = load_u32(sector + offset + 8U);
        UINT64 block_count = load_u32(sector + offset + 12U);

        if (type == 0U || type == 0xEEU ||
            start_lba == 0U || block_count == 0U) {
            continue;
        }

        if (usb_root_try_slice(device, start_lba, block_count)) {
            return 1;
        }
    }

    return 0;
}

static BOOLEAN mount_usb_root_filesystem(void) {
    if (!xhci_hardware_present() ||
        !xhci_usb_mass_storage_configured()) {
        return 0;
    }

    for (UINT32 slot = 1U; slot < 256U; ++slot) {
        uint8_t interface_number = 0U;
        uint8_t bulk_in = 0U;
        uint8_t bulk_out = 0U;

        if (!xhci_usb_msc_query((uint8_t)slot, &interface_number,
                                &bulk_in, &bulk_out)) {
            continue;
        }

        serial_write("LITEOS_ROOT_USB_ATTACH SLOT=");
        serial_write_u32(slot);
        serial_write("\r\n");

        if (!usb_msc_attach((uint8_t)slot)) {
            serial_write("LITEOS_ROOT_USB_ATTACH_FAIL SLOT=");
            serial_write_u32(slot);
            serial_write("\r\n");
            continue;
        }

        LITEOS_BLOCK_DEVICE *device =
            usb_msc_block_device((uint8_t)slot);
        if (device == 0) continue;

        if (usb_root_try_device(device)) {
            serial_write("LITEOS_ROOT_USB_VOLUME_OK SLOT=");
            serial_write_u32(slot);
            serial_write("\r\n");
            return 1;
        }
    }

    return 0;
}
'''
    text = replace_once(text, load_anchor, helpers, "kernel_entry helpers")

    root_anchor = '''    if (!mount_nvme_root_filesystem(active_nvme == 0 ? 0 : active_nvme->device)) {
        serial_write("LITEOS_ROOT_NVME_MOUNT_FAIL\\r\\n");
        halt_forever();
    }
    serial_write("LITEOS_ROOT_NVME_OK\\r\\n");
'''
    root_new = f'''    /*
     * {MARKER}
     *
     * Prefer a removable root volume containing the complete LiteOS payload.
     * If no suitable USB volume exists, retain the original NVMe behavior.
     */
    BOOLEAN root_mounted = 0;

    if (mount_usb_root_filesystem()) {{
        root_mounted = 1;
        serial_write("LITEOS_ROOT_USB_OK\\r\\n");
        serial_write("LITEOS_ROOT_SOURCE=USB\\r\\n");
    }}

    if (!root_mounted && active_nvme != 0 &&
        mount_nvme_root_filesystem(active_nvme->device)) {{
        root_mounted = 1;
        serial_write("LITEOS_ROOT_NVME_OK\\r\\n");
        serial_write("LITEOS_ROOT_SOURCE=NVME\\r\\n");
    }}

    if (!root_mounted) {{
        serial_write("LITEOS_ROOT_MOUNT_FAIL USB_OR_NVME\\r\\n");
        halt_forever();
    }}
'''
    text = replace_once(text, root_anchor, root_new, "kernel_entry root selection")
    return text

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("repo", nargs="?", default=".")
    parser.add_argument("--force-head", action="store_true")
    parser.add_argument("--rollback", action="store_true")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    if not (repo / ".git").exists():
        die(f"not a git repository: {repo}")

    if args.rollback:
        rollback(repo)
        return

    head = git_head(repo)
    if head != EXPECTED_HEAD and not args.force_head:
        die(
            f"HEAD is {head}, expected {EXPECTED_HEAD}. "
            "Regenerate the patch for the new HEAD or review before --force-head."
        )

    backup_root = backup(repo)

    storage_h = repo / "include/usb/storage.h"
    storage_c = repo / "kernel/drivers/usb/storage.c"
    kernel_entry = repo / "kernel/kernel_entry.c"

    write(storage_h, patch_storage_header(read(storage_h)))
    write(storage_c, patch_storage_c(read(storage_c)))
    write(kernel_entry, patch_kernel_entry(read(kernel_entry)))

    print(f"Applied {MARKER}")
    print(f"Backup: {backup_root}")
    print("Changed:")
    for rel in FILES:
        print(f"  {rel}")
    print("")
    print("Next:")
    print("  make clean")
    print("  make -j$(nproc)")
    print("  make test")
    print("")
    print("Expected USB-root diagnostics:")
    print("  LITEOS_ROOT_USB_ATTACH SLOT=n")
    print("  LITEOS_USB_MSC_BLOCK_OK")
    print("  LITEOS_ROOT_USB_VOLUME_OK SLOT=n")
    print("  LITEOS_ROOT_USB_OK")
    print("  LITEOS_ROOT_SOURCE=USB")

if __name__ == "__main__":
    main()
