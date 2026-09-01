#include <kernel/console.h>
#include <kernel/fat32.h>
#include <kernel/bootinfo.h>
#include <kernel/realtest.h>
#include <kernel/vfs.h>
#include <arch/x86_64/reboot.h>

#include <stdatomic.h>

#ifndef LITEOS_REALTEST
#define LITEOS_REALTEST 0
#endif

#if LITEOS_REALTEST

#define REALTEST_LOG_CAPACITY (256U * 1024U)
#define REALTEST_WRITE_CHUNK  4096U
#define REALTEST_STATE_CAPACITY 8192U

static char g_realtest_log[REALTEST_LOG_CAPACITY];
static char g_realtest_chunk[REALTEST_WRITE_CHUNK];
static size_t g_realtest_length;
static size_t g_realtest_flushed;
static bool g_realtest_overflow;
static bool g_realtest_flush_busy;
static bool g_realtest_sealed;
static file_t *g_realtest_file;
static LITEOS_FAT32_FILE *g_realtest_raw_file;
static LITEOS_FAT32 *g_realtest_raw_filesystem;
static uint64_t g_realtest_raw_file_offset;
static EFI_SET_VARIABLE g_realtest_set_variable;
static bool g_realtest_runtime_services_enabled;
static bool g_realtest_specific_failure_state;
static uint32_t g_realtest_flush_count;
static atomic_flag g_realtest_lock = ATOMIC_FLAG_INIT;
static char g_realtest_state[REALTEST_STATE_CAPACITY];
static size_t g_realtest_state_length;

static const EFI_GUID g_realtest_variable_guid = {
    0x4C495445U, 0x4F53U, 0x5254U,
    {0x45U, 0x53U, 0x54U, 0x43U, 0x55U, 0x52U, 0x4EU, 0x31U}
};
static CHAR16 g_realtest_variable_name[] = {
    'L', 'i', 't', 'e', 'O', 'S', 'R', 'e', 'a', 'l', 'T', 'e', 's', 't',
    'S', 't', 'a', 't', 'e', 0
};

static uint64_t realtest_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    __asm__ volatile ("cli" : : : "memory");
    while (atomic_flag_test_and_set_explicit(&g_realtest_lock,
                                             memory_order_acquire)) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void realtest_unlock(uint64_t flags) {
    atomic_flag_clear_explicit(&g_realtest_lock, memory_order_release);
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static void append_text(char *buffer, size_t *length, size_t capacity,
                        const char *text) {
    if (buffer == 0 || length == 0 || text == 0) return;
    while (*text != '\0' && *length + 1U < capacity) {
        buffer[(*length)++] = *text++;
    }
    buffer[*length] = '\0';
}

static void append_hex(char *buffer, size_t *length, size_t capacity,
                       uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    append_text(buffer, length, capacity, "0x");
    for (int shift = 60; shift >= 0 && *length + 1U < capacity;
         shift -= 4) {
        buffer[(*length)++] = digits[(value >> (uint32_t)shift) & 0xFU];
    }
    buffer[*length] = '\0';
}

static void append_u32(char *buffer, size_t *length, size_t capacity,
                       uint32_t value) {
    char digits[10];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U && *length + 1U < capacity) {
        buffer[(*length)++] = digits[--count];
    }
    buffer[*length] = '\0';
}

bool liteos_realtest_enabled(void) {
    return true;
}

void liteos_realtest_boot_info(const struct liteos_boot_info *info) {
    const LITEOS_BOOT_INFO *boot_info = (const LITEOS_BOOT_INFO *)info;
    EFI_RUNTIME_SERVICES *services;
    if (boot_info == 0 || boot_info->RuntimeServices == 0 ||
        boot_info->RuntimeServices > UINT32_MAX) return;
    services = (EFI_RUNTIME_SERVICES *)(uintptr_t)boot_info->RuntimeServices;
    if (services->SetVariable == 0 ||
        (UINT64)(uintptr_t)services->SetVariable > UINT32_MAX) return;
    g_realtest_set_variable = services->SetVariable;
    g_realtest_runtime_services_enabled = true;
}

