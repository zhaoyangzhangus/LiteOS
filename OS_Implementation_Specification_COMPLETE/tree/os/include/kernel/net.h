#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "wait.h"
#include "refcount.h"

struct net_device;

typedef struct net_buffer {
    refcount_t refs;
    uint8_t *head;
    uint8_t *data;
    uint8_t *tail;
    uint8_t *end;
    uint32_t length;
    uint32_t protocol;
    struct net_device *device;
    uint32_t checksum_state;
    uint32_t flags;
    void *fragments;
} net_buffer_t;

typedef struct socket {
    object_header_t object;
    uint16_t family;
    uint16_t type;
    uint16_t protocol;
    uint16_t state;
    uint32_t flags;

    wait_queue_t waitq;
    spinlock_t lock;

    void *rx_queue;
    void *tx_queue;
    void *protocol_state;
} socket_t;
