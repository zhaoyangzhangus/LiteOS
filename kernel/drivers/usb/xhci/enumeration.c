#include <kernel/console.h>
#include <kernel/kmem.h>
#include <kernel/realtest.h>
#include <usb/device.h>
#include <usb/hub.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_ENUMERATION_OWNER: USB descriptor parsing, endpoint
 * context setup, and device enumeration are isolated cold-path policy. */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_ADDRESS_DEVICE_TYPE 11U
#define XHCI_CONFIGURE_ENDPOINT_TYPE 12U
#define XHCI_EVALUATE_CONTEXT_TYPE 13U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)
#define XHCI_PORTSC_SPEED_MASK 0x0FU
#define XHCI_SETUP_GET_DESCRIPTOR 6U
#define XHCI_SETUP_SET_CONFIGURATION 9U
#define XHCI_SETUP_SET_INTERFACE 11U
#define XHCI_DESCRIPTOR_DEVICE 1U
#define XHCI_DESCRIPTOR_CONFIGURATION 2U
#define XHCI_DESCRIPTOR_INTERFACE 4U
#define XHCI_DESCRIPTOR_ENDPOINT 5U
#define XHCI_DESCRIPTOR_CS_INTERFACE 0x24U
#define XHCI_DESCRIPTOR_HUB 0x29U
#define XHCI_DESCRIPTOR_SS_HUB 0x2AU
#define XHCI_DESCRIPTOR_DEVICE_LENGTH 18U
#define USB_CLASS_AUDIO 0x01U
#define USB_AUDIO_SUBCLASS_STREAM 0x02U
#define USB_CLASS_HID 0x03U
#define USB_HID_SUBCLASS_BOOT 0x01U
#define USB_HID_PROTOCOL_KEYBOARD 0x01U
#define USB_HID_PROTOCOL_MOUSE 0x02U
#define USB_HID_SET_PROTOCOL 0x0BU
#define USB_HID_PROTOCOL_BOOT 0x00U
#define USB_CLASS_HUB 0x09U
#define USB_CLASS_MASS_STORAGE 0x08U
#define USB_MSC_SUBCLASS_SCSI 0x06U
#define USB_MSC_PROTOCOL_BULK_ONLY 0x50U
#define USB_MSC_PROTOCOL_UAS 0x62U
#define USB_SS_ENDPOINT_COMPANION 0x30U
#define USB_UAS_PIPE_USAGE 0x24U
#define USB_UAS_PIPE_COMMAND 1U
#define USB_UAS_PIPE_STATUS 2U
#define USB_UAS_PIPE_DATA_IN 3U
#define USB_UAS_PIPE_DATA_OUT 4U
#define USB_CLASS_WIRELESS 0xE0U
#define USB_BT_SUBCLASS 0x01U
#define USB_BT_PROTOCOL 0x01U
#define USB_TRANSFER_BULK 0x02U
#define USB_TRANSFER_INTERRUPT 0x03U
#define USB_TRANSFER_ISOCHRONOUS 0x01U
#define XHCI_SLOT_MTT (1U << 25)
#define XHCI_SLOT_TT_PORT_SHIFT 8U
#define XHCI_SLOT_STATE_SHIFT 27U
#define XHCI_SLOT_STATE_MASK 0x1FU
#define XHCI_SLOT_STATE_ADDRESSED 2U
#define USB_HUB_PROTOCOL_MULTI_TT 2U
#define USB_HUB_MIN_POWER_GOOD_MS 100U

static bool xhci_resolve_tt(
    uint8_t child_speed,
    uint8_t parent_slot,
    uint8_t parent_port,
    uint8_t *tt_slot,
    uint8_t *tt_port,
    bool *tt_multi)
{
    if(tt_slot == 0 ||
       tt_port == 0 ||
       tt_multi == 0)
    {
        return false;
    }

    *tt_slot =
        0U;

    *tt_port =
        0U;

    *tt_multi =
        false;

    /*
     * Only Low/Full-Speed devices use an external USB2 TT.
     */
    if(child_speed > 2U ||
       parent_slot == 0U)
    {
        return true;
    }

    if(parent_port == 0U)
    {
        return false;
    }

    xhci_slot_device_t *parent =
        xhci_topology_slot(parent_slot);

    if(!parent->used ||
       !parent->is_hub ||
       parent->context.device_slot !=
           parent_slot)
    {
        return false;
    }

    /*
     * Directly below a High-Speed Hub:
     * that Hub owns the split transaction.
     */
    if(parent->context.device_speed == 3U)
    {
        *tt_slot =
            parent_slot;

        *tt_port =
            parent_port;

        *tt_multi =
            parent->hub_protocol ==
                USB_HUB_PROTOCOL_MULTI_TT;

        return true;
    }

    /*
     * A Full/Low-Speed Hub may itself be behind a High-Speed TT.
     * All of its Full/Low-Speed descendants remain in that same
     * upstream TT domain.
     */
    if(parent->tt_slot != 0U)
    {
        *tt_slot =
            parent->tt_slot;

        *tt_port =
            parent->tt_port;

        *tt_multi =
            parent->tt_multi;
    }

    return true;
}


