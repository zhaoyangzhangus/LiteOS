#include <kernel/debug_stage.h>
#include <kernel/console.h>
#include <kernel/realtest.h>

#ifndef LITEOS_DEBUG_SERIAL
#define LITEOS_DEBUG_SERIAL 0
#endif
#ifndef LITEOS_REALTEST
#define LITEOS_REALTEST 0
#endif

static atomic_uint_fast64_t g_stage_sequence;
static atomic_uint g_stage_phase;
static atomic_uint g_stage_step;
static atomic_uint g_stage_value;
static atomic_int_fast64_t g_stage_status;
static atomic_uintptr_t g_stage_file;
static atomic_uint g_stage_line;
static atomic_bool g_trace_enabled;

#if LITEOS_DEBUG_SERIAL || LITEOS_REALTEST
#define STAGE_RECORD_CAPACITY 768U

static void stage_append_char(char *output, uint32_t *length, char value) {
    if (*length + 1U >= STAGE_RECORD_CAPACITY) return;
    output[(*length)++] = value;
    output[*length] = 0;
}

static void stage_append_text(char *output, uint32_t *length,
                              const char *text) {
    if (text == 0) return;
    while (*text != 0) stage_append_char(output, length, *text++);
}

static void stage_append_u32(char *output, uint32_t *length, uint32_t value) {
    char digits[11];
    uint32_t index = sizeof(digits) - 1U;
    digits[index] = 0;
    do {
        digits[--index] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    stage_append_text(output, length, &digits[index]);
}

static void stage_append_i64(char *output, uint32_t *length, int64_t value) {
    if (value < 0) {
        stage_append_char(output, length, '-');
        if (value == INT64_MIN) {
            stage_append_text(output, length, "9223372036854775808");
        } else {
            uint64_t magnitude = (uint64_t)(-value);
            if (magnitude <= UINT32_MAX) {
                stage_append_u32(output, length, (uint32_t)magnitude);
            } else {
                stage_append_text(output, length, "large");
            }
        }
        return;
    }
    if ((uint64_t)value <= UINT32_MAX) {
        stage_append_u32(output, length, (uint32_t)value);
    } else {
        stage_append_text(output, length, "large");
    }
}

static const char *stage_phase_name(uint16_t phase) {
    switch (phase) {
        case LITEOS_DEBUG_PHASE_BOOT: return "BOOT";
        case LITEOS_DEBUG_PHASE_CPU: return "CPU";
        case LITEOS_DEBUG_PHASE_MEMORY: return "MEMORY";
        case LITEOS_DEBUG_PHASE_SCHEDULER: return "SCHED";
        case LITEOS_DEBUG_PHASE_DRIVER: return "DRIVER";
        case LITEOS_DEBUG_PHASE_STORAGE: return "STORAGE";
        case LITEOS_DEBUG_PHASE_NETWORK: return "NETWORK";
        case LITEOS_DEBUG_PHASE_DISPLAY: return "DISPLAY";
        case LITEOS_DEBUG_PHASE_USER: return "USER";
        case LITEOS_DEBUG_PHASE_USER_RUNTIME: return "USER_RUNTIME";
        case LITEOS_DEBUG_PHASE_DESKTOP: return "DESKTOP";
        case LITEOS_DEBUG_PHASE_SPEC_0: return "SPEC_P0_REPOSITORY";
        case LITEOS_DEBUG_PHASE_SPEC_1: return "SPEC_P1_CPU";
        case LITEOS_DEBUG_PHASE_SPEC_2: return "SPEC_P2_MEMORY";
        case LITEOS_DEBUG_PHASE_SPEC_3: return "SPEC_P3_SLAB";
        case LITEOS_DEBUG_PHASE_SPEC_4: return "SPEC_P4_SMP";
        case LITEOS_DEBUG_PHASE_SPEC_5: return "SPEC_P5_SCHEDULER";
        case LITEOS_DEBUG_PHASE_SPEC_6: return "SPEC_P6_VM";
        case LITEOS_DEBUG_PHASE_SPEC_7: return "SPEC_P7_SYSCALL";
        case LITEOS_DEBUG_PHASE_SPEC_8: return "SPEC_P8_OBJECT";
        case LITEOS_DEBUG_PHASE_SPEC_9: return "SPEC_P9_DEVICE";
        case LITEOS_DEBUG_PHASE_SPEC_10: return "SPEC_P10_STORAGE";
        case LITEOS_DEBUG_PHASE_SPEC_11: return "SPEC_P11_USB";
        case LITEOS_DEBUG_PHASE_SPEC_12: return "SPEC_P12_NETWORK";
        case LITEOS_DEBUG_PHASE_SPEC_13: return "SPEC_P13_GPU";
        case LITEOS_DEBUG_PHASE_SPEC_14: return "SPEC_P14_WINDOW";
        case LITEOS_DEBUG_PHASE_SPEC_15: return "SPEC_P15_AUDIO_BT";
        case LITEOS_DEBUG_PHASE_SPEC_16: return "SPEC_P16_SERVICES";
        case LITEOS_DEBUG_PHASE_SPEC_17: return "SPEC_P17_POWER";
        case LITEOS_DEBUG_PHASE_SPEC_18: return "SPEC_P18_HARDENING";
        case LITEOS_DEBUG_PHASE_SPEC_19: return "SPEC_P19_ABI";
        case LITEOS_DEBUG_PHASE_SPEC_20: return "SPEC_P20_OS1";
        case LITEOS_DEBUG_PHASE_REFACTOR_0: return "REFACTOR_P0_BASELINE";
        case LITEOS_DEBUG_PHASE_REFACTOR_1: return "REFACTOR_P1_HEADERS";
        case LITEOS_DEBUG_PHASE_REFACTOR_2: return "REFACTOR_P2_PRIMITIVES";
        case LITEOS_DEBUG_PHASE_REFACTOR_3: return "REFACTOR_P3_BOOT";
        case LITEOS_DEBUG_PHASE_REFACTOR_4: return "REFACTOR_P4_SCHEDULER";
        case LITEOS_DEBUG_PHASE_REFACTOR_5: return "REFACTOR_P5_PROCESS";
        case LITEOS_DEBUG_PHASE_REFACTOR_6: return "REFACTOR_P6_MM";
        case LITEOS_DEBUG_PHASE_REFACTOR_7A: return "REFACTOR_P7A_GRAPHICS";
        case LITEOS_DEBUG_PHASE_REFACTOR_7B: return "REFACTOR_P7B_COMPOSITOR";
        case LITEOS_DEBUG_PHASE_REFACTOR_8: return "REFACTOR_P8_DRIVERS";
        case LITEOS_DEBUG_PHASE_REFACTOR_9: return "REFACTOR_P9_CLEANUP";
        default: return "UNKNOWN";
    }
}

static void stage_emit(uint16_t phase, uint16_t step, uint32_t value,
                       bool failed, int64_t status, const char *file,
                       uint32_t line) {
    char output[STAGE_RECORD_CAPACITY];
    uint32_t length = 0U;
    liteos_serial_guard_t serial_guard;

    /* The sequence and the complete record share the console COM1
     * transaction.  This preserves strict sequence order while ordinary
     * printf diagnostics are active on another CPU. */
    liteos_serial_guard_enter(&serial_guard);
    /* Allocate the serial sequence while holding the output lock.  A stage
     * event can be published concurrently by two CPUs; assigning the number
     * before this lock would make the textual log appear out of order. */
    uint64_t sequence = atomic_fetch_add_explicit(&g_stage_sequence, 1U,
                                                  memory_order_release) + 1U;
    output[0] = 0;
    stage_append_text(output, &length,
                      failed ? "LITEOS_STAGE_FAIL phase=" :
                               "LITEOS_STAGE phase=");
    stage_append_text(output, &length, stage_phase_name(phase));
    stage_append_text(output, &length, " step=");
    stage_append_u32(output, &length, step);
    if (failed) {
        stage_append_text(output, &length, " status=");
        stage_append_i64(output, &length, status);
    } else {
        stage_append_text(output, &length, " value=");
        stage_append_u32(output, &length, value);
    }
    stage_append_text(output, &length, " seq=");
    if (sequence <= UINT32_MAX) {
        stage_append_u32(output, &length, (uint32_t)sequence);
    } else {
        stage_append_text(output, &length, "large");
    }
    stage_append_text(output, &length, " loc=");
    stage_append_text(output, &length, file != 0 ? file : "<unknown>");
    stage_append_char(output, &length, ':');
    stage_append_u32(output, &length, line);
    stage_append_text(output, &length, "\r\n");
#if LITEOS_REALTEST
    liteos_realtest_capture(output);
#endif
#if LITEOS_DEBUG_SERIAL
    liteos_serial_write_record_guarded(output);
#endif
    liteos_serial_guard_leave(&serial_guard);
}
#endif

static void stage_publish(uint16_t phase, uint16_t step, uint32_t value,
                          int64_t status, bool failed, const char *file,
                          uint32_t line) {
    atomic_store_explicit(&g_stage_phase, phase, memory_order_relaxed);
    atomic_store_explicit(&g_stage_step, step, memory_order_relaxed);
    atomic_store_explicit(&g_stage_value, value, memory_order_relaxed);
    atomic_store_explicit(&g_stage_status, status, memory_order_relaxed);
    atomic_store_explicit(&g_stage_file, (uintptr_t)file, memory_order_relaxed);
    atomic_store_explicit(&g_stage_line, line, memory_order_relaxed);
#if LITEOS_DEBUG_SERIAL || LITEOS_REALTEST
    stage_emit(phase, step, value, failed, status, file, line);
#else
    (void)atomic_fetch_add_explicit(&g_stage_sequence, 1U,
                                    memory_order_release);
    (void)failed;
#endif
}

/* The header maps normal calls to the location-aware entry point. */
#undef liteos_debug_stage
#undef liteos_debug_stage_fail
#undef liteos_debug_trace_stage

void liteos_debug_stage_at(uint16_t phase, uint16_t step, uint32_t value,
                           const char *file, uint32_t line) {
    stage_publish(phase, step, value, 0, false, file, line);
}

void liteos_debug_stage_fail_at(uint16_t phase, uint16_t step, int64_t status,
                                const char *file, uint32_t line) {
    stage_publish(phase, step, 0, status, true, file, line);
}

void liteos_debug_stage(uint16_t phase, uint16_t step, uint32_t value) {
    liteos_debug_stage_at(phase, step, value, "<unknown>", 0U);
}

void liteos_debug_stage_fail(uint16_t phase, uint16_t step, int64_t status) {
    liteos_debug_stage_fail_at(phase, step, status, "<unknown>", 0U);
}

void liteos_debug_trace_set(bool enabled) {
    atomic_store_explicit(&g_trace_enabled, enabled, memory_order_release);
}

bool liteos_debug_trace_enabled(void) {
    return atomic_load_explicit(&g_trace_enabled, memory_order_acquire);
}

void liteos_debug_trace_stage_at(uint16_t phase, uint16_t step, uint32_t value,
                                 const char *file, uint32_t line) {
    if (liteos_debug_trace_enabled()) {
        liteos_debug_stage_at(phase, step, value, file, line);
    }
}

void liteos_debug_trace_stage(uint16_t phase, uint16_t step, uint32_t value) {
    liteos_debug_trace_stage_at(phase, step, value, "<unknown>", 0U);
}

uint64_t liteos_debug_stage_sequence(void) {
    return atomic_load_explicit(&g_stage_sequence, memory_order_acquire);
}

uint16_t liteos_debug_stage_phase(void) {
    return (uint16_t)atomic_load_explicit(&g_stage_phase, memory_order_acquire);
}

uint16_t liteos_debug_stage_step(void) {
    return (uint16_t)atomic_load_explicit(&g_stage_step, memory_order_acquire);
}

uint32_t liteos_debug_stage_value(void) {
    return atomic_load_explicit(&g_stage_value, memory_order_acquire);
}

int64_t liteos_debug_stage_status(void) {
    return atomic_load_explicit(&g_stage_status, memory_order_acquire);
}

const char *liteos_debug_stage_file(void) {
    return (const char *)(uintptr_t)atomic_load_explicit(&g_stage_file,
                                                          memory_order_acquire);
}

uint32_t liteos_debug_stage_line(void) {
    return atomic_load_explicit(&g_stage_line, memory_order_acquire);
}
