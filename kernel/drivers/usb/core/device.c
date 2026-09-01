#include <stdint.h>
#include <stdbool.h>

#include <usb/device.h>


#define USB_MAX_DEVICES 256


static usb_device_t
usb_devices[
    USB_MAX_DEVICES];



usb_device_t *
usb_get_device(
    uint8_t slot)
{
    if(slot == 0)
    {
        return 0;
    }


    if(!usb_devices[slot].used)
    {
        return 0;
    }


    return
        &usb_devices[slot];
}



usb_device_t *
usb_alloc_device(
    uint8_t slot)
{
    if(slot == 0)
    {
        return 0;
    }


    usb_device_t *dev =
        &usb_devices[slot];


    __builtin_memset(
        dev,
        0,
        sizeof(*dev));


    dev->used = true;

    dev->slot_id = slot;


    return dev;
}


void
usb_release_device(
    uint8_t slot)
{
    if(slot == 0U)
    {
        return;
    }

    __builtin_memset(
        &usb_devices[slot],
        0,
        sizeof(usb_devices[slot]));
}