static bool xhci_prepare_device_resources(xhci_state_t *state, uint8_t slot,
                                          uint8_t port, uint8_t speed,
                                          uint8_t root_port, uint8_t parent_slot,
                                          uint8_t parent_port,
                                          uint8_t tt_slot,
                                          uint8_t tt_port,
                                          bool tt_multi,
                                          uint32_t route_string) {
    if (state == 0 || slot == 0U || port == 0U || speed == 0U ||
        root_port == 0U) return false;
    if (!xhci_alloc_page(state, &state->input_context, DMA_TO_DEVICE) ||
        !xhci_alloc_page(state, &state->output_context, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->ep0_ring, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->descriptor_buffer, DMA_BIDIRECTIONAL)) {
        xhci_set_error(53U);
        xhci_free_device_resources(state);
        return false;
    }
    volatile uint64_t *dcbaa = (volatile uint64_t *)state->dcbaa.cpu;
    dcbaa[slot] = xhci_dma_address(&state->output_context.mapping);

    uint8_t *input = (uint8_t *)state->input_context.cpu;
    uint32_t *control = (uint32_t *)input;
    uint32_t *slot_context = (uint32_t *)(input + state->context_size);
    uint32_t *ep0_context = (uint32_t *)(input + state->context_size * 2U);
    control[0] = 0U;
    control[1] = 0x3U;
    slot_context[0] =
        ((uint32_t)(
            speed &
            XHCI_PORTSC_SPEED_MASK) <<
         20) |
        (route_string &
         0x000FFFFFU) |
        (1U << 27) |
        (tt_multi
             ? XHCI_SLOT_MTT
             : 0U);

    slot_context[1] =
        (uint32_t)root_port <<
        16;

    /*
     * DW2 is NOT software parent identity.
     *
     * It is the external High-Speed Transaction Translator that
     * services this Low/Full-Speed device.
     */
    slot_context[2] =
        (uint32_t)tt_slot |
        ((uint32_t)tt_port <<
         XHCI_SLOT_TT_PORT_SHIFT);
    slot_context[3] = 0U;
    uint32_t max_packet = speed >= 3U ? 64U : 8U;
    if (speed >= 4U) max_packet = 512U;
    ep0_context[0] = 0U;
    ep0_context[1] = (3U << 1) | (4U << 3) | (max_packet << 16);
    ep0_context[2] = (uint32_t)xhci_dma_address(&state->ep0_ring.mapping) | 1U;
    ep0_context[3] = (uint32_t)(xhci_dma_address(&state->ep0_ring.mapping) >> 32);
    /* Endpoint Context DW4: Average TRB Length.  A control TD always
     * contains an eight-byte Setup TRB; zero is tolerated by QEMU but causes
     * some physical xHCI implementations to leave EP0 unscheduled. */
    ep0_context[4] = 8U;

    xhci_trb_t *ring = (xhci_trb_t *)state->ep0_ring.cpu;
    ring[XHCI_RING_TRB_COUNT - 1U].parameter =
        xhci_dma_address(&state->ep0_ring.mapping);
    ring[XHCI_RING_TRB_COUNT - 1U].control =
        (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
    dma_sync_for_device(&state->dcbaa.mapping);
    dma_sync_for_device(&state->input_context.mapping);
    dma_sync_for_device(&state->output_context.mapping);
    dma_sync_for_device(&state->ep0_ring.mapping);
    dma_sync_for_device(&state->descriptor_buffer.mapping);
    state->device_slot = slot;
    state->device_port = port;
    state->device_speed = speed;
    state->root_port = root_port;
    state->parent_slot = parent_slot;
    state->parent_port = parent_port;
    state->route_string = route_string;
    state->ep0_enqueue = 0U;
    return true;
}

static uint32_t xhci_ep0_descriptor_max_packet(
    const xhci_device_context_t *device, const uint8_t *descriptor) {
    uint32_t raw;

    if (device == 0 || descriptor == 0) return 0U;
    if (device->device_speed >= 4U) return 512U;
    raw = descriptor[7];
    if (raw == 8U || raw == 16U || raw == 32U || raw == 64U) return raw;

    /* USB 2 devices must report one of the values above.  A few firmware
     * paths expose the SuperSpeed encoding (9) through a USB2 companion
     * port; retain the speed-specific EP0 default instead of programming an
     * impossible packet size into the xHCI context. */
    return device->device_speed >= 3U ? 64U : 8U;
}

static uint32_t xhci_ep0_initial_max_packet(
    const xhci_device_context_t *device) {
    if (device == 0) return 0U;
    if (device->device_speed >= 4U) return 512U;
    return device->device_speed >= 3U ? 64U : 8U;
}

static bool xhci_prepare_ep0_update(xhci_state_t *state,
                                    const xhci_device_context_t *device,
                                    const uint8_t *descriptor,
                                    bool evaluate_only) {
    uint32_t *input;
    uint32_t *control;
    uint32_t *ep0;
    uint32_t max_packet;
    uint64_t dequeue;

    if (state == 0 || device == 0 || descriptor == 0 ||
        state->context_size == 0U || state->input_context.cpu == 0 ||
        state->output_context.cpu == 0 || state->ep0_ring.cpu == 0) {
        return false;
    }
    input = (uint32_t *)state->input_context.cpu;
    control = input;
    ep0 = input + (state->context_size * 2U) / sizeof(uint32_t);
    max_packet = xhci_ep0_descriptor_max_packet(device, descriptor);
    if (max_packet == 0U || max_packet > 512U) return false;

    if (evaluate_only) {
        const uint32_t *output_ep0;
        uint32_t context_words = state->context_size / sizeof(uint32_t);

        dma_sync_for_cpu(&state->output_context.mapping);
        output_ep0 = (const uint32_t *)state->output_context.cpu +
                     context_words;
        for (uint32_t index = 0U; index < 5U; ++index) {
            ep0[index] = output_ep0[index];
        }
    }

    /* Endpoint State is output-only in an input Endpoint Context. */
    ep0[0] &= ~0x7U;
    ep0[1] = (ep0[1] & 0x0000FFFFU) | (max_packet << 16);
    dequeue = xhci_dma_address(&device->ep0_ring.mapping) +
              (uint64_t)device->ep0_enqueue * sizeof(xhci_trb_t);
    ep0[2] = (uint32_t)dequeue | XHCI_TRB_CYCLE;
    ep0[3] = (uint32_t)(dequeue >> 32);
    control[0] = 0U;
    control[1] = evaluate_only ? (1U << 1) : 0x3U;
    dma_sync_for_device(&state->input_context.mapping);
    return true;
}

static bool xhci_evaluate_ep0_max_packet(
    xhci_state_t *state, const xhci_device_context_t *device,
    const uint8_t *descriptor) {
    if (!xhci_prepare_ep0_update(state, device, descriptor, true)) {
        return false;
    }
    liteos_realtest_mark_number("XHCI_EP0_EVALUATE_BEGIN",
                                device->device_slot);
    if (!xhci_submit_command_ex(
            state, XHCI_EVALUATE_CONTEXT_TYPE, device->device_slot, 0U,
            xhci_dma_address(&device->input_context.mapping), 0)) {
        liteos_realtest_mark_number("XHCI_EP0_EVALUATE_ERROR",
                                    xhci_last_error());
        return false;
    }
    liteos_realtest_mark_number("XHCI_EP0_EVALUATE_OK",
                                device->device_slot);
    return true;
}

static bool xhci_prepare_address_update(xhci_state_t *state,
                                        const xhci_device_context_t *device,
                                        const uint8_t *descriptor) {
    return xhci_prepare_ep0_update(state, device, descriptor, false);
}

static bool xhci_slot_is_addressed(xhci_state_t *state, uint8_t slot) {
    uint32_t slot_state;

    if (state == 0 || state->output_context.cpu == 0) return false;
    dma_sync_for_cpu(&state->output_context.mapping);
    slot_state = ((const uint32_t *)state->output_context.cpu)[3];
    slot_state = (slot_state >> XHCI_SLOT_STATE_SHIFT) &
                 XHCI_SLOT_STATE_MASK;

    liteos_serial_printf_serial_only(
        "LITEOS_DIAG_XHCI_SLOT_ADDRESSED SLOT=%u STATE=%u\r\n",
        slot, slot_state);
    liteos_realtest_mark_number("XHCI_SLOT_STATE", slot_state);
    return slot_state == XHCI_SLOT_STATE_ADDRESSED;
}

static bool xhci_complete_device_address(
    xhci_state_t *state, const xhci_device_context_t *device,
    const uint8_t *descriptor) {
    uint32_t initial_max_packet;
    uint32_t descriptor_max_packet;

    if (state == 0 || device == 0 || descriptor == 0 ||
        device->device_slot == 0U) {
        return false;
    }
    initial_max_packet = xhci_ep0_initial_max_packet(device);
    descriptor_max_packet =
        xhci_ep0_descriptor_max_packet(device, descriptor);
    if (initial_max_packet == 0U || descriptor_max_packet == 0U) {
        return false;
    }

    liteos_realtest_mark_number("XHCI_ADDRESS_SLOT", device->device_slot);
    liteos_realtest_mark_number("XHCI_EP0_MPS", descriptor_max_packet);
    if (descriptor_max_packet != initial_max_packet &&
        !xhci_evaluate_ep0_max_packet(state, device, descriptor)) {
        return false;
    }
    if (!xhci_prepare_address_update(state, device, descriptor) ||
        !xhci_submit_address_device(
            state, device->device_slot,
            xhci_dma_address(&device->input_context.mapping), false)) {
        liteos_realtest_mark_number("XHCI_ADDRESS_ERROR", xhci_last_error());
        return false;
    }
    return xhci_slot_is_addressed(state, device->device_slot);
}

/*
 * V3.10.1A EXPLICIT EP0 DEVICE
 *
 * Controller-owned state:
 *   MMIO / doorbell / Event Ring
 *
 * Device-owned state:
 *   Slot ID / EP0 Ring / EP0 enqueue / descriptor buffer
 */
/*
 * V3.10.1F CONTROL WRAPPER REMOVED
 *
 * Every control-transfer caller now names its device context.
 * This stage changes API shape only; legacy callers explicitly
 * pass &state->device and retain identical ownership semantics.
 */
/*
 * Legacy working-context wrapper.
 *
 * V3.10.1A deliberately leaves every existing caller unchanged.
 * Later patches migrate individual subsystems to the explicit
 * device-context helper one at a time.
 */





static bool xhci_read_hub_descriptor(xhci_state_t *state) {
    uint8_t request[8] = {
        0xA0U, XHCI_SETUP_GET_DESCRIPTOR, 0x00U, XHCI_DESCRIPTOR_HUB,
        0x00U, 0x00U, 0x09U, 0x00U,
    };
    uint8_t descriptor_type = XHCI_DESCRIPTOR_HUB;
    uint8_t descriptor_length = 9U;
    uint8_t *descriptor;
    if (state == 0 || !state->hub_interface_present) return false;
    if (state->device_speed >= 4U) {
        descriptor_type = XHCI_DESCRIPTOR_SS_HUB;
        descriptor_length = 12U;
        request[3] = descriptor_type;
        request[6] = descriptor_length;
    }
    if (!xhci_submit_control_transfer_device(state, &state->device, request,
                                              descriptor_length, true)) {
        return false;
    }
    descriptor = (uint8_t *)state->descriptor_buffer.cpu;
    if (descriptor[0] < 3U || descriptor[1] != descriptor_type ||
        descriptor[2] == 0U || descriptor[2] > 15U) {
        xhci_set_error(68U);
        return false;
    }
    state->hub_port_count =
        descriptor[2];

    uint16_t hub_characteristics =
        (uint16_t)descriptor[3] |
        ((uint16_t)descriptor[4] << 8);

    state->hub_tt_think_time =
        (uint8_t)(
            (hub_characteristics >> 5) &
            0x03U);
    uint32_t power_good_ms = (uint32_t)descriptor[5] * 2U;

    /* Linux powers every external Hub port before looking for children, even
     * when the Hub reports no switching support.  Some devices emulate the
     * request and expose no connection status until it has been sent. */
    uint32_t powered_ports = 0U;
    for (uint32_t port = 1U; port <= state->hub_port_count; ++port) {
        if (xhci_hub_set_port_feature_device(
                state, &state->device, (uint8_t)port,
                USB_HUB_FEATURE_PORT_POWER)) {
            ++powered_ports;
        }
    }

    if (power_good_ms < USB_HUB_MIN_POWER_GOOD_MS) {
        power_good_ms = USB_HUB_MIN_POWER_GOOD_MS;
    }
    liteos_serial_printf_serial_only(
        "LITEOS_DIAG_HUB_POWER SLOT=%u PORTS=%u POWERED=%u DELAY_MS=%u\r\n",
        state->device.device_slot, state->hub_port_count, powered_ports,
        power_good_ms);
    xhci_controller_delay_ns((uint64_t)power_good_ms * 1000000ULL);

    state->hub_configured =
        true;
    return true;
}

static bool xhci_read_configuration_descriptor(xhci_state_t *state) {
    uint8_t request[8] = {
        0x80U, XHCI_SETUP_GET_DESCRIPTOR, 0x00U,
        XHCI_DESCRIPTOR_CONFIGURATION, 0x00U, 0x00U, 0x09U, 0x00U,
    };
    uint8_t *descriptor;
    uint32_t total_length;

    if (state == 0 ||
        !xhci_submit_control_transfer_device(
            state, &state->device, request, 9U, true)) {
        return false;
    }
    descriptor = (uint8_t *)state->descriptor_buffer.cpu;
    if (descriptor[0] < 9U ||
        descriptor[1] != XHCI_DESCRIPTOR_CONFIGURATION) {
        xhci_set_error(57U);
        return false;
    }
    total_length = (uint32_t)descriptor[2] |
                   ((uint32_t)descriptor[3] << 8);
    if (total_length < 9U || total_length > PAGE_SIZE) {
        xhci_set_error(57U);
        return false;
    }
    liteos_realtest_mark_number("XHCI_CONFIG_LENGTH", total_length);
    if (total_length == 9U) return true;

    request[6] = (uint8_t)total_length;
    request[7] = (uint8_t)(total_length >> 8);
    return xhci_submit_control_transfer_device(
        state, &state->device, request, total_length, true);
}

static bool xhci_parse_configuration(xhci_state_t *state) {
    uint8_t *descriptor;
    uint32_t total_length;
    bool auxiliary;
    bool hid_interface = false;
    uint8_t hid_candidate = 0U;
    uint8_t hid_count = 0U;
    bool audio_interface = false;
    bool hub_interface = false;
    bool bt_interface = false;
    bool msc_interface = false;
    bool uas_interface = false;
    uint8_t uas_last_endpoint = 0U;
    uint16_t uas_last_max_packet = 0U;
    uint8_t uas_last_max_burst = 0U;
    uint8_t uas_last_max_streams = 0U;
    if (state == 0) return false;
    auxiliary = state == xhci_hid_controller_state();
    descriptor = (uint8_t *)state->descriptor_buffer.cpu;
    if (descriptor[0] < 9U || descriptor[1] != XHCI_DESCRIPTOR_CONFIGURATION) {
        return false;
    }
    total_length = (uint32_t)descriptor[2] | ((uint32_t)descriptor[3] << 8);
    if (total_length > PAGE_SIZE) total_length = PAGE_SIZE;
    state->configuration_value = descriptor[5];
    state->hid_interface = 0U;
    state->hid_protocol = 0U;
    state->hid_endpoint = 0U;
    state->hid_max_packet = 0U;
    state->hid_interval = 0U;
    state->hid_secondary.interface_number = 0U;
    state->hid_secondary.protocol = 0U;
    state->hid_secondary.endpoint = 0U;
    state->hid_secondary.max_packet = 0U;
    state->hid_secondary.interval = 0U;
    state->audio_endpoint = 0U;
    state->audio_max_packet = 0U;
    state->audio_interval = 0U;
    state->audio_channels = 0U;
    state->audio_bit_resolution = 0U;
    state->audio_sample_rate = 0U;
    state->hub_interface = 0U;
    state->hub_interface_present = false;
    state->hub_port_count = 0U;
    state->hub_protocol = 0U;
    state->hub_tt_think_time = 0U;
    state->hub_configured = false;
    state->hub_endpoint = 0U;
    state->hub_max_packet = 0U;
    state->hub_interval = 0U;
    state->hub_enqueue = 0U;
    state->hub_cycle = 1U;
    state->hub_transfer_pending = false;
    state->hub_change_bitmap = 0U;
    state->msc_interface = 0U;
    state->msc_bulk_in_endpoint = 0U;
    state->msc_bulk_out_endpoint = 0U;
    state->msc_bulk_in_max_burst = 0U;
    state->msc_bulk_out_max_burst = 0U;
    state->msc_bulk_in_max_packet = 0U;
    state->msc_bulk_out_max_packet = 0U;
    state->msc_configured = false;
    state->msc_uas_present = false;
    state->msc_uas_interface = 0U;
    state->msc_uas_alternate = 0U;
    state->msc_uas_command_endpoint = 0U;
    state->msc_uas_status_endpoint = 0U;
    state->msc_uas_data_in_endpoint = 0U;
    state->msc_uas_data_out_endpoint = 0U;
    state->msc_uas_command_address = 0U;
    state->msc_uas_status_address = 0U;
    state->msc_uas_data_in_address = 0U;
    state->msc_uas_data_out_address = 0U;
    state->msc_uas_command_max_burst = 0U;
    state->msc_uas_status_max_burst = 0U;
    state->msc_uas_data_in_max_burst = 0U;
    state->msc_uas_data_out_max_burst = 0U;
    state->msc_uas_status_max_streams = 0U;
    state->msc_uas_data_in_max_streams = 0U;
    state->msc_uas_data_out_max_streams = 0U;
    state->msc_uas_command_max_packet = 0U;
    state->msc_uas_status_max_packet = 0U;
    state->msc_uas_data_in_max_packet = 0U;
    state->msc_uas_data_out_max_packet = 0U;
    state->bt_event_endpoint = 0U;
    state->bt_acl_in_endpoint = 0U;
    state->bt_acl_out_endpoint = 0U;
    state->bt_event_max_packet = 0U;
    state->bt_acl_in_max_packet = 0U;
    state->bt_acl_out_max_packet = 0U;
    state->bt_event_interval = 0U;
    for (uint32_t offset = 0U; offset + 2U <= total_length;) {
        uint8_t length = descriptor[offset];
        uint8_t type = descriptor[offset + 1U];
        if (length < 2U || offset + length > total_length) return false;
        if (type == XHCI_DESCRIPTOR_INTERFACE && length >= 9U) {
            uint8_t interface_class = descriptor[offset + 5U];
            uint8_t interface_subclass = descriptor[offset + 6U];
            uint8_t alternate = descriptor[offset + 3U];
            uint8_t interface_protocol = descriptor[offset + 7U];
            /* Boot protocol gives the driver a fixed report format, avoiding
             * a report-descriptor parser in the interrupt fast path. */
            hid_interface = interface_class == USB_CLASS_HID &&
                            interface_subclass == USB_HID_SUBCLASS_BOOT &&
                            (interface_protocol == USB_HID_PROTOCOL_KEYBOARD ||
                             interface_protocol == USB_HID_PROTOCOL_MOUSE) &&
                            alternate == 0U;
            hid_candidate = 0U;
            if (hid_interface && hid_count < 2U) {
                hid_candidate = ++hid_count;
                if (hid_candidate == 1U) {
                    state->hid_interface = descriptor[offset + 2U];
                    state->hid_protocol = interface_protocol;
                } else {
                    state->hid_secondary.interface_number =
                        descriptor[offset + 2U];
                    state->hid_secondary.protocol = interface_protocol;
                }
            }
            audio_interface = interface_class == USB_CLASS_AUDIO &&
                              interface_subclass == USB_AUDIO_SUBCLASS_STREAM &&
                              alternate != 0U;
            hub_interface =
                interface_class == USB_CLASS_HUB &&
                alternate == 0U;
            bt_interface = interface_class == USB_CLASS_WIRELESS &&
                           interface_subclass == USB_BT_SUBCLASS &&
                           descriptor[offset + 7U] == USB_BT_PROTOCOL;
            msc_interface =
                interface_class == USB_CLASS_MASS_STORAGE &&
                interface_subclass == USB_MSC_SUBCLASS_SCSI &&
                interface_protocol == USB_MSC_PROTOCOL_BULK_ONLY &&
                alternate == 0U;
            uas_interface =
                interface_class == USB_CLASS_MASS_STORAGE &&
                interface_subclass == USB_MSC_SUBCLASS_SCSI &&
                interface_protocol == USB_MSC_PROTOCOL_UAS;
            uas_last_endpoint = 0U;
            uas_last_max_packet = 0U;
            uas_last_max_burst = 0U;
            uas_last_max_streams = 0U;
            if (msc_interface) {
                state->msc_interface = descriptor[offset + 2U];
            }
            if (uas_interface) {
                state->msc_uas_present = true;
                state->msc_uas_interface = descriptor[offset + 2U];
                state->msc_uas_alternate = alternate;
            }
            if (audio_interface) {
                state->audio_interface = descriptor[offset + 2U];
                state->audio_alt_setting = alternate;
            }
            if (hub_interface) {
                state->hub_interface = descriptor[offset + 2U];
                state->hub_interface_present = true;
                state->hub_protocol = interface_protocol;
            }
        } else if (type == USB_SS_ENDPOINT_COMPANION && length >= 3U &&
                   uas_interface && uas_last_endpoint != 0U) {
            uas_last_max_burst = descriptor[offset + 2U];
            if (length >= 4U)
                uas_last_max_streams = descriptor[offset + 3U] & 0x1FU;
        } else if (type == USB_UAS_PIPE_USAGE && length >= 4U &&
                   uas_interface && uas_last_endpoint != 0U) {
            uint8_t pipe_id = descriptor[offset + 2U];
            switch (pipe_id) {
            case USB_UAS_PIPE_COMMAND:
                state->msc_uas_command_address = uas_last_endpoint;
                state->msc_uas_command_endpoint = uas_last_endpoint & 0x0FU;
                state->msc_uas_command_max_packet = uas_last_max_packet;
                state->msc_uas_command_max_burst = uas_last_max_burst;
                break;
            case USB_UAS_PIPE_STATUS:
                state->msc_uas_status_address = uas_last_endpoint;
                state->msc_uas_status_endpoint = uas_last_endpoint & 0x0FU;
                state->msc_uas_status_max_packet = uas_last_max_packet;
                state->msc_uas_status_max_burst = uas_last_max_burst;
                state->msc_uas_status_max_streams = uas_last_max_streams;
                break;
            case USB_UAS_PIPE_DATA_IN:
                state->msc_uas_data_in_address = uas_last_endpoint;
                state->msc_uas_data_in_endpoint = uas_last_endpoint & 0x0FU;
                state->msc_uas_data_in_max_packet = uas_last_max_packet;
                state->msc_uas_data_in_max_burst = uas_last_max_burst;
                state->msc_uas_data_in_max_streams = uas_last_max_streams;
                break;
            case USB_UAS_PIPE_DATA_OUT:
                state->msc_uas_data_out_address = uas_last_endpoint;
                state->msc_uas_data_out_endpoint = uas_last_endpoint & 0x0FU;
                state->msc_uas_data_out_max_packet = uas_last_max_packet;
                state->msc_uas_data_out_max_burst = uas_last_max_burst;
                state->msc_uas_data_out_max_streams = uas_last_max_streams;
                break;
            default:
                break;
            }
            uas_last_endpoint = 0U;
        } else if (type == XHCI_DESCRIPTOR_CS_INTERFACE && length >= 8U &&
                   audio_interface && descriptor[offset + 2U] == 0x02U) {
            /* Audio Format Type I锛氳褰?PCM 璁惧鐨勫熀鏈害鏉燂紝渚涗笂灞傞€夊懆鏈熴€?*/
            state->audio_channels = descriptor[offset + 4U];
            state->audio_bit_resolution = descriptor[offset + 6U];
            if (descriptor[offset + 7U] == 1U && length >= 11U) {
                state->audio_sample_rate = (uint32_t)descriptor[offset + 8U] |
                    ((uint32_t)descriptor[offset + 9U] << 8) |
                    ((uint32_t)descriptor[offset + 10U] << 16);
            }
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U && hid_interface &&
                   (descriptor[offset + 2U] & 0x80U) != 0U &&
                   (descriptor[offset + 3U] & 0x03U) == 0x03U) {
            if (hid_candidate == 1U && state->hid_endpoint == 0U) {
                uint8_t endpoint = descriptor[offset + 2U] & 0x0FU;
                uint16_t max_packet = (uint16_t)descriptor[offset + 4U] |
                                      ((uint16_t)descriptor[offset + 5U] << 8);
                if (endpoint != 0U && max_packet != 0U && max_packet <= PAGE_SIZE) {
                    state->hid_endpoint = endpoint;
                    state->hid_max_packet = max_packet;
                    state->hid_interval = descriptor[offset + 6U];
                }
            } else if (hid_candidate == 2U &&
                       state->hid_secondary.endpoint == 0U) {
                uint8_t endpoint = descriptor[offset + 2U] & 0x0FU;
                uint16_t max_packet = (uint16_t)descriptor[offset + 4U] |
                                      ((uint16_t)descriptor[offset + 5U] << 8);
                if (endpoint != 0U && max_packet != 0U && max_packet <= PAGE_SIZE) {
                    state->hid_secondary.endpoint = endpoint;
                    state->hid_secondary.max_packet = max_packet;
                    state->hid_secondary.interval = descriptor[offset + 6U];
                }
            }
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U && hub_interface &&
                   (descriptor[offset + 2U] & 0x80U) != 0U &&
                   (descriptor[offset + 3U] & 0x03U) == USB_TRANSFER_INTERRUPT) {
            if (state->hub_endpoint == 0U) {
                state->hub_endpoint = descriptor[offset + 2U] & 0x0FU;
                state->hub_max_packet = (uint16_t)descriptor[offset + 4U] |
                                        ((uint16_t)descriptor[offset + 5U] << 8);
                state->hub_interval = descriptor[offset + 6U];
            }
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U && audio_interface &&
                   (descriptor[offset + 3U] & 0x03U) == USB_TRANSFER_ISOCHRONOUS &&
                   state->audio_endpoint == 0U) {
            state->audio_endpoint = descriptor[offset + 2U] & 0x0FU;
            state->audio_endpoint_in = (descriptor[offset + 2U] & 0x80U) != 0U;
            state->audio_max_packet = ((uint16_t)descriptor[offset + 4U] |
                                       ((uint16_t)descriptor[offset + 5U] << 8)) & 0x07FFU;
            state->audio_interval = descriptor[offset + 6U];
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U &&
                   uas_interface &&
                   (descriptor[offset + 3U] & 0x03U) == USB_TRANSFER_BULK) {
            uas_last_endpoint = descriptor[offset + 2U];
            uas_last_max_packet = (uint16_t)descriptor[offset + 4U] |
                                  ((uint16_t)descriptor[offset + 5U] << 8);
            uas_last_max_burst = 0U;
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U &&
                   msc_interface &&
                   (descriptor[offset + 3U] & 0x03U) == USB_TRANSFER_BULK) {
            uint8_t endpoint = descriptor[offset + 2U] & 0x0FU;
            bool direction_in = (descriptor[offset + 2U] & 0x80U) != 0U;
            uint16_t max_packet =
                (uint16_t)descriptor[offset + 4U] |
                ((uint16_t)descriptor[offset + 5U] << 8);
            if (endpoint != 0U && max_packet != 0U) {
                if (direction_in && state->msc_bulk_in_endpoint == 0U) {
                    state->msc_bulk_in_endpoint = endpoint;
                    state->msc_bulk_in_max_packet = max_packet;
                    if (offset + length + 2U <= total_length &&
                        descriptor[offset + length + 1U] == 0x30U &&
                        descriptor[offset + length] >= 3U) {
                        state->msc_bulk_in_max_burst =
                            descriptor[offset + length + 2U];
                    }
                } else if (!direction_in &&
                           state->msc_bulk_out_endpoint == 0U) {
                    state->msc_bulk_out_endpoint = endpoint;
                    state->msc_bulk_out_max_packet = max_packet;
                    if (offset + length + 2U <= total_length &&
                        descriptor[offset + length + 1U] == 0x30U &&
                        descriptor[offset + length] >= 3U) {
                        state->msc_bulk_out_max_burst =
                            descriptor[offset + length + 2U];
                    }
                }
            }
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U && bt_interface) {
            uint8_t endpoint = descriptor[offset + 2U] & 0x0FU;
            uint8_t direction_in = descriptor[offset + 2U] & 0x80U;
            uint8_t transfer_type = descriptor[offset + 3U] & 0x03U;
            uint16_t max_packet = (uint16_t)descriptor[offset + 4U] |
                                  ((uint16_t)descriptor[offset + 5U] << 8);
            /* Alternate setting zero may expose zero-bandwidth isochronous
             * endpoints. Ignore them, but always advance to the next
             * descriptor; continuing here used to loop forever. */
            if (endpoint != 0U && max_packet != 0U) {
                if (direction_in != 0U &&
                    transfer_type == USB_TRANSFER_INTERRUPT &&
                    state->bt_event_endpoint == 0U) {
                    state->bt_event_endpoint = endpoint;
                    state->bt_event_max_packet = max_packet;
                    state->bt_event_interval = descriptor[offset + 6U];
                } else if (direction_in != 0U &&
                           transfer_type == USB_TRANSFER_BULK &&
                           state->bt_acl_in_endpoint == 0U) {
                    state->bt_acl_in_endpoint = endpoint;
                    state->bt_acl_in_max_packet = max_packet;
                } else if (direction_in == 0U &&
                           transfer_type == USB_TRANSFER_BULK &&
                           state->bt_acl_out_endpoint == 0U) {
                    state->bt_acl_out_endpoint = endpoint;
                    state->bt_acl_out_max_packet = max_packet;
                }
            }
        }
        offset += length;
    }
    if (state->msc_uas_present) {
        liteos_serial_printf_serial_only(
            "LITEOS_UAS_DESC SLOT=%u ALT=%u CMD=%u STATUS=%u IN=%u OUT=%u "
            "MPS=%u/%u/%u/%u STREAMS=%u/%u/%u\r\n",
            state->device_slot, state->msc_uas_alternate,
            state->msc_uas_command_endpoint, state->msc_uas_status_endpoint,
            state->msc_uas_data_in_endpoint, state->msc_uas_data_out_endpoint,
            state->msc_uas_command_max_packet,
            state->msc_uas_status_max_packet,
            state->msc_uas_data_in_max_packet,
            state->msc_uas_data_out_max_packet,
            state->msc_uas_status_max_streams,
            state->msc_uas_data_in_max_streams,
            state->msc_uas_data_out_max_streams);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_INTERFACE",
                                    state->msc_uas_interface);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_ALTERNATE",
                                    state->msc_uas_alternate);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_COMMAND",
                                    state->msc_uas_command_endpoint);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_STATUS",
                                    state->msc_uas_status_endpoint);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_DATA_IN",
                                    state->msc_uas_data_in_endpoint);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_DATA_OUT",
                                    state->msc_uas_data_out_endpoint);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_COMMAND_MPS",
                                    state->msc_uas_command_max_packet);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_STATUS_MPS",
                                    state->msc_uas_status_max_packet);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_DATA_IN_MPS",
                                    state->msc_uas_data_in_max_packet);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_DATA_OUT_MPS",
                                    state->msc_uas_data_out_max_packet);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_STATUS_STREAMS",
                                    state->msc_uas_status_max_streams);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_DATA_IN_STREAMS",
                                    state->msc_uas_data_in_max_streams);
        liteos_realtest_mark_number("USB_MSC_UAS_DESC_DATA_OUT_STREAMS",
                                    state->msc_uas_data_out_max_streams);
    }
    if (auxiliary) {
        liteos_realtest_mark_number("XHCI_AUX_CONFIG_TOTAL_LENGTH",
                                    total_length);
        liteos_realtest_mark_number("XHCI_AUX_CONFIG_VALUE",
                                    state->configuration_value);
        liteos_realtest_mark_number("XHCI_AUX_HID_INTERFACE",
                                    state->hid_interface);
        liteos_realtest_mark_number("XHCI_AUX_HID_PROTOCOL",
                                    state->hid_protocol);
        liteos_realtest_mark_number("XHCI_AUX_HID_ENDPOINT",
                                    state->hid_endpoint);
        liteos_realtest_mark_number("XHCI_AUX_HID_MAX_PACKET",
                                    state->hid_max_packet);
        liteos_realtest_mark_number("XHCI_AUX_HID_INTERVAL",
                                    state->hid_interval);
        liteos_realtest_mark_number("XHCI_AUX_HID2_INTERFACE",
                                    state->hid_secondary.interface_number);
        liteos_realtest_mark_number("XHCI_AUX_HID2_PROTOCOL",
                                    state->hid_secondary.protocol);
        liteos_realtest_mark_number("XHCI_AUX_HID2_ENDPOINT",
                                    state->hid_secondary.endpoint);
        liteos_realtest_mark_number("XHCI_AUX_HID2_MAX_PACKET",
                                    state->hid_secondary.max_packet);
    }
    return ((state->hid_endpoint != 0U && state->hid_max_packet != 0U) ||
            (state->hid_secondary.endpoint != 0U &&
             state->hid_secondary.max_packet != 0U)) ||
           (state->audio_endpoint != 0U && state->audio_max_packet != 0U) ||
           state->hub_interface_present ||
           (state->msc_bulk_in_endpoint != 0U &&
            state->msc_bulk_out_endpoint != 0U &&
            state->msc_bulk_in_max_packet != 0U &&
            state->msc_bulk_out_max_packet != 0U) ||
           (state->msc_uas_present &&
            state->msc_uas_command_endpoint != 0U &&
            state->msc_uas_status_endpoint != 0U &&
            state->msc_uas_data_in_endpoint != 0U &&
            state->msc_uas_data_out_endpoint != 0U &&
            state->msc_uas_command_max_packet != 0U &&
            state->msc_uas_status_max_packet != 0U &&
            state->msc_uas_data_in_max_packet != 0U &&
            state->msc_uas_data_out_max_packet != 0U) ||
           (state->bt_event_endpoint != 0U &&
            (state->bt_acl_in_endpoint != 0U || state->bt_acl_out_endpoint != 0U));
}

