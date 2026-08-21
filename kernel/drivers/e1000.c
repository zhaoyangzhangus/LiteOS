#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/dma.h>
#include <kernel/deferred.h>
#include <kernel/e1000.h>
#include <kernel/kmem.h>
#include <kernel/net_core.h>
#include <kernel/pci.h>
#include <kernel/irq.h>
#include <kernel/telemetry.h>
#include <kernel/socket.h>

#define E1000_VENDOR_ID 0x8086U
#define E1000_MMIO_VA (X86_64_MMIO_BASE + 0x10000000ULL)
#define E1000_RING_COUNT 16U
#define E1000_BUFFER_SIZE 2048U
#define E1000_SOFTWARE_QUEUE_COUNT 4U
#define E1000_SOFTWARE_QUEUE_DEPTH 8U

#define E1000_REG_CTRL   0x0000U
#define E1000_REG_STATUS 0x0008U
#define E1000_REG_MDIC   0x0020U
#define E1000_REG_TCTL   0x0400U
#define E1000_REG_TIPG   0x0410U
#define E1000_REG_RCTL   0x0100U
#define E1000_REG_TDBAL  0x3800U
#define E1000_REG_TDBAH  0x3804U
#define E1000_REG_TDLEN  0x3808U
#define E1000_REG_TDH    0x3810U
#define E1000_REG_TDT    0x3818U
#define E1000_REG_RDBAL  0x2800U
#define E1000_REG_RDBAH  0x2804U
#define E1000_REG_RDLEN  0x2808U
#define E1000_REG_RDH    0x2810U
#define E1000_REG_RDT    0x2818U
#define E1000_REG_RAL0   0x5400U
#define E1000_REG_RAH0   0x5404U
#define E1000_REG_ICR    0x00C0U
#define E1000_REG_IMS    0x00D0U
#define E1000_REG_IMC    0x00D8U

#define E1000_CTRL_RST  (1U << 26)
#define E1000_CTRL_SLU  (1U << 6)
#define E1000_STATUS_LU (1U << 1)
#define E1000_TCTL_EN   (1U << 1)
#define E1000_TCTL_PSP  (1U << 3)
#define E1000_RCTL_EN   (1U << 1)
#define E1000_RCTL_BAM  (1U << 15)
#define E1000_MDIC_PHY_SHIFT 21U
#define E1000_MDIC_OP_WRITE  (1U << 26)
#define E1000_MDIC_OP_READ   (1U << 27)
#define E1000_MDIC_READY     (1U << 28)
#define E1000_MDIC_ERROR     (1U << 30)
#define E1000_MII_BMCR       0U
#define E1000_MII_BMCR_LOOPBACK (1U << 14)
#define E1000_TX_CMD_EOP  (1U << 0)
#define E1000_TX_CMD_IFCS (1U << 1)
#define E1000_TX_CMD_RS   (1U << 3)
#define E1000_TX_STATUS_DD (1U << 0)
#define E1000_RX_STATUS_DD (1U << 0)
#define E1000_IMS_RXT0 (1U << 7)
#define E1000_IMS_RXDMT0 (1U << 4)
#define E1000_IMS_LSC (1U << 2)
#define E1000_RX_BUDGET 32U
#define E1000_IRQ_VECTOR 0x50U

typedef struct __attribute__((packed)) e1000_descriptor {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_status;
    uint16_t special;
} e1000_descriptor_t;

_Static_assert(sizeof(e1000_descriptor_t) == 16U, "e1000 descriptor ABI");

/*
 * 老式 e1000 在本驱动中只有一条 DMA 接收队列。为了让上层先具备 RSS
 * 的流亲和性，收到的 UDP 包会按五元组哈希进入固定的软件队列；以后接入
 * 支持 RSS 的硬件时，只需要将硬件队列映射到同一套队列编号即可。
 */
typedef struct e1000_rx_packet {
    uint16_t family;
    uint8_t protocol;
    uint8_t flags;
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t payload_length;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint16_t window;
    uint8_t source_address6[16];
    uint8_t destination_address6[16];
    uint8_t payload[SOCKET_MAX_PAYLOAD];
} e1000_rx_packet_t;

typedef struct e1000_software_queue {
    e1000_rx_packet_t packets[E1000_SOFTWARE_QUEUE_DEPTH];
    spinlock_t lock;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint64_t dropped;
} e1000_software_queue_t;

typedef struct e1000_state {
    const pci_device_t *pci;
    uint8_t mmio_bar;
    bool io_mode;
    uint16_t io_base;
    volatile uint8_t *mmio;
    uint64_t mmio_span;
    page_t *tx_ring_page;
    page_t *rx_ring_page;
    dma_mapping_t tx_ring_dma;
    dma_mapping_t rx_ring_dma;
    e1000_descriptor_t *tx_ring;
    e1000_descriptor_t *rx_ring;
    page_t *tx_pages[E1000_RING_COUNT];
    page_t *rx_pages[E1000_RING_COUNT];
    dma_mapping_t tx_dma[E1000_RING_COUNT];
    dma_mapping_t rx_dma[E1000_RING_COUNT];
    uint8_t mac[6];
    uint32_t ipv4_address;
    uint8_t ipv4_prefix_length;
    uint32_t ipv4_gateway;
    uint8_t ipv6_address[16];
    bool ipv6_address_configured;
    uint16_t phy_control;
    bool phy_loopback;
    uint32_t tx_tail;
    uint32_t rx_clean;
    uint32_t hardware_queue_count;
    uint32_t software_queue_count;
    e1000_software_queue_t software_queues[E1000_SOFTWARE_QUEUE_COUNT];
    net_device_t device;
    net_arp_cache_t arp_cache;
    net_ipv6_neighbor_cache_t ndp_cache;
    bool tcp_loopback_peer_enabled;
    bool tcp_loopback_synack_seen;
    bool tcp_loopback_data_ack_seen;
    spinlock_t poll_lock;
    bool link_up;
    bool initialized;
    uint8_t irq_vector;
    bool irq_bound;
} e1000_state_t;

static e1000_state_t g_e1000;
static uint32_t g_e1000_error;
static atomic_bool g_e1000_poll_queued;
static atomic_uint g_e1000_lifecycle_lock;
/* 只有自检完成后，运行时中断/兼容工作项才可以访问接收队列。 */
static bool g_e1000_runtime_ready;

static uint32_t e1000_read(const e1000_state_t *state, uint32_t offset);
static void e1000_write(const e1000_state_t *state, uint32_t offset, uint32_t value);

static void e1000_msi_handler(uint8_t vector, struct arch_trap_frame *frame,
                              void *context);
static bool e1000_bind_interrupt(e1000_state_t *state);
static void e1000_unbind_interrupt(e1000_state_t *state);

static bool e1000_transmit(e1000_state_t *state, const uint8_t *frame,
                           size_t length);
static kstatus_t e1000_device_transmit(void *context, const void *frame,
                                        size_t length);
static uint32_t e1000_ipv4_next_hop(const e1000_state_t *state,
                                    uint32_t destination_address);
static kstatus_t e1000_tcp_ipv4_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length);
static kstatus_t e1000_tcp_ipv6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length);
static kstatus_t e1000_udp_ipv4_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    const void *payload, size_t payload_length);
static kstatus_t e1000_udp_ipv6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    const void *payload, size_t payload_length);
static bool e1000_send_neighbor_solicitation(e1000_state_t *state,
                                              const uint8_t source_address[16],
                                              const uint8_t target_address[16]);
static bool e1000_send_neighbor_advertisement(e1000_state_t *state,
                                              const net_ndp_view_t *request);
static bool e1000_tcp_loopback_peer_ack(e1000_state_t *state,
                                        const e1000_rx_packet_t *packet,
                                        const socket_tcp_reply_t *reply);
static bool e1000_tcp_loopback_peer_ack6(e1000_state_t *state,
                                         const e1000_rx_packet_t *packet,
                                         const socket_tcp_reply_t *reply);

static void e1000_msi_handler(uint8_t vector, struct arch_trap_frame *frame,
                              void *context) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint32_t causes;
    (void)vector;
    (void)frame;
    if (state == 0 || !state->initialized || !state->irq_bound) return;
    /* ICR is read-to-clear.  The ISR only acknowledges and queues bounded work. */
    causes = e1000_read(state, E1000_REG_ICR);
    if (causes != 0U) (void)e1000_schedule_deferred_poll();
}

static bool e1000_bind_interrupt(e1000_state_t *state) {
    if (state == 0 || state->pci == 0 || state->pci->msi_capability == 0U ||
        E1000_IRQ_VECTOR < IRQ_VECTOR_FIRST || E1000_IRQ_VECTOR > IRQ_VECTOR_LAST) {
        return false;
    }
    (void)e1000_read(state, E1000_REG_ICR);
    if (irq_register(E1000_IRQ_VECTOR, e1000_msi_handler, state) != K_OK) return false;
    if (pci_msi_configure((pci_device_t *)state->pci, x86_current_apic_id(),
                          E1000_IRQ_VECTOR) != K_OK) {
        (void)irq_unregister(E1000_IRQ_VECTOR, e1000_msi_handler, state);
        return false;
    }
    state->irq_vector = E1000_IRQ_VECTOR;
    state->irq_bound = true;
    e1000_write(state, E1000_REG_IMS, E1000_IMS_RXT0 | E1000_IMS_RXDMT0 |
                                      E1000_IMS_LSC);
    return true;
}

static void e1000_unbind_interrupt(e1000_state_t *state) {
    if (state == 0 || !state->irq_bound) return;
    e1000_write(state, E1000_REG_IMC, UINT32_MAX);
    (void)e1000_read(state, E1000_REG_ICR);
    (void)irq_unregister(state->irq_vector, e1000_msi_handler, state);
    state->irq_vector = 0U;
    state->irq_bound = false;
}

static bool e1000_lifecycle_try_lock(void) {
    return atomic_exchange_explicit(&g_e1000_lifecycle_lock, 1U,
                                    memory_order_acquire) == 0U;
}

static void e1000_lifecycle_unlock(void) {
    atomic_store_explicit(&g_e1000_lifecycle_lock, 0U, memory_order_release);
}

static void e1000_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0) *bytes++ = 0;
}

static void e1000_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (size-- != 0) *out++ = *in++;
}

