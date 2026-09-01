#include <usb/device.h>
#include <usb/hub.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_DEVICE_LIFECYCLE_OWNER: device-context DMA lifetime,
 * Slot publication and controller teardown use one private Owner. */

#define XHCI_STOP_ENDPOINT_TYPE 15U
#define XHCI_DISABLE_SLOT_TYPE 10U

static bool xhci_stop_pending_endpoints_device(
    xhci_state_t *state,
    xhci_device_context_t *device);

void xhci_zero_device_context(xhci_device_context_t *context) {
    xhci_device_context_clear(context, sizeof(*context));
}


/*
 * V3.10.6B8B
 *
 * xhci_copy_device_context() removed.
 * No published device context is copied between ownership stores.
 */


void xhci_clear_device_flags(void) {
    xhci_clear_topology();
}

/*
 * V3.10.6B9C-FIX1
 *
 * xhci_set_device_kind() removed.
 * Published runtime state is recomputed from Slot Table.
 */


/*
 * V3.10.6B6C EXPLICIT DEVICE RESOURCE TEARDOWN
 *
 * DMA/resource lifetime belongs to an explicit device context.
 */

static bool xhci_free_device_resources_device(
    xhci_device_context_t *device) {
    bool released = true;
    if (device == 0) return false;
    if (device->audio_stream != 0) {
        /* 鐑嫈鍑哄厛璁╃粺涓€闊抽瀵硅薄杩涘叆 DISCONNECTED锛屽啀閲婃斁鍏剁鐐?DMA銆?*/
        audio_stream_t *stream = device->audio_stream;
        device->audio_stream = 0;
        device->audio_stream_running = false;
        device->audio_stream_queued = false;
        device->audio_stream_bound = false;
        (void)audio_stream_disconnect(stream);
    }
    if (device->bt_controller != 0) {
        (void)bt_controller_disconnect(device->bt_controller);
        bt_controller_destroy(device->bt_controller);
        device->bt_controller = 0;
    }
    if (device->msc_configured && device->device_slot != 0U) {
        xhci_msc_transport_release(device->device_slot);
        device->msc_interface = 0U;
        device->msc_bulk_in_endpoint = 0U;
        device->msc_bulk_out_endpoint = 0U;
        device->msc_bulk_in_max_burst = 0U;
        device->msc_bulk_out_max_burst = 0U;
        device->msc_bulk_in_max_packet = 0U;
        device->msc_bulk_out_max_packet = 0U;
        device->msc_configured = false;
    }
    device->msc_uas_present = false;
    device->msc_uas_interface = 0U;
    device->msc_uas_alternate = 0U;
    device->msc_uas_command_endpoint = 0U;
    device->msc_uas_status_endpoint = 0U;
    device->msc_uas_data_in_endpoint = 0U;
    device->msc_uas_data_out_endpoint = 0U;
    device->msc_uas_command_address = 0U;
    device->msc_uas_status_address = 0U;
    device->msc_uas_data_in_address = 0U;
    device->msc_uas_data_out_address = 0U;
    device->msc_uas_command_max_burst = 0U;
    device->msc_uas_status_max_burst = 0U;
    device->msc_uas_data_in_max_burst = 0U;
    device->msc_uas_data_out_max_burst = 0U;
    device->msc_uas_status_max_streams = 0U;
    device->msc_uas_data_in_max_streams = 0U;
    device->msc_uas_data_out_max_streams = 0U;
    device->msc_uas_command_max_packet = 0U;
    device->msc_uas_status_max_packet = 0U;
    device->msc_uas_data_in_max_packet = 0U;
    device->msc_uas_data_out_max_packet = 0U;

    if(!xhci_free_page(
           &device->descriptor_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hid_report))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hid_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hid_secondary.report))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hid_secondary.ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->audio_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->audio_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_acl_out_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_acl_out_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_acl_in_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_acl_in_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_event_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_event_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hub_report))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hub_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->ep0_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->input_context))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->output_context))
    {
        released =
            false;
    }
    if (!released) return false;
    device->device_slot = 0;
    device->device_port = 0;
    device->device_speed = 0;
    device->root_port = 0U;
    device->parent_slot = 0U;
    device->parent_port = 0U;
    device->route_string = 0U;
    device->ep0_enqueue = 0;
    device->configuration_value = 0;
    device->hid_interface = 0U;
    device->hid_protocol = 0U;
    device->hid_endpoint = 0;
    device->hid_max_packet = 0;
    device->hid_interval = 0;
    device->hid_enqueue = 0;
    device->hid_cycle = 1U;
    device->hid_transfer_pending = false;
    device->hid_previous_modifier = 0U;
    for (uint32_t i = 0; i < 6U; ++i) device->hid_previous_keys[i] = 0U;
    device->hid_previous_buttons = 0U;
    device->hid_secondary.interface_number = 0U;
    device->hid_secondary.protocol = 0U;
    device->hid_secondary.endpoint = 0U;
    device->hid_secondary.max_packet = 0U;
    device->hid_secondary.interval = 0U;
    device->hid_secondary.enqueue = 0U;
    device->hid_secondary.cycle = 1U;
    device->hid_secondary.transfer_pending = false;
    device->hid_secondary.previous_modifier = 0U;
    for (uint32_t i = 0; i < 6U; ++i) {
        device->hid_secondary.previous_keys[i] = 0U;
    }
    device->hid_secondary.previous_buttons = 0U;
    device->audio_interface = 0U;
    device->audio_alt_setting = 0U;
    device->audio_endpoint = 0U;
    device->audio_interval = 0U;
    device->audio_max_packet = 0U;
    device->audio_channels = 0U;
    device->audio_bit_resolution = 0U;
    device->audio_sample_rate = 0U;
    device->audio_endpoint_in = false;
    device->audio_enqueue = 0U;
    device->audio_cycle = 1U;
    device->audio_transfer_pending = false;
    device->audio_completed = 0U;
    device->audio_stream = 0;
    device->audio_period = 0U;
    device->audio_frames = 0U;
    device->audio_stream_running = false;
    device->audio_stream_queued = false;
    device->audio_stream_bound = false;
    device->hub_interface = 0U;
    device->hub_interface_present = false;
    device->hub_port_count = 0U;
    device->hub_protocol = 0U;
    device->hub_tt_think_time = 0U;
    device->hub_configured = false;
    device->hub_endpoint = 0U;
    device->hub_max_packet = 0U;
    device->hub_interval = 0U;
    device->hub_enqueue = 0U;
    device->hub_cycle = 1U;
    device->hub_transfer_pending = false;
    device->hub_change_bitmap = 0U;
    device->bt_event_endpoint = 0U;
    device->bt_acl_in_endpoint = 0U;
    device->bt_acl_out_endpoint = 0U;
    device->bt_event_interval = 0U;
    device->bt_event_max_packet = 0U;
    device->bt_acl_in_max_packet = 0U;
    device->bt_acl_out_max_packet = 0U;
    device->bt_event_enqueue = 0U;
    device->bt_event_cycle = 1U;
    device->bt_event_transfer_pending = false;
    device->bt_acl_in_enqueue = 0U;
    device->bt_acl_in_cycle = 1U;
    device->bt_acl_in_transfer_pending = false;
    device->bt_acl_out_enqueue = 0U;
    device->bt_acl_out_cycle = 1U;
    device->bt_acl_out_transfer_pending = false;
    return true;
}

