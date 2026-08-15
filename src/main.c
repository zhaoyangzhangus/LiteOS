#include "uefi.h"
#include "bootinfo.h"
#include "elf.h"
#include "sha256.h"
#include "rsa.h"
#include "memory_map.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define CONFIG_PATH_CAP 256U
#define CMDLINE_CAP 1024U
#define MAP_EXTRA_DESCRIPTORS 8U
#define MAX_MAP_RETRIES 8U
#define BOOTSTRAP_STACK_SIZE LITEOS_BOOTSTRAP_STACK_SIZE
#define BOOTSTRAP_STACK_PAGES (BOOTSTRAP_STACK_SIZE / 4096ULL)
#define BOOTSTRAP_STACK_ALLOCATION_PAGES (BOOTSTRAP_STACK_PAGES * 2ULL)
#define AP_TRAMPOLINE_PAGES 1ULL
#define AP_TRAMPOLINE_MAX_ADDRESS 0x000FFFFFULL

const EFI_GUID gEfiLoadedImageProtocolGuid =
    {0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
const EFI_GUID gEfiSimpleFileSystemProtocolGuid =
    {0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
const EFI_GUID gEfiDevicePathProtocolGuid =
    {0x09576E91,0x6D3F,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
const EFI_GUID gEfiFileInfoGuid =
    {0x09576E92,0x6D3F,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
const EFI_GUID gEfiGraphicsOutputProtocolGuid =
    {0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};
const EFI_GUID gEfiRngProtocolGuid =
    {0x3152BCA5,0xEADE,0x433D,{0x86,0x2E,0xC0,0x1C,0xD1,0xF4,0x4C,0xD1}};
const EFI_GUID gEfiAcpi20TableGuid =
    {0x8868E871,0xE4F1,0x11D3,{0xBC,0x22,0x00,0x80,0xC7,0x3C,0x88,0x81}};
const EFI_GUID gEfiAcpi10TableGuid =
    {0xEB9D2D30,0x2D88,0x11D3,{0x9A,0x16,0x00,0x90,0x27,0x3F,0xC1,0x4D}};
const EFI_GUID gEfiSmbios3TableGuid =
    {0xF2FD1544,0x9794,0x4A2C,{0x99,0x2E,0xE5,0xBB,0xCF,0x20,0xE3,0x94}};
const EFI_GUID gEfiSmbiosTableGuid =
    {0xEB9D2D31,0x2D88,0x11D3,{0x9A,0x16,0x00,0x90,0x27,0x3F,0xC1,0x4D}};
static const EFI_GUID gLiteosUpdateVendorGuid = LITEOS_UPDATE_VENDOR_GUID_INITIALIZER;
static CHAR16 gLiteosUpdateStateName[] = {
    'L','i','t','e','O','S','U','p','d','a','t','e','S','t','a','t','e',0
};

static EFI_SYSTEM_TABLE *g_system_table;
static const CHAR8 g_loader_name[] = "LiteOS UEFI Loader 1.0";
static const CHAR8 g_default_cmdline[] = "";

/* 交接页表只用于 Loader 到内核早期阶段，最终页表由内核重建。 */
#define HANDOFF_PAGE_TABLE_COUNT 4U
#define HANDOFF_KERNEL_PT_COUNT  16U
static UINT64 g_handoff_pml4[512] __attribute__((aligned(4096)));
static UINT64 g_handoff_pdpt[HANDOFF_PAGE_TABLE_COUNT] __attribute__((aligned(4096)));
static UINT64 g_handoff_pd[HANDOFF_PAGE_TABLE_COUNT][512] __attribute__((aligned(4096)));
static UINT64 g_handoff_kernel_pdpt[512] __attribute__((aligned(4096)));
static UINT64 g_handoff_kernel_pd[512] __attribute__((aligned(4096)));
static UINT64 g_handoff_kernel_pt[HANDOFF_KERNEL_PT_COUNT][512] __attribute__((aligned(4096)));

static VOID mem_zero(VOID *memory, UINTN size);

static UINT64 page_table_address(const VOID *table) {
    return (UINT64)(uintptr_t)table;
}

static BOOLEAN build_handoff_page_tables(const LITEOS_ELF_IMAGE *image, UINT64 *root) {
    if (image == 0 || root == 0 || image->PhysicalBase == 0 || image->Size == 0 ||
        (image->VirtualBase & 0xFFFULL) != 0 || (image->Size & 0xFFFULL) != 0) return 0;
    mem_zero(g_handoff_pml4, sizeof(g_handoff_pml4));
    mem_zero(g_handoff_pdpt, sizeof(g_handoff_pdpt));
    mem_zero(g_handoff_pd, sizeof(g_handoff_pd));
    mem_zero(g_handoff_kernel_pdpt, sizeof(g_handoff_kernel_pdpt));
    mem_zero(g_handoff_kernel_pd, sizeof(g_handoff_kernel_pd));
    mem_zero(g_handoff_kernel_pt, sizeof(g_handoff_kernel_pt));

    /* 低端恒等映射与 DIRECT_MAP_BASE 的前 4 GiB 共享页目录。 */
    for (UINTN table = 0; table < HANDOFF_PAGE_TABLE_COUNT; ++table) {
        g_handoff_pdpt[table] = page_table_address(g_handoff_pd[table]) | 0x03ULL;
        for (UINTN entry = 0; entry < 512U; ++entry) {
            UINT64 physical = ((UINT64)table * 512ULL + entry) * 0x200000ULL;
            g_handoff_pd[table][entry] = physical | 0x83ULL;
        }
    }
    g_handoff_pml4[0] = page_table_address(g_handoff_pdpt) | 0x03ULL;
    g_handoff_pml4[256] = page_table_address(g_handoff_pdpt) | 0x03ULL;

    /* 高半内核使用 4 KiB 页，避免物理装载地址需要 2 MiB 对齐。 */
    g_handoff_pml4[511] = page_table_address(g_handoff_kernel_pdpt) | 0x03ULL;
    UINT64 first = image->VirtualBase;
    UINT64 last = first + image->Size;
    if (last < first) return 0;
    for (UINT64 virtual_address = first; virtual_address < last; virtual_address += 0x1000ULL) {
        UINTN pml4_index = (UINTN)((virtual_address >> 39) & 0x1FFULL);
        UINTN pdpt_index = (UINTN)((virtual_address >> 30) & 0x1FFULL);
        UINTN pd_index = (UINTN)((virtual_address >> 21) & 0x1FFULL);
        UINTN pt_index = (UINTN)((virtual_address >> 12) & 0x1FFULL);
        if (pml4_index != 511U || pd_index >= HANDOFF_KERNEL_PT_COUNT) return 0;
        if (g_handoff_kernel_pdpt[pdpt_index] == 0) {
            g_handoff_kernel_pdpt[pdpt_index] = page_table_address(g_handoff_kernel_pd) | 0x03ULL;
        }
        if (g_handoff_kernel_pd[pd_index] == 0) {
            g_handoff_kernel_pd[pd_index] = page_table_address(g_handoff_kernel_pt[pd_index]) | 0x03ULL;
        }
        UINT64 physical = image->PhysicalBase + (virtual_address - first);
        if (physical < image->PhysicalBase) return 0;
        g_handoff_kernel_pt[pd_index][pt_index] = physical | 0x03ULL;
    }
    *root = page_table_address(g_handoff_pml4);
    return 1;
}

/*
 * 该函数由 UEFI 使用的 Microsoft x64 ABI 调用，并在此完成 Loader 到
 * SysV 内核的全部切换。新栈已经按 2 MiB 对齐，CALL 前再按 SysV ABI
 * 调整为 16 字节对齐。内核从 RDI 接收 BootInfo，不预留 shadow space。
 */
static VOID EFIAPI __attribute__((noreturn, naked)) loader_enter_kernel(
    UINT64, LITEOS_BOOT_INFO *, UINT64, UINT64) {
    __asm__(
        "movq %rcx, %rax\n"
        "movq %rdx, %rdi\n"
        "movq %r8, %rsp\n"
        "movq %r9, %cr3\n"
        "andq $-16, %rsp\n"
        "call *%rax\n"
        "cli\n"
        "hlt\n"
        "jmp .-2\n"
    );
}

typedef struct {
    CHAR16 kernel_path[CONFIG_PATH_CAP];
    CHAR16 kernel_a_path[CONFIG_PATH_CAP];
    CHAR16 kernel_b_path[CONFIG_PATH_CAP];
    BOOLEAN has_kernel_a;
    BOOLEAN has_kernel_b;
    CHAR8 cmdline[CMDLINE_CAP];
    BOOLEAN has_hash;
    BOOLEAN verify_required;
    UINT8 kernel_hash[32];
    BOOLEAN has_signature;
    UINT8 kernel_signature[LITEOS_RSA2048_BYTES];
    BOOLEAN has_slot_hash[2];
    UINT8 slot_hash[2][32];
    BOOLEAN has_slot_signature[2];
    UINT8 slot_signature[2][LITEOS_RSA2048_BYTES];
} LOADER_CONFIG;

typedef struct {
    BOOLEAN enabled;
    BOOLEAN pending;
    BOOLEAN safe_mode;
    BOOLEAN has_hash;
    UINT32 active_slot;
    UINT32 boot_slot;
    UINT32 boot_attempts;
    UINT64 version;
    UINT64 generation;
    const CHAR16 *path;
    UINT8 hash[32];
    BOOLEAN has_signature;
    UINT8 signature[LITEOS_RSA2048_BYTES];
} LOADER_UPDATE_SELECTION;

static VOID mem_zero(VOID *memory, UINTN size) {
    UINT8 *p = (UINT8 *)memory;
    while (size-- != 0) *p++ = 0;
}

static VOID mem_copy(VOID *destination, const VOID *source, UINTN size) {
    UINT8 *d = (UINT8 *)destination;
    const UINT8 *s = (const UINT8 *)source;
    while (size-- != 0) *d++ = *s++;
}

static UINTN ascii_length(const CHAR8 *text) {
    UINTN length = 0;
    while (text[length] != 0) ++length;
    return length;
}

static BOOLEAN guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *x = (const UINT8 *)a;
    const UINT8 *y = (const UINT8 *)b;
    UINT8 difference = 0;
    for (UINTN i = 0; i < sizeof(EFI_GUID); ++i) difference |= (UINT8)(x[i] ^ y[i]);
    return difference == 0;
}

static VOID console_ascii(const CHAR8 *text) {
    if (g_system_table == 0 || g_system_table->ConOut == 0 || g_system_table->ConOut->OutputString == 0) return;
    CHAR16 buffer[128];
    while (*text != 0) {
        UINTN i = 0;
        while (*text != 0 && i + 1 < ARRAY_SIZE(buffer)) buffer[i++] = (CHAR16)(UINT8)*text++;
        buffer[i] = 0;
        g_system_table->ConOut->OutputString(g_system_table->ConOut, buffer);
    }
}

static VOID console_hex(UINT64 value) {
    static const CHAR8 digits[] = "0123456789ABCDEF";
    CHAR8 buffer[19];
    buffer[0] = '0'; buffer[1] = 'x'; buffer[18] = 0;
    for (UINTN i = 0; i < 16; ++i) buffer[2+i] = digits[(value >> (60 - i*4)) & 0xF];
    console_ascii(buffer);
}

static VOID report_error(const CHAR8 *what, EFI_STATUS status) {
    console_ascii("LiteOS loader: ");
    console_ascii(what);
    console_ascii(" (status ");
    console_hex(status);
    console_ascii(")\r\n");
}

static VOID ascii_to_char16(CHAR16 *destination, UINTN capacity, const CHAR8 *source) {
    UINTN i = 0;
    if (capacity == 0) return;
    while (source[i] != 0 && i + 1 < capacity) {
        destination[i] = (CHAR16)(UINT8)source[i];
        ++i;
    }
    destination[i] = 0;
}

static VOID copy_ascii(CHAR8 *destination, UINTN capacity, const CHAR8 *source, UINTN length) {
    if (capacity == 0) return;
    if (length >= capacity) length = capacity - 1;
    for (UINTN i = 0; i < length; ++i) destination[i] = source[i];
    destination[length] = 0;
}

static BOOLEAN ascii_equals(const CHAR8 *a, UINTN length, const CHAR8 *b) {
    UINTN b_length = ascii_length(b);
    if (length != b_length) return 0;
    for (UINTN i = 0; i < length; ++i) {
        CHAR8 left = a[i], right = b[i];
        if (left >= 'A' && left <= 'Z') left = (CHAR8)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (CHAR8)(right - 'A' + 'a');
        if (left != right) return 0;
    }
    return 1;
}

static VOID trim_ascii(const CHAR8 **start, UINTN *length) {
    while (*length != 0 && ((*start)[0] == ' ' || (*start)[0] == '\t')) { ++*start; --*length; }
    while (*length != 0 && ((*start)[*length-1] == ' ' || (*start)[*length-1] == '\t' ||
                            (*start)[*length-1] == '\r' || (*start)[*length-1] == '\n')) --*length;
}

static EFI_STATUS read_file(EFI_BOOT_SERVICES *bs, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,
                            CHAR16 *path, VOID **contents, UINTN *contents_size) {
    EFI_FILE_PROTOCOL *root = 0;
    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) return status;
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(status)) return status;

    UINTN info_size = 0;
    status = file->GetInfo(file, (EFI_GUID *)&gEfiFileInfoGuid, &info_size, 0);
    if (status != EFI_BUFFER_TOO_SMALL || info_size < sizeof(EFI_FILE_INFO)) {
        file->Close(file);
        return EFI_LOAD_ERROR;
    }
    EFI_FILE_INFO *info = 0;
    status = bs->AllocatePool(EFI_LOADER_DATA, info_size, (VOID **)&info);
    if (EFI_ERROR(status)) { file->Close(file); return status; }
    status = file->GetInfo(file, (EFI_GUID *)&gEfiFileInfoGuid, &info_size, info);
    if (EFI_ERROR(status) || info->FileSize > (UINT64)(UINTN)-1) {
        bs->FreePool(info); file->Close(file); return EFI_LOAD_ERROR;
    }
    UINTN size = (UINTN)info->FileSize;
    bs->FreePool(info);
    VOID *buffer = 0;
    if (size != 0) {
        status = bs->AllocatePool(EFI_LOADER_DATA, size, &buffer);
        if (EFI_ERROR(status)) { file->Close(file); return status; }
    }
    UINTN remaining = size;
    UINT8 *cursor = (UINT8 *)buffer;
    while (remaining != 0) {
        UINTN chunk = remaining;
        status = file->Read(file, &chunk, cursor);
        if (EFI_ERROR(status) || chunk == 0) {
            if (buffer != 0) bs->FreePool(buffer);
            file->Close(file);
            return EFI_DEVICE_ERROR;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    status = file->Close(file);
    if (EFI_ERROR(status)) {
        if (buffer != 0) bs->FreePool(buffer);
        return status;
    }
    *contents = buffer;
    *contents_size = size;
    return EFI_SUCCESS;
}

static EFI_STATUS parse_config(VOID *contents, UINTN size, LOADER_CONFIG *config) {
    UINT8 *bytes = (UINT8 *)contents;
    UINTN line_start = 0;
    for (UINTN i = 0; i <= size; ++i) {
        if (i != size && bytes[i] != '\n') continue;
        UINTN line_length = i - line_start;
        const CHAR8 *line = (const CHAR8 *)(bytes + line_start);
        trim_ascii(&line, &line_length);
        if (line_length != 0 && line[0] != '#') {
            UINTN equals = 0;
            while (equals < line_length && line[equals] != '=') ++equals;
            if (equals == line_length) return EFI_LOAD_ERROR;
            const CHAR8 *key = line;
            UINTN key_length = equals;
            const CHAR8 *value = line + equals + 1;
            UINTN value_length = line_length - equals - 1;
            trim_ascii(&key, &key_length);
            trim_ascii(&value, &value_length);
            if (ascii_equals(key, key_length, "kernel")) {
                if (value_length == 0 || value_length >= CONFIG_PATH_CAP) return EFI_LOAD_ERROR;
                for (UINTN n = 0; n < value_length; ++n) config->kernel_path[n] = (CHAR16)(UINT8)value[n];
                config->kernel_path[value_length] = 0;
            } else if (ascii_equals(key, key_length, "kernel_a")) {
                if (value_length == 0 || value_length >= CONFIG_PATH_CAP) return EFI_LOAD_ERROR;
                for (UINTN n = 0; n < value_length; ++n) config->kernel_a_path[n] = (CHAR16)(UINT8)value[n];
                config->kernel_a_path[value_length] = 0;
                config->has_kernel_a = 1;
            } else if (ascii_equals(key, key_length, "kernel_b")) {
                if (value_length == 0 || value_length >= CONFIG_PATH_CAP) return EFI_LOAD_ERROR;
                for (UINTN n = 0; n < value_length; ++n) config->kernel_b_path[n] = (CHAR16)(UINT8)value[n];
                config->kernel_b_path[value_length] = 0;
                config->has_kernel_b = 1;
            } else if (ascii_equals(key, key_length, "cmdline")) {
                copy_ascii(config->cmdline, sizeof(config->cmdline), value, value_length);
            } else if (ascii_equals(key, key_length, "kernel_sha256")) {
                if (value_length != 64) return EFI_SECURITY_VIOLATION;
                CHAR8 hash_text[65];
                copy_ascii(hash_text, sizeof(hash_text), value, value_length);
                if (!sha256_parse_hex(hash_text, config->kernel_hash)) return EFI_SECURITY_VIOLATION;
                config->has_hash = 1;
            } else if (ascii_equals(key, key_length, "kernel_signature")) {
                if (!rsa2048_parse_hex(value, value_length, config->kernel_signature)) {
                    return EFI_SECURITY_VIOLATION;
                }
                config->has_signature = 1;
            } else if (ascii_equals(key, key_length, "kernel_a_sha256") ||
                       ascii_equals(key, key_length, "kernel_b_sha256")) {
                if (value_length != 64) return EFI_SECURITY_VIOLATION;
                CHAR8 hash_text[65];
                UINTN slot = ascii_equals(key, key_length, "kernel_a_sha256") ? 0U : 1U;
                copy_ascii(hash_text, sizeof(hash_text), value, value_length);
                if (!sha256_parse_hex(hash_text, config->slot_hash[slot])) return EFI_SECURITY_VIOLATION;
                config->has_slot_hash[slot] = 1;
            } else if (ascii_equals(key, key_length, "kernel_a_signature") ||
                       ascii_equals(key, key_length, "kernel_b_signature")) {
                UINTN slot = ascii_equals(key, key_length, "kernel_a_signature") ? 0U : 1U;
                if (!rsa2048_parse_hex(value, value_length, config->slot_signature[slot])) {
                    return EFI_SECURITY_VIOLATION;
                }
                config->has_slot_signature[slot] = 1;
            } else if (ascii_equals(key, key_length, "verify")) {
                if (value_length == 8 && ascii_equals(value, value_length, "required")) config->verify_required = 1;
                else if (!(value_length == 4 && ascii_equals(value, value_length, "none"))) return EFI_LOAD_ERROR;
            }
        }
        line_start = i + 1;
    }
    if (config->verify_required &&
        ((!config->has_hash && !config->has_slot_hash[0] && !config->has_slot_hash[1]) ||
         (!config->has_signature && !config->has_slot_signature[0] &&
          !config->has_slot_signature[1]))) return EFI_SECURITY_VIOLATION;
    return EFI_SUCCESS;
}

static EFI_STATUS get_filesystem(EFI_BOOT_SERVICES *bs, EFI_HANDLE image_handle,
                                 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL **fs) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    EFI_STATUS status = bs->HandleProtocol(image_handle, (EFI_GUID *)&gEfiLoadedImageProtocolGuid, (VOID **)&loaded);
    if (EFI_ERROR(status) || loaded == 0 || loaded->DeviceHandle == 0) return EFI_NOT_FOUND;
    status = bs->HandleProtocol(loaded->DeviceHandle, (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, (VOID **)fs);
    return status;
}

static VOID discover_tables(EFI_SYSTEM_TABLE *st, LITEOS_BOOT_INFO *info) {
    for (UINTN i = 0; i < st->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *entry = &st->ConfigurationTable[i];
        if (guid_equal(&entry->VendorGuid, &gEfiAcpi20TableGuid) ||
            (info->AcpiRsdp == 0 && guid_equal(&entry->VendorGuid, &gEfiAcpi10TableGuid))) {
            info->AcpiRsdp = (UINT64)(uintptr_t)entry->VendorTable;
        }
        if (guid_equal(&entry->VendorGuid, &gEfiSmbios3TableGuid)) info->Smbios3 = (UINT64)(uintptr_t)entry->VendorTable;
        if (guid_equal(&entry->VendorGuid, &gEfiSmbiosTableGuid)) info->Smbios = (UINT64)(uintptr_t)entry->VendorTable;
    }
    if (info->AcpiRsdp != 0) info->Flags |= LITEOS_BOOTINFO_HAS_ACPI;
    if (info->Smbios != 0 || info->Smbios3 != 0) info->Flags |= LITEOS_BOOTINFO_HAS_SMBIOS;
}

static VOID discover_graphics(EFI_BOOT_SERVICES *bs, LITEOS_BOOT_INFO *info) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (EFI_ERROR(bs->LocateProtocol((EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, 0, (VOID **)&gop)) ||
        gop == 0 || gop->Mode == 0 || gop->Mode->Info == 0) return;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode = gop->Mode->Info;
    info->FrameBufferBase = gop->Mode->FrameBufferBase;
    info->FrameBufferSize = gop->Mode->FrameBufferSize;
    info->FrameBufferWidth = mode->HorizontalResolution;
    info->FrameBufferHeight = mode->VerticalResolution;
    info->FrameBufferPixelsPerScanLine = mode->PixelsPerScanLine;
    info->FrameBufferFormat = mode->PixelFormat;
    for (UINTN i = 0; i < 4; ++i) info->FrameBufferMask[i] = mode->PixelInformation[i];
    if (mode->PixelFormat <= LITEOS_PIXEL_BITMASK) info->Flags |= LITEOS_BOOTINFO_HAS_FRAMEBUFFER;
}

static VOID discover_rng(EFI_BOOT_SERVICES *bs, LITEOS_BOOT_INFO *info) {
    EFI_RNG_PROTOCOL *rng = 0;
    if (EFI_ERROR(bs->LocateProtocol((EFI_GUID *)&gEfiRngProtocolGuid, 0, (VOID **)&rng)) || rng == 0) return;
    if (!EFI_ERROR(rng->GetRNG(rng, 0, sizeof(info->RandomSeed), info->RandomSeed))) info->Flags |= LITEOS_BOOTINFO_HAS_RNG;
}

static UINT32 device_path_u32(const UINT8 *bytes) {
    return (UINT32)bytes[0] | ((UINT32)bytes[1] << 8) |
           ((UINT32)bytes[2] << 16) | ((UINT32)bytes[3] << 24);
}

static UINT64 device_path_u64(const UINT8 *bytes) {
    UINT64 value = 0;
    for (UINTN i = 0; i < 8; ++i) value |= (UINT64)bytes[i] << (i * 8U);
    return value;
}

/* 在 Boot Services 仍然有效时解析启动设备路径，只复制稳定元数据给内核。 */
static BOOLEAN discover_boot_device(EFI_BOOT_SERVICES *bs, EFI_HANDLE device_handle,
                                    LITEOS_BOOT_INFO *info) {
    EFI_DEVICE_PATH_PROTOCOL *path = 0;
    if (bs == 0 || device_handle == 0 || info == 0 ||
        EFI_ERROR(bs->HandleProtocol(device_handle, (EFI_GUID *)&gEfiDevicePathProtocolGuid,
                                     (VOID **)&path)) || path == 0) return 0;

    const UINTN maximum_path_size = 64U * 1024U;
    UINTN path_size = 0;
    BOOLEAN reached_end = 0;
    for (;;) {
        if (path_size > maximum_path_size - sizeof(EFI_DEVICE_PATH_PROTOCOL)) return 0;
        EFI_DEVICE_PATH_PROTOCOL *node =
            (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)path + path_size);
        UINT16 node_size = node->Length;
        if (node_size < sizeof(EFI_DEVICE_PATH_PROTOCOL) ||
            node_size > maximum_path_size - path_size) return 0;
        if (node->Type == EFI_DEVICE_PATH_TYPE_END &&
            node->SubType == EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE) {
            reached_end = 1;
            path_size += node_size;
            break;
        }
        if (node->Type == EFI_DEVICE_PATH_TYPE_MEDIA &&
            node->SubType == EFI_DEVICE_PATH_SUBTYPE_HARDDRIVE && node_size >= 42U) {
            const UINT8 *data = (const UINT8 *)node + sizeof(EFI_DEVICE_PATH_PROTOCOL);
            info->BootDevicePartitionNumber = device_path_u32(data);
            info->BootDevicePartitionStartLba = device_path_u64(data + 4U);
            info->BootDevicePartitionSizeLba = device_path_u64(data + 12U);
            mem_copy(info->BootDevicePartitionSignature, data + 20U,
                     sizeof(info->BootDevicePartitionSignature));
            info->BootDevicePartitionMbrType = data[36];
            info->BootDevicePartitionSignatureType = data[37];
        }
        path_size += node_size;
    }
    if (!reached_end || path_size < sizeof(EFI_DEVICE_PATH_PROTOCOL) ||
        path_size > UINT32_MAX) return 0;

    sha256_compute((const UINT8 *)path, path_size, info->BootDevicePathHash);
    info->BootDeviceHandle = (UINT64)(uintptr_t)device_handle;
    info->BootDevicePathSize = (UINT32)path_size;
    info->Flags |= LITEOS_BOOTINFO_HAS_BOOT_DEVICE;
    return 1;
}

static BOOLEAN secure_boot_enabled(EFI_SYSTEM_TABLE *st) {
    UINT8 secure_boot = 0;
    UINTN size = sizeof(secure_boot);
    static const EFI_GUID global_variable_guid =
        {0x8BE4DF61,0x93CA,0x11D2,{0xAA,0x0D,0x00,0xE0,0x98,0x03,0x2B,0x8C}};
    static CHAR16 secure_boot_name[] = {'S','e','c','u','r','e','B','o','o','t',0};
    if (st == 0 || st->RuntimeServices == 0 || st->RuntimeServices->GetVariable == 0) {
        return 0;
    }
    EFI_GET_VARIABLE get_variable = (EFI_GET_VARIABLE)st->RuntimeServices->GetVariable;
    return !EFI_ERROR(get_variable(secure_boot_name, (EFI_GUID *)&global_variable_guid,
                                   0, &size, &secure_boot)) && secure_boot != 0;
}

static VOID discover_secure_boot(EFI_SYSTEM_TABLE *st, LITEOS_BOOT_INFO *info) {
    if (info != 0 && secure_boot_enabled(st)) info->Flags |= LITEOS_BOOTINFO_SECURE_BOOT;
}

static UINT32 update_state_checksum(const LITEOS_UPDATE_STATE *state) {
    const UINT8 *bytes = (const UINT8 *)state;
    UINT32 hash = 2166136261U;
    UINTN length = sizeof(*state) - sizeof(UINT32) * 2U;
    for (UINTN i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static BOOLEAN update_state_valid(const LITEOS_UPDATE_STATE *state) {
    if (state == 0 || state->Magic != LITEOS_UPDATE_STATE_MAGIC ||
        state->Version != LITEOS_UPDATE_STATE_VERSION ||
        (state->ActiveSlot != LITEOS_UPDATE_SLOT_A && state->ActiveSlot != LITEOS_UPDATE_SLOT_B) ||
        (state->PendingSlot != LITEOS_UPDATE_SLOT_NONE &&
         state->PendingSlot != LITEOS_UPDATE_SLOT_A && state->PendingSlot != LITEOS_UPDATE_SLOT_B) ||
        state->PendingSlot == state->ActiveSlot ||
        state->BootAttempts > LITEOS_UPDATE_MAX_BOOT_ATTEMPTS || state->SafeMode > 1U ||
        state->Reserved != 0U ||
        ((state->ActiveVersion == 0U) != (state->Generation == 0U)) ||
        (state->PendingSlot == LITEOS_UPDATE_SLOT_NONE &&
         (state->PendingVersion != 0U || state->PendingGeneration != 0U)) ||
        (state->PendingSlot != LITEOS_UPDATE_SLOT_NONE &&
         (state->PendingVersion == 0U || state->PendingGeneration == 0U ||
          state->PendingGeneration <= state->Generation)) ||
        state->Checksum != update_state_checksum(state)) return 0;
    return 1;
}

static VOID update_state_default(LITEOS_UPDATE_STATE *state) {
    mem_zero(state, sizeof(*state));
    state->Magic = LITEOS_UPDATE_STATE_MAGIC;
    state->Version = LITEOS_UPDATE_STATE_VERSION;
    state->ActiveSlot = LITEOS_UPDATE_SLOT_A;
    state->PendingSlot = LITEOS_UPDATE_SLOT_NONE;
    state->Checksum = update_state_checksum(state);
}

static EFI_STATUS update_state_load(EFI_SYSTEM_TABLE *st, LITEOS_UPDATE_STATE *state) {
    if (st == 0 || state == 0 || st->RuntimeServices == 0 ||
        st->RuntimeServices->GetVariable == 0) return EFI_UNSUPPORTED;
    UINT32 attributes = 0;
    UINTN size = sizeof(*state);
    EFI_STATUS status = st->RuntimeServices->GetVariable(
        gLiteosUpdateStateName, (EFI_GUID *)&gLiteosUpdateVendorGuid,
        &attributes, &size, state);
    if (status == EFI_NOT_FOUND) return EFI_NOT_FOUND;
    if (status == EFI_BUFFER_TOO_SMALL || EFI_ERROR(status) || size != sizeof(*state)) {
        return EFI_VOLUME_CORRUPTED;
    }
    return update_state_valid(state) ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}

static EFI_STATUS update_state_store(EFI_SYSTEM_TABLE *st, const LITEOS_UPDATE_STATE *state) {
    if (st == 0 || state == 0 || st->RuntimeServices == 0 ||
        st->RuntimeServices->SetVariable == 0) return EFI_UNSUPPORTED;
    LITEOS_UPDATE_STATE copy = *state;
    copy.Checksum = update_state_checksum(&copy);
    return st->RuntimeServices->SetVariable(
        gLiteosUpdateStateName, (EFI_GUID *)&gLiteosUpdateVendorGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(copy), &copy);
}

static EFI_STATUS select_kernel_for_boot(EFI_SYSTEM_TABLE *st, const LOADER_CONFIG *config,
                                         LOADER_UPDATE_SELECTION *selection) {
    if (st == 0 || config == 0 || selection == 0) return EFI_INVALID_PARAMETER;
    mem_zero(selection, sizeof(*selection));
    if (config->has_kernel_a != config->has_kernel_b) return EFI_LOAD_ERROR;
    if (!config->has_kernel_a) {
        selection->path = config->kernel_path;
        selection->has_hash = config->has_hash;
        if (selection->has_hash) mem_copy(selection->hash, config->kernel_hash, sizeof(selection->hash));
        selection->has_signature = config->has_signature;
        if (selection->has_signature) {
            mem_copy(selection->signature, config->kernel_signature,
                     sizeof(selection->signature));
        }
        if (config->verify_required && (!selection->has_hash || !selection->has_signature)) {
            return EFI_SECURITY_VIOLATION;
        }
        return EFI_SUCCESS;
    }

    LITEOS_UPDATE_STATE state;
    EFI_STATUS status = update_state_load(st, &state);
    BOOLEAN store = 0;
    if (status == EFI_NOT_FOUND) {
        update_state_default(&state);
        store = 1;
    } else if (EFI_ERROR(status)) {
        return status;
    }
    selection->enabled = 1;
    selection->active_slot = state.ActiveSlot;
    selection->boot_slot = state.ActiveSlot;
    selection->version = state.ActiveVersion;
    selection->generation = state.Generation;
    if (state.SafeMode != 0U) {
        selection->safe_mode = 1;
    } else if (state.PendingSlot != LITEOS_UPDATE_SLOT_NONE) {
        if (state.BootAttempts >= LITEOS_UPDATE_MAX_BOOT_ATTEMPTS) {
            state.SafeMode = 1U;
            state.PendingSlot = LITEOS_UPDATE_SLOT_NONE;
            state.PendingVersion = 0U;
            state.PendingGeneration = 0U;
            selection->safe_mode = 1;
            store = 1;
        } else {
            ++state.BootAttempts;
            selection->pending = 1;
            selection->boot_slot = state.PendingSlot;
            selection->version = state.PendingVersion;
            selection->generation = state.PendingGeneration;
            store = 1;
        }
    }
    selection->boot_attempts = state.BootAttempts;
    if (store) {
        status = update_state_store(st, &state);
        if (EFI_ERROR(status)) return status;
    }
    selection->path = selection->boot_slot == LITEOS_UPDATE_SLOT_A ?
                      config->kernel_a_path : config->kernel_b_path;
    UINTN hash_slot = selection->boot_slot == LITEOS_UPDATE_SLOT_A ? 0U : 1U;
    if (config->has_slot_hash[hash_slot]) {
        selection->has_hash = 1;
        mem_copy(selection->hash, config->slot_hash[hash_slot], sizeof(selection->hash));
    } else if (config->has_hash) {
        selection->has_hash = 1;
        mem_copy(selection->hash, config->kernel_hash, sizeof(selection->hash));
    }
    if (config->has_slot_signature[hash_slot]) {
        selection->has_signature = 1;
        mem_copy(selection->signature, config->slot_signature[hash_slot],
                 sizeof(selection->signature));
    } else if (config->has_signature) {
        selection->has_signature = 1;
        mem_copy(selection->signature, config->kernel_signature,
                 sizeof(selection->signature));
    }
    if (config->verify_required && (!selection->has_hash || !selection->has_signature)) {
        return EFI_SECURITY_VIOLATION;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS allocate_command_line(EFI_BOOT_SERVICES *bs, LITEOS_BOOT_INFO *info, const CHAR8 *command_line) {
    UINTN length = ascii_length(command_line) + 1;
    VOID *copy = 0;
    EFI_STATUS status = bs->AllocatePool(EFI_LOADER_DATA, length, &copy);
    if (EFI_ERROR(status)) return status;
    mem_copy(copy, command_line, length);
    info->CommandLine = (UINT64)(uintptr_t)copy;
    info->CommandLineSize = length;
    return EFI_SUCCESS;
}

static EFI_STATUS allocate_loader_name(EFI_BOOT_SERVICES *bs, LITEOS_BOOT_INFO *info) {
    UINTN length = ascii_length(g_loader_name) + 1;
    VOID *copy = 0;
    EFI_STATUS status = bs->AllocatePool(EFI_LOADER_DATA, length, &copy);
    if (EFI_ERROR(status)) return status;
    mem_copy(copy, g_loader_name, length);
    info->LoaderName = (UINT64)(uintptr_t)copy;
    info->LoaderNameSize = length;
    return EFI_SUCCESS;
}

static EFI_STATUS capture_memory_map(EFI_BOOT_SERVICES *bs, EFI_MEMORY_DESCRIPTOR **buffer,
                                     UINTN *capacity, UINTN *size, UINTN *key,
                                     UINTN *descriptor_size, UINT32 *descriptor_version) {
    EFI_STATUS status;
    UINTN requested = 0;
    UINTN current_descriptor_size = 0;
    UINT32 current_version = 0;
    status = bs->GetMemoryMap(&requested, 0, key, &current_descriptor_size, &current_version);
    if (status != EFI_BUFFER_TOO_SMALL || current_descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR)) return status;
    if (*buffer == 0 || *capacity < requested + current_descriptor_size * MAP_EXTRA_DESCRIPTORS) {
        if (*buffer != 0) bs->FreePool(*buffer);
        *capacity = requested + current_descriptor_size * MAP_EXTRA_DESCRIPTORS;
        status = bs->AllocatePool(EFI_LOADER_DATA, *capacity, (VOID **)buffer);
        if (EFI_ERROR(status)) { *buffer = 0; *capacity = 0; return status; }
    }
    *size = *capacity;
    status = bs->GetMemoryMap(size, *buffer, key, descriptor_size, descriptor_version);
    if (!EFI_ERROR(status)) {
        *descriptor_size = current_descriptor_size > *descriptor_size ? current_descriptor_size : *descriptor_size;
        *descriptor_version = current_version;
    }
    return status;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    g_system_table = system_table;
    if (system_table == 0) return EFI_INVALID_PARAMETER;
    EFI_BOOT_SERVICES *bs = system_table->BootServices;
    if (bs == 0) return EFI_LOAD_ERROR;
    if (bs->SetWatchdogTimer != 0) bs->SetWatchdogTimer(0, 0, 0, 0);
    console_ascii("LiteOS UEFI loader\r\n");

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_STATUS status = get_filesystem(bs, image_handle, &fs);
    if (EFI_ERROR(status)) { report_error("filesystem unavailable", status); return status; }

    LOADER_CONFIG config;
    mem_zero(&config, sizeof(config));
    ascii_to_char16(config.kernel_path, ARRAY_SIZE(config.kernel_path), "\\EFI\\LITEOS\\kernel.elf");
    copy_ascii(config.cmdline, sizeof(config.cmdline), g_default_cmdline, ascii_length(g_default_cmdline));

    static CHAR16 config_path[] = {'\\','E','F','I','\\','L','I','T','E','O','S','\\','l','o','a','d','e','r','.','c','o','n','f',0};
    VOID *config_data = 0;
    UINTN config_size = 0;
    status = read_file(bs, fs, config_path, &config_data, &config_size);
    if (!EFI_ERROR(status)) {
        status = parse_config(config_data, config_size, &config);
        bs->FreePool(config_data);
        if (EFI_ERROR(status)) { report_error("invalid loader.conf", status); return status; }
    } else if (status != EFI_NOT_FOUND && status != EFI_NO_MEDIA) {
        report_error("cannot read loader.conf", status);
        return status;
    }
    if (secure_boot_enabled(system_table) && !config.verify_required) {
        report_error("Secure Boot requires signed kernel policy", EFI_SECURITY_VIOLATION);
        return EFI_SECURITY_VIOLATION;
    }

    LOADER_UPDATE_SELECTION update_selection;
    status = select_kernel_for_boot(system_table, &config, &update_selection);
    if (EFI_ERROR(status)) {
        report_error("cannot select kernel slot", status);
        return status;
    }

    VOID *kernel_file = 0;
    UINTN kernel_file_size = 0;
    status = read_file(bs, fs, (CHAR16 *)update_selection.path, &kernel_file, &kernel_file_size);
    if (EFI_ERROR(status)) { report_error("cannot read kernel.elf", status); return status; }
    UINT8 kernel_image_hash[32];
    sha256_compute((const UINT8 *)kernel_file, kernel_file_size, kernel_image_hash);
    if (update_selection.has_hash) {
        if (!sha256_equal(kernel_image_hash, update_selection.hash)) {
            bs->FreePool(kernel_file);
            report_error("kernel SHA-256 mismatch", EFI_SECURITY_VIOLATION);
            return EFI_SECURITY_VIOLATION;
        }
    }
    if (update_selection.has_signature &&
        !rsa2048_sha256_verify(kernel_image_hash, update_selection.signature)) {
        bs->FreePool(kernel_file);
        report_error("kernel RSA-2048 signature mismatch", EFI_SECURITY_VIOLATION);
        return EFI_SECURITY_VIOLATION;
    }

    LITEOS_ELF_IMAGE image;
    status = elf_load(bs, (const UINT8 *)kernel_file, kernel_file_size, &image);
    bs->FreePool(kernel_file);
    if (EFI_ERROR(status)) { report_error("invalid or unsupported kernel.elf", status); return status; }

    LITEOS_BOOT_INFO *boot_info = 0;
    status = bs->AllocatePool(EFI_LOADER_DATA, sizeof(*boot_info), (VOID **)&boot_info);
    if (EFI_ERROR(status)) { report_error("cannot allocate BootInfo", status); return status; }
    mem_zero(boot_info, sizeof(*boot_info));
    boot_info->Magic = LITEOS_BOOTINFO_MAGIC;
    boot_info->Version = LITEOS_BOOTINFO_VERSION;
    boot_info->Size = sizeof(*boot_info);
    boot_info->KernelPhysicalBase = image.PhysicalBase;
    boot_info->KernelVirtualBase = image.VirtualBase;
    boot_info->KernelSize = image.Size;
    boot_info->KernelEntry = image.VirtualEntry;
    for (UINTN hash_index = 0; hash_index < sizeof(boot_info->KernelImageHash); ++hash_index) {
        boot_info->KernelImageHash[hash_index] = kernel_image_hash[hash_index];
    }
    boot_info->Flags |= LITEOS_BOOTINFO_HAS_KERNEL_HASH;
    boot_info->BootInfoPhysicalBase = (UINT64)(uintptr_t)boot_info;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = 0;
    if (!EFI_ERROR(bs->HandleProtocol(image_handle, (EFI_GUID *)&gEfiLoadedImageProtocolGuid,
                                      (VOID **)&loaded_image)) && loaded_image != 0) {
        boot_info->LoaderImageBase = (UINT64)(uintptr_t)loaded_image->ImageBase;
        boot_info->LoaderImageSize = loaded_image->ImageSize;
        if (!discover_boot_device(bs, loaded_image->DeviceHandle, boot_info)) {
            report_error("cannot discover boot device", EFI_NOT_FOUND);
            return EFI_NOT_FOUND;
        }
    } else {
        report_error("loaded image protocol is unavailable", EFI_NOT_FOUND);
        return EFI_NOT_FOUND;
    }
    if (update_selection.has_hash) boot_info->Flags |= LITEOS_BOOTINFO_KERNEL_VERIFIED;
    if (update_selection.has_signature) boot_info->Flags |= LITEOS_BOOTINFO_KERNEL_SIGNED;
    if (update_selection.enabled) {
        boot_info->Flags |= LITEOS_BOOTINFO_HAS_UPDATE_STATE;
        if (update_selection.pending) boot_info->Flags |= LITEOS_BOOTINFO_UPDATE_PENDING;
        if (update_selection.safe_mode) boot_info->Flags |= LITEOS_BOOTINFO_UPDATE_SAFE_MODE;
        boot_info->UpdateActiveSlot = update_selection.active_slot;
        boot_info->UpdateBootSlot = update_selection.boot_slot;
        boot_info->UpdateBootAttempts = update_selection.boot_attempts;
        boot_info->UpdateVersion = update_selection.version;
        boot_info->UpdateGeneration = update_selection.generation;
    }
    status = allocate_command_line(bs, boot_info, config.cmdline);
    if (EFI_ERROR(status)) { report_error("cannot allocate command line", status); return status; }
    status = allocate_loader_name(bs, boot_info);
    if (EFI_ERROR(status)) { report_error("cannot allocate loader name", status); return status; }
    discover_tables(system_table, boot_info);
    discover_graphics(bs, boot_info);
    discover_rng(bs, boot_info);
    discover_secure_boot(system_table, boot_info);
    boot_info->RuntimeServices = (UINT64)(uintptr_t)system_table->RuntimeServices;
    boot_info->SystemTable = (UINT64)(uintptr_t)system_table;

    EFI_PHYSICAL_ADDRESS bootstrap_stack = 0;
    status = bs->AllocatePages(AllocateAnyPages, EFI_LOADER_DATA,
                               BOOTSTRAP_STACK_ALLOCATION_PAGES, &bootstrap_stack);
    if (EFI_ERROR(status)) { report_error("cannot allocate bootstrap stack", status); return status; }
    if (bootstrap_stack > (EFI_PHYSICAL_ADDRESS)-1 - (BOOTSTRAP_STACK_SIZE - 1ULL)) {
        report_error("bootstrap stack address overflow", EFI_OUT_OF_RESOURCES);
        return EFI_OUT_OF_RESOURCES;
    }
    bootstrap_stack = (bootstrap_stack + BOOTSTRAP_STACK_SIZE - 1ULL) &
                      ~(BOOTSTRAP_STACK_SIZE - 1ULL);
    mem_zero((VOID *)(uintptr_t)bootstrap_stack, BOOTSTRAP_STACK_SIZE);
    boot_info->BootstrapStackBase = bootstrap_stack;
    boot_info->BootstrapStackSize = BOOTSTRAP_STACK_SIZE;
    boot_info->BootstrapStackTop = bootstrap_stack + BOOTSTRAP_STACK_SIZE;

    /*
     * SIPI 向量只有 8 位，目标物理页必须位于 1 MiB 以下。由 Loader 预留，
     * 内核稍后复制 trampoline；这样退出 Boot Services 后无需再寻找低端内存。
     */
    EFI_PHYSICAL_ADDRESS ap_trampoline = AP_TRAMPOLINE_MAX_ADDRESS;
    status = bs->AllocatePages(AllocateMaxAddress, EFI_LOADER_DATA,
                               AP_TRAMPOLINE_PAGES, &ap_trampoline);
    if (EFI_ERROR(status) || ap_trampoline < 0x1000ULL ||
        ap_trampoline > 0xFF000ULL || (ap_trampoline & 0xFFFULL) != 0) {
        report_error("cannot allocate AP trampoline", EFI_OUT_OF_RESOURCES);
        return EFI_OUT_OF_RESOURCES;
    }
    mem_zero((VOID *)(uintptr_t)ap_trampoline, 4096U);
    boot_info->ApTrampolineBase = ap_trampoline;
    boot_info->ApTrampolineSize = 4096U;

    EFI_MEMORY_DESCRIPTOR *memory_map = 0;
    UINTN map_capacity = 0, map_size = 0, map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    for (UINTN attempt = 0; attempt < MAX_MAP_RETRIES; ++attempt) {
        status = capture_memory_map(bs, &memory_map, &map_capacity, &map_size, &map_key,
                                    &descriptor_size, &descriptor_version);
        if (EFI_ERROR(status)) { report_error("cannot obtain memory map", status); return status; }
        boot_info->MemoryMap = (UINT64)(uintptr_t)memory_map;
        boot_info->MemoryMapSize = map_size;
        boot_info->MemoryMapBufferSize = map_capacity;
        boot_info->MemoryDescriptorSize = descriptor_size;
        boot_info->MemoryDescriptorVersion = descriptor_version;
        status = bs->ExitBootServices(image_handle, map_key);
        if (!EFI_ERROR(status)) break;
        if (status != EFI_INVALID_PARAMETER) { report_error("ExitBootServices failed", status); return status; }
    }
    if (EFI_ERROR(status)) { report_error("ExitBootServices retry limit", status); return status; }

    /* 最终 MapKey 被接受后，合并相邻的可用内存范围。 */
    map_size = liteos_merge_usable_memory_map(memory_map, map_size, descriptor_size);
    boot_info->MemoryMapSize = map_size;

    /* 使用私有启动栈进入内核。 */
    UINT64 handoff_cr3 = 0;
    LITEOS_ELF_IMAGE handoff_image;
    handoff_image.PhysicalBase = boot_info->KernelPhysicalBase;
    handoff_image.VirtualBase = boot_info->KernelVirtualBase;
    handoff_image.Size = boot_info->KernelSize;
    if (!build_handoff_page_tables(&handoff_image, &handoff_cr3)) {
        report_error("cannot build handoff page tables", EFI_LOAD_ERROR);
        return EFI_LOAD_ERROR;
    }
    loader_enter_kernel(boot_info->KernelEntry, boot_info, boot_info->BootstrapStackTop, handoff_cr3);
    return EFI_ABORTED;
}
