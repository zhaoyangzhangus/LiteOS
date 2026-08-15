#include <kernel/api.h>
#include <uapi/all.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/interrupt.h>
#include <arch/x86_64/syscall.h>

int main(void) {
    page_t *p = 0;
    process_t *proc = 0;
    thread_t *thr = 0;
    device_t *dev = 0;
    gpu_context_t *gpu = 0;
    os_vm_map_args_t args = {0};
    (void)p; (void)proc; (void)thr; (void)dev; (void)gpu; (void)args;
    return 0;
}