/*
 * Compatibility wrapper for unpublished/new-device enumeration.
 */
bool xhci_free_device_resources(
    xhci_state_t *state)
{
    if(state == 0)
    {
        return false;
    }

    return
        xhci_free_device_resources_device(
            &state->device);
}

/* The auxiliary HID controller has a private working context but no entry in
 * the primary controller's Slot Table or USB Core.  Tear it down without
 * touching the primary controller's published devices. */
bool xhci_free_hid_device_resources(xhci_state_t *state) {
    xhci_device_context_t *device;
    bool released = true;

    if (state == 0) return false;
    device = &state->device;
    if (device->device_slot == 0U) return true;

    if (!xhci_stop_pending_endpoints_device(state, device)) {
        released = false;
    }
    if (!xhci_submit_command(state, XHCI_DISABLE_SLOT_TYPE,
                             device->device_slot, 0U, 0)) {
        released = false;
    }
    if (!xhci_free_device_resources_device(device)) {
        released = false;
    }
    return released;
}


/*
 * 鍦ㄩ噴鏀剧鐐圭幆涔嬪墠鍏堝仠姝㈡墍鏈変粛鏈?TRB 鍦ㄩ鐨勭鐐广€侱isable Slot 浼氳
 * 璁惧澶辨晥锛屼絾鏄惧紡 Stop Endpoint 鑳芥妸鈥滄帶鍒跺櫒浠嶅彲鑳芥秷璐?DMA 鐜€濈殑
 * 鐢熷懡鍛ㄦ湡杈圭晫鍓嶇Щ锛涘嵆浣胯澶囧凡绐佺劧鎷斿嚭锛屽懡浠ゅけ璐ヤ篃鍙奖鍝嶈瘖鏂紝
 * 鍚庣画浠嶄細鎵ц鏈€灏忓寲鐨勮祫婧愬洖鏀躲€? */
