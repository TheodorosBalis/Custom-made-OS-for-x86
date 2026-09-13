#ifndef USB_HOST_H
#define USB_HOST_H

#include "Include/KernelTypes.h"

#define UHCI_CONTROLLER_COUNT 4u
#define UHCI_VECTOR_BASE 0x40u
/* Fixed RAM locations containing pointers, not function entry addresses. */
#define UHCI_IRQ_CALLBACK 0x80002350u
#define UHCI_STATUS_POINTER 0x80002354u
#define UHCI_DMA_BASE 0x30060000u
#define UHCI_DMA_STRIDE 0x4000u
#define UHCI_ACTIVE (1u << 23)
#define UHCI_IOC (1u << 24)
#define UHCI_ERRORS 0x00760000u
#define UHCI_REPORT_COUNT 16u
#define UHCI_REPORT_BYTES 64u
#define USB_CONFIGURATION_MAX_BYTES 4096u

enum uhci_status {
    UHCI_ABSENT = 0,
    UHCI_READY = 1,
    UHCI_BAD_ROUTE = 2,
    UHCI_BAD_BAR = 3,
    UHCI_CONTROLLER_ERROR = 4,
    UHCI_TRANSFER_ERROR = 5,
    UHCI_DISCONNECTED = 6
};

struct uhci_controller_info {
    uint32_t pci, io_base, gsi, vector;
    uint32_t status, interrupts, last_status, dma;
    uint32_t port_status[2];
    uint32_t reports, dropped;
};

struct usb_host_info {
    uint32_t controller_count, route_mask, error, unclaimed_interrupts;
    struct uhci_controller_info controller[UHCI_CONTROLLER_COUNT];
    uint32_t polls, poll_completions;
    uint32_t last_td[UHCI_CONTROLLER_COUNT];
    uint32_t port_generation[UHCI_CONTROLLER_COUNT][2];
    uint32_t ehci_bar;
};

/* One raw USB packet copied out of the controller's reusable DMA buffer. */
struct uhci_report {
    uint32_t length;
    uint8_t data[UHCI_REPORT_BYTES];
};

struct uhci_debug_snapshot {
    uint32_t command, status, interrupt_enable, frame;
    uint32_t td, qh, control_td, port[2], request, result;
};
void usb_uhci_debug_snapshot(uint32_t controller, struct uhci_debug_snapshot *snapshot);

/* Hardware reads these exact 16-byte layouts from physical DMA memory.
 * TD (transfer descriptor): one packet's next link, status, USB token and buffer.
 * QH (queue head): link to the next queue and its current TD (element).
 * They are not C function pointers: UHCI interprets the fields as bus work. */
struct uhci_td { volatile uint32_t link, status, token, buffer; };
struct uhci_qh { volatile uint32_t link, element, reserved[2]; };

#define USB_REPORT_CONNECTED 1u
#define USB_REPORT_DATA 2u
#define USB_REPORT_DISCONNECTED 3u
typedef void (*usb_report_callback)(uint32_t controller, uint32_t event,
                                    const uint8_t *data, uint32_t length);
uint32_t usb_host_set_report_callback(usb_report_callback callback);

/* Kernel-only host APIs. No device-class code or user call gate here. */
/* Once at boot: prepare controllers/routes. Returns number ready, not devices. */
uint32_t usb_host_initialize(void);
/* Pointer to live diagnostics owned by this driver; caller must not free it. */
const struct usb_host_info *usb_host_get_info(void);
/* Kernel idle context only: recover completed TDs and disconnected streams. */
uint32_t usb_host_poll(void); /* Returns active-controller bit mask. */
uint32_t usb_uhci_hotplug_due(void); /* One scan per 100 ms. */
/* Blocking boot-time delay; requires the host's timing calibration. */
void usb_uhci_delay(uint32_t milliseconds);
/* Stop traffic on root port 0/1 during probing; ignored if a stream is active. */
void usb_uhci_disable_port(uint32_t controller, uint32_t port);
uint32_t usb_uhci_reset_port(uint32_t controller, uint32_t port); /* 0 absent/error, 1 full, 2 low */
/* Blocking endpoint-zero request. controller=host index 0..3, address=USB
 * device address 0..127, low_speed=0/1, packet=EP0 max packet bytes.
 * setup points to 8 request bytes; data holds/supplies the requested payload.
 * Returns data-byte count (possibly 0), or -1. No active periodic stream. */
int32_t usb_uhci_control(uint32_t controller, uint32_t address, uint32_t low_speed,
                       uint32_t packet, const uint8_t setup[8], void *data);
/* Start one periodic IN endpoint on a configured device. endpoint is its
 * number 1..15, NOT a CPU vector; packet/interval come from its descriptor.
 * Returns 1 when armed, 0 on failure. Does not wait for the first report. */
uint32_t usb_uhci_start_interrupt(uint32_t controller, uint32_t port,
                                uint32_t address, uint32_t endpoint, uint32_t low_speed,
                                uint32_t packet, uint32_t interval_ms);
/* Nonblocking CPU-queue read: 1 fills *report; 0 means no report was copied.
 * Does not access a USB port or submit another transfer. */
uint32_t usb_uhci_read_report(uint32_t controller, struct uhci_report *report);

/* Pure packet/routing helpers, also used by host tests. */
uint32_t uhci_resolve_gsi(uint32_t lpc_id, uint32_t controller_id, uint32_t function,
                        uint32_t pci_pin, uint32_t d29ip, uint32_t d29ir);
uint32_t uhci_token(uint32_t pid, uint32_t address, uint32_t endpoint,
                    uint32_t toggle, uint32_t length);
uint32_t uhci_interval(uint32_t milliseconds);
int32_t uhci_completed_length(uint32_t status, uint32_t requested);
uint32_t uhci_port_update(uint32_t previous, uint32_t clear, uint32_t set);

#endif
