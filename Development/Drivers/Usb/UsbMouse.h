#ifndef USB_MOUSE_H
#define USB_MOUSE_H

#include "UsbHost.h"

/* Full-speed mouse polling override; 0 keeps the descriptor interval. */
#define USB_MOUSE_FULL_SPEED_INTERVAL_MS 1u

enum usb_mouse_status {
    USB_MOUSE_NOT_PROBED = 0,
    USB_MOUSE_ABSENT = 1,
    USB_MOUSE_NOT_SUPPORTED = 2,
    USB_MOUSE_BAD_DESCRIPTOR = 3,
    USB_MOUSE_CONTROL_FAILED = 4,
    USB_MOUSE_START_FAILED = 5,
    USB_MOUSE_STREAMING = 6
};

struct usb_mouse_interface {
    /* Selected descriptor values, not CPU addresses/selectors. packet is
     * endpoint max bytes per report transfer; interval is advertised ms. */
    uint32_t configuration, interface_number, endpoint, packet, interval;
};

struct usb_mouse_port {
    uint32_t status, address, low_speed, vendor, product;
    struct usb_mouse_interface interface;
    uint32_t scheduled_interval;
};

struct usb_mouse_info {
    /* Indexed by UHCI controller then root port, not by USB device address. */
    uint32_t mouse_count;
    struct usb_mouse_port port[UHCI_CONTROLLER_COUNT][2];
    uint32_t boot_mouse_count;
};

/* Kernel-owned diagnostics. Streaming means armed, not necessarily a report yet. */
extern struct usb_mouse_info usb_mouse_state;
/* Boot-only discovery through the host APIs. Returns configured mouse count;
 * received packets remain in the host queues until a consumer reads them. */
uint32_t usb_mouse_initialize(void);
void usb_mouse_service(void); /* Kernel idle task, never an interrupt handler. */
uint32_t usb_mouse_poll_interval(uint32_t low_speed, uint32_t advertised_ms);

/* 1: boot mouse found; 0: unsupported; -1: malformed descriptor stream. */
int32_t usb_mouse_find_interface(const uint8_t *data, uint32_t length,
                               uint32_t low_speed, struct usb_mouse_interface *result);
/* Initialization-only: caller must isolate address zero on this controller.
 * scratch must hold USB_CONFIGURATION_MAX_BYTES. Failures disable this port. */
uint32_t usb_mouse_probe_port(uint32_t controller, uint32_t port,
                             uint8_t *scratch, struct usb_mouse_port *info);

#endif
