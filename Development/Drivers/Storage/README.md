# Read-only ring-0 storage

`Storage.c` compiles the existing `Boot/BootStorage.c` transport with kernel
MMIO/panic hooks. The bootstrap retains its own pre-paging copy. No BIOS disk
calls, user driver task, or call gate is used by the runtime storage service.
The kernel calls C directly; applications use the existing 003B:7FFFF000 gateway
and SYSENTER, then return through SYSEXIT as before.

## Safety and support

- No write API, write syscall, filesystem mount, formatting, erase, partition
  update, or write-back cache exists. Existing disk contents are not modified.
- ATA command-register writes are limited to IDENTIFY DEVICE (EC) and READ
  SECTORS (20). SCSI CDBs are allowlisted to TEST UNIT READY, REQUEST SENSE,
  READ CAPACITY(10), and READ(10). USB OUT packets carry protocol commands,
  never disk-write payloads.
- One directly attached high-speed EHCI USB mass-storage device, BOT, LUN 0,
  512-byte logical sectors. The first suitable device is selected; it need not
  contain our boot image. No hubs, UHCI/OHCI/xHCI, UAS, hotplug or retry recovery.
- Up to four ATA disks on PCI IDE channels already in compatibility mode.
  Native-mode IDE, AHCI/SATA controllers, NVMe and ATAPI are not handled.
  LBA28 reads expose at most the first 128 GiB; non-512-byte logical sectors
  are rejected. IDENTIFY reports the model and readable capacity.
- Read failures return errors without copying partial/stale bounce-buffer data.
  An ATA read failure disables further reads on that channel. USB transfer
  failures disable that transport. Failure to stop DMA safely panics instead
  of recycling live DMA buffers.

## Initialization and memory

`kernel_main()` calls `storage_initialize()` before creating any process.
It reserves a shared supervisor-only page table at 80C00000 for an uncached
EHCI MMIO alias and publishes `storage_syscall` at 8000234C. Disk discovery
is lazy unless the ICH6 USB host coordinator requests the EHCI handoff below.

The runtime reuses the bootstrap's pinned, identity-mapped physical DMA area
30050000..30056FFF only after the bootstrap has stopped its controller.
That memory is already excluded from the PMM and shared supervisor-only in
all process directories. x86 coherent DMA uses the existing WB identity
mapping without a conflicting cache alias; a locked barrier orders submission.

On the ICH6 laptop, `usb_host_initialize()` now requests one-time EHCI
discovery before starting UHCI. The storage port stays with EHCI and other
ports are handed to the companions. USB discovery is then sealed so later
storage requests cannot reset the mouse's ports. ATA discovery remains lazy.
On other platforms without this host coordinator, both remain lazy as before.

This is a single-core, synchronous polling implementation. A bounded request
runs with interrupts disabled to serialize PCI, controller and bounce-buffer
access, restoring the caller's IF afterwards. It can stall scheduling during
I/O, especially discovery/timeouts. Queued asynchronous I/O is future work.

## Ring-0 C calls

```c
uint32_t mask = storage_device_mask();
struct storage_device_info info;
uint8_t sector[512];

/* Use a device whose mask bit is set and inspect its model/capacity first. */
uint32_t status = storage_get_info(device, &info);
status = storage_read(device, 0, 1, sector); /* Read only; no disk change. */
```

Device 0 is USB. Devices 1/2 are primary ATA master/slave, 3/4 secondary.
IDs may have gaps. `storage_read` accepts 1..32 sectors and a trusted kernel
destination large enough for blocks*512 bytes. It returns STORAGE_OK (0)
or an error starting at FFFF0001. `block_count` is the exclusive LBA limit.
There is no filename support yet; these calls read raw blocks.

## SYSENTER API

No process has raw disk permission by default, including the desktop.
The kernel may call `storage_allow_process_reads(process, 1)` for a trusted
storage/filesystem process, or pass 0 to revoke it. The flag lives in the
zero-initialized process object (offset 68), so destroyed/reused objects do
not inherit access. This permission grants raw reads of all supported disks,
not file-level access; do not give it to untrusted applications.

User wrappers in `UserServices.asm`:

- `disk_devices()`: EAX=7; returns the device mask or a high-valued error.
- `disk_get_info(device, data_buffer)`: EAX=5, EBX=device, EDI=buffer offset.
- `disk_read_sector(device, lba, data_buffer)`: EAX=6, EBX=device, ESI=LBA,
  EDI=buffer offset; reads exactly 512 bytes.

These are stdcall wrappers. Output pointers are offsets within the caller's
5 MiB DATA segment. Do not pass stack-local buffers: SS has a different base
from DS in this OS. Use mapped user data/heap memory. The kernel validates
the owning process, current CR3, segment bounds, and present/user/writable/
owned PTEs for every output page before any device access. DMA always targets
the kernel bounce buffer, never a user-supplied physical address.

## Testing

Run `tests/Storage/test-storage.ps1` for host-only validation and the static
ATA command audit. `tests/Desktop/KernelLayoutTests.asm` checks ABI offsets.
These tests never access real devices. Hardware reads and USB disconnect/error
paths still require laptop/VM testing by the user. Do not use a write test.

Transport references:
- https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf
- https://www.usb.org/sites/default/files/usbmassbulk_10.pdf
- https://courses.cs.washington.edu/courses/cse451/16au/readings/ata.pdf
