#include <kernel/bluetooth.h>
#include <kernel/input.h>
#include <kernel/mm.h>

#include <stdio.h>
#include <stdlib.h>

static input_event_t g_events[16];
static uint32_t g_event_count;

void *kzalloc(size_t size, uint32_t flags) {
    (void)flags;
    return calloc(1U, size);
}

void kfree(void *ptr) {
    free(ptr);
}

bool input_core_init(void) {
    return true;
}

kstatus_t input_core_push(const input_event_t *event) {
    if (event == 0 || g_event_count >= 16U) return K_ENOMEM;
    g_events[g_event_count++] = *event;
    return K_OK;
}

kstatus_t input_core_pop(input_event_t *event) {
    if (event == 0 || g_event_count == 0U) return K_ENOENT;
    *event = g_events[0];
    for (uint32_t i = 1U; i < g_event_count; ++i) g_events[i - 1U] = g_events[i];
    --g_event_count;
    return K_OK;
}

uint32_t input_core_pending(void) {
    return g_event_count;
}

typedef struct test_transport {
    uint32_t sends;
} test_transport_t;

static kstatus_t test_send(void *context, const uint8_t *packet, size_t length) {
    test_transport_t *transport = (test_transport_t *)context;
    if (transport == 0 || packet == 0 || length < 4U || packet[0] != 1U) return K_EINVAL;
    ++transport->sends;
    return K_OK;
}

int main(void) {
    test_transport_t transport = {0};
    bt_controller_t *controller = 0;
    bt_controller_stats_t stats = {0};
    uint8_t parameter = 1U;
    uint8_t complete[] = {0x0EU,4U,1U,0x03U,0x0CU,0U};
    uint8_t command_status[] = {0x0FU,4U,0U,1U,0x04U,0x0CU};
    uint8_t invalid[] = {0x0EU,3U,1U,0x03U,0x0CU};
    uint8_t acl[] = {0x40U,0x20U,0x08U,0x00U,0x04U,0x00U,0x40U,0x00U,
                     0x01U,0x02U,0x03U,0x04U};
    if (bt_controller_create(test_send, &transport, &controller) != K_OK) return 1;
    kstatus_t start = bt_controller_start(controller);
    kstatus_t submit = bt_hci_submit(controller, 0x0C03U, &parameter, 1U);
    kstatus_t complete_status = bt_hci_receive_event(controller, complete, sizeof(complete));
    kstatus_t status_submit = bt_hci_submit(controller, 0x0C04U, 0, 0U);
    kstatus_t status_event = bt_hci_receive_event(controller, command_status,
                                                   sizeof(command_status));
    kstatus_t invalid_status = bt_hci_receive_event(controller, invalid, sizeof(invalid));
    kstatus_t acl_status = bt_acl_receive(controller, acl, sizeof(acl));
    bt_controller_get_stats(controller, &stats);
    if (start != K_OK || submit != K_OK || complete_status != K_OK ||
        status_submit != K_OK || status_event != K_OK || invalid_status != K_EINVAL ||
        acl_status != K_OK || transport.sends != 2U ||
        stats.commands_submitted != 2U || stats.commands_completed != 2U ||
        stats.hci_events != 3U || stats.invalid_packets != 1U ||
        stats.acl_packets != 1U) {
        bt_controller_destroy(controller);
        puts("bluetooth: fail");
        return 1;
    }
    bt_controller_destroy(controller);
    if (!bluetooth_core_self_test()) {
        puts("bluetooth: fail");
        return 1;
    }
    puts("bluetooth: ok");
    return 0;
}