void liteos_realtest_mark(const char *state) {
    char data[REALTEST_STATE_CAPACITY];
    size_t state_length = 0U;
    if (!g_realtest_runtime_services_enabled || g_realtest_set_variable == 0 ||
        state == 0) return;
    while (state[state_length] != '\0') {
        ++state_length;
    }
    if (state_length + 2U >= sizeof(g_realtest_state) ||
        g_realtest_state_length + state_length + 2U >=
            sizeof(g_realtest_state)) return;
    for (size_t index = 0U; index < state_length; ++index) {
        g_realtest_state[g_realtest_state_length++] = state[index];
    }
    g_realtest_state[g_realtest_state_length++] = '\r';
    g_realtest_state[g_realtest_state_length++] = '\n';
    for (size_t index = 0U; index < g_realtest_state_length; ++index) {
        data[index] = g_realtest_state[index];
    }
    (void)g_realtest_set_variable(
        g_realtest_variable_name, (EFI_GUID *)&g_realtest_variable_guid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        (UINTN)g_realtest_state_length, data);
}

void liteos_realtest_checkpoint(const char *state) {
    if (state == 0) return;
    liteos_realtest_mark(state);
    liteos_realtest_capture(state);
    liteos_realtest_capture("\r\n");
    (void)liteos_realtest_flush();
}

void liteos_realtest_mark_number(const char *prefix, uint32_t value) {
    char state[128] = {0};
    size_t length = 0U;
    append_text(state, &length, sizeof(state), prefix != 0 ? prefix : "STATE");
    append_text(state, &length, sizeof(state), "=");
    append_u32(state, &length, sizeof(state), value);
    liteos_realtest_mark(state);
}

void liteos_realtest_clear_failure_state(void) {
    g_realtest_specific_failure_state = false;
}

void liteos_realtest_mark_value(const char *prefix, uint32_t value) {
    char state[128] = {0};
    size_t length = 0U;
    if (g_realtest_specific_failure_state) return;
    append_text(state, &length, sizeof(state), prefix != 0 ? prefix : "STATE");
    append_text(state, &length, sizeof(state), "=");
    append_u32(state, &length, sizeof(state), value);
    g_realtest_specific_failure_state = true;
    liteos_realtest_mark(state);
}

void liteos_realtest_mark_xhci_control(const char *kind, uint32_t error,
                                       uint32_t slot, uint32_t port,
                                       uint32_t length, uint32_t event_index,
                                       uint32_t event_cycle) {
    char state[256] = {0};
    size_t state_length = 0U;
    if (g_realtest_specific_failure_state) return;
    append_text(state, &state_length, sizeof(state), "FAILURE_XHCI_EP0_");
    append_text(state, &state_length, sizeof(state), kind != 0 ? kind : "ERROR");
    append_text(state, &state_length, sizeof(state), " error=");
    append_u32(state, &state_length, sizeof(state), error);
    append_text(state, &state_length, sizeof(state), " slot=");
    append_u32(state, &state_length, sizeof(state), slot);
    append_text(state, &state_length, sizeof(state), " port=");
    append_u32(state, &state_length, sizeof(state), port);
    append_text(state, &state_length, sizeof(state), " length=");
    append_u32(state, &state_length, sizeof(state), length);
    append_text(state, &state_length, sizeof(state), " event_index=");
    append_u32(state, &state_length, sizeof(state), event_index);
    append_text(state, &state_length, sizeof(state), " event_cycle=");
    append_u32(state, &state_length, sizeof(state), event_cycle);
    g_realtest_specific_failure_state = true;
    liteos_realtest_mark(state);
}

void liteos_realtest_disable_runtime_services(void) {
    g_realtest_runtime_services_enabled = false;
    g_realtest_set_variable = 0;
}