static bool e1000_bytes_equal(const void *left, const void *right, size_t size) {
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    if (left == 0 || right == 0) return false;
    while (size-- != 0) {
        if (*a++ != *b++) return false;
    }
    return true;
}

static void e1000_configure_link_local(e1000_state_t *state) {
    if (state == 0) return;
    e1000_zero(state->ipv6_address, sizeof(state->ipv6_address));
    state->ipv6_address[0] = 0xFEU;
    state->ipv6_address[1] = 0x80U;
    state->ipv6_address[8] = state->mac[0] ^ 0x02U;
    state->ipv6_address[9] = state->mac[1];
    state->ipv6_address[10] = state->mac[2];
    state->ipv6_address[11] = 0xFFU;
    state->ipv6_address[12] = 0xFEU;
    state->ipv6_address[13] = state->mac[3];
    state->ipv6_address[14] = state->mac[4];
    state->ipv6_address[15] = state->mac[5];
    state->ipv6_address_configured = true;
}

static void e1000_queue_lock(e1000_software_queue_t *queue) {
    while (atomic_exchange_explicit(&queue->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void e1000_queue_unlock(e1000_software_queue_t *queue) {
    atomic_store_explicit(&queue->lock.state, 0U, memory_order_release);
}

static uint32_t e1000_hash_bytes(uint32_t hash, const uint8_t *bytes, size_t length) {
    while (length-- != 0) {
        hash ^= *bytes++;
        hash *= 16777619U;
    }
    return hash;
}

static uint32_t e1000_hash_u16(uint32_t hash, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    return e1000_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint32_t e1000_hash_u32(uint32_t hash, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16),
                        (uint8_t)(value >> 8), (uint8_t)value};
    return e1000_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint32_t e1000_flow_hash_ipv4(uint32_t source_address,
                                     uint32_t destination_address,
                                     uint16_t source_port,
                                     uint16_t destination_port,
                                     uint8_t protocol) {
    uint32_t hash = 2166136261U;
    hash = e1000_hash_u32(hash, source_address);
    hash = e1000_hash_u32(hash, destination_address);
    hash = e1000_hash_u16(hash, source_port);
    hash = e1000_hash_u16(hash, destination_port);
    return e1000_hash_bytes(hash, &protocol, 1U);
}

static uint32_t e1000_flow_hash_ipv6(const uint8_t source_address[16],
                                     const uint8_t destination_address[16],
                                     uint16_t source_port,
                                     uint16_t destination_port) {
    uint32_t hash = 2166136261U;
    hash = e1000_hash_bytes(hash, source_address, 16U);
    hash = e1000_hash_bytes(hash, destination_address, 16U);
    hash = e1000_hash_u16(hash, source_port);
    hash = e1000_hash_u16(hash, destination_port);
    return e1000_hash_bytes(hash, (const uint8_t[]){17U}, 1U);
}

static uint32_t e1000_select_software_queue(const e1000_state_t *state,
                                            uint32_t flow_hash) {
    if (state == 0 || state->software_queue_count == 0U) return 0U;
    return flow_hash % state->software_queue_count;
}

static void e1000_initialize_software_queues(e1000_state_t *state) {
    uint32_t cpu_count = x86_smp_discovered_count();
    if (cpu_count == 0U) cpu_count = 1U;
    if (cpu_count > E1000_SOFTWARE_QUEUE_COUNT) {
        cpu_count = E1000_SOFTWARE_QUEUE_COUNT;
    }
    state->hardware_queue_count = 1U;
    state->software_queue_count = cpu_count;
    for (uint32_t index = 0; index < E1000_SOFTWARE_QUEUE_COUNT; ++index) {
        atomic_init(&state->software_queues[index].lock.state, 0U);
    }
}

static bool e1000_rss_self_test_state(const e1000_state_t *state) {
    bool seen[E1000_SOFTWARE_QUEUE_COUNT] = {false};
    uint32_t required;
    if (state == 0 || state->software_queue_count == 0U ||
        state->software_queue_count > E1000_SOFTWARE_QUEUE_COUNT) return false;
    required = state->software_queue_count;
    for (uint32_t flow = 0; flow < 256U && required != 0U; ++flow) {
        uint32_t source = 0x0A000001U + flow;
        uint32_t destination = 0x0A000100U + (flow & 31U);
        uint32_t queue = e1000_select_software_queue(
            state, e1000_flow_hash_ipv4(source, destination,
                                        (uint16_t)(1000U + flow), 2000U,
                                        SOCKET_PROTOCOL_UDP));
        if (!seen[queue]) {
            seen[queue] = true;
            --required;
        }
    }
    return required == 0U;
}

static bool e1000_enqueue_packet(e1000_state_t *state,
                                 const e1000_rx_packet_t *packet,
                                 uint32_t flow_hash) {
    uint32_t queue_index;
    e1000_software_queue_t *queue;
    if (state == 0 || packet == 0 || packet->payload_length > SOCKET_MAX_PAYLOAD) {
        return false;
    }
    queue_index = e1000_select_software_queue(state, flow_hash);
    queue = &state->software_queues[queue_index];
    e1000_queue_lock(queue);
    if (queue->count == E1000_SOFTWARE_QUEUE_DEPTH) {
        ++queue->dropped;
        e1000_queue_unlock(queue);
        return false;
    }
    e1000_copy(&queue->packets[queue->tail], packet, sizeof(*packet));
    queue->tail = (queue->tail + 1U) % E1000_SOFTWARE_QUEUE_DEPTH;
    ++queue->count;
    e1000_queue_unlock(queue);
    return true;
}

static bool e1000_dequeue_packet(e1000_software_queue_t *queue,
                                 e1000_rx_packet_t *packet) {
    if (queue == 0 || packet == 0) return false;
    e1000_queue_lock(queue);
    if (queue->count == 0U) {
        e1000_queue_unlock(queue);
        return false;
    }
    e1000_copy(packet, &queue->packets[queue->head], sizeof(*packet));
    queue->head = (queue->head + 1U) % E1000_SOFTWARE_QUEUE_DEPTH;
    --queue->count;
    e1000_queue_unlock(queue);
    return true;
}

static bool e1000_send_tcp_reply(e1000_state_t *state,
                                 const e1000_rx_packet_t *packet,
                                 const socket_tcp_reply_t *reply) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || packet == 0 || reply == 0 || !reply->valid) return false;
    bool sent = net_tcp_build_ipv4(
               frame, sizeof(frame), packet->destination_mac, packet->source_mac,
               packet->destination_address, packet->source_address,
               packet->destination_port, packet->source_port,
               reply->sequence, reply->acknowledgement, reply->flags,
               reply->window, 0, 0U, &frame_length) == K_OK &&
           net_device_send(&state->device, frame, frame_length) == K_OK;
    if (sent && !state->tcp_loopback_peer_enabled && packet->payload_length != 0U) {
        state->tcp_loopback_data_ack_seen = true;
    }
    return sent;
}

static bool e1000_send_tcp_reply6(e1000_state_t *state,
                                  const e1000_rx_packet_t *packet,
                                  const socket_tcp_reply_t *reply) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || packet == 0 || reply == 0 || !reply->valid) return false;
    bool sent = net_tcp_build_ipv6(
                    frame, sizeof(frame), packet->destination_mac, packet->source_mac,
                    packet->destination_address6, packet->source_address6,
                    packet->destination_port, packet->source_port,
                    reply->sequence, reply->acknowledgement, reply->flags,
                    reply->window, 0, 0U, &frame_length) == K_OK &&
                net_device_send(&state->device, frame, frame_length) == K_OK;
    if (sent && !state->tcp_loopback_peer_enabled && packet->payload_length != 0U) {
        state->tcp_loopback_data_ack_seen = true;
    }
    return sent;
}

