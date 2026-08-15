#ifndef LITEOS_IPC_H
#define LITEOS_IPC_H

#include "uefi.h"

#define LITEOS_IPC_MESSAGE_SIZE  64U
#define LITEOS_IPC_MESSAGE_COUNT 32U

typedef struct {
    UINT8 Data[LITEOS_IPC_MESSAGE_SIZE];
    UINT32 Size;
} LITEOS_IPC_MESSAGE;

typedef struct {
    LITEOS_IPC_MESSAGE Messages[LITEOS_IPC_MESSAGE_COUNT];
    UINT32 Head;
    UINT32 Tail;
    UINT32 Count;
    BOOLEAN Closed;
} LITEOS_MESSAGE_PORT;

typedef struct {
    volatile UINT32 Signaled;
} LITEOS_EVENT;

typedef struct {
    volatile UINT32 Count;
    UINT32 Maximum;
} LITEOS_SEMAPHORE;

typedef struct {
    volatile UINT8 Locked;
} LITEOS_MUTEX;

BOOLEAN liteos_message_port_init(LITEOS_MESSAGE_PORT *port);
BOOLEAN liteos_message_port_send(LITEOS_MESSAGE_PORT *port,
                                 const VOID *data, UINT32 size);
BOOLEAN liteos_message_port_receive(LITEOS_MESSAGE_PORT *port,
                                    VOID *data, UINT32 capacity, UINT32 *size);
BOOLEAN liteos_message_port_close(LITEOS_MESSAGE_PORT *port);

BOOLEAN liteos_event_init(LITEOS_EVENT *event, BOOLEAN signaled);
BOOLEAN liteos_event_set(LITEOS_EVENT *event);
BOOLEAN liteos_event_reset(LITEOS_EVENT *event);
BOOLEAN liteos_event_try_wait(LITEOS_EVENT *event);

BOOLEAN liteos_semaphore_init(LITEOS_SEMAPHORE *semaphore,
                              UINT32 initial_count, UINT32 maximum_count);
BOOLEAN liteos_semaphore_release(LITEOS_SEMAPHORE *semaphore, UINT32 count);
BOOLEAN liteos_semaphore_try_wait(LITEOS_SEMAPHORE *semaphore);

BOOLEAN liteos_mutex_init(LITEOS_MUTEX *mutex);
BOOLEAN liteos_mutex_try_lock(LITEOS_MUTEX *mutex);
BOOLEAN liteos_mutex_unlock(LITEOS_MUTEX *mutex);

#endif
