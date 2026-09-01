#include <arch/x86_64/cpu.h>
#include <kernel/completion_port.h>
#include <kernel/deferred.h>
#include <kernel/kmem.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>
#include <kernel/wait.h>

#include "socket_internal.h"
#include "socket_model.h"

/* REFACTOR_P8_SOCKET_IO_OWNER: socket send, receive, and async I/O. */

static kstatus_t socket_send_stream(socket_t *socket,
                                    socket_private_t *private,
                                    const void *buffer,
                                    size_t length,
                                    uint64_t *bytes) {
    socket_t *peer;
    socket_tcp_ipv4_output_fn output4 = 0;
    socket_tcp_ipv6_output_fn output6 = 0;
    void *output4_context = 0;
    void *output6_context = 0;
    bool wire_output;

    if (length > SOCKET_STREAM_BUFFER_SIZE) return K_EAGAIN;
    socket_tcp_output_get(&output4, &output4_context);
    socket_tcp6_output_get(&output6, &output6_context);
    socket_lock(&socket->lock);

    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        *bytes = 0U;
        return K_EPIPE;
    }

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
            private->tcp_peer_window == 0U ||
            length > private->tcp_peer_window ||
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
        private->tcp_send_deadline_tsc =
            socket_tcp_retransmit_deadline(x86_read_tsc());
        socket_copy(private->tcp_send_payload, buffer, length);

        uint16_t source_port_wire = private->local_port;
        uint16_t destination_port_wire = private->peer_port;
        uint32_t sequence_wire = private->tcp_send_unacknowledged;
        uint32_t acknowledgement_wire = private->tcp_next_sequence;
        uint16_t window_wire = (uint16_t)(
            SOCKET_STREAM_BUFFER_SIZE -
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
            output_status = output4(
                output4_context, source_address_wire, source_port_wire,
                destination_address_wire, destination_port_wire,
                sequence_wire, acknowledgement_wire,
                NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH, window_wire,
                buffer, length);
        } else {
            output_status = output6(
                output6_context, source_address6_wire, source_port_wire,
                destination_address6_wire, destination_port_wire,
                sequence_wire, acknowledgement_wire,
                NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH, window_wire,
                buffer, length);
        }

        if (output_status == K_EAGAIN) {
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
        *bytes = 0U;
        return K_EDEVREMOVED;
    }

    socket_private_t *peer_private = socket_private(peer);
    if (peer_private == 0) {
        object_put(peer);
        *bytes = 0U;
        return K_EIO;
    }

    socket_lock(&peer->lock);
    uint32_t count = atomic_load_explicit(
        &peer_private->stream_count, memory_order_relaxed);
    if (atomic_load_explicit(&peer_private->closed, memory_order_acquire) ||
        atomic_load_explicit(&peer_private->receive_shutdown, memory_order_acquire)) {
        socket_unlock(&peer->lock);
        object_put(peer);
        *bytes = 0U;
        return K_EPIPE;
    }
    if (count > SOCKET_STREAM_BUFFER_SIZE - length) {
        socket_unlock(&peer->lock);
        object_put(peer);
        *bytes = 0U;
        return K_EAGAIN;
    }

    for (size_t index = 0U; index < length; ++index) {
        peer_private->stream_buffer[peer_private->stream_write] =
            ((const uint8_t *)buffer)[index];
        peer_private->stream_write =
            (peer_private->stream_write + 1U) % SOCKET_STREAM_BUFFER_SIZE;
    }
    atomic_store_explicit(&peer_private->stream_count,
                          count + (uint32_t)length,
                          memory_order_release);
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
        bytes == 0 || (buffer == 0 && length != 0U) ||
        length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        *bytes = 0U;
        return K_EDEVREMOVED;
    }
    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        *bytes = 0U;
        return K_EPIPE;
    }
    if (socket->type == OS_SOCK_STREAM) {
        return socket_send_stream(socket, private, buffer, length, bytes);
    }

    socket_lock(&socket->lock);
    if (address == 0U && port == 0U && private->connected) {
        address = private->peer_address;
        port = private->peer_port;
    }
    uint32_t source_address = private->bound ?
        private->local_address : 0x7F000001U;
    uint16_t source_port = private->bound ? private->local_port : 0U;
    socket_unlock(&socket->lock);

    if (source_port == 0U) {
        *bytes = 0U;
        return K_EINVAL;
    }
    if (address == 0x7F000001U || address == source_address) {
        kstatus_t status = socket_inject_udp(
            source_address, source_port, address, port, buffer, length);
        *bytes = status == K_OK ? length : 0U;
        return status;
    }

    socket_udp4_output_get(&output, &output_context);
    if (output != 0) {
        kstatus_t output_status = output(
            output_context, source_address, source_port,
            address, port, buffer, length);
        *bytes = output_status == K_OK ? length : 0U;
        return output_status;
    }

    kstatus_t status = socket_inject_udp(
        source_address, source_port, address, port, buffer, length);
    *bytes = status == K_OK ? length : 0U;
    return status;
}

