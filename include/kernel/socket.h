#pragma once

#include <kernel/base.h>
#include <kernel/net.h>
#include <uapi/socket.h>

struct completion_port;

#define KOBJECT_TYPE_SOCKET 0x0105U

#define SOCKET_RIGHT_READ  (1U << 0)
#define SOCKET_RIGHT_WRITE (1U << 1)
#define SOCKET_RIGHT_WAIT  (1U << 31)
#define SOCKET_RIGHT_ALL   (SOCKET_RIGHT_READ | SOCKET_RIGHT_WRITE | SOCKET_RIGHT_WAIT)

#define SOCKET_PROTOCOL_UDP 17U
#define SOCKET_PROTOCOL_TCP 6U
#define SOCKET_RX_QUEUE_DEPTH 16U
#define SOCKET_MAX_PAYLOAD 4096U
#define SOCKET_LISTEN_BACKLOG 8U
#define SOCKET_STREAM_BUFFER_SIZE 8192U
#define SOCKET_TCP_RETRANSMIT_TIMEOUT_NS 200000000ULL
#define SOCKET_TCP_MAX_RETRANSMITS 5U

typedef struct socket_ipv4_endpoint {
    uint32_t address;
    uint16_t port;
    uint16_t reserved;
} socket_ipv4_endpoint_t;

typedef struct socket_ipv6_endpoint {
    uint8_t address[16];
    uint16_t port;
    uint16_t reserved;
} socket_ipv6_endpoint_t;

/* TCP 被动握手返回给网卡驱动的控制报文参数。 */
typedef struct socket_tcp_reply {
    bool valid;
    uint8_t flags;
    uint16_t window;
    uint32_t sequence;
    uint32_t acknowledgement;
} socket_tcp_reply_t;

/* TCP 输出只暴露三层以上的报文参数，具体二层封装由网卡后端负责。 */
typedef kstatus_t (*socket_tcp_ipv4_output_fn)(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length);

/* IPv6 TCP 输出只把三层以上参数交给网卡后端，二层封装由后端完成。 */
typedef kstatus_t (*socket_tcp_ipv6_output_fn)(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length);

/* UDP 输出只携带三层参数，邻居解析和二层封装由网卡后端完成。 */
typedef kstatus_t (*socket_udp_ipv4_output_fn)(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    const void *payload, size_t payload_length);

typedef kstatus_t (*socket_udp_ipv6_output_fn)(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    const void *payload, size_t payload_length);

kstatus_t socket_create(uint16_t family, uint16_t type, uint16_t protocol,
                        socket_t **out);
kstatus_t socket_close(socket_t *socket);
kstatus_t socket_bind(socket_t *socket, uint32_t address, uint16_t port);
kstatus_t socket_connect(socket_t *socket, uint32_t address, uint16_t port);
kstatus_t socket_listen(socket_t *socket, uint32_t backlog);
kstatus_t socket_accept(socket_t *socket, uint64_t timeout_ns, socket_t **out);
kstatus_t socket_send(socket_t *socket, const void *buffer, size_t length,
                      uint32_t address, uint16_t port, uint64_t *bytes);
kstatus_t socket_recv(socket_t *socket, void *buffer, size_t length,
                      socket_ipv4_endpoint_t *source, uint64_t timeout_ns,
                      uint64_t *bytes);
kstatus_t socket_send_async(socket_t *socket, const void *buffer, size_t length,
                            uint32_t address, uint16_t port,
                            struct completion_port *completion_port,
                            uint64_t user_key, uint64_t *request_id);

kstatus_t socket_bind_ipv6(socket_t *socket, const uint8_t address[16], uint16_t port);
kstatus_t socket_connect_ipv6(socket_t *socket, const uint8_t address[16], uint16_t port);
kstatus_t socket_send_ipv6(socket_t *socket, const void *buffer, size_t length,
                           const uint8_t address[16], uint16_t port, uint64_t *bytes);
kstatus_t socket_recv_ipv6(socket_t *socket, void *buffer, size_t length,
                           socket_ipv6_endpoint_t *source, uint64_t timeout_ns,
                           uint64_t *bytes);