static bool raw_file_ready(LITEOS_FAT32 *filesystem) {
    os_file_info_t info = {0};
    if (filesystem == 0 || g_realtest_file != 0 || g_realtest_raw_file != 0 ||
        !liteos_fat32_stat_path(filesystem, "EFI/LITEOS/realtest.log", &info) ||
        info.type != OS_FILE_TYPE_REGULAR) {
        return false;
    }
    if (!liteos_fat32_open(filesystem, "EFI/LITEOS/realtest.log",
                           &g_realtest_raw_file)) {
        return false;
    }
    g_realtest_raw_filesystem = filesystem;
    g_realtest_raw_file_offset = info.size;
    return true;
}

void liteos_realtest_capture(const char *text) {
    uint64_t flags;
    if (text == 0) return;
    flags = realtest_lock();
    if (g_realtest_sealed) {
        realtest_unlock(flags);
        return;
    }
    while (*text != '\0') {
        if (g_realtest_length + 1U >= sizeof(g_realtest_log)) {
            g_realtest_overflow = true;
            break;
        }
        g_realtest_log[g_realtest_length++] = *text++;
    }
    g_realtest_log[g_realtest_length] = '\0';
    realtest_unlock(flags);
}

static bool open_realtest_file(void) {
    static const char *const paths[] = {
        "/EFI/LITEOS/realtest.log",
        "/realtest.log",
    };
    /* The early FAT handle remains the least dependent logging path once
     * scheduler and device workers are running.  Keep using it instead of
     * replacing it with a VFS file that can share page-cache locks. */
    if (g_realtest_raw_filesystem != 0) return true;
    if (g_realtest_file != 0) return true;
    for (uint32_t index = 0U; index < sizeof(paths) / sizeof(paths[0]);
         ++index) {
        file_t *file = 0;
        if (vfs_open_kernel(paths[index],
                            VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_APPEND,
                            0644U, &file) == K_OK) {
            g_realtest_file = file;
            return true;
        }
    }
    return false;
}

bool liteos_realtest_flush(void) {
    bool success = true;
    uint64_t flags;
    if (g_realtest_file == 0 && g_realtest_raw_filesystem == 0) return false;
    flags = realtest_lock();
    if (g_realtest_flush_busy || g_realtest_flushed >= g_realtest_length) {
        realtest_unlock(flags);
        return true;
    }
    g_realtest_flush_busy = true;
    realtest_unlock(flags);
    ++g_realtest_flush_count;
    liteos_realtest_mark_number("REALTEST_FLUSH_BEGIN", g_realtest_flush_count);

    for (;;) {
        size_t offset;
        size_t count;
        uint64_t written = 0U;
        bool write_ok;
        flags = realtest_lock();
        if (g_realtest_flushed >= g_realtest_length) {
            realtest_unlock(flags);
            break;
        }
        offset = g_realtest_flushed;
        count = g_realtest_length - offset;
        if (count > sizeof(g_realtest_chunk)) count = sizeof(g_realtest_chunk);
        for (size_t index = 0U; index < count; ++index) {
            g_realtest_chunk[index] = g_realtest_log[offset + index];
        }
        realtest_unlock(flags);

        if (g_realtest_file != 0) {
            write_ok = vfs_write_kernel(g_realtest_file, g_realtest_chunk, count,
                                        &written) == K_OK;
        } else if (g_realtest_raw_file != 0) {
            uint32_t raw_written = 0U;
            write_ok = liteos_fat32_write_file(
                g_realtest_raw_file, g_realtest_raw_file_offset + offset,
                g_realtest_chunk, (uint32_t)count, &raw_written);
            written = raw_written;
        } else {
            uint32_t raw_written = 0U;
            write_ok = liteos_fat32_write_path(
                g_realtest_raw_filesystem, "EFI/LITEOS/realtest.log",
                g_realtest_raw_file_offset + offset, g_realtest_chunk,
                (uint32_t)count, &raw_written);
            written = raw_written;
        }
        if (!write_ok || written != count) {
            liteos_serial_write_serial_only(
                "LITEOS_REALTEST_FLUSH_WRITE_FAIL\r\n");
            success = false;
            break;
        }
        flags = realtest_lock();
        if (g_realtest_flushed == offset) g_realtest_flushed += count;
        realtest_unlock(flags);
    }
    if (success) {
        liteos_realtest_mark_number("REALTEST_FLUSH_WRITE_OK",
                                    g_realtest_flush_count);
        liteos_realtest_mark_number("REALTEST_FLUSH_SYNC_BEGIN",
                                    g_realtest_flush_count);
        bool sync_ok = g_realtest_file != 0 ?
            vfs_fsync(g_realtest_file) == K_OK :
            liteos_fat32_sync(g_realtest_raw_filesystem);
        if (!sync_ok) {
            liteos_serial_write_serial_only(
                "LITEOS_REALTEST_FLUSH_SYNC_FAIL\r\n");
            success = false;
        } else {
            liteos_realtest_mark_number("REALTEST_FLUSH_DONE",
                                        g_realtest_flush_count);
        }
    }
    flags = realtest_lock();
    g_realtest_flush_busy = false;
    realtest_unlock(flags);
    return success && !g_realtest_overflow;
}

