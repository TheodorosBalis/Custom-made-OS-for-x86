# ICH6 UHCI host support

`UsbHost.c` is the ring-0 USB host coordinator and UHCI driver.
`UsbMouse.c` now enumerates directly connected HID boot mice and starts their
interrupt-IN streams. `MouseDriver.c` supplies the ring-2 decoder/call-gate
service and, in its kernel build, installs the shared queue and gate.
No separately scheduled driver process is created. The desktop consumes events
and moves the Intel915 hardware cursor through the ring-1 graphics gate.

## Controller ownership

`kernel_main()` calls `storage_initialize()`, `usb_host_initialize()` and then
`usb_mouse_initialize()`
before any process is created. On a verified ICH6/ICH6-M, USB initialization:

1. Reads RCBA, D29IP, D29IR and each UHCI function's PCI interrupt pin.
2. Calls the storage transport's one-time EHCI discovery/handoff. EHCI retains
   the selected high-speed storage port; the other ports are handed to UHCI.
3. Starts the four supported UHCI functions with private DMA schedules.
4. Programs verified masked I/O APIC routes, then enables their interrupts.

The EHCI discovery-complete flag prevents later disk requests from resetting
controllers or taking ports back from UHCI. ATA identification stays lazy.
This is a laptop-specific ownership coordinator, not a general multi-host USB
bus manager. It supports the integrated EHCI and its companions at PCI 00:1D.
Unsupported platforms, including ordinary VirtualBox PIIX configurations,
are left alone; no PCI interrupt-line byte is guessed to be an APIC GSI.
The one-time kernel USB handoff adds enumeration time to laptop startup.
The EHCI BAR is only 1 KiB-aligned on ICH6 (datasheet section 14.1.10).
The storage mapper maps its containing 4 KiB physical page and adds the BAR's
offset to the virtual alias; it must not reject a BAR ending in 400h/800h/C00h.
The host status includes the raw `EHCI BAR`. A global `error=4` with all
controller records zero means the EHCI handoff/timing prerequisite failed
before UHCI initialization, not that four initialized controllers went idle.

## Interrupt routing

For known Intel 2640/2641 LPC and 2658..265B UHCI device IDs, the driver checks
the PCI pin against D29IP, resolves its PIRQ through D29IR, and uses the ICH6
APIC mapping PIRQA..H -> GSI 16..23. Vector = 40h + (GSI - 16), so vectors
40h..47h are reserved. Active-low, level-triggered, physical fixed delivery
goes to the current LAPIC ID. Existing active/unowned routes are not replaced.
No RCBA route registers or legacy PIC routes are rewritten.

Each vector retains its 4 KiB IDT slot. The stub saves registers/segments and
calls `usb_host_interrupt`; it issues exactly one LAPIC EOI after C returns.
C services every registered UHCI controller sharing that GSI, acknowledges
USBSTS, queues completed reports, and rearms the periodic TD. It does not
draw, parse HID, or call userspace in the interrupt handler.

One stale/unclaimed interrupt is tolerated. After 32 consecutive unclaimed
interrupts the shared line is masked to prevent a storm; the kernel idle task
can still collect completed mouse TDs. Claimed interrupts reset that streak.
`unclaimed_interrupts` exposes this case. Host-system/process errors disable
that controller's INTx source and require a reboot; they are not blindly reset.

## Mouse enumeration

`usb_mouse_initialize()` runs once before desktop/process creation. For each
ready UHCI controller it disables both root ports, waits 100 ms for initial
connection debounce, then probes one port at a time:

1. Reset/enable the connected port and detect low/full speed.
2. Read the first 8 device-descriptor bytes at USB address zero and validate
   endpoint-zero packet size.
3. Send SET_ADDRESS (port + 1), wait the required 2 ms recovery, and read the
   full device descriptor using the assigned address.
4. Read configuration headers and their descriptor streams. Select an
   alternate-setting-zero HID interface with subclass 1, protocol 2, and an
   interrupt-IN endpoint. Composite devices may contain that mouse interface.
