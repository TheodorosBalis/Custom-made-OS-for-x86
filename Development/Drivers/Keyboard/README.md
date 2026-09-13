# Ring-2 Keyboard Driver

## Memory

| Linear address | Purpose |
| --- | --- |
| `0x70000000..0x70003FFF` | Driver image, including assembly entries and C |
| `0x70000000` | Call-gate entry (CS offset zero) |
| `0x70001000` | C driver service entry |
| `0x70008000` | IRQ queue and key state, defined in KeyboardShared.h |
| `0x70010000 + slot * 0x2000` | Start of that process's 8 KiB ring-2 stack |

The kernel reserves a 1 MiB virtual region at 0x70000000, outside the high-half
kernel and above the 100 existing process slots. All 256 physical
pages are obtained from the physical allocator at boot, with supervisor-only
4 KiB mappings. The region's PDE is established before process creation and
shared by subsequent process directories. Physical pages need not be contiguous.
All 100 stack slots are reserved at boot and cleared when a process slot is reused.

Ring-2 CS/DS/SS use base 0x70000000. The driver C image is linked to offsets
beginning at 0x1000, so its ordinary C pointers work within that segment.
The kernel accesses the same queue through its full linear address.
Code has a 16 KiB segment limit; data/stack have a 1 MiB limit. DS and SS
must keep the same base for C stack pointers to work.

This is trusted OS driver code. It is not a malicious-driver sandbox: paging
does not distinguish rings 0, 1 and 2, and existing flat ring-3 segments are
also loadable from ring 2. No direct port access is granted to this driver.
Controller initialization, port reads, and EOI remain in ring 0.

## Request ABI

Every application calls 003Bh:0x7FFFF000. That remains ring 3.
The copied page is read-only to applications and contains a dispatch table
and the actual allocated GDT call-gate selector.

| EAX request | EBX argument | Dispatch | EAX result |
| --- | --- | --- | --- |
| 1 | Existing VGA service arguments | SYSENTER to ring 0 | Existing result |
| 2 | Key ID, 0..255 | Call gate to ring 2 | 1 down, 0 up |
| 3 | Unused | Call gate to ring 2 | 1 if any key is down, otherwise 0 |

Unknown requests return 0xFFFFFFFE. Invalid key IDs return 0xFFFFFFFF.
Key IDs use the Set-1 base code; E0 keys use base code + 0x80.
Pause is consumed as a pulse with no persistent down state because its
Set-1 sequence has no release. A later event interface can expose that pulse.

UserServices.h and UserServices.o provide stdcall C wrappers. Link the object
into an application, not into the kernel. For example:

```c
uint32_t pressed = keyboard_is_key_down(KEY_A);
uint32_t any = keyboard_is_any_key_down();
```

The driver stub saves registers and segments, aligns the C stack, and uses
RETF to restore the gateway's ring-3 stack. The gateway uses a second RETF
to restore the application's original CS. A call gate does not change CR3.

## Queue And Scheduling

The IRQ 0x31 handler reads port 0x60, appends a byte, and sends LAPIC EOI.
Only ring-2 code decodes bytes and modifies key state. The 256-byte ring
queue has 255 usable entries. Overflow increments a counter; the driver
discards uncertain state to avoid a permanently stuck key after a lost release.

There is no scheduled driver task or driver-owned scheduler slot. Each query
enters CPL 2 in the calling application's task and drains pending input before
answering. A lock serializes callers; IRQ code never takes it. Keep IF enabled
in callers, because the timer scheduler must be able to resume a preempted owner.
Without queries, input remains buffered and can overflow. Event-driven deferred
processing is a future improvement.

Driver exceptions panic rather than terminating a task while it may own
the shared driver lock. Application exceptions retain the existing termination path.

## Build And Test

build-kernel-c.ps1 builds the kernel C image, ring-2 driver, and user-service
wrappers. The driver image is embedded at VirtualKernel.bin offset 0x18000;
gateway helper code starts at 0x1C000. Kernel C remains below 0x80018000 and
kernel_main remains at 0x80002400. The 128 KiB image reservation is unchanged.

The temporary keyboard demo and probe processes have been removed, including
their loaders and embedded images. No user process is created at boot. The
kernel returns to the assembly idle loop; the driver and call gate remain
available for future applications. F1 has no reserved shutdown action.

Run tests/Keyboard/test-keyboard.ps1 for native decoder tests. These do not execute
privilege transitions.

For driver diagnostics, inspect `dd 0x70008000 L10` in VBoxDbg:

| Offset | Meaning |
| --- | --- |
| +0x00 | Consumer lock |
| +0x04 / +0x08 | Queue head / tail |
| +0x0C | Dropped byte count |
| +0x10 | Last execution CPL; 2 after a driver call |
| +0x14 | Driver service call count |
| +0x24 | Number of keys currently down |
| +0x28 | Start of 256-byte key-state table |

Without an application querying the driver, incoming scan codes remain queued
and can overflow.