/*
 * V3.10.1B EXPLICIT HUB GET STATUS
 *
 * Hub EP0 state belongs to the supplied device context.
 */
/*
 * V3.10.1E HUB CONTROL WRAPPERS REMOVED
 *
 * All Hub control callers now name the device context explicitly.
 * Runtime callers still pass &state->device in this stage.
 */
/*
 * V3.9.6 INITIAL HUB SLOT WALK
 *
 * Initial downstream enumeration is now entirely Slot Table based.
 *
 * Newly discovered Hub Slots are processed in a following pass,
 * allowing arbitrary xHCI-supported Hub nesting without inventory.
 */
/*
 * V3.10.1D DIRECT STARTUP HUB CONTEXT
 *
 * Startup Hub control transfers operate directly on the canonical
 * Slot.context.
 *
 * state->device is reserved for a child while that child is being
 * enumerated.
 */
/*
 * V3.10.6B10B4 HUB RUNTIME ARM BARRIER
 *
 * A Hub interrupt-IN TD is allowed to exist only after the explicit startup
 * topology walk has reached a stable baseline.  This prevents port resets
 * used for enumeration from becoming runtime hotplug events.
 */
/*
 * V3.10.6B10B8 STARTUP EVENT DRAIN
 *
 * Hub endpoints are armed during Configure Endpoint so QEMU can establish its
 * normal NAK/retry ownership.  Initial downstream probing then produces
 * expected C_CONNECTION/C_ENABLE/C_RESET notifications.
 *
 * Consume those startup completions before MSI-X runtime is exposed.  The
 * ordinary event handlers perform the same reconciliation/ACK/re-arm logic as
 * runtime, so there is only one Hub state machine.
 */


