#include "ipc.h"

static VOID memory_copy(UINT8 *destination, const UINT8 *source, UINT32 size) {
    for (UINT32 i = 0; i < size; ++i) destination[i] = source[i];
}

BOOLEAN liteos_message_port_init(LITEOS_MESSAGE_PORT *port) {
    if (port == 0) return 0;
    port->Head = 0;
    port->Tail = 0;
    port->Count = 0;
    port->Closed = 0;
    return 1;
}

BOOLEAN liteos_message_port_send(LITEOS_MESSAGE_PORT *port,
                                 const VOID *data, UINT32 size) {
    if (port == 0 || data == 0 || size == 0 || size > LITEOS_IPC_MESSAGE_SIZE ||
        port->Closed || port->Count >= LITEOS_IPC_MESSAGE_COUNT) return 0;
    LITEOS_IPC_MESSAGE *message = &port->Messages[port->Tail];
    memory_copy(message->Data, (const UINT8 *)data, size);
    message->Size = size;
    port->Tail = (port->Tail + 1U) % LITEOS_IPC_MESSAGE_COUNT;
    ++port->Count;
    return 1;
}

BOOLEAN liteos_message_port_receive(LITEOS_MESSAGE_PORT *port,
                                    VOID *data, UINT32 capacity, UINT32 *size) {
    if (port == 0 || data == 0 || size == 0 || capacity == 0 || port->Count == 0) return 0;
    LITEOS_IPC_MESSAGE *message = &port->Messages[port->Head];
    if (capacity < message->Size) return 0;
    memory_copy((UINT8 *)data, message->Data, message->Size);
    *size = message->Size;
    port->Head = (port->Head + 1U) % LITEOS_IPC_MESSAGE_COUNT;
    --port->Count;
    return 1;
}

BOOLEAN liteos_message_port_close(LITEOS_MESSAGE_PORT *port) {
    if (port == 0 || port->Closed) return 0;
    port->Closed = 1;
    return 1;
}

BOOLEAN liteos_event_init(LITEOS_EVENT *event, BOOLEAN signaled) {
    if (event == 0) return 0;
    __atomic_store_n(&event->Signaled, signaled ? 1U : 0U, __ATOMIC_RELEASE);
    return 1;
}

BOOLEAN liteos_event_set(LITEOS_EVENT *event) {
    if (event == 0) return 0;
    __atomic_store_n(&event->Signaled, 1U, __ATOMIC_RELEASE);
    return 1;
}

BOOLEAN liteos_event_reset(LITEOS_EVENT *event) {
    if (event == 0) return 0;
    __atomic_store_n(&event->Signaled, 0U, __ATOMIC_RELEASE);
    return 1;
}

BOOLEAN liteos_event_try_wait(LITEOS_EVENT *event) {
    if (event == 0) return 0;
    return __atomic_load_n(&event->Signaled, __ATOMIC_ACQUIRE) != 0;
}

BOOLEAN liteos_semaphore_init(LITEOS_SEMAPHORE *semaphore,
                              UINT32 initial_count, UINT32 maximum_count) {
    if (semaphore == 0 || maximum_count == 0 || initial_count > maximum_count) return 0;
    semaphore->Maximum = maximum_count;
    __atomic_store_n(&semaphore->Count, initial_count, __ATOMIC_RELEASE);
    return 1;
}

BOOLEAN liteos_semaphore_release(LITEOS_SEMAPHORE *semaphore, UINT32 count) {
    if (semaphore == 0 || count == 0) return 0;
    UINT32 old_count = __atomic_load_n(&semaphore->Count, __ATOMIC_RELAXED);
    for (;;) {
        if (old_count > semaphore->Maximum || count > semaphore->Maximum - old_count) return 0;
        if (__atomic_compare_exchange_n(&semaphore->Count, &old_count, old_count + count,
                                        0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return 1;
    }
}

BOOLEAN liteos_semaphore_try_wait(LITEOS_SEMAPHORE *semaphore) {
    if (semaphore == 0) return 0;
    UINT32 old_count = __atomic_load_n(&semaphore->Count, __ATOMIC_RELAXED);
    while (old_count != 0) {
        if (__atomic_compare_exchange_n(&semaphore->Count, &old_count, old_count - 1U,
                                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    }
    return 0;
}

BOOLEAN liteos_mutex_init(LITEOS_MUTEX *mutex) {
    if (mutex == 0) return 0;
    __atomic_store_n(&mutex->Locked, 0U, __ATOMIC_RELEASE);
    return 1;
}

BOOLEAN liteos_mutex_try_lock(LITEOS_MUTEX *mutex) {
    if (mutex == 0) return 0;
    UINT8 expected = 0;
    return __atomic_compare_exchange_n(&mutex->Locked, &expected, 1U, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

BOOLEAN liteos_mutex_unlock(LITEOS_MUTEX *mutex) {
    if (mutex == 0 || __atomic_load_n(&mutex->Locked, __ATOMIC_RELAXED) == 0) return 0;
    __atomic_store_n(&mutex->Locked, 0U, __ATOMIC_RELEASE);
    return 1;
}