kstatus_t socket_send_ipv6(socket_t *socket, const void *buffer, size_t length,
                           const uint8_t address[16], uint16_t port,
                           uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);
    socket_udp_ipv6_output_fn output = 0;
    void *output_context = 0;
    uint8_t destination[16];
    uint16_t destination_port = port;
    static const uint8_t loopback6[16] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1,
    };

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || private == 0 || bytes == 0 ||
        (buffer == 0 && length != 0U) || length > SOCKET_MAX_PAYLOAD) {
        return K_EINVAL;
    }
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        *bytes = 0U;
        return K_EDEVREMOVED;
    }
    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        *bytes = 0U;
        return K_EPIPE;
    }
    if (socket->type == OS_SOCK_STREAM) {
        if ((address != 0 && port != 0U) &&
            (!private->connected ||
             !socket_address6_equal(address, private->peer_address6) ||
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
    if (private->bound) {
        socket_copy(source, private->local_address6, 16U);
    } else {
        socket_copy(source, loopback6, 16U);
    }
    uint16_t source_port = private->bound ? private->local_port : 0U;
    socket_unlock(&socket->lock);

    if (source_port == 0U) {
        *bytes = 0U;
        return K_EINVAL;
    }
    if (socket_address6_equal(destination, loopback6) ||
        socket_address6_equal(destination, source)) {
        kstatus_t status = socket_inject_udp_ipv6(
            source, source_port, destination, destination_port,
            buffer, length);
        *bytes = status == K_OK ? length : 0U;
        return status;
    }

    socket_udp6_output_get(&output, &output_context);
    if (output != 0) {
        kstatus_t output_status = output(
            output_context, source, source_port, destination,
            destination_port, buffer, length);
        *bytes = output_status == K_OK ? length : 0U;
        return output_status;
    }

    kstatus_t status = socket_inject_udp_ipv6(
        source, source_port, destination, destination_port, buffer, length);
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
    if (private == 0 ||
        atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        return K_EPIPE;
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
    request->request_id = socket_next_async_request_id();
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

kstatus_t socket_send_async_ipv6(socket_t *socket, const void *buffer,
                                 size_t length, const uint8_t address[16],
                                 uint16_t port,
                                 struct completion_port *completion_port,
                                 uint64_t user_key, uint64_t *request_id) {
    socket_async_send_t *request;
    socket_private_t *private;
    bool socket_ref;
    bool port_ref;

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || socket->type != OS_SOCK_DGRAM ||
        address == 0 || port == 0U || (buffer == 0 && length != 0U) ||
        length > SOCKET_MAX_PAYLOAD || completion_port == 0 ||
        request_id == 0 ||
        ((completion_port_t *)completion_port)->object.type !=
            KOBJECT_TYPE_COMPLETION_PORT) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0 ||
        atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        return K_EPIPE;
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
    request->request_id = socket_next_async_request_id();
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
    return atomic_load_explicit(&wait->private->datagram_count,
                                memory_order_acquire) != 0U ||
           atomic_load_explicit(&wait->private->stream_count,
                                memory_order_acquire) != 0U ||
           atomic_load_explicit(&wait->private->receive_shutdown,
                                memory_order_acquire) ||
           atomic_load_explicit(&wait->private->closed,
                                memory_order_acquire) ||
           atomic_load_explicit(&wait->private->peer_closed,
                                memory_order_acquire);
}

kstatus_t socket_recv(socket_t *socket, void *buffer, size_t length,
                      socket_ipv4_endpoint_t *source, uint64_t timeout_ns,
                      uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4 || private == 0 || source == 0 ||
        bytes == 0 || (buffer == 0 && length != 0U)) return K_EINVAL;
    if (socket->type == OS_SOCK_STREAM &&
        length > SOCKET_STREAM_BUFFER_SIZE) {
        length = SOCKET_STREAM_BUFFER_SIZE;
    }

    socket_wait_context_t wait = {.socket = socket, .private = private};
    for (;;) {
        if (atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
            *bytes = 0U;
            source->address = 0U;
            source->port = 0U;
            source->reserved = 0U;
            return K_OK;
        }
        kstatus_t status = wait_on_queue(&socket->waitq,
                                         socket_receive_ready,
                                         &wait, timeout_ns);
        if (status != K_OK) return status;
        socket_lock(&socket->lock);

        if (atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
            private->datagram_read = private->datagram_write = 0U;
            private->stream_read = private->stream_write = 0U;
            atomic_store_explicit(&private->datagram_count, 0U,
                                  memory_order_release);
            atomic_store_explicit(&private->stream_count, 0U,
                                  memory_order_release);
            source->address = 0U;
            source->port = 0U;
            source->reserved = 0U;
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return K_OK;
        }

        if (socket->type == OS_SOCK_STREAM) {
            uint32_t count = atomic_load_explicit(
                &private->stream_count, memory_order_relaxed);
            if (count != 0U) {
                size_t copy_length = count < length ? count : length;
                for (size_t index = 0U; index < copy_length; ++index) {
                    ((uint8_t *)buffer)[index] =
                        private->stream_buffer[private->stream_read];
                    private->stream_read =
                        (private->stream_read + 1U) % SOCKET_STREAM_BUFFER_SIZE;
                }
                source->address = private->peer_address;
                source->port = private->peer_port;
                source->reserved = 0U;
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
                uint16_t update_window = (uint16_t)(
                    SOCKET_STREAM_BUFFER_SIZE - new_count);
                atomic_store_explicit(&private->stream_count, new_count,
                                      memory_order_release);
                socket_unlock(&socket->lock);

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

            bool closed =
                atomic_load_explicit(&private->closed,
                                     memory_order_acquire) ||
                atomic_load_explicit(&private->peer_closed,
                                     memory_order_acquire);
            socket_unlock(&socket->lock);
            if (closed) {
                *bytes = 0U;
                source->address = private->peer_address;
                source->port = private->peer_port;
                source->reserved = 0U;
                return K_OK;
            }
            continue;
        }

        uint32_t count = atomic_load_explicit(
            &private->datagram_count, memory_order_relaxed);
        if (count != 0U) {
            socket_datagram_t *datagram =
                &private->datagrams[private->datagram_read];
            uint16_t datagram_length = datagram->length;
            size_t copy_length = datagram_length > length ?
                length : datagram_length;
            if (copy_length != 0U) {
                socket_copy(buffer, datagram->payload, copy_length);
            }
            source->address = datagram->source_address;
            source->port = datagram->source_port;
            source->reserved = 0U;
            *bytes = copy_length;
            private->datagram_read =
                (private->datagram_read + 1U) % SOCKET_RX_QUEUE_DEPTH;
            atomic_store_explicit(&private->datagram_count, count - 1U,
                                  memory_order_release);
            socket_unlock(&socket->lock);
            return datagram_length > length ? K_EAGAIN : K_OK;
        }

        bool closed = atomic_load_explicit(&private->closed,
                                           memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}

kstatus_t socket_recv_ipv6(socket_t *socket, void *buffer, size_t length,
                           socket_ipv6_endpoint_t *source,
                           uint64_t timeout_ns, uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        private == 0 || socket->family != OS_AF_INET6 || source == 0 ||
        bytes == 0 || (buffer == 0 && length != 0U)) return K_EINVAL;
    if (socket->type == OS_SOCK_STREAM &&
        length > SOCKET_STREAM_BUFFER_SIZE) {
        length = SOCKET_STREAM_BUFFER_SIZE;
    }

    socket_wait_context_t wait = {.socket = socket, .private = private};
    for (;;) {
        if (atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
            socket_copy(source->address, private->peer_address6, 16U);
            source->port = 0U;
            source->reserved = 0U;
            *bytes = 0U;
            return K_OK;
        }
        kstatus_t status = wait_on_queue(&socket->waitq,
                                         socket_receive_ready,
                                         &wait, timeout_ns);
        if (status != K_OK) return status;
        socket_lock(&socket->lock);

        if (atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
            private->datagram_read = private->datagram_write = 0U;
            private->stream_read = private->stream_write = 0U;
            atomic_store_explicit(&private->datagram_count, 0U,
                                  memory_order_release);
            atomic_store_explicit(&private->stream_count, 0U,
                                  memory_order_release);
            socket_copy(source->address, private->peer_address6, 16U);
            source->port = 0U;
            source->reserved = 0U;
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return K_OK;
        }

        if (socket->type == OS_SOCK_STREAM) {
            uint32_t stream_count = atomic_load_explicit(
                &private->stream_count, memory_order_relaxed);
            if (stream_count != 0U) {
                size_t copy_length = stream_count < length ?
                    stream_count : length;
                for (size_t index = 0U; index < copy_length; ++index) {
                    ((uint8_t *)buffer)[index] =
                        private->stream_buffer[private->stream_read];
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
                uint16_t update_window = (uint16_t)(
                    SOCKET_STREAM_BUFFER_SIZE - new_count);
                atomic_store_explicit(&private->stream_count, new_count,
                                      memory_order_release);
                socket_unlock(&socket->lock);

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

            bool stream_closed =
                atomic_load_explicit(&private->closed,
                                     memory_order_acquire) ||
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

        uint32_t count = atomic_load_explicit(
            &private->datagram_count, memory_order_relaxed);
        if (count != 0U) {
            socket_datagram_t *datagram =
                &private->datagrams[private->datagram_read];
            uint16_t datagram_length = datagram->length;
            size_t copy_length = datagram_length > length ?
                length : datagram_length;
            if (datagram->family != OS_AF_INET6) {
                socket_unlock(&socket->lock);
                return K_EIO;
            }
            if (copy_length != 0U) {
                socket_copy(buffer, datagram->payload, copy_length);
            }
            socket_copy(source->address, datagram->source_address6, 16U);
            source->port = datagram->source_port;
            source->reserved = 0U;
            *bytes = copy_length;
            private->datagram_read =
                (private->datagram_read + 1U) % SOCKET_RX_QUEUE_DEPTH;
            atomic_store_explicit(&private->datagram_count, count - 1U,
                                  memory_order_release);
            socket_unlock(&socket->lock);
            return datagram_length > length ? K_EAGAIN : K_OK;
        }

        bool closed = atomic_load_explicit(&private->closed,
                                           memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}
