#include <kernel/e1000.h>
#include <kernel/net_manager.h>
#include <kernel/spinlock.h>

static net_manager_status_t g_net_manager;
static spinlock_t g_net_manager_lock;
static atomic_uint g_net_manager_init_state;
static uint32_t g_net_manager_poll_divider;

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

void net_manager_poll(void) {
    bool hardware_present;
    bool link_up = false;
    uint32_t ipv4_address = 0U;
    uint8_t ipv4_prefix_length = 0U;
    uint32_t ipv4_gateway = 0U;
    uint8_t ipv6_address[16] = {0};
    uint8_t mac[6] = {0};
    bool ipv6_configured = false;
    if (atomic_load_explicit(&g_net_manager_init_state, memory_order_acquire) != 2U) {
        return;
    }
    /* Runtime E1000 receive is interrupt-driven when MSI is available.  The
     * legacy QEMU e1000 has no MSI capability; only that fallback is kicked
     * from this low-priority manager path. */
    if (!e1000_interrupt_ready()) (void)e1000_schedule_deferred_poll();
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
    }
    net_manager_lock();
    if (g_net_manager.hardware_present != hardware_present ||
        g_net_manager.link_up != link_up) {
        ++g_net_manager.link_transitions;
    }
    g_net_manager.hardware_present = hardware_present;
    g_net_manager.link_up = link_up;
    g_net_manager.ipv4_address = ipv4_address;
    g_net_manager.ipv4_prefix_length = ipv4_prefix_length;
    g_net_manager.ipv4_gateway = ipv4_gateway;
    g_net_manager.ipv6_configured = ipv6_configured;
    net_manager_copy(g_net_manager.mac, mac, sizeof(mac));
    net_manager_zero(g_net_manager.mac_reserved, sizeof(g_net_manager.mac_reserved));
    if (ipv6_configured) {
        net_manager_copy(g_net_manager.ipv6_address, ipv6_address, sizeof(ipv6_address));
    } else {
        net_manager_zero(g_net_manager.ipv6_address, sizeof(g_net_manager.ipv6_address));
    }
    net_manager_unlock();
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