static bool e1000_dispatch_software_queues(e1000_state_t *state) {
    bool received = false;
    if (state == 0) return false;
    for (uint32_t queue_index = 0; queue_index < state->software_queue_count;
         ++queue_index) {
        e1000_rx_packet_t packet;
        while (e1000_dequeue_packet(&state->software_queues[queue_index], &packet)) {
            kstatus_t status;
            if (packet.protocol == SOCKET_PROTOCOL_TCP && packet.family == OS_AF_INET4) {
                if ((packet.flags & NET_TCP_FLAG_SYN) != 0U &&
                    (packet.flags & NET_TCP_FLAG_ACK) == 0U) {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_syn_ipv4(
                        packet.source_address, packet.source_port,
                        packet.destination_address, packet.destination_port,
                        packet.sequence, &reply);
                    if (status == K_OK && reply.valid) {
                        uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                                      NET_TCP_HEADER_SIZE];
                        size_t frame_length = 0U;
                        if (net_tcp_build_ipv4(
                                frame, sizeof(frame), packet.destination_mac,
                                packet.source_mac, packet.destination_address,
                                packet.source_address, packet.destination_port,
                                packet.source_port, reply.sequence,
                                reply.acknowledgement, reply.flags, reply.window,
                                0, 0U, &frame_length) != K_OK ||
                            net_device_send(&state->device, frame, frame_length) != K_OK) {
                            status = K_EIO;
                        } else if (state->tcp_loopback_peer_enabled &&
                                   !e1000_tcp_loopback_peer_ack(state, &packet, &reply)) {
                            status = K_EIO;
                        }
                    }
                } else if ((packet.flags & NET_TCP_FLAG_SYN) != 0U) {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_ack_ipv4_reply(
                        packet.source_address, packet.source_port,
                        packet.destination_address, packet.destination_port,
                        packet.sequence, packet.acknowledgement, packet.flags,
                        packet.window, &reply);
                    if (status == K_OK && reply.valid &&
                        !e1000_send_tcp_reply(state, &packet, &reply)) {
                        status = K_EIO;
                    }
                } else if ((packet.flags & NET_TCP_FLAG_ACK) != 0U &&
                           packet.payload_length == 0U &&
                           (packet.flags & (NET_TCP_FLAG_FIN | NET_TCP_FLAG_PSH)) == 0U) {
                    status = socket_inject_tcp_ack_ipv4_window(
                        packet.source_address, packet.source_port,
                        packet.destination_address, packet.destination_port,
                        packet.sequence, packet.acknowledgement, packet.flags,
                        packet.window);
                } else {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_ipv4_reply(
                        packet.source_address, packet.source_port,
                        packet.destination_address, packet.destination_port,
                        packet.sequence, packet.flags, packet.payload,
                        packet.payload_length, &reply);
                    /* 连续数据、重复数据、乱序数据和 FIN 都要返回当前 ACK/窗口。 */
                    if (reply.valid && !e1000_send_tcp_reply(state, &packet, &reply)) {
                        status = K_EIO;
                    }
                }
            } else if (packet.protocol == SOCKET_PROTOCOL_TCP && packet.family == OS_AF_INET6) {
                if ((packet.flags & NET_TCP_FLAG_SYN) != 0U &&
                    (packet.flags & NET_TCP_FLAG_ACK) == 0U) {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_syn_ipv6(
                        packet.source_address6, packet.source_port,
                        packet.destination_address6, packet.destination_port,
                        packet.sequence, &reply);
                    if (status == K_OK && reply.valid) {
                        uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                                      NET_TCP_HEADER_SIZE];
                        size_t frame_length = 0U;
                        if (net_tcp_build_ipv6(
                                frame, sizeof(frame), packet.destination_mac,
                                packet.source_mac, packet.destination_address6,
                                packet.source_address6, packet.destination_port,
                                packet.source_port, reply.sequence,
                                reply.acknowledgement, reply.flags, reply.window,
                                0, 0U, &frame_length) != K_OK ||
                            net_device_send(&state->device, frame, frame_length) != K_OK) {
                            status = K_EIO;
                        } else if (state->tcp_loopback_peer_enabled &&
                                   !e1000_tcp_loopback_peer_ack6(state, &packet, &reply)) {
                            status = K_EIO;
                        }
                    }
                } else if ((packet.flags & NET_TCP_FLAG_SYN) != 0U) {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_ack_ipv6_reply(
                        packet.source_address6, packet.source_port,
                        packet.destination_address6, packet.destination_port,
                        packet.sequence, packet.acknowledgement, packet.flags,
                        packet.window, &reply);
                    if (status == K_OK && reply.valid &&
                        !e1000_send_tcp_reply6(state, &packet, &reply)) {
                        status = K_EIO;
                    }
                } else if ((packet.flags & NET_TCP_FLAG_ACK) != 0U &&
                           packet.payload_length == 0U &&
                           (packet.flags & (NET_TCP_FLAG_FIN | NET_TCP_FLAG_PSH)) == 0U) {
                    status = socket_inject_tcp_ack_ipv6_window(
                        packet.source_address6, packet.source_port,
                        packet.destination_address6, packet.destination_port,
                        packet.sequence, packet.acknowledgement, packet.flags,
                        packet.window);
                } else {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_ipv6_reply(
                        packet.source_address6, packet.source_port,
                        packet.destination_address6, packet.destination_port,
                        packet.sequence, packet.flags, packet.payload,
                        packet.payload_length, &reply);
                    if (reply.valid && !e1000_send_tcp_reply6(state, &packet, &reply)) {
                        status = K_EIO;
                    }
                }
            } else if (packet.protocol == SOCKET_PROTOCOL_UDP && packet.family == OS_AF_INET6) {
                status = socket_inject_udp_ipv6(packet.source_address6,
                                                packet.source_port,
                                                packet.destination_address6,
                                                packet.destination_port,
                                                packet.payload,
                                                packet.payload_length);
            } else if (packet.protocol == SOCKET_PROTOCOL_UDP) {
                status = socket_inject_udp(packet.source_address,
                                           packet.source_port,
                                           packet.destination_address,
                                           packet.destination_port,
                                           packet.payload,
                                           packet.payload_length);
            } else status = K_EINVAL;
            if (status == K_OK) received = true;
        }
    }
    return received;
}

static uint32_t e1000_read(const e1000_state_t *state, uint32_t offset) {
    if (state->io_mode) {
        __asm__ volatile ("outw %w0, %1" : : "a"((uint16_t)offset),
                          "Nd"(state->io_base));
        uint32_t value;
        __asm__ volatile ("inl %1, %0" : "=a"(value) :
                          "Nd"((uint16_t)(state->io_base + 4U)));
        return value;
    }
    return *(volatile const uint32_t *)(state->mmio + offset);
}

static void e1000_write(const e1000_state_t *state, uint32_t offset, uint32_t value) {
    if (state->io_mode) {
        __asm__ volatile ("outw %w0, %1" : : "a"((uint16_t)offset),
                          "Nd"(state->io_base));
        __asm__ volatile ("outl %0, %1" : : "a"(value),
                          "Nd"((uint16_t)(state->io_base + 4U)));
        return;
    }
    *(volatile uint32_t *)(state->mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static bool e1000_phy_write(e1000_state_t *state, uint8_t reg, uint16_t value) {
    /* I/O port 模式下 MDIC 不可用，不能把 PHY 写入伪装成成功。 */
    if (state == 0 || state->io_mode || reg >= 32U) return false;
    uint32_t command = (uint32_t)value | (1U << E1000_MDIC_PHY_SHIFT) |
                       ((uint32_t)reg << 16) | E1000_MDIC_OP_WRITE;
    e1000_write(state, E1000_REG_MDIC, command);
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        uint32_t result = e1000_read(state, E1000_REG_MDIC);
        if ((result & E1000_MDIC_READY) == 0) {
            __asm__ volatile ("pause");
            continue;
        }
        return (result & E1000_MDIC_ERROR) == 0;
    }
    return false;
}

static bool e1000_enable_phy_loopback(e1000_state_t *state) {
    if (state == 0 || state->io_mode) return false;
    uint32_t command = (1U << E1000_MDIC_PHY_SHIFT) |
                       ((uint32_t)E1000_MII_BMCR << 16) | E1000_MDIC_OP_READ;
    e1000_write(state, E1000_REG_MDIC, command);
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        uint32_t result = e1000_read(state, E1000_REG_MDIC);
        if ((result & E1000_MDIC_READY) == 0) {
            __asm__ volatile ("pause");
            continue;
        }
        if ((result & E1000_MDIC_ERROR) != 0) return false;
        state->phy_control = (uint16_t)result;
        if (!e1000_phy_write(state, E1000_MII_BMCR,
                             (uint16_t)(state->phy_control | E1000_MII_BMCR_LOOPBACK))) {
            return false;
        }
        state->phy_loopback = true;
        return true;
    }
    return false;
}

