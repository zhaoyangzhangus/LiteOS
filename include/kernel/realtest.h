#pragma once

#include <kernel/base.h>

struct OS_FAT32;
struct os_boot_info;

/* The real-hardware test build keeps a persistent boot trace on the root FAT
 * volume and returns to the host OS after either a pass or a fatal failure. */
bool realtest_enabled(void);
void realtest_boot_info(const struct os_boot_info *info);
void realtest_mark(const char *state);
void realtest_checkpoint(const char *state);
/* Driver/RM checkpoints run while interrupts and vendor-owned locks may be
 * active.  They must not enter UEFI variables or FAT/VFS; the accumulated
 * capture is flushed at the next safe runtime boundary. */
void realtest_checkpoint_deferred(const char *state);
void realtest_mark_number(const char *prefix, uint32_t value);
void realtest_clear_failure_state(void);
void realtest_mark_value(const char *prefix, uint32_t value);
void realtest_mark_xhci_control(const char *kind, uint32_t error,
                                       uint32_t slot, uint32_t port,
                                       uint32_t length, uint32_t event_index,
                                       uint32_t event_cycle);
void realtest_disable_runtime_services(void);
void realtest_capture(const char *text);
void realtest_fat_ready(struct OS_FAT32 *filesystem);
void realtest_fat_lost(const char *reason);
void realtest_filesystem_ready(void);
bool realtest_flush(void);
void realtest_record_failure(const char *file, uint32_t line);
void realtest_record_exception(uint64_t vector, uint64_t error_code,
                                      uint64_t rip, uint64_t fault_address,
                                      bool has_fault_address);
/* The real-test path must be able to leave the machine even when a driver
 * thread is stuck in a blocking vendor call.  These operations are lock-free
 * from the timer interrupt; the timeout reboot intentionally writes only to
 * the serial port and does not touch the FAT capture state. */
void realtest_timeout_start(void);
void realtest_timeout_stop(void);
bool realtest_timeout_poll(uint64_t now_tsc);
__attribute__((noreturn)) void realtest_timeout_reboot(void);
void realtest_finish_success(void);
void realtest_finish_failure(void) __attribute__((noreturn));
