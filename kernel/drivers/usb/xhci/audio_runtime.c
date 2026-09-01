#include "internal.h"

/* REFACTOR_P8_XHCI_AUDIO_RUNTIME_OWNER: audio DMA/TRB queueing and endpoint
 * doorbell submission remain a direct controller-side Owner. */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_CONFIGURE_ENDPOINT_TYPE 12U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define XHCI_TRB_SIA (1U << 31)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)

bool xhci_configure_audio_endpoint(xhci_state_t *state) {
    uint32_t endpoint_id;
    uint32_t endpoint_type;
    if (state == 0 || state->audio_endpoint == 0U ||
        state->audio_max_packet == 0U || state->audio_max_packet > PAGE_SIZE) return false;
    endpoint_id = (uint32_t)state->audio_endpoint * 2U +
                  (state->audio_endpoint_in ? 1U : 0U);
    endpoint_type = state->audio_endpoint_in ? 5U : 1U;
    if (endpoint_id > 31U ||
        !xhci_alloc_page(state, &state->audio_ring, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->audio_buffer,
                         state->audio_endpoint_in ? DMA_FROM_DEVICE : DMA_TO_DEVICE)) {
        xhci_free_page(&state->audio_ring);
        xhci_free_page(&state->audio_buffer);
        return false;
    }
    state->audio_enqueue = 0U;
    state->audio_cycle = 1U;
    state->audio_transfer_pending = false;
    for (uint32_t i = 0; i < PAGE_SIZE; ++i) {
        ((uint8_t *)state->audio_buffer.cpu)[i] = 0U;
    }

    uint8_t *input = (uint8_t *)state->input_context.cpu;
    uint32_t *control = (uint32_t *)input;
    uint32_t *input_slot = (uint32_t *)(input + state->context_size);
    uint32_t *output_slot = (uint32_t *)state->output_context.cpu;
    uint32_t *endpoint = (uint32_t *)(input + state->context_size * (endpoint_id + 1U));
    for (uint32_t i = 0; i < 4U; ++i) input_slot[i] = output_slot[i];
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= endpoint_id << 27;
    control[0] = 0U;
    control[1] = 1U | (1U << endpoint_id);
    endpoint[0] = ((uint32_t)state->audio_interval << 16);
    endpoint[1] = (3U << 1) | (endpoint_type << 3) |
                  ((uint32_t)state->audio_max_packet << 16);
    endpoint[2] = (uint32_t)xhci_dma_address(&state->audio_ring.mapping) | 1U;
    endpoint[3] = (uint32_t)(xhci_dma_address(&state->audio_ring.mapping) >> 32);
    endpoint[4] = 0U;
    xhci_trb_t *ring = (xhci_trb_t *)state->audio_ring.cpu;
    ring[XHCI_RING_TRB_COUNT - 1U].parameter =
        xhci_dma_address(&state->audio_ring.mapping);
    ring[XHCI_RING_TRB_COUNT - 1U].control =
        (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
    dma_sync_for_device(&state->input_context.mapping);
    dma_sync_for_device(&state->audio_ring.mapping);
    dma_sync_for_device(&state->audio_buffer.mapping);
    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE,
                             state->device_slot,
                             xhci_dma_address(&state->input_context.mapping), 0)) {
        xhci_free_page(&state->audio_ring);
        xhci_free_page(&state->audio_buffer);
        return false;
    }
    /* 绔偣閰嶇疆瀹屾垚鍚庣瓑寰呯粺涓€闊抽鏈嶅姟鎻愪氦绗竴涓懆鏈熴€?*/
    return true;
}


