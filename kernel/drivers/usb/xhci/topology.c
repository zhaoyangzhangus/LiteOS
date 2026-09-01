#include "internal.h"

#define USB_HID_PROTOCOL_KEYBOARD 0x01U
#define USB_HID_PROTOCOL_MOUSE 0x02U

/* Topology.c owns the Slot Table storage and its derived presence snapshot. */
static xhci_slot_device_t g_xhci_slot_devices[XHCI_MAX_SLOT_TABLE]
    __attribute__((used));
static bool g_xhci_usb_enumerated;
static uint32_t g_xhci_usb_device_count;
static bool g_xhci_usb_hid;
static bool g_xhci_usb_keyboard;
static bool g_xhci_usb_mouse;
static bool g_xhci_usb_hub;
static uint8_t g_xhci_usb_hub_ports;
static bool g_xhci_usb_hub_present;
static uint8_t g_xhci_usb_hub_inventory_ports;
static bool g_xhci_usb_hub_downstream;
static bool g_xhci_usb_audio;
static bool g_xhci_usb_bluetooth;

void xhci_clear_topology(void) {
    g_xhci_usb_enumerated = false;
    g_xhci_usb_device_count = 0U;
    g_xhci_usb_hid = false;
    g_xhci_usb_keyboard = false;
    g_xhci_usb_mouse = false;
    g_xhci_usb_hub = false;
    g_xhci_usb_hub_ports = 0U;
    g_xhci_usb_hub_present = false;
    g_xhci_usb_hub_inventory_ports = 0U;
    g_xhci_usb_hub_downstream = false;
    g_xhci_usb_audio = false;
    g_xhci_usb_bluetooth = false;
}

static bool xhci_topology_slot_valid(uint8_t slot)
{
    /* XHCI_MAX_SLOT_TABLE is 256 and Slot IDs are uint8_t values. */
    return slot != 0U;
}

xhci_slot_device_t *xhci_topology_slot(uint32_t slot)
{
    if(slot == 0U || slot >= XHCI_MAX_SLOT_TABLE)
    {
        return 0;
    }
    return &g_xhci_slot_devices[slot];
}

bool xhci_topology_device_enumerated(void)
{
    return g_xhci_usb_enumerated;
}

uint32_t xhci_topology_device_count(void)
{
    return g_xhci_usb_device_count;
}

bool xhci_topology_hid_configured(void)
{
    return g_xhci_usb_hid;
}

bool xhci_topology_keyboard_configured(void)
{
    return g_xhci_usb_keyboard;
}

bool xhci_topology_mouse_configured(void)
{
    return g_xhci_usb_mouse;
}

bool xhci_topology_audio_configured(void)
{
    return g_xhci_usb_audio;
}

bool xhci_topology_hub_configured(void)
{
    return g_xhci_usb_hub_present;
}

uint8_t xhci_topology_hub_port_count(void)
{
    return g_xhci_usb_hub_inventory_ports;
}

bool xhci_topology_hub_downstream_configured(void)
{
    return g_xhci_usb_hub_downstream;
}

bool xhci_topology_bluetooth_configured(void)
{
    return g_xhci_usb_bluetooth;
}

/*
 * The enumeration Owner supplies immutable topology inputs here.  This is
 * the only entry point that creates a tentative Slot Table node.
 */
bool xhci_topology_begin_slot(
    uint8_t slot,
    uint8_t speed,
    uint8_t parent_slot,
    uint8_t parent_port,
    uint8_t root_port,
    uint32_t route_string,
    uint8_t tt_slot,
    uint8_t tt_port,
    bool tt_multi)
{
    if(!xhci_topology_slot_valid(slot))
    {
        return false;
    }

    xhci_slot_device_t *slot_dev =
        &g_xhci_slot_devices[slot];

    __builtin_memset(
        slot_dev,
        0,
        sizeof(*slot_dev));

    slot_dev->used = true;
    slot_dev->slot_id = slot;
    slot_dev->speed = speed;
    slot_dev->parent_slot = parent_slot;
    slot_dev->parent_port = parent_port;
    slot_dev->root_port = root_port;
    slot_dev->route_string = route_string;
    slot_dev->tt_slot = tt_slot;
    slot_dev->tt_port = tt_port;
    slot_dev->tt_multi = tt_multi;
    return true;
}