/*
 * V3.10.6B6C EXPLICIT ENDPOINT STOP
 */
static bool xhci_stop_pending_endpoints_device(
    xhci_state_t *state,
    xhci_device_context_t *device) {
    uint8_t endpoints[8];
    uint32_t count = 0U;
    bool stopped = true;
    if (state == 0 ||
        device == 0 ||
        device->device_slot == 0U) return true;
    if (device->hid_transfer_pending && device->hid_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->hid_endpoint * 2U + 1U);
    }
    if (device->hid_secondary.transfer_pending &&
        device->hid_secondary.endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->hid_secondary.endpoint * 2U + 1U);
    }
    if (device->audio_transfer_pending && device->audio_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->audio_endpoint * 2U +
                                       (device->audio_endpoint_in ? 1U : 0U));
    }
    if (device->hub_transfer_pending && device->hub_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->hub_endpoint * 2U + 1U);
    }
    if (device->bt_event_transfer_pending && device->bt_event_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->bt_event_endpoint * 2U + 1U);
    }
    if (device->bt_acl_in_transfer_pending && device->bt_acl_in_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->bt_acl_in_endpoint * 2U + 1U);
    }
    if (device->bt_acl_out_transfer_pending && device->bt_acl_out_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->bt_acl_out_endpoint * 2U);
    }
    for (uint32_t i = 0U; i < count; ++i) {
        bool duplicate = false;
        for (uint32_t previous = 0U; previous < i; ++previous) {
            if (endpoints[previous] == endpoints[i]) duplicate = true;
        }
        if (duplicate || endpoints[i] > 31U) continue;
        if (!xhci_submit_command_ex(state, XHCI_STOP_ENDPOINT_TYPE,
                                    device->device_slot, endpoints[i], 0U, 0)) {
            stopped = false;
        }
    }
    return stopped;
}

/*
 * Compatibility wrapper for the unpublished working context.
 */
/*
 * V3.10.6B9E-FIX1 STOP ENDPOINT WRAPPER REMOVED
 *
 * Endpoint teardown always receives an explicit
 * xhci_device_context_t *.
 */



/* 璁惧鍙互鍦?active context 鍜?inventory 涔嬮棿绉诲姩锛屾嫇鎵戞爣璁板繀椤婚殢涔嬮噸绠椼€?*/
/* V3.10.6B7D-FIX1 removed xhci_take_active_device(): runtime devices never activate state->device. */



/* V3.10.6B7D-FIX1 removed xhci_restore_active_device(): runtime devices never activate state->device. */



/*
 * V3.6.6 SLOT UNPUBLISH
 *
 * Hardware teardown is still owned by xHCI.
 *
 * Once Disable Slot has been issued, remove the same device from:
 *
 *   xHCI Slot Table
 *   parent Hub child map
 *   USB Hub Core
 *   USB Device Core
 */
