#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>

#include "socket_internal.h"
#include "socket_model.h"

/* REFACTOR_P8_SOCKET_TRANSPORT_OWNER: TCP retransmit polling. */

#if OS_DEBUG_SERIAL
static uint32_t g_debug_tcp_poll_logs;
#endif

static void socket_tcp_keepalive_output_failed_locked(
    socket_private_t *private, uint64_t deadline, uint64_t now_tsc) {
    // Local TX backpressure is not evidence of a dead peer. Do not undo a
    // concurrent ACK/option change which has already reset this generation.
    if (private->tcp_keep_deadline_tsc == deadline &&
        private->tcp_keep_probes != 0U) {
        --private->tcp_keep_probes;
        uint64_t ticks = x86_timeout_ns_to_tsc(1000000000ULL);
        private->tcp_keep_deadline_tsc = ticks > UINT64_MAX - now_tsc ?
                                       UINT64_MAX : now_tsc + ticks;
    }
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
        bool keepalive = false;
        uint64_t keep_deadline = 0U;

        socket_lock(socket_binding_lock());
        const socket_tcp_connection_t *connection =
            socket_tcp_connection_at(index);
        if (connection != 0 && connection->socket != 0) {
            target = connection->socket;
            if (!object_try_get(target)) target = 0;
        }
        socket_unlock(socket_binding_lock());
        if (target == 0) continue;

        private = socket_private(target);
        if (private == 0) {
            object_put(target);
            continue;
        }

        socket_lock(&target->lock);
        if (private->tcp_state == SOCKET_TCP_ESTABLISHED &&
            private->peer == 0 && private->tcp_fin_pending &&
            !private->tcp_fin_sent && !private->tcp_send_pending) {
            /* FIN consumes one sequence number and is retransmitted through
             * the same pending-segment path as SYN/data. */
            private->tcp_fin_pending = false;
            private->tcp_fin_sent = true;
            private->tcp_send_pending = true;
            private->tcp_send_unacknowledged = private->tcp_send_next_sequence;
            ++private->tcp_send_next_sequence;
            private->tcp_send_length = 0U;
            private->tcp_send_flags = NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN;
            private->tcp_send_retransmits = 0U;
            private->tcp_send_deadline_tsc =
                socket_tcp_retransmit_deadline(now_tsc);
            flags = private->tcp_send_flags;
            source_address = private->local_address;
            source_port = private->local_port;
            destination_address = private->peer_address;
            destination_port = private->peer_port;
            sequence = private->tcp_send_unacknowledged;
            acknowledgement = private->tcp_next_sequence;
            window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE -
                atomic_load_explicit(&private->stream_count, memory_order_relaxed));
            retransmit = true;
        } else if ((private->tcp_state == SOCKET_TCP_SYN_SENT ||
             private->tcp_state == SOCKET_TCP_SYN_RECEIVED ||
             private->tcp_state == SOCKET_TCP_ESTABLISHED) &&
            private->tcp_send_pending &&
            private->tcp_send_deadline_tsc != 0U &&
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
                    uint32_t count = atomic_load_explicit(
                        &private->stream_count, memory_order_relaxed);
                    window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count);
                }
                ++private->tcp_send_retransmits;
                private->tcp_send_deadline_tsc =
                    socket_tcp_retransmit_deadline(now_tsc);
                retransmit = true;
            }
        }
        if (!retransmit && !failed &&
            socket_tcp_keepalive_poll_locked(private, now_tsc, &failed)) {
            flags = NET_TCP_FLAG_ACK;
            source_address = private->local_address;
            source_port = private->local_port;
            destination_address = private->peer_address;
            destination_port = private->peer_port;
            // An empty segment just before SND.NXT elicits an ACK without
            // consuming sequence space or modifying an outstanding send.
            sequence = private->tcp_send_next_sequence - 1U;
            acknowledgement = private->tcp_next_sequence;
            window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE -
                atomic_load_explicit(&private->stream_count, memory_order_relaxed));
            retransmit = true;
            keepalive = true;
            keep_deadline = private->tcp_keep_deadline_tsc;
        }
        socket_unlock(&target->lock);

        if (failed) {
            (void)wake_all(&target->waitq);
            object_notify_changed(target);
        } else if (retransmit && output4 != 0) {
            kstatus_t output_status = output4(
                output4_context, source_address, source_port,
                destination_address, destination_port, sequence,
                acknowledgement, flags, window, payload, payload_length);
            if (keepalive && output_status != K_OK) {
                socket_lock(&target->lock);
                socket_tcp_keepalive_output_failed_locked(private, keep_deadline, now_tsc);
                socket_unlock(&target->lock);
            }
#if OS_DEBUG_SERIAL
            if (g_debug_tcp_poll_logs < 32U) {
                kernel_serial_printf_serial_only(
                    "OS_TCP_POLL src=%x:%u dst=%x:%u flags=%x retry=%u status=%lld\r\n",
                    source_address, source_port, destination_address,
                    destination_port, flags, private->tcp_send_retransmits,
                    (long long)output_status);
                ++g_debug_tcp_poll_logs;
            }
#endif
            (void)output_status;
        } else if (keepalive) {
            socket_lock(&target->lock);
            socket_tcp_keepalive_output_failed_locked(private, keep_deadline, now_tsc);
            socket_unlock(&target->lock);
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
        bool keepalive = false;
        uint64_t keep_deadline = 0U;

        socket_lock(socket_binding_lock());
        const socket_tcp6_connection_t *connection =
            socket_tcp6_connection_at(index);
        if (connection != 0 && connection->socket != 0) {
            target = connection->socket;
            if (!object_try_get(target)) target = 0;
        }
        socket_unlock(socket_binding_lock());
        if (target == 0) continue;

        private = socket_private(target);
        if (private == 0) {
            object_put(target);
            continue;
        }

        socket_lock(&target->lock);
        if (private->tcp_state == SOCKET_TCP_ESTABLISHED &&
            private->peer == 0 && private->tcp_fin_pending &&
            !private->tcp_fin_sent && !private->tcp_send_pending) {
            private->tcp_fin_pending = false;
            private->tcp_fin_sent = true;
            private->tcp_send_pending = true;
            private->tcp_send_unacknowledged = private->tcp_send_next_sequence;
            ++private->tcp_send_next_sequence;
            private->tcp_send_length = 0U;
            private->tcp_send_flags = NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN;
            private->tcp_send_retransmits = 0U;
            private->tcp_send_deadline_tsc =
                socket_tcp_retransmit_deadline(now_tsc);
            flags = private->tcp_send_flags;
            socket_copy(source_address, private->local_address6, 16U);
            socket_copy(destination_address, private->peer_address6, 16U);
            source_port = private->local_port;
            destination_port = private->peer_port;
            sequence = private->tcp_send_unacknowledged;
            acknowledgement = private->tcp_next_sequence;
            window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE -
                atomic_load_explicit(&private->stream_count, memory_order_relaxed));
            retransmit = true;
        } else if ((private->tcp_state == SOCKET_TCP_SYN_SENT ||
             private->tcp_state == SOCKET_TCP_SYN_RECEIVED ||
             private->tcp_state == SOCKET_TCP_ESTABLISHED) &&
            private->tcp_send_pending &&
            private->tcp_send_deadline_tsc != 0U &&
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
                    uint32_t count = atomic_load_explicit(
                        &private->stream_count, memory_order_relaxed);
                    window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE - count);
                }
                ++private->tcp_send_retransmits;
                private->tcp_send_deadline_tsc =
                    socket_tcp_retransmit_deadline(now_tsc);
                retransmit = true;
            }
        }
        if (!retransmit && !failed &&
            socket_tcp_keepalive_poll_locked(private, now_tsc, &failed)) {
            flags = NET_TCP_FLAG_ACK;
            socket_copy(source_address, private->local_address6, 16U);
            socket_copy(destination_address, private->peer_address6, 16U);
            source_port = private->local_port;
            destination_port = private->peer_port;
            sequence = private->tcp_send_next_sequence - 1U;
            acknowledgement = private->tcp_next_sequence;
            window = (uint16_t)(SOCKET_STREAM_BUFFER_SIZE -
                atomic_load_explicit(&private->stream_count, memory_order_relaxed));
            retransmit = true;
            keepalive = true;
            keep_deadline = private->tcp_keep_deadline_tsc;
        }
        socket_unlock(&target->lock);

        if (failed) {
            (void)wake_all(&target->waitq);
            object_notify_changed(target);
        } else if (retransmit && output6 != 0) {
            kstatus_t output_status = output6(output6_context, source_address, source_port,
                          destination_address, destination_port, sequence,
                          acknowledgement, flags, window, payload,
                          payload_length);
            if (keepalive && output_status != K_OK) {
                socket_lock(&target->lock);
                socket_tcp_keepalive_output_failed_locked(private, keep_deadline, now_tsc);
                socket_unlock(&target->lock);
            }
        }
        else if (keepalive) {
            socket_lock(&target->lock);
            socket_tcp_keepalive_output_failed_locked(private, keep_deadline, now_tsc);
            socket_unlock(&target->lock);
        }
        object_put(target);
    }
}
