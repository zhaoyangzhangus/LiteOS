#include <kernel/api.h>
#include <bootinfo.h>
#include <rsa.h>
#include <uapi/all.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/interrupt.h>
#include <arch/x86_64/syscall.h>
#include <kernel/litefs.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>
#include <kernel/hda.h>

int main(void) {
    page_t *page = 0;
    process_t *process = 0;
    thread_t *thread = 0;
    device_t *device = 0;
    gpu_context_t *gpu = 0;
    socket_t *socket = 0;
    const x86_acpi_platform_t *acpi = 0;
    os_vm_map_args_t args = {0};
    LITEOS_BOOT_INFO boot_info = {0};
    UINT8 rsa_signature[LITEOS_RSA2048_BYTES] = {0};
    (void)page;
    (void)process;
    (void)thread;
    (void)device;
    (void)gpu;
    (void)socket;
    (void)acpi;
    (void)args;
    (void)boot_info;
    (void)rsa_signature;
    return 0;
}
