#include <kernel/net_core.h>
#include <uapi/socket.h>

#define NET_IPV4_VERSION   4U
#define NET_IPV4_UDP       17U
#define NET_IPV4_TCP       6U

static void net_zero(void *destination, size_t length) {
    uint8_t *bytes = (uint8_t *)destination;
    while (length-- != 0) *bytes++ = 0;
}

static void net_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0) *out++ = *in++;
}

static bool net_mac_equal(const uint8_t left[6], const uint8_t right[6]) {
    for (uint32_t i = 0; i < 6U; ++i) if (left[i] != right[i]) return false;
    return true;
}

static bool net_address6_equal(const uint8_t left[16], const uint8_t right[16]) {
    uint8_t difference = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static bool net_address6_zero(const uint8_t address[16]) {
    uint8_t value = 0U;
    if (address == 0) return true;
    for (uint32_t index = 0U; index < 16U; ++index) value |= address[index];
    return value == 0U;
}

static void net_store_be16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static void net_store_be32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static uint16_t net_load_be16(const uint8_t *source) {
    return (uint16_t)(((uint16_t)source[0] << 8) | source[1]);
}

static uint32_t net_load_be32(const uint8_t *source) {
    return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) |
           ((uint32_t)source[2] << 8) | source[3];
}

void net_device_init(net_device_t *device, const char *name,
                     const uint8_t mac[6], uint16_t mtu,
                     net_device_transmit_fn transmit, void *context) {
    if (device == 0) return;
    net_zero(device, sizeof(*device));
    if (name != 0) {
        for (size_t index = 0U; index + 1U < sizeof(device->name) &&
                                  name[index] != '\0'; ++index) {
            device->name[index] = name[index];
        }
    }
    if (mac != 0) net_copy(device->mac, mac, sizeof(device->mac));
    device->mtu = mtu;
    device->max_frame_size = (uint16_t)(mtu + NET_ETHERNET_HEADER_SIZE + 4U);
    device->context = context;
    device->transmit = transmit;
}

kstatus_t net_device_send(net_device_t *device, const void *frame, size_t length) {
    if (device == 0 || frame == 0 || length == 0U || device->transmit == 0 ||
        length > device->max_frame_size) return K_EINVAL;
    if (!device->link_up) return K_EAGAIN;
    return device->transmit(device->context, frame, length);
}

static uint32_t net_checksum_add(uint32_t sum, const uint8_t *data,
                                 size_t length) {
    while (length >= 2U) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        length -= 2U;
    }
    if (length != 0) sum += (uint32_t)data[0] << 8;
    while ((sum >> 16) != 0) sum = (sum & 0xFFFFU) + (sum >> 16);
    return sum;
}

uint16_t net_checksum16(const void *data, size_t length) {
    if (data == 0 && length != 0) return 0;
    uint32_t sum = net_checksum_add(0, (const uint8_t *)data, length);
    return (uint16_t)~sum;
}

static uint16_t net_udp_checksum(uint32_t source_address,
                                 uint32_t destination_address,
                                 const uint8_t *udp, uint16_t length) {
    uint8_t address[8];
    uint8_t protocol[4] = {0, NET_IPV4_UDP, (uint8_t)(length >> 8), (uint8_t)length};
    net_store_be32(address, source_address);
    net_store_be32(address + 4U, destination_address);
    uint32_t sum = net_checksum_add(0, address, sizeof(address));
    sum = net_checksum_add(sum, protocol, sizeof(protocol));
    sum = net_checksum_add(sum, udp, length);
    return (uint16_t)~sum;
}

static uint16_t net_udp_checksum_ipv6(const uint8_t source_address[16],
                                      const uint8_t destination_address[16],
                                      const uint8_t *udp, uint16_t length) {
    uint8_t pseudo_header[40];
    net_zero(pseudo_header, sizeof(pseudo_header));
    net_copy(pseudo_header, source_address, 16U);
    net_copy(pseudo_header + 16U, destination_address, 16U);
    net_store_be32(pseudo_header + 32U, length);
    pseudo_header[39] = NET_IPV4_UDP;
    uint32_t sum = net_checksum_add(0, pseudo_header, sizeof(pseudo_header));
    sum = net_checksum_add(sum, udp, length);
    return (uint16_t)~sum;
}

static uint16_t net_tcp_checksum_ipv4(uint32_t source_address,
                                      uint32_t destination_address,
                                      const uint8_t *tcp, uint16_t length) {
    uint8_t address[8];
    uint8_t protocol[4] = {0, NET_IPV4_TCP, (uint8_t)(length >> 8), (uint8_t)length};
    net_store_be32(address, source_address);
    net_store_be32(address + 4U, destination_address);
    uint32_t sum = net_checksum_add(0, address, sizeof(address));
    sum = net_checksum_add(sum, protocol, sizeof(protocol));
    sum = net_checksum_add(sum, tcp, length);
    return (uint16_t)~sum;
}

static uint16_t net_tcp_checksum_ipv6(const uint8_t source_address[16],
                                      const uint8_t destination_address[16],
                                      const uint8_t *tcp, uint16_t length) {
    uint8_t pseudo_header[40];
    net_zero(pseudo_header, sizeof(pseudo_header));
    net_copy(pseudo_header, source_address, 16U);
    net_copy(pseudo_header + 16U, destination_address, 16U);
    net_store_be32(pseudo_header + 32U, length);
    pseudo_header[39] = NET_IPV4_TCP;
    uint32_t sum = net_checksum_add(0, pseudo_header, sizeof(pseudo_header));
    sum = net_checksum_add(sum, tcp, length);
    return (uint16_t)~sum;
}

static uint16_t net_icmpv6_checksum(const uint8_t source_address[16],
                                    const uint8_t destination_address[16],
                                    const uint8_t *icmp, uint16_t length) {
    uint8_t pseudo_header[40];
    net_zero(pseudo_header, sizeof(pseudo_header));
    net_copy(pseudo_header, source_address, 16U);
    net_copy(pseudo_header + 16U, destination_address, 16U);
    net_store_be32(pseudo_header + 32U, length);
    pseudo_header[39] = NET_ICMPV6_NEXT_HEADER;
    uint32_t sum = net_checksum_add(0, pseudo_header, sizeof(pseudo_header));
    sum = net_checksum_add(sum, icmp, length);
    return (uint16_t)~sum;
}