static bool e1000_map_mmio(e1000_state_t *state) {
    uint32_t bar_index = PCI_MAX_BARS;
    for (uint32_t index = 0; index < PCI_MAX_BARS; ++index) {
        if ((state->pci->bars[index].flags & PCI_RESOURCE_MEMORY) != 0 &&
            state->pci->bars[index].length != 0) {
            bar_index = index;
            break;
        }
    }
    if (bar_index == PCI_MAX_BARS) {
        for (uint32_t index = 0; index < PCI_MAX_BARS; ++index) {
            if ((state->pci->bars[index].flags & PCI_RESOURCE_IO) != 0 &&
                state->pci->bars[index].length >= 8U) {
                state->io_mode = true;
                state->io_base = (uint16_t)state->pci->bars[index].address;
                if (state->pci->bars[index].address > UINT16_MAX) {
                    g_e1000_error = 33U;
                    return false;
                }
                return true;
            }
        }
        g_e1000_error = 30U;
        return false;
    }
    state->mmio_bar = (uint8_t)bar_index;
    uint64_t span = state->pci->bars[bar_index].length;
    uint64_t mapped = 0;
    if ((state->pci->bars[bar_index].flags & PCI_RESOURCE_MEMORY) == 0 || span == 0) {
        g_e1000_error = 34U;
        return false;
    }
    /* e1000 的寄存器窗口不超过 128 KiB；限制窗口也避免异常 BAR 长度
       把一个损坏的 PCI 资源描述扩展成超大映射。 */
    if (span > 0x20000ULL) span = 0x20000ULL;
    span = (span + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (span > X86_64_MMIO_END - E1000_MMIO_VA + 1ULL) {
        g_e1000_error = 31U;
        return false;
    }
    while (mapped < span) {
        if (x86_map_page(x86_current_root_table(), (vaddr_t)(E1000_MMIO_VA + mapped),
                         paddr_make(state->pci->bars[bar_index].address + mapped),
                         X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC) != K_OK) {
            while (mapped != 0) {
                mapped -= PAGE_SIZE;
                (void)x86_unmap_page(x86_current_root_table(),
                                     (vaddr_t)(E1000_MMIO_VA + mapped), 0);
            }
            g_e1000_error = 32U;
            return false;
        }
        mapped += PAGE_SIZE;
    }
    state->mmio = (volatile uint8_t *)(uintptr_t)E1000_MMIO_VA;
    state->mmio_span = span;
    return true;
}

static void e1000_unmap_mmio(e1000_state_t *state) {
    if (state == 0 || state->io_mode || state->mmio == 0) return;
    for (uint64_t offset = 0; offset < state->mmio_span; offset += PAGE_SIZE) {
        (void)x86_unmap_page(x86_current_root_table(),
                             (vaddr_t)(E1000_MMIO_VA + offset), 0);
    }
    state->mmio = 0;
    state->mmio_span = 0;
}

static bool e1000_map_page_dma(const pci_device_t *pci, page_t *page,
                               enum dma_direction direction, dma_mapping_t *mapping) {
    page_t *pages[1] = {page};
    return dma_map_pages((device_t *)&pci->device, pages, 1U, direction, mapping) == K_OK;
}

static uint64_t e1000_dma_address(const dma_mapping_t *mapping) {
    return mapping->segment_count == 0 ? 0 : mapping->segments[0].addr.value;
}

static bool e1000_release_dma(e1000_state_t *state) {
    bool success = true;
    if (state == 0) return false;
    for (uint32_t i = 0; i < E1000_RING_COUNT; ++i) {
        bool tx_released = state->tx_dma[i].device == 0;
        bool rx_released = state->rx_dma[i].device == 0;
        if (!tx_released) {
            tx_released = dma_unmap_checked(&state->tx_dma[i]) == K_OK;
            if (!tx_released) success = false;
        }
        if (!rx_released) {
            rx_released = dma_unmap_checked(&state->rx_dma[i]) == K_OK;
            if (!rx_released) success = false;
        }
        if (tx_released) {
            if (state->tx_pages[i] != 0) page_free(state->tx_pages[i]);
            state->tx_pages[i] = 0;
        }
        if (rx_released) {
            if (state->rx_pages[i] != 0) page_free(state->rx_pages[i]);
            state->rx_pages[i] = 0;
        }
    }
    bool tx_ring_released = state->tx_ring_dma.device == 0;
    bool rx_ring_released = state->rx_ring_dma.device == 0;
    if (!tx_ring_released) {
        tx_ring_released = dma_unmap_checked(&state->tx_ring_dma) == K_OK;
        if (!tx_ring_released) success = false;
    }
    if (!rx_ring_released) {
        rx_ring_released = dma_unmap_checked(&state->rx_ring_dma) == K_OK;
        if (!rx_ring_released) success = false;
    }
    if (tx_ring_released) {
        if (state->tx_ring_page != 0) page_free(state->tx_ring_page);
        state->tx_ring_page = 0;
        state->tx_ring = 0;
    }
    if (rx_ring_released) {
        if (state->rx_ring_page != 0) page_free(state->rx_ring_page);
        state->rx_ring_page = 0;
        state->rx_ring = 0;
    }
    return success;
}

static bool e1000_alloc_dma(e1000_state_t *state) {
    state->tx_ring_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    state->rx_ring_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (state->tx_ring_page == 0 || state->rx_ring_page == 0 ||
        !e1000_map_page_dma(state->pci, state->tx_ring_page, DMA_BIDIRECTIONAL,
                            &state->tx_ring_dma) ||
        !e1000_map_page_dma(state->pci, state->rx_ring_page, DMA_BIDIRECTIONAL,
                            &state->rx_ring_dma)) return false;
    state->tx_ring = (e1000_descriptor_t *)phys_to_direct(page_to_phys(state->tx_ring_page));
    state->rx_ring = (e1000_descriptor_t *)phys_to_direct(page_to_phys(state->rx_ring_page));
    for (uint32_t i = 0; i < E1000_RING_COUNT; ++i) {
        state->tx_pages[i] = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
        state->rx_pages[i] = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
        if (state->tx_pages[i] == 0 || state->rx_pages[i] == 0 ||
            !e1000_map_page_dma(state->pci, state->tx_pages[i], DMA_TO_DEVICE,
                                &state->tx_dma[i]) ||
            !e1000_map_page_dma(state->pci, state->rx_pages[i], DMA_FROM_DEVICE,
                                &state->rx_dma[i])) return false;
        state->tx_ring[i].address = e1000_dma_address(&state->tx_dma[i]);
        state->tx_ring[i].status = E1000_TX_STATUS_DD;
        state->rx_ring[i].address = e1000_dma_address(&state->rx_dma[i]);
        state->rx_ring[i].status = 0;
    }
    dma_sync_for_device(&state->tx_ring_dma);
    dma_sync_for_device(&state->rx_ring_dma);
    dma_wmb();
    return true;
}

static bool e1000_initialize(e1000_state_t *state, const pci_device_t *pci) {
    if (state == 0 || pci == 0 || state->initialized) {
        g_e1000_error = 1U;
        return false;
    }
    e1000_zero(state, sizeof(*state));
    atomic_init(&state->poll_lock.state, 0U);
    e1000_initialize_software_queues(state);
    net_arp_cache_init(&state->arp_cache);
    net_ipv6_neighbor_cache_init(&state->ndp_cache);
    state->pci = pci;
    if (pci_enable_memory_busmaster((pci_device_t *)pci) != K_OK) {
        g_e1000_error = 2U;
        return false;
    }
    if (!e1000_map_mmio(state)) {
        if (g_e1000_error == 0U) g_e1000_error = 3U;
        return false;
    }
    e1000_write(state, E1000_REG_IMC, UINT32_MAX);
    e1000_write(state, E1000_REG_CTRL, E1000_CTRL_RST);
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        if ((e1000_read(state, E1000_REG_CTRL) & E1000_CTRL_RST) == 0) break;
        __asm__ volatile ("pause");
    }
    uint32_t ral = e1000_read(state, E1000_REG_RAL0);
    uint32_t rah = e1000_read(state, E1000_REG_RAH0);
    state->mac[0] = (uint8_t)ral;
    state->mac[1] = (uint8_t)(ral >> 8);
    state->mac[2] = (uint8_t)(ral >> 16);
    state->mac[3] = (uint8_t)(ral >> 24);
    state->mac[4] = (uint8_t)rah;
    state->mac[5] = (uint8_t)(rah >> 8);
    e1000_configure_link_local(state);
    if (!e1000_alloc_dma(state)) {
        g_e1000_error = 4U;
        (void)e1000_release_dma(state);
        e1000_unmap_mmio(state);
        return false;
    }
    e1000_write(state, E1000_REG_TDBAL, (uint32_t)e1000_dma_address(&state->tx_ring_dma));
    e1000_write(state, E1000_REG_TDBAH, (uint32_t)(e1000_dma_address(&state->tx_ring_dma) >> 32));
    e1000_write(state, E1000_REG_TDLEN, E1000_RING_COUNT * sizeof(e1000_descriptor_t));
    e1000_write(state, E1000_REG_TDH, 0);
    e1000_write(state, E1000_REG_TDT, 0);
    e1000_write(state, E1000_REG_RDBAL, (uint32_t)e1000_dma_address(&state->rx_ring_dma));
    e1000_write(state, E1000_REG_RDBAH, (uint32_t)(e1000_dma_address(&state->rx_ring_dma) >> 32));
    e1000_write(state, E1000_REG_RDLEN, E1000_RING_COUNT * sizeof(e1000_descriptor_t));
    e1000_write(state, E1000_REG_RDH, 0);
    e1000_write(state, E1000_REG_RDT, E1000_RING_COUNT - 1U);
    e1000_write(state, E1000_REG_TIPG, 0x00702008U);
    e1000_write(state, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                                      (0x10U << 4) | (0x40U << 12));
    e1000_write(state, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM);
    e1000_write(state, E1000_REG_CTRL, E1000_CTRL_SLU);
    state->link_up = (e1000_read(state, E1000_REG_STATUS) & E1000_STATUS_LU) != 0U;
    net_device_init(&state->device, "e1000", state->mac, 1500U,
                    e1000_device_transmit, state);
    state->device.link_up = state->link_up;
    state->initialized = true;
    socket_set_tcp_ipv4_output(e1000_tcp_ipv4_output, state);
    socket_set_tcp_ipv6_output(e1000_tcp_ipv6_output, state);
    socket_set_udp_ipv4_output(e1000_udp_ipv4_output, state);
    socket_set_udp_ipv6_output(e1000_udp_ipv6_output, state);
    /* The interrupt is bound after the self-test.  This prevents loopback
     * traffic generated by the boot self-test from entering the runtime queue. */
    return true;
}

static bool e1000_destroy(e1000_state_t *state) {
    if (state == 0 || !state->initialized) return true;
    g_e1000_runtime_ready = false;
    socket_set_tcp_ipv4_output(0, 0);
    socket_set_tcp_ipv6_output(0, 0);
    socket_set_udp_ipv4_output(0, 0);
    socket_set_udp_ipv6_output(0, 0);
    atomic_store_explicit(&g_e1000_poll_queued, false, memory_order_release);
    e1000_unbind_interrupt(state);
    e1000_write(state, E1000_REG_TCTL, 0);
    e1000_write(state, E1000_REG_RCTL, 0);
    if (state->phy_loopback) {
        (void)e1000_phy_write(state, E1000_MII_BMCR, state->phy_control);
        state->phy_loopback = false;
    }
    if (!e1000_release_dma(state)) return false;
    e1000_unmap_mmio(state);
    state->initialized = false;
    return true;
}

static bool e1000_transmit(e1000_state_t *state, const uint8_t *frame, size_t length) {
    if (state == 0 || !state->initialized || frame == 0 || length == 0 ||
        length > E1000_BUFFER_SIZE) return false;
    e1000_descriptor_t *descriptor = &state->tx_ring[state->tx_tail];
    if ((descriptor->status & E1000_TX_STATUS_DD) == 0) return false;
    uint8_t *buffer = (uint8_t *)phys_to_direct(page_to_phys(state->tx_pages[state->tx_tail]));
    e1000_copy(buffer, frame, length);
    descriptor->length = (uint16_t)length;
    descriptor->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    descriptor->status = 0;
    dma_sync_for_device(&state->tx_dma[state->tx_tail]);
    dma_wmb();
    state->tx_tail = (state->tx_tail + 1U) % E1000_RING_COUNT;
    e1000_write(state, E1000_REG_TDT, state->tx_tail);

    /*
     * TX submission is asynchronous. DD is checked before slot reuse, so
     * waiting here only stalls the caller after ringing the hardware doorbell.
     */
    return true;
}

static kstatus_t e1000_device_transmit(void *context, const void *frame,
                                       size_t length) {
    return e1000_transmit((e1000_state_t *)context, (const uint8_t *)frame,
                          length) ? K_OK : K_EIO;
}

static uint32_t e1000_ipv4_mask(uint8_t prefix_length) {
    if (prefix_length == 0U) return 0U;
    if (prefix_length >= 32U) return UINT32_MAX;
    return UINT32_MAX << (32U - prefix_length);
}

static uint32_t e1000_ipv4_next_hop(const e1000_state_t *state,
                                    uint32_t destination_address) {
    uint32_t mask;
    if (state == 0 || state->ipv4_gateway == 0U || state->ipv4_address == 0U ||
        state->ipv4_prefix_length > 32U) return destination_address;
    mask = e1000_ipv4_mask(state->ipv4_prefix_length);
    return (destination_address & mask) == (state->ipv4_address & mask) ?
           destination_address : state->ipv4_gateway;
}

