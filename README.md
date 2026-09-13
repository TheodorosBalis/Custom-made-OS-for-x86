# Custom made OS for x86

A custom operating system written in x86 assembly (FASM) and freestanding C, built around Intel's 32-bit IA-32 architecture.

**The goal of this OS is to make use of most Intel's 32-bit hardware in the operating system itself: TSS structures, hardware task switching, per-process LDTs, virtual-8086 mode, paging, call gates, and all four privilege rings.** The OS is currently aiming for low-end, 1GB RAM memory systems. Each process is allocated with 10MiB of RAM private address space, so a total of 100 processes running possible.

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
build.ps1            Complete source-to-image build
build-kernel-c.ps1   C payloads, driver/user images, and high-half kernel
build-bootstrap.ps1 Bootstrap manifest, native loader, and boot sector
build-tools.ps1     Tool discovery and shared boot-layout validation
```

`Development` contains the active OS and component documentation. The four build scripts above are included in the repository. Historical prebuilt component files are also present, but the build regenerates every required object, executable payload, and boot manifest from source. New build outputs are covered by `.gitignore`; files already tracked by Git remain tracked.

## Build from a fresh clone (Windows)

The supported build host is 64-bit Windows with PowerShell 5.1 or later. The output is a **32-bit x86 raw boot disk image**, not a Windows executable. Everything specific to this OS is in the repository; install the external compiler and assembler below once.

### 1. Install the tools

- **Git:** install [Git for Windows](https://git-scm.com/downloads/win) to clone the repository.
- **FASM 1.x:** download the Windows package from [flatassembler.net](https://flatassembler.net/download.php) and extract it, for example to `C:\Tools\fasm`. Use `FASM.EXE`, not the GUI editor or the separate fasmg assembler.
- **LLVM:** install a Windows x64 distribution containing **`clang.exe`, `ld.lld.exe`, and `llvm-objcopy.exe`**. The [LLVM 19.1.5 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-19.1.5) is the tested compiler version. Point the build at the directory containing all three programs, commonly `C:\Program Files\LLVM\bin`. A Clang-only installation is insufficient.

The source-only build was verified with **FASM 1.73.32 and LLVM/LLD 19.1.5**. Other versions have not been verified. Visual Studio is optional if your LLVM installation already supplies those three tools; the build does not require MSVC libraries, the Windows SDK, Python, NASM, GCC, or a separately installed .NET SDK.

### 2. Clone and compile

Open PowerShell and run:

```powershell
git clone https://github.com/TheodorosBalis/Custom-made-OS-for-x86.git
cd Custom-made-OS-for-x86

powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
    -FasmPath 'C:\Tools\fasm\FASM.EXE' `
    -LlvmBin 'C:\Program Files\LLVM\bin'
```

Replace the two tool paths with your installation paths. `-FasmPath` names the executable; `-LlvmBin` names the directory containing the LLVM executables. The execution-policy option applies only to this PowerShell process.

Alternatively, if all four executables are on `PATH`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

The scripts also accept the `FASM` and `LLVM_BIN` environment variables. Explicit parameters take precedence. No developer-specific installation path is assumed.

### 3. Find and check the result

A successful build ends with `Created: ...\myos.img (2097152 bytes)`. The image is written to the cloned repository's root:

```powershell
(Get-Item .\myos.img).Length  # Expected: 2097152 (2 MiB)
```

`build.ps1` only builds files in the checkout. It does not write a USB drive, request administrator access, start a VM, or alter an existing VM disk. Re-run the same command after source changes; every component is rebuilt, including any prebuilt files shipped in the repository.

The scripts assemble interrupt handlers; compile and link the freestanding C kernel, drivers and desktop; assemble the high-half kernel; generate the payload CRC32 manifest; build the native bootstrap; then pack and validate the final image. The C compiler targets `i386-unknown-none-elf` with `-march=pentium-m` and no host C library.

| Image region | Starting LBA | Reserved sectors (512 bytes each) |
| --- | ---: | ---: |
| Boot sector | 0 | 1 |
| Bootstrap and native storage loader | 1 | 64 |
| High-half kernel and embedded driver/user images | 65 | 256 |
| Interrupt handlers | 321 | 2049 |
| Remaining space | 2370 | 1726 |

[BootLayout.inc](Development/Boot/BootLayout.inc) defines the layout. The build checks component sizes and the boot signature; the bootstrap checks the padded payload CRCs when booting.

### Booting the image

Compilation does not require an emulator. To run the result, use a legacy BIOS x86 VM with **one CPU, at least 1 GiB RAM, and an IDE-attached disk**; the documented loader supports compatibility-mode ATA. `myos.img` is a raw disk image, not an ISO: import or convert it as a hard disk with your emulator's tooling. The fixed physical payload addresses reach `0x34000000`, so smaller RAM configurations are unsuitable for this layout.

Intel915 graphics are hardware-specific. The graphical desktop is skipped when that GPU is absent, including with VirtualBox's standard graphics adapter. A successful build validates compilation and image layout; it does not verify hardware task switching or device behavior in a running VM.

### Troubleshooting

- **Required build tool was not found:** check the exact executable/directory paths above. The script stops before compilation if any tool is missing.
- **`ld.lld.exe` or `llvm-objcopy.exe` is missing:** install the complete LLVM tool distribution, or point `-LlvmBin` at an existing LLVM directory that contains both. `lld-link.exe` is not a substitute for the ELF linker used here.
- **Linker overflow or FASM reports an invalid value:** the OS has fixed image reservations and linker addresses. Use the tested tool versions and inspect the first build error; changing only the final image size does not fix a payload that exceeds its reserved region.

Repository: [TheodorosBalis/Custom-made-OS-for-x86](https://github.com/TheodorosBalis/Custom-made-OS-for-x86).
