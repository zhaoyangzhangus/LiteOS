#include <arch/x86_64/interrupt.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/process.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <kernel/crash_dump.h>
#include <kernel/console.h>

#define COM1_DATA   0x3F8U
#define COM1_STATUS 0x3FDU

static volatile uint64_t g_exception_count[32];
static volatile bool g_ud_self_test;

static uint8_t port_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void port_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void exception_write(const char *text) {
    while (*text != '\0') {
        for (uint32_t tries = 0; tries < 100000U &&
             (port_in8(COM1_STATUS) & 0x20U) == 0; ++tries) { }
        port_out8(COM1_DATA, (uint8_t)*text++);
    }
}

static void exception_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    exception_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        port_out8(COM1_DATA, (uint8_t)digits[(value >> (uint32_t)shift) & 0xFU]);
    }
}

static uint64_t read_cr2(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static const char *exception_name(uint64_t vector) {
    static const char *const names[32] = {
        "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
        "#DF", "保留", "#TS", "#NP", "#SS", "#GP", "#PF", "保留",
        "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "保留", "保留",
        "保留", "保留", "保留", "保留", "#HV", "#VC", "#SX", "保留",
    };
    return vector < 32U ? names[vector] : "IRQ";
}

static void record_user_opcode(const arch_trap_frame_t *frame) {
    uint8_t bytes[8] = {0};
    if (frame == 0 || (frame->cs & 3U) != 3U ||
        !x86_user_range_valid((const void __user *)(uintptr_t)frame->rip,
                              sizeof(bytes)) ||
        copy_from_user(bytes,
                       (const void __user *)(uintptr_t)frame->rip,
                       sizeof(bytes)) != K_OK) {
        return;
    }
    liteos_serial_write("LITEOS_REALTEST_OPCODE");
    for (uint32_t index = 0U; index < sizeof(bytes); ++index) {
        liteos_serial_write(" ");
        liteos_serial_write_u32(bytes[index]);
    }
    liteos_serial_write("\r\n");
}

void x86_interrupt_dispatch(arch_trap_frame_t *frame) {
    if (frame == 0) {
        liteos_realtest_record_exception(0U, 0U, 0U, 0U, false);
        if (liteos_realtest_enabled()) liteos_realtest_finish_failure();
        exception_write("LITEOS_EXCEPTION_BAD_FRAME\r\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (frame->vector < 32U) ++g_exception_count[frame->vector];

    /* 合法懒分配先解决并重试；无效 UAccess 再通过修复表返回稳定错误码。 */
    if (frame->vector == 14U) {
        uint64_t fault_address = read_cr2();
        bool from_user = (frame->cs & 3U) == 3U;
        bool from_uaccess = x86_uaccess_fault_site(frame);
        if (vm_handle_current_fault((vaddr_t)fault_address,
                                    (uint32_t)frame->error_code,
                                    from_user, from_uaccess) ||
            x86_uaccess_fixup(frame)) return;
        if ((frame->cs & 3U) == 3U) {
            thread_t *thread = sched_current_thread();
            if (thread != 0 && thread->object.type == KOBJECT_TYPE_THREAD &&
                thread->process != 0) {
                exception_write("LITEOS_USER_PAGE_FAULT error=");
                exception_write_hex(frame->error_code);
                exception_write(" rip=");
                exception_write_hex(frame->rip);
                exception_write(" cr2=");
                exception_write_hex(fault_address);
                exception_write("\r\n");
                thread_exit(K_EACCES);
            }
        }
    }
    if (frame->vector == 3U) return;
    if (frame->vector == 6U && g_ud_self_test) {
        frame->rip += 2U; /* 跳过本自检使用的 UD2。 */
        return;
    }

    crash_dump_capture(frame, frame->vector == 14U ? read_cr2() : 0U);
    exception_write("LITEOS_FATAL_EXCEPTION ");
    exception_write(exception_name(frame->vector));
    exception_write(" vector=");
    exception_write_hex(frame->vector);
    exception_write(" error=");
    exception_write_hex(frame->error_code);
    exception_write(" rip=");
    exception_write_hex(frame->rip);
    if (frame->vector == 14U) {
        exception_write(" cr2=");
        exception_write_hex(read_cr2());
    }
    exception_write("\r\n");
    record_user_opcode(frame);
    liteos_realtest_record_exception(frame->vector, frame->error_code,
                                     frame->rip, frame->vector == 14U ?
                                     read_cr2() : 0U, frame->vector == 14U);
    if (liteos_realtest_enabled()) liteos_realtest_finish_failure();
    for (;;) __asm__ volatile ("cli; hlt");
}

bool x86_exception_self_test(void) {
    uint64_t breakpoint_before = g_exception_count[3];
    uint64_t invalid_opcode_before = g_exception_count[6];
    __asm__ volatile ("int3" : : : "memory");
    g_ud_self_test = true;
    __asm__ volatile ("ud2" : : : "memory");
    g_ud_self_test = false;
    return g_exception_count[3] == breakpoint_before + 1U &&
           g_exception_count[6] == invalid_opcode_before + 1U;
}