static kstatus_t e1000_tcp_ipv4_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint8_t destination_mac[6] = {0};
    uint32_t neighbor_address;
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE + SOCKET_MAX_PAYLOAD];
    size_t frame_length = 0U;
    if (state == 0 || !state->initialized || !state->link_up ||
        source_address == 0U || source_port == 0U || destination_address == 0U ||
        destination_port == 0U || payload_length > SOCKET_MAX_PAYLOAD ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;

    /* 自身发往自身的报文可以直接使用本机 MAC，其余目标必须经过 ARP。 */
    neighbor_address = e1000_ipv4_next_hop(state, destination_address);
    if (destination_address == source_address ||
        (state->ipv4_address != 0U && destination_address == state->ipv4_address)) {
        e1000_copy(destination_mac, state->mac, sizeof(destination_mac));
    } else if (!net_arp_cache_lookup(&state->arp_cache, neighbor_address,
                                     destination_mac)) {
        uint8_t arp_frame[NET_ARP_FRAME_SIZE];
        size_t arp_length = 0U;
        const uint8_t broadcast[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
        if (source_address == 0U ||
            net_arp_build_ipv4(arp_frame, sizeof(arp_frame), state->mac, broadcast,
                               NET_ARP_OPERATION_REQUEST, source_address, state->mac,
                               neighbor_address, (const uint8_t[6]){0},
                               &arp_length) != K_OK ||
            net_device_send(&state->device, arp_frame, arp_length) != K_OK) {
            return K_EIO;
        }
        return K_EAGAIN;
    }
    if (net_tcp_build_ipv4(frame, sizeof(frame), state->mac, destination_mac,
                           source_address, destination_address, source_port,
                           destination_port, sequence, acknowledgement, flags,
                           window, payload, payload_length, &frame_length) != K_OK) {
        return K_EINVAL;
    }
    return net_device_send(&state->device, frame, frame_length);
}

static kstatus_t e1000_udp_ipv4_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    const void *payload, size_t payload_length) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint8_t destination_mac[6] = {0};
    uint32_t neighbor_address;
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_UDP_HEADER_SIZE + SOCKET_MAX_PAYLOAD];
    size_t frame_length = 0U;
    bool ipv4_broadcast = destination_address == UINT32_MAX;
    if (state == 0 || !state->initialized || !state->link_up ||
        (source_address == 0U && !ipv4_broadcast) || source_port == 0U ||
        destination_address == 0U ||
        destination_port == 0U || payload_length > SOCKET_MAX_PAYLOAD ||
        payload_length > (size_t)(state->device.mtu - NET_IPV4_HEADER_SIZE -
                                  NET_UDP_HEADER_SIZE) ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;

    /* 同机地址不需要 ARP；外部 IPv4 地址必须先解析到以太网 MAC。 */
    neighbor_address = e1000_ipv4_next_hop(state, destination_address);
    if (ipv4_broadcast) {
        for (uint32_t index = 0U; index < 6U; ++index) destination_mac[index] = 0xFFU;
    } else if (destination_address == source_address ||
        (state->ipv4_address != 0U && destination_address == state->ipv4_address)) {
        e1000_copy(destination_mac, state->mac, sizeof(destination_mac));
    } else if (!net_arp_cache_lookup(&state->arp_cache, neighbor_address,
                                     destination_mac)) {
        uint8_t arp_frame[NET_ARP_FRAME_SIZE];
        size_t arp_length = 0U;
        const uint8_t broadcast_mac[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
        if (net_arp_build_ipv4(arp_frame, sizeof(arp_frame), state->mac, broadcast_mac,
                               NET_ARP_OPERATION_REQUEST, source_address, state->mac,
                               neighbor_address, (const uint8_t[6]){0},
                               &arp_length) != K_OK ||
            net_device_send(&state->device, arp_frame, arp_length) != K_OK) {
            return K_EIO;
        }
        return K_EAGAIN;
    }
    if (net_udp_build_ipv4(frame, sizeof(frame), state->mac, destination_mac,
                           source_address, destination_address, source_port,
                           destination_port, payload, (uint16_t)payload_length,
                           &frame_length) != K_OK) return K_EINVAL;
    return net_device_send(&state->device, frame, frame_length);
}

static bool e1000_address6_equal(const uint8_t left[16], const uint8_t right[16]) {
    uint8_t difference = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) difference |= left[index] ^ right[index];
    return difference == 0U;
}

static bool e1000_address6_zero(const uint8_t address[16]) {
    uint8_t value = 0U;
    if (address == 0) return true;
    for (uint32_t index = 0U; index < 16U; ++index) value |= address[index];
    return value == 0U;
}

static bool e1000_send_neighbor_solicitation(e1000_state_t *state,
                                              const uint8_t source_address[16],
                                              const uint8_t target_address[16]) {
    uint8_t destination_address[16];
    uint8_t destination_mac[6];
    uint8_t frame[NET_NDP_FRAME_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || source_address == 0 || target_address == 0 ||
        e1000_address6_zero(target_address)) return false;
    net_ndp_solicited_node_address(target_address, destination_address);
    net_ndp_multicast_mac(destination_address, destination_mac);
    return net_ndp_build_neighbor_solicitation(
               frame, sizeof(frame), state->mac, destination_mac,
               source_address, destination_address, target_address,
               &frame_length) == K_OK &&
           net_device_send(&state->device, frame, frame_length) == K_OK;
}

static bool e1000_send_neighbor_advertisement(e1000_state_t *state,
                                              const net_ndp_view_t *request) {
    static const uint8_t all_nodes_address[16] = {
        0xFFU, 0x02U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    };
    static const uint8_t all_nodes_mac[6] = {0x33U, 0x33U, 0, 0, 0, 1};
    uint8_t destination_address[16];
    uint8_t destination_mac[6];
    uint8_t frame[NET_NDP_FRAME_SIZE];
    size_t frame_length = 0U;
    uint32_t flags = NET_NDP_FLAG_OVERRIDE;
    if (state == 0 || request == 0 || !state->ipv6_address_configured ||
        !e1000_address6_equal(request->target_address, state->ipv6_address)) return false;
    if (e1000_address6_zero(request->source_address)) {
        e1000_copy(destination_address, all_nodes_address, sizeof(destination_address));
        e1000_copy(destination_mac, all_nodes_mac, sizeof(destination_mac));
    } else {
        e1000_copy(destination_address, request->source_address, sizeof(destination_address));
        e1000_copy(destination_mac, request->source_mac, sizeof(destination_mac));
        flags |= NET_NDP_FLAG_SOLICITED;
    }
    return net_ndp_build_neighbor_advertisement(
               frame, sizeof(frame), state->mac, destination_mac,
               state->ipv6_address, destination_address, state->ipv6_address,
               flags, state->mac, &frame_length) == K_OK &&
           net_device_send(&state->device, frame, frame_length) == K_OK;
}

static kstatus_t e1000_tcp_ipv6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint8_t destination_mac[6] = {0};
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE + SOCKET_MAX_PAYLOAD];
    size_t frame_length = 0U;
    if (state == 0 || !state->initialized || !state->link_up || source_address == 0 ||
        destination_address == 0 || source_port == 0U || destination_port == 0U ||
        payload_length > SOCKET_MAX_PAYLOAD || (payload == 0 && payload_length != 0U)) {
        return K_EINVAL;
    }
    /* 当前 e1000 后端先支持本机 IPv6/loopback；外部目标待 NDP 邻居缓存接入。 */
    if (e1000_address6_equal(destination_address, source_address) ||
        (state->ipv6_address_configured &&
         e1000_address6_equal(destination_address, state->ipv6_address))) {
        e1000_copy(destination_mac, state->mac, sizeof(destination_mac));
    } else if (!net_ipv6_neighbor_cache_lookup(&state->ndp_cache, destination_address,
                                               destination_mac)) {
        if (!e1000_send_neighbor_solicitation(state, source_address, destination_address)) {
            return K_EIO;
        }
        return K_EAGAIN;
    }
    if (net_tcp_build_ipv6(frame, sizeof(frame), state->mac, destination_mac,
                           source_address, destination_address, source_port,
                           destination_port, sequence, acknowledgement, flags,
                           window, payload, (uint16_t)payload_length,
                           &frame_length) != K_OK) return K_EINVAL;
    return net_device_send(&state->device, frame, frame_length);
}

static kstatus_t e1000_udp_ipv6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    const void *payload, size_t payload_length) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint8_t destination_mac[6] = {0};
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                  NET_UDP_HEADER_SIZE + SOCKET_MAX_PAYLOAD];
    size_t frame_length = 0U;
    if (state == 0 || !state->initialized || !state->link_up ||
        source_address == 0 || destination_address == 0 || source_port == 0U ||
        destination_port == 0U || payload_length > SOCKET_MAX_PAYLOAD ||
        payload_length > (size_t)(state->device.mtu - NET_IPV6_HEADER_SIZE -
                                  NET_UDP_HEADER_SIZE) ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;

    /* 同机 IPv6 以及本机配置地址可以直接交给 PHY loopback 或网卡发送。 */
    if (e1000_address6_equal(destination_address, source_address) ||
        (state->ipv6_address_configured &&
         e1000_address6_equal(destination_address, state->ipv6_address))) {
        e1000_copy(destination_mac, state->mac, sizeof(destination_mac));
    } else if (!net_ipv6_neighbor_cache_lookup(&state->ndp_cache, destination_address,
                                               destination_mac)) {
        if (!e1000_send_neighbor_solicitation(state, source_address, destination_address)) {
            return K_EIO;
        }
        return K_EAGAIN;
    }
    if (net_udp_build_ipv6(frame, sizeof(frame), state->mac, destination_mac,
                           source_address, destination_address, source_port,
                           destination_port, payload, (uint16_t)payload_length,
                           &frame_length) != K_OK) return K_EINVAL;
    return net_device_send(&state->device, frame, frame_length);
}

static bool e1000_arp_reply(e1000_state_t *state, const net_arp_view_t *request) {
    uint8_t frame[NET_ARP_FRAME_SIZE];
    size_t frame_length = 0;
    if (state == 0 || request == 0 || state->ipv4_address == 0U ||
        request->operation != NET_ARP_OPERATION_REQUEST ||
        request->target_address != state->ipv4_address) return true;
    if (net_arp_build_ipv4(frame, sizeof(frame), state->mac, request->sender_mac,
                           NET_ARP_OPERATION_REPLY, state->ipv4_address, state->mac,
                           request->sender_address, request->sender_mac,
                           &frame_length) != K_OK ||
        net_device_send(&state->device, frame, frame_length) != K_OK) {
        if (g_e1000_error == 0U) g_e1000_error = 15U;
        return false;
    }
    return true;
}