static void seal_realtest_capture(void) {
    uint64_t flags = realtest_lock();
    g_realtest_sealed = true;
    realtest_unlock(flags);
}

void liteos_realtest_fat_ready(struct LITEOS_FAT32 *filesystem) {
    LITEOS_FAT32 *fat = (LITEOS_FAT32 *)filesystem;
    /* The Windows test harness removes the previous log before copying the
     * ESP.  Create the file while FAT is still the only storage owner so the
     * runtime logger never falls back to the VFS/page-cache path. */
    if (!raw_file_ready(fat)) {
        if (!liteos_fat32_create_path(fat, "EFI/LITEOS/realtest.log", 0) ||
            !raw_file_ready(fat)) {
            liteos_realtest_mark("LITEOS_REALTEST_FAT_LOG_CREATE_FAIL");
            return;
        }
        liteos_realtest_mark("LITEOS_REALTEST_FAT_LOG_CREATED");
    }
    liteos_realtest_capture("LITEOS_REALTEST_FAT_READY\r\n");
    (void)liteos_realtest_flush();
}

void liteos_realtest_fat_lost(const char *reason) {
    if (g_realtest_raw_filesystem == 0 || g_realtest_file != 0) return;
    liteos_realtest_capture("LITEOS_REALTEST_FAT_LOST ");
    liteos_realtest_capture(reason != 0 ? reason : "unknown");
    liteos_realtest_capture("\r\n");
    (void)liteos_realtest_flush();
    if (g_realtest_raw_file != 0) {
        (void)liteos_fat32_close(g_realtest_raw_file);
        g_realtest_raw_file = 0;
    }
    g_realtest_raw_filesystem = 0;
    g_realtest_raw_file_offset = 0U;
}

void liteos_realtest_filesystem_ready(void) {
    bool flushed;
    if (g_realtest_file != 0 || g_realtest_raw_file != 0 ||
        g_realtest_raw_filesystem != 0) {
        liteos_realtest_capture("LITEOS_REALTEST_LOG_READY\r\n");
        flushed = liteos_realtest_flush();
        if (flushed) liteos_realtest_disable_runtime_services();
        return;
    }
    if (!open_realtest_file()) return;
    liteos_realtest_capture("LITEOS_REALTEST_LOG_READY\r\n");
    flushed = liteos_realtest_flush();
    if (flushed) liteos_realtest_disable_runtime_services();
}

void liteos_realtest_record_failure(const char *file, uint32_t line) {
    char record[256] = {0};
    char state[256] = {0};
    size_t length = 0U;
    size_t state_length = 0U;
    append_text(record, &length, sizeof(record), "LITEOS_REALTEST_FAIL");
    append_text(state, &state_length, sizeof(state), "FAILURE");
    if (file != 0) {
        append_text(record, &length, sizeof(record), " loc=");
        append_text(record, &length, sizeof(record), file);
        append_text(record, &length, sizeof(record), ":");
        append_u32(record, &length, sizeof(record), line);
        append_text(state, &state_length, sizeof(state), " ");
        append_text(state, &state_length, sizeof(state), file);
        append_text(state, &state_length, sizeof(state), ":");
        append_u32(state, &state_length, sizeof(state), line);
    }
    append_text(record, &length, sizeof(record), "\r\n");
    if (!g_realtest_specific_failure_state) liteos_realtest_mark(state);
    liteos_realtest_capture(record);
}

