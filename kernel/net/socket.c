#include <kernel/kmem.h>
#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>
#include <kernel/sched.h>
#include <kernel/wait.h>

#include "socket_internal.h"
#include "socket_model.h"

/* Private state types are declared once in socket_model.h. */

/* 瀹告彃缂撶粩?TCP 鏉╃偞甯撮惃鍕磽閸忓啰绮嶇槐銏犵穿閿涙稓娲冮崥顒傤伂閸欙絽鎷?accepted socket 閸欘垯浜掗崗鍗炵摠閵?*/
/* Connection and wait types are declared once in socket_model.h. */

/* 瀵倹顒為崣鎴︹偓浣锋崲閸斺€愁槻閸掓儼绀嬫潪钘夎嫙閹镐焦婀佹稉銈囶伂鐎电钖勫鏇犳暏閿涘苯鍙ч梻?socket 娑撳秳绱伴柌濠冩杹娴犺濮熸笟婵婄閵?*/
/* Async request state is declared once in socket_model.h. */

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

socket_private_t *socket_private(socket_t *socket);

void socket_zero(void *memory, size_t length) {
    uint8_t *bytes = (uint8_t *)memory;
    while (length-- != 0) *bytes++ = 0;
}

void socket_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0) *out++ = *in++;
}

bool socket_address6_equal(const uint8_t left[16], const uint8_t right[16]) {
    uint8_t difference = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) difference |= left[index] ^ right[index];
    return difference == 0U;
}

bool socket_address6_zero(const uint8_t address[16]) {
    uint8_t value = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) value |= address[index];
    return value == 0U;
}

void socket_lock(spinlock_t *lock) {
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

void socket_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
    sched_preempt_enable();
}

void socket_initialize_globals(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_socket_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        socket_zero(g_bindings, sizeof(g_bindings));
        socket_zero(g_tcp_connections, sizeof(g_tcp_connections));
        socket_zero(g_tcp6_connections, sizeof(g_tcp6_connections));
        atomic_init(&g_binding_lock.state, 0U);
        atomic_init(&g_next_ephemeral_port, SOCKET_EPHEMERAL_FIRST);
        atomic_init(&g_next_async_request_id, 0U);
        atomic_store_explicit(&g_socket_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_socket_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

spinlock_t *socket_binding_lock(void) {
    socket_initialize_globals();
    return &g_binding_lock;
}

const socket_binding_t *socket_binding_at(uint32_t index) {
    return index < SOCKET_BINDING_COUNT ? &g_bindings[index] : 0;
}

const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t index) {
    return index < SOCKET_BINDING_COUNT ? &g_tcp_connections[index] : 0;
}

const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t index) {
    return index < SOCKET_BINDING_COUNT ? &g_tcp6_connections[index] : 0;
}

uint64_t socket_next_async_request_id(void) {
    socket_initialize_globals();
    return atomic_fetch_add_explicit(&g_next_async_request_id, 1U,
                                     memory_order_relaxed) + 1U;
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

void socket_udp4_output_get(socket_udp_ipv4_output_fn *output,
                            void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_udp_ipv4_output;
    if (context != 0) *context = g_udp_ipv4_output_context;
    socket_unlock(&g_binding_lock);
}

void socket_udp6_output_get(socket_udp_ipv6_output_fn *output,
                            void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_udp_ipv6_output;
    if (context != 0) *context = g_udp_ipv6_output_context;
    socket_unlock(&g_binding_lock);
}

uint64_t socket_tcp_retransmit_deadline(uint64_t now_tsc) {
    uint64_t timeout = x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS);
    return timeout > UINT64_MAX - now_tsc ? UINT64_MAX : now_tsc + timeout;
}

void socket_tcp_output_get(socket_tcp_ipv4_output_fn *output,
                           void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_tcp_ipv4_output;
    if (context != 0) *context = g_tcp_ipv4_output_context;
    socket_unlock(&g_binding_lock);
}

void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *output,
                            void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_tcp_ipv6_output;
    if (context != 0) *context = g_tcp_ipv6_output_context;
    socket_unlock(&g_binding_lock);
}

socket_private_t *socket_private(socket_t *socket) {
    return socket != 0 ? (socket_private_t *)socket->protocol_state : 0;
}

