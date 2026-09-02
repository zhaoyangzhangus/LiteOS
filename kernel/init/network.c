#include <kernel/init_network.h>

#include <kernel/debug_stage.h>
#include <kernel/e1000.h>
#include <kernel/net_core.h>
#include <kernel/net_manager.h>
#include <kernel/rtl8126.h>
#include <kernel/socket.h>

static BOOLEAN network_fail_at(const liteos_init_network_hooks_t *hooks,
                               const CHAR8 *message, const char *file,
                               uint32_t line) {
    liteos_debug_stage_fail_at(LITEOS_DEBUG_PHASE_NETWORK,
                               LITEOS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    /* Network hardware is optional during boot.  Keep the failure in the
     * canonical log and let the runtime start without a NIC. */
    return 0;
}

#define network_fail(hooks, message) \
    network_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN liteos_init_network(const liteos_init_network_hooks_t *hooks) {
    if (hooks == 0 || hooks->write == 0 || hooks->write_u32 == 0 ||
        hooks->halt == 0) return 0;

    liteos_debug_stage_enter(LITEOS_DEBUG_PHASE_SPEC_12);
    if (!net_core_self_test()) {
        return network_fail(hooks, "LITEOS_NET_CORE_FAIL\r\n");
    }
    hooks->write("LITEOS_NET_CORE_OK\r\n");
    if (!net_arp_self_test()) {
        return network_fail(hooks, "LITEOS_NET_ARP_FAIL\r\n");
    }
    hooks->write("LITEOS_NET_ARP_OK\r\n");
    if (!net_ipv6_self_test()) {
        return network_fail(hooks, "LITEOS_NET_IPV6_FAIL\r\n");
    }
    hooks->write("LITEOS_NET_IPV6_OK\r\n");
    if (!net_ndp_self_test()) {
        return network_fail(hooks, "LITEOS_NET_NDP_FAIL\r\n");
    }
    hooks->write("LITEOS_NET_NDP_OK\r\n");
    if (!net_tcp_self_test()) {
        return network_fail(hooks, "LITEOS_NET_TCP_FAIL\r\n");
    }
    hooks->write("LITEOS_NET_TCP_OK\r\n");
    if (!net_firewall_self_test()) {
        return network_fail(hooks, "LITEOS_FIREWALL_FAIL\r\n");
    }
    hooks->write("LITEOS_FIREWALL_OK\r\n");
    if (!socket_self_test()) {
        return network_fail(hooks, "LITEOS_SOCKET_FAIL\r\n");
    }
    hooks->write("LITEOS_SOCKET_OK\r\nLITEOS_SOCKET_IPV6_OK\r\n");
    if (!e1000_packet_queue_self_test()) {
        return network_fail(hooks, "LITEOS_E1000_QUEUE_FAIL\r\n");
    }
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                       LITEOS_DEBUG_STEP_PROGRESS, 7U);
    if (rtl8126_hardware_present()) {
        if (!rtl8126_self_test()) {
            hooks->write("LITEOS_RTL8126_FAIL=");
            hooks->write_u32(e1000_last_error());
            return network_fail(hooks, "\r\n");
        }
        hooks->write("LITEOS_RTL8126_HW_OK\r\n");
        hooks->write(rtl8126_firmware_required() ?
                     "LITEOS_RTL8126_FIRMWARE_REQUIRED\r\n" :
                     "LITEOS_RTL8126_FIRMWARE_OK\r\n");
        hooks->write(rtl8126_interrupt_ready() ?
                     "LITEOS_RTL8126_IRQ_OK\r\n" :
                     "LITEOS_RTL8126_IRQ_FAIL\r\n");
    } else {
        if (!e1000_self_test()) {
            hooks->write("LITEOS_E1000_FAIL=");
            hooks->write_u32(e1000_last_error());
            return network_fail(hooks, "\r\n");
        }
        /* The interrupt lifecycle is now independently locatable in
         * recovery.c. */
        liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_8,
                           LITEOS_DEBUG_STEP_PROGRESS, 18U);
        if (e1000_intel_hardware_present()) {
            hooks->write("LITEOS_E1000_HW_OK\r\n");
            hooks->write("LITEOS_E1000_RESET_OK\r\n");
        } else {
            hooks->write("LITEOS_E1000_NONE\r\n");
        }
        if (e1000_intel_hardware_present() && !e1000_rss_self_test()) {
            return network_fail(hooks, "LITEOS_E1000_RSS_FAIL\r\n");
        }
        if (e1000_intel_hardware_present()) {
            hooks->write("LITEOS_E1000_QUEUES_HW=");
            hooks->write_u32(e1000_hardware_queue_count());
            hooks->write(" SW=");
            hooks->write_u32(e1000_software_queue_count());
            hooks->write("\r\nLITEOS_E1000_RSS_OK\r\n");
            hooks->write(e1000_interrupt_ready() ?
                         "LITEOS_E1000_IRQ_OK\r\n" :
                         "LITEOS_E1000_INTX_COMPAT\r\n");
        }
    }
    if (!net_manager_init() || !net_manager_self_test()) {
        return network_fail(hooks, "LITEOS_NET_MANAGER_FAIL\r\n");
    }
    hooks->write("LITEOS_NET_MANAGER_OK\r\n");
    liteos_debug_stage(LITEOS_DEBUG_PHASE_NETWORK,
                       LITEOS_DEBUG_STEP_READY, 1U);
    liteos_debug_stage_ready(LITEOS_DEBUG_PHASE_SPEC_12);
    liteos_debug_stage(LITEOS_DEBUG_PHASE_REFACTOR_3,
                       LITEOS_DEBUG_STEP_PROGRESS, 8U);
    return 1;
}
