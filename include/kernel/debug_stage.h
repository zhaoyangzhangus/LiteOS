#pragma once

#include <kernel/base.h>

/*
 * Boot/runtime phases are deliberately small and stable.  The numeric values
 * are part of the debug-log contract: a serial trace can be decoded even when
 * the kernel is too early in boot to print a symbolic name.
 */
typedef enum debug_phase {
    OS_DEBUG_PHASE_BOOT = 1,
    OS_DEBUG_PHASE_CPU = 2,
    OS_DEBUG_PHASE_MEMORY = 3,
    OS_DEBUG_PHASE_SCHEDULER = 4,
    OS_DEBUG_PHASE_DRIVER = 5,
    OS_DEBUG_PHASE_STORAGE = 6,
    OS_DEBUG_PHASE_NETWORK = 7,
    OS_DEBUG_PHASE_DISPLAY = 8,
    OS_DEBUG_PHASE_USER = 9,
    OS_DEBUG_PHASE_USER_RUNTIME = 10,
    OS_DEBUG_PHASE_DESKTOP = 11,

    /*
     * The implementation specification has its own 0..20 phase contract.
     * Keep it in a separate numeric range so existing operational phase
     * names and old log parsers remain stable while a debug image can still
     * locate every specification phase.
     */
    OS_DEBUG_PHASE_SPEC_0 = 32,
    OS_DEBUG_PHASE_SPEC_1 = 33,
    OS_DEBUG_PHASE_SPEC_2 = 34,
    OS_DEBUG_PHASE_SPEC_3 = 35,
    OS_DEBUG_PHASE_SPEC_4 = 36,
    OS_DEBUG_PHASE_SPEC_5 = 37,
    OS_DEBUG_PHASE_SPEC_6 = 38,
    OS_DEBUG_PHASE_SPEC_7 = 39,
    OS_DEBUG_PHASE_SPEC_8 = 40,
    OS_DEBUG_PHASE_SPEC_9 = 41,
    OS_DEBUG_PHASE_SPEC_10 = 42,
    OS_DEBUG_PHASE_SPEC_11 = 43,
    OS_DEBUG_PHASE_SPEC_12 = 44,
    OS_DEBUG_PHASE_SPEC_13 = 45,
    OS_DEBUG_PHASE_SPEC_14 = 46,
    OS_DEBUG_PHASE_SPEC_15 = 47,
    OS_DEBUG_PHASE_SPEC_16 = 48,
    OS_DEBUG_PHASE_SPEC_17 = 49,
    OS_DEBUG_PHASE_SPEC_18 = 50,
    OS_DEBUG_PHASE_SPEC_19 = 51,
    OS_DEBUG_PHASE_SPEC_20 = 52,

    /* Refactoring-roadmap phases 0..9 use a third range.  These records
     * describe repository/build gates, not OS implementation claims. */
    OS_DEBUG_PHASE_REFACTOR_0 = 64,
    OS_DEBUG_PHASE_REFACTOR_1 = 65,
    OS_DEBUG_PHASE_REFACTOR_2 = 66,
    OS_DEBUG_PHASE_REFACTOR_3 = 67,
    OS_DEBUG_PHASE_REFACTOR_4 = 68,
    OS_DEBUG_PHASE_REFACTOR_5 = 69,
    OS_DEBUG_PHASE_REFACTOR_6 = 70,
    OS_DEBUG_PHASE_REFACTOR_7A = 71,
    OS_DEBUG_PHASE_REFACTOR_7B = 72,
    OS_DEBUG_PHASE_REFACTOR_8 = 73,
    OS_DEBUG_PHASE_REFACTOR_9 = 74,
} debug_phase_t;