static uint8_t xhci_interrupt_interval(
    uint8_t speed,
    uint8_t b_interval)
{
    if(b_interval == 0U)
    {
        return 0U;
    }

    /*
     * xHCI speed IDs used by this driver:
     *
     *   1 = Full
     *   2 = Low
     *   3 = High
     *   4 = Super
     */
    if(speed >= 3U)
    {
        uint8_t value =
            b_interval;

        if(value > 16U)
        {
            value =
                16U;
        }

        return
            (uint8_t)(
                value - 1U);
    }

    /*
     * Full/Low-Speed bInterval is in 1ms frames.
     *
     * Convert:
     *
     *     milliseconds -> microframes
     *
     * then round DOWN to the nearest power of two, matching the
     * xHCI scheduling rule used by mature host-controller drivers.
     */
    uint32_t microframes =
        (uint32_t)b_interval *
        8U;

    uint8_t exponent =
        0U;

    while(exponent < 15U &&
          (1U <<
           (uint32_t)(
               exponent + 1U)) <=
              microframes)
    {
        ++exponent;
    }

    /*
     * FS/LS periodic endpoint valid useful range:
     *
     *   2^3 microframes =   1 ms
     *   2^10 microframes = 128 ms
     */
    if(exponent < 3U)
    {
        exponent =
            3U;
    }

    if(exponent > 10U)
    {
        exponent =
            10U;
    }

    return exponent;
}


