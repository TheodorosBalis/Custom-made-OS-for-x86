# 32-bit Custom Operating System for x86

A custom operating system written in x86 assembly (FASM) and freestanding C, built around Intel's 32-bit IA-32 architecture.

**The goal of this OS is to make extensive use of Intel's 32-bit hardware in the operating system itself: TSS structures, hardware task switching, per-process LDTs, virtual-8086 mode, paging, call gates, and all four privilege rings.** The OS is currently aiming for low-end, 1GB RAM memory systems. Each process is allocated with 10MiB of RAM private address space, so a total of 100 processes running possible.

## Intel IA-32 features at the heart of the OS

| Hardware feature | How this project uses it |
| --- | --- |
| **32-bit Task State Segments (TSS)** | Kernel and process task state, address-space state, and per-task stacks for transitions into more privileged rings, including `SS0:ESP0`, `SS1:ESP1`, and `SS2:ESP2`. |
| **Hardware task switching** | The interrupt/scheduler path uses far jumps to TSS selectors so the processor saves and restores architectural task state. |
| **Local Descriptor Tables (LDTs)** | Protected processes receive their own code, data, and stack descriptors with process-specific segment bases and limits; their TSS records the LDT selector. |
| **Virtual-8086 mode and VME** | VM86 task creation builds a private low-memory view, sets `EFLAGS.VM`, and configures a TSS with I/O and interrupt-redirection bitmaps. The kernel checks CPU support before enabling `CR4.VME`. |
| **Privilege rings 0, 1, 2, and 3** | Ring 0 runs the kernel and privileged device support; ring 1 hosts graphics/audio service entries; ring 2 handles keyboard/mouse services; ring 3 runs the desktop process. |
| **GDT descriptors and call gates** | Dynamically allocated descriptors and call gates connect application requests to driver services, using TSS-provided stacks and far returns for privilege transitions. |
| **Paging and segmentation together** | Per-process page directories, 4 KiB mappings, supervisor/user permissions, and a high-half kernel work alongside segment bases and limits. |
| **SYSENTER / SYSEXIT** | A shared user gateway exposes kernel services through the processor's fast system-call entry/exit mechanism. |
| **IDT and local APIC interrupts** | Dedicated exception/interrupt entries, local APIC timer scheduling, device interrupt routing, and fault handling. |

The core implementation is in [VirtualKernel.asm](Development/Arch/x86/VirtualKernel.asm), [InterruptHandlers.asm](Development/Arch/x86/InterruptHandlers.asm), and the [bootstrap](Development/Boot/kernel.asm). VM86 support is a task-creation facility; it is not a claim of a complete DOS environment. Rings 1 and 2 run trusted driver code: x86 paging treats rings 0–2 as supervisor, so these rings alone do not provide full driver isolation.

## Current components

- **Boot:** BIOS boot sector followed by a protected-mode bootstrap, native EHCI USB mass-storage / compatibility-mode ATA reads, and CRC32 validation of the loaded payloads. See [boot documentation](Development/Boot/README.md).
- **Kernel:** process and memory management, hardware task scheduling, exceptions, system calls, and a small freestanding C runtime.
- **Input:** [PS/2 keyboard and ring-2 decoding](Development/Drivers/Keyboard/README.md), plus [UHCI USB mouse support and a ring-2 mouse service](Development/Drivers/Usb/README.md).
- **Graphics:** [Intel 915GM / 910GML support](Development/Drivers/Intel915/README.md), a GPU-drawn background, hardware cursor, and VGA text diagnostics.
- **Desktop:** a [scheduled ring-3 desktop foundation](Development/Desktop/README.md) that uses driver call gates for graphics and mouse services.
- **Storage:** a [read-only ring-0 block service](Development/Drivers/Storage/README.md) with validated application requests.
- **Audio:** [Intel HD Audio / Realtek ALC269 discovery](Development/Drivers/Audio/README.md) through a ring-1 query service; PCM playback is not implemented yet.

This is an experimental, single-core OS under active development. The desktop is not yet a window manager, and storage does not yet provide a filesystem or write API. Device support is specific to the documented controllers; the Intel desktop is skipped on other GPUs, including VirtualBox's graphics adapter.

## Project layout

```text
Development/
  Arch/x86/          IA-32 tasking, descriptors, paging, interrupts, driver gateway
  Boot/              Boot sector, protected-mode setup, native storage loader
  Kernel/            C kernel entry, panic handling, kernel linker script
  Include/           Shared kernel types, APIs, and runtime declarations
  Runtime/           Freestanding memory and string routines
  User/              Application service declarations and assembly wrappers
  Desktop/           Ring-3 desktop source, entry stub, linker script, documentation
  Drivers/
    Audio/           HDA / ALC269 discovery
    Intel915/        Intel graphics
    Keyboard/        PS/2 controller support and ring-2 keyboard driver
    Storage/         Runtime read-only block service
    Usb/             UHCI host, USB mouse, ring-2 mouse service
    Vga/             VGA text output
docs/
  architecture/      OS block diagram and E820 memory-layout document
  hardware/          Board and chipset reference documents
  history/           Earlier build/USB commands, address notes, and map
examples/legacy/     Earlier assembly helpers grouped by subject
tests/              Host-side tests grouped by component
tools/              USB image writer and E820 PDF generator
build-image.bat      Complete image build and optional local deployment workflow
build-kernel-c.ps1   C payloads, driver/user images, and high-half kernel build
build-bootstrap.ps1 Bootstrap manifest, native loader, and boot-sector build
```

`Development` contains the active OS. The assembly helpers in `examples/legacy` are historical reference material and are not part of the current build. See the [documentation index](docs/README.md) for reference notes.

Generated objects, ELF files, and component binaries are currently written beside their sources; the final image is `myos.img` at the project root. `.gitignore` excludes those outputs, test executables, local caches, and scratch files from source control.

## Build on Windows

Requirements: PowerShell, FASM 1.x, and LLVM with `clang`, `ld.lld`, and `llvm-objcopy`. The current C target is `i386-unknown-none-elf` with `-march=pentium-m`. The scripts default to LLVM bundled with Visual Studio 2022 Community and the original local FASM installation; set `LLVM_BIN` and `FASM` for your installation.

From the project root, build the image without writing a physical drive or changing a VM:

```powershell
$env:FASM = 'C:\path\to\fasm\FASM.EXE'
$env:LLVM_BIN = 'C:\path\to\llvm\bin'
$env:SKIP_USB_FLASH = '1'
$env:SKIP_VM_LAUNCH = '1'
.\build-image.bat
```

The builder assembles the interrupt image, builds the C/assembly kernel and driver payloads, generates their CRC manifest, builds the bootstrap, and packs the 2 MiB raw disk image. The fixed physical load addresses require RAM covering the payload region at `0x34000000`; use at least 1 GiB RAM for the existing VM setup.

These check parsers, driver logic, mocked I/O, payload layout, and embedded interfaces. They do not replace booting the OS to exercise actual hardware task switches, privilege transitions, and device behavior.

Repository: [TheodorosBalis/32-bit-Custom-Operating-System-for-x86](https://github.com/TheodorosBalis/32-bit-Custom-Operating-System-for-x86).
