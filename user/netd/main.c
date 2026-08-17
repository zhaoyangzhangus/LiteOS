#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

#define NETMGR_DHCP_CLIENT_PORT 68U
#define NETMGR_DHCP_SERVER_PORT 67U
#define NETMGR_DHCP_BROADCAST  0xFFFFFFFFU
#define NETMGR_DHCP_COOKIE     0x63825363U
#define NETMGR_DHCP_PACKET_SIZE 576U
#define NETMGR_DHCP_POLL_NS    250000000ULL
#define NETMGR_DHCP_RECEIVE_ATTEMPTS 8U
#define NETMGR_DHCP_RETRY_NS   2000000000ULL
#define NETMGR_DHCP_OPTION_END 255U

static int64_t netmgr_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t netmgr_syscall_three(uint64_t number, uint64_t arg0,
                                    uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

/* socket syscall 使用完整的 x86_64 原始六参数约定。 */
static int64_t netmgr_syscall_six(uint64_t number, uint64_t arg0,
                                  uint64_t arg1, uint64_t arg2,
                                  uint64_t arg3, uint64_t arg4,
                                  uint64_t arg5) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    register uint64_t r9 __asm__("r9") = arg5;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static void netmgr_exit(uint64_t status) {
    (void)netmgr_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static int64_t netmgr_get_status(os_net_status_t *status) {
    if (status == 0) return -22;
    status->hdr.size = sizeof(*status);
    status->hdr.version = OS_SYSCALL_ABI_VERSION;
    status->hdr.flags = 0U;
    return netmgr_syscall_one(OS_SYS_NET_GET_STATUS, (uint64_t)status);
}

static int64_t netmgr_set_ipv4(uint32_t address, uint8_t prefix_length,
                               uint32_t gateway) {
    os_net_set_ipv4_config_t config = {0};
    config.hdr.size = sizeof(config);
    config.hdr.version = OS_SYSCALL_ABI_VERSION;
    config.hdr.flags = 0U;
    config.address = address;
    config.gateway = gateway;
    config.prefix_length = prefix_length;
    return netmgr_syscall_one(OS_SYS_NET_SET_IPV4, (uint64_t)&config);
}

static void netmgr_delay(uint64_t timeout_ns) {
    volatile uint32_t word = 0U;
    (void)netmgr_syscall_three(OS_SYS_FUTEX_WAIT, (uint64_t)&word, 0U,
                               timeout_ns);
}

static void put_be16(uint8_t *buffer, uint16_t value) {
    buffer[0] = (uint8_t)(value >> 8U);
    buffer[1] = (uint8_t)value;
}

static void put_be32(uint8_t *buffer, uint32_t value) {
    buffer[0] = (uint8_t)(value >> 24U);
    buffer[1] = (uint8_t)(value >> 16U);
    buffer[2] = (uint8_t)(value >> 8U);
    buffer[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t *buffer) {
    return ((uint32_t)buffer[0] << 24U) | ((uint32_t)buffer[1] << 16U) |
           ((uint32_t)buffer[2] << 8U) | buffer[3];
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t length) {
    while (length-- != 0U) *destination++ = *source++;
}

static bool append_option(uint8_t *packet, size_t capacity, size_t *offset,
                          uint8_t type, const void *value, uint8_t length) {
    if (packet == 0 || offset == 0 || (value == 0 && length != 0U) ||
        *offset > capacity || capacity - *offset < (size_t)length + 2U) {
        return false;
    }
    packet[(*offset)++] = type;
    packet[(*offset)++] = length;
    copy_bytes(packet + *offset, (const uint8_t *)value, length);
    *offset += length;
    return true;
}

static bool append_byte_option(uint8_t *packet, size_t capacity, size_t *offset,
                               uint8_t type, uint8_t value) {
    return append_option(packet, capacity, offset, type, &value, 1U);
}

static bool append_u32_option(uint8_t *packet, size_t capacity, size_t *offset,
                              uint8_t type, uint32_t value) {
    uint8_t encoded[4];
    put_be32(encoded, value);
    return append_option(packet, capacity, offset, type, encoded, sizeof(encoded));
}

static bool build_dhcp_packet(uint8_t *packet, size_t capacity, uint32_t xid,
                              const uint8_t mac[6], uint8_t message_type,
                              uint32_t requested_address, uint32_t server_id,
                              size_t *length) {
    static const uint8_t parameter_request_list[] = {1U, 3U, 6U, 15U, 51U, 54U,
                                                     58U, 59U};
    uint8_t client_identifier[7] = {1U, 0U, 0U, 0U, 0U, 0U, 0U};
    size_t offset = 240U;
    if (packet == 0 || mac == 0 || length == 0 || capacity < offset + 1U ||
        message_type == 0U) return false;
    for (size_t index = 0U; index < capacity; ++index) packet[index] = 0U;
    packet[0] = 1U;                 /* BOOTP request */
    packet[1] = 1U;                 /* Ethernet */
    packet[2] = 6U;                 /* MAC length */
    put_be32(packet + 4U, xid);
    put_be16(packet + 10U, 0x8000U); /* 广播接收 DHCP OFFER/ACK */
    copy_bytes(packet + 28U, mac, 6U);
    copy_bytes(client_identifier + 1U, mac, 6U);
    put_be32(packet + 236U, NETMGR_DHCP_COOKIE);
    if (!append_byte_option(packet, capacity, &offset, 53U, message_type) ||
        !append_option(packet, capacity, &offset, 55U, parameter_request_list,
                       (uint8_t)sizeof(parameter_request_list)) ||
        !append_option(packet, capacity, &offset, 61U,
                       client_identifier, (uint8_t)sizeof(client_identifier))) {
        return false;
    }
    if (requested_address != 0U &&
        !append_u32_option(packet, capacity, &offset, 50U, requested_address)) {
        return false;
    }
    if (server_id != 0U &&
        !append_u32_option(packet, capacity, &offset, 54U, server_id)) {
        return false;
    }
    if (offset >= capacity) return false;
    packet[offset++] = NETMGR_DHCP_OPTION_END;
    *length = offset;
    return true;
}

typedef struct dhcp_offer {
    uint32_t address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t server_id;
    uint8_t message_type;
    bool has_subnet_mask;
    bool has_server_id;
} dhcp_offer_t;

static bool parse_dhcp_packet(const uint8_t *packet, size_t length, uint32_t xid,
                              dhcp_offer_t *offer) {
    size_t offset = 240U;
    if (packet == 0 || offer == 0 || length < offset || packet[0] != 2U ||
        packet[1] != 1U || packet[2] != 6U || get_be32(packet + 4U) != xid ||
        get_be32(packet + 236U) != NETMGR_DHCP_COOKIE) return false;
    offer->address = get_be32(packet + 16U);
    offer->subnet_mask = 0U;
    offer->gateway = 0U;
    offer->server_id = 0U;
    offer->message_type = 0U;
    offer->has_subnet_mask = false;
    offer->has_server_id = false;
    while (offset < length) {
        uint8_t type = packet[offset++];
        uint8_t option_length;
        if (type == 0U) continue;
        if (type == NETMGR_DHCP_OPTION_END) break;
        if (offset >= length) return false;
        option_length = packet[offset++];
        if (option_length > length - offset) return false;
        if (type == 53U && option_length == 1U) {
            offer->message_type = packet[offset];
        } else if (type == 1U && option_length == 4U) {
            offer->subnet_mask = get_be32(packet + offset);
            offer->has_subnet_mask = true;
        } else if (type == 3U && option_length >= 4U) {
            offer->gateway = get_be32(packet + offset);
        } else if (type == 54U && option_length == 4U) {
            offer->server_id = get_be32(packet + offset);
            offer->has_server_id = true;
        }
        offset += option_length;
    }
    return offer->message_type != 0U && offer->address != 0U;
}

static bool prefix_from_mask(uint32_t mask, uint8_t *prefix) {
    bool saw_zero = false;
    uint8_t count = 0U;
    if (prefix == 0) return false;
    for (int32_t bit = 31; bit >= 0; --bit) {
        if ((mask & (1U << (uint32_t)bit)) != 0U) {
            if (saw_zero) return false;
            ++count;
        } else {
            saw_zero = true;
        }
    }
    *prefix = count;
    return true;
}

static int64_t socket_create(os_handle_t *handle) {
    if (handle == 0) return -22;
    *handle = OS_INVALID_HANDLE;
    return netmgr_syscall_six(OS_SYS_SOCKET_CREATE, OS_AF_INET4, OS_SOCK_DGRAM,
                              0U, (uint64_t)handle, 0U, 0U);
}

static int64_t socket_bind(os_handle_t handle, uint32_t address, uint16_t port) {
    return netmgr_syscall_three(OS_SYS_SOCKET_BIND, handle, address, port);
}

static int64_t socket_send(os_handle_t handle, const uint8_t *packet, size_t length,
                           uint32_t address, uint16_t port) {
    return netmgr_syscall_six(OS_SYS_SOCKET_SEND, handle, (uint64_t)packet, length,
                              address, port, 0U);
}

static int64_t socket_receive(os_handle_t handle, uint8_t *packet, size_t capacity,
                              uint64_t timeout_ns) {
    uint32_t source_address = 0U;
    uint16_t source_port = 0U;
    return netmgr_syscall_six(OS_SYS_SOCKET_RECV, handle, (uint64_t)packet, capacity,
                              (uint64_t)&source_address, (uint64_t)&source_port,
                              timeout_ns);
}

/*
 * 网卡轮询是延迟工作，旧事务的 DHCP 包可能在当前事务之后才进入 socket
 * 队列。这里在一个接收窗口内持续读取，只接受匹配 xid 和消息类型的包。
 */
static bool receive_dhcp_message(os_handle_t socket, uint8_t *packet, size_t capacity,
                                 uint32_t xid, uint8_t message_type,
                                 dhcp_offer_t *offer) {
    for (uint32_t attempt = 0U; attempt < NETMGR_DHCP_RECEIVE_ATTEMPTS; ++attempt) {
        int64_t received = socket_receive(socket, packet, capacity,
                                          NETMGR_DHCP_POLL_NS);
        if (received <= 0 || !parse_dhcp_packet(packet, (size_t)received, xid, offer) ||
            offer->message_type != message_type) continue;
        return true;
    }
    return false;
}

static bool dhcp_exchange(os_handle_t socket, const os_net_status_t *status,
                          uint32_t xid, uint32_t *address, uint8_t *prefix,
                          uint32_t *gateway) {
    uint8_t packet[NETMGR_DHCP_PACKET_SIZE];
    uint8_t reply[NETMGR_DHCP_PACKET_SIZE];
    dhcp_offer_t offer;
    size_t packet_length = 0U;
    if (status == 0 || address == 0 || prefix == 0 || gateway == 0 ||
        !build_dhcp_packet(packet, sizeof(packet), xid, status->mac, 1U, 0U, 0U,
                           &packet_length) ||
        socket_send(socket, packet, packet_length, NETMGR_DHCP_BROADCAST,
                    NETMGR_DHCP_SERVER_PORT) < 0) return false;
    if (!receive_dhcp_message(socket, reply, sizeof(reply), xid, 2U, &offer) ||
        !offer.has_subnet_mask || !offer.has_server_id ||
        !prefix_from_mask(offer.subnet_mask, prefix)) return false;
    if (!build_dhcp_packet(packet, sizeof(packet), xid, status->mac, 3U,
                           offer.address, offer.server_id, &packet_length) ||
        socket_send(socket, packet, packet_length, NETMGR_DHCP_BROADCAST,
                    NETMGR_DHCP_SERVER_PORT) < 0) return false;
    if (!receive_dhcp_message(socket, reply, sizeof(reply), xid, 5U, &offer) ||
        !offer.has_subnet_mask || !prefix_from_mask(offer.subnet_mask, prefix)) return false;
    *address = offer.address;
    *gateway = offer.gateway;
    return *address != 0U;
}

__attribute__((noreturn)) void netd_entry(void) {
    os_net_status_t status = {0};
    os_handle_t socket = OS_INVALID_HANDLE;
    uint32_t transaction = 0x4C544F53U;
    if (netmgr_get_status(&status) < 0) netmgr_exit(1U);
    for (;;) {
        uint32_t address;
        uint32_t gateway;
        uint8_t prefix;
        bool link_up;
        if (netmgr_get_status(&status) < 0) netmgr_exit(1U);
        link_up = (status.flags & OS_NET_STATUS_LINK_UP) != 0U;
        if (!link_up) {
            if (socket != OS_INVALID_HANDLE) {
                (void)netmgr_syscall_one(OS_SYS_HANDLE_CLOSE, socket);
                socket = OS_INVALID_HANDLE;
            }
            if (status.ipv4_address != 0U) (void)netmgr_set_ipv4(0U, 0U, 0U);
            netmgr_delay(NETMGR_DHCP_RETRY_NS);
            continue;
        }
        if (status.ipv4_address != 0U && status.ipv4_prefix_length != 0U) {
            netmgr_delay(100000000ULL);
            continue;
        }
        if (socket == OS_INVALID_HANDLE &&
            (socket_create(&socket) < 0 || socket == OS_INVALID_HANDLE ||
             socket_bind(socket, 0U, NETMGR_DHCP_CLIENT_PORT) < 0)) {
            if (socket != OS_INVALID_HANDLE) {
                (void)netmgr_syscall_one(OS_SYS_HANDLE_CLOSE, socket);
                socket = OS_INVALID_HANDLE;
            }
            netmgr_delay(NETMGR_DHCP_RETRY_NS);
            continue;
        }
        ++transaction;
        if (dhcp_exchange(socket, &status, transaction, &address, &prefix, &gateway) &&
            netmgr_set_ipv4(address, prefix, gateway) == 0) {
            (void)netmgr_syscall_one(OS_SYS_HANDLE_CLOSE, socket);
            socket = OS_INVALID_HANDLE;
            netmgr_delay(100000000ULL);
        } else {
            netmgr_delay(NETMGR_DHCP_RETRY_NS);
        }
    }
}