void net_ndp_solicited_node_address(const uint8_t address[16],
                                    uint8_t solicited_node[16]) {
    if (address == 0 || solicited_node == 0) return;
    net_zero(solicited_node, 16U);
    solicited_node[0] = 0xFFU;
    solicited_node[1] = 0x02U;
    solicited_node[11] = 0x01U;
    solicited_node[12] = 0xFFU;
    solicited_node[13] = address[13];
    solicited_node[14] = address[14];
    solicited_node[15] = address[15];
}

void net_ndp_multicast_mac(const uint8_t multicast_address[16], uint8_t mac[6]) {
    if (multicast_address == 0 || mac == 0) return;
    mac[0] = 0x33U;
    mac[1] = 0x33U;
    mac[2] = multicast_address[12];
    mac[3] = multicast_address[13];
    mac[4] = multicast_address[14];
    mac[5] = multicast_address[15];
}

static kstatus_t net_ndp_build(uint8_t *frame, size_t capacity,
                               const uint8_t source_mac[6],
                               const uint8_t destination_mac[6],
                               const uint8_t source_address[16],
                               const uint8_t destination_address[16],
                               uint8_t type, const uint8_t target_address[16],
                               uint32_t flags, const uint8_t option_mac[6],
                               size_t *frame_length) {
    const uint16_t icmp_length = 32U;
    const uint32_t total_length = NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                                  icmp_length;
    uint8_t *ip;
    uint8_t *icmp;
    if (frame == 0 || capacity < total_length || source_mac == 0 ||
        destination_mac == 0 || source_address == 0 || destination_address == 0 ||
        target_address == 0 || option_mac == 0 || frame_length == 0 ||
        (type != NET_ICMPV6_NEIGHBOR_SOLICITATION &&
         type != NET_ICMPV6_NEIGHBOR_ADVERTISEMENT)) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_IPV6);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV6_HEADER_SIZE);
    ip[0] = 0x60U;
    net_store_be16(ip + 4U, icmp_length);
    ip[6] = NET_ICMPV6_NEXT_HEADER;
    ip[7] = 255U;
    net_copy(ip + 8U, source_address, 16U);
    net_copy(ip + 24U, destination_address, 16U);
    icmp = ip + NET_IPV6_HEADER_SIZE;
    net_zero(icmp, icmp_length);
    icmp[0] = type;
    net_store_be32(icmp + 4U, flags);
    net_copy(icmp + 8U, target_address, 16U);
    icmp[24] = type == NET_ICMPV6_NEIGHBOR_SOLICITATION ?
                   NET_NDP_OPTION_SOURCE_LINK_LAYER : NET_NDP_OPTION_TARGET_LINK_LAYER;
    icmp[25] = 1U;
    net_copy(icmp + 26U, option_mac, 6U);
    net_store_be16(icmp + 2U, net_icmpv6_checksum(source_address,
                                                  destination_address,
                                                  icmp, icmp_length));
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_ndp_build_neighbor_solicitation(
    uint8_t *frame, size_t capacity, const uint8_t source_mac[6],
    const uint8_t destination_mac[6], const uint8_t source_address[16],
    const uint8_t destination_address[16], const uint8_t target_address[16],
    size_t *frame_length) {
    return net_ndp_build(frame, capacity, source_mac, destination_mac,
                         source_address, destination_address,
                         NET_ICMPV6_NEIGHBOR_SOLICITATION, target_address, 0U,
                         source_mac, frame_length);
}

kstatus_t net_ndp_build_neighbor_advertisement(
    uint8_t *frame, size_t capacity, const uint8_t source_mac[6],
    const uint8_t destination_mac[6], const uint8_t source_address[16],
    const uint8_t destination_address[16], const uint8_t target_address[16],
    uint32_t flags, const uint8_t target_mac[6], size_t *frame_length) {
    return net_ndp_build(frame, capacity, source_mac, destination_mac,
                         source_address, destination_address,
                         NET_ICMPV6_NEIGHBOR_ADVERTISEMENT, target_address, flags,
                         target_mac, frame_length);
}

static kstatus_t net_ndp_parse(const uint8_t *frame, size_t frame_length,
                               uint8_t type, net_ndp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *icmp;
    uint16_t payload_length;
    if (frame == 0 || view == 0 || frame_length < NET_NDP_FRAME_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_IPV6) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    payload_length = net_load_be16(ip + 4U);
    if ((ip[0] >> 4) != 6U || ip[6] != NET_ICMPV6_NEXT_HEADER || ip[7] != 255U ||
        payload_length < 32U || NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
        payload_length > frame_length) return K_EIO;
    icmp = ip + NET_IPV6_HEADER_SIZE;
    if (icmp[0] != type || icmp[1] != 0U || net_icmpv6_checksum(ip + 8U, ip + 24U,
                                                                  icmp, payload_length) != 0U ||
        icmp[24] != (type == NET_ICMPV6_NEIGHBOR_SOLICITATION ?
                     NET_NDP_OPTION_SOURCE_LINK_LAYER : NET_NDP_OPTION_TARGET_LINK_LAYER) ||
        icmp[25] != 1U) return K_EIO;
    net_zero(view, sizeof(*view));
    net_copy(view->destination_mac, frame, 6U);
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->source_address, ip + 8U, 16U);
    net_copy(view->destination_address, ip + 24U, 16U);
    view->type = type;
    view->flags = type == NET_ICMPV6_NEIGHBOR_ADVERTISEMENT ? net_load_be32(icmp + 4U) : 0U;
    net_copy(view->target_address, icmp + 8U, 16U);
    net_copy(view->target_mac, icmp + 26U, 6U);
    view->has_link_layer_address = true;
    return K_OK;
}

kstatus_t net_ndp_parse_neighbor_solicitation(const uint8_t *frame,
                                              size_t frame_length,
                                              net_ndp_view_t *view) {
    return net_ndp_parse(frame, frame_length, NET_ICMPV6_NEIGHBOR_SOLICITATION, view);
}