5. Send SET_CONFIGURATION using bConfigurationValue (not the descriptor index),
   then SET_PROTOCOL(boot) addressed to the actual interface number. Try
   SET_IDLE(0); this request is optional for a boot mouse and may STALL.
6. Start periodic interrupt-IN transfers with the descriptor's endpoint,
   maximum packet size and polling interval. Stop probing this controller
   because its current host schedule supports one interrupt stream.

A failed/unsupported probe disables its port before proceeding, including
when SET_ADDRESS failed. The other port is kept disabled once a mouse is
selected. No EHCI storage port is reset by this mouse code. Configuration
streams are bounded to 4096 bytes, at most 8 configurations per device;
oversized configurations, hubs, non-boot mice and nonzero alternate settings
are not supported. A mouse can also be connected after boot. This is a deliberately
limited first HID implementation, not a general USB device manager.

`kernel_high_scheduler_wait` calls `kernel_background_service`, which calls
`usb_mouse_service`, through the callback
at `80002360`. The existing kernel TSS gets a turn between scheduler rounds,
without using one of the 100 process slots. Enumeration never runs in an ISR.
The service collects completed TDs missed by IRQ delivery and scans ports at
100 ms intervals. A new connection must survive another scan before reset.
EHCI returns port ownership to itself on disconnect, so the storage coordinator
hands connected non-storage ports back to UHCI. Its selected disk port and
controller configuration are retained, with no disk rescan or disk commands.
Disconnect/error recovery halts UHCI before recycling the schedule, invalidates
old mouse events, and re-enumerates with DATA0. Unsupported/failed probes are
not retried continuously; another physical connection change permits a retry.

Reports are raw packets queued by `usb_host_interrupt()`. Its kernel callback
also copies them into the ring-2 driver's independent queue. The original
`usb_uhci_read_report()` queue remains available for host debugging. A standard
boot-mouse report begins with buttons,
signed X delta, signed Y delta. Wheel/extra-button report-descriptor parsing is
not implemented. Hardware polling continues when a device NAKs because there
is no movement; an IRQ/report counter need not increase until movement occurs.
Without a consumer, each queue fills and drops later reports independently.
The diagnostic host queue filling does not prevent delivery to the mouse queue.

## Ring-2 driver and user calls

`kernel_main()` calls `mouse_driver_initialize()` after the keyboard driver,
before USB enumeration and process creation. The driver shares the keyboard's
ring-2 DS/SS base (70000000) and existing per-process SS2:ESP2 stack. A TSS has
only one ring-2 stack pair; no second TSS or scheduled driver task is needed.
The mouse has its own DPL2 code descriptor and DPL3 call gate, with zero copied
stack parameters. These trusted ring-2 drivers are not isolated from each other.

Layout: entry at driver offset 4000h, C service at 5000h, image ends at 7000h;
shared state at A000h. Keyboard code/state and per-process stacks are untouched.
The 12 KiB image is embedded at high-kernel address 8001D000h, within the existing
128 KiB image. All driver pages remain supervisor-only in process directories.

User wrappers in `UserServices.h` go through `003Bh:7FFFF000h`, then
`call far [cs:ebx]` to the mouse gate. The assembly entry saves caller state,
loads the shared ring-2 DS from SS, calls `mouse_driver_service`, and returns
with RETF. EAX carries the operation/result; other registers and flags are
preserved. UHCI I/O stays in ring 0; ring 2 only decodes RAM packets.

- Public operation 8, `mouse_read_event()`: dequeue one event, or return zero.
- Public operation 9, `mouse_get_buttons()`: OR of button states in consumed
  reports (bits 0/1/2 = left/right/middle). Does not consume motion events.
- Public operation 10, `mouse_get_devices()`: active stream mask, bits 0..3
  identify UHCI controllers, not physical jack numbers.

