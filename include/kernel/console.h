#pragma once

#include <kernel/base.h>

/*
 * 内核与驱动共享的调试输出接口。默认只绘制到 GOP 控制台；构建时
 * 显式设置 LITEOS_DEBUG_SERIAL=1 才会额外镜像到 COM1。
 * 保留 serial 名称是为了兼容现有驱动调用点。
 */
void liteos_serial_write(const char *text);
void liteos_serial_write_u32(uint32_t value);
void liteos_serial_write_u32_serial_only(uint32_t value);
void liteos_serial_enable_concurrency(void);

/* Internal serial transaction primitives.  They let location-aware stage
 * records share the COM1 lock with printf without drawing into the GOP. */
typedef struct liteos_serial_guard {
    uint64_t flags;
    uint8_t owned;
    uint8_t preempt_disabled;
} liteos_serial_guard_t;

void liteos_serial_guard_enter(liteos_serial_guard_t *guard);
void liteos_serial_guard_leave(liteos_serial_guard_t *guard);
void liteos_serial_write_guarded(const char *text);
void liteos_serial_write_record_guarded(const char *text);
void liteos_serial_write_serial_only(const char *text);
int liteos_serial_printf_serial_only(const char *format, ...);

/* The kernel terminal is a 12x24 fixed-cell A8 console.  printk formats one
 * complete record before it reaches the character/color buffers; the backend
 * then renders the dirty rows to GOP in one transaction. */
int printk(const char *format, ...);
int printf(const char *format, ...);
void liteos_console_refresh(void);
void liteos_console_set_color(uint32_t color);