kstatus_t net_ndp_parse_neighbor_advertisement(const uint8_t *frame,
                                               size_t frame_length,
                                               net_ndp_view_t *view) {
    return net_ndp_parse(frame, frame_length, NET_ICMPV6_NEIGHBOR_ADVERTISEMENT, view);
}

kstatus_t net_arp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint16_t operation, uint32_t sender_address,
                             const uint8_t sender_mac[6], uint32_t target_address,
                             const uint8_t target_mac[6], size_t *frame_length) {
    if (frame == 0 || capacity < NET_ARP_FRAME_SIZE || source_mac == 0 ||
        destination_mac == 0 || sender_mac == 0 || target_mac == 0 ||
        frame_length == 0 ||
        (operation != NET_ARP_OPERATION_REQUEST && operation != NET_ARP_OPERATION_REPLY)) {
        return K_EINVAL;
    }
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_ARP);
    net_store_be16(frame + 14U, 1U); /* Ethernet hardware type. */
    net_store_be16(frame + 16U, NET_ETHERTYPE_IPV4);
    frame[18] = 6U;
    frame[19] = 4U;
    net_store_be16(frame + 20U, operation);
    net_copy(frame + 22U, sender_mac, 6U);
    net_store_be32(frame + 28U, sender_address);
    net_copy(frame + 32U, target_mac, 6U);
    net_store_be32(frame + 38U, target_address);
    *frame_length = NET_ARP_FRAME_SIZE;
    return K_OK;
}

kstatus_t net_arp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_arp_view_t *view) {
    if (frame == 0 || view == 0 || frame_length < NET_ARP_FRAME_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_ARP ||
        net_load_be16(frame + 14U) != 1U || net_load_be16(frame + 16U) != NET_ETHERTYPE_IPV4 ||
        frame[18] != 6U || frame[19] != 4U ||
        (net_load_be16(frame + 20U) != NET_ARP_OPERATION_REQUEST &&
         net_load_be16(frame + 20U) != NET_ARP_OPERATION_REPLY)) return K_EIO;
    net_copy(view->destination_mac, frame, 6U);
    net_copy(view->source_mac, frame + 6U, 6U);
    view->operation = net_load_be16(frame + 20U);
    net_copy(view->sender_mac, frame + 22U, 6U);
    view->sender_address = net_load_be32(frame + 28U);
    net_copy(view->target_mac, frame + 32U, 6U);
    view->target_address = net_load_be32(frame + 38U);
    return K_OK;
}

kstatus_t net_udp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint32_t source_address,
                             uint32_t destination_address,
                             uint16_t source_port, uint16_t destination_port,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    uint32_t ip_length = NET_IPV4_HEADER_SIZE + NET_UDP_HEADER_SIZE + payload_length;
    uint32_t total_length = NET_ETHERNET_HEADER_SIZE + ip_length;
    uint8_t *ip;
    uint8_t *udp;
    if (frame == 0 || source_mac == 0 || destination_mac == 0 ||
        frame_length == 0 || (payload == 0 && payload_length != 0) ||
        ip_length > UINT16_MAX || total_length > capacity) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_IPV4);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV4_HEADER_SIZE);
    ip[0] = (NET_IPV4_VERSION << 4) | 5U;
    ip[1] = 0;
    net_store_be16(ip + 2U, (uint16_t)ip_length);
    net_store_be16(ip + 4U, 1U);
    net_store_be16(ip + 6U, 0U);
    ip[8] = 64U;
    ip[9] = NET_IPV4_UDP;
    net_store_be32(ip + 12U, source_address);
    net_store_be32(ip + 16U, destination_address);
    net_store_be16(ip + 10U, net_checksum16(ip, NET_IPV4_HEADER_SIZE));
    udp = ip + NET_IPV4_HEADER_SIZE;
    net_zero(udp, NET_UDP_HEADER_SIZE);
    net_store_be16(udp, source_port);
    net_store_be16(udp + 2U, destination_port);
    net_store_be16(udp + 4U, (uint16_t)(NET_UDP_HEADER_SIZE + payload_length));
    if (payload_length != 0) net_copy(udp + NET_UDP_HEADER_SIZE, payload, payload_length);
    net_store_be16(udp + 6U, net_udp_checksum(source_address, destination_address,
                                               udp,
                                               (uint16_t)(NET_UDP_HEADER_SIZE + payload_length)));
    if (net_load_be16(udp + 6U) == 0U) net_store_be16(udp + 6U, UINT16_MAX);
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_udp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_udp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *udp;
    uint32_t ip_header_length;
    uint16_t ip_total_length;
    uint16_t udp_length;
    if (frame == 0 || view == 0 || frame_length < NET_ETHERNET_HEADER_SIZE +
        NET_IPV4_HEADER_SIZE + NET_UDP_HEADER_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_IPV4) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    /* TOS/DSCP 是 IPv4 的合法服务质量字段，不能要求它必须为零。 */
    if ((ip[0] >> 4) != NET_IPV4_VERSION || (ip[0] & 0x0FU) < 5U) return K_EIO;
    ip_header_length = (uint32_t)(ip[0] & 0x0FU) * 4U;
    ip_total_length = net_load_be16(ip + 2U);
    if (ip_total_length < ip_header_length + NET_UDP_HEADER_SIZE ||
        NET_ETHERNET_HEADER_SIZE + ip_total_length > frame_length ||
        ip[9] != NET_IPV4_UDP ||
        net_checksum16(ip, ip_header_length) != 0U) return K_EIO;
    udp = ip + ip_header_length;
    udp_length = net_load_be16(udp + 4U);
    if (udp_length < NET_UDP_HEADER_SIZE || udp_length > ip_total_length - ip_header_length ||
        /* IPv4 允许 UDP 校验和为 0，表示发送端明确关闭校验和；DHCP 常使用此形式。 */
        (net_load_be16(udp + 6U) != 0U &&
         net_udp_checksum(net_load_be32(ip + 12U), net_load_be32(ip + 16U),
                          udp, udp_length) != 0U)) return K_EIO;
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->destination_mac, frame, 6U);
    view->source_address = net_load_be32(ip + 12U);
    view->destination_address = net_load_be32(ip + 16U);
    view->source_port = net_load_be16(udp);
    view->destination_port = net_load_be16(udp + 2U);
    view->payload = udp + NET_UDP_HEADER_SIZE;
    view->payload_length = (uint16_t)(udp_length - NET_UDP_HEADER_SIZE);
    return K_OK;
}

