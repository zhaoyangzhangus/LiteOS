#include <arch/x86_64/cpu.h>
#include <kernel/spinlock.h>
#include <kernel/telemetry.h>

static struct {
    spinlock_t lock;
    atomic_uint init_state;
    telemetry_record_t records[TELEMETRY_RING_CAPACITY];
    uint64_t next_sequence;
} g_telemetry;

bool telemetry_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_telemetry.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_telemetry.lock.state, 0U);
        for (uint32_t i = 0; i < TELEMETRY_RING_CAPACITY; ++i) {
            g_telemetry.records[i] = (telemetry_record_t){0};
        }
        g_telemetry.next_sequence = 0U;
        atomic_store_explicit(&g_telemetry.init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_telemetry.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

uint64_t telemetry_timestamp(void) {
    return x86_read_tsc();
}

kstatus_t telemetry_record(uint32_t category, uint64_t object_id,
                           uint64_t value0, uint64_t value1) {
    telemetry_record_t *record;
    if (category == 0U || !telemetry_init()) return K_EINVAL;
    while (atomic_exchange_explicit(&g_telemetry.lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    if (g_telemetry.next_sequence == UINT64_MAX) {
        atomic_store_explicit(&g_telemetry.lock.state, 0U, memory_order_release);
        return K_EIO;
    }
    uint64_t sequence = ++g_telemetry.next_sequence;
    record = &g_telemetry.records[(size_t)((sequence - 1U) % TELEMETRY_RING_CAPACITY)];
    record->timestamp_tsc = x86_read_tsc();
    record->cpu = x86_current_cpu_index();
    record->category = category;
    record->object_id = object_id;
    record->value0 = value0;
    record->value1 = value1;
    __atomic_store_n(&record->sequence, sequence, __ATOMIC_RELEASE);
    atomic_store_explicit(&g_telemetry.lock.state, 0U, memory_order_release);
    return K_OK;
}

kstatus_t telemetry_record_latency(uint32_t category, uint64_t object_id,
                                   uint64_t start_tsc) {
    uint64_t now = telemetry_timestamp();
    uint64_t elapsed = now >= start_tsc ? now - start_tsc :
                       UINT64_MAX - start_tsc + now + 1U;
    uint64_t nanoseconds = x86_boot_cpu_features.tsc_hz != 0U ?
                           x86_tsc_to_ns(elapsed) : 0U;
    return telemetry_record(category, object_id, elapsed, nanoseconds);
}

size_t telemetry_snapshot(uint64_t after_sequence, telemetry_record_t *records,
                          size_t capacity, uint64_t *next_sequence) {
    size_t count = 0U;
    if (!telemetry_init() || records == 0 || capacity == 0U) return 0U;
    while (atomic_exchange_explicit(&g_telemetry.lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
    uint64_t newest = g_telemetry.next_sequence;
    if (after_sequence == UINT64_MAX) {
        if (next_sequence != 0) *next_sequence = newest;
        atomic_store_explicit(&g_telemetry.lock.state, 0U, memory_order_release);
        return 0U;
    }
    uint64_t first = newest >= TELEMETRY_RING_CAPACITY ?
                     newest - TELEMETRY_RING_CAPACITY + 1U : 1U;
    if (after_sequence + 1U > first) first = after_sequence + 1U;
    for (uint64_t sequence = first;
         sequence <= newest && count < capacity; ++sequence) {
        telemetry_record_t record = g_telemetry.records[
            (size_t)((sequence - 1U) % TELEMETRY_RING_CAPACITY)];
        if (__atomic_load_n(&record.sequence, __ATOMIC_ACQUIRE) != sequence) continue;
        records[count++] = record;
    }
    if (next_sequence != 0) *next_sequence = newest;
    atomic_store_explicit(&g_telemetry.lock.state, 0U, memory_order_release);
    return count;
}

bool telemetry_self_test(void) {
    telemetry_record_t record = {0};
    telemetry_record_t baseline = {0};
    uint64_t before = 0U;
    uint64_t next = 0U;
    uint64_t start_tsc;
    /* 自检可能在真实 I/O 已经产生遥测后执行，不能假设 ring 为空。 */
    (void)telemetry_snapshot(UINT64_MAX, &baseline, 1U, &before);
    if (telemetry_record(1U, 0x42U, 11U, 22U) != K_OK ||
        telemetry_snapshot(before, &record, 1U, &next) != 1U ||
        next == 0U || record.category != 1U || record.object_id != 0x42U ||
        record.value0 != 11U || record.value1 != 22U) return false;
    start_tsc = telemetry_timestamp();
    if (telemetry_record_latency(2U, 0x43U, start_tsc) != K_OK ||
        telemetry_snapshot(next, &record, 1U, 0) != 1U ||
        record.category != 2U || record.object_id != 0x43U) return false;
    return record.value1 == 0U || record.value1 != UINT64_MAX;
}