A valid event has bit 31 set, buttons in bits 0..2, signed X delta in bits
8..15, signed Y delta in bits 16..23, controller in bits 24..25. Positive Y
is downward. Use `USER_MOUSE_DX/DY/BUTTONS/CONTROLLER` after testing
`USER_MOUSE_VALID`. Wheel and extra buttons are not decoded in boot protocol.

All calls may instead return `USER_MOUSE_LOST` (queue overflow: pending events
discarded and button state cleared), `USER_MOUSE_BUSY` (retry later), or
`USER_MOUSE_BAD_REQUEST`. These have no VALID bit. Calls are nonblocking;
there is no spin waiting for a task holding the driver lock to be scheduled.
There is one global event consumer, intended to be the desktop, not a separate
per-application event stream. Any process with the gateway can currently read
it; desktop-only authorization/focus dispatch is not implemented yet.

The queue holds 31 packets (32 slots, one left empty). Ring 0 writes head and
packet bytes; ring 2 writes tail and decoded state. The producer never takes
the ring-2 caller lock. Startup/transfer-failure notifications update the
connected mask; hotplug/recovery limitations of the host still apply.

For hardware debugging, gate selector is published at 80002358h, ready at
8000235Ch. Dump 7000A000h: lock, head, tail, dropped, connected, last CPL, calls,
decoded, malformed, observed-dropped (DWORDs), followed by four button bytes
and packet slots. After a user wrapper runs, last CPL at 7000A014h should be 2.
The desktop now consumes events in bounded batches and updates the hardware cursor.

## Host API

- `usb_host_initialize()`: once at boot, returns the ready-controller count.
- `usb_uhci_reset_port(controller, port)`: ports 0/1; returns 1 full speed,
  2 low speed, or 0 absent/error. Enumerate/address one port before resetting
  the next; never leave two enabled devices at address zero on the same bus.
- `usb_uhci_control(controller, address, low_speed, packet, setup, data)`:
  synchronous endpoint-zero control transfer, up to 4096 data bytes, returns
  the transferred data length or -1. Short IN packets and zero-length status
  stages are handled. Endpoint-zero packet sizes are 8/16/32/64, only 8 for
  low-speed devices. Call before starting the periodic interrupt endpoint.
- `usb_uhci_start_interrupt(controller, port, address, endpoint, low_speed,
  packet, interval_ms)`: starts one interrupt-IN endpoint per controller.
  Descriptor interval is rounded down to a supported power-of-two period.
  Low-speed packets are at most 8 bytes; full-speed packets at most 64 bytes.
- `usb_uhci_read_report(controller, report)`: nonblocking dequeue, returns
  1 for a report or 0 when empty. The queue holds 16 reports per controller;
  on overflow it drops the newest report and increments the drop counter.

These are trusted kernel host APIs, not passthrough user syscalls. Endpoint
addresses/configuration come from `UsbMouse.c`'s enumeration code.
There is no arbitrary controller/physical-DMA access from ring 3.

## Memory and lifecycle

DMA schedules use physical/identity addresses 30060000..3006FFFF, four 16 KiB
slots. Within a slot: frame list +0000, QHs/TDs +1000, packet buffers +2000,
CPU report queue +3000. This lies inside the existing 30000000..30800000 PMM
reservation and supervisor mapping copied into all process CR3s. It does
not overlap storage DMA at 30050000..30056FFF or the GDT/IDT/page directory.
There are no conflicting cache aliases; locked barriers order coherent x86
DMA submissions. Buffers remain pinned, including on fatal errors.

RCBA uses the uncached supervisor alias 80D00000..80D03FFF. All mappings are
installed before process creation. Controller initialization, port resets and
control transfers are synchronous with interrupts disabled on this single-core
machine. Periodic endpoint polling is performed by UHCI hardware; completion
delivery uses the I/O APIC, not a CPU loop that repeatedly reads the mouse.
There is no hub support or suspend/resume yet. Endpoint transfer failures use
the same bounded re-enumeration path as disconnects. Controller-system errors
remain fatal to that controller. Control transfers are rejected while a
periodic stream is active; live DMA memory is never freed.