kstatus_t net_udp_build_ipv6(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             const uint8_t source_address[16],
                             const uint8_t destination_address[16],
                             uint16_t source_port, uint16_t destination_port,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    uint32_t udp_length = NET_UDP_HEADER_SIZE + (uint32_t)payload_length;
    uint32_t total_length = NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + udp_length;
    uint8_t *ip;
    uint8_t *udp;
    if (frame == 0 || source_mac == 0 || destination_mac == 0 ||
        source_address == 0 || destination_address == 0 || frame_length == 0 ||
        (payload == 0 && payload_length != 0) || udp_length > UINT16_MAX ||
        total_length > capacity) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, 0x86DDU);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV6_HEADER_SIZE);
    ip[0] = 0x60U;
    net_store_be16(ip + 4U, (uint16_t)udp_length);
    ip[6] = NET_IPV4_UDP;
    ip[7] = 64U;
    net_copy(ip + 8U, source_address, 16U);
    net_copy(ip + 24U, destination_address, 16U);
    udp = ip + NET_IPV6_HEADER_SIZE;
    net_zero(udp, NET_UDP_HEADER_SIZE);
    net_store_be16(udp, source_port);
    net_store_be16(udp + 2U, destination_port);
    net_store_be16(udp + 4U, (uint16_t)udp_length);
    if (payload_length != 0) net_copy(udp + NET_UDP_HEADER_SIZE,
                                      payload, payload_length);
    net_store_be16(udp + 6U, net_udp_checksum_ipv6(source_address,
                                                    destination_address,
                                                    udp, (uint16_t)udp_length));
    if (net_load_be16(udp + 6U) == 0U) net_store_be16(udp + 6U, UINT16_MAX);
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_udp_parse_ipv6(const uint8_t *frame, size_t frame_length,
                             net_ipv6_udp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *udp;
    uint16_t ip_payload_length;
    uint16_t udp_length;
    if (frame == 0 || view == 0 || frame_length < NET_ETHERNET_HEADER_SIZE +
        NET_IPV6_HEADER_SIZE + NET_UDP_HEADER_SIZE ||
        net_load_be16(frame + 12U) != 0x86DDU) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    ip_payload_length = net_load_be16(ip + 4U);
    if ((ip[0] >> 4) != 6U || ip[6] != NET_IPV4_UDP ||
        ip_payload_length < NET_UDP_HEADER_SIZE ||
        NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + ip_payload_length > frame_length) {
        return K_EIO;
    }
    udp = ip + NET_IPV6_HEADER_SIZE;
    udp_length = net_load_be16(udp + 4U);
    if (udp_length < NET_UDP_HEADER_SIZE || udp_length > ip_payload_length ||
        net_udp_checksum_ipv6(ip + 8U, ip + 24U, udp, udp_length) != 0U) return K_EIO;
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->destination_mac, frame, 6U);
    net_copy(view->source_address, ip + 8U, 16U);
    net_copy(view->destination_address, ip + 24U, 16U);
    view->source_port = net_load_be16(udp);
    view->destination_port = net_load_be16(udp + 2U);
    view->payload = udp + NET_UDP_HEADER_SIZE;
    view->payload_length = (uint16_t)(udp_length - NET_UDP_HEADER_SIZE);
    return K_OK;
}

static void net_tcp_store_header(uint8_t *tcp, uint16_t source_port,
                                 uint16_t destination_port, uint32_t sequence,
                                 uint32_t acknowledgement, uint8_t flags,
                                 uint16_t window) {
    net_zero(tcp, NET_TCP_HEADER_SIZE);
    net_store_be16(tcp, source_port);
    net_store_be16(tcp + 2U, destination_port);
    net_store_be32(tcp + 4U, sequence);
    net_store_be32(tcp + 8U, acknowledgement);
    tcp[12] = 5U << 4;
    tcp[13] = flags;
    net_store_be16(tcp + 14U, window);
}

kstatus_t net_tcp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint32_t source_address,
                             uint32_t destination_address,
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    uint32_t tcp_length = NET_TCP_HEADER_SIZE + (uint32_t)payload_length;
    uint32_t ip_length = NET_IPV4_HEADER_SIZE + tcp_length;
    uint32_t total_length = NET_ETHERNET_HEADER_SIZE + ip_length;
    uint8_t *ip;
    uint8_t *tcp;
    if (frame == 0 || source_mac == 0 || destination_mac == 0 || frame_length == 0 ||
        (payload == 0 && payload_length != 0U) || tcp_length > UINT16_MAX ||
        ip_length > UINT16_MAX || total_length > capacity) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_IPV4);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV4_HEADER_SIZE);
    ip[0] = (NET_IPV4_VERSION << 4) | 5U;
    net_store_be16(ip + 2U, (uint16_t)ip_length);
    net_store_be16(ip + 4U, 1U);
    ip[8] = 64U;
    ip[9] = NET_IPV4_TCP;
    net_store_be32(ip + 12U, source_address);
    net_store_be32(ip + 16U, destination_address);
    net_store_be16(ip + 10U, net_checksum16(ip, NET_IPV4_HEADER_SIZE));
    tcp = ip + NET_IPV4_HEADER_SIZE;
    net_tcp_store_header(tcp, source_port, destination_port, sequence,
                         acknowledgement, flags, window);
    if (payload_length != 0U) net_copy(tcp + NET_TCP_HEADER_SIZE, payload, payload_length);
    net_store_be16(tcp + 16U, net_tcp_checksum_ipv4(source_address,
                                                     destination_address,
                                                     tcp, (uint16_t)tcp_length));
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_tcp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_tcp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *tcp;
    uint32_t ip_header_length;
    uint16_t ip_total_length;
    uint32_t tcp_header_length;
    uint16_t tcp_length;
    if (frame == 0 || view == 0 || frame_length < NET_ETHERNET_HEADER_SIZE +
        NET_IPV4_HEADER_SIZE + NET_TCP_HEADER_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_IPV4) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    /* TOS/DSCP 由发送端设置，合法非零值不应导致 TCP 包被丢弃。 */
    if ((ip[0] >> 4) != NET_IPV4_VERSION || (ip[0] & 0x0FU) < 5U) {
        return K_EIO;
    }
    ip_header_length = (uint32_t)(ip[0] & 0x0FU) * 4U;
    ip_total_length = net_load_be16(ip + 2U);
    if (ip_total_length < ip_header_length + NET_TCP_HEADER_SIZE ||
        NET_ETHERNET_HEADER_SIZE + ip_total_length > frame_length || ip[9] != NET_IPV4_TCP ||
        net_checksum16(ip, ip_header_length) != 0U) return K_EIO;
    tcp = ip + ip_header_length;
    tcp_header_length = (uint32_t)(tcp[12] >> 4) * 4U;
    tcp_length = (uint16_t)(ip_total_length - ip_header_length);
    if (tcp_header_length < NET_TCP_HEADER_SIZE || tcp_header_length > tcp_length ||
        net_tcp_checksum_ipv4(net_load_be32(ip + 12U), net_load_be32(ip + 16U),
                              tcp, tcp_length) != 0U) return K_EIO;
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->destination_mac, frame, 6U);
    view->source_address = net_load_be32(ip + 12U);
    view->destination_address = net_load_be32(ip + 16U);
    view->source_port = net_load_be16(tcp);
    view->destination_port = net_load_be16(tcp + 2U);
    view->sequence = net_load_be32(tcp + 4U);
    view->acknowledgement = net_load_be32(tcp + 8U);
    view->header_length = (uint8_t)tcp_header_length;
    view->flags = tcp[13];
    view->window = net_load_be16(tcp + 14U);
    view->payload = tcp + tcp_header_length;
    view->payload_length = (uint16_t)(tcp_length - tcp_header_length);
    return K_OK;
}

