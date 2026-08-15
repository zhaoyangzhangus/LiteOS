#pragma once

#include <kernel/base.h>

struct audio_stream;
struct device;

/* xHCI 控制器后端：控制器没有出现时允许无 USB 的 QEMU/物理机继续启动。 */
bool xhci_hardware_present(void);
bool xhci_hardware_self_test(void);
bool xhci_usb_device_enumerated(void);
uint32_t xhci_usb_device_count(void);
bool xhci_usb_hid_configured(void);
bool xhci_usb_mouse_configured(void);
bool xhci_usb_audio_configured(void);
bool xhci_usb_hub_configured(void);
uint8_t xhci_usb_hub_port_count(void);
bool xhci_usb_hub_downstream_configured(void);
bool xhci_usb_bluetooth_configured(void);
uint32_t xhci_usb_audio_completed(void);
struct device *xhci_audio_device(void);
kstatus_t xhci_audio_stream_configure(struct audio_stream *stream);
kstatus_t xhci_audio_stream_start(struct audio_stream *stream);
kstatus_t xhci_audio_stream_stop(struct audio_stream *stream);
kstatus_t xhci_audio_stream_queue(struct audio_stream *stream, uint32_t period,
                                  uint32_t frames);
kstatus_t xhci_audio_stream_reset(struct audio_stream *stream);
kstatus_t xhci_audio_stream_disconnect(struct audio_stream *stream);
bool xhci_process_events(uint32_t budget);
bool xhci_schedule_deferred_work(void);
void xhci_deferred_work(void *argument);
uint32_t xhci_last_error(void);