kstatus_t socket_shutdown(socket_t *socket, uint32_t how);
kstatus_t socket_get_info(socket_t *socket, os_socket_info_t *info);
kstatus_t socket_get_option(socket_t *socket, uint32_t option, int32_t *value);
kstatus_t socket_set_option(socket_t *socket, uint32_t option, int32_t value);
kstatus_t socket_send_async_ipv6(socket_t *socket, const void *buffer, size_t length,
                                 const uint8_t address[16], uint16_t port,
                                 struct completion_port *completion_port,
                                 uint64_t user_key, uint64_t *request_id);

/* 网卡驱动和 loopback 使用同一个入口把已经校验的 UDP 数据交给 socket 层。 */
kstatus_t socket_inject_udp(uint32_t source_address, uint16_t source_port,
                            uint32_t destination_address, uint16_t destination_port,
                            const void *buffer, size_t length);
kstatus_t socket_inject_udp_ipv6(const uint8_t source_address[16], uint16_t source_port,
                                 const uint8_t destination_address[16],
                                 uint16_t destination_port,
                                 const void *buffer, size_t length);
kstatus_t socket_inject_tcp_ipv4(uint32_t source_address, uint16_t source_port,
                                 uint32_t destination_address,
                                 uint16_t destination_port, uint32_t sequence,
                                 uint8_t flags, const void *buffer, size_t length);
kstatus_t socket_inject_tcp_ipv4_reply(uint32_t source_address, uint16_t source_port,
                                       uint32_t destination_address,
                                       uint16_t destination_port, uint32_t sequence,
                                       uint8_t flags, const void *buffer, size_t length,
                                       socket_tcp_reply_t *reply);
kstatus_t socket_inject_tcp_syn_ipv4(uint32_t source_address, uint16_t source_port,
                                     uint32_t destination_address,
                                     uint16_t destination_port, uint32_t sequence,
                                     socket_tcp_reply_t *reply);
kstatus_t socket_inject_tcp_ack_ipv4(uint32_t source_address, uint16_t source_port,
                                     uint32_t destination_address,
                                     uint16_t destination_port, uint32_t sequence,
                                     uint32_t acknowledgement, uint8_t flags);
kstatus_t socket_inject_tcp_ack_ipv4_window(uint32_t source_address,
                                            uint16_t source_port,
                                            uint32_t destination_address,
                                            uint16_t destination_port,
                                            uint32_t sequence,
                                            uint32_t acknowledgement,
                                            uint8_t flags, uint16_t window);
kstatus_t socket_inject_tcp_ack_ipv4_reply(uint32_t source_address,
                                           uint16_t source_port,
                                           uint32_t destination_address,
                                           uint16_t destination_port,
                                           uint32_t sequence,
                                           uint32_t acknowledgement,
                                           uint8_t flags, uint16_t window,
                                           socket_tcp_reply_t *reply);

kstatus_t socket_inject_tcp_ipv6_reply(const uint8_t source_address[16],
                                       uint16_t source_port,
                                       const uint8_t destination_address[16],
                                       uint16_t destination_port, uint32_t sequence,
                                       uint8_t flags, const void *buffer, size_t length,
                                       socket_tcp_reply_t *reply);
kstatus_t socket_inject_tcp_syn_ipv6(const uint8_t source_address[16],
                                     uint16_t source_port,
                                     const uint8_t destination_address[16],
                                     uint16_t destination_port, uint32_t sequence,
                                     socket_tcp_reply_t *reply);
kstatus_t socket_inject_tcp_ack_ipv6_reply(const uint8_t source_address[16],
                                           uint16_t source_port,
                                           const uint8_t destination_address[16],
                                           uint16_t destination_port, uint32_t sequence,
                                           uint32_t acknowledgement, uint8_t flags,
                                           uint16_t window, socket_tcp_reply_t *reply);
kstatus_t socket_inject_tcp_ack_ipv6_window(const uint8_t source_address[16],
                                            uint16_t source_port,
                                            const uint8_t destination_address[16],
                                            uint16_t destination_port, uint32_t sequence,
                                            uint32_t acknowledgement, uint8_t flags,
                                            uint16_t window);

void socket_set_tcp_ipv4_output(socket_tcp_ipv4_output_fn output, void *context);
void socket_set_tcp_ipv6_output(socket_tcp_ipv6_output_fn output, void *context);
void socket_set_udp_ipv4_output(socket_udp_ipv4_output_fn output, void *context);
void socket_set_udp_ipv6_output(socket_udp_ipv6_output_fn output, void *context);
void socket_tcp_poll(uint64_t now_tsc);

bool socket_self_test(void);
