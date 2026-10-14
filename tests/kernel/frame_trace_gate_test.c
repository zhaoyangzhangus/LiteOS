#include <kernel/frame_trace.h>

static unsigned int g_scope_calls;
static unsigned int g_value_calls;
static unsigned int g_argument_evaluations;

void kernel_perf_emit_scope_at(const char *name, uint64_t start_tsc,
                              const char *file, uint32_t line) {
    (void)name;
    (void)start_tsc;
    (void)file;
    (void)line;
    ++g_scope_calls;
}

void kernel_perf_emit_value_at(const char *name, uint64_t value,
                              const char *file, uint32_t line) {
    (void)name;
    (void)value;
    (void)file;
    (void)line;
    ++g_value_calls;
}

const char *trace_name(void) {
    ++g_argument_evaluations;
    return "frame_trace_test";
}

uint64_t trace_value(void) {
    ++g_argument_evaluations;
    return 17U;
}

int main(void) {
    kernel_frame_trace_emit_scope(trace_name(), trace_value());
    kernel_frame_trace_emit_value(trace_name(), trace_value());

#if OS_ENABLE_FRAME_TRACE
    return g_scope_calls == 1U && g_value_calls == 1U &&
                   g_argument_evaluations == 4U ? 0 : 1;
#else
    return g_scope_calls == 0U && g_value_calls == 0U &&
                   g_argument_evaluations == 0U ? 0 : 1;
#endif
}
