/* Recompile against the current kernel headers before inspecting guest state.
 * This object provides DWARF types only; it is never linked into the kernel. */
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/vfs.h>
#include <arch/x86_64/context.h>
#include "../../kernel/graphics/internal.h"
#include "../../kernel/sched/internal.h"

process_t *system_inspection_process_type;
thread_t *system_inspection_thread_type;
wait_queue_t *system_inspection_wait_type;
arch_trap_frame_t *system_inspection_trap_type;
file_t *system_inspection_file_type;
window_server_window_t *system_inspection_window_type;
scheduler_cpu_t *system_inspection_cpu_type;
