#include "UsbHost.h"

/* USB transport layer: this file controls UHCI hardware, not mouse behavior.
 * Boot: usb_host_initialize -> UsbMouse.c enumerates/configures a mouse.
 * Setup requests: usb_uhci_control -> control_packet -> controller DMA.
 * Reports: controller DMA -> I/O APIC -> assembly stub -> usb_host_interrupt
 *          -> software queue -> usb_uhci_read_report (future mouse driver).
 * USB "interrupt-IN" means hardware polls a device endpoint periodically;
 * it is not an x86 INT instruction and its endpoint number is not an IRQ.
 */

/* Translate chipset wiring: PCI interrupt pin -> PIRQ A..H -> I/O APIC GSI.
 * This routing is specific to the supported ICH6 controllers, not all PCs. */
uint32_t uhci_resolve_gsi(uint32_t lpc_id, uint32_t controller_id, uint32_t function,
                        uint32_t pin, uint32_t d29ip, uint32_t d29ir)
{
    if ((lpc_id != 0x26418086u && lpc_id != 0x26408086u) || function >= 4 ||
        controller_id != (0x26588086u + (function << 16)) || pin < 1 || pin > 4 ||
        ((d29ip >> (function * 4)) & 15u) != pin) return 0xFFFFFFFFu;
    uint32_t pirq = (d29ir >> ((pin - 1) * 4)) & 15u;
    return pirq < 8 ? 16 + pirq : 0xFFFFFFFFu;
}

/* Build a transfer descriptor's token DWORD, which tells UHCI whom to contact.
 * PID: 69h = IN (device to RAM), E1h = OUT, 2Dh = SETUP.
 * toggle selects USB DATA0/DATA1; length is encoded as bytes minus one. */
uint32_t uhci_token(uint32_t pid, uint32_t address, uint32_t endpoint,
                    uint32_t toggle, uint32_t length)
{
    if ((pid != 0x69 && pid != 0xE1 && pid != 0x2D) || address > 127 ||
        endpoint > 15 || toggle > 1 || length > 64) return 0xFFFFFFFFu;
    return pid | (address << 8) | (endpoint << 15) | (toggle << 19) |
           (((length - 1) & 0x7FFu) << 21);
}

uint32_t uhci_interval(uint32_t ms)
{
    /* Use a power-of-two frame interval: e.g. requested 10 ms becomes 8 ms. */
    if (!ms || ms > 255) return 0;
    uint32_t result = 1;
    while (result * 2 <= ms) result *= 2;
    return result;
}

int32_t uhci_completed_length(uint32_t status, uint32_t requested)
{
    if (status & UHCI_ACTIVE) return -2;
    if (status & UHCI_ERRORS) return -1;
    /* Hardware also stores actual length minus one; 7FFh means zero bytes. */
    uint32_t actual = ((status & 0x7FFu) + 1) & 0x7FFu;
    return actual <= requested ? (int32_t)actual : -1;
}

uint32_t uhci_port_update(uint32_t previous, uint32_t clear, uint32_t set)
{
    /* Never echo connection/enable/overcurrent change (W1C) bits. */
    return (previous & ~(0x080Au | clear)) | set;
}

#ifndef USB_HOST_TEST
#include "Include/KernelApi.h"
#include "Include/KernelRuntime.h"
#include "Arch/x86/Exceptions.h"
#include "Drivers/Storage/Storage.h"

/* PCI configuration addresses: bus 0, LPC device 31/function 0;
 * UHCI device 29/functions 0..3; EHCI device 29/function 7.
 * These are not RAM addresses or USB device addresses. */
#define ICH6_LPC 0x0000F800u
#define ICH6_USB 0x0000E800u
#define ICH6_EHCI 0x0000EF00u
#define RCBA_ALIAS 0x80D00000u
#define USB_RUN 0xC1u
#define USB_HALTED 0x20u
#define PORT_CONNECTED 1u
#define PORT_ENABLED 4u
#define PORT_LOW_SPEED 0x100u
#define PORT_RESET 0x200u
#define IOAPIC_MASK 0x10000u

struct uhci_controller {
    /* Controller-visible schedule and packet buffers in reserved DMA RAM. */
    volatile uint32_t *frames;
    struct uhci_qh *control_qh, *interrupt_qh;
    struct uhci_td *control_td, *interrupt_td;
    uint8_t *control_data, *interrupt_data;
    struct uhci_report *queue; /* CPU-only copy queue, not the UHCI schedule. */
    uint32_t producer, consumer, address, endpoint, low_speed, packet, toggle;
    uint32_t active, port;
    uint32_t last_request, last_control_result;
};

/* host is debugger-facing status; controllers holds the working driver state. */
static struct usb_host_info host;
static struct uhci_controller controllers[4];
static uint32_t ticks_per_ms, initialized;
static uint32_t hotplug_time, unclaimed_streak[8];
static usb_report_callback report_callback;

uint32_t usb_host_set_report_callback(usb_report_callback callback)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    uint32_t result = 1;
    for (uint32_t id = 0; id < 4; ++id) {
        if (controllers[id].active) result = 0;
    }
    if (result) report_callback = callback;
    if (flags & 0x200u) __asm__ volatile ("sti" ::: "memory");
    return result;
}

#ifndef USB_HOST_IO_TEST
static uint32_t irq_save(void)
{
    /* On this single-core OS, CLI prevents ISR/foreground queue races.
     * irq_restore must preserve an already-disabled IF, not blindly STI. */
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint32_t flags)
{
    if (flags & 0x200u) __asm__ volatile ("sti" ::: "memory");
}

static uint16_t in16(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %w1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void out16(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %w1" :: "a"(value), "Nd"(port));
}

static void out32(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %w1" :: "a"(value), "Nd"(port));
}

static uint32_t pci_read(uint32_t device, uint32_t reg)
{
    /* CF8 selects a PCI configuration DWORD; CFC reads that selected DWORD. */
    uint32_t value;
    out32(0xCF8, 0x80000000u | device | (reg & ~3u));
    __asm__ volatile ("inl %w1, %0" : "=a"(value) : "Nd"((uint16_t)0xCFC));
    return value;
}

static void pci_write16(uint32_t device, uint32_t reg, uint16_t value)
{
    out32(0xCF8, 0x80000000u | device | (reg & ~3u));
    out16((uint16_t)(0xCFCu + (reg & 2u)), value);
}

static uint32_t tsc(void)
{
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return low;
}

static void delay(uint32_t ms)
{
    uint32_t start = tsc();
    while ((uint32_t)(tsc() - start) < ms * ticks_per_ms) __asm__ volatile ("pause");
}

static void dma_barrier(void)
{
    /* Make descriptor/buffer writes visible before giving UHCI their address.
     * volatile alone does not provide this CPU/device ordering guarantee. */
    __asm__ volatile ("lock; addl $0, (%%esp)" ::: "memory", "cc");
}

static uint32_t ioapic_read(uint32_t reg)
{
    *(volatile uint32_t *)0xFEC00000u = reg;
    return *(volatile uint32_t *)0xFEC00010u;
}

static void ioapic_write(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)0xFEC00000u = reg;
    *(volatile uint32_t *)0xFEC00010u = value;
    (void)*(volatile uint32_t *)0xFEC00010u;
}
#endif

static int wait_io(uint16_t port, uint16_t mask, uint16_t value)
{
    uint32_t start = tsc();
    do {
        if ((in16(port) & mask) == value) return 1;
        __asm__ volatile ("pause");
    } while ((uint32_t)(tsc() - start) < ticks_per_ms * 1000u);
    return 0;
}

static void stop(uint32_t id)
{
    /* A stopped CPU is not a stopped USB controller. Wait for hardware halt
     * before reusing descriptors that the controller can read through DMA. */
    uint16_t io = (uint16_t)host.controller[id].io_base;
    out16(io, 0);
    if (!wait_io(io + 2, USB_HALTED, USB_HALTED)) {
        out16(io + 4, 0);
        KPANIC("UHCI cannot stop DMA safely");
    }
}

static void arm_interrupt(uint32_t id)
{
    /* Prepare ONE incoming report. The ISR calls this again after completion.
     * Link bit 0 = end of list; IOC = request a CPU interrupt on completion. */
    struct uhci_controller *c = &controllers[id];
    c->interrupt_td->link = 1;
    c->interrupt_td->token = uhci_token(0x69, c->address, c->endpoint, c->toggle, c->packet);
    c->interrupt_td->buffer = (uint32_t)c->interrupt_data;
    c->interrupt_td->status = 0x7FFu | UHCI_ACTIVE | UHCI_IOC | (3u << 27) |
                              (1u << 29) | (c->low_speed << 26);
    dma_barrier();
    c->interrupt_qh->element = (uint32_t)c->interrupt_td;
}

static uint32_t service_controller(uint32_t id, uint32_t from_irq)
{
    struct uhci_controller_info *info = &host.controller[id];
    struct uhci_controller *c = &controllers[id];
    if (!c->frames || info->status == UHCI_CONTROLLER_ERROR) return 0;
    uint16_t io = (uint16_t)info->io_base;
    uint32_t status = in16(io + 2);
    if (status & 0x1Fu) {
        if (from_irq) ++info->interrupts;
        info->last_status = status;
        out16(io + 2, (uint16_t)(status & 0x1Fu));
    }
    if (status & 0x18u) {
        out16(io + 4, 0);
        out16(io, 0);
        pci_write16(info->pci, 4, (uint16_t)(pci_read(info->pci, 4) | 0x400u));
        c->active = 0;
        info->status = UHCI_CONTROLLER_ERROR;
        if (report_callback) report_callback(id, USB_REPORT_DISCONNECTED, 0, 0);
        return 1;
    }
    if (!c->active) return !!(status & 0x1Fu);
    uint32_t port = in16((uint16_t)(io + 0x10u + c->port * 2u));
    info->port_status[c->port] = port;
    uint32_t td_status = c->interrupt_td->status;
    host.last_td[id] = td_status;
    if ((port & 5u) != 5u || (port & 2u)) {
        info->status = UHCI_DISCONNECTED;
    } else if (td_status & UHCI_ACTIVE) {
        return !!(status & 0x1Fu);
    } else if (uhci_completed_length(td_status, c->packet) < 0) {
        info->status = UHCI_TRANSFER_ERROR;
    }
    c->interrupt_qh->element = 1;
    if (info->status != UHCI_READY) {
        c->active = 0;
        if (report_callback) report_callback(id, USB_REPORT_DISCONNECTED, 0, 0);
        return 1;
    }
    uint32_t length = (uint32_t)uhci_completed_length(td_status, c->packet);
    if (!from_irq) ++host.poll_completions;
    ++info->reports;
    if (report_callback) report_callback(id, USB_REPORT_DATA, c->interrupt_data, length);
    if (c->producer - c->consumer < UHCI_REPORT_COUNT) {
        struct uhci_report *report = &c->queue[c->producer & (UHCI_REPORT_COUNT - 1)];
        report->length = length;
        memset(report->data, 0, sizeof(report->data));
        memcpy(report->data, c->interrupt_data, length);
        ++c->producer;
    } else {
        ++info->dropped;
    }
    c->toggle ^= 1;
    arm_interrupt(id);
    return 1;
}

/* Called by the assembly IRQ stub, not directly by the mouse or initializer.
 * Several controllers can share one PCI interrupt line, so inspect all of
 * them. This routine only acknowledges hardware and queues bytes; no drawing
 * or user code runs here. The assembly stub sends LAPIC EOI after we return. */
static void usb_host_interrupt(const struct exception_frame *frame)
{
    if (!frame || frame->vector < 0x40 || frame->vector > 0x47) KPANIC("Invalid UHCI interrupt frame");
    uint32_t gsi = 16 + frame->vector - 0x40;
    uint32_t claimed = 0;
    for (uint32_t id = 0; id < 4; ++id) {
        struct uhci_controller_info *info = &host.controller[id];
        if (info->gsi == gsi) claimed |= service_controller(id, 1);
    }
    if (claimed) {
        unclaimed_streak[gsi - 16] = 0;
    } else {
        ++host.unclaimed_interrupts;
        /* A delayed IRQ after polling/boot acknowledgement is harmless.
         * A sustained unknown level source still needs storm protection. */
        if (++unclaimed_streak[gsi - 16] < 32) return;
        uint32_t reg = 0x10 + gsi * 2;
        ioapic_write(reg, ioapic_read(reg) | IOAPIC_MASK);
        host.route_mask &= ~(1u << (gsi - 16));
    }
}

uint32_t usb_host_poll(void)
{
    if (!ticks_per_ms) return 0;
    uint32_t flags = irq_save(), active = 0;
    ++host.polls;
    storage_refresh_usb_companions();
    for (uint32_t id = 0; id < 4; ++id) {
        struct uhci_controller *c = &controllers[id];
        struct uhci_controller_info *info = &host.controller[id];
        if (!c->frames || info->status == UHCI_CONTROLLER_ERROR) continue;
        uint16_t io = (uint16_t)info->io_base;
        uint32_t paused = c->active && !(c->interrupt_td->status & UHCI_ACTIVE);
        if (paused) {
            /* Unlike an end-of-frame IRQ, polling may see TD completion
             * before UHCI finishes writing the queue head. */
            out16(io + 4, 0);
            stop(id);
        }
        service_controller(id, 0);
        if (info->status == UHCI_CONTROLLER_ERROR) continue;
        if (paused && c->active) {
            out16(io, USB_RUN);
            out16(io + 4, 0x0D);
        }
        if (info->status == UHCI_DISCONNECTED || info->status == UHCI_TRANSFER_ERROR) {
            out16(io + 4, 0);
            stop(id);
            c->interrupt_qh->element = c->control_qh->element = 1;
            c->interrupt_td->status = 0;
            c->producer = c->consumer = 0;
            for (uint32_t frame = 0; frame < 1024; ++frame)
                c->frames[frame] = (uint32_t)c->control_qh | 2u;
            dma_barrier();
            info->status = UHCI_READY;
            out16(io + 2, 0x1F);
            out16(io, USB_RUN);
            out16(io + 4, 0x0D);
        }
        for (uint32_t port = 0; port < 2; ++port) {
            uint16_t reg = (uint16_t)(io + 0x10 + port * 2);
            uint32_t value = in16(reg);
            info->port_status[port] = value;
            if (value & 2u) ++host.port_generation[id][port];
            if (value & 0x080Au)
                out16(reg, (uint16_t)(uhci_port_update(value, 0, 0) | (value & 0x080Au)));
        }
        if (c->active) active |= 1u << id;
    }
    irq_restore(flags);
    return active;
}

uint32_t usb_uhci_hotplug_due(void)
{
    if (!ticks_per_ms) return 0;
    uint32_t now = tsc();
    if ((uint32_t)(now - hotplug_time) < ticks_per_ms * 100u) return 0;
    hotplug_time = now;
    return 1;
}

uint32_t usb_host_initialize(void)
{
    /* 1. Initialize once, before processes start. Return CONTROLLER count,
     * not mouse count. Preserve the caller's interrupt-enable state. */
    uint32_t flags = irq_save();
    if (initialized) {
        irq_restore(flags);
        return host.controller_count;
    }
    initialized = 1;
    /* These fixed RAM slots are the assembly/C interface. Store addresses:
     * &host points to data; usb_host_interrupt points to a FUNCTION.
     * No parentheses means we are NOT calling usb_host_interrupt here.
     * Later the IRQ stub loads this slot and performs an indirect call. */
    *(uint32_t *)UHCI_STATUS_POINTER = (uint32_t)&host;
    *(uint32_t *)UHCI_IRQ_CALLBACK = (uint32_t)usb_host_interrupt;
    /* 2. Verify this chipset, then map its routing registers (RCBA).
     * D29IP selects each USB function's PCI pin; D29IR maps pins to PIRQs. */
    uint32_t lpc = pci_read(ICH6_LPC, 0);
    if (lpc != 0x26418086u && lpc != 0x26408086u) goto done;
    uint32_t rcba = pci_read(ICH6_LPC, 0xF0);
    if (!(rcba & 1) || (rcba & ~0x3FFFu) < 0x40000000u) {
        host.error = UHCI_BAD_ROUTE;
        goto done;
    }
    for (uint32_t offset = 0; offset < 0x4000; offset += 4096) {
        if (!KERNEL_API->map_page(RCBA_ALIAS + offset, (rcba & ~0x3FFFu) + offset, 0x1Bu)) {
            KPANIC("Cannot map ICH6 routing registers");
        }
    }
    uint32_t d29ip = *(volatile uint32_t *)(RCBA_ALIAS + 0x3108);
    uint32_t d29ir = *(volatile uint32_t *)(RCBA_ALIAS + 0x3144) & 0xFFFFu;
    /* 3. Coordinate ownership with the USB 2.0 EHCI/storage driver. Keep its
     * selected storage port and hand other ports to UHCI companions.
     * The return value also supplies calibrated TSC ticks per millisecond. */
    host.ehci_bar = pci_read(ICH6_EHCI, 0x10);
    ticks_per_ms = storage_prepare_usb_companions(ICH6_EHCI);
    if (!ticks_per_ms) {
        host.error = UHCI_CONTROLLER_ERROR;
        goto done;
    }
    uint32_t max_gsi = (ioapic_read(1) >> 16) & 255;
    /* 4. Discover each supported UHCI function and validate its I/O/IRQ route. */
    for (uint32_t id = 0; id < 4; ++id) {
        struct uhci_controller_info *info = &host.controller[id];
        uint32_t pci = ICH6_USB | (id << 8);
        uint32_t device = pci_read(pci, 0);
        if ((device & 0xFFFFu) == 0xFFFFu || (pci_read(pci, 8) >> 8) != 0x0C0300u) continue;
        info->pci = pci;
        uint32_t pin = (pci_read(pci, 0x3C) >> 8) & 255;
        uint32_t gsi = uhci_resolve_gsi(lpc, device, id, pin, d29ip, d29ir);
        if (gsi == 0xFFFFFFFFu || gsi > max_gsi) { info->status = UHCI_BAD_ROUTE; continue; }
        uint32_t bit = 1u << (gsi - 16);
        uint32_t redir = 0x10 + gsi * 2;
        uint32_t previous = ioapic_read(redir);
        if (!(host.route_mask & bit) && (!(previous & IOAPIC_MASK) || (previous & 0x5000u))) {
            info->status = UHCI_BAD_ROUTE; /* Do not steal an active route. */
            continue;
        }
        /* BAR4 contains an I/O-port base, unlike a memory-mapped GPU BAR.
         * USB registers are accessed with IN/OUT relative to this base. */
        uint32_t bar = pci_read(pci, 0x20);
        if (!(bar & 1) || (bar & 0xFFFF0000u) || (bar & ~0x1Fu) < 0x100u || (bar & 0x1Eu)) {
            info->status = UHCI_BAD_BAR;
            continue;
        }
        info->io_base = bar & ~0x1Fu;
        info->gsi = gsi;
        info->vector = UHCI_VECTOR_BASE + gsi - 16;
        info->dma = UHCI_DMA_BASE + id * UHCI_DMA_STRIDE;
        uint16_t io = (uint16_t)info->io_base;
        /* 5. Enable PCI I/O + bus mastering (DMA), then reset the controller.
         * Register offsets: +0 command, +2 status, +4 interrupt enables,
         * +6 frame number, +8 physical frame-list address. */
        pci_write16(pci, 4, (uint16_t)((pci_read(pci, 4) | 5u) & ~0x400u));
        pci_write16(pci, 0xC0, 0x2000); /* PCI interrupt enabled, legacy SMI traps disabled. */
        out16(io + 4, 0);
        stop(id);
        out16(io, 2);
        if (!wait_io(io, 2, 0)) { info->status = UHCI_CONTROLLER_ERROR; continue; }
        /* 6. Build an EMPTY schedule in this controller's reserved 16 KiB.
         * These addresses are identity mapped: pointer value == physical DMA
         * address. Do not substitute an arbitrary heap pointer here. */
        memset((void *)info->dma, 0, UHCI_DMA_STRIDE);
        struct uhci_controller *c = &controllers[id];
        c->frames = (volatile uint32_t *)info->dma;
        c->control_qh = (struct uhci_qh *)(info->dma + 0x1000);
        c->interrupt_qh = (struct uhci_qh *)(info->dma + 0x1010);
        c->control_td = (struct uhci_td *)(info->dma + 0x1020);
        c->interrupt_td = (struct uhci_td *)(info->dma + 0x1030);
        c->control_data = (uint8_t *)(info->dma + 0x2000);
        c->interrupt_data = (uint8_t *)(info->dma + 0x2100);
        c->queue = (struct uhci_report *)(info->dma + 0x3000);
        /* Link bit 0 (1) terminates a list; bit 1 (2) identifies a queue head.
         * UHCI reads one of 1024 frame entries each millisecond, then follows
         * its QH/TD links. With no TD attached, there is no mouse transfer yet. */
        c->control_qh->link = c->control_qh->element = 1;
        c->interrupt_qh->link = (uint32_t)c->control_qh | 2u;
        c->interrupt_qh->element = 1;
        for (uint32_t frame = 0; frame < 1024; ++frame) c->frames[frame] = (uint32_t)c->control_qh | 2u;
        out16(io + 2, 0x1F);
        out16(io + 6, 0);
        out32(io + 8, info->dma);
        dma_barrier();
        out16(io, USB_RUN);
        if (!wait_io(io + 2, USB_HALTED, 0)) { info->status = UHCI_CONTROLLER_ERROR; continue; }
        /* Snapshot two root ports. Connection bit 0 says "device present",
         * not "mouse present"; UsbMouse.c must read descriptors to find out. */
        info->port_status[0] = in16(io + 0x10);
        info->port_status[1] = in16(io + 0x12);
        /* 7. Point this GSI at the current CPU, retaining its mask until all
         * controllers sharing a route have been initialized. */
        if (!(host.route_mask & bit)) {
            ioapic_write(redir, previous | IOAPIC_MASK);
            ioapic_write(redir + 1, *(volatile uint32_t *)0xFEE00020u & 0xFF000000u);
            /* PCI INTx: fixed delivery, physical destination, active-low, level. */
            ioapic_write(redir, IOAPIC_MASK | 0xA000u | info->vector);
            host.route_mask |= bit;
        }
        info->status = UHCI_READY;
        out16(io + 4, 0x0D); /* IOC, short packet, CRC/timeout. */
        ++host.controller_count;
    }
    /* Allow the configured I/O APIC lines to deliver interrupts. CPU delivery
     * still requires IF=1; irq_restore below does not force that on. */
    for (uint32_t route = 0; route < 8; ++route) {
        if (host.route_mask & (1u << route)) {
            uint32_t reg = 0x10 + (16 + route) * 2;
            ioapic_write(reg, ioapic_read(reg) & ~IOAPIC_MASK);
        }
    }
done:
    irq_restore(flags);
    return host.controller_count;
}

uint32_t usb_uhci_reset_port(uint32_t id, uint32_t port)
{
    /* Reset one USB root port, not the whole host controller. A reset device
     * responds at USB address zero, so enumeration must isolate other ports. */
    if (id >= 4 || port > 1 || host.controller[id].status != UHCI_READY) return 0;
    uint32_t flags = irq_save();
    if (controllers[id].active) { irq_restore(flags); return 0; }
    uint16_t io = (uint16_t)(host.controller[id].io_base + 0x10 + port * 2);
    uint32_t value = in16(io);
    if (!(value & PORT_CONNECTED)) { irq_restore(flags); return 0; }
    /* Hold reset for 50 ms, release it, then enable USB traffic on this port. */
    out16(io, (uint16_t)uhci_port_update(value, PORT_ENABLED, PORT_RESET));
    delay(50);
    out16(io, (uint16_t)uhci_port_update(in16(io), PORT_RESET, 0));
    delay(10);
    out16(io, (uint16_t)uhci_port_update(in16(io), 0, PORT_ENABLED));
    delay(10);
    value = in16(io);
    host.controller[id].port_status[port] = value;
    irq_restore(flags);
    return (value & 5u) == 5u ? ((value & PORT_LOW_SPEED) ? 2u : 1u) : 0;
}

const struct usb_host_info *usb_host_get_info(void)
{
    return &host;
}

void usb_uhci_debug_snapshot(uint32_t id, struct uhci_debug_snapshot *snapshot)
{
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (id >= 4 || !controllers[id].frames) return;
    uint32_t flags = irq_save();
    struct uhci_controller *c = &controllers[id];
    uint16_t io = (uint16_t)host.controller[id].io_base;
    snapshot->command = in16(io);
    snapshot->status = in16(io + 2);
    snapshot->interrupt_enable = in16(io + 4);
    snapshot->frame = in16(io + 6);
    snapshot->port[0] = in16(io + 0x10);
    snapshot->port[1] = in16(io + 0x12);
    snapshot->td = c->interrupt_td->status;
    snapshot->qh = c->interrupt_qh->element;
    snapshot->control_td = c->control_td->status;
    snapshot->request = c->last_request;
    snapshot->result = c->last_control_result;
    irq_restore(flags);
}

void usb_uhci_delay(uint32_t milliseconds)
{
    /* Keep each TSC comparison short, including across its low-DWORD wrap. */
    while (milliseconds--) delay(1);
}

void usb_uhci_disable_port(uint32_t id, uint32_t port)
{
    if (id >= 4 || port > 1 || !host.controller[id].io_base || controllers[id].active) return;
    uint32_t flags = irq_save();
    uint16_t io = (uint16_t)(host.controller[id].io_base + 0x10 + port * 2);
    out16(io, (uint16_t)uhci_port_update(in16(io), PORT_ENABLED | PORT_RESET, 0));
    host.controller[id].port_status[port] = in16(io);
    irq_restore(flags);
}

static int32_t control_packet(uint32_t id, uint32_t pid, uint32_t address,
                              uint32_t low, uint32_t toggle, uint32_t length)
{
    /* Submit one endpoint-zero packet through the reusable control TD.
     * This blocking helper polls DMA completion; it is used during device
     * configuration, not for the ongoing stream of mouse movement reports. */
    struct uhci_controller *c = &controllers[id];
    uint16_t io = (uint16_t)host.controller[id].io_base;
    stop(id);
    c->control_td->link = 1;
    c->control_td->token = uhci_token(pid, address, 0, toggle, length);
    c->control_td->buffer = (uint32_t)c->control_data;
    c->control_td->status = 0x7FFu | UHCI_ACTIVE | (3u << 27) | (low << 26);
    c->control_qh->element = (uint32_t)c->control_td;
    dma_barrier();
    out16(io, USB_RUN);
    uint32_t start = tsc();
    while ((c->control_td->status & UHCI_ACTIVE) &&
           (uint32_t)(tsc() - start) < ticks_per_ms * 1000u) __asm__ volatile ("pause" ::: "memory");
    stop(id);
    int32_t result = uhci_completed_length(c->control_td->status, length);
    if (in16(io + 2) & 0x18u) {
        host.controller[id].status = UHCI_CONTROLLER_ERROR;
        pci_write16(host.controller[id].pci, 4,
                    (uint16_t)(pci_read(host.controller[id].pci, 4) | 0x400u));
        result = -1;
    }
    c->control_qh->element = 1;
    out16(io + 2, 0x1F);
    return result;
}

int32_t usb_uhci_control(uint32_t id, uint32_t address, uint32_t low,
                       uint32_t packet, const uint8_t setup[8], void *data)
{
    /* A whole USB control transfer has three stages:
     * SETUP (8-byte request), optional DATA packets, then zero-byte STATUS.
     * packet is EP0's maximum packet size, NOT the total request length. */
    if (id >= 4 || address > 127 || low > 1 || !setup || host.controller[id].status != UHCI_READY ||
        (packet != 8 && packet != 16 && packet != 32 && packet != 64) || (low && packet != 8)) return -1;
    uint32_t length = setup[6] | ((uint32_t)setup[7] << 8);
    if (length > USB_CONFIGURATION_MAX_BYTES || (length && !data)) return -1;
    uint32_t flags = irq_save();
    struct uhci_controller *c = &controllers[id];
    if (c->active) { irq_restore(flags); return -1; }
    c->last_request = ((uint32_t)setup[0] << 24) | ((uint32_t)setup[1] << 16) |
                      ((uint32_t)setup[3] << 8) | setup[2];
    uint16_t io = (uint16_t)host.controller[id].io_base;
    out16(io + 4, 0); /* Enumeration is synchronous; periodic transfers are not active yet. */
    /* Stage 1: SETUP always uses DATA0. Only the reserved DMA bounce buffer
     * is given to UHCI; the caller's data pointer need not be physical. */
    memcpy(c->control_data, setup, 8);
    int32_t result = -1;
    if (control_packet(id, 0x2D, address, low, 0, 8) != 8) goto done;
    /* Stage 2: DATA starts at DATA1 and alternates after successful packets.
     * bmRequestType bit 7 selects IN (device -> caller) versus OUT. */
    uint32_t received = 0, toggle = 1;
    while (received < length) {
        uint32_t count = length - received;
        if (count > packet) count = packet;
        if (!(setup[0] & 0x80)) memcpy(c->control_data, (uint8_t *)data + received, count);
        int32_t actual = control_packet(id, (setup[0] & 0x80) ? 0x69 : 0xE1, address, low, toggle, count);
        if (actual < 0) goto done;
        if (setup[0] & 0x80) memcpy((uint8_t *)data + received, c->control_data, (uint32_t)actual);
        received += (uint32_t)actual;
        toggle ^= 1;
        if ((uint32_t)actual < count) {
            /* A short IN packet ends the response; do not read stale bytes. */
            if (!(setup[0] & 0x80)) goto done;
            break;
        }
    }
    /* Stage 3: STATUS uses DATA1 and the opposite direction to DATA.
     * Requests with no DATA stage use an IN status packet. */
    if (control_packet(id, length && (setup[0] & 0x80) ? 0xE1 : 0x69, address, low, 1, 0) != 0) goto done;
    result = (int32_t)received;
done:
    c->last_control_result = (uint32_t)result;
    if (host.controller[id].status == UHCI_READY) {
        out16(io, USB_RUN);
        out16(io + 4, 0x0D);
    }
    irq_restore(flags);
    return result;
}

uint32_t usb_uhci_start_interrupt(uint32_t id, uint32_t port, uint32_t address,
                                uint32_t endpoint, uint32_t low, uint32_t packet, uint32_t ms)
{
    /* Called AFTER SET_CONFIGURATION/SET_PROTOCOL. Schedule periodic device
     * polls; return immediately rather than waiting for the mouse to move. */
    uint32_t interval = uhci_interval(ms);
    if (id >= 4 || port > 1 || address < 1 || address > 127 || endpoint < 1 || endpoint > 15 ||
        low > 1 || !packet || packet > 64 || (low && packet > 8) || !interval ||
        host.controller[id].status != UHCI_READY) return 0;
    uint32_t flags = irq_save();
    struct uhci_controller *c = &controllers[id];
    if (c->active) {
        irq_restore(flags); return 0;
    }
    uint16_t io = (uint16_t)host.controller[id].io_base;
    uint32_t port_value = in16((uint16_t)(io + 0x10 + port * 2));
    if ((port_value & 5u) != 5u || !!(port_value & PORT_LOW_SPEED) != low) {
        irq_restore(flags); return 0;
    }
    stop(id);
    uint16_t port_reg = (uint16_t)(io + 0x10 + port * 2);
    out16(port_reg, (uint16_t)(uhci_port_update(port_value, 0, 0) | (port_value & 0x080Au)));
    c->address = address;
    c->endpoint = endpoint;
    c->low_speed = low;
    c->packet = packet;
    c->toggle = 0;
    c->port = port;
    c->producer = c->consumer = 0;
    c->active = 1;
    if (report_callback) report_callback(id, USB_REPORT_CONNECTED, 0, 0);
    /* Put the report QH in every interval-th frame; other frames remain idle.
     * Hardware walks this repeating frame list without a CPU polling loop. */
    for (uint32_t frame = 0; frame < 1024; ++frame) {
        c->frames[frame] = (uint32_t)(frame % interval ? c->control_qh : c->interrupt_qh) | 2u;
    }
    arm_interrupt(id);
    out16(io + 2, 0x1F);
    out16(io + 4, 0x0D);
    out16(io, USB_RUN);
    irq_restore(flags);
    return 1;
}

uint32_t usb_uhci_read_report(uint32_t id, struct uhci_report *report)
{
    /* Consume a CPU-owned copy already produced by the ISR. This does not
     * send a USB request. Return 1 with one report, or 0 if the queue is empty. */
    if (id >= 4 || !report) return 0;
    uint32_t flags = irq_save();
    struct uhci_controller *c = &controllers[id];
    uint32_t result = 0;
    if (c->consumer != c->producer) {
        memcpy(report, &c->queue[c->consumer & (UHCI_REPORT_COUNT - 1)], sizeof(*report));
        ++c->consumer;
        result = 1;
    }
    irq_restore(flags);
    return result;
}
#endif
