# Native Bootstrap Storage

`bl.asm` uses BIOS CHS reads only for LBA 1..64 (`kernel.bin`, 32 KiB).
The bootstrap establishes flat 32-bit segments and a temporary fault IDT,
then calls `bootstrap_load_images()` at physical 0x30001000 with paging and
interrupts disabled. This C code is embedded in kernel.bin, not VirtualKernel.bin.

It discovers an EHCI USB 2.0 controller through PCI, requests firmware ownership,
resets it, enumerates a directly connected high-speed mass-storage device, and
uses Bulk-Only Transport / SCSI READ(10). The initial supported configuration is
LUN 0, alternate setting 0, 512-byte logical blocks and 512-byte bulk packets.
There is no hub driver, UHCI/OHCI/xHCI support, full-speed split-transaction
support, UAS, or automatic recovery from a stalled/dead transport yet. A transport
failure stops use of that device; it is never treated as a successful read.

For the existing VirtualBox IDE-attached image, a native compatibility-mode
ATA PIO reader is available after the USB search. Native-mode IDE, AHCI and ATAPI
are not supported. There are NO BIOS calls in either of these readers.

Device selection matches immutable boot-sector bytes and the bootstrap manifest,
not an assumed conversion from BIOS DL to a USB address or an ATA master. Identical
cloned images are indistinguishable; the first matching copy is used. This is an
image identity/checksum check, not cryptographic authentication. Raw whole-device
images are required; partition-relative images are not supported.

## Disk And RAM Layout

| Image | LBAs | Physical destination |
|---|---|---|
| Boot sector | 0 | 0x00007C00 |
| Bootstrap + native storage | 1..64 | 0x30000000 |
| Virtual kernel | 65..320 | 0x34000000 |
| Interrupt handlers | 321..2369 | 0x32000000 |

`BootLayout.inc` describes this layout. The builder validates it against the
manifest ABI, rather than silently accepting inconsistent sizes. The generated
`BootManifest.inc` at bootstrap +0x800 contains the actual padded payload CRC32s.
All data is read through a 16 KiB bounce buffer, then copied to the fixed physical
destinations. Both CRCs must match before the high kernel can execute.

Bootstrap assembly occupies +0..+0x7FF, its manifest +0x800, and linked C starts at
+0x1000. The image ends at +0x8000, below the stack at +0xF000. The GDT at +0x10000,
IDT at +0x20000, and directory at +0x30000 are unchanged. Boot BSS occupies
0x30040000..0x3004EFFF; USB QH/qTDs, buffers and the disabled periodic list occupy
0x30050000..0x30056FFF.
These addresses are already inside the PMM's reserved bootstrap region.

DMA schedules are stopped and confirmed quiescent before descriptors are reused.
The EHCI controller is halted before paging is enabled. No DMA remains active
when the virtual kernel starts; this boot instance is not a live syscall driver.
Boot waits use PIT-channel-2-calibrated TSC deadlines; no scheduler is needed.

## Build And Test

Use the usual `build-image.bat` yourself. It builds the payloads first, then invokes
`build-bootstrap.ps1` to generate their manifest and compile/assemble the bootstrap
and boot sector, and finally merges/flashes the image as before.

For component-only builds: build the C virtual kernel and interrupt handlers,
then run `build-bootstrap.ps1`. `tests/Boot/test-boot-storage.ps1` tests parsers,
CRC, binary entry addresses and an in-memory image layout without touching disks.
Hardware USB enumeration and transfer timing still require the real-laptop test.

Progress and failures appear on the first VGA text row. An early CPU exception
prints a red `!` and halts rather than entering interrupt code that is not loaded.
The normal exception IDT is installed only after its payload has been loaded and
mapped. Unsupported USB devices halt with a no-matching-image message if no
matching native IDE disk exists. No BIOS fallback loads the remaining payloads.

The diagnostic report at physical 0x3004F000 contains eight dwords: signature,
last USB stage, PCI configuration address (bus/device/function bits), one-based
root port, failed-transfer USBSTS, failed-transfer QH token, current payload LBA,
and selected backend (1 USB, 2 IDE). USB stages: 1 scanning, 2 controller setup/
ownership, 3 root-port reset, 4 device descriptor/address, 5 configuration,
6 SCSI readiness/capacity, 7 image matching, 8 boot loading complete. If boot
halts, a photo of the first VGA row plus this report helps locate the failure.

## Runtime Storage

This component performs read-only boot loading. The separate
[ring-0 storage service](../Drivers/Storage/README.md) reuses the transport with
kernel mappings, serialized requests, per-process access checks, and SYSENTER
buffer validation. Write/flush commands and a filesystem are not implemented.
The bootstrap itself exposes no public disk syscall.

References: [Intel EHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf),
[USB-IF Bulk-Only Transport](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
[Linux ATA register definitions](https://github.com/torvalds/linux/blob/master/include/linux/ata.h).
