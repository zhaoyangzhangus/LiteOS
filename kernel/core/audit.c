#include <arch/x86_64/cpu.h>
#include <kernel/audit.h>
#include <kernel/spinlock.h>

static spinlock_t g_audit_lock;
static atomic_uint g_audit_init_state;
static audit_record_t g_audit_records[AUDIT_RING_CAPACITY];
static uint64_t g_audit_next_sequence;

static void audit_lock(void) {
    while (atomic_exchange_explicit(&g_audit_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void audit_unlock(void) {
    atomic_store_explicit(&g_audit_lock.state, 0U, memory_order_release);
}

bool audit_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_audit_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_audit_lock.state, 0U);
        for (uint32_t i = 0; i < AUDIT_RING_CAPACITY; ++i) {
            g_audit_records[i] = (audit_record_t){0};
        }
        g_audit_next_sequence = 0U;
        atomic_store_explicit(&g_audit_init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_audit_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

kstatus_t audit_emit(uint32_t event_type, uint64_t object_id,
                     uint32_t uid, uint32_t gid, uint32_t requested,
                     kstatus_t result) {
    if (event_type == 0U || !audit_init()) return K_EINVAL;
    audit_lock();
    uint64_t sequence = ++g_audit_next_sequence;
    audit_record_t *record = &g_audit_records[
        (size_t)(sequence % AUDIT_RING_CAPACITY)];
    record->timestamp_tsc = x86_read_tsc();
    record->object_id = object_id;
    record->event_type = event_type;
    record->cpu = x86_current_cpu_index();
    record->uid = uid;
    record->gid = gid;
    record->requested = requested;
    record->result = (int32_t)result;
    record->reserved = 0U;
    atomic_thread_fence(memory_order_release);
    record->sequence = sequence;
    audit_unlock();
    return K_OK;
}

size_t audit_snapshot(uint64_t after_sequence, audit_record_t *records,
                      size_t capacity, uint64_t *next_sequence) {
    if (!audit_init() || records == 0 || capacity == 0U) return 0U;
    audit_lock();
    uint64_t newest = g_audit_next_sequence;
    uint64_t first = newest >= AUDIT_RING_CAPACITY ?
                     newest - AUDIT_RING_CAPACITY + 1U : 1U;
    if (after_sequence + 1U > first) first = after_sequence + 1U;
    size_t count = 0U;
    for (uint64_t sequence = first;
         sequence <= newest && count < capacity; ++sequence) {
        audit_record_t record = g_audit_records[
            (size_t)(sequence % AUDIT_RING_CAPACITY)];
        if (record.sequence != sequence) continue;
        records[count++] = record;
    }
    if (next_sequence != 0) *next_sequence = newest;
    audit_unlock();
    return count;
}

bool audit_self_test(void) {
    audit_record_t record;
    uint64_t before = 0U;
    uint64_t after = 0U;
    (void)audit_snapshot(0U, &record, 1U, &before);
    if (audit_emit(AUDIT_EVENT_SERVICE, 0xA11DULL, 0U, 0U, 1U, K_OK) != K_OK) {
        return false;
    }
    if (audit_snapshot(before, &record, 1U, &after) != 1U ||
        after <= before || record.sequence != after ||
        record.event_type != AUDIT_EVENT_SERVICE || record.object_id != 0xA11DULL ||
        record.result != K_OK) return false;
    return true;
}