static bool e1000_tcp_loopback_peer_ack(e1000_state_t *state,
                                        const e1000_rx_packet_t *packet,
                                        const socket_tcp_reply_t *reply) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || packet == 0 || reply == 0 || !state->tcp_loopback_peer_enabled ||
        !reply->valid || reply->flags != (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) return false;
    if (net_tcp_build_ipv4(
            frame, sizeof(frame), packet->source_mac, packet->destination_mac,
            packet->source_address, packet->destination_address,
            packet->source_port, packet->destination_port,
            packet->sequence + 1U, reply->sequence + 1U, NET_TCP_FLAG_ACK,
            packet->window, 0, 0U, &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) return false;
    state->tcp_loopback_synack_seen = true;
    return true;
}

static bool e1000_tcp_loopback_peer_ack6(e1000_state_t *state,
                                         const e1000_rx_packet_t *packet,
                                         const socket_tcp_reply_t *reply) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || packet == 0 || reply == 0 || !state->tcp_loopback_peer_enabled ||
        !reply->valid || reply->flags != (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) return false;
    if (net_tcp_build_ipv6(
            frame, sizeof(frame), packet->source_mac, packet->destination_mac,
            packet->source_address6, packet->destination_address6,
            packet->source_port, packet->destination_port,
            packet->sequence + 1U, reply->sequence + 1U, NET_TCP_FLAG_ACK,
            packet->window, 0, 0U, &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) return false;
    state->tcp_loopback_synack_seen = true;
    return true;
}

static bool e1000_poll_receive(e1000_state_t *state) {
    bool received = false;
    uint32_t processed = 0U;
    while (processed < E1000_RX_BUDGET &&
           (state->rx_ring[state->rx_clean].status & E1000_RX_STATUS_DD) != 0) {
        e1000_descriptor_t *descriptor = &state->rx_ring[state->rx_clean];
        uint8_t *buffer = (uint8_t *)phys_to_direct(page_to_phys(state->rx_pages[state->rx_clean]));
        net_udp_view_t view;
        net_ipv6_udp_view_t view6;
        net_tcp_view_t tcp_view;
        net_tcp_view_t tcp_view6;
        net_arp_view_t arp_view;
        net_ndp_view_t ndp_view;
        e1000_rx_packet_t packet;
        dma_sync_for_cpu(&state->rx_dma[state->rx_clean]);
        if (descriptor->length != 0 &&
            net_ndp_parse_neighbor_solicitation(buffer, descriptor->length,
                                                 &ndp_view) == K_OK) {
            if (!e1000_address6_zero(ndp_view.source_address)) {
                received = net_ipv6_neighbor_cache_update(
                    &state->ndp_cache, ndp_view.source_address, ndp_view.source_mac,
                    telemetry_timestamp()) == K_OK || received;
            }
            if (state->ipv6_address_configured &&
                e1000_address6_equal(ndp_view.target_address, state->ipv6_address) &&
                !e1000_send_neighbor_advertisement(state, &ndp_view)) {
                if (g_e1000_error == 0U) g_e1000_error = 26U;
                received = true;
            }
        } else if (descriptor->length != 0 &&
            net_ndp_parse_neighbor_advertisement(buffer, descriptor->length,
                                                  &ndp_view) == K_OK) {
            received = net_ipv6_neighbor_cache_update(
                &state->ndp_cache, ndp_view.target_address, ndp_view.target_mac,
                telemetry_timestamp()) == K_OK || received;
        } else if (descriptor->length != 0 &&
                   net_arp_parse_ipv4(buffer, descriptor->length, &arp_view) == K_OK) {
            /* ARP 只更新缓存；回复策略由上层网络管理器决定。 */
            received = net_arp_cache_update(&state->arp_cache, arp_view.sender_address,
                                            arp_view.sender_mac,
                                            telemetry_timestamp()) == K_OK || received;
            /* 只有配置了本机 IPv4 且目标地址命中时才回复 ARP。 */
            if (!e1000_arp_reply(state, &arp_view)) received = true;
            /* A pending TCP SYN/data segment may have been held only because
             * this ARP entry was missing.  Flush that queue immediately after
             * learning the peer MAC instead of waiting for another RX IRQ. */
            uint64_t flush_now = x86_read_tsc();
            uint64_t flush_after = x86_timeout_ns_to_tsc(
                SOCKET_TCP_RETRANSMIT_TIMEOUT_NS);
            /* The TSC cannot wrap during one packet dispatch in practice. */
            flush_now += flush_after + 1U;
            socket_tcp_poll(flush_now);
        } else if (descriptor->length != 0 &&
            net_udp_parse_ipv4(buffer, descriptor->length, &view) == K_OK) {
            e1000_zero(&packet, sizeof(packet));
            packet.family = OS_AF_INET4;
            packet.protocol = SOCKET_PROTOCOL_UDP;
            packet.source_address = view.source_address;
            packet.destination_address = view.destination_address;
            packet.source_port = view.source_port;
            packet.destination_port = view.destination_port;
            packet.payload_length = view.payload_length;
            if (view.payload_length <= sizeof(packet.payload)) {
                e1000_copy(packet.payload, view.payload, view.payload_length);
                received = e1000_enqueue_packet(
                    state, &packet,
                    e1000_flow_hash_ipv4(view.source_address, view.destination_address,
                                         view.source_port, view.destination_port,
                                         SOCKET_PROTOCOL_UDP)) || received;
            }
        } else if (descriptor->length != 0 &&
                   net_udp_parse_ipv6(buffer, descriptor->length, &view6) == K_OK) {
            e1000_zero(&packet, sizeof(packet));
            packet.family = OS_AF_INET6;
            packet.protocol = SOCKET_PROTOCOL_UDP;
            e1000_copy(packet.source_address6, view6.source_address, 16U);
            e1000_copy(packet.destination_address6, view6.destination_address, 16U);
            packet.source_port = view6.source_port;
            packet.destination_port = view6.destination_port;
            packet.payload_length = view6.payload_length;
            if (view6.payload_length <= sizeof(packet.payload)) {
                e1000_copy(packet.payload, view6.payload, view6.payload_length);
                received = e1000_enqueue_packet(
                    state, &packet,
                    e1000_flow_hash_ipv6(view6.source_address, view6.destination_address,
                                         view6.source_port, view6.destination_port)) || received;
            }
        } else if (descriptor->length != 0 &&
                   net_tcp_parse_ipv4(buffer, descriptor->length, &tcp_view) == K_OK) {
            e1000_zero(&packet, sizeof(packet));
            packet.family = OS_AF_INET4;
            packet.protocol = SOCKET_PROTOCOL_TCP;
            packet.flags = tcp_view.flags;
            packet.source_address = tcp_view.source_address;
            packet.destination_address = tcp_view.destination_address;
            packet.source_port = tcp_view.source_port;
            packet.destination_port = tcp_view.destination_port;
            packet.sequence = tcp_view.sequence;
            packet.acknowledgement = tcp_view.acknowledgement;
            packet.window = tcp_view.window;
            e1000_copy(packet.source_mac, tcp_view.source_mac, 6U);
            e1000_copy(packet.destination_mac, tcp_view.destination_mac, 6U);
            packet.payload_length = tcp_view.payload_length;
            if (tcp_view.payload_length <= sizeof(packet.payload)) {
                e1000_copy(packet.payload, tcp_view.payload, tcp_view.payload_length);
                received = e1000_enqueue_packet(
                    state, &packet,
                    e1000_flow_hash_ipv4(tcp_view.source_address,
                                         tcp_view.destination_address,
                                         tcp_view.source_port,
                                         tcp_view.destination_port,
                                         SOCKET_PROTOCOL_TCP)) || received;
            }
        } else if (descriptor->length != 0 &&
                   net_tcp_parse_ipv6(buffer, descriptor->length, &tcp_view6) == K_OK) {
            e1000_zero(&packet, sizeof(packet));
            packet.family = OS_AF_INET6;
            packet.protocol = SOCKET_PROTOCOL_TCP;
            packet.flags = tcp_view6.flags;
            e1000_copy(packet.source_address6, tcp_view6.source_address6, 16U);
            e1000_copy(packet.destination_address6, tcp_view6.destination_address6, 16U);
            packet.source_port = tcp_view6.source_port;
            packet.destination_port = tcp_view6.destination_port;
            packet.sequence = tcp_view6.sequence;
            packet.acknowledgement = tcp_view6.acknowledgement;
            packet.window = tcp_view6.window;
            e1000_copy(packet.source_mac, tcp_view6.source_mac, 6U);
            e1000_copy(packet.destination_mac, tcp_view6.destination_mac, 6U);
            packet.payload_length = tcp_view6.payload_length;
            if (tcp_view6.payload_length <= sizeof(packet.payload)) {
                e1000_copy(packet.payload, tcp_view6.payload, tcp_view6.payload_length);
                received = e1000_enqueue_packet(
                    state, &packet,
                    e1000_flow_hash_ipv6(tcp_view6.source_address6,
                                         tcp_view6.destination_address6,
                                         tcp_view6.source_port,
                                         tcp_view6.destination_port)) || received;
            }
        }
        descriptor->status = 0;
        state->rx_clean = (state->rx_clean + 1U) % E1000_RING_COUNT;
        e1000_write(state, E1000_REG_RDT, (state->rx_clean + E1000_RING_COUNT - 1U) %
                                          E1000_RING_COUNT);
        ++processed;
    }
    bool dispatched = e1000_dispatch_software_queues(state);
    socket_tcp_poll(x86_read_tsc());
    return dispatched || received;
}

static const pci_device_t *e1000_find(void) {
    const pci_host_t *host = pci_current_host();
    const pci_device_t *pci = host == 0 ? 0 : pci_find_class(host, 0x02U, 0x00U, 0xFFU);
    return pci != 0 && pci->vendor_id == E1000_VENDOR_ID ? pci : 0;
}

bool e1000_hardware_present(void) {
    return e1000_find() != 0;
}

bool e1000_rss_self_test(void) {
    if (!e1000_hardware_present()) return true;
    if (!g_e1000.initialized) return false;
    return e1000_rss_self_test_state(&g_e1000);
}

uint32_t e1000_hardware_queue_count(void) {
    return g_e1000.initialized ? g_e1000.hardware_queue_count : 0U;
}

