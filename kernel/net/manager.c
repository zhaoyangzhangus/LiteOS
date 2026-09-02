#include <arch/x86_64/cpu.h>
#include <kernel/e1000.h>
#include <kernel/message_port.h>
#include <kernel/net_manager.h>
#include <kernel/rtl8126.h>
#include <kernel/spinlock.h>
#include <kernel/socket.h>
#include <uapi/network.h>
#include <uapi/syscall.h>

static net_manager_status_t g_net_manager;
static spinlock_t g_net_manager_lock;
static atomic_uint g_net_manager_init_state;
static uint32_t g_net_manager_poll_divider;
static message_port_t *g_net_manager_event_port;

static void net_manager_lock(void) {
    while (atomic_exchange_explicit(&g_net_manager_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void net_manager_unlock(void) {
    atomic_store_explicit(&g_net_manager_lock.state, 0U, memory_order_release);
}

static void net_manager_zero(void *memory, size_t length) {
    uint8_t *bytes = (uint8_t *)memory;
    while (length-- != 0U) *bytes++ = 0U;
}

static void net_manager_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0U) *out++ = *in++;
}

bool net_manager_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_net_manager_init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        net_manager_zero(&g_net_manager, sizeof(g_net_manager));
        atomic_init(&g_net_manager_lock.state, 0U);
        g_net_manager.initialized = true;
        /* 初始化阶段立即采样一次，之后由内核主循环低频刷新。 */
        g_net_manager_poll_divider = 63U;
        atomic_store_explicit(&g_net_manager_init_state, 2U, memory_order_release);
        net_manager_poll();
        return true;
    }
    while (atomic_load_explicit(&g_net_manager_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

kstatus_t net_manager_subscribe(struct message_port *raw_port) {
    message_port_t *port = (message_port_t *)raw_port;
    message_port_t *old_port;

    if (port != 0 && port->object.type != KOBJECT_TYPE_MESSAGE_PORT) {
        return K_EINVAL;
    }

    if (port != 0) object_get(port);

    net_manager_lock();
    old_port = g_net_manager_event_port;
    g_net_manager_event_port = port;
    net_manager_unlock();

    if (old_port != 0) object_put(old_port);
    return K_OK;
}

void net_manager_poll(void) {
    bool hardware_present;
    bool link_up = false;
    uint32_t ipv4_address = 0U;
    uint8_t ipv4_prefix_length = 0U;
    uint32_t ipv4_gateway = 0U;
    uint8_t ipv6_address[16] = {0};
    uint8_t mac[6] = {0};
    bool ipv6_configured = false;
    bool link_changed = false;
    message_port_t *event_port = 0;
    os_net_event_t event = {0};
    if (atomic_load_explicit(&g_net_manager_init_state, memory_order_acquire) != 2U) {
        return;
    }
    /* RX is interrupt-driven when MSI is available, but TCP retransmission
     * and ARP/NDP resolution still need a periodic poll after an initial
     * output returned K_EAGAIN.  In particular, wget's first SYN can be
     * queued behind an ARP request; an IRQ for the ARP reply alone is not a
     * timer.  The deferred-poll gate coalesces this with an IRQ-triggered
     * poll, so keeping the kick here is bounded and does not duplicate work. */
    if (!e1000_interrupt_ready()) (void)e1000_schedule_deferred_poll();
    /* Keep the retransmission clock independent from RX/deferred-worker
     * progress.  An ARP reply may be the last packet received before a
     * pending SYN needs to be emitted. */
    socket_tcp_poll(x86_read_tsc());
    if (++g_net_manager_poll_divider < 64U) return;
    g_net_manager_poll_divider = 0U;
    hardware_present = e1000_hardware_present();
    if (hardware_present) {
        link_up = e1000_link_up();
        ipv4_address = e1000_ipv4_address();
        ipv4_prefix_length = e1000_ipv4_prefix_length();
        ipv4_gateway = e1000_ipv4_gateway();
        ipv6_configured = e1000_ipv6_address(ipv6_address);
        (void)e1000_get_mac_address(mac);
        rtl8126_emit_diagnostic();
    }
    net_manager_lock();
    link_changed =
        g_net_manager.hardware_present != hardware_present ||
        g_net_manager.link_up != link_up;
    if (link_changed) ++g_net_manager.link_transitions;

    g_net_manager.hardware_present = hardware_present;
    g_net_manager.link_up = link_up;
    g_net_manager.ipv4_address = ipv4_address;
    g_net_manager.ipv4_prefix_length = ipv4_prefix_length;
    g_net_manager.ipv4_gateway = ipv4_gateway;
    g_net_manager.ipv6_configured = ipv6_configured;
    net_manager_copy(g_net_manager.mac, mac, sizeof(mac));
    net_manager_zero(g_net_manager.mac_reserved, sizeof(g_net_manager.mac_reserved));

    if (ipv6_configured) {
        net_manager_copy(g_net_manager.ipv6_address,
                         ipv6_address,
                         sizeof(ipv6_address));
    } else {
        net_manager_zero(g_net_manager.ipv6_address,
                         sizeof(g_net_manager.ipv6_address));
    }

    if (link_changed && g_net_manager_event_port != 0 &&
        object_try_get(g_net_manager_event_port)) {
        event_port = g_net_manager_event_port;
    }

    event.hdr.size = sizeof(event);
    event.hdr.version = OS_SYSCALL_ABI_VERSION;
    event.type = OS_NET_EVENT_LINK_CHANGED;
    event.status_flags =
        (hardware_present ? OS_NET_STATUS_HARDWARE_PRESENT : 0U) |
        (link_up ? OS_NET_STATUS_LINK_UP : 0U) |
        (ipv6_configured ? OS_NET_STATUS_IPV6_CONFIGURED : 0U);
    event.link_transitions = g_net_manager.link_transitions;
    net_manager_unlock();

    if (event_port != 0) {
        (void)message_port_send(event_port, &event, sizeof(event));
        object_put(event_port);
    }
}

bool net_manager_get_status(net_manager_status_t *status) {
    if (status == 0 || !net_manager_init()) return false;
    net_manager_lock();
    *status = g_net_manager;
    net_manager_unlock();
    return true;
}

bool net_manager_ready(void) {
    net_manager_status_t status;
    return net_manager_get_status(&status) && status.hardware_present && status.link_up;
}

bool net_manager_self_test(void) {
    net_manager_status_t status;
    if (!net_manager_init() || !net_manager_get_status(&status) || !status.initialized) {
        return false;
    }
    /* QEMU 没有 e1000 时网络设备允许缺席，但管理器自身必须保持可用。 */
    return status.hardware_present ? status.ipv6_configured : !status.link_up;
}