/* Common step identifiers used by the current boot and runtime paths. */
enum {
    OS_DEBUG_STEP_ENTER = 1,
    OS_DEBUG_STEP_LOAD = 2,
    OS_DEBUG_STEP_CREATE = 3,
    OS_DEBUG_STEP_START = 4,
    OS_DEBUG_STEP_READY = 5,
    OS_DEBUG_STEP_EXIT = 6,
    OS_DEBUG_STEP_WAIT_ONE_ENTER = 7,
    OS_DEBUG_STEP_WAIT_ONE_BLOCK = 8,
    OS_DEBUG_STEP_WAIT_ONE_RETURN = 9,
    OS_DEBUG_STEP_THREAD_EXIT_ENTER = 10,
    OS_DEBUG_STEP_THREAD_EXIT_PUBLISHED = 11,
    OS_DEBUG_STEP_THREAD_EXIT_SCHEDULE = 12,
    OS_DEBUG_STEP_USER_MARK = 13,
    OS_DEBUG_STEP_STRESS = 14,
    /* The phase exists in this build, but its acceptance implementation is
     * intentionally not claimed yet. */
    OS_DEBUG_STEP_PENDING = 15,
    /* A refactor boundary has landed, but its complete acceptance gate is
     * still open. */
    OS_DEBUG_STEP_PROGRESS = 16,
    /* A fatal path stopped the current phase.  The status and call site are
     * emitted for failure-only boot diagnostics. */
    OS_DEBUG_STEP_FAIL = 17,
};

/*
 * Publish a phase transition.  The latest phase/step/value/sequence remain
 * available to a debugger in all builds; OS_DEBUG_SERIAL additionally
 * emits one COM1-only record.  It never writes the GOP framebuffer.
 *
 * Callers use the macros below so every record carries the source location
 * that emitted it.  The explicit *_at entry points are kept separate from
 * the macros so the implementation can still export a normal function
 * symbol for debuggers and compatibility callers.
 */
void kernel_debug_stage_at(uint16_t phase, uint16_t step, uint32_t value,
                           const char *file, uint32_t line);
void kernel_debug_stage_fail_at(uint16_t phase, uint16_t step, int64_t status,
                                const char *file, uint32_t line);

/* Stable symbols for debugger expressions and older out-of-tree callers. */
void kernel_debug_stage(uint16_t phase, uint16_t step, uint32_t value);
void kernel_debug_stage_fail(uint16_t phase, uint16_t step, int64_t status);

#define kernel_debug_stage(phase, step, value) \
    kernel_debug_stage_at((phase), (step), (value), __FILE__, __LINE__)
#define kernel_debug_stage_fail(phase, step, status) \
    kernel_debug_stage_fail_at((phase), (step), (status), __FILE__, __LINE__)

/* Keep common phase transitions at the real caller's source location. */
#define kernel_debug_stage_enter(phase) \
    kernel_debug_stage_at((phase), OS_DEBUG_STEP_ENTER, 0U, \
                          __FILE__, __LINE__)
#define kernel_debug_stage_ready(phase) \
    kernel_debug_stage_at((phase), OS_DEBUG_STEP_READY, 1U, \
                          __FILE__, __LINE__)
#define kernel_debug_stage_pending(phase) \
    kernel_debug_stage_at((phase), OS_DEBUG_STEP_PENDING, 0U, \
                          __FILE__, __LINE__)

/* Runtime self-tests enable this narrow event stream around their user image. */
void debug_trace_set(bool enabled);
bool debug_trace_enabled(void);
void debug_trace_stage_at(uint16_t phase, uint16_t step, uint32_t value,
                                 const char *file, uint32_t line);
void debug_trace_stage(uint16_t phase, uint16_t step, uint32_t value);
#define debug_trace_stage(phase, step, value) \
    debug_trace_stage_at((phase), (step), (value), __FILE__, __LINE__)

uint64_t kernel_debug_stage_sequence(void);
uint16_t kernel_debug_stage_phase(void);
uint16_t kernel_debug_stage_step(void);
uint32_t kernel_debug_stage_value(void);
int64_t kernel_debug_stage_status(void);
const char *kernel_debug_stage_file(void);
uint32_t kernel_debug_stage_line(void);