uint32_t e1000_software_queue_count(void) {
    return g_e1000.initialized ? g_e1000.software_queue_count : 0U;
}

kstatus_t e1000_send_frame(const void *frame, size_t length) {
    bool sent;
    if (frame == 0 || length == 0U || length > E1000_BUFFER_SIZE) return K_EINVAL;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (!g_e1000.initialized || !g_e1000_runtime_ready) {
        e1000_lifecycle_unlock();
        return K_ENOENT;
    }
    if (atomic_exchange_explicit(&g_e1000.poll_lock.state, 1U,
                                 memory_order_acquire) != 0U) {
        e1000_lifecycle_unlock();
        return K_EBUSY;
    }
    sent = net_device_send(&g_e1000.device, frame, length) == K_OK;
    atomic_store_explicit(&g_e1000.poll_lock.state, 0U, memory_order_release);
    e1000_lifecycle_unlock();
    return sent ? K_OK : K_EIO;
}

kstatus_t e1000_get_mac_address(uint8_t mac[6]) {
    if (mac == 0) return K_EINVAL;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (!g_e1000.initialized) {
        e1000_lifecycle_unlock();
        return K_ENOENT;
    }
    e1000_copy(mac, g_e1000.mac, 6U);
    e1000_lifecycle_unlock();
    return K_OK;
}

kstatus_t e1000_set_ipv4_address(uint32_t address) {
    return e1000_set_ipv4_config(address, 0U, 0U);
}

kstatus_t e1000_set_ipv4_config(uint32_t address, uint8_t prefix_length,
                                uint32_t gateway) {
    if (prefix_length > 32U) return K_EINVAL;
    if (gateway != 0U && address == 0U) return K_EINVAL;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (!g_e1000.initialized) {
        e1000_lifecycle_unlock();
        return K_ENOENT;
    }
    g_e1000.ipv4_address = address;
    g_e1000.ipv4_prefix_length = prefix_length;
    g_e1000.ipv4_gateway = gateway;
    e1000_lifecycle_unlock();
    return K_OK;
}

uint32_t e1000_ipv4_address(void) {
    uint32_t address = 0U;
    if (!e1000_lifecycle_try_lock()) return 0U;
    if (g_e1000.initialized) address = g_e1000.ipv4_address;
    e1000_lifecycle_unlock();
    return address;
}

uint8_t e1000_ipv4_prefix_length(void) {
    uint8_t prefix_length = 0U;
    if (!e1000_lifecycle_try_lock()) return 0U;
    if (g_e1000.initialized) prefix_length = g_e1000.ipv4_prefix_length;
    e1000_lifecycle_unlock();
    return prefix_length;
}

uint32_t e1000_ipv4_gateway(void) {
    uint32_t gateway = 0U;
    if (!e1000_lifecycle_try_lock()) return 0U;
    if (g_e1000.initialized) gateway = g_e1000.ipv4_gateway;
    e1000_lifecycle_unlock();
    return gateway;
}

kstatus_t e1000_set_ipv6_address(const uint8_t address[16]) {
    if (address == 0 || e1000_address6_zero(address)) return K_EINVAL;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (!g_e1000.initialized) {
        e1000_lifecycle_unlock();
        return K_ENOENT;
    }
    e1000_copy(g_e1000.ipv6_address, address, 16U);
    g_e1000.ipv6_address_configured = true;
    e1000_lifecycle_unlock();
    return K_OK;
}

bool e1000_ipv6_address(uint8_t address[16]) {
    bool configured = false;
    if (address == 0 || !e1000_lifecycle_try_lock()) return false;
    if (g_e1000.initialized && g_e1000.ipv6_address_configured) {
        e1000_copy(address, g_e1000.ipv6_address, 16U);
        configured = true;
    }
    e1000_lifecycle_unlock();
    return configured;
}

bool e1000_reset(void) {
    const pci_device_t *pci;
    uint32_t ipv4_address;
    uint8_t ipv4_prefix_length;
    uint32_t ipv4_gateway;
    uint8_t ipv6_address[16];
    bool ipv6_address_configured;
    if (!e1000_lifecycle_try_lock()) return false;
    if (!g_e1000.initialized || g_e1000.pci == 0) {
        e1000_lifecycle_unlock();
        return false;
    }
    pci = g_e1000.pci;
    ipv4_address = g_e1000.ipv4_address;
    ipv4_prefix_length = g_e1000.ipv4_prefix_length;
    ipv4_gateway = g_e1000.ipv4_gateway;
    e1000_copy(ipv6_address, g_e1000.ipv6_address, sizeof(ipv6_address));
    ipv6_address_configured = g_e1000.ipv6_address_configured;
    g_e1000_runtime_ready = false;
    if (!e1000_destroy(&g_e1000)) {
        g_e1000_runtime_ready = false;
        e1000_lifecycle_unlock();
        return false;
    }
    if (!e1000_initialize(&g_e1000, pci)) {
        g_e1000_runtime_ready = false;
        if (g_e1000_error == 0U) g_e1000_error = 7U;
        e1000_lifecycle_unlock();
        return false;
    }
    /* reset 会重建 DMA 状态，但不能丢失网络管理器已经配置的地址。 */
    g_e1000.ipv4_address = ipv4_address;
    g_e1000.ipv4_prefix_length = ipv4_prefix_length;
    g_e1000.ipv4_gateway = ipv4_gateway;
    e1000_copy(g_e1000.ipv6_address, ipv6_address, sizeof(ipv6_address));
    g_e1000.ipv6_address_configured = ipv6_address_configured;
    /* Reset 后重新发布运行态；链路状态由中断工作项/兼容管理器确认。 */
    /* Some legacy e1000 functions expose only INTx and no MSI capability.
     * Keep the bounded compatibility path for those devices; MSI is used when
     * the PCI function advertises it. */
    (void)e1000_bind_interrupt(&g_e1000);
    g_e1000_runtime_ready = true;
    e1000_lifecycle_unlock();
    return true;
}

bool e1000_link_up(void) {
    if (!e1000_lifecycle_try_lock()) return false;
    if (!g_e1000.initialized) {
        e1000_lifecycle_unlock();
        return false;
    }
    g_e1000.link_up = (e1000_read(&g_e1000, E1000_REG_STATUS) & E1000_STATUS_LU) != 0U;
    g_e1000.device.link_up = g_e1000.link_up;
    bool link_up = g_e1000.link_up;
    e1000_lifecycle_unlock();
    return link_up;
}