kstatus_t net_tcp_build_ipv6(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             const uint8_t source_address[16],
                             const uint8_t destination_address[16],
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    uint32_t tcp_length = NET_TCP_HEADER_SIZE + (uint32_t)payload_length;
    uint32_t total_length = NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + tcp_length;
    uint8_t *ip;
    uint8_t *tcp;
    if (frame == 0 || source_mac == 0 || destination_mac == 0 || source_address == 0 ||
        destination_address == 0 || frame_length == 0 || (payload == 0 && payload_length != 0U) ||
        tcp_length > UINT16_MAX || total_length > capacity) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_IPV6);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV6_HEADER_SIZE);
    ip[0] = 0x60U;
    net_store_be16(ip + 4U, (uint16_t)tcp_length);
    ip[6] = NET_IPV4_TCP;
    ip[7] = 64U;
    net_copy(ip + 8U, source_address, 16U);
    net_copy(ip + 24U, destination_address, 16U);
    tcp = ip + NET_IPV6_HEADER_SIZE;
    net_tcp_store_header(tcp, source_port, destination_port, sequence,
                         acknowledgement, flags, window);
    if (payload_length != 0U) net_copy(tcp + NET_TCP_HEADER_SIZE, payload, payload_length);
    net_store_be16(tcp + 16U, net_tcp_checksum_ipv6(source_address, destination_address,
                                                    tcp, (uint16_t)tcp_length));
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_tcp_parse_ipv6(const uint8_t *frame, size_t frame_length,
                             net_tcp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *tcp;
    uint16_t ip_payload_length;
    uint32_t tcp_header_length;
    uint16_t tcp_length;
    if (frame == 0 || view == 0 || frame_length < NET_ETHERNET_HEADER_SIZE +
        NET_IPV6_HEADER_SIZE + NET_TCP_HEADER_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_IPV6) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    ip_payload_length = net_load_be16(ip + 4U);
    if ((ip[0] >> 4) != 6U || ip[6] != NET_IPV4_TCP ||
        ip_payload_length < NET_TCP_HEADER_SIZE ||
        NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + ip_payload_length > frame_length) {
        return K_EIO;
    }
    tcp = ip + NET_IPV6_HEADER_SIZE;
    tcp_length = ip_payload_length;
    tcp_header_length = (uint32_t)(tcp[12] >> 4) * 4U;
    if (tcp_header_length < NET_TCP_HEADER_SIZE || tcp_header_length > tcp_length ||
        net_tcp_checksum_ipv6(ip + 8U, ip + 24U, tcp, tcp_length) != 0U) return K_EIO;
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->destination_mac, frame, 6U);
    net_copy(view->source_address6, ip + 8U, 16U);
    net_copy(view->destination_address6, ip + 24U, 16U);
    view->source_port = net_load_be16(tcp);
    view->destination_port = net_load_be16(tcp + 2U);
    view->sequence = net_load_be32(tcp + 4U);
    view->acknowledgement = net_load_be32(tcp + 8U);
    view->header_length = (uint8_t)tcp_header_length;
    view->flags = tcp[13];
    view->window = net_load_be16(tcp + 14U);
    view->payload = tcp + tcp_header_length;
    view->payload_length = (uint16_t)(tcp_length - tcp_header_length);
    return K_OK;
}

