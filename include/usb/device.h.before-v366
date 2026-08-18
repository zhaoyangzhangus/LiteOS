#ifndef LITEOS_USB_DEVICE_H
#define LITEOS_USB_DEVICE_H

#include <stdint.h>
#include <stdbool.h>


#define USB_MAX_ENDPOINTS 32


typedef struct usb_endpoint
{
    uint8_t address;

    uint8_t type;

    uint16_t max_packet;

    uint8_t interval;

} usb_endpoint_t;



typedef struct usb_device
{
    bool used;


    /*
     * xHCI Slot binding
     */
    uint8_t slot_id;


    /*
     * topology
     */
    uint8_t parent_slot;

    uint8_t parent_port;


    /*
     * USB identity
     */
    uint8_t speed;

    uint8_t device_class;

    uint8_t subclass;

    uint8_t protocol;


    /*
     * endpoints
     */
    usb_endpoint_t endpoints[
        USB_MAX_ENDPOINTS];


} usb_device_t;



usb_device_t *
usb_get_device(
    uint8_t slot);


usb_device_t *
usb_alloc_device(
    uint8_t slot);



#endif
