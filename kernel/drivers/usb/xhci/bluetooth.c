#include <arch/x86_64/paging.h>
#include <kernel/xhci.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_BLUETOOTH_OWNER: USB Bluetooth transport and events. */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_CONFIGURE_ENDPOINT_TYPE 12U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_SLOT_SHIFT 24U
#define XHCI_TRB_ENDPOINT_SHIFT 16U
#define XHCI_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)
#define XHCI_TRANSFER_EVENT_TYPE 32U
#define XHCI_COMPLETION_SHIFT 24U
#define XHCI_COMPLETION_SUCCESS 1U

typedef struct xhci_bt_transport {
    xhci_state_t *state;
    uint8_t slot;
} xhci_bt_transport_t;


static xhci_bt_transport_t
g_xhci_bt_transports[
    XHCI_MAX_SLOT_TABLE];
void xhci_bt_transport_release(uint8_t slot) {
    if (slot == 0U) return;
    __builtin_memset(&g_xhci_bt_transports[slot],
                     0,
                     sizeof(g_xhci_bt_transports[slot]));
}

static bool xhci_queue_bt_event_device(
    xhci_state_t *state,
    xhci_device_context_t *device) {
    if(device == 0 ||
       device->device_slot == 0U)
    {
        return false;
    }


    xhci_trb_t *ring;
    uint32_t endpoint_id;
    uint32_t index;
    if (state == 0 || device->bt_controller == 0 || device->bt_event_endpoint == 0U ||
        device->bt_event_ring.cpu == 0 || device->bt_event_buffer.cpu == 0 ||
        device->bt_event_transfer_pending) return false;
    endpoint_id = (uint32_t)device->bt_event_endpoint * 2U + 1U;
    index = device->bt_event_enqueue;
    if (endpoint_id > 31U || index >= XHCI_RING_TRB_COUNT - 1U ||
        device->bt_event_max_packet == 0U) return false;
    ring = (xhci_trb_t *)device->bt_event_ring.cpu;
    if (!xhci_transfer_encode_normal(
            &ring[index], xhci_dma_address(&device->bt_event_buffer.mapping),
            device->bt_event_max_packet,
            XHCI_TRB_INTERRUPT_ON_COMPLETION,
            device->bt_event_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->bt_event_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->bt_event_enqueue = 0U;
        device->bt_event_cycle ^= 1U;
    } else {
        device->bt_event_enqueue = index;
    }
    device->bt_event_transfer_pending = true;
    dma_sync_for_device(&device->bt_event_ring.mapping);
    dma_sync_for_device(&device->bt_event_buffer.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

/*
 * Compatibility wrapper.
 *
 * V3.10.5A does not migrate callers yet.
 */
/*
 * V3.10.6B9F BT WORKING WRAPPERS REMOVED
 *
 * Bluetooth enumeration now names state->device explicitly.
 * Runtime Bluetooth paths continue to resolve permanent Slot.context
 * from their Slot ID.
 */


static bool xhci_queue_bt_acl_in_device(
    xhci_state_t *state,
    xhci_device_context_t *device) {
    if(device == 0 ||
       device->device_slot == 0U)
    {
        return false;
    }


    xhci_trb_t *ring;
    uint32_t endpoint_id;
    uint32_t index;
    if (state == 0 || device->bt_controller == 0 || device->bt_acl_in_endpoint == 0U ||
        device->bt_acl_in_ring.cpu == 0 || device->bt_acl_in_buffer.cpu == 0 ||
        device->bt_acl_in_transfer_pending) return false;
    endpoint_id = (uint32_t)device->bt_acl_in_endpoint * 2U + 1U;
    index = device->bt_acl_in_enqueue;
    if (endpoint_id > 31U || index >= XHCI_RING_TRB_COUNT - 1U) return false;
    ring = (xhci_trb_t *)device->bt_acl_in_ring.cpu;
    if (!xhci_transfer_encode_normal(
            &ring[index], xhci_dma_address(&device->bt_acl_in_buffer.mapping),
            PAGE_SIZE, XHCI_TRB_INTERRUPT_ON_COMPLETION,
            device->bt_acl_in_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->bt_acl_in_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->bt_acl_in_enqueue = 0U;
        device->bt_acl_in_cycle ^= 1U;
    } else {
        device->bt_acl_in_enqueue = index;
    }
    device->bt_acl_in_transfer_pending = true;
    dma_sync_for_device(&device->bt_acl_in_ring.mapping);
    dma_sync_for_device(&device->bt_acl_in_buffer.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}



static bool xhci_queue_bt_acl_out_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    size_t length) {
    if(device == 0 ||
       device->device_slot == 0U)
    {
        return false;
    }


    xhci_trb_t *ring;
    uint32_t endpoint_id;
    uint32_t index;
    if (state == 0 || device->bt_acl_out_endpoint == 0U ||
        device->bt_acl_out_ring.cpu == 0 || device->bt_acl_out_buffer.cpu == 0 ||
        device->bt_acl_out_buffer.mapping.device == 0 || length == 0U ||
        length > PAGE_SIZE || device->bt_acl_out_buffer.mapping.segment_count == 0U) {
        return false;
    }
    endpoint_id = (uint32_t)device->bt_acl_out_endpoint * 2U;
    index = device->bt_acl_out_enqueue;
    if (endpoint_id > 31U || index >= XHCI_RING_TRB_COUNT - 1U) return false;
    ring = (xhci_trb_t *)device->bt_acl_out_ring.cpu;
    if (!xhci_transfer_encode_normal(
            &ring[index], xhci_dma_address(&device->bt_acl_out_buffer.mapping),
            (uint32_t)length, XHCI_TRB_INTERRUPT_ON_COMPLETION,
            device->bt_acl_out_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->bt_acl_out_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->bt_acl_out_enqueue = 0U;
        device->bt_acl_out_cycle ^= 1U;
    } else {
        device->bt_acl_out_enqueue = index;
    }
    device->bt_acl_out_transfer_pending = true;
    dma_sync_for_device(&device->bt_acl_out_ring.mapping);
    dma_sync_for_device(&device->bt_acl_out_buffer.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}


/*
 * V3.10.6B7B BT ACL-OUT WRAPPER REMOVED
 */


/*
 * Send using the context currently installed in state->device.
 *
 * The public Bluetooth callback below resolves Slot ID first and
 * temporarily installs the owning context when necessary.
 */
/*
 * V3.10.6B7B EXPLICIT BT SEND CORE
 *
 * All Bluetooth TX state belongs to the supplied device context.
 */
static kstatus_t xhci_bt_send_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    const uint8_t *packet,
    size_t length)
{
    uint8_t setup[8] = {
        0x20U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U
    };

    if(state == 0 ||
       device == 0 ||
       device->device_slot == 0U ||
       device->bt_controller == 0 ||
       packet == 0 ||
       length < 4U ||
       length - 1U > PAGE_SIZE)
    {
        return K_EINVAL;
    }

    /*
     * HCI ACL packet -> USB Bulk OUT.
     */
    if(packet[0] == 0x02U)
    {
        if(device->bt_acl_out_endpoint == 0U ||
           device->bt_acl_out_transfer_pending ||
           device->bt_acl_out_buffer.cpu == 0 ||
           length - 1U > PAGE_SIZE)
        {
            return K_EBUSY;
        }

        for(size_t i = 1U;
            i < length;
            ++i)
        {
            ((uint8_t *)
                device->
                    bt_acl_out_buffer.cpu)
                [i - 1U] =
                    packet[i];
        }

        return
            xhci_queue_bt_acl_out_device(
                state,
                device,
                length - 1U)
            ? K_OK
            : K_EIO;
    }

    /*
     * HCI Command packet -> USB class control transfer.
     */
    if(packet[0] != 0x01U ||
       device->descriptor_buffer.cpu == 0)
    {
        return K_EINVAL;
    }

    for(size_t i = 1U;
        i < length;
        ++i)
    {
        ((uint8_t *)
            device->
                descriptor_buffer.cpu)
            [i - 1U] =
                packet[i];
    }

    setup[6] =
        (uint8_t)(
            (length - 1U) &
            0xFFU);

    setup[7] =
        (uint8_t)(
            (length - 1U) >>
            8);

    dma_sync_for_device(
        &device->
            descriptor_buffer.mapping);

    return
        xhci_submit_control_transfer_device(
            state,
            device,
            setup,
            (uint32_t)(
                length - 1U),
            false)
        ? K_OK
        : K_EIO;
}


/*
 * V3.7.7 BT SLOT TRANSPORT
 *
 * Bluetooth Core -> xHCI transmission is now routed by the Slot ID
 * permanently bound when bt_controller is created.
 */
/*
 * V3.10.6B7B CANONICAL BT SEND
 *
 * Bluetooth transport is permanently bound to one hardware Slot:
 *
 *     transport->slot
 *           |
 *           v
 *      Slot.context
 *           |
 *           v
 *   xhci_bt_send_device()
 *
 * No inventory lookup.
 * No state->device activation.
 */
static kstatus_t xhci_bt_send(
    void *context,
    const uint8_t *packet,
    size_t length)
{
    xhci_bt_transport_t *transport =
        (xhci_bt_transport_t *)context;

    if(transport == 0 ||
       transport->state == 0 ||
       transport->slot == 0U)
    {
        return K_EINVAL;
    }

    xhci_state_t *state =
        transport->state;

    uint8_t slot =
        transport->slot;

    xhci_slot_device_t *slot_dev =
        xhci_topology_slot(slot);

    if(!slot_dev->used ||
       slot_dev->context.device_slot !=
           slot ||
       slot_dev->context.bt_controller ==
           0)
    {
        return K_EIO;
    }

    return
        xhci_bt_send_device(
            state,
            &slot_dev->context,
            packet,
            length);
}



bool xhci_configure_bt_endpoints(xhci_state_t *state) {
    uint32_t event_id;
    uint32_t acl_in_id;
    uint32_t acl_out_id;
    uint32_t *input;
    uint32_t *control;
    uint32_t *input_slot;
    uint32_t *output_slot;
    if (state == 0 || state->bt_event_endpoint == 0U || state->bt_event_max_packet == 0U ||
        (state->bt_acl_in_endpoint == 0U && state->bt_acl_out_endpoint == 0U)) return false;
    event_id = (uint32_t)state->bt_event_endpoint * 2U + 1U;
    acl_in_id = state->bt_acl_in_endpoint == 0U ? 0U :
                (uint32_t)state->bt_acl_in_endpoint * 2U + 1U;
    acl_out_id = state->bt_acl_out_endpoint == 0U ? 0U :
                 (uint32_t)state->bt_acl_out_endpoint * 2U;
    if (event_id > 31U || (acl_in_id != 0U && acl_in_id > 31U) ||
        (acl_out_id != 0U && acl_out_id > 31U) || event_id == acl_in_id ||
        event_id == acl_out_id || (acl_in_id != 0U && acl_in_id == acl_out_id)) return false;
    if (!xhci_alloc_page(state, &state->bt_event_ring, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->bt_event_buffer, DMA_FROM_DEVICE) ||
        (acl_in_id != 0U &&
         (!xhci_alloc_page(state, &state->bt_acl_in_ring, DMA_BIDIRECTIONAL) ||
          !xhci_alloc_page(state, &state->bt_acl_in_buffer, DMA_FROM_DEVICE))) ||
    (acl_out_id != 0U &&
     (!xhci_alloc_page(state, &state->bt_acl_out_ring, DMA_BIDIRECTIONAL) ||
      !xhci_alloc_page(state, &state->bt_acl_out_buffer, DMA_TO_DEVICE)))) return false;
    xhci_ring_init_link(state->bt_event_ring.cpu,
                        xhci_dma_address(&state->bt_event_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    xhci_ring_init_link(state->bt_acl_in_ring.cpu,
                        xhci_dma_address(&state->bt_acl_in_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    xhci_ring_init_link(state->bt_acl_out_ring.cpu,
                        xhci_dma_address(&state->bt_acl_out_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    input = (uint32_t *)state->input_context.cpu;
    control = input;
    input_slot = (uint32_t *)((uint8_t *)input + state->context_size);
    output_slot = (uint32_t *)state->output_context.cpu;
    for (uint32_t i = 0U; i < 4U; ++i) input_slot[i] = output_slot[i];
    uint32_t context_entries = event_id;
    if (acl_in_id > context_entries) context_entries = acl_in_id;
    if (acl_out_id > context_entries) context_entries = acl_out_id;
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= context_entries << 27;
    control[0] = 0U;
    control[1] = 1U | (1U << event_id) |
                 (acl_in_id == 0U ? 0U : 1U << acl_in_id) |
                 (acl_out_id == 0U ? 0U : 1U << acl_out_id);
    xhci_init_endpoint_context(state,
        (uint32_t *)((uint8_t *)input + state->context_size * (event_id + 1U)),
        state->bt_event_interval == 0U ? 1U : state->bt_event_interval, 7U,
        state->bt_event_max_packet, &state->bt_event_ring.mapping);
    if (acl_in_id != 0U) {
        xhci_init_endpoint_context(state,
            (uint32_t *)((uint8_t *)input + state->context_size * (acl_in_id + 1U)),
            0U, 6U, state->bt_acl_in_max_packet, &state->bt_acl_in_ring.mapping);
    }
    if (acl_out_id != 0U) {
        xhci_init_endpoint_context(state,
            (uint32_t *)((uint8_t *)input + state->context_size * (acl_out_id + 1U)),
            0U, 2U, state->bt_acl_out_max_packet, &state->bt_acl_out_ring.mapping);
    }
    /*
     * Bind this Bluetooth controller permanently to its hardware
     * Slot before Bluetooth Core receives the callback pointer.
     */
    if(state->device_slot == 0U)
    {
        return false;
    }


    xhci_bt_transport_t *transport =
        &g_xhci_bt_transports[
            state->device_slot];


    transport->state =
        state;

    transport->slot =
        state->device_slot;


    dma_sync_for_device(
        &state->input_context.mapping);
    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE,
                             state->device_slot,
                             xhci_dma_address(&state->input_context.mapping), 0) ||
        bt_controller_create(
            xhci_bt_send,
            transport,
            &state->bt_controller) != K_OK ||
        bt_controller_start(state->bt_controller) != K_OK ||
        !xhci_queue_bt_event_device(
            state,
            &state->device) ||
        (acl_in_id != 0U && !xhci_queue_bt_acl_in_device(
            state,
            &state->device))) return false;
    return true;
}


/*
 * V3.10.6B7C CANONICAL BT TRANSFER EVENT
 *
 * Transfer Event Slot ID is the permanent Bluetooth identity.
 *
 *     Transfer Event
 *          |
 *        Slot ID
 *          |
 *          v
 *     Slot.context
 *          |
 *     +----+----+
 *     |         |
 * completion  re-arm
 *
 * Published Bluetooth runtime state never enters state->device.
 * inventory[] is not involved in BT event routing.
 */
bool xhci_handle_bt_transfer_event(
    xhci_state_t *state,
    const xhci_trb_t *event)
{
    if(state == 0 ||
       event == 0 ||
       ((event->control >>
         XHCI_TRB_TYPE_SHIFT) &
        0x3FU) !=
           XHCI_TRANSFER_EVENT_TYPE)
    {
        return false;
    }


    uint8_t slot =
        (uint8_t)(
            event->control >>
            XHCI_TRB_SLOT_SHIFT);


    if(slot == 0U)
    {
        return false;
    }


    xhci_slot_device_t *slot_dev =
        xhci_topology_slot(slot);


    /*
     * Only a fully published canonical Bluetooth context owns
     * runtime Transfer Events.
     */
    if(!slot_dev->used ||
       slot_dev->context.device_slot !=
           slot ||
       slot_dev->context.bt_controller ==
           0)
    {
        return false;
    }


    xhci_device_context_t *device =
        &slot_dev->context;


    uint32_t endpoint_id =
        (event->control >>
         XHCI_TRB_ENDPOINT_SHIFT) &
        0x1FU;


    uint32_t completion =
        event->status >>
        XHCI_COMPLETION_SHIFT;


    uint32_t residual =
        event->status &
        0x00FFFFFFU;


    /*
     * HCI Event interrupt-IN.
     */
    if(device->bt_event_endpoint !=
           0U &&
       endpoint_id ==
           (uint32_t)
               device->
                   bt_event_endpoint *
               2U + 1U)
    {
        device->bt_event_transfer_pending =
            false;


        if(completion ==
               XHCI_COMPLETION_SUCCESS &&
           residual <=
               device->
                   bt_event_max_packet)
        {
            size_t length =
                device->
                    bt_event_max_packet -
                residual;


            dma_sync_for_cpu(
                &device->
                    bt_event_buffer.mapping);


            if(length != 0U)
            {
                (void)bt_hci_receive_event(
                    device->bt_controller,
                    device->
                        bt_event_buffer.cpu,
                    length);
            }


            (void)xhci_queue_bt_event_device(
                state,
                device);
        }
        else
        {
            xhci_set_error(70U + completion);
        }


        return true;
    }


    /*
     * ACL bulk-IN.
     */
    if(device->bt_acl_in_endpoint !=
           0U &&
       endpoint_id ==
           (uint32_t)
               device->
                   bt_acl_in_endpoint *
               2U + 1U)
    {
        device->bt_acl_in_transfer_pending =
            false;


        if(completion ==
               XHCI_COMPLETION_SUCCESS &&
           residual <=
               PAGE_SIZE)
        {
            size_t length =
                PAGE_SIZE -
                residual;


            dma_sync_for_cpu(
                &device->
                    bt_acl_in_buffer.mapping);


            if(length != 0U)
            {
                (void)bt_acl_receive(
                    device->bt_controller,
                    device->
                        bt_acl_in_buffer.cpu,
                    length);
            }


            (void)xhci_queue_bt_acl_in_device(
                state,
                device);
        }
        else
        {
            xhci_set_error(71U + completion);
        }


        return true;
    }


    /*
     * ACL bulk-OUT completion.
     */
    if(device->bt_acl_out_endpoint !=
           0U &&
       endpoint_id ==
           (uint32_t)
               device->
                   bt_acl_out_endpoint *
               2U)
    {
        device->bt_acl_out_transfer_pending =
            false;


        if(completion !=
           XHCI_COMPLETION_SUCCESS)
        {
            xhci_set_error(72U + completion);
        }


        return true;
    }


    return false;
}