void xhci_init_endpoint_context(xhci_state_t *state, uint32_t *endpoint,
                                uint8_t interval, uint8_t type,
                                uint16_t max_packet,
                                const dma_mapping_t *ring) {
    uint64_t address = xhci_dma_address(ring);
    uint8_t encoded_interval =
        interval;

    /*
     * Endpoint types:
     *
     *   3 = Interrupt OUT
     *   7 = Interrupt IN
     */
    if(type == 3U ||
       type == 7U)
    {
        encoded_interval =
            xhci_interrupt_interval(
                state->device_speed,
                interval);
    }

    xhci_endpoint_context_encode(endpoint, encoded_interval, type,
                                  max_packet, address);
    /* Linux leaves Average TRB Length at zero for bulk endpoints: their
     * transfer buffer size is not known when the endpoint is initialized.
     * Periodic endpoints use their packet size as a conservative payload. */
    if (type == 3U || type == 7U)
        xhci_endpoint_context_set_max_esit_payload(endpoint, max_packet);
    if (type == 3U || type == 7U)
        xhci_endpoint_context_set_average_trb_length(endpoint, max_packet);
    else if (type == 4U && state != 0 && state->hci_version >= 0x0100U)
        xhci_endpoint_context_set_average_trb_length(endpoint, 8U);
}