kstatus_t socket_get_info(socket_t *socket, os_socket_info_t *info) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || info == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        private == 0) return K_EINVAL;
    socket_lock(&socket->lock);
    info->family = socket->family;
    info->type = socket->type;
    info->protocol = socket->protocol;
    info->reserved = 0U;
    bool stream_connected = private->connected &&
        (socket->type != OS_SOCK_STREAM || private->peer != 0 ||
         private->tcp_state == SOCKET_TCP_ESTABLISHED);
    info->flags = (private->bound ? OS_SOCKET_INFO_BOUND : 0U) |
                  (stream_connected ? OS_SOCKET_INFO_CONNECTED : 0U) |
                  (private->listening ? OS_SOCKET_INFO_LISTENING : 0U);
    info->local_address = private->local_address;
    info->peer_address = private->peer_address;
    socket_copy(info->local_address6, private->local_address6, 16U);
    socket_copy(info->peer_address6, private->peer_address6, 16U);
    info->local_port = private->local_port;
    info->peer_port = private->peer_port;
    info->reserved2 = 0U;
    socket_unlock(&socket->lock);
    return K_OK;
}

kstatus_t socket_get_option(socket_t *socket, uint32_t option, int32_t *value) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || value == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        private == 0) return K_EINVAL;
    socket_lock(&socket->lock);
    switch (option) {
    case OS_SOCKET_OPTION_REUSE_ADDRESS:
        *value = atomic_load_explicit(&private->reuse_address,
                                      memory_order_acquire);
        break;
    case OS_SOCKET_OPTION_ERROR: *value = private->tcp_send_failed ? 5 : 0; break;
    case OS_SOCKET_OPTION_TYPE: *value = socket->type; break;
    case OS_SOCKET_OPTION_DOMAIN: *value = socket->family; break;
    case OS_SOCKET_OPTION_PROTOCOL: *value = socket->protocol; break;
    case OS_SOCKET_OPTION_ACCEPT_CONNECTION: *value = private->listening; break;
    case OS_SOCKET_OPTION_RECEIVE_BUFFER:
        *value = socket->type == OS_SOCK_STREAM ? SOCKET_STREAM_BUFFER_SIZE :
                 SOCKET_RX_QUEUE_DEPTH * SOCKET_MAX_PAYLOAD;
        break;
    case OS_SOCKET_OPTION_SEND_BUFFER: *value = SOCKET_STREAM_BUFFER_SIZE; break;
    default:
        socket_unlock(&socket->lock);
        return K_EINVAL;
    }
    socket_unlock(&socket->lock);
    return K_OK;
}

kstatus_t socket_set_option(socket_t *socket, uint32_t option, int32_t value) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET || private == 0 ||
        option != OS_SOCKET_OPTION_REUSE_ADDRESS) return K_EINVAL;
    socket_lock(&socket->lock);
    atomic_store_explicit(&private->reuse_address, value != 0,
                          memory_order_release);
    socket_unlock(&socket->lock);
    return K_OK;
}

const socket_private_t *socket_private_const(const socket_t *socket) {
    return socket != 0 ? (const socket_private_t *)socket->protocol_state : 0;
}

static bool socket_is_signaled(const void *object) {
    const socket_t *socket = (const socket_t *)object;
    const socket_private_t *private = socket_private_const(socket);
    if (private == 0) return false;
    return atomic_load_explicit(&private->datagram_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->stream_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->accept_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->receive_shutdown, memory_order_acquire) ||
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

bool socket_register_tcp_locked(socket_t *socket, socket_private_t *private) {
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

bool socket_tcp_slots_available_locked(uint32_t required) {
    uint32_t free_slots = 0U;
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_tcp_connections[index].socket == 0) ++free_slots;
    }
    return free_slots >= required;
}

bool socket_tcp6_slots_available_locked(uint32_t required) {
    uint32_t free_slots = 0U;
    for (uint32_t index = 0; index < SOCKET_BINDING_COUNT; ++index) {
        if (g_tcp6_connections[index].socket == 0) ++free_slots;
    }
    return free_slots >= required;
}

