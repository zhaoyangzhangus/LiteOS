#pragma once

#include <kernel/base.h>

/* Native Realtek RTL8126A wired-NIC backend. */
bool rtl8126_hardware_present(void);
bool rtl8126_self_test(void);
bool rtl8126_interrupt_ready(void);
bool rtl8126_firmware_required(void);
void rtl8126_emit_diagnostic(void);