## Debugging and verification

Normal boot continues from USB enumeration to Intel915 initialization and
desktop creation. The temporary movement panic, timeout, F12 snapshot trigger,
and graphics checkpoint overlay have been removed. The read-only
`usb_uhci_debug_snapshot()` helper and host/report counters remain available for
debugger inspection; they do not halt or draw anything during normal operation.

`80002350` holds the C interrupt callback; `80002354` holds a pointer to
`struct usb_host_info`. Read the latter pointer, then dump its first four
DWORDs: ready-controller count, enabled-route mask, initialization error,
unclaimed-interrupt count. Four 48-byte `uhci_controller_info` records follow
at offset 16. They contain PCI address, I/O base, GSI, vector, status, IRQ count,
last USBSTS, DMA base, two PORTSC snapshots, report count and dropped count.
The report count now counts every successful report, including when the
optional host debug queue is full. That queue's drops do not lose driver events.
Appended at offset D0h: idle poll count, fallback completion count, four last-TD
status DWORDs, then eight port-generation DWORDs. The older offsets are unchanged.

`usb_mouse_state` is a separate kernel symbol (its address is in KernelMain.elf).
Its first DWORD is the current number of armed mice, followed by eight
40-byte port records ordered controller 0 port 0, controller 0 port 1, etc.
Each record contains status, USB address, low-speed flag, vendor ID, product ID,
configuration value, interface number, endpoint number, max packet size and
interval. Status: 0 not probed, 1 absent/reset failed, 2 unsupported, 3 bad
descriptor, 4 control failed, 5 stream start failed, 6 streaming. This is the
live discovery state, refreshed by the idle service; consult the host's
controller status/reports/dropped counters for later errors and activity.

For a stationary cursor, compare host report counts, the ring-2 queue's
`decoded` DWORD at `7000A01C`, and graphics `cursor_updates` at `71008098`.
Reports without decoded events point to the desktop/gateway/decoder; decoded
nonzero deltas without updates point to graphics dispatch. Increasing cursor
updates with a stationary image point to the GPU update path. Idle `polls`
must increase even when the desktop is the only user process.

`tests/Usb/test-usb-host.ps1` checks route decoding, token layouts, packet
lengths, polling periods, W1C-safe port updates and vectors 40h..47h in the
assembled handler image. It also builds `UsbMouseTests.c`, which runs the actual
enumeration code against a mocked USB transport: request order/recipients,
address recovery, composite-interface selection, optional SET_IDLE failure,
malformed/truncated descriptors, port isolation and failure cleanup.
It also checks disconnect/reconnect debounce and bounded failure retries.
`UsbHostIoTests.c` runs the actual host completion/recovery code against mocked
registers: lost IRQs, no duplicate reports, stale IRQs, storm masking, polling
fallback, quick disconnect/reconnect and safe schedule cleanup.
`MouseDriverTests.c` checks event packing, signed deltas, queue wrap/overflow,
button release and disconnected streams. The test script checks the linked
ring-2 entry, embedded image and absence of privileged port I/O in the decoder.
These are host-only tests; actual controller startup,
IRQ delivery, disconnect handling and report reception require laptop testing.
Disk safety is unchanged: no disk write/format/erase operations are introduced.

Reference: Intel ICH6 Family Datasheet, sections 7.1.43, 7.1.48, 13.1 and 13.2:
https://www.intel.co.id/content/dam/doc/datasheet/io-controller-hub-6-datasheet.pdf
UHCI 1.1:
https://netwinder.oregonstate.edu/pub/misc/docs/29765002-usb-uhci%20design%20guide.pdf
USB HID 1.11, sections 7.2.4/7.2.6 and appendix B.2 (boot mouse):
https://www.usb.org/sites/default/files/hid1_11.pdf
EHCI 1.0 section 4.2.2 (port ownership returns on disconnect):
https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf
