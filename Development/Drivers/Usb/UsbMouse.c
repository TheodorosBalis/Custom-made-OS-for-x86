#include "UsbMouse.h"

/* Mouse discovery/configuration, using UsbHost.c to perform actual USB I/O.
 * Device -> configuration -> interface -> endpoint is a DESCRIPTOR hierarchy:
 * - device: one addressed USB device, possibly with several functions;
 * - configuration: a selectable collection of interfaces;
 * - interface: a function such as mouse or keyboard (not a physical socket);
 * - endpoint: a numbered data channel inside the device.
 * All devices have endpoint zero for configuration requests. Movement reports
 * come from a separate interrupt-IN endpoint found in the mouse interface.
 * This file starts raw reports; it does not decode them or draw the cursor.
 */

/* Startup results for the debugger. The scratch buffer is reused serially;
 * it is not a mouse framebuffer or a controller-visible DMA buffer. */
struct usb_mouse_info usb_mouse_state;
static uint8_t configuration_buffer[USB_CONFIGURATION_MAX_BYTES];
static uint32_t initialized;
static struct {
    uint32_t generation, connected, attempted;
} hotplug[UHCI_CONTROLLER_COUNT][2];

static uint32_t read16(const uint8_t *bytes)
{
    /* USB descriptor/request 16-bit fields are stored low byte first. */
    return bytes[0] | ((uint32_t)bytes[1] << 8);
}

uint32_t usb_mouse_poll_interval(uint32_t low_speed, uint32_t advertised_ms)
{
    if (low_speed > 1 || !advertised_ms || advertised_ms > 255) return 0;
    if (!low_speed && USB_MOUSE_FULL_SPEED_INTERVAL_MS &&
        advertised_ms > USB_MOUSE_FULL_SPEED_INTERVAL_MS) return USB_MOUSE_FULL_SPEED_INTERVAL_MS;
    return advertised_ms;
}

int32_t usb_mouse_find_interface(const uint8_t *data, uint32_t length,
                               uint32_t low, struct usb_mouse_interface *result)
{
    /* Parse bytes already fetched from the device; this function does no I/O.
     * Configuration header: [0] length, [1] type=2, [2..3] total stream length,
     * [5] bConfigurationValue (the value for SET_CONFIGURATION). */
    if (!data || !result || low > 1 || length < 9 || length > USB_CONFIGURATION_MAX_BYTES ||
        data[0] != 9 || data[1] != 2 || read16(data + 2) != length || !data[5]) return -1;
    uint32_t candidate = 0, number = 0, found = 0, have_interface = 0;
    uint32_t endpoints = 0, expected = 0;
    struct usb_mouse_interface selected = {0};
    /* Each following descriptor begins with its byte length and type.
     * Validate the whole stream, even after finding a candidate endpoint. */
    for (uint32_t offset = 9; offset < length;) {
        if (length - offset < 2) return -1;
        const uint8_t *d = data + offset;
        uint32_t size = d[0];
        if (size < 2 || size > length - offset) return -1;
        if (d[1] == 4) { /* Interface descriptor: subsequent endpoints belong here. */
            if (size < 9 || (have_interface && endpoints != expected)) return -1;
            have_interface = 1;
            endpoints = 0;
            expected = d[4];
            number = d[2];
            /* Alternate setting 0; HID class 3; boot subclass 1; mouse protocol 2.
             * A keyboard interface (protocol 1) on the same device is skipped. */
            candidate = d[3] == 0 && d[5] == 3 && d[6] == 1 && d[7] == 2;
        } else if (d[1] == 5) { /* Endpoint descriptor. */
            if (size < 7 || !have_interface || ++endpoints > expected) return -1;
            uint32_t packet = read16(d + 4);
            /* Endpoint address bit 7 = IN; low nibble = endpoint number.
             * Attributes low bits 3 = interrupt transfer; d[6] = interval ms.
             * "IN" always means device -> host, not input to the device. */
            if (candidate && !found && (d[2] & 0x80) && (d[3] & 3) == 3) {
                if (!(d[2] & 15) || (d[2] & 0x70) || packet < 3 ||
                    packet > (low ? 8u : 64u) || !d[6] || (low && d[6] < 10)) return -1;
                selected.configuration = data[5];
                selected.interface_number = number;
                selected.endpoint = d[2] & 15;
                selected.packet = packet;
                selected.interval = d[6];
                found = 1;
            }
        } else if (d[1] == 2 || d[1] == 1) {
            return -1;
        }
        offset += size;
    }
    if (have_interface && endpoints != expected) return -1;
    if (found) *result = selected;
    return (int32_t)found;
}

