#pragma once

#include <kernel/base.h>

/* 可选 Intel e1000 后端：QEMU 未提供网卡时不影响无网卡启动。 */
bool e1000_hardware_present(void);
bool e1000_self_test(void);
bool e1000_packet_queue_self_test(void);
bool e1000_rss_self_test(void);
uint32_t e1000_hardware_queue_count(void);
uint32_t e1000_software_queue_count(void);
bool e1000_reset(void);
bool e1000_link_up(void);
kstatus_t e1000_get_mac_address(uint8_t mac[6]);
kstatus_t e1000_send_frame(const void *frame, size_t length);
kstatus_t e1000_set_ipv4_address(uint32_t address);
kstatus_t e1000_set_ipv4_config(uint32_t address, uint8_t prefix_length,
                                uint32_t gateway);
uint32_t e1000_ipv4_address(void);
uint8_t e1000_ipv4_prefix_length(void);
uint32_t e1000_ipv4_gateway(void);
kstatus_t e1000_set_ipv6_address(const uint8_t address[16]);
bool e1000_ipv6_address(uint8_t address[16]);
bool e1000_poll(void);
bool e1000_interrupt_ready(void);
bool e1000_schedule_deferred_poll(void);
void e1000_deferred_poll(void *argument);
uint32_t e1000_last_error(void);