void xhci_unpublish_slot_device(
    uint8_t slot,
    uint8_t parent_slot,
    uint8_t parent_port)
{
    if(slot == 0U)
    {
        return;
    }

    /*
     * Detach from parent first.
     */
    if(parent_slot != 0U &&
       parent_port != 0U)
    {
        usb_hub_port_disconnect(
            parent_slot,
            parent_port,
            slot);

        xhci_topology_detach_slot(
            slot,
            parent_slot,
            parent_port);
    }

    /*
     * Safe for non-Hub devices too:
     * usb_hub_remove() simply clears the corresponding class slot.
     */
    usb_hub_remove(
        slot);

    usb_release_device(
        slot);

        /*
     * bt_controller_disconnect/destroy runs before unpublish.
     * The callback binding can now be invalidated safely.
     */
    xhci_bt_transport_release(slot);


    xhci_topology_clear_slot(slot);
}


static bool xhci_release_device_context(
    xhci_state_t *state,
    xhci_device_context_t *device);

/*
 * V3.10.6B9D UNIFIED EXPLICIT TEARDOWN
 *
 * state->device is unpublished enumeration working storage.
 *
 * Its resource lifetime uses the same explicit device-context
 * teardown core as a published Slot.context.
 */
bool xhci_release_working_device(
    xhci_state_t *state)
{
    if(state == 0 ||
       state->device.device_slot == 0U)
    {
        return true;
    }


    return
        xhci_release_device_context(
            state,
            &state->device);
}

/*
 * V3.10.6B6C EXPLICIT DEVICE TEARDOWN
 *
 * Destroy one device context without installing it in state->device.
 *
 * The caller owns current-device kind/flag bookkeeping.
 */
static bool xhci_release_device_context(
    xhci_state_t *state,
    xhci_device_context_t *device)
{
    if(state == 0 ||
       device == 0 ||
       device->device_slot == 0U)
    {
        return true;
    }

    /*
     * Resource teardown clears identity fields, therefore preserve
     * software topology identity first.
     */
    uint8_t released_slot =
        device->device_slot;

    uint8_t released_parent_slot =
        device->parent_slot;

    uint8_t released_parent_port =
        device->parent_port;

    if(!xhci_stop_pending_endpoints_device(
           state,
           device))
    {
        xhci_set_error(60U);
    }

    if(!xhci_submit_command(
           state,
           XHCI_DISABLE_SLOT_TYPE,
           device->device_slot,
           0U,
           0))
    {
        /*
         * A disconnected device may no longer produce a Disable
         * Slot completion, but its software/DMA ownership still
         * has to be released.
         */
        xhci_set_error(61U);
    }

    bool released =
        xhci_free_device_resources_device(
            device);

    /*
     * Hardware Slot lifetime has ended.
     */
    xhci_unpublish_slot_device(
        released_slot,
        released_parent_slot,
        released_parent_port);

    xhci_recompute_topology(
        state);

    return released;
}


/*
 * V3.9.2 DIRECT SLOT RELEASE
 *
 * Hardware/device lifetime is now terminated directly by Slot ID.
 *
 * No:
 *
 *     inventory scan
 *     inventory index lookup
 *     inventory context
 *
 * is needed to stop endpoints, Disable Slot, release DMA or
 * unpublish topology.
 */
/*
 * V3.10.6B6C CANONICAL SLOT TEARDOWN
 *
 * Published Slot.context is destroyed in place.
 * state->device remains untouched unless it is itself the target.
 */
/*
 * V3.10.6B9C PURE PUBLISHED SLOT RELEASE
 *
 * This function owns one published hardware Slot only:
 *
 *     Slot ID
 *        |
 *        v
 *   Slot.context
 *        |
 *        v
 * xhci_release_device_context()
 *
 * state->device is a separate unpublished enumeration lifetime and
 * is intentionally invisible here.
 */