static void net_firewall_lock(net_firewall_t *firewall) {
    while (atomic_exchange_explicit(&firewall->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void net_firewall_unlock(net_firewall_t *firewall) {
    atomic_store_explicit(&firewall->lock.state, 0U, memory_order_release);
}

static bool net_firewall_prefix_matches(const uint8_t left[16],
                                        const uint8_t right[16],
                                        uint8_t prefix_length,
                                        uint8_t maximum_length) {
    uint8_t full_bytes;
    uint8_t remainder;
    if (prefix_length > maximum_length) return false;
    full_bytes = (uint8_t)(prefix_length / 8U);
    remainder = (uint8_t)(prefix_length & 7U);
    for (uint8_t index = 0; index < full_bytes; ++index) {
        if (left[index] != right[index]) return false;
    }
    if (remainder != 0U) {
        uint8_t mask = (uint8_t)(0xFFU << (8U - remainder));
        if ((left[full_bytes] & mask) != (right[full_bytes] & mask)) return false;
    }
    return true;
}

static bool net_firewall_rule_matches(const net_firewall_rule_t *rule,
                                      const net_firewall_packet_t *packet) {
    uint8_t maximum_prefix = packet->family == OS_AF_INET4 ? 32U : 128U;
    if (!rule->enabled || rule->family != packet->family ||
        (rule->direction != 0U && rule->direction != packet->direction) ||
        (rule->protocol != 0U && rule->protocol != packet->protocol) ||
        (rule->source_port != 0U && rule->source_port != packet->source_port) ||
        (rule->destination_port != 0U &&
         rule->destination_port != packet->destination_port) ||
        !net_firewall_prefix_matches(rule->source_address, packet->source_address,
                                     rule->source_prefix_length, maximum_prefix) ||
        !net_firewall_prefix_matches(rule->destination_address,
                                     packet->destination_address,
                                     rule->destination_prefix_length, maximum_prefix)) {
        return false;
    }
    return true;
}

void net_firewall_init(net_firewall_t *firewall, uint8_t default_action) {
    if (firewall == 0) return;
    net_zero(firewall, sizeof(*firewall));
    atomic_init(&firewall->lock.state, 0U);
    firewall->default_action = default_action == NET_FIREWALL_ALLOW ?
                               NET_FIREWALL_ALLOW : NET_FIREWALL_DENY;
}

kstatus_t net_firewall_add(net_firewall_t *firewall,
                           const net_firewall_rule_t *rule,
                           uint32_t *rule_id) {
    if (firewall == 0 || rule == 0 || rule_id == 0 || !rule->enabled ||
        (rule->action != NET_FIREWALL_ALLOW && rule->action != NET_FIREWALL_DENY) ||
        (rule->family != OS_AF_INET4 && rule->family != OS_AF_INET6) ||
        rule->direction > NET_FIREWALL_EGRESS || rule->source_prefix_length > 128U ||
        rule->destination_prefix_length > 128U) return K_EINVAL;
    if (rule->family == OS_AF_INET4 &&
        (rule->source_prefix_length > 32U || rule->destination_prefix_length > 32U)) {
        return K_EINVAL;
    }
    net_firewall_lock(firewall);
    for (uint32_t index = 0; index < NET_FIREWALL_MAX_RULES; ++index) {
        if (firewall->rules[index].enabled) continue;
        firewall->rules[index] = *rule;
        firewall->rules[index].enabled = true;
        *rule_id = index;
        net_firewall_unlock(firewall);
        return K_OK;
    }
    net_firewall_unlock(firewall);
    return K_ENOMEM;
}

kstatus_t net_firewall_remove(net_firewall_t *firewall, uint32_t rule_id) {
    if (firewall == 0 || rule_id >= NET_FIREWALL_MAX_RULES) return K_EINVAL;
    net_firewall_lock(firewall);
    if (!firewall->rules[rule_id].enabled) {
        net_firewall_unlock(firewall);
        return K_ENOENT;
    }
    firewall->rules[rule_id].enabled = false;
    net_firewall_unlock(firewall);
    return K_OK;
}

bool net_firewall_check(net_firewall_t *firewall,
                        const net_firewall_packet_t *packet) {
    bool allowed;
    if (firewall == 0 || packet == 0 ||
        (packet->family != OS_AF_INET4 && packet->family != OS_AF_INET6) ||
        (packet->direction != NET_FIREWALL_INGRESS &&
         packet->direction != NET_FIREWALL_EGRESS)) return false;
    net_firewall_lock(firewall);
    allowed = firewall->default_action == NET_FIREWALL_ALLOW;
    for (uint32_t index = 0; index < NET_FIREWALL_MAX_RULES; ++index) {
        if (!net_firewall_rule_matches(&firewall->rules[index], packet)) continue;
        allowed = firewall->rules[index].action == NET_FIREWALL_ALLOW;
        break;
    }
    net_firewall_unlock(firewall);
    return allowed;
}

static void net_cache_lock(net_arp_cache_t *cache) {
    while (atomic_exchange_explicit(&cache->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void net_cache_unlock(net_arp_cache_t *cache) {
    atomic_store_explicit(&cache->lock.state, 0U, memory_order_release);
}

static void net_neighbor_lock(net_ipv6_neighbor_cache_t *cache) {
    while (atomic_exchange_explicit(&cache->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void net_neighbor_unlock(net_ipv6_neighbor_cache_t *cache) {
    atomic_store_explicit(&cache->lock.state, 0U, memory_order_release);
}

void net_arp_cache_init(net_arp_cache_t *cache) {
    if (cache == 0) return;
    net_zero(cache, sizeof(*cache));
    atomic_init(&cache->lock.state, 0U);
}

kstatus_t net_arp_cache_update(net_arp_cache_t *cache, uint32_t address,
                               const uint8_t mac[6], uint64_t timestamp) {
    if (cache == 0 || mac == 0 || address == 0U) return K_EINVAL;
    net_cache_lock(cache);
    uint32_t slot = NET_ARP_CACHE_SIZE;
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        if (cache->entries[i].valid && cache->entries[i].address == address) {
            slot = i;
            break;
        }
    }
    if (slot == NET_ARP_CACHE_SIZE) {
        for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
            if (!cache->entries[i].valid) {
                slot = i;
                break;
            }
        }
    }
    if (slot == NET_ARP_CACHE_SIZE) {
        slot = cache->next_replacement++ % NET_ARP_CACHE_SIZE;
    }
    cache->entries[slot].valid = true;
    cache->entries[slot].address = address;
    net_copy(cache->entries[slot].mac, mac, 6U);
    cache->entries[slot].updated = timestamp;
    net_cache_unlock(cache);
    return K_OK;
}

bool net_arp_cache_lookup(net_arp_cache_t *cache, uint32_t address,
                          uint8_t mac[6]) {
    bool found = false;
    if (cache == 0 || mac == 0 || address == 0U) return false;
    net_cache_lock(cache);
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        if (!cache->entries[i].valid || cache->entries[i].address != address) continue;
        net_copy(mac, cache->entries[i].mac, 6U);
        found = true;
        break;
    }
    net_cache_unlock(cache);
    return found;
}

void net_ipv6_neighbor_cache_init(net_ipv6_neighbor_cache_t *cache) {
    if (cache == 0) return;
    net_zero(cache, sizeof(*cache));
    atomic_init(&cache->lock.state, 0U);
}

kstatus_t net_ipv6_neighbor_cache_update(net_ipv6_neighbor_cache_t *cache,
                                          const uint8_t address[16],
                                          const uint8_t mac[6],
                                          uint64_t timestamp) {
    uint32_t slot = NET_NDP_CACHE_SIZE;
    if (cache == 0 || address == 0 || mac == 0 || net_address6_zero(address)) {
        return K_EINVAL;
    }
    net_neighbor_lock(cache);
    for (uint32_t index = 0U; index < NET_NDP_CACHE_SIZE; ++index) {
        if (cache->entries[index].valid &&
            net_address6_equal(cache->entries[index].address, address)) {
            slot = index;
            break;
        }
    }
    if (slot == NET_NDP_CACHE_SIZE) {
        for (uint32_t index = 0U; index < NET_NDP_CACHE_SIZE; ++index) {
            if (!cache->entries[index].valid) {
                slot = index;
                break;
            }
        }
    }
    if (slot == NET_NDP_CACHE_SIZE) {
        slot = cache->next_replacement++ % NET_NDP_CACHE_SIZE;
    }
    cache->entries[slot].valid = true;
    net_copy(cache->entries[slot].address, address, 16U);
    net_copy(cache->entries[slot].mac, mac, 6U);
    cache->entries[slot].updated = timestamp;
    net_neighbor_unlock(cache);
    return K_OK;
}

bool net_ipv6_neighbor_cache_lookup(net_ipv6_neighbor_cache_t *cache,
                                    const uint8_t address[16], uint8_t mac[6]) {
    bool found = false;
    if (cache == 0 || address == 0 || mac == 0 || net_address6_zero(address)) return false;
    net_neighbor_lock(cache);
    for (uint32_t index = 0U; index < NET_NDP_CACHE_SIZE; ++index) {
        if (!cache->entries[index].valid ||
            !net_address6_equal(cache->entries[index].address, address)) continue;
        net_copy(mac, cache->entries[index].mac, 6U);
        found = true;
        break;
    }
    net_neighbor_unlock(cache);
    return found;
}

bool net_core_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 1};
    static const uint8_t destination_mac[6] = {0x02, 0, 0, 0, 0, 2};
    static const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t frame[128];
    uint8_t cached_mac[6];
    net_udp_view_t view;
    net_arp_cache_t cache;
    net_device_t device;
    size_t frame_length = 0;
    net_device_init(&device, "test", source_mac, 1500U, 0, 0);
    if (device.mtu != 1500U || device.max_frame_size != 1518U ||
        device.transmit != 0 || net_device_send(&device, frame, 1U) != K_EINVAL) {
        return false;
    }
    if (net_udp_build_ipv4(frame, sizeof(frame), source_mac, destination_mac,
                           0xC0A80002U, 0xC0A80001U, 1234U, 4321U,
                           payload, sizeof(payload), &frame_length) != K_OK ||
        net_udp_parse_ipv4(frame, frame_length, &view) != K_OK ||
        view.source_address != 0xC0A80002U || view.destination_address != 0xC0A80001U ||
        view.source_port != 1234U || view.destination_port != 4321U ||
        view.payload_length != sizeof(payload) ||
        !net_mac_equal(view.source_mac, source_mac) ||
        !net_mac_equal(view.destination_mac, destination_mac)) return false;
    frame[NET_ETHERNET_HEADER_SIZE + 10U] ^= 1U;
    if (net_udp_parse_ipv4(frame, frame_length, &view) == K_OK) return false;
    net_arp_cache_init(&cache);
    if (net_arp_cache_update(&cache, 0xC0A80001U, destination_mac, 1U) != K_OK ||
        !net_arp_cache_lookup(&cache, 0xC0A80001U, cached_mac) ||
        !net_mac_equal(cached_mac, destination_mac)) return false;
    return !net_arp_cache_lookup(&cache, 0xC0A800FEU, cached_mac);
}

bool net_arp_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 7};
    static const uint8_t destination_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t empty_mac[6] = {0, 0, 0, 0, 0, 0};
    uint8_t frame[64];
    net_arp_view_t view;
    size_t frame_length = 0U;
    if (net_arp_build_ipv4(frame, sizeof(frame), source_mac, destination_mac,
                           NET_ARP_OPERATION_REQUEST, 0xC0A80002U, source_mac,
                           0xC0A80001U, empty_mac, &frame_length) != K_OK ||
        frame_length != NET_ARP_FRAME_SIZE || net_arp_parse_ipv4(frame, frame_length,
                                                                  &view) != K_OK ||
        view.operation != NET_ARP_OPERATION_REQUEST || view.sender_address != 0xC0A80002U ||
        view.target_address != 0xC0A80001U || !net_mac_equal(view.sender_mac, source_mac) ||
        !net_mac_equal(view.target_mac, empty_mac)) return false;
    frame[21] = 3U;
    return net_arp_parse_ipv4(frame, frame_length, &view) != K_OK;
}

bool net_ipv6_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 3};
    static const uint8_t destination_mac[6] = {0x02, 0, 0, 0, 0, 4};
    static const uint8_t source_address[16] = {
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    };
    static const uint8_t destination_address[16] = {
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2
    };
    static const uint8_t payload[] = {'v', '6'};
    uint8_t frame[128];
    net_ipv6_udp_view_t view;
    size_t frame_length = 0;
    if (net_udp_build_ipv6(frame, sizeof(frame), source_mac, destination_mac,
                           source_address, destination_address, 1234U, 4321U,
                           payload, sizeof(payload), &frame_length) != K_OK ||
        net_udp_parse_ipv6(frame, frame_length, &view) != K_OK ||
        view.source_port != 1234U || view.destination_port != 4321U ||
        view.payload_length != sizeof(payload)) return false;
    for (uint32_t index = 0; index < 16U; ++index) {
        if (view.source_address[index] != source_address[index] ||
            view.destination_address[index] != destination_address[index]) return false;
    }
    if (view.payload[0] != payload[0] || view.payload[1] != payload[1]) return false;
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + NET_UDP_HEADER_SIZE] ^= 1U;
    return net_udp_parse_ipv6(frame, frame_length, &view) != K_OK;
}