/*
 * V3.10.5A EXPLICIT BT QUEUES
 *
 * Bluetooth endpoint/ring producer state belongs to the explicit
 * device context.  Controller MMIO state remains in xhci_state_t.
 */
static bool xhci_configure_hid_endpoint(xhci_state_t *state) {
    uint32_t endpoint_id;
    uint32_t secondary_endpoint_id = 0U;
    uint32_t last_endpoint_id;
    bool has_primary;
    bool has_secondary;
    if (state == 0 ||
        (state->hid_endpoint == 0U && state->hid_secondary.endpoint == 0U)) {
        return false;
    }
    if (state->hid_endpoint != 0U &&
        ((state->hid_protocol != USB_HID_PROTOCOL_KEYBOARD &&
          state->hid_protocol != USB_HID_PROTOCOL_MOUSE) ||
         state->hid_max_packet == 0U || state->hid_max_packet > PAGE_SIZE)) {
        return false;
    }
    if (state->hid_secondary.endpoint != 0U &&
        ((state->hid_secondary.protocol != USB_HID_PROTOCOL_KEYBOARD &&
          state->hid_secondary.protocol != USB_HID_PROTOCOL_MOUSE) ||
         state->hid_secondary.max_packet == 0U ||
         state->hid_secondary.max_packet > PAGE_SIZE)) {
        return false;
    }
    has_primary = state->hid_endpoint != 0U;
    has_secondary = state->hid_secondary.endpoint != 0U;
    endpoint_id = has_primary ?
        (uint32_t)state->hid_endpoint * 2U + 1U : 0U;
    if (has_primary && endpoint_id > 31U) return false;
    if (has_secondary) {
        secondary_endpoint_id =
            (uint32_t)state->hid_secondary.endpoint * 2U + 1U;
        if (secondary_endpoint_id > 31U ||
            secondary_endpoint_id == endpoint_id) return false;
    }
    /* Context Entries is the highest enabled DCI, not the first HID DCI.
     * A composite receiver commonly places mouse on DCI 3 and keyboard on
     * DCI 7.  Advertising only DCI 3 makes the controller ignore the valid
     * keyboard endpoint context even though its Add Context flag is set. */
    last_endpoint_id = endpoint_id > secondary_endpoint_id ?
        endpoint_id : secondary_endpoint_id;
    if (state == xhci_hid_controller_state()) {
        liteos_realtest_mark_number("XHCI_AUX_HID_CONTEXT_ENTRIES",
                                    last_endpoint_id);
    }
    if (state->hid_endpoint != 0U &&
        (!xhci_alloc_page(state, &state->hid_ring, DMA_BIDIRECTIONAL) ||
         !xhci_alloc_page(state, &state->hid_report, DMA_FROM_DEVICE))) {
        xhci_free_page(&state->hid_ring);
        xhci_free_page(&state->hid_report);
        return false;
    }
    if (has_secondary &&
        (!xhci_alloc_page(state, &state->hid_secondary.ring,
                          DMA_BIDIRECTIONAL) ||
         !xhci_alloc_page(state, &state->hid_secondary.report,
                          DMA_FROM_DEVICE))) {
        xhci_free_page(&state->hid_secondary.ring);
        xhci_free_page(&state->hid_secondary.report);
        xhci_free_page(&state->hid_ring);
        xhci_free_page(&state->hid_report);
        return false;
    }
    state->hid_enqueue = 0U;
    state->hid_cycle = 1U;
    state->hid_transfer_pending = false;
    uint8_t *input = (uint8_t *)state->input_context.cpu;
    uint32_t *control = (uint32_t *)input;
    uint32_t *input_slot = (uint32_t *)(input + state->context_size);
    uint32_t *output_slot = (uint32_t *)state->output_context.cpu;
    for (uint32_t i = 0; i < 4U; ++i) input_slot[i] = output_slot[i];
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= last_endpoint_id << 27;
    control[0] = 0U;
    /* Add Context Flags 蹇呴』鍚屾椂鍖呭惈 Slot Context 鍜岀洰鏍囩鐐广€?*/
    control[1] = 1U;
    if (has_primary) control[1] |= 1U << endpoint_id;
    if (has_secondary) control[1] |= 1U << secondary_endpoint_id;
    if (has_primary) {
        uint32_t *endpoint = (uint32_t *)(input +
            state->context_size * (endpoint_id + 1U));
        xhci_init_endpoint_context(state, endpoint, state->hid_interval, 7U,
                                   state->hid_max_packet,
                                   &state->hid_ring.mapping);
        xhci_trb_t *ring = (xhci_trb_t *)state->hid_ring.cpu;
        ring[XHCI_RING_TRB_COUNT - 1U].parameter =
            xhci_dma_address(&state->hid_ring.mapping);
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
    }
    if (has_secondary) {
        uint8_t *input_secondary = (uint8_t *)state->input_context.cpu;
        uint32_t *endpoint_secondary = (uint32_t *)(input_secondary +
            state->context_size * (secondary_endpoint_id + 1U));
        state->hid_secondary.enqueue = 0U;
        state->hid_secondary.cycle = 1U;
        state->hid_secondary.transfer_pending = false;
        xhci_init_endpoint_context(
            state, endpoint_secondary, state->hid_secondary.interval, 7U,
            state->hid_secondary.max_packet,
            &state->hid_secondary.ring.mapping);
        xhci_trb_t *secondary_ring =
            (xhci_trb_t *)state->hid_secondary.ring.cpu;
        secondary_ring[XHCI_RING_TRB_COUNT - 1U].parameter =
            xhci_dma_address(&state->hid_secondary.ring.mapping);
        secondary_ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
    }
    dma_sync_for_device(&state->input_context.mapping);
    if (has_primary) {
        dma_sync_for_device(&state->hid_ring.mapping);
        dma_sync_for_device(&state->hid_report.mapping);
    }
    if (has_secondary) {
        dma_sync_for_device(&state->hid_secondary.ring.mapping);
        dma_sync_for_device(&state->hid_secondary.report.mapping);
    }
    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE,
                             state->device_slot,
                             xhci_dma_address(&state->input_context.mapping), 0)) {
        if (has_primary) {
            xhci_free_page(&state->hid_ring);
            xhci_free_page(&state->hid_report);
        }
        if (has_secondary) {
            xhci_free_page(&state->hid_secondary.ring);
            xhci_free_page(&state->hid_secondary.report);
        }
        return false;
    }
    if (has_primary &&
        !xhci_queue_hid_report(state, &state->device)) return false;
    return !has_secondary ||
           xhci_queue_hid_report_secondary(state, &state->device);
}

