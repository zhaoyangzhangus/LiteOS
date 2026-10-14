#pragma once
#include "nv_port.h"

/* OS-owned notification state. Vendor payload translation stays above this
 * boundary. Callers retain the queue owner throughout every operation. */
typedef struct nv_port_event_record {
    uint32_t parent, object, index, info32;
    uint16_t info16;
} nv_port_event_record_t;
struct nv_port_event_node;
typedef struct nv_port_event_queue {
    nv_port_spinlock_t lock;
    struct nv_port_event_node *head, *tail;
    bool dataless_pending, closed;
} nv_port_event_queue_t;

#define NV_PORT_EVENT_READ 1U
#define NV_PORT_EVENT_CLOSED 2U
void nv_port_event_queue_init(nv_port_event_queue_t *queue);
kstatus_t nv_port_event_post(nv_port_event_queue_t *queue,
                             const nv_port_event_record_t *record);
/* IRQ route uses only existing slab capacity and may fail on contention.
 * Dataless posts still require no allocation. The owner remains pinned. */
kstatus_t nv_port_event_post_irq(nv_port_event_queue_t *queue,
                                 const nv_port_event_record_t *record);
kstatus_t nv_port_event_take(nv_port_event_queue_t *queue,
                             nv_port_event_record_t *record, bool *more);
uint32_t nv_port_event_poll(nv_port_event_queue_t *queue, bool consume_dataless);
bool nv_port_event_queue_close(nv_port_event_queue_t *queue);

/* Refcounted, independently pinnable wait source for poll/epoll. The creator
 * owns one object reference. Close logical state before dropping that reference;
 * waiters may retain the closed source until they have unsubscribed. */
typedef struct nv_port_event_source nv_port_event_source_t;
nv_port_event_source_t *nv_port_event_source_create(void);
void *nv_port_event_source_wait_object(nv_port_event_source_t *source);
kstatus_t nv_port_event_source_post(nv_port_event_source_t *source,
                                    const nv_port_event_record_t *record);
kstatus_t nv_port_event_source_post_irq(nv_port_event_source_t *source,
                                        const nv_port_event_record_t *record);
kstatus_t nv_port_event_source_take(nv_port_event_source_t *source,
                                    nv_port_event_record_t *record, bool *more);
uint32_t nv_port_event_source_poll(nv_port_event_source_t *source,
                                   bool consume_dataless);
void nv_port_event_source_close(nv_port_event_source_t *source);
