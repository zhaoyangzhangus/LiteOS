#include <kernel/kmem.h>
#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>
#include <kernel/completion_port.h>
#include <kernel/deferred.h>
#include <kernel/sched.h>
#include <kernel/wait.h>

#define SOCKET_BINDING_COUNT 64U
#define SOCKET_EPHEMERAL_FIRST 49152U
#define SOCKET_EPHEMERAL_LAST  65535U
#define SOCKET_LONG_TEST_CHUNK 1024U
#define SOCKET_LONG_TEST_BATCH 8U
#define SOCKET_LONG_TEST_ROUNDS 8U

typedef struct socket_datagram {
    uint16_t family;
    uint16_t reserved;
    uint32_t source_address;
    uint16_t source_port;
    uint32_t destination_address;
    uint16_t destination_port;
    uint16_t length;
    uint8_t source_address6[16];
    uint8_t destination_address6[16];
    uint8_t payload[SOCKET_MAX_PAYLOAD];
} socket_datagram_t;

typedef struct socket_private {
    socket_datagram_t datagrams[SOCKET_RX_QUEUE_DEPTH];
    uint32_t datagram_read;
    uint32_t datagram_write;
    atomic_uint datagram_count;

    uint8_t stream_buffer[SOCKET_STREAM_BUFFER_SIZE];
    uint32_t stream_read;
    uint32_t stream_write;
    atomic_uint stream_count;

    atomic_bool closed;
    atomic_bool peer_closed;
    bool bound;
    bool connected;
    bool listening;
    uint32_t local_address;
    uint8_t local_address6[16];
    uint16_t local_port;
    uint32_t peer_address;
    uint8_t peer_address6[16];
    uint16_t peer_port;
    uint32_t tcp_next_sequence;
    bool tcp_sequence_valid;
    uint32_t tcp_initial_sequence;
    uint32_t tcp_peer_initial_sequence;
    uint8_t tcp_state;
    uint32_t tcp_send_next_sequence;
    uint32_t tcp_send_unacknowledged;
    uint16_t tcp_peer_window;
    uint16_t tcp_send_length;
    uint32_t tcp_send_retransmits;
    uint64_t tcp_send_deadline_tsc;
    bool tcp_send_pending;
    bool tcp_send_failed;
    uint8_t tcp_send_flags;
    uint8_t tcp_send_payload[SOCKET_MAX_PAYLOAD];
    socket_t *peer;
    socket_t *listener;

    socket_t *accept_queue[SOCKET_LISTEN_BACKLOG];
    uint32_t accept_read;
    uint32_t accept_write;
    atomic_uint accept_count;
} socket_private_t;

typedef struct socket_binding {
    socket_t *socket;
    uint16_t family;
    uint16_t reserved;
    uint32_t address;
    uint8_t address6[16];
    uint16_t port;
    uint16_t type;
} socket_binding_t;

/* 已建立 TCP 连接的四元组索引；监听端口和 accepted socket 可以共存。 */
typedef struct socket_tcp_connection {
    socket_t *socket;
    uint32_t local_address;
    uint16_t local_port;
    uint32_t peer_address;
    uint16_t peer_port;
} socket_tcp_connection_t;

typedef struct socket_tcp6_connection {
    socket_t *socket;
    uint8_t local_address[16];
    uint16_t local_port;
    uint8_t peer_address[16];
    uint16_t peer_port;
} socket_tcp6_connection_t;

enum {
    SOCKET_TCP_CLOSED = 0U,
    SOCKET_TCP_SYN_RECEIVED = 1U,
    SOCKET_TCP_ESTABLISHED = 2U,
    SOCKET_TCP_SYN_SENT = 3U,
};

typedef struct socket_wait_context {
    socket_t *socket;
    socket_private_t *private;
} socket_wait_context_t;

/* 异步发送任务复制负载并持有两端对象引用，关闭 socket 不会释放任务依赖。 */
typedef struct socket_async_send {
    socket_t *socket;
    completion_port_t *completion_port;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    size_t length;
    uint32_t address;
    uint8_t address6[16];
    bool ipv6;
    uint16_t port;
    uint64_t user_key;
    uint64_t request_id;
} socket_async_send_t;

static socket_binding_t g_bindings[SOCKET_BINDING_COUNT];
static socket_tcp_connection_t g_tcp_connections[SOCKET_BINDING_COUNT];
static socket_tcp6_connection_t g_tcp6_connections[SOCKET_BINDING_COUNT];
static spinlock_t g_binding_lock;
static atomic_uint g_socket_init_state;
static atomic_uint g_next_ephemeral_port;
static atomic_uint_fast64_t g_next_async_request_id;
static socket_tcp_ipv4_output_fn g_tcp_ipv4_output;
static void *g_tcp_ipv4_output_context;
static socket_tcp_ipv6_output_fn g_tcp_ipv6_output;
static void *g_tcp_ipv6_output_context;
static socket_udp_ipv4_output_fn g_udp_ipv4_output;
static void *g_udp_ipv4_output_context;
static socket_udp_ipv6_output_fn g_udp_ipv6_output;
static void *g_udp_ipv6_output_context;

static socket_private_t *socket_private(socket_t *socket);

static void socket_zero(void *memory, size_t length) {
    uint8_t *bytes = (uint8_t *)memory;
    while (length-- != 0) *bytes++ = 0;
}

static void socket_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0) *out++ = *in++;
}

static bool socket_address6_equal(const uint8_t left[16], const uint8_t right[16]) {
    uint8_t difference = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) difference |= left[index] ^ right[index];
    return difference == 0U;
}

static bool socket_address6_zero(const uint8_t address[16]) {
    uint8_t value = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) value |= address[index];
    return value == 0U;
}

static void socket_lock(spinlock_t *lock) {
    /*
     * LiteOS is preemptive.  A thread that owns a spin lock must not be
     * scheduled out on this CPU while another local thread spins for it.
     * The preempt-disable state is a nesting counter, so nested socket
     * locks remain valid.
     */
    sched_preempt_disable();
    while (atomic_exchange_explicit(&lock->state, 1U, memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void socket_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
    sched_preempt_enable();
}

static void socket_initialize_globals(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_socket_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        socket_zero(g_bindings, sizeof(g_bindings));
        socket_zero(g_tcp_connections, sizeof(g_tcp_connections));
        socket_zero(g_tcp6_connections, sizeof(g_tcp6_connections));
        atomic_init(&g_binding_lock.state, 0U);
        atomic_init(&g_next_ephemeral_port, SOCKET_EPHEMERAL_FIRST);
        atomic_store_explicit(&g_socket_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_socket_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

void socket_set_tcp_ipv4_output(socket_tcp_ipv4_output_fn output, void *context) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    g_tcp_ipv4_output = output;
    g_tcp_ipv4_output_context = context;
    socket_unlock(&g_binding_lock);
}

void socket_set_tcp_ipv6_output(socket_tcp_ipv6_output_fn output, void *context) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    g_tcp_ipv6_output = output;
    g_tcp_ipv6_output_context = context;
    socket_unlock(&g_binding_lock);
}

void socket_set_udp_ipv4_output(socket_udp_ipv4_output_fn output, void *context) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    g_udp_ipv4_output = output;
    g_udp_ipv4_output_context = context;
    socket_unlock(&g_binding_lock);
}

void socket_set_udp_ipv6_output(socket_udp_ipv6_output_fn output, void *context) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    g_udp_ipv6_output = output;
    g_udp_ipv6_output_context = context;
    socket_unlock(&g_binding_lock);
}

static void socket_udp4_output_get(socket_udp_ipv4_output_fn *output,
                                   void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_udp_ipv4_output;
    if (context != 0) *context = g_udp_ipv4_output_context;
    socket_unlock(&g_binding_lock);
}

static void socket_udp6_output_get(socket_udp_ipv6_output_fn *output,
                                   void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_udp_ipv6_output;
    if (context != 0) *context = g_udp_ipv6_output_context;
    socket_unlock(&g_binding_lock);
}

static uint64_t socket_tcp_retransmit_deadline(uint64_t now_tsc) {
    uint64_t timeout = x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS);
    return timeout > UINT64_MAX - now_tsc ? UINT64_MAX : now_tsc + timeout;
}

static void socket_tcp_output_get(socket_tcp_ipv4_output_fn *output,
                                  void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_tcp_ipv4_output;
    if (context != 0) *context = g_tcp_ipv4_output_context;
    socket_unlock(&g_binding_lock);
}

static void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *output,
                                   void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_tcp_ipv6_output;
    if (context != 0) *context = g_tcp_ipv6_output_context;
    socket_unlock(&g_binding_lock);
}

void socket_tcp_poll(uint64_t now_tsc) {
    socket_tcp_ipv4_output_fn output4 = 0;
    socket_tcp_ipv6_output_fn output6 = 0;
    void *output4_context = 0;
    void *output6_context = 0;
    socket_initialize_globals();
    socket_tcp_output_get(&output4, &output4_context);
    socket_tcp6_output_get(&output6, &output6_context);

    for (uint32_t index = 0U; index < SOCKET_BINDING_COUNT; ++index) {
        socket_t *target = 0;
        socket_private_t *private;
        uint8_t payload[SOCKET_MAX_PAYLOAD];
        uint16_t payload_length = 0U;
        uint8_t flags = 0U;
        uint32_t source_address = 0U;
        uint16_t source_port = 0U;
        uint32_t destination_address = 0U;
        uint16_t destination_port = 0U;
        uint32_t sequence = 0U;
        uint32_t acknowledgement = 0U;
        uint16_t window = 0U;
        bool retransmit = false;
        bool failed = false;

        socket_lock(&g_binding_lock);
        if (g_tcp_connections[index].socket != 0) {
            target = g_tcp_connections[index].socket;
            if (!object_try_get(target)) target = 0;
        }
        socket_unlock(&g_binding_lock);
        if (target == 0) continue;

        private = socket_private(target);
        if (private == 0) {
            object_put(target);
            continue;
        }
        socket_lock(&target->lock);
        if ((private->tcp_state == SOCKET_TCP_SYN_SENT ||
             private->tcp_state == SOCKET_TCP_SYN_RECEIVED ||
             private->tcp_state == SOCKET_TCP_ESTABLISHED) &&
            private->tcp_send_pending && private->tcp_send_deadline_tsc != 0U &&
            (int64_t)(now_tsc - private->tcp_send_deadline_tsc) >= 0) {
            if (private->tcp_send_retransmits >= SOCKET_TCP_MAX_RETRANSMITS) {
                private->tcp_send_pending = false;
                private->tcp_send_failed = true;
                private->tcp_send_deadline_tsc = 0U;
                failed = true;
            } else {
                payload_length = private->tcp_send_length;
                socket_copy(payload, private->tcp_send_payload, payload_length);
                flags = private->tcp_send_flags;
                source_address = private->local_address;
                source_port = private->local_port;
                destination_address = private->peer_address;
                destination_port = private->peer_port;
                sequence = private->tcp_send_unacknowledged;
                if (private->tcp_state == SOCKET_TCP_SYN_SENT) {
                    acknowledgement = 0U;
                    window = SOCKET_STREAM_BUFFER_SIZE;
                } else if (private->tcp_state == SOCKET_TCP_SYN_RECEIVED) {
                    acknowledgement = private->tcp_peer_initial_sequence + 1U;
                    window = SOCKET_STREAM_BUFFER_SIZE;
                } else {
                    acknowledgement = private->tcp_next_sequence;
                    uint32_t count = atomic_load_explicit(&private->stream_count,
                                                          memory_order_relaxed);
                    window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count);
                }
                ++private->tcp_send_retransmits;
                private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(now_tsc);
                retransmit = true;
            }
        }
        socket_unlock(&target->lock);

        if (failed) {
            (void)wake_all(&target->waitq);
        } else if (retransmit && output4 != 0) {
            (void)output4(output4_context, source_address, source_port,
                         destination_address, destination_port, sequence,
                         acknowledgement, flags, window, payload, payload_length);
        }
        object_put(target);
    }

    for (uint32_t index = 0U; index < SOCKET_BINDING_COUNT; ++index) {
        socket_t *target = 0;
        socket_private_t *private;
        uint8_t payload[SOCKET_MAX_PAYLOAD];
        uint8_t source_address[16];
        uint8_t destination_address[16];
        uint16_t payload_length = 0U;
        uint8_t flags = 0U;
        uint16_t source_port = 0U;
        uint16_t destination_port = 0U;
        uint32_t sequence = 0U;
        uint32_t acknowledgement = 0U;
        uint16_t window = 0U;
        bool retransmit = false;
        bool failed = false;

        socket_lock(&g_binding_lock);
        if (g_tcp6_connections[index].socket != 0) {
            target = g_tcp6_connections[index].socket;
            if (!object_try_get(target)) target = 0;
        }
        socket_unlock(&g_binding_lock);
        if (target == 0) continue;

        private = socket_private(target);
        if (private == 0) {
            object_put(target);
            continue;
        }
        socket_lock(&target->lock);
        if ((private->tcp_state == SOCKET_TCP_SYN_SENT ||
             private->tcp_state == SOCKET_TCP_SYN_RECEIVED ||
             private->tcp_state == SOCKET_TCP_ESTABLISHED) &&
            private->tcp_send_pending && private->tcp_send_deadline_tsc != 0U &&
            (int64_t)(now_tsc - private->tcp_send_deadline_tsc) >= 0) {
            if (private->tcp_send_retransmits >= SOCKET_TCP_MAX_RETRANSMITS) {
                private->tcp_send_pending = false;
                private->tcp_send_failed = true;
                private->tcp_send_deadline_tsc = 0U;
                failed = true;
            } else {
                payload_length = private->tcp_send_length;
                socket_copy(payload, private->tcp_send_payload, payload_length);
                socket_copy(source_address, private->local_address6, 16U);
                socket_copy(destination_address, private->peer_address6, 16U);
                flags = private->tcp_send_flags;
                source_port = private->local_port;
                destination_port = private->peer_port;
                sequence = private->tcp_send_unacknowledged;
                if (private->tcp_state == SOCKET_TCP_SYN_SENT) {
                    acknowledgement = 0U;
                    window = SOCKET_STREAM_BUFFER_SIZE;
                } else if (private->tcp_state == SOCKET_TCP_SYN_RECEIVED) {
                    acknowledgement = private->tcp_peer_initial_sequence + 1U;
                    window = SOCKET_STREAM_BUFFER_SIZE;
                } else {
                    acknowledgement = private->tcp_next_sequence;
                    uint32_t count = atomic_load_explicit(&private->stream_count,
                                                          memory_order_relaxed);
                    window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count);
                }
                ++private->tcp_send_retransmits;
                private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(now_tsc);
                retransmit = true;
            }
        }
        socket_unlock(&target->lock);

        if (failed) {
            (void)wake_all(&target->waitq);
        } else if (retransmit && output6 != 0) {
            (void)output6(output6_context, source_address, source_port,
                          destination_address, destination_port, sequence,
                          acknowledgement, flags, window, payload, payload_length);
        }
        object_put(target);
    }
}