static void xhci_enumeration_rollback(bool publish_topology,
                                       uint8_t slot,
                                       uint8_t parent_slot,
                                       uint8_t parent_port) {
    if (publish_topology) {
        xhci_unpublish_slot_device(slot, parent_slot, parent_port);
    }
}

static void xhci_mark_aux_enumeration_stage(bool auxiliary_hid,
                                             uint32_t stage) {
    if (auxiliary_hid) {
        liteos_realtest_mark_number("XHCI_AUX_ENUM_STAGE", stage);
    }
}

/* Keep the existing failure paths identical for the normal enumerator while
 * making the auxiliary HID path independent of the primary Slot Table. */
#define xhci_unpublish_slot_device(slot, parent_slot, parent_port) \
    xhci_enumeration_rollback(publish_topology, slot, parent_slot, parent_port)

static bool xhci_enumerate_device_mode(xhci_state_t *state, uint8_t slot,
                                       uint8_t port, uint8_t speed,
                                       uint8_t root_port,
                                       uint8_t parent_slot,
                                       uint8_t parent_port,
                                       uint32_t route_string,
                                       bool publish_topology) {
    bool auxiliary_hid = !publish_topology;

    xhci_mark_aux_enumeration_stage(auxiliary_hid, 1U);
    if (auxiliary_hid) xhci_clear_error();

    /*
     * V3.7.2 ENUMERATION FAILURE ROLLBACK
     *
     * Slot Table and USB Core objects can be created before
     * hardware enumeration has fully succeeded.
     *
     * Every failure exit must remove those tentative software
     * objects.  Hardware Disable Slot and DMA teardown remain
     * owned by the existing caller.
     */


    /*
     * V3.5.2 USB CORE BIND
     *
     * Bind xHCI Slot ID to USB Core device.
     */


    /*
     * V3.5.3 USB descriptor sync
     *
     * Bind xHCI enumeration result
     * into USB Core object.
     */

    usb_device_t *usb_dev = publish_topology ? usb_alloc_device(slot) : 0;


    if(usb_dev != 0)
    {
        usb_dev->speed =
            speed;

        usb_dev->parent_slot =
            parent_slot;

        usb_dev->parent_port =
            parent_port;


        /*
         * Device identity.
         *
         * Descriptor parser will
         * fill these fields later.
         */
        usb_dev->device_class =
            0;

        usb_dev->subclass =
            0;

        usb_dev->protocol =
            0;
    }

    /*
     * V3.6.4 USB HUB CLASS BIND
     *
     * Hub devices get their own class object.
     */

    if(publish_topology && usb_dev != 0 &&
       usb_dev->device_class == 0x09)
    {
        usb_hub_init(
            slot,
            0);
    }






    /*
     * V3.2 Slot Table mirror
     *
     * Keep old inventory path unchanged.
     * This only creates a native Slot ID view.
     */
    /*
     * parent_slot/parent_port remain software topology.
     *
     * tt_slot/tt_port describe the hardware split-transaction
     * domain required by xHCI Slot Context DW2.
     */
    uint8_t tt_slot = 0U;
    uint8_t tt_port = 0U;
    bool tt_multi = false;
    if(!xhci_resolve_tt(
           speed,
           parent_slot,
           parent_port,
           &tt_slot,
           &tt_port,
           &tt_multi) ||
       (publish_topology && !xhci_topology_begin_slot(
           slot,
           speed,
           parent_slot,
           parent_port,
           root_port,
           route_string,
           tt_slot,
           tt_port,
           tt_multi)))
    {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);

        return false;
    }


    static const uint8_t get_device_descriptor_header[8] = {
        0x80U, XHCI_SETUP_GET_DESCRIPTOR, 0x00U, XHCI_DESCRIPTOR_DEVICE,
        0x00U, 0x00U, 0x08U, 0x00U
    };
    static const uint8_t get_device_descriptor[8] = {
        0x80U, XHCI_SETUP_GET_DESCRIPTOR, 0x00U, XHCI_DESCRIPTOR_DEVICE,
        0x00U, 0x00U, XHCI_DESCRIPTOR_DEVICE_LENGTH, 0x00U

    /* V3.2 Slot Table mirror completed */
};
    uint8_t set_configuration[8] = {0};
    uint8_t set_interface[8] = {0};
    /* High-speed EP0 is already 64 bytes wide.  The AMD auxiliary controller
     * is more reliable when it receives the full descriptor in one request;
     * low/full-speed devices still need the eight-byte MPS probe. */
    bool probe_ep0_size = publish_topology || speed < 3U;
    if (!xhci_prepare_device_resources(state, slot, port, speed, root_port,
                                        parent_slot, parent_port,
                                        tt_slot,
                                        tt_port,
                                        tt_multi,
                                        route_string)) {
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 2U);
        if (xhci_last_error() == 0U) xhci_set_error(53U);
        do {
            xhci_unpublish_slot_device(slot, parent_slot, parent_port);
            return false;
        } while (0);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 2U);
    if (!xhci_submit_address_device(
            state, slot, xhci_dma_address(&state->input_context.mapping), true)) {
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 3U);
        if (xhci_last_error() == 0U) xhci_set_error(55U);
        do {
            xhci_unpublish_slot_device(slot, parent_slot, parent_port);
            return false;
        } while (0);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 3U);
    if (probe_ep0_size) {
        if (!xhci_submit_control_transfer_device(
                state, &state->device, get_device_descriptor_header,
                8U, true)) {
            xhci_mark_aux_enumeration_stage(auxiliary_hid, 4U);
            do {
                xhci_unpublish_slot_device(slot, parent_slot, parent_port);
                return false;
            } while (0);
        }
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 4U);
        uint8_t *header = (uint8_t *)state->descriptor_buffer.cpu;
        if (auxiliary_hid) {
            liteos_realtest_mark_number("XHCI_AUX_EP0_MPS", header[7]);
            liteos_realtest_mark_number("XHCI_AUX_EP0_MPS_EFFECTIVE",
                xhci_ep0_descriptor_max_packet(&state->device, header));
        }
        /* The first Address Device command used BSR so EP0 could read the
         * descriptor header while the slot remained in Default state.  A
         * non-BSR command is required even when bMaxPacketSize0 already
         * matches the initial value; it assigns the USB address and moves the
         * slot to Addressed state. */
        if (!xhci_complete_device_address(state, &state->device, header)) {
            xhci_mark_aux_enumeration_stage(auxiliary_hid, 5U);
            do {
                xhci_unpublish_slot_device(slot, parent_slot, parent_port);
                return false;
            } while (0);
        }
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 6U);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 7U);
    if (!xhci_submit_control_transfer_device(state, &state->device,
                                              get_device_descriptor,
                                              XHCI_DESCRIPTOR_DEVICE_LENGTH,
                                              true)) {
        liteos_realtest_mark_number("XHCI_AUX_FULL_ERROR", xhci_last_error());
        do {
            xhci_unpublish_slot_device(slot, parent_slot, parent_port);
            return false;
        } while (0);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 8U);
    uint8_t *descriptor = (uint8_t *)state->descriptor_buffer.cpu;
    if (descriptor[0] < XHCI_DESCRIPTOR_DEVICE_LENGTH ||
        descriptor[1] != XHCI_DESCRIPTOR_DEVICE) {
        xhci_set_error(56U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }
    if (publish_topology) {
        uint16_t vendor = (uint16_t)descriptor[8] |
                          ((uint16_t)descriptor[9] << 8);
        uint16_t product = (uint16_t)descriptor[10] |
                           ((uint16_t)descriptor[11] << 8);
        liteos_realtest_mark_number("XHCI_DEVICE_SLOT", slot);
        liteos_realtest_mark_number("XHCI_DEVICE_ROOT", root_port);
        liteos_realtest_mark_number("XHCI_DEVICE_VID", vendor);
        liteos_realtest_mark_number("XHCI_DEVICE_PID", product);
        liteos_realtest_mark_number("XHCI_DEVICE_CLASS", descriptor[4]);
        liteos_serial_printf_serial_only(
            "LITEOS_DIAG_USB_DEVICE SLOT=%u ROOT=%u PARENT=%u PORT=%u "
            "VID=%u PID=%u\r\n",
            slot, root_port, parent_slot, parent_port, vendor, product);
    }
    if (auxiliary_hid && !probe_ep0_size &&
        !xhci_complete_device_address(state, &state->device, descriptor)) {
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 9U);
        if (xhci_last_error() == 0U) xhci_set_error(58U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 9U);
    if (!xhci_read_configuration_descriptor(state) ||
        !xhci_parse_configuration(state)) {
        /* Keep the EP0 completion/timeout code.  Replacing it with the
         * generic parser code made real hardware failures look identical and
         * hid whether the controller ever completed the request. */
        if (xhci_last_error() == 0U) xhci_set_error(57U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }
    set_configuration[0] = 0x00U;
    set_configuration[1] = XHCI_SETUP_SET_CONFIGURATION;
    set_configuration[2] = state->configuration_value;
    if (auxiliary_hid) {
        liteos_realtest_mark_number("XHCI_AUX_SET_CONFIGURATION",
                                    state->configuration_value);
    }
    if (!xhci_submit_control_transfer_device(state, &state->device,  set_configuration, 0U, false)) {
        if (auxiliary_hid) {
            liteos_realtest_mark_number("XHCI_AUX_SET_CONFIGURATION_ERROR",
                                        xhci_last_error());
        }
        xhci_set_error(58U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }
    /* Hub 绗竴闃舵鍙畬鎴愮被璇嗗埆鍜?Hub 鎻忚堪绗﹁鍙栵紱涓嬫父绔彛鐨?     * route string銆乻lot/context 闅旂浠ュ強绔彛浜嬩欢澶勭悊鍦ㄥ悗缁樁娈垫帴鍏ャ€?*/
    if (state->hub_interface_present) {
        if (!xhci_read_hub_descriptor(state)) do {
        liteos_serial_write_serial_only("LITEOS_DIAG_HUB_DESCRIPTOR_FAIL\r\n");
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        liteos_serial_write_serial_only("LITEOS_DIAG_HUB_DESCRIPTOR_OK\r\n");

        /*
         * Establish a clean Hub change baseline before interrupt-IN is
         * armed. Existing downstream devices are discovered explicitly by
         * xhci_hub_runtime_probe_downstream(), so clearing their old C_CONNECTION
         * state here cannot hide a device.
         */
        if(!xhci_hub_ack_all_port_changes_device(
               state,
               &state->device))
        {
            liteos_serial_write_serial_only("LITEOS_DIAG_HUB_ACK_FAIL\r\n");
            if(xhci_last_error() == 0U)
            {
                xhci_set_error(68U);
            }

            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
        liteos_serial_write_serial_only("LITEOS_DIAG_HUB_ACK_OK\r\n");

        if (state->hub_endpoint != 0U && !xhci_configure_hub_endpoint(state)) {
            liteos_serial_write_serial_only(
                "LITEOS_DIAG_HUB_CONFIGURE_FAIL\r\n");
            if (xhci_last_error() == 0U) xhci_set_error(68U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
        liteos_serial_write_serial_only("LITEOS_DIAG_HUB_CONFIGURE_OK\r\n");
    } else if (state->bt_event_endpoint != 0U) {
        if (!xhci_configure_bt_endpoints(state)) {
            if (xhci_last_error() == 0U) xhci_set_error(65U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
    } else if (state->audio_endpoint != 0U) {
        /* USB Audio 鐨勭瓑鏃剁鐐归€氬父浣嶄簬 alternate setting 1锛?         * 鍙湁鍒囨崲鎺ュ彛鍚庤澶囨墠浼氱湡姝ｆ墦寮€鏁版嵁甯﹀銆?*/
        set_interface[0] = 0x01U;
        set_interface[1] = XHCI_SETUP_SET_INTERFACE;
        set_interface[2] = state->audio_alt_setting;
        set_interface[4] = state->audio_interface;
        if (!xhci_submit_control_transfer_device(state, &state->device,  set_interface, 0U, false)) {
            xhci_set_error(59U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
    }
    if (state->hid_endpoint != 0U ||
        state->hid_secondary.endpoint != 0U) {
        /* A Boot-subclass interface may currently be in report protocol.
         * Select Boot protocol before arming its fixed-format interrupt TRB. */
        if (state->hid_endpoint != 0U) {
            uint8_t set_protocol[8] = {
                0x21U, USB_HID_SET_PROTOCOL, USB_HID_PROTOCOL_BOOT, 0x00U,
                state->hid_interface, 0x00U, 0x00U, 0x00U,
            };
            if (!xhci_submit_control_transfer_device(
                    state, &state->device, set_protocol, 0U, false)) {
                xhci_set_error(59U);
                do {
                    xhci_unpublish_slot_device(slot, parent_slot,
                                                parent_port);
                    return false;
                } while (0);
            }
        }
        if (state->hid_secondary.endpoint != 0U) {
            uint8_t secondary_set_protocol[8] = {
                0x21U, USB_HID_SET_PROTOCOL, USB_HID_PROTOCOL_BOOT, 0x00U,
                state->hid_secondary.interface_number, 0x00U, 0x00U, 0x00U,
            };
            if (!xhci_submit_control_transfer_device(
                    state, &state->device, secondary_set_protocol, 0U,
                    false)) {
                xhci_set_error(59U);
                do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
            }
        }
        if (!xhci_configure_hid_endpoint(state)) {
            if (xhci_last_error() == 0U) xhci_set_error(59U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
    } else if ((state->msc_uas_present &&
                state->msc_uas_command_endpoint != 0U &&
                state->msc_uas_status_endpoint != 0U &&
                state->msc_uas_data_in_endpoint != 0U &&
                state->msc_uas_data_out_endpoint != 0U) ||
               (state->msc_bulk_in_endpoint != 0U &&
                state->msc_bulk_out_endpoint != 0U)) {
        if (!xhci_configure_msc_endpoints(state)) {
            if (xhci_last_error() == 0U) xhci_set_error(165U);
            do {
                xhci_unpublish_slot_device(slot, parent_slot, parent_port);
                return false;
            } while (0);
        }
    } else if (state->audio_endpoint != 0U) {
        if (!xhci_configure_audio_endpoint(state)) {
            if (xhci_last_error() == 0U) xhci_set_error(63U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
    } else if (state->bt_event_endpoint != 0U) {
        /* Bluetooth 璁惧宸茬粡鍦ㄤ笂闈㈢殑澶氱鐐归厤缃矾寰勪腑瀹屾垚鍒濆鍖栥€?*/
    } else if (state->hub_configured) {
        /* Hub 绗竴闃舵娌℃湁渚涘唴鏍告暟鎹潰浣跨敤鐨勬櫘閫氭暟鎹鐐广€?*/
    } else {
        if (xhci_last_error() == 0U) xhci_set_error(59U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }


return true;
}

#undef xhci_unpublish_slot_device

bool xhci_enumerate_device(xhci_state_t *state, uint8_t slot,
                           uint8_t port, uint8_t speed,
                           uint8_t root_port, uint8_t parent_slot,
                           uint8_t parent_port, uint32_t route_string) {
    return xhci_enumerate_device_mode(state, slot, port, speed, root_port,
                                      parent_slot, parent_port, route_string,
                                      true);
}

bool xhci_enumerate_hid_device(xhci_state_t *state, uint8_t slot,
                               uint8_t port, uint8_t speed,
                               uint8_t root_port) {
    return xhci_enumerate_device_mode(state, slot, port, speed, root_port,
                                      0U, 0U, 0U, false);
}