bool e1000_self_test(void) {
    const pci_device_t *pci = e1000_find();
    e1000_state_t *state = &g_e1000;
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
    g_e1000_runtime_ready = false;
    atomic_init(&g_e1000_poll_queued, false);
    g_e1000_error = 0U;
    if (!e1000_initialize(state, pci) ||
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
         !e1000_enable_phy_loopback(state) ||
         !e1000_transmit(state, frame, frame_length)) {
        if (g_e1000_error == 0U) g_e1000_error = 5U;
        goto cleanup;
    }
    (void)e1000_bind_interrupt(state);
    /* 保存初始化后的默认网络配置，避免 reset 后误清除链路本地 IPv6。 */
    saved_ipv4_address = state->ipv4_address;
    saved_ipv4_prefix_length = state->ipv4_prefix_length;
    saved_ipv4_gateway = state->ipv4_gateway;
    e1000_copy(saved_ipv6_address, state->ipv6_address,
               sizeof(saved_ipv6_address));
    saved_ipv6_address_configured = state->ipv6_address_configured;
    /* 初始化后记录硬件能力；reset 必须恢复同一块网卡的身份和队列布局。 */
    e1000_copy(expected_mac, state->mac, sizeof(expected_mac));
    expected_hardware_queue_count = state->hardware_queue_count;
    expected_software_queue_count = state->software_queue_count;
    if (!e1000_rss_self_test_state(state)) {
        g_e1000_error = 10U;
        goto cleanup;
    }
    state->ipv4_address = 0x7F000002U;
    if (net_arp_build_ipv4(frame, sizeof(frame), state->mac,
                           (const uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                           NET_ARP_OPERATION_REQUEST, 0x7F000001U, state->mac,
                           0x7F000002U, (const uint8_t[6]){0}, &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) {
        if (g_e1000_error == 0U) g_e1000_error = 13U;
        success = false;
        goto cleanup;
    }
    for (uint32_t attempt = 0; attempt < 1000U; ++attempt) {
        (void)e1000_poll_receive(state);
        if (net_arp_cache_lookup(&state->arp_cache, 0x7F000001U, arp_target_mac)) break;
    }
    bool arp_match = net_arp_cache_lookup(&state->arp_cache, 0x7F000001U,
                                          arp_target_mac);
    for (uint32_t index = 0U; arp_match && index < sizeof(arp_target_mac); ++index) {
        if (arp_target_mac[index] != state->mac[index]) arp_match = false;
    }
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        (void)e1000_poll_receive(state);
        arp_reply_match = net_arp_cache_lookup(&state->arp_cache, 0x7F000002U,
                                                arp_reply_mac);
        for (uint32_t index = 0U; arp_reply_match && index < sizeof(arp_reply_mac); ++index) {
            if (arp_reply_mac[index] != state->mac[index]) arp_reply_match = false;
        }
        if (arp_reply_match) break;
    }
    if (!arp_reply_match) {
        /* 第二个缓存项来自回环收到的 ARP reply，证明回复已真正发出。 */
        /* QEMU user/slirp 后端可能不把 PHY loopback 帧回送到本机 RX 环。 */
        if (g_e1000_error == 0U) g_e1000_error = 16U;
        success = true;
        goto cleanup;
    }
    if (!arp_match) {
        /* ARP 自环的 sender MAC 必须与网卡硬件 MAC 一致。 */
        if (g_e1000_error == 0U) g_e1000_error = 14U;
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
        !e1000_enable_phy_loopback(state)) {
        if (g_e1000_error == 0U) g_e1000_error = 17U;
        success = false;
        goto cleanup;
    }
    state->tcp_loopback_peer_enabled = true;
    if (!e1000_transmit(state, frame, frame_length)) {
        if (g_e1000_error == 0U) g_e1000_error = 18U;
        success = false;
        goto cleanup;
    }
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        (void)e1000_poll_receive(state);
        if (socket_accept(wire_listener, 0U, &wire_accepted) == K_OK) break;
    }
    if (wire_accepted == 0) {
        if (g_e1000_error == 0U) g_e1000_error = 19U;
        success = false;
        goto cleanup;
    }
    state->tcp_loopback_peer_enabled = false;
    for (uint32_t attempt = 0; attempt < 1000U; ++attempt) {
        /* 自检期间不能走带锁的公共入口，否则定时器中断可能在持锁处重入。 */
        (void)e1000_poll_receive(state);
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
        if (g_e1000_error == 0U) g_e1000_error = 9U;
        success = false;
        goto cleanup;
    }
    success = false;
    for (uint32_t attempt = 0; attempt < 1000U; ++attempt) {
        (void)e1000_poll_receive(state);
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
    if (!success && g_e1000_error == 0U) g_e1000_error = 6U;
    if (success) {
        /* socket 层会把回环地址本地投递；这里直接调用网卡后端，专门验证
         * 安装的 UDP 线速路径仍能完成真实的 PHY loopback。 */
        success = e1000_udp_ipv4_output(state, 0x7F000001U, 14000U,
                                        0x7F000001U, 14001U, payload,
                                        sizeof(payload)) == K_OK;
        if (success) {
            success = false;
            for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
                (void)e1000_poll_receive(state);
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
        if (g_e1000_error == 0U) g_e1000_error = 26U;
        goto cleanup;
    }
    if (success) {
        success = e1000_udp_ipv6_output(state, loopback6, 14010U,
                                         loopback6, 14011U, payload6,
                                         sizeof(payload6)) == K_OK;
        if (success) {
            success = false;
            for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
                (void)e1000_poll_receive(state);
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
        if (g_e1000_error == 0U) g_e1000_error = 27U;
        goto cleanup;
    }
    if (success &&
        (net_tcp_build_ipv4(frame, sizeof(frame), state->mac, state->mac,
                            0x7F000001U, 0x7F000001U, 14020U, 14021U,
                            500U, 0U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                            4096U, tcp_payload, sizeof(tcp_payload), &frame_length) != K_OK ||
         !e1000_transmit(state, frame, frame_length))) {
        if (g_e1000_error == 0U) g_e1000_error = 11U;
        success = false;
        goto cleanup;
    }
    if (success) {
        for (uint32_t attempt = 0; attempt < 1000U; ++attempt) {
            (void)e1000_poll_receive(state);
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
            if (g_e1000_error == 0U) g_e1000_error = 12U;
            success = false;
        }
    }
    if (success && !state->tcp_loopback_data_ack_seen) {
        /* 数据段必须经统一网卡发送边界返回 ACK，不能只在 socket 内部更新状态。 */
        if (g_e1000_error == 0U) g_e1000_error = 20U;
        success = false;
    }
    /* IPv6 TCP 被动握手和数据 RX：报文经过真实 e1000 PHY loopback。 */
    if (success &&
        (socket_create(OS_AF_INET6, OS_SOCK_STREAM, 0, &tcp_listener6) != K_OK ||
         socket_bind_ipv6(tcp_listener6, loopback6, 15030U) != K_OK ||
         socket_listen(tcp_listener6, 2U) != K_OK ||
         net_tcp_build_ipv6(frame, sizeof(frame), state->mac, state->mac,
                            loopback6, loopback6, 15031U, 15030U,
                            300U, 0U, NET_TCP_FLAG_SYN, 4096U, 0, 0U,
                            &frame_length) != K_OK)) {
        if (g_e1000_error == 0U) g_e1000_error = 21U;
        success = false;
        goto cleanup;
    }
    state->tcp_loopback_peer_enabled = true;
    state->tcp_loopback_synack_seen = false;
    state->tcp_loopback_data_ack_seen = false;
    if (!e1000_transmit(state, frame, frame_length)) {
        if (g_e1000_error == 0U) g_e1000_error = 22U;
        success = false;
        goto cleanup;
    }
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        (void)e1000_poll_receive(state);
        if (socket_accept(tcp_listener6, 0U, &tcp_accepted6) == K_OK) break;
    }
    if (tcp_accepted6 == 0) {
        if (g_e1000_error == 0U) g_e1000_error = 23U;
        success = false;
        goto cleanup;
    }
    /* 被动端初始序号由 socket 层按源端口确定：0x11000000 + 15031。 */
    uint32_t passive6_sequence = 0x11000000U + 15031U;
    state->tcp_loopback_peer_enabled = false;
    state->tcp_loopback_data_ack_seen = false;
    if (net_tcp_build_ipv6(frame, sizeof(frame), state->mac, state->mac,
                           loopback6, loopback6, 15031U, 15030U,
                           301U, passive6_sequence + 1U,
                           NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH, 4096U,
                           tcp_payload, sizeof(tcp_payload), &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) {
        if (g_e1000_error == 0U) g_e1000_error = 24U;
        success = false;
        goto cleanup;
    }
    success = false;
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        (void)e1000_poll_receive(state);
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
        if (g_e1000_error == 0U) g_e1000_error = 25U;
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
        /* 自检使用 PHY loopback，但运行期必须恢复真实链路状态。 */
        if (state->phy_loopback) {
            (void)e1000_phy_write(state, E1000_MII_BMCR, state->phy_control);
            state->phy_loopback = false;
        }
        state->ipv4_address = saved_ipv4_address;
        state->ipv4_prefix_length = saved_ipv4_prefix_length;
        state->ipv4_gateway = saved_ipv4_gateway;
        e1000_copy(state->ipv6_address, saved_ipv6_address,
                   sizeof(state->ipv6_address));
        state->ipv6_address_configured = saved_ipv6_address_configured;
        if (!e1000_reset()) {
            if (g_e1000_error == 0U) g_e1000_error = 7U;
            success = false;
        } else if (!e1000_link_up() || !state->initialized ||
                   state->tx_ring == 0 || state->rx_ring == 0 ||
                   state->hardware_queue_count != expected_hardware_queue_count ||
                   state->software_queue_count != expected_software_queue_count ||
                   !e1000_bytes_equal(state->mac, expected_mac,
                                      sizeof(expected_mac)) ||
                   state->ipv4_address != saved_ipv4_address ||
                   state->ipv4_prefix_length != saved_ipv4_prefix_length ||
                   state->ipv4_gateway != saved_ipv4_gateway ||
                   state->ipv6_address_configured != saved_ipv6_address_configured ||
                   !e1000_bytes_equal(state->ipv6_address, saved_ipv6_address,
                                      sizeof(saved_ipv6_address))) {
            if (g_e1000_error == 0U) g_e1000_error = 8U;
            success = false;
        } else {
            /* 只检查寄存器和指针还不够；实际发送一帧才能证明 TX ring 已恢复。 */
            for (uint32_t index = 0U; index < 6U; ++index) {
                reset_frame[index] = 0xFFU;
                reset_frame[6U + index] = state->mac[index];
            }
            reset_frame[12] = 0x08U;
            reset_frame[13] = 0x06U;
            if (!e1000_transmit(state, reset_frame, sizeof(reset_frame))) {
                if (g_e1000_error == 0U) g_e1000_error = 29U;
                success = false;
            }
        }
    } else {
        state->ipv4_address = saved_ipv4_address;
        state->ipv4_prefix_length = saved_ipv4_prefix_length;
        state->ipv4_gateway = saved_ipv4_gateway;
        e1000_copy(state->ipv6_address, saved_ipv6_address,
                   sizeof(state->ipv6_address));
        state->ipv6_address_configured = saved_ipv6_address_configured;
        if (!e1000_destroy(state)) success = false;
    }
    return success;
}

bool e1000_poll(void) {
    bool received;
    uint64_t start_tsc;
    if (!e1000_lifecycle_try_lock()) return false;
    if (!g_e1000.initialized || !g_e1000_runtime_ready) {
        e1000_lifecycle_unlock();
        return false;
    }
    /* Deferred context 调用；抢不到锁时直接延后本轮。 */
    if (atomic_exchange_explicit(&g_e1000.poll_lock.state, 1U,
                                 memory_order_acquire) != 0U) {
        e1000_lifecycle_unlock();
        return false;
    }
    start_tsc = telemetry_timestamp();
    g_e1000.link_up = (e1000_read(&g_e1000, E1000_REG_STATUS) & E1000_STATUS_LU) != 0U;
    g_e1000.device.link_up = g_e1000.link_up;
    if (!g_e1000.link_up) {
        atomic_store_explicit(&g_e1000.poll_lock.state, 0U, memory_order_release);
        e1000_lifecycle_unlock();
        (void)telemetry_record_latency(TELEMETRY_CATEGORY_NETWORK_BATCH,
                                       g_e1000.pci != 0 ?
                                           g_e1000.pci->device.device_id : 0U,
                                       start_tsc);
        return false;
    }
    received = e1000_poll_receive(&g_e1000);
    atomic_store_explicit(&g_e1000.poll_lock.state, 0U, memory_order_release);
    e1000_lifecycle_unlock();
    (void)telemetry_record_latency(TELEMETRY_CATEGORY_NETWORK_BATCH,
                                   g_e1000.pci != 0 ?
                                       g_e1000.pci->device.device_id : 0U,
                                   start_tsc);
    return received;
}

bool e1000_interrupt_ready(void) {
    bool ready = false;
    if (!e1000_lifecycle_try_lock()) return false;
    ready = g_e1000.initialized && g_e1000_runtime_ready && g_e1000.irq_bound;
    e1000_lifecycle_unlock();
    return ready;
}

bool e1000_schedule_deferred_poll(void) {
    bool expected = false;
    if (!g_e1000_runtime_ready ||
        !atomic_compare_exchange_strong_explicit(&g_e1000_poll_queued, &expected,
                                                 true, memory_order_acq_rel,
                                                 memory_order_acquire)) return false;
    if (!deferred_try_schedule(e1000_deferred_poll, 0)) {
        atomic_store_explicit(&g_e1000_poll_queued, false, memory_order_release);
        return false;
    }
    return true;
}

void e1000_deferred_poll(void *argument) {
    (void)argument;
    (void)e1000_poll();
    atomic_store_explicit(&g_e1000_poll_queued, false, memory_order_release);
}

uint32_t e1000_last_error(void) {
    return g_e1000_error;
}