int32_t socket_find_tcp6_locked(const uint8_t source_address[16],
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

int32_t socket_find_tcp_locked(uint32_t source_address, uint16_t source_port,
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

    /* 娑撱倗顏幐澶婃勾閸р偓閹烘帒绨崝鐘绘敚閿涘矂浼╅崗宥呮倱閺冭泛鍙ч梻顓＄箾閹恒儲妞傝ぐ銏″灇 ABBA 濮濆鏀ｉ妴?*/
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

static void socket_handle_close(void *object) {
    (void)socket_close((socket_t *)object);
}

static const object_ops_t g_socket_object_ops = {
    .destroy = socket_destroy,
    .handle_close = socket_handle_close,
    .type_name = "Socket",
    .is_signaled = socket_is_signaled,
    .wait_value = socket_wait_value,
};

int32_t socket_find_binding_locked(uint32_t address, uint16_t port,
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

int32_t socket_find_binding6_locked(const uint8_t address[16], uint16_t port,
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

kstatus_t socket_allocate(uint16_t family, uint16_t type, uint16_t protocol,
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
    atomic_init(&private->receive_shutdown, false);
    atomic_init(&private->send_shutdown, false);
    atomic_init(&private->reuse_address, false);
    socket->rx_queue = private;
    socket->tx_queue = 0;
    socket->protocol_state = private;
    *out = socket;
    return K_OK;
}

kstatus_t socket_shutdown(socket_t *socket, uint32_t how) {
    socket_private_t *private;
    socket_t *peer = 0;
    bool notify_self = false;
    bool notify_peer = false;
    bool kick_transport = false;

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        how > OS_SOCKET_SHUT_BOTH) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0) return K_EIO;

    socket_lock(&socket->lock);
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        return K_EDEVREMOVED;
    }

    if (how == OS_SOCKET_SHUT_READ || how == OS_SOCKET_SHUT_BOTH) {
        atomic_store_explicit(&private->receive_shutdown, true,
                              memory_order_release);
        private->datagram_read = private->datagram_write = 0U;
        private->stream_read = private->stream_write = 0U;
        atomic_store_explicit(&private->datagram_count, 0U,
                              memory_order_release);
        atomic_store_explicit(&private->stream_count, 0U,
                              memory_order_release);
        notify_self = true;
    }

    if (how == OS_SOCKET_SHUT_WRITE || how == OS_SOCKET_SHUT_BOTH) {
        atomic_store_explicit(&private->send_shutdown, true,
                              memory_order_release);
        notify_self = true;
        if (socket->type == OS_SOCK_STREAM && private->peer == 0 &&
            private->connected && private->tcp_state != SOCKET_TCP_CLOSED &&
            !private->tcp_fin_sent) {
            private->tcp_fin_pending = true;
            kick_transport = true;
        }
    }

    /* Keep the descriptor alive while publishing the half-close to a local
     * peer.  The peer's own lock is acquired only after releasing ours. */
    if (socket->type == OS_SOCK_STREAM && private->peer != 0 &&
        object_try_get(private->peer)) {
        peer = private->peer;
        notify_peer = true;
    }
    socket_unlock(&socket->lock);

    if (notify_self) (void)wake_all(&socket->waitq);
    if (peer != 0) {
        socket_private_t *peer_private = socket_private(peer);
        if (peer_private != 0) {
            socket_lock(&peer->lock);
            if (peer_private->peer == socket) {
                if (how == OS_SOCKET_SHUT_WRITE || how == OS_SOCKET_SHUT_BOTH) {
                    atomic_store_explicit(&peer_private->peer_closed, true,
                                          memory_order_release);
                }
            }
            socket_unlock(&peer->lock);
            if (notify_peer) (void)wake_all(&peer->waitq);
        }
        object_put(peer);
    }
    if (kick_transport) socket_tcp_poll(x86_read_tsc());
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

void socket_abort_child(socket_t *child) {
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
        /* ARP 閺堫亜鎳℃稉顓熸 SYN 閻ｆ瑥婀柌宥勭炊闂冪喎鍨敍灞芥倵缂侇叀鐤嗙拠顫窗閸愬秵顐肩亸婵婄槸閸欐垿鈧降鈧?*/
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
        /* 鏉╃偞甯寸悰銊﹀姬鏉炶棄褰цぐ鍗炴惙缂冩垵宕?RX閿涘奔绗夐惍鏉戞綎瀹歌尙绮″铏圭彌閻ㄥ嫬鍞撮弽鍛婃拱閸︽媽绻涢幒銉ｂ偓?*/
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
            *out = accepted; /* 鏉烆剛些閻╂垵鎯夐梼鐔峰灙閹镐焦婀侀惃鍕嚠鐠炩€崇穿閻劎绮扮拫鍐暏閼板懌鈧?*/
            return K_OK;
        }
        bool closed = atomic_load_explicit(&private->closed, memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}