bool net_ndp_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 7};
    static const uint8_t destination_mac[6] = {0x33, 0x33, 0xFF, 0, 0, 2};
    static const uint8_t source_address[16] = {
        0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 1
    };
    static const uint8_t target_address[16] = {
        0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 2
    };
    uint8_t solicited_node[16];
    uint8_t multicast_mac[6];
    uint8_t frame[NET_NDP_FRAME_SIZE];
    uint8_t cached_mac[6];
    net_ndp_view_t view;
    net_ipv6_neighbor_cache_t cache;
    size_t frame_length = 0U;
    net_ndp_solicited_node_address(target_address, solicited_node);
    net_ndp_multicast_mac(solicited_node, multicast_mac);
    if (!net_mac_equal(multicast_mac, destination_mac) ||
        net_ndp_build_neighbor_solicitation(frame, sizeof(frame), source_mac,
                                            multicast_mac, source_address,
                                            solicited_node, target_address,
                                            &frame_length) != K_OK ||
        frame_length != NET_NDP_FRAME_SIZE ||
        net_ndp_parse_neighbor_solicitation(frame, frame_length, &view) != K_OK ||
        !net_address6_equal(view.target_address, target_address) ||
        !net_mac_equal(view.target_mac, source_mac)) return false;
    if (net_ndp_build_neighbor_advertisement(frame, sizeof(frame), source_mac,
                                             source_mac, target_address,
                                             source_address, target_address,
                                             NET_NDP_FLAG_SOLICITED |
                                             NET_NDP_FLAG_OVERRIDE,
                                             source_mac, &frame_length) != K_OK ||
        net_ndp_parse_neighbor_advertisement(frame, frame_length, &view) != K_OK ||
        view.flags != (NET_NDP_FLAG_SOLICITED | NET_NDP_FLAG_OVERRIDE) ||
        !net_address6_equal(view.target_address, target_address) ||
        !net_mac_equal(view.target_mac, source_mac)) return false;
    net_ipv6_neighbor_cache_init(&cache);
    if (net_ipv6_neighbor_cache_update(&cache, target_address, source_mac, 1U) != K_OK ||
        !net_ipv6_neighbor_cache_lookup(&cache, target_address, cached_mac) ||
        !net_mac_equal(cached_mac, source_mac)) return false;
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + 8U] ^= 1U;
    return net_ndp_parse_neighbor_advertisement(frame, frame_length, &view) != K_OK;
}

