#include <arch/x86_64/cpu.h>
#include <kernel/crash_dump.h>

static crash_dump_record_t g_crash_dumps[CRASH_DUMP_RECORD_COUNT];
static atomic_uint g_crash_dump_sequence;

void crash_dump_capture(const arch_trap_frame_t *frame, uint64_t cr2) {
    if (frame == 0) return;
    uint32_t sequence = atomic_fetch_add_explicit(&g_crash_dump_sequence, 1U,
                                                  memory_order_relaxed) + 1U;
    crash_dump_record_t *record = &g_crash_dumps[
        (sequence - 1U) % CRASH_DUMP_RECORD_COUNT];
    record->magic = CRASH_DUMP_MAGIC;
    record->version = CRASH_DUMP_VERSION;
    record->reserved = 0U;
    record->vector = (uint32_t)frame->vector;
    record->cpu_index = x86_current_cpu_index();
    record->error_code = frame->error_code;
    record->rip = frame->rip;
    record->rsp = frame->rsp;
    record->rflags = frame->rflags;
    record->cr2 = cr2;
    /* 序号最后发布，读者据此判断整条记录是否已经写完。 */
    __atomic_store_n(&record->sequence, sequence, __ATOMIC_RELEASE);
}

uint32_t crash_dump_count(void) {
    uint32_t count = atomic_load_explicit(&g_crash_dump_sequence,
                                          memory_order_acquire);
    return count > CRASH_DUMP_RECORD_COUNT ? CRASH_DUMP_RECORD_COUNT : count;
}

bool crash_dump_read_latest(crash_dump_record_t *out) {
    if (out == 0) return false;
    uint32_t sequence = atomic_load_explicit(&g_crash_dump_sequence,
                                             memory_order_acquire);
    if (sequence == 0U) return false;
    crash_dump_record_t *record = &g_crash_dumps[
        (sequence - 1U) % CRASH_DUMP_RECORD_COUNT];
    uint32_t published = __atomic_load_n(&record->sequence, __ATOMIC_ACQUIRE);
    if (published != sequence) return false;
    *out = *record;
    return __atomic_load_n(&record->sequence, __ATOMIC_ACQUIRE) == sequence;
}

bool crash_dump_self_test(void) {
    arch_trap_frame_t frame = {0};
    crash_dump_record_t record = {0};
    frame.vector = 13U;
    frame.error_code = 0x55AAU;
    frame.rip = 0x0000000040001234ULL;
    frame.rsp = 0x000000007FFF0000ULL;
    frame.rflags = 0x202ULL;
    crash_dump_capture(&frame, 0x00000000DEADBEEFULL);
    return crash_dump_read_latest(&record) && record.magic == CRASH_DUMP_MAGIC &&
           record.version == CRASH_DUMP_VERSION && record.vector == 13U &&
           record.error_code == 0x55AAU && record.rip == frame.rip &&
           record.rsp == frame.rsp && record.cr2 == 0x00000000DEADBEEFULL;
}
