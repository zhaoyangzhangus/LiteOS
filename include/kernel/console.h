#pragma once

#include <kernel/base.h>

/*
 * 内核与驱动共享的调试输出接口。默认只绘制到 GOP 控制台；构建时
 * 显式设置 LITEOS_DEBUG_SERIAL=1 才会额外镜像到 COM1。
 * 保留 serial 名称是为了兼容现有驱动调用点。
 */
void liteos_serial_write(const char *text);
void liteos_serial_write_u32(uint32_t value);