bool xhci_release_slot_device(
    xhci_state_t *state,
    uint8_t slot)
{
    if(state == 0 ||
       slot == 0U)
    {
        return false;
    }


    xhci_slot_device_t *slot_dev =
        xhci_topology_slot(slot);


    if(!slot_dev->used ||
       slot_dev->context.device_slot !=
           slot)
    {
        return false;
    }


    return
        xhci_release_device_context(
            state,
            &slot_dev->context);
}


/*
 * inventory[] compatibility wrapper.
 *
 * Initial enumeration still uses inventory metadata, but hardware
 * teardown itself is now purely Slot-based.
 */

/*
 * V3.10.6B8C
 *
 * xhci_release_inventory_entry() removed.
 * Teardown is Slot-ID based.
 */




/*
 * V3.9.6 SLOT ONLY SAVE
 *
 * Slot Table is the permanent device store.
 *

*/

/*
 * V3.10.6B8C
 *
 * xhci_save_active_device() removed.
 * Publication is Slot.context-only.
 */




/*
 * V3.10.6B2 CANONICAL SELF TEST CONTEXT
 *
 * xhci_load_inventory_device() removed.
 *
 * Hardware self-test resolves initialization inventory directly
 * to canonical Slot.context and never installs published device
 * state into state->device.
 */



/*
 * V3.9.5 SLOT TABLE CONTROLLER TEARDOWN
 *
 * Controller destruction is driven exclusively by hardware Slot
 * identity.
 *
 * inventory[] is not inspected.
 */
bool xhci_free_slot_resources(
    xhci_state_t *state)
{
    bool released =
        true;


    if(state == 0)
    {
        return false;
    }


    /*
     * First handle the current working context.
     *
     * A fully enumerated active device should also exist in the
     * Slot Table.  A partially constructed enumeration context may
     * not, so retain a raw-resource fallback for that case.
     */
    if(state->device.device_slot != 0U)
    {
        /*
         * V3.10.6B9B WORKING CONTEXT TEARDOWN
         *
         * Slot.used may already be true during enumeration before
         * Slot.context has been published.
         *
         * Therefore state->device always owns its own teardown path.
         */
        if(!xhci_release_working_device(
               state))
        {
            released =
                false;
        }
    }


    uint32_t limit =
        state->max_slots;


    if(limit >=
       XHCI_MAX_SLOT_TABLE)
    {
        limit =
            XHCI_MAX_SLOT_TABLE -
            1U;
    }


    /*
     * Every remaining published device is parked in Slot.context.
     *
     * xhci_release_slot_device() performs:
     *
     *     Stop Endpoint
     *        鈫?     *     Disable Slot
     *        鈫?     *     release DMA
     *        鈫?     *     unlink parent
     *        鈫?     *     remove USB Core / Hub Core object
     *        鈫?     *     clear Slot Table entry
     */
    for(uint32_t slot = 1U;
        slot <= limit;
        ++slot)
    {
        xhci_slot_device_t *slot_dev =
            xhci_topology_slot(slot);


        if(!slot_dev->used)
        {
            continue;
        }


        /*
         * Ignore an uncommitted/stale tentative node here.
         *
         * If it somehow still owns the active working context it
         * was handled above.  A published parked Slot must have a
         * matching canonical context.
         */
        if(slot_dev->context.device_slot !=
           (uint8_t)slot)
        {
            xhci_topology_clear_slot((uint8_t)slot);

            released =
                false;

            continue;
        }


        if(!xhci_release_slot_device(
                state,
                (uint8_t)slot))
        {
            released =
                false;
        }
    }


    /*
     * Nothing may remain installed after controller teardown.
     */
    xhci_zero_device_context(
        &state->device);

    xhci_clear_device_flags();


    return released;
}


    /* runtime 鍖哄墠 0x20 瀛楄妭鏄?MFINDEX锛岀 0 涓?interrupter 浠?0x20 寮€濮嬨€?*/
        /* 鍒濆浜嬩欢闃熷垪灏氭湭琚帶鍒跺櫒澶勭悊锛孍RDP 涓嶅簲鎼哄甫 EHB銆?*/