static socket_private_t *socket_private(socket_t *socket) {
    return socket != 0 ? (socket_private_t *)socket->protocol_state : 0;
}

static const socket_private_t *socket_private_const(const socket_t *socket) {
    return socket != 0 ? (const socket_private_t *)socket->protocol_state : 0;
}

static bool socket_is_signaled(const void *object) {
    const socket_t *socket = (const socket_t *)object;
    const socket_private_t *private = socket_private_const(socket);
    if (private == 0) return false;
    return atomic_load_explicit(&private->datagram_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->stream_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->accept_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->closed, memory_order_acquire) ||
           atomic_load_explicit(&private->peer_closed, memory_order_acquire);
}

static int64_t socket_wait_value(const void *object) {
    const socket_t *socket = (const socket_t *)object;
    const socket_private_t *private = socket_private_const(socket);
    if (private == 0) return K_EIO;
    if (socket->type == OS_SOCK_STREAM && private->listening) {
        return (int64_t)atomic_load_explicit(&private->accept_count, memory_order_acquire);
    }
    return socket->type == OS_SOCK_STREAM ?
           (int64_t)atomic_load_explicit(&private->stream_count, memory_order_acquire) :
           (int64_t)atomic_load_explicit(&private->datagram_count, memory_order_acquire);
}

static void socket_unbind_locked(socket_t *socket) {
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_bindings[index].socket == socket) {
            socket_zero(&g_bindings[index], sizeof(g_bindings[index]));
        }
    }
}

static bool socket_register_tcp_locked(socket_t *socket, socket_private_t *private) {
    uint32_t slot = SOCKET_BINDING_COUNT;
    if (socket == 0 || private == 0 || socket->type != OS_SOCK_STREAM ||
        !private->bound || !private->connected || private->peer_port == 0U) return false;
    if (socket->family == OS_AF_INET6) {
        for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
            if (g_tcp6_connections[index].socket == socket) return true;
            if (g_tcp6_connections[index].socket == 0 && slot == SOCKET_BINDING_COUNT) {
                slot = index;
            }
        }
        if (slot == SOCKET_BINDING_COUNT) return false;
        g_tcp6_connections[slot].socket = socket;
        socket_copy(g_tcp6_connections[slot].local_address, private->local_address6, 16U);
        g_tcp6_connections[slot].local_port = private->local_port;
        socket_copy(g_tcp6_connections[slot].peer_address, private->peer_address6, 16U);
        g_tcp6_connections[slot].peer_port = private->peer_port;
        return true;
    }
    if (socket->family != OS_AF_INET4) return false;
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_tcp_connections[index].socket == socket) return true;
        if (g_tcp_connections[index].socket == 0 && slot == SOCKET_BINDING_COUNT) {
            slot = index;
        }
    }
    if (slot == SOCKET_BINDING_COUNT) return false;
    g_tcp_connections[slot].socket = socket;
    g_tcp_connections[slot].local_address = private->local_address;
    g_tcp_connections[slot].local_port = private->local_port;
    g_tcp_connections[slot].peer_address = private->peer_address;
    g_tcp_connections[slot].peer_port = private->peer_port;
    return true;
}

static uint32_t socket_unregister_tcp_locked(socket_t *socket) {
    uint32_t released = 0U;
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_tcp_connections[index].socket != socket) continue;
        socket_zero(&g_tcp_connections[index], sizeof(g_tcp_connections[index]));
        ++released;
    }
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_tcp6_connections[index].socket != socket) continue;
        socket_zero(&g_tcp6_connections[index], sizeof(g_tcp6_connections[index]));
        ++released;
    }
    return released;
}

static bool socket_tcp_slots_available_locked(uint32_t required) {
    uint32_t free_slots = 0U;
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_tcp_connections[index].socket == 0) ++free_slots;
    }
    return free_slots >= required;
}

static bool socket_tcp6_slots_available_locked(uint32_t required) {
    uint32_t free_slots = 0U;
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_tcp6_connections[index].socket == 0) ++free_slots;
    }
    return free_slots >= required;
}

static int32_t socket_find_tcp6_locked(const uint8_t source_address[16],
                                       uint16_t source_port,
                                       const uint8_t destination_address[16],
                                       uint16_t destination_port) {
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        const socket_tcp6_connection_t *connection = &g_tcp6_connections[index];
        if (connection->socket == 0 || connection->local_port != destination_port ||
            connection->peer_port != source_port ||
            (!socket_address6_zero(connection->local_address) &&
             !socket_address6_equal(connection->local_address, destination_address)) ||
            !socket_address6_equal(connection->peer_address, source_address)) continue;
        return (int32_t)index;
    }
    return -1;
}

static int32_t socket_find_tcp_locked(uint32_t source_address, uint16_t source_port,
                                      uint32_t destination_address,
                                      uint16_t destination_port) {
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        const socket_tcp_connection_t *connection = &g_tcp_connections[index];
        if (connection->socket == 0 || connection->local_port != destination_port ||
            connection->peer_port != source_port ||
            (connection->local_address != 0U &&
             connection->local_address != destination_address) ||
            connection->peer_address != source_address) continue;
        return (int32_t)index;
    }
    return -1;
}

static void socket_lock_pair(socket_t *left, socket_t *right) {
    if (left == right) {
        socket_lock(&left->lock);
        return;
    }
    if ((uintptr_t)left < (uintptr_t)right) {
        socket_lock(&left->lock);
        socket_lock(&right->lock);
    } else {
        socket_lock(&right->lock);
        socket_lock(&left->lock);
    }
}

static void socket_unlock_pair(socket_t *left, socket_t *right) {
    if (left == right) {
        socket_unlock(&left->lock);
        return;
    }
    if ((uintptr_t)left < (uintptr_t)right) {
        socket_unlock(&right->lock);
        socket_unlock(&left->lock);
    } else {
        socket_unlock(&left->lock);
        socket_unlock(&right->lock);
    }
}

static void socket_detach_peer(socket_t *socket, socket_private_t *private) {
    socket_t *peer;
    if (socket == 0 || private == 0) return;
    socket_lock(&socket->lock);
    peer = private->peer;
    private->peer = 0;
    socket_unlock(&socket->lock);
    if (peer == 0) return;

    /* 两端按地址排序加锁，避免同时关闭连接时形成 ABBA 死锁。 */
    socket_lock_pair(socket, peer);
    socket_private_t *peer_private = socket_private(peer);
    if (peer_private != 0 && peer_private->peer == socket) {
        peer_private->peer = 0;
        atomic_store_explicit(&peer_private->peer_closed, true, memory_order_release);
    }
    socket_unlock_pair(socket, peer);
    (void)wake_all(&peer->waitq);
    object_put(peer);
}

static void socket_destroy(void *object) {
    socket_t *socket = (socket_t *)object;
    if (socket == 0) return;
    socket_private_t *private = socket_private(socket);
    (void)socket_close(socket);
    kfree(private);
    kfree(socket);
}

static const object_ops_t g_socket_object_ops = {
    .destroy = socket_destroy,
    .type_name = "Socket",
    .is_signaled = socket_is_signaled,
    .wait_value = socket_wait_value,
};

static int32_t socket_find_binding_locked(uint32_t address, uint16_t port,
                                          uint16_t type, bool listening_only) {
    int32_t wildcard = -1;
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        const socket_binding_t *binding = &g_bindings[index];
        const socket_private_t *private;
        if (binding->socket == 0 || binding->family != OS_AF_INET4 ||
            binding->port != port || binding->type != type) continue;
        private = socket_private_const(binding->socket);
        if (listening_only && (private == 0 || !private->listening)) continue;
        if (binding->address == address) return (int32_t)index;
        if (binding->address == 0U) wildcard = (int32_t)index;
    }
    return wildcard;
}

static int32_t socket_find_binding6_locked(const uint8_t address[16], uint16_t port,
                                           uint16_t type, bool listening_only) {
    int32_t wildcard = -1;
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        const socket_binding_t *binding = &g_bindings[index];
        const socket_private_t *private;
        if (binding->socket == 0 || binding->family != OS_AF_INET6 ||
            binding->port != port || binding->type != type) continue;
        private = socket_private_const(binding->socket);
        if (listening_only && (private == 0 || !private->listening)) continue;
        if (socket_address6_equal(binding->address6, address)) return (int32_t)index;
        if (socket_address6_zero(binding->address6)) wildcard = (int32_t)index;
    }
    return wildcard;
}

static bool socket_port_available_locked(uint32_t address, uint16_t port,
                                         uint16_t type, const socket_t *self) {
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        const socket_binding_t *binding = &g_bindings[index];
        if (binding->socket == 0 || binding->socket == self ||
            binding->family != OS_AF_INET4 || binding->port != port ||
            binding->type != type) continue;
        if (binding->address == 0U || address == 0U || binding->address == address) {
            return false;
        }
    }
    return true;
}

static bool socket_port6_available_locked(const uint8_t address[16], uint16_t port,
                                          uint16_t type, const socket_t *self) {
    bool wildcard = socket_address6_zero(address);
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        const socket_binding_t *binding = &g_bindings[index];
        if (binding->socket == 0 || binding->socket == self ||
            binding->family != OS_AF_INET6 || binding->port != port ||
            binding->type != type) continue;
        if (wildcard || socket_address6_zero(binding->address6) ||
            socket_address6_equal(binding->address6, address)) return false;
    }
    return true;
}

static kstatus_t socket_bind_locked(socket_t *socket, socket_private_t *private,
                                    uint32_t address, uint16_t port) {
    uint32_t slot = SOCKET_BINDING_COUNT;
    if (private->bound || !socket_port_available_locked(address, port, socket->type, socket)) {
        return K_EBUSY;
    }
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_bindings[index].socket == 0) {
            slot = index;
            break;
        }
    }
    if (slot == SOCKET_BINDING_COUNT) return K_ENOMEM;
    g_bindings[slot].socket = socket;
    g_bindings[slot].family = OS_AF_INET4;
    g_bindings[slot].address = address;
    g_bindings[slot].port = port;
    g_bindings[slot].type = socket->type;
    private->bound = true;
    private->local_address = address;
    private->local_port = port;
    return K_OK;
}

static kstatus_t socket_bind6_locked(socket_t *socket, socket_private_t *private,
                                     const uint8_t address[16], uint16_t port) {
    uint32_t slot = SOCKET_BINDING_COUNT;
    if (private->bound || !socket_port6_available_locked(address, port, socket->type, socket)) {
        return K_EBUSY;
    }
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_bindings[index].socket == 0) {
            slot = index;
            break;
        }
    }
    if (slot == SOCKET_BINDING_COUNT) return K_ENOMEM;
    g_bindings[slot].socket = socket;
    g_bindings[slot].family = OS_AF_INET6;
    socket_copy(g_bindings[slot].address6, address, 16U);
    g_bindings[slot].port = port;
    g_bindings[slot].type = socket->type;
    private->bound = true;
    socket_copy(private->local_address6, address, 16U);
    private->local_port = port;
    return K_OK;
}

static kstatus_t socket_bind_ephemeral(socket_t *socket, socket_private_t *private) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    uint32_t first = atomic_fetch_add_explicit(&g_next_ephemeral_port, 1U,
                                               memory_order_relaxed);
    uint32_t span = SOCKET_EPHEMERAL_LAST - SOCKET_EPHEMERAL_FIRST + 1U;
    kstatus_t status = K_EAGAIN;
    for (uint32_t attempt = 0; attempt < span; ++attempt) {
        uint16_t port = (uint16_t)(SOCKET_EPHEMERAL_FIRST +
            ((first - SOCKET_EPHEMERAL_FIRST + attempt) % span));
        status = socket_bind_locked(socket, private, 0x7F000001U, port);
        if (status == K_OK || status != K_EBUSY) break;
    }
    socket_unlock(&g_binding_lock);
    return status;
}

static kstatus_t socket_bind6_ephemeral(socket_t *socket, socket_private_t *private) {
    static const uint8_t loopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1};
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    uint32_t first = atomic_fetch_add_explicit(&g_next_ephemeral_port, 1U,
                                               memory_order_relaxed);
    uint32_t span = SOCKET_EPHEMERAL_LAST - SOCKET_EPHEMERAL_FIRST + 1U;
    kstatus_t status = K_EAGAIN;
    for (uint32_t attempt = 0; attempt < span; ++attempt) {
        uint16_t port = (uint16_t)(SOCKET_EPHEMERAL_FIRST +
            ((first - SOCKET_EPHEMERAL_FIRST + attempt) % span));
        status = socket_bind6_locked(socket, private, loopback6, port);
        if (status == K_OK || status != K_EBUSY) break;
    }
    socket_unlock(&g_binding_lock);
    return status;
}

static kstatus_t socket_allocate(uint16_t family, uint16_t type, uint16_t protocol,
                                 socket_t **out) {
    socket_t *socket = (socket_t *)kzalloc(sizeof(*socket), 0);
    socket_private_t *private = (socket_private_t *)kzalloc(sizeof(*private), 0);
    if (socket == 0 || private == 0) {
        kfree(private);
        kfree(socket);
        return K_ENOMEM;
    }
    refcount_init(&socket->object.refs, 1U);
    socket->object.type = KOBJECT_TYPE_SOCKET;
    socket->object.flags = 0;
    socket->object.ops = &g_socket_object_ops;
    socket->object.security = 0;
    socket->family = family;
    socket->type = type;
    socket->protocol = protocol;
    wait_queue_init(&socket->waitq);
    atomic_init(&socket->lock.state, 0U);
    atomic_init(&private->datagram_count, 0U);
    atomic_init(&private->stream_count, 0U);
    atomic_init(&private->accept_count, 0U);
    atomic_init(&private->closed, false);
    atomic_init(&private->peer_closed, false);
    socket->rx_queue = private;
    socket->tx_queue = 0;
    socket->protocol_state = private;
    *out = socket;
    return K_OK;
}

