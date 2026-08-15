#pragma once

#include "base.h"
#include "spinlock.h"

#define BT_HCI_MAX_COMMAND_PARAMS 255U
#define BT_HCI_COMMAND_QUEUE_SIZE 16U
#define BT_ACL_MAX_PAYLOAD 1024U
#define BT_GATT_MAX_ATTRIBUTES 32U
#define BT_GATT_MAX_VALUE 64U
#define BT_GATT_PERM_READ (1U << 0)
#define BT_GATT_PERM_WRITE (1U << 1)

typedef enum bt_controller_state {
    BT_CONTROLLER_OFFLINE = 0,
    BT_CONTROLLER_READY,
    BT_CONTROLLER_ACTIVE,
    BT_CONTROLLER_RESETTING,
    BT_CONTROLLER_DISCONNECTED,
} bt_controller_state_t;

typedef kstatus_t (*bt_transport_send_fn)(void *context,
                                          const uint8_t *packet,
                                          size_t length);

typedef struct bt_controller bt_controller_t;

typedef struct bt_controller_stats {
    uint64_t commands_submitted;
    uint64_t commands_completed;
    uint64_t hci_events;
    uint64_t acl_packets;
    uint64_t invalid_packets;
    uint64_t hid_reports;
    uint64_t controller_resets;
    bt_controller_state_t state;
    uint16_t pending_opcode;
} bt_controller_stats_t;

kstatus_t bt_controller_create(bt_transport_send_fn send, void *context,
                               bt_controller_t **out);
void bt_controller_destroy(bt_controller_t *controller);
kstatus_t bt_controller_start(bt_controller_t *controller);
kstatus_t bt_controller_reset(bt_controller_t *controller);
kstatus_t bt_controller_disconnect(bt_controller_t *controller);
kstatus_t bt_hci_submit(bt_controller_t *controller, uint16_t opcode,
                        const void *parameters, uint8_t parameter_length);
kstatus_t bt_hci_complete(bt_controller_t *controller, uint16_t opcode,
                          uint8_t status);
/*
 * 接收控制器送回的 HCI Event packet。
 * 该接口至少处理 Command Complete/Command Status，并将命令状态机推进；
 * 其它格式正确的事件交给上层后续扩展，但仍会计入 hci_events。
 */
kstatus_t bt_hci_receive_event(bt_controller_t *controller,
                               const void *packet, size_t length);
kstatus_t bt_acl_receive(bt_controller_t *controller, const void *packet,
                         size_t length);
kstatus_t bt_acl_submit(bt_controller_t *controller, const void *packet,
                        size_t length);
kstatus_t bt_l2cap_validate(const void *payload, size_t length);
kstatus_t bt_gatt_add_attribute(bt_controller_t *controller, uint16_t handle,
                                uint16_t permissions, const void *value,
                                uint16_t value_length);
kstatus_t bt_gatt_read_attribute(bt_controller_t *controller, uint16_t handle,
                                 void *value, uint16_t capacity,
                                 uint16_t *value_length);
kstatus_t bt_hid_keyboard_report(bt_controller_t *controller, const void *report,
                                 size_t length, uint32_t device_id);
void bt_controller_get_stats(bt_controller_t *controller,
                             bt_controller_stats_t *stats);
bool bluetooth_core_self_test(void);