/* Publish the cache fields after the working context becomes canonical. */
void xhci_topology_publish_slot(uint8_t slot)
{
    if(!xhci_topology_slot_valid(slot))
    {
        return;
    }

    xhci_slot_device_t *slot_dev =
        &g_xhci_slot_devices[slot];
    if(!slot_dev->used ||
       slot_dev->context.device_slot != slot)
    {
        return;
    }

    slot_dev->slot_id = slot_dev->context.device_slot;
    slot_dev->speed = slot_dev->context.device_speed;
    slot_dev->parent_slot = slot_dev->context.parent_slot;
    slot_dev->parent_port = slot_dev->context.parent_port;
    slot_dev->root_port = slot_dev->context.root_port;
    slot_dev->route_string = slot_dev->context.route_string;
    slot_dev->is_hub = slot_dev->context.hub_interface_present &&
                       slot_dev->context.hub_port_count != 0U;
    slot_dev->hub_port_count = slot_dev->is_hub
                                   ? slot_dev->context.hub_port_count
                                   : 0U;
    slot_dev->hub_protocol = slot_dev->is_hub
                                 ? slot_dev->context.hub_protocol
                                 : 0U;

    if(!xhci_topology_slot_valid(slot_dev->parent_slot) ||
       slot_dev->parent_port == 0U ||
       slot_dev->parent_port > 16U)
    {
        return;
    }

    xhci_slot_device_t *parent =
        &g_xhci_slot_devices[slot_dev->parent_slot];
    if(parent->used &&
       parent->is_hub &&
       parent->context.device_slot == slot_dev->parent_slot)
    {
        parent->child_slots[slot_dev->parent_port - 1U] = slot;
    }
}

/* Remove one published child from the Slot Table acceleration cache. */
void xhci_topology_detach_slot(
    uint8_t slot,
    uint8_t parent_slot,
    uint8_t parent_port)
{
    if(!xhci_topology_slot_valid(slot) ||
       !xhci_topology_slot_valid(parent_slot) ||
       parent_port == 0U ||
       parent_port > 16U)
    {
        return;
    }

    xhci_slot_device_t *parent =
        &g_xhci_slot_devices[parent_slot];
    if(parent->used &&
       parent->child_slots[parent_port - 1U] == slot)
    {
        parent->child_slots[parent_port - 1U] = 0U;
    }
}

/* Clear a tentative or published Slot Table node after hardware teardown. */
void xhci_topology_clear_slot(uint8_t slot)
{
    if(!xhci_topology_slot_valid(slot))
    {
        return;
    }

    __builtin_memset(
        &g_xhci_slot_devices[slot],
        0,
        sizeof(g_xhci_slot_devices[slot]));
}

/*
 * REFACTOR_P8_XHCI_TOPOLOGY_OWNER: Slot Table-derived presence and
 * downstream topology state are recomputed in this dedicated unit.
 */
void xhci_recompute_topology(
    xhci_state_t *state)
{
    bool hub_present =
        false;

    bool downstream =
        false;

    bool hid_present =
        false;

    bool mouse_present =
        false;

    bool keyboard_present =
        false;

    bool audio_present =
        false;

    bool bluetooth_present =
        false;

    uint8_t hub_ports =
        0U;

    uint32_t count =
        0U;


    if(state == 0)
    {
        return;
    }


    uint32_t slot_limit =
        state->max_slots;


    if(slot_limit >=
       XHCI_MAX_SLOT_TABLE)
    {
        slot_limit =
            XHCI_MAX_SLOT_TABLE - 1U;
    }


    for(uint32_t slot = 1U;
        slot <= slot_limit;
        ++slot)
    {
        xhci_slot_device_t *slot_dev =
            &g_xhci_slot_devices[
                slot];


        if(!slot_dev->used)
        {
            continue;
        }


        /*
         * V3.2 creates a tentative Slot Table node before
         * enumeration has completely succeeded.
         *
         * Only a published context whose hardware Slot ID
         * matches the table index counts as a live USB device.
         *
         * This prevents a failed enumeration from becoming a
         * phantom device in global topology state.
         */
        if(slot_dev->context.device_slot !=
           (uint8_t)slot)
        {
            continue;
        }


        ++count;


        xhci_device_context_t *device =
            &slot_dev->context;


        if(device->hid_endpoint != 0U ||
           device->hid_secondary.endpoint != 0U)
        {
            hid_present =
                true;

            if(device->hid_protocol == USB_HID_PROTOCOL_MOUSE ||
               device->hid_secondary.protocol == USB_HID_PROTOCOL_MOUSE)
                mouse_present = true;
            if(device->hid_protocol == USB_HID_PROTOCOL_KEYBOARD ||
               device->hid_secondary.protocol == USB_HID_PROTOCOL_KEYBOARD)
                keyboard_present = true;
        }


        if(device->audio_endpoint != 0U)
        {
            audio_present =
                true;
        }


        if(device->bt_event_endpoint != 0U ||
           device->bt_controller != 0)
        {
            bluetooth_present =
                true;
        }


        if(slot_dev->is_hub)
        {
            hub_present =
                true;


            if(hub_ports == 0U)
            {
                hub_ports =
                    slot_dev->
                        hub_port_count;

                /*
                 * Compatibility fallback while Hub fields are
                 * still being migrated out of xHCI context.
                 */
                if(hub_ports == 0U)
                {
                    hub_ports =
                        device->
                            hub_port_count;
                }
            }
        }


        /*
         * Any non-root device proves that a downstream topology
         * exists, including:
         *
         * Hub -> device
         * Hub -> Hub
         * Hub -> Hub -> device
         */
        if(slot_dev->parent_slot != 0U ||
           device->parent_slot != 0U)
        {
            downstream =
                true;
        }
    }


    g_xhci_usb_device_count =
        count;

    g_xhci_usb_enumerated =
        count != 0U;

    g_xhci_usb_hid =
        hid_present;

    g_xhci_usb_keyboard =
        keyboard_present;

    g_xhci_usb_mouse =
        mouse_present;

    g_xhci_usb_audio =
        audio_present;

    g_xhci_usb_bluetooth =
        bluetooth_present;

    g_xhci_usb_hub_present =
        hub_present;

    g_xhci_usb_hub =
        hub_present;

    g_xhci_usb_hub_ports =
        hub_ports;

    /*
     * Keep the existing public/runtime diagnostic field alive
     * until Hub class migration removes the legacy name.
     */
    g_xhci_usb_hub_inventory_ports =
        hub_ports;

    g_xhci_usb_hub_downstream =
        downstream;
}