kstatus_t socket_create(uint16_t family, uint16_t type, uint16_t protocol,
                        socket_t **out) {
    if (out == 0 || (family != OS_AF_INET4 && family != OS_AF_INET6) ||
        (type != OS_SOCK_DGRAM && type != OS_SOCK_STREAM) ||
        (protocol != 0U && protocol != SOCKET_PROTOCOL_UDP &&
         protocol != SOCKET_PROTOCOL_TCP) ||
        (type == OS_SOCK_DGRAM && protocol == SOCKET_PROTOCOL_TCP) ||
        (type == OS_SOCK_STREAM && protocol == SOCKET_PROTOCOL_UDP)) return K_EINVAL;
    socket_initialize_globals();
    return socket_allocate(family, type,
                           protocol == 0U ? (type == OS_SOCK_STREAM ?
                                             SOCKET_PROTOCOL_TCP : SOCKET_PROTOCOL_UDP) : protocol,
                           out);
}

kstatus_t socket_bind(socket_t *socket, uint32_t address, uint16_t port) {
    socket_private_t *private;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4 || port == 0U) {
        return K_EINVAL;
    }
    private = socket_private(socket);
    if (private == 0 || atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    kstatus_t status = socket_bind_locked(socket, private, address, port);
    socket_unlock(&g_binding_lock);
    return status;
}

kstatus_t socket_bind_ipv6(socket_t *socket, const uint8_t address[16], uint16_t port) {
    socket_private_t *private;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || address == 0 || port == 0U) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0 || atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    kstatus_t status = socket_bind6_locked(socket, private, address, port);
    socket_unlock(&g_binding_lock);
    return status;
}

static void socket_abort_child(socket_t *child) {
    (void)socket_close(child);
    object_put(child);
}

static void socket_remove_from_accept_queue_locked(socket_t *listener,
                                                   socket_t *child) {
    socket_private_t *private = socket_private(listener);
    uint32_t count;
    uint32_t offset;
    if (private == 0 || child == 0) return;
    count = atomic_load_explicit(&private->accept_count, memory_order_relaxed);
    for (offset = 0U; offset < count; ++offset) {
        uint32_t index = (private->accept_read + offset) % SOCKET_LISTEN_BACKLOG;
        if (private->accept_queue[index] != child) continue;
        for (uint32_t shift = offset; shift + 1U < count; ++shift) {
            uint32_t from = (private->accept_read + shift + 1U) % SOCKET_LISTEN_BACKLOG;
            uint32_t to = (private->accept_read + shift) % SOCKET_LISTEN_BACKLOG;
            private->accept_queue[to] = private->accept_queue[from];
        }
        uint32_t last = (private->accept_read + count - 1U) % SOCKET_LISTEN_BACKLOG;
        private->accept_queue[last] = 0;
        private->accept_write = last;
        atomic_store_explicit(&private->accept_count, count - 1U, memory_order_release);
        return;
    }
}

kstatus_t socket_close(socket_t *socket) {
    socket_private_t *private;
    socket_t *accepted[SOCKET_LISTEN_BACKLOG];
    socket_t *listener_ref = 0;
    uint32_t accepted_count = 0;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0) return K_EIO;
    socket_initialize_globals();

    socket_lock(&socket->lock);
    bool was_closed = atomic_exchange_explicit(&private->closed, true,
                                               memory_order_acq_rel);
    while (!was_closed &&
           atomic_load_explicit(&private->accept_count, memory_order_relaxed) != 0U &&
           accepted_count < SOCKET_LISTEN_BACKLOG) {
        accepted[accepted_count++] = private->accept_queue[private->accept_read];
        private->accept_queue[private->accept_read] = 0;
        private->accept_read = (private->accept_read + 1U) % SOCKET_LISTEN_BACKLOG;
        atomic_fetch_sub_explicit(&private->accept_count, 1U, memory_order_relaxed);
    }
    listener_ref = private->listener;
    private->listener = 0;
    socket_unlock(&socket->lock);
    if (was_closed) return K_OK;

    if (listener_ref != 0) {
        socket_lock(&listener_ref->lock);
        socket_remove_from_accept_queue_locked(listener_ref, socket);
        socket_unlock(&listener_ref->lock);
        object_put(listener_ref);
    }

    socket_lock(&g_binding_lock);
    socket_unbind_locked(socket);
    (void)socket_unregister_tcp_locked(socket);
    socket_unlock(&g_binding_lock);
    (void)wake_all(&socket->waitq);
    socket_detach_peer(socket, private);
    for (uint32_t index = 0; index < accepted_count; ++index) {
        (void)socket_close(accepted[index]);
        object_put(accepted[index]);
    }
    return K_OK;
}

kstatus_t socket_connect(socket_t *socket, uint32_t address, uint16_t port) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4 || private == 0 ||
        address == 0U || port == 0U) return K_EINVAL;
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) return K_EDEVREMOVED;

    if (socket->type == OS_SOCK_DGRAM) {
        socket_lock(&socket->lock);
        private->connected = true;
        private->peer_address = address;
        private->peer_port = port;
        socket_unlock(&socket->lock);
        return K_OK;
    }

    if (!private->bound) {
        kstatus_t bind_status = socket_bind_ephemeral(socket, private);
        if (bind_status != K_OK) return bind_status;
    }
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t binding = socket_find_binding_locked(address, port, OS_SOCK_STREAM, true);
    socket_t *listener = binding >= 0 ? g_bindings[binding].socket : 0;
    if (listener != 0 && !object_try_get(listener)) listener = 0;
    socket_unlock(&g_binding_lock);
    if (listener == 0) {
        socket_tcp_ipv4_output_fn output = 0;
        void *output_context = 0;
        uint32_t initial_sequence;
        kstatus_t output_status;
        socket_tcp_output_get(&output, &output_context);
        if (output == 0) return K_ENOENT;

        socket_lock(&socket->lock);
        if (private->connected ||
            atomic_load_explicit(&private->closed, memory_order_acquire)) {
            socket_unlock(&socket->lock);
            return K_EBUSY;
        }
        initial_sequence = 0x20000000U ^ ((uint32_t)private->local_port << 8U) ^ port;
        private->connected = true;
        private->peer_address = address;
        private->peer_port = port;
        private->tcp_initial_sequence = initial_sequence;
        private->tcp_send_next_sequence = initial_sequence + 1U;
        private->tcp_send_unacknowledged = initial_sequence;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = NET_TCP_FLAG_SYN;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_pending = true;
        private->tcp_send_failed = false;
        private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
        private->tcp_peer_window = 0U;
        private->tcp_sequence_valid = false;
        private->tcp_state = SOCKET_TCP_SYN_SENT;
        socket_unlock(&socket->lock);

        socket_lock(&g_binding_lock);
        bool registered = socket_register_tcp_locked(socket, private);
        socket_unlock(&g_binding_lock);
        if (!registered) {
            socket_lock(&socket->lock);
            private->connected = false;
            private->tcp_state = SOCKET_TCP_CLOSED;
            private->tcp_send_pending = false;
            socket_unlock(&socket->lock);
            return K_ENOMEM;
        }

        output_status = output(output_context, private->local_address,
                               private->local_port, address, port,
                               initial_sequence, 0U, NET_TCP_FLAG_SYN,
                               SOCKET_STREAM_BUFFER_SIZE, 0, 0U);
        /* ARP 未命中时 SYN 留在重传队列，后续轮询会再次尝试发送。 */
        return output_status == K_EAGAIN ? K_OK : output_status;
    }

    socket_lock(&g_binding_lock);
    bool tcp_slots_available = socket_tcp_slots_available_locked(2U);
    socket_unlock(&g_binding_lock);
    if (!tcp_slots_available) {
        object_put(listener);
        return K_ENOMEM;
    }

    socket_private_t *listener_private = socket_private(listener);
    socket_t *child = 0;
    if (listener_private == 0 ||
        socket_allocate(OS_AF_INET4, OS_SOCK_STREAM, SOCKET_PROTOCOL_TCP, &child) != K_OK) {
        object_put(listener);
        return K_ENOMEM;
    }
    socket_private_t *child_private = socket_private(child);
    socket_lock(&socket->lock);
    bool already_connected = private->connected ||
        atomic_load_explicit(&private->closed, memory_order_acquire);
    socket_unlock(&socket->lock);
    if (already_connected) {
        object_put(listener);
        socket_abort_child(child);
        return K_EBUSY;
    }

    socket_lock(&listener->lock);
    bool can_accept = listener_private->listening &&
        !atomic_load_explicit(&listener_private->closed, memory_order_acquire) &&
        atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) <
            SOCKET_LISTEN_BACKLOG;
    if (!can_accept) {
        socket_unlock(&listener->lock);
        object_put(listener);
        socket_abort_child(child);
        return K_EAGAIN;
    }

    child_private->bound = true;
    child_private->local_address = listener_private->local_address;
    child_private->local_port = listener_private->local_port;
    child_private->connected = true;
    child_private->peer_address = private->local_address;
    child_private->peer_port = private->local_port;
    child_private->peer = socket;
    child_private->tcp_state = SOCKET_TCP_ESTABLISHED;
    child_private->tcp_sequence_valid = false;
    object_get(socket);

    socket_lock(&socket->lock);
    private->connected = true;
    private->peer_address = child_private->local_address;
    private->peer_port = child_private->local_port;
    private->peer = child;
    object_get(child);
    socket_unlock(&socket->lock);

    listener_private->accept_queue[listener_private->accept_write] = child;
    listener_private->accept_write =
        (listener_private->accept_write + 1U) % SOCKET_LISTEN_BACKLOG;
    atomic_fetch_add_explicit(&listener_private->accept_count, 1U, memory_order_release);
    socket_unlock(&listener->lock);
    (void)wake_all(&listener->waitq);
    object_put(listener);

    socket_lock(&g_binding_lock);
    bool registered = socket_register_tcp_locked(socket, private) &&
                      socket_register_tcp_locked(child, child_private);
    socket_unlock(&g_binding_lock);
    if (!registered) {
        /* 连接表满载只影响网卡 RX，不破坏已经建立的内核本地连接。 */
    }
    return K_OK;
}

kstatus_t socket_connect_ipv6(socket_t *socket, const uint8_t address[16], uint16_t port) {
    socket_private_t *private = socket_private(socket);
    socket_tcp_ipv6_output_fn output = 0;
    void *output_context = 0;
    uint32_t initial_sequence;
    kstatus_t output_status;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || private == 0 || address == 0 || port == 0U ||
        socket_address6_zero(address)) return K_EINVAL;
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) return K_EDEVREMOVED;
    if (socket->type == OS_SOCK_DGRAM) {
        if (!private->bound) {
            kstatus_t bind_status = socket_bind6_ephemeral(socket, private);
            if (bind_status != K_OK) return bind_status;
        }
        socket_lock(&socket->lock);
        private->connected = true;
        socket_copy(private->peer_address6, address, 16U);
        private->peer_port = port;
        socket_unlock(&socket->lock);
        return K_OK;
    }
    if (!private->bound) {
        kstatus_t bind_status = socket_bind6_ephemeral(socket, private);
        if (bind_status != K_OK) return bind_status;
    }
    socket_tcp6_output_get(&output, &output_context);
    if (output == 0) return K_ENOENT;
    socket_lock(&socket->lock);
    if (private->connected || atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        return K_EBUSY;
    }
    initial_sequence = 0x30000000U ^ ((uint32_t)private->local_port << 8U) ^ port;
    private->connected = true;
    socket_copy(private->peer_address6, address, 16U);
    private->peer_port = port;
    private->tcp_initial_sequence = initial_sequence;
    private->tcp_send_next_sequence = initial_sequence + 1U;
    private->tcp_send_unacknowledged = initial_sequence;
    private->tcp_send_length = 0U;
    private->tcp_send_flags = NET_TCP_FLAG_SYN;
    private->tcp_send_retransmits = 0U;
    private->tcp_send_pending = true;
    private->tcp_send_failed = false;
    private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
    private->tcp_peer_window = 0U;
    private->tcp_sequence_valid = false;
    private->tcp_state = SOCKET_TCP_SYN_SENT;
    uint8_t source_address[16];
    socket_copy(source_address, private->local_address6, 16U);
    socket_unlock(&socket->lock);

    socket_lock(&g_binding_lock);
    bool registered = socket_register_tcp_locked(socket, private);
    socket_unlock(&g_binding_lock);
    if (!registered) {
        socket_lock(&socket->lock);
        private->connected = false;
        private->tcp_state = SOCKET_TCP_CLOSED;
        private->tcp_send_pending = false;
        socket_unlock(&socket->lock);
        return K_ENOMEM;
    }
    output_status = output(output_context, source_address, private->local_port,
                           address, port, initial_sequence, 0U, NET_TCP_FLAG_SYN,
                           SOCKET_STREAM_BUFFER_SIZE, 0, 0U);
    return output_status == K_EAGAIN ? K_OK : output_status;
}

kstatus_t socket_listen(socket_t *socket, uint32_t backlog) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->type != OS_SOCK_STREAM || private == 0 || backlog == 0U) return K_EINVAL;
    if (backlog > SOCKET_LISTEN_BACKLOG) backlog = SOCKET_LISTEN_BACKLOG;
    socket_lock(&socket->lock);
    if (!private->bound || private->connected || private->listening ||
        atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        return K_EBUSY;
    }
    private->listening = true;
    socket_unlock(&socket->lock);
    (void)backlog;
    return K_OK;
}

static bool socket_accept_ready(void *context) {
    socket_wait_context_t *wait = (socket_wait_context_t *)context;
    bool ready = false;
    if (wait == 0 || wait->private == 0) return false;
    socket_t *socket = wait->socket;
    socket_lock(&socket->lock);
    uint32_t count = atomic_load_explicit(&wait->private->accept_count,
                                          memory_order_acquire);
    for (uint32_t offset = 0U; offset < count; ++offset) {
        uint32_t index = (wait->private->accept_read + offset) % SOCKET_LISTEN_BACKLOG;
        socket_t *candidate = wait->private->accept_queue[index];
        socket_private_t *candidate_private = socket_private(candidate);
        if (candidate_private != 0 &&
            candidate_private->tcp_state == SOCKET_TCP_ESTABLISHED) {
            ready = true;
            break;
        }
    }
    ready = ready || atomic_load_explicit(&wait->private->closed, memory_order_acquire);
    socket_unlock(&socket->lock);
    return ready;
}