bool net_tcp_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 5};
    static const uint8_t destination_mac[6] = {0x02, 0, 0, 0, 0, 6};
    static const uint8_t source_address6[16] = {
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5
    };
    static const uint8_t destination_address6[16] = {
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6
    };
    static const uint8_t payload[] = {'t', 'c', 'p'};
    uint8_t frame[256];
    net_tcp_view_t view;
    size_t frame_length = 0;
    if (net_tcp_build_ipv4(frame, sizeof(frame), source_mac, destination_mac,
                           0xC0A80002U, 0xC0A80001U, 1234U, 4321U,
                           100U, 200U, NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK,
                           4096U, payload, sizeof(payload), &frame_length) != K_OK ||
        net_tcp_parse_ipv4(frame, frame_length, &view) != K_OK ||
        view.source_address != 0xC0A80002U || view.destination_address != 0xC0A80001U ||
        view.source_port != 1234U || view.destination_port != 4321U ||
        view.sequence != 100U || view.acknowledgement != 200U ||
        view.flags != (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK) ||
        view.window != 4096U || view.payload_length != sizeof(payload) ||
        view.payload[0] != payload[0] || view.payload[1] != payload[1] ||
        view.payload[2] != payload[2]) return false;
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE + NET_TCP_HEADER_SIZE] ^= 1U;
    if (net_tcp_parse_ipv4(frame, frame_length, &view) == K_OK) return false;
    if (net_tcp_build_ipv6(frame, sizeof(frame), source_mac, destination_mac,
                           source_address6, destination_address6, 1234U, 4321U,
                           300U, 400U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                           8192U, payload, sizeof(payload), &frame_length) != K_OK ||
        net_tcp_parse_ipv6(frame, frame_length, &view) != K_OK ||
        view.source_port != 1234U || view.destination_port != 4321U ||
        view.sequence != 300U || view.acknowledgement != 400U ||
        view.flags != (NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH) ||
        view.window != 8192U || view.payload_length != sizeof(payload)) return false;
    for (uint32_t index = 0U; index < 16U; ++index) {
        if (view.source_address6[index] != source_address6[index] ||
            view.destination_address6[index] != destination_address6[index]) return false;
    }
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + NET_TCP_HEADER_SIZE] ^= 1U;
    return net_tcp_parse_ipv6(frame, frame_length, &view) != K_OK;
}

bool net_firewall_self_test(void) {
    net_firewall_t firewall;
    net_firewall_rule_t rule = {0};
    net_firewall_packet_t packet = {0};
    uint32_t rule_id = UINT32_MAX;
    net_firewall_init(&firewall, NET_FIREWALL_DENY);
    rule.enabled = true;
    rule.direction = NET_FIREWALL_INGRESS;
    rule.protocol = NET_IPV4_UDP;
    rule.family = OS_AF_INET4;
    rule.destination_port = 53U;
    rule.action = NET_FIREWALL_ALLOW;
    if (net_firewall_add(&firewall, &rule, &rule_id) != K_OK ||
        rule_id >= NET_FIREWALL_MAX_RULES) return false;
    packet.family = OS_AF_INET4;
    packet.direction = NET_FIREWALL_INGRESS;
    packet.protocol = NET_IPV4_UDP;
    packet.destination_port = 53U;
    if (!net_firewall_check(&firewall, &packet)) return false;
    packet.destination_port = 54U;
    if (net_firewall_check(&firewall, &packet) ||
        net_firewall_remove(&firewall, rule_id) != K_OK ||
        net_firewall_remove(&firewall, rule_id) != K_ENOENT) return false;
    return !net_firewall_check(&firewall, &packet);
}