void liteos_realtest_record_exception(uint64_t vector, uint64_t error_code,
                                      uint64_t rip, uint64_t fault_address,
                                      bool has_fault_address) {
    char record[256] = {0};
    size_t length = 0U;
    append_text(record, &length, sizeof(record),
                "LITEOS_REALTEST_EXCEPTION vector=");
    append_hex(record, &length, sizeof(record), vector);
    append_text(record, &length, sizeof(record), " error=");
    append_hex(record, &length, sizeof(record), error_code);
    append_text(record, &length, sizeof(record), " rip=");
    append_hex(record, &length, sizeof(record), rip);
    if (has_fault_address) {
        append_text(record, &length, sizeof(record), " cr2=");
        append_hex(record, &length, sizeof(record), fault_address);
    }
    append_text(record, &length, sizeof(record), "\r\n");
    liteos_realtest_capture(record);
}

void liteos_realtest_finish_success(void) {
    /* Keep capture open: the desktop is the real hardware test boundary, so
     * input and compositor diagnostics must remain durable after PASS. */
    liteos_realtest_mark("LITEOS_REALTEST_PASS");
    liteos_realtest_capture("LITEOS_REALTEST_PASS\r\n");
    liteos_realtest_capture("LITEOS_REALTEST_DESKTOP_RUNNING\r\n");
    (void)liteos_realtest_flush();
    liteos_serial_write_serial_only("LITEOS_REALTEST_PASS\r\n");
}

void liteos_realtest_finish_failure(void) {
    liteos_realtest_mark("LITEOS_REALTEST_FAIL");
    liteos_realtest_capture("LITEOS_REALTEST_FAIL\r\n");
    seal_realtest_capture();
    (void)liteos_realtest_flush();
    liteos_serial_write_serial_only("LITEOS_REALTEST_FAIL\r\n");
    x86_system_reboot();
}

#else

bool liteos_realtest_enabled(void) { return false; }
void liteos_realtest_boot_info(const struct liteos_boot_info *info) {
    (void)info;
}
void liteos_realtest_mark(const char *state) { (void)state; }
void liteos_realtest_checkpoint(const char *state) { (void)state; }
void liteos_realtest_mark_number(const char *prefix, uint32_t value) {
    (void)prefix;
    (void)value;
}
void liteos_realtest_clear_failure_state(void) { }
void liteos_realtest_mark_value(const char *prefix, uint32_t value) {
    (void)prefix;
    (void)value;
}
void liteos_realtest_mark_xhci_control(const char *kind, uint32_t error,
                                       uint32_t slot, uint32_t port,
                                       uint32_t length, uint32_t event_index,
                                       uint32_t event_cycle) {
    (void)kind;
    (void)error;
    (void)slot;
    (void)port;
    (void)length;
    (void)event_index;
    (void)event_cycle;
}
void liteos_realtest_disable_runtime_services(void) { }
void liteos_realtest_capture(const char *text) { (void)text; }
void liteos_realtest_fat_ready(struct LITEOS_FAT32 *filesystem) {
    (void)filesystem;
}
void liteos_realtest_fat_lost(const char *reason) { (void)reason; }
void liteos_realtest_filesystem_ready(void) { }
bool liteos_realtest_flush(void) { return false; }
void liteos_realtest_record_failure(const char *file, uint32_t line) {
    (void)file;
    (void)line;
}
void liteos_realtest_record_exception(uint64_t vector, uint64_t error_code,
                                      uint64_t rip, uint64_t fault_address,
                                      bool has_fault_address) {
    (void)vector;
    (void)error_code;
    (void)rip;
    (void)fault_address;
    (void)has_fault_address;
}
void liteos_realtest_finish_success(void) { }
void liteos_realtest_finish_failure(void) {
    x86_system_reboot();
    __builtin_unreachable();
}

#endif