kstatus_t socket_accept(socket_t *socket, uint64_t timeout_ns, socket_t **out) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->type != OS_SOCK_STREAM || private == 0 || out == 0) return K_EINVAL;
    socket_wait_context_t wait = {.socket = socket, .private = private};
    for (;;) {
        kstatus_t status = wait_on_queue(&socket->waitq, socket_accept_ready,
                                         &wait, timeout_ns);
        if (status != K_OK) return status;
        socket_lock(&socket->lock);
        uint32_t count = atomic_load_explicit(&private->accept_count, memory_order_relaxed);
        socket_t *accepted = 0;
        uint32_t accepted_offset = 0U;
        for (; accepted_offset < count; ++accepted_offset) {
            uint32_t index = (private->accept_read + accepted_offset) % SOCKET_LISTEN_BACKLOG;
            socket_private_t *candidate_private = socket_private(private->accept_queue[index]);
            if (candidate_private != 0 &&
                candidate_private->tcp_state == SOCKET_TCP_ESTABLISHED) {
                accepted = private->accept_queue[index];
                break;
            }
        }
        if (accepted != 0) {
            for (uint32_t shift = accepted_offset; shift + 1U < count; ++shift) {
                uint32_t from = (private->accept_read + shift + 1U) % SOCKET_LISTEN_BACKLOG;
                uint32_t to = (private->accept_read + shift) % SOCKET_LISTEN_BACKLOG;
                private->accept_queue[to] = private->accept_queue[from];
            }
            uint32_t last = (private->accept_read + count - 1U) % SOCKET_LISTEN_BACKLOG;
            private->accept_queue[last] = 0;
            private->accept_write = last;
            atomic_store_explicit(&private->accept_count, count - 1U, memory_order_release);
            socket_unlock(&socket->lock);
            *out = accepted; /* 转移监听队列持有的对象引用给调用者。 */
            return K_OK;
        }
        bool closed = atomic_load_explicit(&private->closed, memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}

kstatus_t socket_inject_udp(uint32_t source_address, uint16_t source_port,
                            uint32_t destination_address, uint16_t destination_port,
                            const void *buffer, size_t length) {
    socket_t *target = 0;
    socket_private_t *private;
    if (source_address == 0U || source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0) || length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t binding = socket_find_binding_locked(destination_address, destination_port,
                                                 OS_SOCK_DGRAM, false);
    if (binding >= 0) {
        target = g_bindings[binding].socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(&g_binding_lock);
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    uint32_t count = atomic_load_explicit(&private->datagram_count, memory_order_relaxed);
    if (atomic_load_explicit(&private->closed, memory_order_acquire) ||
        count >= SOCKET_RX_QUEUE_DEPTH) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    socket_datagram_t *datagram = &private->datagrams[private->datagram_write];
    datagram->family = OS_AF_INET4;
    datagram->source_address = source_address;
    datagram->source_port = source_port;
    datagram->destination_address = destination_address;
    datagram->destination_port = destination_port;
    datagram->length = (uint16_t)length;
    if (length != 0) socket_copy(datagram->payload, buffer, length);
    private->datagram_write = (private->datagram_write + 1U) % SOCKET_RX_QUEUE_DEPTH;
    atomic_store_explicit(&private->datagram_count, count + 1U, memory_order_release);
    socket_unlock(&target->lock);
    (void)wake_all(&target->waitq);
    object_put(target);
    return K_OK;
}

kstatus_t socket_inject_udp_ipv6(const uint8_t source_address[16], uint16_t source_port,
                                 const uint8_t destination_address[16],
                                 uint16_t destination_port,
                                 const void *buffer, size_t length) {
    socket_t *target = 0;
    socket_private_t *private;
    if (source_address == 0 || destination_address == 0 || source_port == 0U ||
        destination_port == 0U || (buffer == 0 && length != 0U) ||
        length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t binding = socket_find_binding6_locked(destination_address, destination_port,
                                                  OS_SOCK_DGRAM, false);
    if (binding >= 0) {
        target = g_bindings[binding].socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(&g_binding_lock);
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    uint32_t count = atomic_load_explicit(&private->datagram_count, memory_order_relaxed);
    if (atomic_load_explicit(&private->closed, memory_order_acquire) ||
        count >= SOCKET_RX_QUEUE_DEPTH) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    socket_datagram_t *datagram = &private->datagrams[private->datagram_write];
    datagram->family = OS_AF_INET6;
    socket_copy(datagram->source_address6, source_address, 16U);
    socket_copy(datagram->destination_address6, destination_address, 16U);
    datagram->source_port = source_port;
    datagram->destination_port = destination_port;
    datagram->length = (uint16_t)length;
    if (length != 0U) socket_copy(datagram->payload, buffer, length);
    private->datagram_write = (private->datagram_write + 1U) % SOCKET_RX_QUEUE_DEPTH;
    atomic_store_explicit(&private->datagram_count, count + 1U, memory_order_release);
    socket_unlock(&target->lock);
    (void)wake_all(&target->waitq);
    object_put(target);
    return K_OK;
}

kstatus_t socket_inject_tcp_ipv4_reply(uint32_t source_address, uint16_t source_port,
                                       uint32_t destination_address,
                                       uint16_t destination_port, uint32_t sequence,
                                       uint8_t flags, const void *buffer, size_t length,
                                       socket_tcp_reply_t *reply) {
    socket_t *target = 0;
    socket_private_t *private;
    bool notify = false;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    if (source_address == 0U || source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0U) || length > SOCKET_STREAM_BUFFER_SIZE ||
        (flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) != NET_TCP_FLAG_ACK) {
        return K_EINVAL;
    }
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t connection = socket_find_tcp_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = g_tcp_connections[connection].socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(&g_binding_lock);
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    if (private->tcp_state != SOCKET_TCP_ESTABLISHED) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    uint32_t count = atomic_load_explicit(&private->stream_count, memory_order_relaxed);
    uint32_t expected = private->tcp_sequence_valid ? private->tcp_next_sequence : sequence;
    if (reply != 0) {
        reply->valid = true;
        reply->flags = NET_TCP_FLAG_ACK;
        reply->sequence = private->tcp_next_sequence;
        reply->acknowledgement = expected;
        reply->window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count);
    }
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EDEVREMOVED;
    }
    if (sequence != expected) {
        /* 当前 RX 窗口只接受下一个连续序号；重复/乱序包由上层重传机制处理。 */
        socket_unlock(&target->lock);
        object_put(target);
        return sequence < expected ? K_OK : K_EAGAIN;
    }
    if (count > SOCKET_STREAM_BUFFER_SIZE - length) {
        if (reply != 0) reply->window = 0U;
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    for (size_t index = 0U; index < length; ++index) {
        private->stream_buffer[private->stream_write] = ((const uint8_t *)buffer)[index];
        private->stream_write = (private->stream_write + 1U) % SOCKET_STREAM_BUFFER_SIZE;
    }
    private->tcp_sequence_valid = true;
    private->tcp_next_sequence = sequence + (uint32_t)length;
    if ((flags & NET_TCP_FLAG_FIN) != 0U) {
        ++private->tcp_next_sequence;
        atomic_store_explicit(&private->peer_closed, true, memory_order_release);
    }
    atomic_store_explicit(&private->stream_count, count + (uint32_t)length,
                          memory_order_release);
    if (reply != 0) {
        reply->acknowledgement = private->tcp_next_sequence;
        reply->window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count - length);
    }
    notify = length != 0U || (flags & NET_TCP_FLAG_FIN) != 0U;
    socket_unlock(&target->lock);
    if (notify) (void)wake_all(&target->waitq);
    object_put(target);
    return K_OK;
}

kstatus_t socket_inject_tcp_ipv6_reply(const uint8_t source_address[16],
                                       uint16_t source_port,
                                       const uint8_t destination_address[16],
                                       uint16_t destination_port, uint32_t sequence,
                                       uint8_t flags, const void *buffer, size_t length,
                                       socket_tcp_reply_t *reply) {
    socket_t *target = 0;
    socket_private_t *private;
    bool notify = false;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    if (source_address == 0 || destination_address == 0 ||
        socket_address6_zero(source_address) || socket_address6_zero(destination_address) ||
        source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0U) || length > SOCKET_STREAM_BUFFER_SIZE ||
        (flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) != NET_TCP_FLAG_ACK) {
        return K_EINVAL;
    }
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t connection = socket_find_tcp6_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = g_tcp6_connections[connection].socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(&g_binding_lock);
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    if (private->tcp_state != SOCKET_TCP_ESTABLISHED) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    uint32_t count = atomic_load_explicit(&private->stream_count, memory_order_relaxed);
    uint32_t expected = private->tcp_sequence_valid ? private->tcp_next_sequence : sequence;
    if (reply != 0) {
        reply->valid = true;
        reply->flags = NET_TCP_FLAG_ACK;
        reply->sequence = private->tcp_next_sequence;
        reply->acknowledgement = expected;
        reply->window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count);
    }
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EDEVREMOVED;
    }
    if (sequence != expected) {
        socket_unlock(&target->lock);
        object_put(target);
        return sequence < expected ? K_OK : K_EAGAIN;
    }
    if (count > SOCKET_STREAM_BUFFER_SIZE - length) {
        if (reply != 0) reply->window = 0U;
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    for (size_t index = 0U; index < length; ++index) {
        private->stream_buffer[private->stream_write] = ((const uint8_t *)buffer)[index];
        private->stream_write = (private->stream_write + 1U) % SOCKET_STREAM_BUFFER_SIZE;
    }
    private->tcp_sequence_valid = true;
    private->tcp_next_sequence = sequence + (uint32_t)length;
    if ((flags & NET_TCP_FLAG_FIN) != 0U) {
        ++private->tcp_next_sequence;
        atomic_store_explicit(&private->peer_closed, true, memory_order_release);
    }
    atomic_store_explicit(&private->stream_count, count + (uint32_t)length,
                          memory_order_release);
    if (reply != 0) {
        reply->acknowledgement = private->tcp_next_sequence;
        reply->window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count - length);
    }
    notify = length != 0U || (flags & NET_TCP_FLAG_FIN) != 0U;
    socket_unlock(&target->lock);
    if (notify) (void)wake_all(&target->waitq);
    object_put(target);
    return K_OK;
}

kstatus_t socket_inject_tcp_ipv4(uint32_t source_address, uint16_t source_port,
                                 uint32_t destination_address,
                                 uint16_t destination_port, uint32_t sequence,
                                 uint8_t flags, const void *buffer, size_t length) {
    return socket_inject_tcp_ipv4_reply(source_address, source_port,
                                        destination_address, destination_port,
                                        sequence, flags, buffer, length, 0);
}

kstatus_t socket_inject_tcp_syn_ipv4(uint32_t source_address, uint16_t source_port,
                                     uint32_t destination_address,
                                     uint16_t destination_port, uint32_t sequence,
                                     socket_tcp_reply_t *reply) {
    socket_t *listener = 0;
    socket_t *child = 0;
    socket_private_t *listener_private;
    socket_private_t *child_private;
    if (source_address == 0U || source_port == 0U || destination_address == 0U ||
        destination_port == 0U || reply == 0) return K_EINVAL;
    socket_zero(reply, sizeof(*reply));
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t binding = socket_find_binding_locked(destination_address, destination_port,
                                                 OS_SOCK_STREAM, true);
    if (binding >= 0) {
        listener = g_bindings[binding].socket;
        if (!object_try_get(listener)) listener = 0;
    }
    socket_unlock(&g_binding_lock);
    if (listener == 0) return K_ENOENT;
    listener_private = socket_private(listener);
    if (listener_private == 0) {
        object_put(listener);
        return K_EIO;
    }
    socket_lock(&g_binding_lock);
    int32_t existing = socket_find_tcp_locked(source_address, source_port,
                                               destination_address, destination_port);
    socket_t *existing_socket = existing >= 0 ? g_tcp_connections[existing].socket : 0;
    if (existing_socket != 0 && !object_try_get(existing_socket)) existing_socket = 0;
    socket_unlock(&g_binding_lock);
    if (existing_socket != 0) {
        socket_private_t *existing_private = socket_private(existing_socket);
        socket_lock(&existing_socket->lock);
        if (existing_private != 0 && existing_private->tcp_state == SOCKET_TCP_SYN_RECEIVED) {
            reply->valid = true;
            reply->flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
            reply->window = SOCKET_STREAM_BUFFER_SIZE;
            reply->sequence = existing_private->tcp_initial_sequence;
            reply->acknowledgement = sequence + 1U;
            socket_unlock(&existing_socket->lock);
            object_put(existing_socket);
            object_put(listener);
            return K_OK;
        }
        socket_unlock(&existing_socket->lock);
        object_put(existing_socket);
    }
    socket_lock(&listener->lock);
    if (!listener_private->listening ||
        atomic_load_explicit(&listener_private->closed, memory_order_acquire) ||
        atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) >=
            SOCKET_LISTEN_BACKLOG) {
        socket_unlock(&listener->lock);
        object_put(listener);
        return K_EAGAIN;
    }
    socket_unlock(&listener->lock);
    socket_lock(&g_binding_lock);
    bool available = socket_tcp_slots_available_locked(1U);
    socket_unlock(&g_binding_lock);
    if (!available || socket_allocate(OS_AF_INET4, OS_SOCK_STREAM,
                                      SOCKET_PROTOCOL_TCP, &child) != K_OK) {
        object_put(listener);
        return K_ENOMEM;
    }
    child_private = socket_private(child);
    child_private->bound = true;
    child_private->connected = true;
    child_private->local_address = destination_address;
    child_private->local_port = destination_port;
    child_private->peer_address = source_address;
    child_private->peer_port = source_port;
    child_private->tcp_peer_initial_sequence = sequence;
    child_private->tcp_initial_sequence = 0x10000000U + source_port;
    child_private->tcp_next_sequence = child_private->tcp_initial_sequence + 1U;
    child_private->tcp_send_next_sequence = child_private->tcp_initial_sequence + 1U;
    child_private->tcp_send_unacknowledged = child_private->tcp_initial_sequence;
    child_private->tcp_send_length = 0U;
    child_private->tcp_send_flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
    child_private->tcp_send_pending = true;
    child_private->tcp_send_retransmits = 0U;
    child_private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
    child_private->tcp_peer_window = SOCKET_STREAM_BUFFER_SIZE;
    child_private->tcp_sequence_valid = true;
    child_private->tcp_state = SOCKET_TCP_SYN_RECEIVED;
    child_private->peer = 0;
    child_private->listener = listener;
    object_get(listener);

    socket_lock(&listener->lock);
    if (atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) >=
        SOCKET_LISTEN_BACKLOG) {
        socket_unlock(&listener->lock);
        socket_abort_child(child);
        object_put(listener);
        return K_EAGAIN;
    }
    listener_private->accept_queue[listener_private->accept_write] = child;
    listener_private->accept_write =
        (listener_private->accept_write + 1U) % SOCKET_LISTEN_BACKLOG;
    atomic_fetch_add_explicit(&listener_private->accept_count, 1U, memory_order_release);
    socket_unlock(&listener->lock);
    (void)wake_all(&listener->waitq);
    object_put(listener);

    socket_lock(&g_binding_lock);
    bool registered = socket_register_tcp_locked(child, child_private);
    socket_unlock(&g_binding_lock);
    if (!registered) {
        socket_abort_child(child);
        return K_ENOMEM;
    }
    reply->valid = true;
    reply->flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
    reply->window = SOCKET_STREAM_BUFFER_SIZE;
    reply->sequence = child_private->tcp_initial_sequence;
    reply->acknowledgement = sequence + 1U;
    return K_OK;
}

