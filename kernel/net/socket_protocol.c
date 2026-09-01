#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include <kernel/object.h>
#include <kernel/socket.h>
#include <kernel/wait.h>

#include "socket_model.h"

/* REFACTOR_P8_SOCKET_PROTOCOL_OWNER: packet injection and TCP state transitions. */

kstatus_t socket_inject_udp(uint32_t source_address, uint16_t source_port,
                            uint32_t destination_address, uint16_t destination_port,
                            const void *buffer, size_t length) {
    socket_t *target = 0;
    socket_private_t *private;
    if (source_address == 0U || source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0) || length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t binding = socket_find_binding_locked(destination_address, destination_port,
                                                 OS_SOCK_DGRAM, false);
    if (binding >= 0) {
        target = socket_binding_at((uint32_t)binding)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    uint32_t count = atomic_load_explicit(&private->datagram_count, memory_order_relaxed);
    if (atomic_load_explicit(&private->closed, memory_order_acquire) ||
        atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_OK;
    }
    if (count >= SOCKET_RX_QUEUE_DEPTH) {
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
    socket_lock(socket_binding_lock());
    int32_t binding = socket_find_binding6_locked(destination_address, destination_port,
                                                  OS_SOCK_DGRAM, false);
    if (binding >= 0) {
        target = socket_binding_at((uint32_t)binding)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    uint32_t count = atomic_load_explicit(&private->datagram_count, memory_order_relaxed);
    if (atomic_load_explicit(&private->closed, memory_order_acquire) ||
        atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_OK;
    }
    if (count >= SOCKET_RX_QUEUE_DEPTH) {
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
    bool discard_receive = false;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    if (source_address == 0U || source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0U) || length > SOCKET_STREAM_BUFFER_SIZE ||
        (flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) != NET_TCP_FLAG_ACK) {
        return K_EINVAL;
    }
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t connection = socket_find_tcp_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = socket_tcp_connection_at((uint32_t)connection)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
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
    discard_receive = atomic_load_explicit(&private->receive_shutdown,
                                            memory_order_acquire);
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
        /* 瑜版挸澧?RX 缁愭褰涢崣顏呭复閸欐ぞ绗呮稉鈧稉顏囩箾缂侇厼绨崣鍑ょ幢闁插秴顦?娑斿崬绨崠鍛暠娑撳﹤鐪伴柌宥勭炊閺堝搫鍩楁径鍕倞閵?*/
        socket_unlock(&target->lock);
        object_put(target);
        return sequence < expected ? K_OK : K_EAGAIN;
    }
    if (!discard_receive && count > SOCKET_STREAM_BUFFER_SIZE - length) {
        if (reply != 0) reply->window = 0U;
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    if (!discard_receive) {
        for (size_t index = 0U; index < length; ++index) {
            private->stream_buffer[private->stream_write] = ((const uint8_t *)buffer)[index];
            private->stream_write = (private->stream_write + 1U) % SOCKET_STREAM_BUFFER_SIZE;
        }
        atomic_store_explicit(&private->stream_count,
                              count + (uint32_t)length,
                              memory_order_release);
    }
    private->tcp_sequence_valid = true;
    private->tcp_next_sequence = sequence + (uint32_t)length;
    if ((flags & NET_TCP_FLAG_FIN) != 0U) {
        ++private->tcp_next_sequence;
        atomic_store_explicit(&private->peer_closed, true, memory_order_release);
    }
    if (reply != 0) {
        reply->acknowledgement = private->tcp_next_sequence;
        reply->window = discard_receive ? SOCKET_STREAM_BUFFER_SIZE :
            (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count - length);
    }
    notify = (!discard_receive && length != 0U) ||
             (flags & NET_TCP_FLAG_FIN) != 0U;
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
    bool discard_receive = false;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    if (source_address == 0 || destination_address == 0 ||
        socket_address6_zero(source_address) || socket_address6_zero(destination_address) ||
        source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0U) || length > SOCKET_STREAM_BUFFER_SIZE ||
        (flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) != NET_TCP_FLAG_ACK) {
        return K_EINVAL;
    }
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t connection = socket_find_tcp6_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = socket_tcp6_connection_at((uint32_t)connection)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
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
    discard_receive = atomic_load_explicit(&private->receive_shutdown,
                                            memory_order_acquire);
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
    if (!discard_receive && count > SOCKET_STREAM_BUFFER_SIZE - length) {
        if (reply != 0) reply->window = 0U;
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    if (!discard_receive) {
        for (size_t index = 0U; index < length; ++index) {
            private->stream_buffer[private->stream_write] = ((const uint8_t *)buffer)[index];
            private->stream_write = (private->stream_write + 1U) % SOCKET_STREAM_BUFFER_SIZE;
        }
        atomic_store_explicit(&private->stream_count,
                              count + (uint32_t)length,
                              memory_order_release);
    }
    private->tcp_sequence_valid = true;
    private->tcp_next_sequence = sequence + (uint32_t)length;
    if ((flags & NET_TCP_FLAG_FIN) != 0U) {
        ++private->tcp_next_sequence;
        atomic_store_explicit(&private->peer_closed, true, memory_order_release);
    }
    if (reply != 0) {
        reply->acknowledgement = private->tcp_next_sequence;
        reply->window = discard_receive ? SOCKET_STREAM_BUFFER_SIZE :
            (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count - length);
    }
    notify = (!discard_receive && length != 0U) ||
             (flags & NET_TCP_FLAG_FIN) != 0U;
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
    socket_lock(socket_binding_lock());
    int32_t binding = socket_find_binding_locked(destination_address, destination_port,
                                                 OS_SOCK_STREAM, true);
    if (binding >= 0) {
        listener = socket_binding_at((uint32_t)binding)->socket;
        if (!object_try_get(listener)) listener = 0;
    }
    socket_unlock(socket_binding_lock());
    if (listener == 0) return K_ENOENT;
    listener_private = socket_private(listener);
    if (listener_private == 0) {
        object_put(listener);
        return K_EIO;
    }
    socket_lock(socket_binding_lock());
    int32_t existing = socket_find_tcp_locked(source_address, source_port,
                                               destination_address, destination_port);
    socket_t *existing_socket = existing >= 0 ?
        socket_tcp_connection_at((uint32_t)existing)->socket : 0;
    if (existing_socket != 0 && !object_try_get(existing_socket)) existing_socket = 0;
    socket_unlock(socket_binding_lock());
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
    socket_lock(socket_binding_lock());
    bool available = socket_tcp_slots_available_locked(1U);
    socket_unlock(socket_binding_lock());
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

    socket_lock(socket_binding_lock());
    bool registered = socket_register_tcp_locked(child, child_private);
    socket_unlock(socket_binding_lock());
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
    socket_lock(socket_binding_lock());
    int32_t binding = socket_find_binding6_locked(destination_address, destination_port,
                                                  OS_SOCK_STREAM, true);
    if (binding >= 0) {
        listener = socket_binding_at((uint32_t)binding)->socket;
        if (!object_try_get(listener)) listener = 0;
    }
    socket_unlock(socket_binding_lock());
    if (listener == 0) return K_ENOENT;
    listener_private = socket_private(listener);
    if (listener_private == 0) {
        object_put(listener);
        return K_EIO;
    }
    socket_lock(socket_binding_lock());
    int32_t existing = socket_find_tcp6_locked(source_address, source_port,
                                                destination_address, destination_port);
    socket_t *existing_socket = existing >= 0 ?
        socket_tcp6_connection_at((uint32_t)existing)->socket : 0;
    if (existing_socket != 0 && !object_try_get(existing_socket)) existing_socket = 0;
    socket_unlock(socket_binding_lock());
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
    socket_lock(socket_binding_lock());
    bool available = socket_tcp6_slots_available_locked(1U);
    socket_unlock(socket_binding_lock());
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

    socket_lock(socket_binding_lock());
    bool registered = socket_register_tcp_locked(child, child_private);
    socket_unlock(socket_binding_lock());
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
    socket_lock(socket_binding_lock());
    int32_t connection = socket_find_tcp_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = socket_tcp_connection_at((uint32_t)connection)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
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
    socket_lock(socket_binding_lock());
    int32_t connection = socket_find_tcp6_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = socket_tcp6_connection_at((uint32_t)connection)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
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
