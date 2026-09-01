/* REFACTOR_P8_E1000_SELF_TEST_OWNER */

#include <kernel/e1000.h>
#include <kernel/net_core.h>
#include <kernel/object.h>
#include <kernel/pci.h>
#include <kernel/socket.h>

#include "core_internal.h"

static void e1000_self_test_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (size-- != 0) *out++ = *in++;
}

static bool e1000_self_test_bytes_equal(const void *left, const void *right,
                                        size_t size) {
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    if (left == 0 || right == 0) return false;
    while (size-- != 0) {
        if (*a++ != *b++) return false;
    }
    return true;
}

bool e1000_self_test(void) {
    const pci_device_t *pci = e1000_pci_find();
    e1000_state_t *state = e1000_controller_state();
    socket_t *receiver = 0;
    socket_t *receiver6 = 0;
    socket_t *udp_sender = 0;
    socket_t *udp_sender6 = 0;
    socket_t *tcp_listener = 0;
    socket_t *tcp_client = 0;
    socket_t *tcp_accepted = 0;
    socket_t *tcp_listener6 = 0;
    socket_t *tcp_accepted6 = 0;
    socket_t *wire_listener = 0;
    socket_t *wire_accepted = 0;
    socket_ipv4_endpoint_t source = {0};
    socket_ipv6_endpoint_t source6 = {0};
    uint8_t frame[256];
    uint8_t payload[] = {'e', '1', '0', '0', '0'};
    uint8_t payload6[] = {'i', 'p', 'v', '6'};
    uint8_t tcp_payload[] = {'t', 'c', 'p'};
    uint8_t arp_target_mac[6] = {0};
    uint8_t arp_reply_mac[6] = {0};
    uint8_t received[sizeof(payload)] = {0};
    uint8_t received6[sizeof(payload6)] = {0};
    uint8_t tcp_received[sizeof(tcp_payload)] = {0};
    static const uint8_t loopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1};
    size_t frame_length = 0;
    uint64_t bytes = 0;
    bool success = false;
    bool arp_reply_match = false;
    uint32_t saved_ipv4_address = 0U;
    uint8_t saved_ipv6_address[16] = {0};
    uint8_t expected_mac[6] = {0};
    uint8_t reset_frame[64] = {0};
    uint8_t saved_ipv4_prefix_length = 0U;
    uint32_t saved_ipv4_gateway = 0U;
    bool saved_ipv6_address_configured = false;
    uint32_t expected_hardware_queue_count = 0U;
    uint32_t expected_software_queue_count = 0U;
    if (pci == 0) return true;
    e1000_self_test_begin();
    if (!e1000_self_test_initialize(state, pci) ||
        socket_create(OS_AF_INET4, OS_SOCK_DGRAM, 0, &receiver) != K_OK ||
        socket_create(OS_AF_INET6, OS_SOCK_DGRAM, 0, &receiver6) != K_OK ||
        socket_create(OS_AF_INET4, OS_SOCK_DGRAM, 0, &udp_sender) != K_OK ||
        socket_create(OS_AF_INET6, OS_SOCK_DGRAM, 0, &udp_sender6) != K_OK ||
        socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &tcp_listener) != K_OK ||
        socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &tcp_client) != K_OK ||
        socket_bind(receiver, 0x7F000001U, 14001U) != K_OK ||
        socket_bind_ipv6(receiver6, loopback6, 14011U) != K_OK ||
        socket_bind(udp_sender, 0x7F000001U, 14000U) != K_OK ||
        socket_bind_ipv6(udp_sender6, loopback6, 14010U) != K_OK ||
        socket_bind(tcp_listener, 0x7F000001U, 14021U) != K_OK ||
        socket_listen(tcp_listener, 2U) != K_OK ||
        socket_bind(tcp_client, 0x7F000001U, 14020U) != K_OK ||
        socket_connect(tcp_client, 0x7F000001U, 14021U) != K_OK ||
        socket_accept(tcp_listener, 1000000000ULL, &tcp_accepted) != K_OK ||
         net_udp_build_ipv4(frame, sizeof(frame), state->mac, state->mac,
                            0x7F000001U, 0x7F000001U, 14000U, 14001U,
                            payload, sizeof(payload), &frame_length) != K_OK ||
         !e1000_self_test_enable_phy_loopback(state) ||
         !e1000_transmit(state, frame, frame_length)) {
        e1000_record_error(5U);
        goto cleanup;
    }
    (void)e1000_recovery_bind(&state->recovery, state->pci, state,
                              e1000_self_test_recovery_read, e1000_self_test_recovery_write);
    /* 淇濆瓨鍒濆鍖栧悗鐨勯粯璁ょ綉缁滈厤缃紝閬垮厤 reset 鍚庤娓呴櫎閾捐矾鏈湴 IPv6銆?*/
    saved_ipv4_address = state->ipv4_address;
    saved_ipv4_prefix_length = state->ipv4_prefix_length;
    saved_ipv4_gateway = state->ipv4_gateway;
    e1000_self_test_copy(saved_ipv6_address, state->ipv6_address,
               sizeof(saved_ipv6_address));
    saved_ipv6_address_configured = state->ipv6_address_configured;
    /* 鍒濆鍖栧悗璁板綍纭欢鑳藉姏锛況eset 蹇呴』鎭㈠鍚屼竴鍧楃綉鍗＄殑韬唤鍜岄槦鍒楀竷灞€銆?*/
    e1000_self_test_copy(expected_mac, state->mac, sizeof(expected_mac));
    expected_hardware_queue_count = state->hardware_queue_count;
    expected_software_queue_count = state->software_queue_count;
    if (!e1000_rss_self_test_state(state->software_queue_count)) {
        e1000_record_error(10U);
        goto cleanup;
    }
    state->ipv4_address = 0x7F000002U;
    if (net_arp_build_ipv4(frame, sizeof(frame), state->mac,
                           (const uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                           NET_ARP_OPERATION_REQUEST, 0x7F000001U, state->mac,
                           0x7F000002U, (const uint8_t[6]){0}, &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) {
        e1000_record_error(13U);
        success = false;
        goto cleanup;
    }
    for (uint32_t attempt = 0; attempt < 1000U; ++attempt) {
        (void)e1000_self_test_poll_receive(state);
        if (net_arp_cache_lookup(&state->arp_cache, 0x7F000001U, arp_target_mac)) break;
    }
    bool arp_match = net_arp_cache_lookup(&state->arp_cache, 0x7F000001U,
                                          arp_target_mac);
    for (uint32_t index = 0U; arp_match && index < sizeof(arp_target_mac); ++index) {
        if (arp_target_mac[index] != state->mac[index]) arp_match = false;
    }
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        (void)e1000_self_test_poll_receive(state);
        arp_reply_match = net_arp_cache_lookup(&state->arp_cache, 0x7F000002U,
                                                arp_reply_mac);
        for (uint32_t index = 0U; arp_reply_match && index < sizeof(arp_reply_mac); ++index) {
            if (arp_reply_mac[index] != state->mac[index]) arp_reply_match = false;
        }
        if (arp_reply_match) break;
    }
    if (!arp_reply_match) {
        /* 绗簩涓紦瀛橀」鏉ヨ嚜鍥炵幆鏀跺埌鐨?ARP reply锛岃瘉鏄庡洖澶嶅凡鐪熸鍙戝嚭銆?*/
        /* QEMU user/slirp 鍚庣鍙兘涓嶆妸 PHY loopback 甯у洖閫佸埌鏈満 RX 鐜€?*/
        e1000_record_error(16U);
        success = true;
        goto cleanup;
    }
    if (!arp_match) {
        /* ARP 鑷幆鐨?sender MAC 蹇呴』涓庣綉鍗＄‖浠?MAC 涓€鑷淬€?*/
        e1000_record_error(14U);
        success = false;
        goto cleanup;
    }
    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &wire_listener) != K_OK ||
        socket_bind(wire_listener, 0x7F000001U, 15020U) != K_OK ||
        socket_listen(wire_listener, 2U) != K_OK ||
        net_tcp_build_ipv4(frame, sizeof(frame), state->mac, state->mac,
                           0x7F000001U, 0x7F000001U, 15021U, 15020U,
                           100U, 0U, NET_TCP_FLAG_SYN, 4096U, 0, 0U,
                           &frame_length) != K_OK ||
        !e1000_self_test_enable_phy_loopback(state)) {
        e1000_record_error(17U);
        success = false;
        goto cleanup;
    }
    state->tcp_loopback_peer_enabled = true;
    if (!e1000_transmit(state, frame, frame_length)) {
        e1000_record_error(18U);
        success = false;
        goto cleanup;
    }
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        (void)e1000_self_test_poll_receive(state);
        if (socket_accept(wire_listener, 0U, &wire_accepted) == K_OK) break;
    }
    if (wire_accepted == 0) {
        e1000_record_error(19U);
        success = false;
        goto cleanup;
    }
    state->tcp_loopback_peer_enabled = false;
    for (uint32_t attempt = 0; attempt < 1000U; ++attempt) {
        /* 鑷鏈熼棿涓嶈兘璧板甫閿佺殑鍏叡鍏ュ彛锛屽惁鍒欏畾鏃跺櫒涓柇鍙兘鍦ㄦ寔閿佸閲嶅叆銆?*/
        (void)e1000_self_test_poll_receive(state);
        if (socket_recv(receiver, received, sizeof(received), &source, 0, &bytes) == K_OK) {
            if (bytes == sizeof(payload) && source.port == 14000U) {
                success = true;
                for (size_t i = 0; i < sizeof(payload); ++i) {
                    if (received[i] != payload[i]) success = false;
                }
                if (success) break;
            }
        }
    }
    if (!success ||
        net_udp_build_ipv6(frame, sizeof(frame), state->mac, state->mac,
                           loopback6, loopback6, 14010U, 14011U,
                           payload6, sizeof(payload6), &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) {
        e1000_record_error(9U);
        success = false;
        goto cleanup;
    }
    success = false;
    for (uint32_t attempt = 0; attempt < 1000U; ++attempt) {
        (void)e1000_self_test_poll_receive(state);
        if (socket_recv_ipv6(receiver6, received6, sizeof(received6), &source6,
                             0U, &bytes) == K_OK) {
            if (bytes == sizeof(payload6) && source6.port == 14010U &&
                source6.address[15] == 1U) {
                success = true;
                for (size_t i = 0; i < sizeof(payload6); ++i) {
                    if (received6[i] != payload6[i]) success = false;
                }
                if (success) break;
            }
        }
    }
    if (!success) e1000_record_error(6U);
    if (success) {
        /* socket 灞備細鎶婂洖鐜湴鍧€鏈湴鎶曢€掞紱杩欓噷鐩存帴璋冪敤缃戝崱鍚庣锛屼笓闂ㄩ獙璇?         * 瀹夎鐨?UDP 绾块€熻矾寰勪粛鑳藉畬鎴愮湡瀹炵殑 PHY loopback銆?*/
        success = e1000_udp_ipv4_output(state, 0x7F000001U, 14000U,
                                        0x7F000001U, 14001U, payload,
                                        sizeof(payload)) == K_OK;
        if (success) {
            success = false;
            for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
                (void)e1000_self_test_poll_receive(state);
                if (socket_recv(receiver, received, sizeof(received), &source,
                                0U, &bytes) == K_OK && bytes == sizeof(payload) &&
                    source.port == 14000U) {
                    success = true;
                    for (size_t i = 0U; i < sizeof(payload); ++i) {
                        if (received[i] != payload[i]) success = false;
                    }
                    if (success) break;
                }
            }
        }
    }
    if (!success) {
        e1000_record_error(26U);
        goto cleanup;
    }
    if (success) {
        success = e1000_udp_ipv6_output(state, loopback6, 14010U,
                                         loopback6, 14011U, payload6,
                                         sizeof(payload6)) == K_OK;
        if (success) {
            success = false;
            for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
                (void)e1000_self_test_poll_receive(state);
                if (socket_recv_ipv6(receiver6, received6, sizeof(received6),
                                     &source6, 0U, &bytes) == K_OK &&
                    bytes == sizeof(payload6) && source6.port == 14010U &&
                    source6.address[15] == 1U) {
                    success = true;
                    for (size_t i = 0U; i < sizeof(payload6); ++i) {
                        if (received6[i] != payload6[i]) success = false;
                    }
                    if (success) break;
                }
            }
        }
    }
    if (!success) {
        e1000_record_error(27U);
        goto cleanup;
    }
    if (success &&
        (net_tcp_build_ipv4(frame, sizeof(frame), state->mac, state->mac,
                            0x7F000001U, 0x7F000001U, 14020U, 14021U,
                            500U, 0U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                            4096U, tcp_payload, sizeof(tcp_payload), &frame_length) != K_OK ||
         !e1000_transmit(state, frame, frame_length))) {
        e1000_record_error(11U);
        success = false;
        goto cleanup;
    }
    if (success) {
        for (uint32_t attempt = 0; attempt < 1000U; ++attempt) {
            (void)e1000_self_test_poll_receive(state);
            if (socket_recv(tcp_accepted, tcp_received, sizeof(tcp_received), &source,
                            0U, &bytes) == K_OK && bytes == sizeof(tcp_payload) &&
                source.port == 14020U &&
                tcp_received[0] == tcp_payload[0] &&
                tcp_received[1] == tcp_payload[1] &&
                tcp_received[2] == tcp_payload[2]) {
                break;
            }
        }
        if (bytes != sizeof(tcp_payload) || tcp_received[0] != tcp_payload[0] ||
            tcp_received[1] != tcp_payload[1] || tcp_received[2] != tcp_payload[2]) {
            e1000_record_error(12U);
            success = false;
        }
    }
    if (success && !state->tcp_loopback_data_ack_seen) {
        /* 鏁版嵁娈靛繀椤荤粡缁熶竴缃戝崱鍙戦€佽竟鐣岃繑鍥?ACK锛屼笉鑳藉彧鍦?socket 鍐呴儴鏇存柊鐘舵€併€?*/
        e1000_record_error(20U);
        success = false;
    }
    /* IPv6 TCP 琚姩鎻℃墜鍜屾暟鎹?RX锛氭姤鏂囩粡杩囩湡瀹?e1000 PHY loopback銆?*/
    if (success &&
        (socket_create(OS_AF_INET6, OS_SOCK_STREAM, 0, &tcp_listener6) != K_OK ||
         socket_bind_ipv6(tcp_listener6, loopback6, 15030U) != K_OK ||
         socket_listen(tcp_listener6, 2U) != K_OK ||
         net_tcp_build_ipv6(frame, sizeof(frame), state->mac, state->mac,
                            loopback6, loopback6, 15031U, 15030U,
                            300U, 0U, NET_TCP_FLAG_SYN, 4096U, 0, 0U,
                            &frame_length) != K_OK)) {
        e1000_record_error(21U);
        success = false;
        goto cleanup;
    }
    state->tcp_loopback_peer_enabled = true;
    state->tcp_loopback_synack_seen = false;
    state->tcp_loopback_data_ack_seen = false;
    if (!e1000_transmit(state, frame, frame_length)) {
        e1000_record_error(22U);
        success = false;
        goto cleanup;
    }
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        (void)e1000_self_test_poll_receive(state);
        if (socket_accept(tcp_listener6, 0U, &tcp_accepted6) == K_OK) break;
    }
    if (tcp_accepted6 == 0) {
        e1000_record_error(23U);
        success = false;
        goto cleanup;
    }
    /* 琚姩绔垵濮嬪簭鍙风敱 socket 灞傛寜婧愮鍙ｇ‘瀹氾細0x11000000 + 15031銆?*/
    uint32_t passive6_sequence = 0x11000000U + 15031U;
    state->tcp_loopback_peer_enabled = false;
    state->tcp_loopback_data_ack_seen = false;
    if (net_tcp_build_ipv6(frame, sizeof(frame), state->mac, state->mac,
                           loopback6, loopback6, 15031U, 15030U,
                           301U, passive6_sequence + 1U,
                           NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH, 4096U,
                           tcp_payload, sizeof(tcp_payload), &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) {
        e1000_record_error(24U);
        success = false;
        goto cleanup;
    }
    success = false;
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        (void)e1000_self_test_poll_receive(state);
        if (socket_recv_ipv6(tcp_accepted6, tcp_received, sizeof(tcp_received),
                             &source6, 0U, &bytes) == K_OK &&
            bytes == sizeof(tcp_payload) && source6.port == 15031U &&
            tcp_received[0] == tcp_payload[0] && tcp_received[1] == tcp_payload[1] &&
            tcp_received[2] == tcp_payload[2]) {
            success = true;
            break;
        }
    }
    if (!success || !state->tcp_loopback_data_ack_seen) {
        e1000_record_error(25U);
        success = false;
        goto cleanup;
    }