static int32_t request(uint32_t controller, const struct usb_mouse_port *info,
                       uint32_t packet, uint32_t type, uint32_t operation,
                       uint32_t value, uint32_t index, uint32_t length, void *data)
{
    /* Assemble the standard 8-byte USB SETUP request, not executable code:
     * type, operation, value(low/high), index(low/high), length(low/high).
     * type=80h: standard device read; 00h: standard device write;
     * type=21h: HID-class interface write (index is the interface number).
     * packet is EP0's packet size; length is the total optional data size.
     * Return data bytes transferred, zero for a no-data success, or -1. */
    uint8_t setup[8] = {(uint8_t)type, (uint8_t)operation, (uint8_t)value,
        (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8),
        (uint8_t)length, (uint8_t)(length >> 8)};
    return usb_uhci_control(controller, info->address, info->low_speed, packet, setup, data);
}

uint32_t usb_mouse_probe_port(uint32_t controller, uint32_t port,
                             uint8_t *scratch, struct usb_mouse_port *info)
{
    /* Configure at most one mouse on this root port. status is set before
     * each operation so a failure leaves its stage visible in the debugger. */
    if (controller >= UHCI_CONTROLLER_COUNT || port > 1 || !scratch || !info) return 0;
    *info = (struct usb_mouse_port){0};
    info->status = USB_MOUSE_ABSENT;
    /* 1. Reset assigns default USB address 0. No other enabled device on this
     * controller may still be answering address 0 during this probe. */
    uint32_t speed = usb_uhci_reset_port(controller, port);
    if (!speed) goto failed;
    info->low_speed = speed == 2;
    uint8_t device[18];
    info->status = USB_MOUSE_CONTROL_FAILED;
    /* 2. GET_DESCRIPTOR=6, value=0100h means device descriptor (type 1/index 0).
     * Fetch only 8 bytes initially to learn bMaxPacketSize0 at byte 7. */
    if (request(controller, info, 8, 0x80, 6, 0x0100, 0, 8, device) != 8) goto failed;
    info->status = USB_MOUSE_BAD_DESCRIPTOR;
    uint32_t packet = device[7];
    if (device[0] != 18 || device[1] != 1 ||
        (packet != 8 && packet != 16 && packet != 32 && packet != 64) ||
        (info->low_speed && packet != 8)) goto failed;

    info->status = USB_MOUSE_CONTROL_FAILED;
    /* 3. SET_ADDRESS=5 is sent to OLD address 0. Only after its status stage
     * succeeds and recovery time expires do we use address 1 or 2.
     * This number is a USB bus address, unrelated to RAM or I/O APIC routing. */
    uint32_t address = port + 1;
    if (request(controller, info, packet, 0, 5, address, 0, 0, 0) != 0) goto failed;
    usb_uhci_delay(2); /* SET_ADDRESS recovery before using the new address. */
    info->address = address;
    if (request(controller, info, packet, 0x80, 6, 0x0100, 0, 18, device) != 18) goto failed;
    info->status = USB_MOUSE_BAD_DESCRIPTOR;
    if (device[0] != 18 || device[1] != 1 || device[7] != packet || !device[17]) goto failed;
    /* Full device descriptor: [8..9] vendor, [10..11] product,
     * [4] device class, [17] number of available configurations. */
    info->vendor = read16(device + 8);
    info->product = read16(device + 10);
    info->status = USB_MOUSE_NOT_SUPPORTED;
    if (device[4] == 9 || device[17] > 8) goto failed; /* No hubs; bounded probing. */

    /* 4. GET_DESCRIPTOR value=0200h|index selects a configuration descriptor.
     * Read its 9-byte header first, then its advertised total-length stream
     * (including interface, HID and endpoint descriptors) into scratch. */
    for (uint32_t config = 0; config < device[17]; ++config) {
        info->status = USB_MOUSE_CONTROL_FAILED;
        if (request(controller, info, packet, 0x80, 6, 0x0200 | config, 0, 9, scratch) != 9) goto failed;
        uint32_t length = read16(scratch + 2);
        info->status = USB_MOUSE_BAD_DESCRIPTOR;
        if (scratch[0] != 9 || scratch[1] != 2 || length < 9 || !scratch[5]) goto failed;
        if (length > USB_CONFIGURATION_MAX_BYTES) continue;
        info->status = USB_MOUSE_CONTROL_FAILED;
        if (request(controller, info, packet, 0x80, 6, 0x0200 | config, 0, length, scratch) != (int32_t)length)
            goto failed;
        int32_t found = usb_mouse_find_interface(scratch, length, info->low_speed, &info->interface);
        info->status = USB_MOUSE_BAD_DESCRIPTOR;
        if (found < 0) goto failed;
        if (!found) continue;

        info->status = USB_MOUSE_CONTROL_FAILED;
        /* 5. SET_CONFIGURATION=9 activates bConfigurationValue, NOT the loop's
         * zero-based config index. This also selects alternate setting zero. */
        if (request(controller, info, packet, 0, 9, info->interface.configuration, 0, 0, 0) != 0)
            goto failed;
        /* SET_PROTOCOL=11, value=0 selects boot protocol: buttons + signed X/Y
         * without parsing a HID report descriptor. index names the interface. */
        if (request(controller, info, packet, 0x21, 11, 0, info->interface.interface_number, 0, 0) != 0)
            goto failed;
        /* SET_IDLE=10, value=0 requests reports on changes rather than periodic
         * unchanged copies. It is optional for a boot mouse; STALL is allowed. */
        (void)request(controller, info, packet, 0x21, 10, 0, info->interface.interface_number, 0, 0);
        info->status = USB_MOUSE_START_FAILED;
        /* 6. Give the discovered endpoint to UHCI's periodic schedule. The
         * controller requests reports in hardware; future completions go to
         * usb_host_interrupt and its queue, not back into this probe function. */
        info->scheduled_interval = usb_mouse_poll_interval(info->low_speed, info->interface.interval);
        if (!usb_uhci_start_interrupt(controller, port, address, info->interface.endpoint,
                                      info->low_speed, info->interface.packet, info->scheduled_interval))
            goto failed;
        info->status = USB_MOUSE_STREAMING;
        return 1;
    }
    info->status = USB_MOUSE_NOT_SUPPORTED;
failed:
    /* A failed SET_ADDRESS must not leave an enabled address-zero responder. */
    usb_uhci_disable_port(controller, port);
    return 0;
}

