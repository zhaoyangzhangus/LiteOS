#pragma once

#include <kernel/completion_port.h>
#include <kernel/socket.h>

/* REFACTOR_P8_SOCKET_MODEL_OWNER: one private socket state model. */

#define SOCKET_BINDING_COUNT 64U
#define SOCKET_EPHEMERAL_FIRST 49152U
#define SOCKET_EPHEMERAL_LAST  65535U

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
    atomic_bool receive_shutdown;
    atomic_bool send_shutdown;
    bool bound;
    bool connected;
    bool listening;
    atomic_bool reuse_address;
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
    bool tcp_fin_pending;
    bool tcp_fin_sent;
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

/* socket.c owns the registry storage; protocol and transport use read-only
 * lookup views while holding the binding lock. */
spinlock_t *socket_binding_lock(void);
const socket_binding_t *socket_binding_at(uint32_t index);
const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t index);
const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t index);
uint64_t socket_next_async_request_id(void);

socket_private_t *socket_private(socket_t *socket);
const socket_private_t *socket_private_const(const socket_t *socket);
void socket_zero(void *memory, size_t length);
void socket_copy(void *destination, const void *source, size_t length);
bool socket_address6_equal(const uint8_t left[16], const uint8_t right[16]);
bool socket_address6_zero(const uint8_t address[16]);
void socket_lock(spinlock_t *lock);
void socket_unlock(spinlock_t *lock);
void socket_initialize_globals(void);
void socket_udp4_output_get(socket_udp_ipv4_output_fn *output, void **context);
void socket_udp6_output_get(socket_udp_ipv6_output_fn *output, void **context);
uint64_t socket_tcp_retransmit_deadline(uint64_t now_tsc);
bool socket_register_tcp_locked(socket_t *socket, socket_private_t *private);
bool socket_tcp_slots_available_locked(uint32_t required);
bool socket_tcp6_slots_available_locked(uint32_t required);
int32_t socket_find_tcp_locked(uint32_t source_address, uint16_t source_port,
                               uint32_t destination_address, uint16_t destination_port);
int32_t socket_find_tcp6_locked(const uint8_t source_address[16], uint16_t source_port,
                                const uint8_t destination_address[16], uint16_t destination_port);
int32_t socket_find_binding_locked(uint32_t address, uint16_t port,
                                   uint16_t type, bool listening_only);
int32_t socket_find_binding6_locked(const uint8_t address[16], uint16_t port,
                                    uint16_t type, bool listening_only);
kstatus_t socket_allocate(uint16_t family, uint16_t type, uint16_t protocol,
                          socket_t **out);
void socket_abort_child(socket_t *child);
