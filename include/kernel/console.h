#pragma once

#include <kernel/base.h>

/* 内核与驱动共享的 COM1 调试串口输出接口。 */
void liteos_serial_write(const char *text);
void liteos_serial_write_u32(uint32_t value);