kstatus_t socket_inject_tcp_syn_ipv6(const uint8_t source_address[16],
                                     uint16_t source_port,
                                     const uint8_t destination_address[16],
                                     uint16_t destination_port, uint32_t sequence,
                                     socket_tcp_reply_t *reply) {
    socket_t *listener = 0;
    socket_t *child = 0;
    socket_private_t *listener_private;
    socket_private_t *child_private;
    if (source_address == 0 || destination_address == 0 ||
        socket_address6_zero(source_address) || socket_address6_zero(destination_address) ||
        source_port == 0U || destination_port == 0U || reply == 0) return K_EINVAL;
    socket_zero(reply, sizeof(*reply));
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t binding = socket_find_binding6_locked(destination_address, destination_port,
                                                  OS_SOCK_STREAM, true);
    if (binding >= 0) {
        listener = g_bindings[binding].socket;
        if (!object_try_get(listener)) listener = 0;
    }
    socket_unlock(&g_binding_lock);
    if (listener == 0) return K_ENOENT;
    listener_private = socket_private(listener);
    if (listener_private == 0) {
        object_put(listener);
        return K_EIO;
    }
    socket_lock(&g_binding_lock);
    int32_t existing = socket_find_tcp6_locked(source_address, source_port,
                                                destination_address, destination_port);
    socket_t *existing_socket = existing >= 0 ? g_tcp6_connections[existing].socket : 0;
    if (existing_socket != 0 && !object_try_get(existing_socket)) existing_socket = 0;
    socket_unlock(&g_binding_lock);
    if (existing_socket != 0) {
        socket_private_t *existing_private = socket_private(existing_socket);
        socket_lock(&existing_socket->lock);
        if (existing_private != 0 && existing_private->tcp_state == SOCKET_TCP_SYN_RECEIVED) {
            reply->valid = true;
            reply->flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
            reply->window = SOCKET_STREAM_BUFFER_SIZE;
            reply->sequence = existing_private->tcp_initial_sequence;
            reply->acknowledgement = sequence + 1U;
            socket_unlock(&existing_socket->lock);
            object_put(existing_socket);
            object_put(listener);
            return K_OK;
        }
        socket_unlock(&existing_socket->lock);
        object_put(existing_socket);
    }
    socket_lock(&listener->lock);
    if (!listener_private->listening ||
        atomic_load_explicit(&listener_private->closed, memory_order_acquire) ||
        atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) >=
            SOCKET_LISTEN_BACKLOG) {
        socket_unlock(&listener->lock);
        object_put(listener);
        return K_EAGAIN;
    }
    socket_unlock(&listener->lock);
    socket_lock(&g_binding_lock);
    bool available = socket_tcp6_slots_available_locked(1U);
    socket_unlock(&g_binding_lock);
    if (!available || socket_allocate(OS_AF_INET6, OS_SOCK_STREAM,
                                      SOCKET_PROTOCOL_TCP, &child) != K_OK) {
        object_put(listener);
        return K_ENOMEM;
    }
    child_private = socket_private(child);
    child_private->bound = true;
    child_private->connected = true;
    socket_copy(child_private->local_address6, destination_address, 16U);
    child_private->local_port = destination_port;
    socket_copy(child_private->peer_address6, source_address, 16U);
    child_private->peer_port = source_port;
    child_private->tcp_peer_initial_sequence = sequence;
    child_private->tcp_initial_sequence = 0x11000000U + source_port;
    child_private->tcp_next_sequence = child_private->tcp_initial_sequence + 1U;
    child_private->tcp_send_next_sequence = child_private->tcp_initial_sequence + 1U;
    child_private->tcp_send_unacknowledged = child_private->tcp_initial_sequence;
    child_private->tcp_send_length = 0U;
    child_private->tcp_send_flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
    child_private->tcp_send_pending = true;
    child_private->tcp_send_retransmits = 0U;
    child_private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
    child_private->tcp_peer_window = SOCKET_STREAM_BUFFER_SIZE;
    child_private->tcp_sequence_valid = true;
    child_private->tcp_state = SOCKET_TCP_SYN_RECEIVED;
    child_private->peer = 0;
    child_private->listener = listener;
    object_get(listener);

    socket_lock(&listener->lock);
    if (atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) >=
        SOCKET_LISTEN_BACKLOG) {
        socket_unlock(&listener->lock);
        socket_abort_child(child);
        object_put(listener);
        return K_EAGAIN;
    }
    listener_private->accept_queue[listener_private->accept_write] = child;
    listener_private->accept_write =
        (listener_private->accept_write + 1U) % SOCKET_LISTEN_BACKLOG;
    atomic_fetch_add_explicit(&listener_private->accept_count, 1U, memory_order_release);
    socket_unlock(&listener->lock);
    (void)wake_all(&listener->waitq);
    object_put(listener);

    socket_lock(&g_binding_lock);
    bool registered = socket_register_tcp_locked(child, child_private);
    socket_unlock(&g_binding_lock);
    if (!registered) {
        socket_abort_child(child);
        return K_ENOMEM;
    }
    reply->valid = true;
    reply->flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
    reply->window = SOCKET_STREAM_BUFFER_SIZE;
    reply->sequence = child_private->tcp_initial_sequence;
    reply->acknowledgement = sequence + 1U;
    return K_OK;
}

kstatus_t socket_inject_tcp_ack_ipv4(uint32_t source_address, uint16_t source_port,
                                     uint32_t destination_address,
                                     uint16_t destination_port, uint32_t sequence,
                                     uint32_t acknowledgement, uint8_t flags) {
    return socket_inject_tcp_ack_ipv4_window(source_address, source_port,
                                             destination_address, destination_port,
                                             sequence, acknowledgement, flags,
                                             UINT16_MAX);
}

kstatus_t socket_inject_tcp_ack_ipv4_reply(uint32_t source_address,
                                           uint16_t source_port,
                                           uint32_t destination_address,
                                           uint16_t destination_port,
                                           uint32_t sequence,
                                           uint32_t acknowledgement,
                                           uint8_t flags, uint16_t window,
                                           socket_tcp_reply_t *reply) {
    socket_t *target = 0;
    socket_private_t *private;
    bool notify = false;
    if (source_address == 0U || source_port == 0U || destination_address == 0U ||
        destination_port == 0U || (flags & NET_TCP_FLAG_ACK) == 0U) return K_EINVAL;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t connection = socket_find_tcp_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = g_tcp_connections[connection].socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(&g_binding_lock);
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    if (window != UINT16_MAX) private->tcp_peer_window = window;
    if (private->tcp_state == SOCKET_TCP_SYN_RECEIVED &&
        acknowledgement == private->tcp_initial_sequence + 1U &&
        sequence == private->tcp_peer_initial_sequence + 1U) {
        private->tcp_state = SOCKET_TCP_ESTABLISHED;
        private->tcp_sequence_valid = true;
        private->tcp_next_sequence = sequence;
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        if (window == UINT16_MAX) private->tcp_peer_window = SOCKET_STREAM_BUFFER_SIZE;
        socket_t *listener = private->listener;
        private->listener = 0;
        socket_unlock(&target->lock);
        if (listener != 0) {
            (void)wake_all(&listener->waitq);
            object_put(listener);
        }
        object_put(target);
        return K_OK;
    }
    if (private->tcp_state == SOCKET_TCP_SYN_SENT &&
        (flags & NET_TCP_FLAG_SYN) != 0U &&
        acknowledgement == private->tcp_initial_sequence + 1U) {
        private->tcp_peer_initial_sequence = sequence;
        private->tcp_next_sequence = sequence + 1U;
        private->tcp_sequence_valid = true;
        private->tcp_state = SOCKET_TCP_ESTABLISHED;
        private->tcp_peer_window = window;
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        if (reply != 0) {
            reply->valid = true;
            reply->flags = NET_TCP_FLAG_ACK;
            reply->sequence = private->tcp_send_next_sequence;
            reply->acknowledgement = private->tcp_next_sequence;
            reply->window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE -
                atomic_load_explicit(&private->stream_count, memory_order_relaxed));
        }
        socket_unlock(&target->lock);
        (void)wake_all(&target->waitq);
        object_put(target);
        return K_OK;
    }
    if (private->tcp_state == SOCKET_TCP_ESTABLISHED &&
        private->tcp_send_pending &&
        acknowledgement >= private->tcp_send_next_sequence) {
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_unacknowledged = acknowledgement;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        notify = true;
    }
    bool established = private->tcp_state == SOCKET_TCP_ESTABLISHED;
    socket_unlock(&target->lock);
    if (notify) (void)wake_all(&target->waitq);
    object_put(target);
    return established ? K_OK : K_EAGAIN;
}

kstatus_t socket_inject_tcp_ack_ipv6_reply(const uint8_t source_address[16],
                                           uint16_t source_port,
                                           const uint8_t destination_address[16],
                                           uint16_t destination_port,
                                           uint32_t sequence,
                                           uint32_t acknowledgement,
                                           uint8_t flags, uint16_t window,
                                           socket_tcp_reply_t *reply) {
    socket_t *target = 0;
    socket_private_t *private;
    bool notify = false;
    if (source_address == 0 || destination_address == 0 ||
        socket_address6_zero(source_address) || socket_address6_zero(destination_address) ||
        source_port == 0U || destination_port == 0U ||
        (flags & NET_TCP_FLAG_ACK) == 0U) return K_EINVAL;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t connection = socket_find_tcp6_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = g_tcp6_connections[connection].socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(&g_binding_lock);
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    if (window != UINT16_MAX) private->tcp_peer_window = window;
    if (private->tcp_state == SOCKET_TCP_SYN_RECEIVED &&
        acknowledgement == private->tcp_initial_sequence + 1U &&
        sequence == private->tcp_peer_initial_sequence + 1U) {
        private->tcp_state = SOCKET_TCP_ESTABLISHED;
        private->tcp_sequence_valid = true;
        private->tcp_next_sequence = sequence;
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        if (window == UINT16_MAX) private->tcp_peer_window = SOCKET_STREAM_BUFFER_SIZE;
        socket_t *listener = private->listener;
        private->listener = 0;
        socket_unlock(&target->lock);
        if (listener != 0) {
            (void)wake_all(&listener->waitq);
            object_put(listener);
        }
        object_put(target);
        return K_OK;
    }
    if (private->tcp_state == SOCKET_TCP_SYN_SENT &&
        (flags & NET_TCP_FLAG_SYN) != 0U &&
        acknowledgement == private->tcp_initial_sequence + 1U) {
        private->tcp_peer_initial_sequence = sequence;
        private->tcp_next_sequence = sequence + 1U;
        private->tcp_sequence_valid = true;
        private->tcp_state = SOCKET_TCP_ESTABLISHED;
        private->tcp_peer_window = window;
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        if (reply != 0) {
            reply->valid = true;
            reply->flags = NET_TCP_FLAG_ACK;
            reply->sequence = private->tcp_send_next_sequence;
            reply->acknowledgement = private->tcp_next_sequence;
            reply->window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE -
                atomic_load_explicit(&private->stream_count, memory_order_relaxed));
        }
        socket_unlock(&target->lock);
        (void)wake_all(&target->waitq);
        object_put(target);
        return K_OK;
    }
    if (private->tcp_state == SOCKET_TCP_ESTABLISHED &&
        private->tcp_send_pending &&
        acknowledgement >= private->tcp_send_next_sequence) {
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_unacknowledged = acknowledgement;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        notify = true;
    }
    bool established = private->tcp_state == SOCKET_TCP_ESTABLISHED;
    socket_unlock(&target->lock);
    if (notify) (void)wake_all(&target->waitq);
    object_put(target);
    return established ? K_OK : K_EAGAIN;
}

kstatus_t socket_inject_tcp_ack_ipv4_window(uint32_t source_address,
                                            uint16_t source_port,
                                            uint32_t destination_address,
                                            uint16_t destination_port,
                                            uint32_t sequence,
                                            uint32_t acknowledgement,
                                            uint8_t flags, uint16_t window) {
    return socket_inject_tcp_ack_ipv4_reply(source_address, source_port,
                                            destination_address, destination_port,
                                            sequence, acknowledgement, flags,
                                            window, 0);
}

kstatus_t socket_inject_tcp_ack_ipv6_window(const uint8_t source_address[16],
                                            uint16_t source_port,
                                            const uint8_t destination_address[16],
                                            uint16_t destination_port,
                                            uint32_t sequence,
                                            uint32_t acknowledgement,
                                            uint8_t flags, uint16_t window) {
    return socket_inject_tcp_ack_ipv6_reply(source_address, source_port,
                                            destination_address, destination_port,
                                            sequence, acknowledgement, flags,
                                            window, 0);
}