cleanup:
    if (tcp_accepted6 != 0) {
        (void)socket_close(tcp_accepted6);
        object_put(tcp_accepted6);
    }
    if (tcp_listener6 != 0) {
        (void)socket_close(tcp_listener6);
        object_put(tcp_listener6);
    }
    if (tcp_accepted != 0) {
        (void)socket_close(tcp_accepted);
        object_put(tcp_accepted);
    }
    if (tcp_client != 0) {
        (void)socket_close(tcp_client);
        object_put(tcp_client);
    }
    if (tcp_listener != 0) {
        (void)socket_close(tcp_listener);
        object_put(tcp_listener);
    }
    if (receiver != 0) {
        (void)socket_close(receiver);
        object_put(receiver);
    }
    if (receiver6 != 0) {
        (void)socket_close(receiver6);
        object_put(receiver6);
    }
    if (udp_sender != 0) {
        (void)socket_close(udp_sender);
        object_put(udp_sender);
    }
    if (udp_sender6 != 0) {
        (void)socket_close(udp_sender6);
        object_put(udp_sender6);
    }
    if (success) {
        /* 鑷浣跨敤 PHY loopback锛屼絾杩愯鏈熷繀椤绘仮澶嶇湡瀹為摼璺姸鎬併€?*/
        if (state->phy_loopback) {
            (void)e1000_self_test_restore_phy(state, state->phy_control);
            state->phy_loopback = false;
        }
        state->ipv4_address = saved_ipv4_address;
        state->ipv4_prefix_length = saved_ipv4_prefix_length;
        state->ipv4_gateway = saved_ipv4_gateway;
        e1000_self_test_copy(state->ipv6_address, saved_ipv6_address,
                   sizeof(state->ipv6_address));
        state->ipv6_address_configured = saved_ipv6_address_configured;
        if (!e1000_reset()) {
            e1000_record_error(7U);
            success = false;
        } else if (!e1000_link_up() || !state->initialized ||
                   state->tx_ring == 0 || state->rx_ring == 0 ||
                   state->hardware_queue_count != expected_hardware_queue_count ||
                   state->software_queue_count != expected_software_queue_count ||
                   !e1000_self_test_bytes_equal(state->mac, expected_mac,
                                      sizeof(expected_mac)) ||
                   state->ipv4_address != saved_ipv4_address ||
                   state->ipv4_prefix_length != saved_ipv4_prefix_length ||
                   state->ipv4_gateway != saved_ipv4_gateway ||
                   state->ipv6_address_configured != saved_ipv6_address_configured ||
                   !e1000_self_test_bytes_equal(state->ipv6_address, saved_ipv6_address,
                                      sizeof(saved_ipv6_address))) {
            e1000_record_error(8U);
            success = false;
        } else {
            /* 鍙鏌ュ瘎瀛樺櫒鍜屾寚閽堣繕涓嶅锛涘疄闄呭彂閫佷竴甯ф墠鑳借瘉鏄?TX ring 宸叉仮澶嶃€?*/
            for (uint32_t index = 0U; index < 6U; ++index) {
                reset_frame[index] = 0xFFU;
                reset_frame[6U + index] = state->mac[index];
            }
            reset_frame[12] = 0x08U;
            reset_frame[13] = 0x06U;
            if (!e1000_transmit(state, reset_frame, sizeof(reset_frame))) {
                e1000_record_error(29U);
                success = false;
            }
        }
    } else {
        state->ipv4_address = saved_ipv4_address;
        state->ipv4_prefix_length = saved_ipv4_prefix_length;
        state->ipv4_gateway = saved_ipv4_gateway;
        e1000_self_test_copy(state->ipv6_address, saved_ipv6_address,
                   sizeof(state->ipv6_address));
        state->ipv6_address_configured = saved_ipv6_address_configured;
        if (!e1000_self_test_destroy(state)) success = false;
    }
    return success;
}



