#include <kernel/bluetooth.h>
#include <kernel/input.h>
#include <kernel/kmem.h>

typedef struct bt_attribute {
    uint16_t handle;
    uint16_t permissions;
    uint16_t value_length;
    uint8_t value[BT_GATT_MAX_VALUE];
    bool used;
} bt_attribute_t;

struct bt_controller {
    spinlock_t lock;
    bt_transport_send_fn send;
    void *context;
    bt_controller_state_t state;
    uint16_t pending_opcode;
    uint8_t hid_keys[6];
    bt_attribute_t attributes[BT_GATT_MAX_ATTRIBUTES];
    bt_controller_stats_t stats;
};

#define BT_HCI_EVENT_COMMAND_COMPLETE 0x0EU
#define BT_HCI_EVENT_COMMAND_STATUS   0x0FU
#define BT_HCI_MAX_EVENT_PARAMS       255U

static void bt_lock(bt_controller_t *controller) {
    while (atomic_exchange_explicit(&controller->lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void bt_unlock(bt_controller_t *controller) {
    atomic_store_explicit(&controller->lock.state, 0U, memory_order_release);
}

static bool bt_contains_key(const uint8_t *keys, uint8_t key) {
    if (keys == 0 || key == 0U) return false;
    for (uint32_t i = 0; i < 6U; ++i) {
        if (keys[i] == key) return true;
    }
    return false;
}

kstatus_t bt_controller_create(bt_transport_send_fn send, void *context,
                               bt_controller_t **out) {
    bt_controller_t *controller;
    /* 没有物理传输后端的控制器不能进入可用状态，避免假成功。 */
    if (out == 0) return K_EINVAL;
    *out = 0;
    if (send == 0) return K_EINVAL;
    controller = (bt_controller_t *)kzalloc(sizeof(*controller), 0);
    if (controller == 0) return K_ENOMEM;
    atomic_init(&controller->lock.state, 0U);
    controller->send = send;
    controller->context = context;
    controller->state = BT_CONTROLLER_OFFLINE;
    controller->pending_opcode = 0U;
    controller->stats.state = BT_CONTROLLER_OFFLINE;
    *out = controller;
    return K_OK;
}

void bt_controller_destroy(bt_controller_t *controller) {
    if (controller == 0) return;
    kfree(controller);
}

kstatus_t bt_controller_start(bt_controller_t *controller) {
    if (controller == 0) return K_EINVAL;
    bt_lock(controller);
    if (controller->state == BT_CONTROLLER_DISCONNECTED) {
        bt_unlock(controller);
        return K_EDEVREMOVED;
    }
    if (controller->state == BT_CONTROLLER_ACTIVE) {
        bt_unlock(controller);
        return K_OK;
    }
    if (controller->state == BT_CONTROLLER_RESETTING) {
        bt_unlock(controller);
        return K_EBUSY;
    }
    controller->state = BT_CONTROLLER_ACTIVE;
    controller->stats.state = BT_CONTROLLER_ACTIVE;
    bt_unlock(controller);
    return K_OK;
}

kstatus_t bt_controller_reset(bt_controller_t *controller) {
    if (controller == 0) return K_EINVAL;
    bt_lock(controller);
    if (controller->state == BT_CONTROLLER_DISCONNECTED) {
        bt_unlock(controller);
        return K_EDEVREMOVED;
    }
    controller->state = BT_CONTROLLER_RESETTING;
    controller->pending_opcode = 0U;
    ++controller->stats.controller_resets;
    controller->state = BT_CONTROLLER_ACTIVE;
    controller->stats.state = BT_CONTROLLER_ACTIVE;
    bt_unlock(controller);
    return K_OK;
}

kstatus_t bt_controller_disconnect(bt_controller_t *controller) {
    if (controller == 0) return K_EINVAL;
    bt_lock(controller);
    controller->pending_opcode = 0U;
    controller->state = BT_CONTROLLER_DISCONNECTED;
    controller->stats.state = BT_CONTROLLER_DISCONNECTED;
    bt_unlock(controller);
    return K_OK;
}

kstatus_t bt_hci_submit(bt_controller_t *controller, uint16_t opcode,
                        const void *parameters, uint8_t parameter_length) {
    uint8_t packet[4U + BT_HCI_MAX_COMMAND_PARAMS];
    bt_transport_send_fn send;
    void *context;
    kstatus_t status = K_OK;
    if (controller == 0 || opcode == 0U ||
        (parameter_length != 0U && parameters == 0)) return K_EINVAL;
    packet[0] = 0x01U;
    packet[1] = (uint8_t)opcode;
    packet[2] = (uint8_t)(opcode >> 8);
    packet[3] = parameter_length;
    for (uint32_t i = 0; i < parameter_length; ++i) {
        packet[4U + i] = ((const uint8_t *)parameters)[i];
    }
    bt_lock(controller);
    if (controller->state != BT_CONTROLLER_ACTIVE) {
        kstatus_t result = controller->state == BT_CONTROLLER_DISCONNECTED ?
            K_EDEVREMOVED : K_EBUSY;
        bt_unlock(controller);
        return result;
    }
    if (controller->pending_opcode != 0U) {
        bt_unlock(controller);
        return K_EBUSY;
    }
    controller->pending_opcode = opcode;
    ++controller->stats.commands_submitted;
    send = controller->send;
    context = controller->context;
    bt_unlock(controller);

    status = send(context, packet, 4U + parameter_length);
    if (status != K_OK) {
        bt_lock(controller);
        if (controller->pending_opcode == opcode) controller->pending_opcode = 0U;
        bt_unlock(controller);
    }
    return status;
}

kstatus_t bt_hci_receive_event(bt_controller_t *controller,
                               const void *packet, size_t length) {
    const uint8_t *bytes = (const uint8_t *)packet;
    uint8_t event_code;
    uint8_t parameter_length;
    uint16_t opcode;
    uint8_t status;
    kstatus_t result;

    if (controller == 0 || bytes == 0 || length < 2U ||
        length > 2U + BT_HCI_MAX_EVENT_PARAMS) return K_EINVAL;
    event_code = bytes[0];
    parameter_length = bytes[1];
    if (length != 2U + parameter_length) {
        bt_lock(controller);
        ++controller->stats.invalid_packets;
        bt_unlock(controller);
        return K_EINVAL;
    }

    bt_lock(controller);
    if (controller->state != BT_CONTROLLER_ACTIVE) {
        result = controller->state == BT_CONTROLLER_DISCONNECTED ?
            K_EDEVREMOVED : K_EBUSY;
        bt_unlock(controller);
        return result;
    }
    ++controller->stats.hci_events;
    bt_unlock(controller);

    if (event_code == BT_HCI_EVENT_COMMAND_COMPLETE) {
        /* ncmd(1), opcode(2)，至少还要有一个 status 返回参数。 */
        if (parameter_length < 4U) goto invalid;
        opcode = (uint16_t)bytes[3] | ((uint16_t)bytes[4] << 8);
        status = bytes[5];
        return bt_hci_complete(controller, opcode, status);
    }
    if (event_code == BT_HCI_EVENT_COMMAND_STATUS) {
        /* status(1), ncmd(1), opcode(2)。 */
        if (parameter_length != 4U) goto invalid;
        status = bytes[2];
        opcode = (uint16_t)bytes[4] | ((uint16_t)bytes[5] << 8);
        return bt_hci_complete(controller, opcode, status);
    }
    return K_OK;

invalid:
    bt_lock(controller);
    ++controller->stats.invalid_packets;
    bt_unlock(controller);
    return K_EINVAL;
}

kstatus_t bt_hci_complete(bt_controller_t *controller, uint16_t opcode,
                          uint8_t status) {
    if (controller == 0 || opcode == 0U) return K_EINVAL;
    bt_lock(controller);
    if (controller->state == BT_CONTROLLER_DISCONNECTED) {
        bt_unlock(controller);
        return K_EDEVREMOVED;
    }
    if (controller->pending_opcode != opcode) {
        bt_unlock(controller);
        return K_ENOENT;
    }
    controller->pending_opcode = 0U;
    ++controller->stats.commands_completed;
    bt_unlock(controller);
    return status == 0U ? K_OK : K_EIO;
}

kstatus_t bt_l2cap_validate(const void *payload, size_t length) {
    const uint8_t *bytes = (const uint8_t *)payload;
    uint16_t payload_length;
    uint16_t channel;
    if (bytes == 0 || length < 4U || length > BT_ACL_MAX_PAYLOAD) return K_EINVAL;
    payload_length = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    channel = (uint16_t)bytes[2] | ((uint16_t)bytes[3] << 8);
    if (channel == 0U || payload_length != length - 4U) return K_EINVAL;
    return K_OK;
}

kstatus_t bt_acl_receive(bt_controller_t *controller, const void *packet,
                         size_t length) {
    const uint8_t *bytes = (const uint8_t *)packet;
    uint16_t payload_length;
    kstatus_t status;
    if (controller == 0 || bytes == 0 || length < 4U || length > 4U + BT_ACL_MAX_PAYLOAD) {
        return K_EINVAL;
    }
    bt_lock(controller);
    if (controller->state != BT_CONTROLLER_ACTIVE) {
        kstatus_t result = controller->state == BT_CONTROLLER_DISCONNECTED ?
            K_EDEVREMOVED : K_EBUSY;
        bt_unlock(controller);
        return result;
    }
    bt_unlock(controller);
    payload_length = (uint16_t)bytes[2] | ((uint16_t)bytes[3] << 8);
    if (payload_length > BT_ACL_MAX_PAYLOAD || length != 4U + payload_length) {
        bt_lock(controller);
        ++controller->stats.invalid_packets;
        bt_unlock(controller);
        return K_EINVAL;
    }
    status = bt_l2cap_validate(bytes + 4U, payload_length);
    bt_lock(controller);
    if (status != K_OK) {
        ++controller->stats.invalid_packets;
    } else {
        ++controller->stats.acl_packets;
    }
    bt_unlock(controller);
    return status;
}

kstatus_t bt_acl_submit(bt_controller_t *controller, const void *packet,
                        size_t length) {
    const uint8_t *input = (const uint8_t *)packet;
    uint8_t output[1U + 4U + BT_ACL_MAX_PAYLOAD];
    bt_transport_send_fn send;
    void *context;
    uint16_t payload_length;
    if (controller == 0 || input == 0 || length < 8U ||
        length > 4U + BT_ACL_MAX_PAYLOAD) return K_EINVAL;
    payload_length = (uint16_t)input[2] | ((uint16_t)input[3] << 8);
    if (payload_length > BT_ACL_MAX_PAYLOAD || length != 4U + payload_length ||
        bt_l2cap_validate(input + 4U, payload_length) != K_OK) return K_EINVAL;
    bt_lock(controller);
    if (controller->state != BT_CONTROLLER_ACTIVE) {
        kstatus_t status = controller->state == BT_CONTROLLER_DISCONNECTED ?
            K_EDEVREMOVED : K_EBUSY;
        bt_unlock(controller);
        return status;
    }
    send = controller->send;
    context = controller->context;
    bt_unlock(controller);
    output[0] = 0x02U;
    for (size_t i = 0U; i < length; ++i) output[i + 1U] = input[i];
    return send(context, output, length + 1U);
}

kstatus_t bt_gatt_add_attribute(bt_controller_t *controller, uint16_t handle,
                                uint16_t permissions, const void *value,
                                uint16_t value_length) {
    bt_attribute_t *free_attribute = 0;
    if (controller == 0 || handle == 0U || permissions == 0U ||
        value_length > BT_GATT_MAX_VALUE ||
        (value_length != 0U && value == 0)) return K_EINVAL;
    bt_lock(controller);
    for (uint32_t i = 0; i < BT_GATT_MAX_ATTRIBUTES; ++i) {
        bt_attribute_t *attribute = &controller->attributes[i];
        if (attribute->used && attribute->handle == handle) {
            bt_unlock(controller);
            return K_EBUSY;
        }
        if (!attribute->used && free_attribute == 0) free_attribute = attribute;
    }
    if (free_attribute == 0) {
        bt_unlock(controller);
        return K_ENOMEM;
    }
    free_attribute->handle = handle;
    free_attribute->permissions = permissions;
    free_attribute->value_length = value_length;
    for (uint32_t i = 0; i < value_length; ++i) {
        free_attribute->value[i] = ((const uint8_t *)value)[i];
    }
    free_attribute->used = true;
    bt_unlock(controller);
    return K_OK;
}

kstatus_t bt_gatt_read_attribute(bt_controller_t *controller, uint16_t handle,
                                 void *value, uint16_t capacity,
                                 uint16_t *value_length) {
    if (controller == 0 || value_length == 0 ||
        (capacity != 0U && value == 0)) return K_EINVAL;
    bt_lock(controller);
    for (uint32_t i = 0; i < BT_GATT_MAX_ATTRIBUTES; ++i) {
        bt_attribute_t *attribute = &controller->attributes[i];
        if (!attribute->used || attribute->handle != handle) continue;
        if ((attribute->permissions & BT_GATT_PERM_READ) == 0U) {
            bt_unlock(controller);
            return K_EACCES;
        }
        if (capacity < attribute->value_length) {
            bt_unlock(controller);
            return K_ENOMEM;
        }
        for (uint32_t j = 0; j < attribute->value_length; ++j) {
            ((uint8_t *)value)[j] = attribute->value[j];
        }
        *value_length = attribute->value_length;
        bt_unlock(controller);
        return K_OK;
    }
    bt_unlock(controller);
    return K_ENOENT;
}

kstatus_t bt_hid_keyboard_report(bt_controller_t *controller, const void *report,
                                 size_t length, uint32_t device_id) {
    const uint8_t *bytes = (const uint8_t *)report;
    uint8_t old_keys[6];
    uint8_t new_keys[6];
    input_event_t events[12];
    uint32_t event_count = 0U;
    if (controller == 0 || bytes == 0 || length < 8U) return K_EINVAL;
    bt_lock(controller);
    if (controller->state != BT_CONTROLLER_ACTIVE) {
        kstatus_t status = controller->state == BT_CONTROLLER_DISCONNECTED ?
            K_EDEVREMOVED : K_EBUSY;
        bt_unlock(controller);
        return status;
    }
    bt_unlock(controller);
    for (uint32_t i = 0; i < 6U; ++i) {
        new_keys[i] = bytes[2U + i];
        bool duplicate = false;
        for (uint32_t j = 0; j < i; ++j) {
            if (new_keys[j] != 0U && new_keys[j] == new_keys[i]) duplicate = true;
        }
        if ((new_keys[i] != 0U &&
             (new_keys[i] < 4U || new_keys[i] > 0xE7U)) || duplicate) {
            bt_lock(controller);
            ++controller->stats.invalid_packets;
            bt_unlock(controller);
            return K_EINVAL;
        }
    }
    bt_lock(controller);
    for (uint32_t i = 0; i < 6U; ++i) old_keys[i] = controller->hid_keys[i];
    for (uint32_t i = 0; i < 6U; ++i) controller->hid_keys[i] = new_keys[i];
    ++controller->stats.hid_reports;
    bt_unlock(controller);

    for (uint32_t i = 0; i < 6U; ++i) {
        if (old_keys[i] == 0U || bt_contains_key(new_keys, old_keys[i])) continue;
        events[event_count++] = (input_event_t){0, device_id, INPUT_EVENT_KEY, 0,
                                                old_keys[i], INPUT_VALUE_RELEASE,
                                                0, 0, 0};
    }
    for (uint32_t i = 0; i < 6U; ++i) {
        if (new_keys[i] == 0U || bt_contains_key(old_keys, new_keys[i])) continue;
        events[event_count++] = (input_event_t){0, device_id, INPUT_EVENT_KEY, 0,
                                                new_keys[i], INPUT_VALUE_PRESS,
                                                0, 0, 0};
    }
    for (uint32_t i = 0; i < event_count; ++i) {
        kstatus_t status = input_core_push(&events[i]);
        if (status != K_OK) return status;
    }
    return K_OK;
}

void bt_controller_get_stats(bt_controller_t *controller,
                             bt_controller_stats_t *stats) {
    if (controller == 0 || stats == 0) return;
    bt_lock(controller);
    *stats = controller->stats;
    stats->pending_opcode = controller->pending_opcode;
    stats->state = controller->state;
    bt_unlock(controller);
}

typedef struct bt_test_transport {
    uint32_t sends;
    uint16_t opcode;
    uint8_t length;
} bt_test_transport_t;

static kstatus_t bt_test_send(void *context, const uint8_t *packet, size_t length) {
    bt_test_transport_t *transport = (bt_test_transport_t *)context;
    if (transport == 0 || packet == 0 || length < 4U ||
        (packet[0] != 0x01U && packet[0] != 0x02U)) {
        return K_EINVAL;
    }
    ++transport->sends;
    transport->opcode = (uint16_t)packet[1] | ((uint16_t)packet[2] << 8);
    transport->length = packet[3];
    return K_OK;
}

bool bluetooth_core_self_test(void) {
    bt_test_transport_t transport = {0};
    bt_controller_t *controller = 0;
    bt_controller_t *rejected = (bt_controller_t *)1;
    bt_controller_stats_t stats = {0};
    uint8_t parameter = 0x01U;
    uint8_t command_complete[] = {BT_HCI_EVENT_COMMAND_COMPLETE, 4U,
                                  1U, 0x03U, 0x0CU, 0U};
    uint8_t command_status[] = {BT_HCI_EVENT_COMMAND_STATUS, 4U,
                                0U, 1U, 0x04U, 0x0CU};
    uint8_t invalid_event[] = {BT_HCI_EVENT_COMMAND_COMPLETE, 3U,
                               1U, 0x03U, 0x0CU};
    uint8_t acl[12] = {0x40U, 0x20U, 0x08U, 0x00U,
                       0x04U, 0x00U, 0x40U, 0x00U,
                       0x01U, 0x02U, 0x03U, 0x04U};
    uint8_t value[8] = {0};
    uint16_t value_length = 0U;
    uint8_t report[8] = {0};
    input_event_t event = {0};
    bool success = false;
    if (bt_controller_create(0, 0, &rejected) != K_EINVAL || rejected != 0 ||
        bt_controller_create(bt_test_send, &transport, &controller) != K_OK ||
        bt_controller_start(controller) != K_OK ||
        bt_hci_submit(controller, 0x0C03U, &parameter, 1U) != K_OK ||
        transport.sends != 1U || transport.opcode != 0x0C03U ||
        transport.length != 1U ||
        bt_hci_receive_event(controller, command_complete,
                             sizeof(command_complete)) != K_OK ||
        bt_hci_submit(controller, 0x0C04U, 0, 0U) != K_OK ||
        bt_hci_receive_event(controller, command_status,
                             sizeof(command_status)) != K_OK ||
        bt_hci_receive_event(controller, invalid_event,
                             sizeof(invalid_event)) != K_EINVAL ||
        bt_acl_receive(controller, acl, sizeof(acl)) != K_OK ||
        bt_acl_submit(controller, acl, sizeof(acl)) != K_OK ||
        transport.sends != 3U) goto done;
    acl[2] = 0x07U;
    if (bt_acl_receive(controller, acl, sizeof(acl)) != K_EINVAL ||
        bt_gatt_add_attribute(controller, 1U, 1U, "ok", 2U) != K_OK ||
        bt_gatt_read_attribute(controller, 1U, value, sizeof(value), &value_length) != K_OK ||
        value_length != 2U || value[0] != 'o' || value[1] != 'k') goto done;
    report[2] = 0x04U;
    if (bt_hid_keyboard_report(controller, report, sizeof(report), 7U) != K_OK ||
        input_core_pop(&event) != K_OK || event.type != INPUT_EVENT_KEY ||
        event.code != 0x04U || event.value != INPUT_VALUE_PRESS) goto done;
    report[2] = 0U;
    if (bt_hid_keyboard_report(controller, report, sizeof(report), 7U) != K_OK ||
        input_core_pop(&event) != K_OK || event.value != INPUT_VALUE_RELEASE ||
        event.code != 0x04U || bt_controller_reset(controller) != K_OK ||
        bt_controller_disconnect(controller) != K_OK ||
        bt_hci_submit(controller, 1U, 0, 0U) != K_EDEVREMOVED) goto done;
    bt_controller_get_stats(controller, &stats);
    success = stats.commands_submitted == 2U && stats.commands_completed == 2U &&
              stats.hci_events == 3U &&
              stats.acl_packets == 1U && stats.invalid_packets == 2U &&
              stats.hid_reports == 2U && stats.controller_resets == 1U &&
              stats.state == BT_CONTROLLER_DISCONNECTED;
done:
    if (controller != 0) bt_controller_destroy(controller);
    return success;
}