static kstatus_t socket_send_stream(socket_t *socket, socket_private_t *private,
                                    const void *buffer, size_t length, uint64_t *bytes) {
    socket_t *peer;
    socket_tcp_ipv4_output_fn output4;
    socket_tcp_ipv6_output_fn output6;
    void *output4_context;
    void *output6_context;
    bool wire_output;
    if (length > SOCKET_STREAM_BUFFER_SIZE) return K_EAGAIN;
    output4 = 0;
    output6 = 0;
    output4_context = 0;
    output6_context = 0;
    socket_tcp_output_get(&output4, &output4_context);
    socket_tcp6_output_get(&output6, &output6_context);
    socket_lock(&socket->lock);

    /*
    * An active TCP connect is asynchronous.  socket_connect() may return
    * after SYN has been queued but before SYN+ACK arrives.
    *
    * This is NOT a removed peer.  Tell Ring3 to retry until the handshake
    * becomes ESTABLISHED.
    */
    if ((socket->family == OS_AF_INET4 ||
        socket->family == OS_AF_INET6) &&
        private->peer == 0 &&
        private->tcp_state != SOCKET_TCP_ESTABLISHED) {

        kstatus_t status;

        if (private->tcp_send_failed) {
            status = K_EIO;
        } else if (private->tcp_state == SOCKET_TCP_SYN_SENT ||
                private->tcp_state == SOCKET_TCP_SYN_RECEIVED) {
            status = K_EAGAIN;
        } else {
            status = K_EDEVREMOVED;
        }

        socket_unlock(&socket->lock);

        *bytes = 0U;
        return status;
    }

    wire_output =
        (socket->family == OS_AF_INET4 ||
        socket->family == OS_AF_INET6) &&
        private->peer == 0 &&
        private->tcp_state == SOCKET_TCP_ESTABLISHED;
    if (wire_output) {
        if (private->tcp_send_pending || private->tcp_send_failed ||
            private->tcp_peer_window == 0U || length > private->tcp_peer_window ||
            length > SOCKET_MAX_PAYLOAD) {
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return private->tcp_send_failed ? K_EIO : K_EAGAIN;
        }
        if ((socket->family == OS_AF_INET4 && output4 == 0) ||
            (socket->family == OS_AF_INET6 && output6 == 0)) {
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return K_EAGAIN;
        }
        private->tcp_send_unacknowledged = private->tcp_send_next_sequence;
        private->tcp_send_next_sequence += (uint32_t)length;
        private->tcp_send_length = (uint16_t)length;
        private->tcp_send_flags = NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH;
        private->tcp_send_pending = true;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
        socket_copy(private->tcp_send_payload, buffer, length);
        uint16_t source_port_wire = private->local_port;
        uint16_t destination_port_wire = private->peer_port;
        uint32_t sequence_wire = private->tcp_send_unacknowledged;
        uint32_t acknowledgement_wire = private->tcp_next_sequence;
        uint16_t window_wire = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE -
            atomic_load_explicit(&private->stream_count, memory_order_relaxed));
        uint32_t source_address_wire = private->local_address;
        uint32_t destination_address_wire = private->peer_address;
        uint8_t source_address6_wire[16];
        uint8_t destination_address6_wire[16];
        socket_copy(source_address6_wire, private->local_address6, 16U);
        socket_copy(destination_address6_wire, private->peer_address6, 16U);
        socket_unlock(&socket->lock);
        kstatus_t output_status;
        if (socket->family == OS_AF_INET4) {
            output_status = output4(output4_context, source_address_wire,
                                    source_port_wire, destination_address_wire,
                                    destination_port_wire, sequence_wire,
                                    acknowledgement_wire, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                                    window_wire, buffer, length);
        } else {
            output_status = output6(output6_context, source_address6_wire,
                                    source_port_wire, destination_address6_wire,
                                    destination_port_wire, sequence_wire,
                                    acknowledgement_wire, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                                    window_wire, buffer, length);
        }
        if (output_status == K_EAGAIN) {
            /* ARP、链路或发送队列暂时不可用时，保留待确认段交给重传轮询。 */
            *bytes = 0U;
            return K_EAGAIN;
        }
        if (output_status != K_OK) {
            socket_lock(&socket->lock);
            private->tcp_send_pending = false;
            private->tcp_send_failed = true;
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return output_status;
        }
        *bytes = length;
        return K_OK;
    }
    peer = private->peer;
    if (peer != 0 && !object_try_get(peer)) peer = 0;
    uint32_t source_address = private->local_address;
    uint16_t source_port = private->local_port;
    socket_unlock(&socket->lock);
    if (peer == 0) {
        *bytes = 0;
        return K_EDEVREMOVED;
    }
    socket_private_t *peer_private = socket_private(peer);
    if (peer_private == 0) {
        object_put(peer);
        *bytes = 0;
        return K_EIO;
    }
    socket_lock(&peer->lock);
    uint32_t count = atomic_load_explicit(&peer_private->stream_count, memory_order_relaxed);
    if (atomic_load_explicit(&peer_private->closed, memory_order_acquire) ||
        count > SOCKET_STREAM_BUFFER_SIZE - length) {
        socket_unlock(&peer->lock);
        object_put(peer);
        *bytes = 0;
        return K_EAGAIN;
    }
    for (size_t index = 0; index < length; ++index) {
        peer_private->stream_buffer[peer_private->stream_write] =
            ((const uint8_t *)buffer)[index];
        peer_private->stream_write =
            (peer_private->stream_write + 1U) % SOCKET_STREAM_BUFFER_SIZE;
    }
    atomic_store_explicit(&peer_private->stream_count, count + (uint32_t)length,
                          memory_order_release);
    /* 发送端点信息作为接收端的稳定来源地址保存。 */
    peer_private->peer_address = source_address;
    peer_private->peer_port = source_port;
    socket_unlock(&peer->lock);
    (void)wake_all(&peer->waitq);
    object_put(peer);
    *bytes = length;
    return K_OK;
}

kstatus_t socket_send(socket_t *socket, const void *buffer, size_t length,
                      uint32_t address, uint16_t port, uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);
    socket_udp_ipv4_output_fn output = 0;
    void *output_context = 0;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4 || private == 0 ||
        bytes == 0 || (buffer == 0 && length != 0) || length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        *bytes = 0U;
        return K_EDEVREMOVED;
    }
    if (socket->type == OS_SOCK_STREAM) {
        return socket_send_stream(socket, private, buffer, length, bytes);
    }
    socket_lock(&socket->lock);
    if (address == 0U && port == 0U && private->connected) {
        address = private->peer_address;
        port = private->peer_port;
    }
    uint32_t source_address = private->bound ? private->local_address : 0x7F000001U;
    uint16_t source_port = private->bound ? private->local_port : 0U;
    socket_unlock(&socket->lock);
    if (source_port == 0U) {
        *bytes = 0;
        return K_EINVAL;
    }
    /* 回环流量由协议栈本地投递，不能因为网卡安装了输出回调而绕到物理层。 */
    if (address == 0x7F000001U || address == source_address) {
        kstatus_t status = socket_inject_udp(source_address, source_port,
                                             address, port, buffer, length);
        *bytes = status == K_OK ? length : 0U;
        return status;
    }
    socket_udp4_output_get(&output, &output_context);
    if (output != 0) {
        kstatus_t output_status = output(output_context, source_address, source_port,
                                         address, port, buffer, length);
        *bytes = output_status == K_OK ? length : 0U;
        return output_status;
    }
    kstatus_t status = socket_inject_udp(source_address, source_port, address, port,
                                         buffer, length);
    *bytes = status == K_OK ? length : 0;
    return status;
}

kstatus_t socket_send_ipv6(socket_t *socket, const void *buffer, size_t length,
                           const uint8_t address[16], uint16_t port, uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);
    socket_udp_ipv6_output_fn output = 0;
    void *output_context = 0;
    uint8_t destination[16];
    uint16_t destination_port = port;
    static const uint8_t loopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1};
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || private == 0 || bytes == 0 ||
        (buffer == 0 && length != 0U) || length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        *bytes = 0U;
        return K_EDEVREMOVED;
    }
    if (socket->type == OS_SOCK_STREAM) {
        if ((address != 0 && port != 0U) &&
            (!private->connected || !socket_address6_equal(address, private->peer_address6) ||
             port != private->peer_port)) {
            *bytes = 0U;
            return K_EINVAL;
        }
        return socket_send_stream(socket, private, buffer, length, bytes);
    }
    socket_lock(&socket->lock);
    if (address == 0 && port == 0U && private->connected) {
        socket_copy(destination, private->peer_address6, 16U);
        destination_port = private->peer_port;
    } else if (address != 0 && port != 0U) {
        socket_copy(destination, address, 16U);
    } else {
        socket_unlock(&socket->lock);
        *bytes = 0U;
        return K_EINVAL;
    }
    uint8_t source[16];
    if (private->bound) socket_copy(source, private->local_address6, 16U);
    else socket_copy(source, loopback6, 16U);
    uint16_t source_port = private->bound ? private->local_port : 0U;
    socket_unlock(&socket->lock);
    if (source_port == 0U) {
        *bytes = 0U;
        return K_EINVAL;
    }
    /* IPv6 loop回环以及发往本机地址的数据同样只在本地协议栈中传递。 */
    if (socket_address6_equal(destination, loopback6) ||
        socket_address6_equal(destination, source)) {
        kstatus_t status = socket_inject_udp_ipv6(source, source_port, destination,
                                                  destination_port, buffer, length);
        *bytes = status == K_OK ? length : 0U;
        return status;
    }
    socket_udp6_output_get(&output, &output_context);
    if (output != 0) {
        kstatus_t output_status = output(output_context, source, source_port,
                                         destination, destination_port,
                                         buffer, length);
        *bytes = output_status == K_OK ? length : 0U;
        return output_status;
    }
    kstatus_t status = socket_inject_udp_ipv6(source, source_port, destination,
                                              destination_port, buffer, length);
    *bytes = status == K_OK ? length : 0U;
    return status;
}

static void socket_async_send_work(void *argument) {
    socket_async_send_t *request = (socket_async_send_t *)argument;
    os_completion_entry_t completion = {0};
    uint64_t bytes = 0U;
    kstatus_t status;
    if (request == 0) return;
    status = request->ipv6 ?
        socket_send_ipv6(request->socket, request->payload, request->length,
                         request->address6, request->port, &bytes) :
        socket_send(request->socket, request->payload, request->length,
                    request->address, request->port, &bytes);
    completion.user_key = request->user_key;
    completion.status = status;
    completion.bytes_done = bytes;
    completion.request_id = request->request_id;
    (void)completion_port_post(request->completion_port, &completion);
    object_put(request->completion_port);
    object_put(request->socket);
    kfree(request);
}

kstatus_t socket_send_async(socket_t *socket, const void *buffer, size_t length,
                            uint32_t address, uint16_t port,
                            struct completion_port *completion_port,
                            uint64_t user_key, uint64_t *request_id) {
    socket_async_send_t *request;
    socket_private_t *private;
    bool socket_ref;
    bool port_ref;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        (buffer == 0 && length != 0U) || length > SOCKET_MAX_PAYLOAD ||
        completion_port == 0 || request_id == 0 ||
        ((completion_port_t *)completion_port)->object.type !=
            KOBJECT_TYPE_COMPLETION_PORT) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0 || atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    request = (socket_async_send_t *)kzalloc(sizeof(*request), 0);
    if (request == 0) return K_ENOMEM;
    socket_ref = object_try_get(socket);
    port_ref = object_try_get(completion_port);
    if (!socket_ref || !port_ref) {
        if (port_ref) object_put(completion_port);
        if (socket_ref) object_put(socket);
        kfree(request);
        return K_ECANCELED;
    }
    request->socket = socket;
    request->completion_port = (completion_port_t *)completion_port;
    request->length = length;
    request->address = address;
    request->port = port;
    request->user_key = user_key;
    request->request_id = atomic_fetch_add_explicit(&g_next_async_request_id, 1U,
                                                    memory_order_relaxed) + 1U;
    for (size_t i = 0U; i < length; ++i) request->payload[i] = ((const uint8_t *)buffer)[i];
    if (!deferred_schedule(socket_async_send_work, request)) {
        object_put(completion_port);
        object_put(socket);
        kfree(request);
        return K_EBUSY;
    }
    *request_id = request->request_id;
    return K_OK;
}

kstatus_t socket_send_async_ipv6(socket_t *socket, const void *buffer, size_t length,
                                 const uint8_t address[16], uint16_t port,
                                 struct completion_port *completion_port,
                                 uint64_t user_key, uint64_t *request_id) {
    socket_async_send_t *request;
    socket_private_t *private;
    bool socket_ref;
    bool port_ref;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || socket->type != OS_SOCK_DGRAM ||
        address == 0 || port == 0U || (buffer == 0 && length != 0U) ||
        length > SOCKET_MAX_PAYLOAD || completion_port == 0 || request_id == 0 ||
        ((completion_port_t *)completion_port)->object.type != KOBJECT_TYPE_COMPLETION_PORT) {
        return K_EINVAL;
    }
    private = socket_private(socket);
    if (private == 0 || atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    request = (socket_async_send_t *)kzalloc(sizeof(*request), 0);
    if (request == 0) return K_ENOMEM;
    socket_ref = object_try_get(socket);
    port_ref = object_try_get(completion_port);
    if (!socket_ref || !port_ref) {
        if (port_ref) object_put(completion_port);
        if (socket_ref) object_put(socket);
        kfree(request);
        return K_ECANCELED;
    }
    request->socket = socket;
    request->completion_port = (completion_port_t *)completion_port;
    request->length = length;
    request->ipv6 = true;
    socket_copy(request->address6, address, 16U);
    request->port = port;
    request->user_key = user_key;
    request->request_id = atomic_fetch_add_explicit(&g_next_async_request_id, 1U,
                                                    memory_order_relaxed) + 1U;
    for (size_t index = 0U; index < length; ++index) {
        request->payload[index] = ((const uint8_t *)buffer)[index];
    }
    if (!deferred_schedule(socket_async_send_work, request)) {
        object_put(completion_port);
        object_put(socket);
        kfree(request);
        return K_EBUSY;
    }
    *request_id = request->request_id;
    return K_OK;
}

static bool socket_receive_ready(void *context) {
    socket_wait_context_t *wait = (socket_wait_context_t *)context;
    if (wait == 0 || wait->private == 0) return false;
    return atomic_load_explicit(&wait->private->datagram_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&wait->private->stream_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&wait->private->closed, memory_order_acquire) ||
           atomic_load_explicit(&wait->private->peer_closed, memory_order_acquire);
}

