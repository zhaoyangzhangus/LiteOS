#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_MAX_ENDPOINTS 32U

typedef struct usb_endpoint {
    uint8_t address;
    uint8_t type;
    uint16_t max_packet;
    uint8_t interval;
} usb_endpoint_t;

typedef struct usb_device {
    bool used;
    uint8_t slot_id;
    uint8_t parent_slot;
    uint8_t parent_port;
    uint8_t speed;
    uint8_t device_class;
    uint8_t subclass;
    uint8_t protocol;
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
} usb_device_t;

usb_device_t *usb_get_device(uint8_t slot);
usb_device_t *usb_alloc_device(uint8_t slot);
void usb_release_device(uint8_t slot);
