#pragma once

#include <kernel/base.h>

struct LITEOS_FAT32;
struct liteos_boot_info;

/* The real-hardware test build keeps a persistent boot trace on the root FAT
 * volume and returns to the host OS after either a pass or a fatal failure. */
bool liteos_realtest_enabled(void);
void liteos_realtest_boot_info(const struct liteos_boot_info *info);
void liteos_realtest_mark(const char *state);
void liteos_realtest_checkpoint(const char *state);
void liteos_realtest_mark_number(const char *prefix, uint32_t value);
void liteos_realtest_clear_failure_state(void);
void liteos_realtest_mark_value(const char *prefix, uint32_t value);
void liteos_realtest_mark_xhci_control(const char *kind, uint32_t error,
                                       uint32_t slot, uint32_t port,
                                       uint32_t length, uint32_t event_index,
                                       uint32_t event_cycle);
void liteos_realtest_disable_runtime_services(void);
void liteos_realtest_capture(const char *text);
void liteos_realtest_fat_ready(struct LITEOS_FAT32 *filesystem);
void liteos_realtest_fat_lost(const char *reason);
void liteos_realtest_filesystem_ready(void);
bool liteos_realtest_flush(void);
void liteos_realtest_record_failure(const char *file, uint32_t line);
void liteos_realtest_record_exception(uint64_t vector, uint64_t error_code,
                                      uint64_t rip, uint64_t fault_address,
                                      bool has_fault_address);
void liteos_realtest_finish_success(void);
void liteos_realtest_finish_failure(void) __attribute__((noreturn));