kstatus_t socket_recv(socket_t *socket, void *buffer, size_t length,
                      socket_ipv4_endpoint_t *source, uint64_t timeout_ns,
                      uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4 || private == 0 ||
        source == 0 || bytes == 0 || (buffer == 0 && length != 0)) return K_EINVAL;
    if (socket->type == OS_SOCK_STREAM && length > SOCKET_STREAM_BUFFER_SIZE) {
        length = SOCKET_STREAM_BUFFER_SIZE;
    }
    socket_wait_context_t wait = {.socket = socket, .private = private};
    for (;;) {
        kstatus_t status = wait_on_queue(&socket->waitq, socket_receive_ready,
                                         &wait, timeout_ns);
        if (status != K_OK) return status;
        socket_lock(&socket->lock);
        if (socket->type == OS_SOCK_STREAM) {
            uint32_t count = atomic_load_explicit(&private->stream_count, memory_order_relaxed);
            if (count != 0U) {
                size_t copy_length = count < length ? count : length;
                for (size_t index = 0; index < copy_length; ++index) {
                    ((uint8_t *)buffer)[index] = private->stream_buffer[private->stream_read];
                    private->stream_read =
                        (private->stream_read + 1U) % SOCKET_STREAM_BUFFER_SIZE;
                }
                source->address = private->peer_address;
                source->port = private->peer_port;
                source->reserved = 0;
                *bytes = copy_length;
                uint32_t new_count = count - (uint32_t)copy_length;
                bool window_update =
                    private->peer == 0 &&
                    private->tcp_state == SOCKET_TCP_ESTABLISHED &&
                    private->tcp_sequence_valid &&
                    count >= (SOCKET_STREAM_BUFFER_SIZE * 3U / 4U);
                uint32_t update_source = private->local_address;
                uint16_t update_source_port = private->local_port;
                uint32_t update_destination = private->peer_address;
                uint16_t update_destination_port = private->peer_port;
                uint32_t update_sequence = private->tcp_send_next_sequence;
                uint32_t update_acknowledgement = private->tcp_next_sequence;
                uint16_t update_window =
                    (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - new_count);
                atomic_store_explicit(&private->stream_count, new_count,
                                      memory_order_release);
                socket_unlock(&socket->lock);

                /* WGET/TCP receive-window reopening: a peer that observed a
                 * near-zero advertised window must be told when userspace
                 * drains the 8 KiB stream ring.  Keep the ACK outside the
                 * socket lock; the NIC output path may perform ARP/NDP work. */
                if (window_update) {
                    socket_tcp_ipv4_output_fn output = 0;
                    void *output_context = 0;
                    socket_tcp_output_get(&output, &output_context);
                    if (output != 0) {
                        (void)output(output_context, update_source,
                                     update_source_port, update_destination,
                                     update_destination_port, update_sequence,
                                     update_acknowledgement, NET_TCP_FLAG_ACK,
                                     update_window, 0, 0U);
                    }
                }
                return K_OK;
            }
            bool closed = atomic_load_explicit(&private->closed, memory_order_acquire) ||
                          atomic_load_explicit(&private->peer_closed, memory_order_acquire);
            socket_unlock(&socket->lock);
            if (closed) {
                *bytes = 0;
                source->address = private->peer_address;
                source->port = private->peer_port;
                source->reserved = 0;
                return K_OK; /* TCP EOF：没有剩余字节，但不是错误。 */
            }
            continue;
        }

        uint32_t count = atomic_load_explicit(&private->datagram_count, memory_order_relaxed);
        if (count != 0U) {
            socket_datagram_t *datagram = &private->datagrams[private->datagram_read];
            uint16_t datagram_length = datagram->length;
            size_t copy_length = datagram_length > length ? length : datagram_length;
            if (copy_length != 0) socket_copy(buffer, datagram->payload, copy_length);
            source->address = datagram->source_address;
            source->port = datagram->source_port;
            source->reserved = 0;
            *bytes = copy_length;
            private->datagram_read = (private->datagram_read + 1U) % SOCKET_RX_QUEUE_DEPTH;
            atomic_store_explicit(&private->datagram_count, count - 1U, memory_order_release);
            socket_unlock(&socket->lock);
            return datagram_length > length ? K_EAGAIN : K_OK;
        }
        bool closed = atomic_load_explicit(&private->closed, memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}

kstatus_t socket_recv_ipv6(socket_t *socket, void *buffer, size_t length,
                           socket_ipv6_endpoint_t *source, uint64_t timeout_ns,
                           uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET || private == 0 ||
        socket->family != OS_AF_INET6 ||
        source == 0 || bytes == 0 || (buffer == 0 && length != 0U)) return K_EINVAL;
    if (socket->type == OS_SOCK_STREAM && length > SOCKET_STREAM_BUFFER_SIZE) {
        length = SOCKET_STREAM_BUFFER_SIZE;
    }
    socket_wait_context_t wait = {.socket = socket, .private = private};
    for (;;) {
        kstatus_t status = wait_on_queue(&socket->waitq, socket_receive_ready,
                                         &wait, timeout_ns);
        if (status != K_OK) return status;
        socket_lock(&socket->lock);
        if (socket->type == OS_SOCK_STREAM) {
            uint32_t stream_count = atomic_load_explicit(&private->stream_count,
                                                         memory_order_relaxed);
            if (stream_count != 0U) {
                size_t copy_length = stream_count < length ? stream_count : length;
                for (size_t index = 0U; index < copy_length; ++index) {
                    ((uint8_t *)buffer)[index] = private->stream_buffer[private->stream_read];
                    private->stream_read =
                        (private->stream_read + 1U) % SOCKET_STREAM_BUFFER_SIZE;
                }
                socket_copy(source->address, private->peer_address6, 16U);
                source->port = private->peer_port;
                source->reserved = 0U;
                *bytes = copy_length;
                uint32_t new_count = stream_count - (uint32_t)copy_length;
                bool window_update =
                    private->peer == 0 &&
                    private->tcp_state == SOCKET_TCP_ESTABLISHED &&
                    private->tcp_sequence_valid &&
                    stream_count >= (SOCKET_STREAM_BUFFER_SIZE * 3U / 4U);
                uint8_t update_source[16];
                uint8_t update_destination[16];
                socket_copy(update_source, private->local_address6, 16U);
                socket_copy(update_destination, private->peer_address6, 16U);
                uint16_t update_source_port = private->local_port;
                uint16_t update_destination_port = private->peer_port;
                uint32_t update_sequence = private->tcp_send_next_sequence;
                uint32_t update_acknowledgement = private->tcp_next_sequence;
                uint16_t update_window =
                    (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - new_count);
                atomic_store_explicit(&private->stream_count, new_count,
                                      memory_order_release);
                socket_unlock(&socket->lock);

                /* WGET/TCP receive-window reopening (IPv6 mirror). */
                if (window_update) {
                    socket_tcp_ipv6_output_fn output = 0;
                    void *output_context = 0;
                    socket_tcp6_output_get(&output, &output_context);
                    if (output != 0) {
                        (void)output(output_context, update_source,
                                     update_source_port, update_destination,
                                     update_destination_port, update_sequence,
                                     update_acknowledgement, NET_TCP_FLAG_ACK,
                                     update_window, 0, 0U);
                    }
                }
                return K_OK;
            }
            bool stream_closed = atomic_load_explicit(&private->closed, memory_order_acquire) ||
                                 atomic_load_explicit(&private->peer_closed,
                                                      memory_order_acquire);
            socket_unlock(&socket->lock);
            if (stream_closed) {
                socket_copy(source->address, private->peer_address6, 16U);
                source->port = private->peer_port;
                source->reserved = 0U;
                *bytes = 0U;
                return K_OK;
            }
            continue;
        }
        uint32_t count = atomic_load_explicit(&private->datagram_count, memory_order_relaxed);
        if (count != 0U) {
            socket_datagram_t *datagram = &private->datagrams[private->datagram_read];
            uint16_t datagram_length = datagram->length;
            size_t copy_length = datagram_length > length ? length : datagram_length;
            if (datagram->family != OS_AF_INET6) {
                socket_unlock(&socket->lock);
                return K_EIO;
            }
            if (copy_length != 0U) socket_copy(buffer, datagram->payload, copy_length);
            socket_copy(source->address, datagram->source_address6, 16U);
            source->port = datagram->source_port;
            source->reserved = 0U;
            *bytes = copy_length;
            private->datagram_read = (private->datagram_read + 1U) % SOCKET_RX_QUEUE_DEPTH;
            atomic_store_explicit(&private->datagram_count, count - 1U, memory_order_release);
            socket_unlock(&socket->lock);
            return datagram_length > length ? K_EAGAIN : K_OK;
        }
        bool closed = atomic_load_explicit(&private->closed, memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}

static bool socket_check_payload(const uint8_t *actual, const uint8_t *expected, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        if (actual[index] != expected[index]) return false;
    }
    return true;
}

typedef struct socket_test_tcp_output {
    uint32_t count;
    uint32_t source_address;
    uint16_t source_port;
    uint32_t destination_address;
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t flags;
    uint16_t window;
    size_t payload_length;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
} socket_test_tcp_output_t;

typedef struct socket_test_tcp6_output {
    uint32_t count;
    uint8_t source_address[16];
    uint16_t source_port;
    uint8_t destination_address[16];
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t flags;
    uint16_t window;
    size_t payload_length;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
} socket_test_tcp6_output_t;

static kstatus_t socket_test_tcp_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length) {
    socket_test_tcp_output_t *capture = (socket_test_tcp_output_t *)context;
    if (capture == 0 || payload_length > sizeof(capture->payload) ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;
    ++capture->count;
    capture->source_address = source_address;
    capture->source_port = source_port;
    capture->destination_address = destination_address;
    capture->destination_port = destination_port;
    capture->sequence = sequence;
    capture->acknowledgement = acknowledgement;
    capture->flags = flags;
    capture->window = window;
    capture->payload_length = payload_length;
    if (payload_length != 0U) socket_copy(capture->payload, payload, payload_length);
    return K_OK;
}

static kstatus_t socket_test_tcp6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length) {
    socket_test_tcp6_output_t *capture = (socket_test_tcp6_output_t *)context;
    if (capture == 0 || source_address == 0 || destination_address == 0 ||
        payload_length > sizeof(capture->payload) ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;
    ++capture->count;
    socket_copy(capture->source_address, source_address, 16U);
    capture->source_port = source_port;
    socket_copy(capture->destination_address, destination_address, 16U);
    capture->destination_port = destination_port;
    capture->sequence = sequence;
    capture->acknowledgement = acknowledgement;
    capture->flags = flags;
    capture->window = window;
    capture->payload_length = payload_length;
    if (payload_length != 0U) socket_copy(capture->payload, payload, payload_length);
    return K_OK;
}

bool socket_self_test(void) {
    socket_t *sender = 0;
    socket_t *receiver = 0;
    socket_t *sender6 = 0;
    socket_t *receiver6 = 0;
    socket_t *chaos_receiver = 0;
    socket_t *listener = 0;
    socket_t *client = 0;
    socket_t *accepted = 0;
    socket_t *wire_listener = 0;
    socket_t *wire_accepted = 0;
    socket_t *active_client = 0;
    socket_t *active_client6 = 0;
    completion_port_t *completion_port = 0;
    const uint8_t udp_payload[] = {'s', 'o', 'c', 'k', 'e', 't'};
    const uint8_t tcp_payload[] = {'t', 'c', 'p'};
    const uint8_t tcp_reply[] = {'a', 'c', 'k'};
    uint8_t received[sizeof(udp_payload)] = {0};
    uint8_t long_payload[SOCKET_LONG_TEST_CHUNK];
    uint8_t long_received[SOCKET_LONG_TEST_CHUNK];
    static const uint8_t reordered_datagrams[] = {3U, 1U, 1U, 4U};
    static const uint8_t loopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1};
    socket_ipv4_endpoint_t source = {0};
    socket_ipv6_endpoint_t source6 = {0};
    uint64_t bytes = 0;
    bool success = false;
    socket_tcp_reply_t wire_reply = {0};
    socket_tcp_reply_t wire_data_reply = {0};
    socket_tcp_reply_t active_reply = {0};
    socket_tcp_reply_t active_reply6 = {0};
    socket_tcp_ipv4_output_fn saved_tcp_output = 0;
    void *saved_tcp_output_context = 0;
    socket_tcp_ipv6_output_fn saved_tcp6_output = 0;
    void *saved_tcp6_output_context = 0;
    socket_test_tcp_output_t captured_tcp_output;
    socket_test_tcp6_output_t captured_tcp6_output;
    bool tcp_output_installed = false;
    bool tcp6_output_installed = false;
    socket_zero(&captured_tcp_output, sizeof(captured_tcp_output));
    socket_zero(&captured_tcp6_output, sizeof(captured_tcp6_output));

    if (socket_create(OS_AF_INET4, OS_SOCK_DGRAM, 0, &sender) != K_OK ||
        socket_create(OS_AF_INET4, OS_SOCK_DGRAM, 0, &receiver) != K_OK ||
        socket_bind(sender, 0x7F000001U, 10000U) != K_OK ||
        socket_bind(receiver, 0x7F000001U, 10001U) != K_OK ||
        socket_connect(sender, 0x7F000001U, 10001U) != K_OK ||
        socket_send(sender, udp_payload, sizeof(udp_payload), 0, 0, &bytes) != K_OK ||
        bytes != sizeof(udp_payload) ||
        socket_recv(receiver, received, sizeof(received), &source, 1000000000ULL,
                    &bytes) != K_OK || bytes != sizeof(udp_payload) ||
        source.address != 0x7F000001U || source.port != 10000U ||
        !socket_check_payload(received, udp_payload, sizeof(udp_payload))) goto cleanup;

    /* IPv6 UDP loopback：验证 128 位地址不会被截断或按 IPv4 规则匹配。 */
    if (socket_create(OS_AF_INET6, OS_SOCK_DGRAM, 0, &sender6) != K_OK ||
        socket_create(OS_AF_INET6, OS_SOCK_DGRAM, 0, &receiver6) != K_OK ||
        socket_bind_ipv6(sender6, loopback6, 10010U) != K_OK ||
        socket_bind_ipv6(receiver6, loopback6, 10011U) != K_OK ||
        socket_connect_ipv6(sender6, loopback6, 10011U) != K_OK ||
        socket_send_ipv6(sender6, udp_payload, sizeof(udp_payload), 0, 0U, &bytes) != K_OK ||
        bytes != sizeof(udp_payload) ||
        socket_recv_ipv6(receiver6, received, sizeof(received), &source6,
                         1000000000ULL, &bytes) != K_OK || bytes != sizeof(udp_payload) ||
        !socket_address6_equal(source6.address, loopback6) || source6.port != 10010U ||
        !socket_check_payload(received, udp_payload, sizeof(udp_payload))) goto cleanup;

    /*
     * 通过注入器构造“丢失 2、乱序 3/1、重复 1”的序列。
     * socket 层必须保持到达顺序和重复包，未注入的序号不能凭空出现。
     */
    if (socket_create(OS_AF_INET4, OS_SOCK_DGRAM, 0, &chaos_receiver) != K_OK ||
        socket_bind(chaos_receiver, 0x7F000001U, 10002U) != K_OK) goto cleanup;
    for (size_t index = 0U; index < sizeof(reordered_datagrams); ++index) {
        uint8_t value = reordered_datagrams[index];
        if (socket_inject_udp(0x7F000001U, 15000U, 0x7F000001U, 10002U,
                              &value, sizeof(value)) != K_OK) goto cleanup;
    }
    for (size_t index = 0U; index < sizeof(reordered_datagrams); ++index) {
        if (socket_recv(chaos_receiver, received, sizeof(uint8_t), &source, 0U,
                        &bytes) != K_OK || bytes != sizeof(uint8_t) ||
            source.address != 0x7F000001U || source.port != 15000U ||
            received[0] != reordered_datagrams[index]) goto cleanup;
    }
    /* 队列已经耗尽；2 的丢失不能被错误地报告为一个额外数据报。 */
    if (socket_recv(chaos_receiver, received, sizeof(uint8_t), &source, 0U,
                    &bytes) != K_ETIMEDOUT) goto cleanup;

    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &listener) != K_OK ||
        socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &client) != K_OK ||
        socket_bind(listener, 0x7F000001U, 13000U) != K_OK ||
        socket_listen(listener, 4U) != K_OK ||
        socket_bind(client, 0x7F000001U, 13001U) != K_OK ||
        socket_connect(client, 0x7F000001U, 13000U) != K_OK ||
        socket_accept(listener, 1000000000ULL, &accepted) != K_OK || accepted == 0 ||
        socket_send(client, tcp_payload, sizeof(tcp_payload), 0, 0, &bytes) != K_OK ||
        bytes != sizeof(tcp_payload) ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 1000000000ULL,
                    &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        source.address != 0x7F000001U || source.port != 13001U ||
        !socket_check_payload(received, tcp_payload, sizeof(tcp_payload)) ||
        socket_send(accepted, tcp_reply, sizeof(tcp_reply), 0, 0, &bytes) != K_OK ||
        bytes != sizeof(tcp_reply) ||
        socket_recv(client, received, sizeof(tcp_reply), &source, 1000000000ULL,
                    &bytes) != K_OK || bytes != sizeof(tcp_reply) ||
        !socket_check_payload(received, tcp_reply, sizeof(tcp_reply))) goto cleanup;

    /*
     * 每批先填满 8 KiB 对端流缓冲，再按相同分片读回；总量为 64 KiB，
     * 用来覆盖环形缓冲绕回、连续发送和连续接收的长传输路径。
     */
    for (uint32_t round = 0U; round < SOCKET_LONG_TEST_ROUNDS; ++round) {
        for (uint32_t chunk = 0U; chunk < SOCKET_LONG_TEST_BATCH; ++chunk) {
            uint64_t base = ((uint64_t)round * SOCKET_LONG_TEST_BATCH + chunk) *
                            SOCKET_LONG_TEST_CHUNK;
            for (size_t index = 0U; index < sizeof(long_payload); ++index) {
                long_payload[index] = (uint8_t)((base + index) & 0xFFU);
            }
            if (socket_send(client, long_payload, sizeof(long_payload), 0U, 0U,
                            &bytes) != K_OK || bytes != sizeof(long_payload)) {
                goto cleanup;
            }
        }
        for (uint32_t chunk = 0U; chunk < SOCKET_LONG_TEST_BATCH; ++chunk) {
            uint64_t base = ((uint64_t)round * SOCKET_LONG_TEST_BATCH + chunk) *
                            SOCKET_LONG_TEST_CHUNK;
            if (socket_recv(accepted, long_received, sizeof(long_received), &source,
                            0U, &bytes) != K_OK || bytes != sizeof(long_received) ||
                source.address != 0x7F000001U || source.port != 13001U) goto cleanup;
            for (size_t index = 0U; index < sizeof(long_received); ++index) {
                if (long_received[index] != (uint8_t)((base + index) & 0xFFU)) {
                    goto cleanup;
                }
            }
        }
    }

    /* 关闭 socket 后再运行异步任务，完成项必须仍然恰好投递一次。 */
    /* TCP wire RX 边界：连续序号、重复/乱序包和 FIN EOF。 */
    if (socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               700U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                               tcp_payload, sizeof(tcp_payload)) != K_OK ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 0U, &bytes) != K_OK ||
        bytes != sizeof(tcp_payload) || source.port != 13001U ||
        !socket_check_payload(received, tcp_payload, sizeof(tcp_payload)) ||
        socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               700U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                               tcp_payload, sizeof(tcp_payload)) != K_OK ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 0U, &bytes) !=
            K_ETIMEDOUT ||
        socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               704U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                               tcp_payload, sizeof(tcp_payload)) != K_EAGAIN ||
        socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               703U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN,
                               0, 0U) != K_OK ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 0U, &bytes) != K_OK ||
        bytes != 0U) goto cleanup;

    /* 真正的被动握手：SYN 只创建半连接，第三次握手 ACK 后才允许 accept。 */
    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &wire_listener) != K_OK ||
        socket_bind(wire_listener, 0x7F000001U, 15000U) != K_OK ||
        socket_listen(wire_listener, 2U) != K_OK ||
        socket_inject_tcp_syn_ipv4(0x7F000001U, 15001U, 0x7F000001U, 15000U,
                                   900U, &wire_reply) != K_OK || !wire_reply.valid ||
        (wire_reply.flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) !=
            (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK) ||
        socket_accept(wire_listener, 0U, &wire_accepted) != K_ETIMEDOUT ||
        socket_inject_tcp_ack_ipv4(0x7F000001U, 15001U, 0x7F000001U, 15000U,
                                   901U, wire_reply.sequence + 1U,
                                   NET_TCP_FLAG_ACK) != K_OK ||
        socket_accept(wire_listener, 1000000000ULL, &wire_accepted) != K_OK ||
        socket_inject_tcp_ipv4_reply(0x7F000001U, 15001U, 0x7F000001U, 15000U,
                                     901U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                                     tcp_payload, sizeof(tcp_payload),
                                     &wire_data_reply) != K_OK ||
        !wire_data_reply.valid || wire_data_reply.flags != NET_TCP_FLAG_ACK ||
        wire_data_reply.acknowledgement != 904U || wire_data_reply.window !=
            SOCKET_STREAM_BUFFER_SIZE - sizeof(tcp_payload) ||
        socket_recv(wire_accepted, received, sizeof(tcp_payload), &source,
                    1000000000ULL, &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        !socket_check_payload(received, tcp_payload, sizeof(tcp_payload))) goto cleanup;

    /* 主动 TCP 连接必须走真实的 SYN/SYN-ACK/ACK 状态机，并具备重传能力。 */
    socket_tcp_output_get(&saved_tcp_output, &saved_tcp_output_context);
    socket_set_tcp_ipv4_output(socket_test_tcp_output, &captured_tcp_output);
    tcp_output_installed = true;
    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &active_client) != K_OK ||
        socket_connect(active_client, 0x7F000002U, 16000U) != K_OK ||
        captured_tcp_output.count != 1U ||
        captured_tcp_output.flags != NET_TCP_FLAG_SYN ||
        captured_tcp_output.payload_length != 0U ||
        socket_inject_tcp_ack_ipv4_reply(
            captured_tcp_output.destination_address,
            captured_tcp_output.destination_port,
            captured_tcp_output.source_address,
            captured_tcp_output.source_port,
            400U, captured_tcp_output.sequence + 1U,
            NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK, 4096U, &active_reply) != K_OK ||
        !active_reply.valid || active_reply.flags != NET_TCP_FLAG_ACK ||
        active_reply.acknowledgement != 401U ||
        captured_tcp_output.source_port == 0U) goto cleanup;
    socket_zero(&captured_tcp_output, sizeof(captured_tcp_output));
    if (socket_send(active_client, tcp_payload, sizeof(tcp_payload), 0U, 0U,
                    &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        captured_tcp_output.count != 1U ||
        captured_tcp_output.flags != (NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH) ||
        captured_tcp_output.payload_length != sizeof(tcp_payload) ||
        !socket_check_payload(captured_tcp_output.payload, tcp_payload,
                              sizeof(tcp_payload)) ||
        socket_inject_tcp_ack_ipv4_window(
            0x7F000002U, 16000U, captured_tcp_output.source_address,
            captured_tcp_output.source_port, 401U,
            captured_tcp_output.sequence + (uint32_t)sizeof(tcp_payload),
            NET_TCP_FLAG_ACK, 4096U) != K_OK) goto cleanup;
    socket_zero(&captured_tcp_output, sizeof(captured_tcp_output));
    if (socket_send(active_client, tcp_payload, sizeof(tcp_payload), 0U, 0U,
                    &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        captured_tcp_output.count != 1U) goto cleanup;
    uint32_t retransmit_count = captured_tcp_output.count;
    socket_tcp_poll(x86_read_tsc() +
                    x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS) + 1U);
    if (captured_tcp_output.count != retransmit_count + 1U ||
        captured_tcp_output.sequence == 0U) goto cleanup;

    /* IPv6 主动 TCP：验证同一套状态机使用 128 位四元组和 IPv6 输出回调。 */
    socket_tcp6_output_get(&saved_tcp6_output, &saved_tcp6_output_context);
    socket_set_tcp_ipv6_output(socket_test_tcp6_output, &captured_tcp6_output);
    tcp6_output_installed = true;
    static const uint8_t active_peer6[16] = {
        0x20U, 0x01U, 0x0DU, 0xB8U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6U
    };
    if (socket_create(OS_AF_INET6, OS_SOCK_STREAM, 0, &active_client6) != K_OK ||
        socket_connect_ipv6(active_client6, active_peer6, 16001U) != K_OK ||
        captured_tcp6_output.count != 1U ||
        captured_tcp6_output.flags != NET_TCP_FLAG_SYN ||
        captured_tcp6_output.payload_length != 0U ||
        socket_inject_tcp_ack_ipv6_reply(
            captured_tcp6_output.destination_address,
            captured_tcp6_output.destination_port,
            captured_tcp6_output.source_address,
            captured_tcp6_output.source_port, 500U,
            captured_tcp6_output.sequence + 1U,
            NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK, 4096U, &active_reply6) != K_OK ||
        !active_reply6.valid || active_reply6.flags != NET_TCP_FLAG_ACK ||
        active_reply6.acknowledgement != 501U) goto cleanup;
    socket_zero(&captured_tcp6_output, sizeof(captured_tcp6_output));
    if (socket_send_ipv6(active_client6, tcp_payload, sizeof(tcp_payload), 0, 0U,
                         &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        captured_tcp6_output.count != 1U ||
        captured_tcp6_output.flags != (NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH) ||
        captured_tcp6_output.payload_length != sizeof(tcp_payload) ||
        !socket_check_payload(captured_tcp6_output.payload, tcp_payload,
                               sizeof(tcp_payload)) ||
        socket_inject_tcp_ack_ipv6_window(
            active_peer6, 16001U, captured_tcp6_output.source_address,
            captured_tcp6_output.source_port, 501U,
            captured_tcp6_output.sequence + (uint32_t)sizeof(tcp_payload),
            NET_TCP_FLAG_ACK, 4096U) != K_OK) goto cleanup;
    socket_zero(&captured_tcp6_output, sizeof(captured_tcp6_output));
    if (socket_send_ipv6(active_client6, tcp_payload, sizeof(tcp_payload), 0, 0U,
                         &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        captured_tcp6_output.count != 1U) goto cleanup;
    uint32_t retransmit6_count = captured_tcp6_output.count;
    socket_tcp_poll(x86_read_tsc() +
                    x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS) + 1U);
    if (captured_tcp6_output.count != retransmit6_count + 1U ||
        captured_tcp6_output.sequence == 0U) goto cleanup;

    uint64_t request_id = 0U;
    os_completion_entry_t completion = {0};
    if (completion_port_create(4U, &completion_port) != K_OK ||
        socket_send_async(sender, udp_payload, sizeof(udp_payload), 0U, 0U,
                          completion_port, 0xCAFEU, &request_id) != K_OK ||
        request_id == 0U || socket_close(sender) != K_OK ||
        deferred_run(64U) == 0U ||
        completion_port_wait(completion_port, 0U, &completion) != K_OK ||
        completion.request_id != request_id || completion.user_key != 0xCAFEU ||
        completion.status != K_EDEVREMOVED || completion.bytes_done != 0U) goto cleanup;

    success = true;
cleanup:
    if (tcp_output_installed) {
        socket_set_tcp_ipv4_output(saved_tcp_output, saved_tcp_output_context);
    }
    if (tcp6_output_installed) {
        socket_set_tcp_ipv6_output(saved_tcp6_output, saved_tcp6_output_context);
    }
    if (completion_port != 0) object_put(completion_port);
    if (sender6 != 0) {
        (void)socket_close(sender6);
        object_put(sender6);
    }
    if (receiver6 != 0) {
        (void)socket_close(receiver6);
        object_put(receiver6);
    }
    if (accepted != 0) {
        (void)socket_close(accepted);
        object_put(accepted);
    }
    if (wire_accepted != 0) {
        (void)socket_close(wire_accepted);
        object_put(wire_accepted);
    }
    if (active_client != 0) {
        (void)socket_close(active_client);
        object_put(active_client);
    }
    if (active_client6 != 0) {
        (void)socket_close(active_client6);
        object_put(active_client6);
    }
    if (wire_listener != 0) {
        (void)socket_close(wire_listener);
        object_put(wire_listener);
    }
    if (client != 0) {
        (void)socket_close(client);
        object_put(client);
    }
    if (listener != 0) {
        (void)socket_close(listener);
        object_put(listener);
    }
    if (sender != 0) {
        (void)socket_close(sender);
        object_put(sender);
    }
    if (receiver != 0) {
        (void)socket_close(receiver);
        object_put(receiver);
    }
    if (chaos_receiver != 0) {
        (void)socket_close(chaos_receiver);
        object_put(chaos_receiver);
    }
    return success;
}