/* Device-kind classification is derived from the canonical Slot context. */
uint8_t xhci_context_kind(
    const xhci_device_context_t *context)
{
    if (context == 0) return 0U;
    if (context->hid_endpoint != 0U) return 1U;
    if (context->audio_endpoint != 0U) return 2U;
    if (context->bt_event_endpoint != 0U) return 3U;
    if (context->hub_configured || context->hub_interface_present) return 4U;
    if (context->msc_configured ||
        (context->msc_bulk_in_endpoint != 0U &&
         context->msc_bulk_out_endpoint != 0U)) return 5U;
    return 0U;
}

/*
 * The parent_slot/parent_port relation is canonical.  child_slots[] is only
 * an acceleration cache and is repaired from the Slot Table when stale.
 */
bool xhci_topology_find_child(
    uint8_t parent_slot,
    uint8_t parent_port,
    uint8_t *child_slot)
{
    if(child_slot == 0)
    {
        return false;
    }

    *child_slot = 0U;
    if(parent_slot == 0U ||
       parent_port == 0U ||
       parent_port > 16U)
    {
        return false;
    }

    xhci_slot_device_t *parent =
        &g_xhci_slot_devices[parent_slot];
    if(!parent->used ||
       !parent->is_hub ||
       parent->context.device_slot != parent_slot)
    {
        return false;
    }

    uint8_t slot = parent->child_slots[parent_port - 1U];
    if(slot != 0U)
    {
        xhci_slot_device_t *child =
            &g_xhci_slot_devices[slot];
        if(child->used &&
           child->context.device_slot == slot &&
           child->parent_slot == parent_slot &&
           child->parent_port == parent_port)
        {
            *child_slot = slot;
            return true;
        }

        parent->child_slots[parent_port - 1U] = 0U;
    }

    for(uint32_t candidate = 1U;
        candidate < XHCI_MAX_SLOT_TABLE;
        ++candidate)
    {
        xhci_slot_device_t *child =
            &g_xhci_slot_devices[candidate];
        if(!child->used ||
           child->context.device_slot != (uint8_t)candidate)
        {
            continue;
        }

        if(child->parent_slot == parent_slot &&
           child->parent_port == parent_port)
        {
            parent->child_slots[parent_port - 1U] =
                (uint8_t)candidate;
            *child_slot = (uint8_t)candidate;
            return true;
        }
    }

    return false;
}

/* Build the xHCI route-string nibble for one downstream Hub port. */
bool xhci_topology_child_route(
    const xhci_device_context_t *hub,
    uint8_t port,
    uint32_t *route_out)
{
    if(hub == 0 ||
       route_out == 0 ||
       port == 0U ||
       port > 15U)
    {
        return false;
    }

    uint32_t route = hub->route_string;
    uint32_t shift = route == 0U ? 0U : 4U;
    while(shift < 20U &&
          ((route >> shift) & 0x0FU) != 0U)
    {
        shift += 4U;
    }
    if(shift >= 20U)
    {
        return false;
    }

    *route_out = route | ((uint32_t)port << shift);
    return true;
}