uint32_t usb_mouse_initialize(void)
{
    /* Entry called by kernel_main AFTER usb_host_initialize, before desktop
     * creation. Returns configured mouse count, unlike the host's controller
     * count. usb_mouse_service handles later connections. */
    if (initialized) return usb_mouse_state.mouse_count;
    initialized = 1;
    const struct usb_host_info *host = usb_host_get_info();
    for (uint32_t controller = 0; controller < UHCI_CONTROLLER_COUNT; ++controller) {
        if (host->controller[controller].status != UHCI_READY) continue;
        /* Isolate default address zero, including ports firmware had enabled. */
        usb_uhci_disable_port(controller, 0);
        usb_uhci_disable_port(controller, 1);
        usb_uhci_delay(100); /* Initial connection debounce before reset. */
        for (uint32_t port = 0; port < 2; ++port) {
            if (usb_mouse_probe_port(controller, port, configuration_buffer, &usb_mouse_state.port[controller][port])) {
                ++usb_mouse_state.mouse_count;
                break; /* Current UHCI schedule supports one stream per controller. */
            }
            if (host->controller[controller].status != UHCI_READY) break;
        }
        for (uint32_t port = 0; port < 2; ++port) {
            hotplug[controller][port].connected = host->controller[controller].port_status[port] & 1u;
            hotplug[controller][port].generation = host->port_generation[controller][port];
            hotplug[controller][port].attempted = usb_mouse_state.port[controller][port].status != 0;
        }
    }
    usb_mouse_state.boot_mouse_count = usb_mouse_state.mouse_count;
    return usb_mouse_state.mouse_count;
}

void usb_mouse_service(void)
{
    if (!initialized) return;
    uint32_t active = usb_host_poll();
    if (!usb_uhci_hotplug_due()) return;
    const struct usb_host_info *host = usb_host_get_info();
    uint32_t count = 0;
    for (uint32_t id = 0; id < UHCI_CONTROLLER_COUNT; ++id) {
        for (uint32_t port = 0; port < 2; ++port) {
            struct usb_mouse_port *info = &usb_mouse_state.port[id][port];
            if (info->status == USB_MOUSE_STREAMING && !(active & (1u << id))) {
                info->status = USB_MOUSE_ABSENT;
                hotplug[id][port].connected = 0;
                hotplug[id][port].attempted = 0;
            }
            if (host->controller[id].status != UHCI_READY) continue;
            uint32_t connected = host->controller[id].port_status[port] & 1u;
            uint32_t generation = host->port_generation[id][port];
            if (connected != hotplug[id][port].connected || generation != hotplug[id][port].generation) {
                hotplug[id][port].connected = connected;
                hotplug[id][port].generation = generation;
                hotplug[id][port].attempted = 0;
                if (!connected) info->status = USB_MOUSE_ABSENT;
                continue; /* Require another scan, at least 100 ms later. */
            }
            if (!connected || (active & (1u << id)) || hotplug[id][port].attempted) continue;
            hotplug[id][port].attempted = 1;
            usb_uhci_disable_port(id, 0);
            usb_uhci_disable_port(id, 1);
            if (usb_mouse_probe_port(id, port, configuration_buffer, info)) active |= 1u << id;
        }
        if (active & (1u << id)) ++count;
    }
    usb_mouse_state.mouse_count = count;
}