bool xhci_queue_audio_transfer_device(
    xhci_state_t *state,
    xhci_device_context_t *device) {
    if(device == 0 ||
       device->device_slot == 0U)
    {
        return false;
    }


    xhci_trb_t *ring;
    uint32_t index;
    uint32_t endpoint_id;
    uint32_t transfer_bytes;
    if (state == 0 || !state->initialized || device->audio_ring.cpu == 0 ||
        device->audio_buffer.cpu == 0 || device->audio_transfer_pending ||
        device->audio_endpoint == 0U || device->audio_max_packet == 0U) return false;
    if (device->audio_stream != 0) {
        if (!device->audio_stream_queued || device->audio_frames == 0U) return false;
        uint64_t bytes = audio_stream_bytes_for_frames(device->audio_stream,
                                                       device->audio_frames);
        if (bytes == 0U || bytes > PAGE_SIZE) return false;
        transfer_bytes = (uint32_t)bytes;
        if (!device->audio_endpoint_in &&
            audio_stream_period_read(device->audio_stream, device->audio_period,
                                     device->audio_buffer.cpu, bytes) != K_OK) {
            return false;
        }
    } else {
        transfer_bytes = device->audio_max_packet;
    }
    index = device->audio_enqueue;
    if (index >= XHCI_RING_TRB_COUNT - 1U) return false;
    endpoint_id = (uint32_t)device->audio_endpoint * 2U +
                  (device->audio_endpoint_in ? 1U : 0U);
    ring = (xhci_trb_t *)device->audio_ring.cpu;
    if (!xhci_transfer_encode_isoch(
            &ring[index], xhci_dma_address(&device->audio_buffer.mapping),
            transfer_bytes,
            XHCI_TRB_INTERRUPT_ON_COMPLETION | XHCI_TRB_SIA,
            device->audio_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->audio_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->audio_enqueue = 0U;
        device->audio_cycle ^= 1U;
    } else {
        device->audio_enqueue = index;
    }
    device->audio_transfer_pending = true;
    dma_sync_for_device(&device->audio_ring.mapping);
    dma_sync_for_device(&device->audio_buffer.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

/* Transfer Event Slot ID is the permanent Audio device identity.  Published
 * audio state is never routed through the enumeration scratch context. */
bool xhci_handle_audio_transfer_event(
    xhci_state_t *state,
    const xhci_trb_t *event) {
    uint8_t slot;
    xhci_slot_device_t *slot_dev;
    xhci_device_context_t *device;
    uint32_t endpoint_id;
    uint32_t expected_endpoint_id;
    uint32_t completion;

    if (state == 0 || event == 0 ||
        ((event->control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU) !=
            XHCI_TRANSFER_EVENT_TYPE) {
        return false;
    }
    slot = (uint8_t)(event->control >> XHCI_TRB_SLOT_SHIFT);
    if (slot == 0U) return false;

    slot_dev = xhci_topology_slot(slot);
    if (!slot_dev->used || slot_dev->context.device_slot != slot ||
        slot_dev->context.audio_endpoint == 0U) {
        return false;
    }
    device = &slot_dev->context;
    endpoint_id = (event->control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU;
    expected_endpoint_id = (uint32_t)device->audio_endpoint * 2U +
                           (device->audio_endpoint_in ? 1U : 0U);
    if (endpoint_id != expected_endpoint_id) return false;

    completion = event->status >> XHCI_COMPLETION_SHIFT;
    device->audio_transfer_pending = false;
    if (completion == XHCI_COMPLETION_SUCCESS) {
        dma_sync_for_cpu(&device->audio_buffer.mapping);
        ++device->audio_completed;
        if (device->audio_stream != 0) {
            if (device->audio_endpoint_in && device->audio_stream_queued) {
                uint64_t bytes = audio_stream_bytes_for_frames(
                    device->audio_stream, device->audio_frames);
                if (bytes != 0U && bytes <= PAGE_SIZE) {
                    (void)audio_stream_period_write(
                        device->audio_stream, device->audio_period,
                        device->audio_buffer.cpu, bytes);
                }
            }
            device->audio_stream_queued = false;
        } else if (!device->audio_stream_bound) {
            /* Legacy autonomous mode re-arms from the canonical Slot. */
            (void)xhci_queue_audio_transfer_device(state, device);
        }
    } else if (completion != 0U) {
        device->audio_stream_queued = false;
        xhci_set_error(64U + completion);
    }
    return true;
}
