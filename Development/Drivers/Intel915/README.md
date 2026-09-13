# Intel 915GM / 910GML desktop graphics driver

This is a boot-time bring-up test for PCI `8086:2592`, not a complete display
driver. Its host setup is called by `kernel_main()` after keyboard/runtime initialization.
Other GPUs, including VirtualBox's graphics adapter, are skipped.

## What Runs

- The kernel finds the PCI BARs and maps MMIO/GTT registers uncached, supervisor-only.
- The PMM allocates the driver's RAM and physical framebuffer pages. All GPU-owned
  pages remain pinned until reboot, including on a timeout. There is no teardown yet.
- The kernel allocates ring-1 code/data descriptors and a DPL3 entry call gate.
- The ring-3 desktop process requests the initial surface through gateway request 4
  at `03Bh:0x7FFFF000`. The CPU enters ring 1 using this task's TSS SS1:ESP1; `RETF`
  returns to the gateway and another `RETF` returns to the desktop.
- The graphics-only IRETD wrapper and ring-0 return gate no longer exist. The driver
  is not a scheduled worker. Timer preemption remains enabled during calls, with
  a separate 8 KiB ring-1 stack for every protected process slot.
- Only the designated desktop TSS may use the graphics service. A non-spinning lock
  guards submission and repeat initialization returns success without reprogramming.
- The GPU executes `XY_COLOR_BLT` to fill the complete screen teal. A separate
  hardware cursor plane displays a 12x16 arrow inside a transparent 64x64 ARGB image.
- A hardware status-page breadcrumb and framebuffer readback must succeed before
  the native BGRX8888 scanout plane replaces VGA. The CPU generates the small
  cursor bitmap once; the GPU composites it over the framebuffer during scanout.

The desktop now reads events from the ring-2 mouse gate, clamps its pointer to
screen bounds, and calls the ring-1 graphics gate to move the hardware cursor.
The driver validates coordinates too. Each move writes CURPOS then CURBASE to
arm the update; it does not redraw the background, remap the GTT, modify the
cursor bitmap, or restart the BLT ring. The cursor hotspot is its top-left tip;
the rest of the image is clipped naturally at the right/bottom screen edges.

The gate accepts operation 0 (initialize), 1 (packed screen size: width in low
16 bits, height in high 16), and 2 (move cursor). Internal register ABI is
EAX=operation, EDX=x, ECX=y. User wrappers are `graphics_get_screen_size()`
(public request 11) and `graphics_move_cursor(x, y)` (public request 12).
Movement returns I915_DRAWN (2), I915_BUSY for a retry, or a driver error.
All operations check the caller TSS; another process cannot take over the cursor.

The resident desktop drains up to 32 mouse events per batch and submits only
when position changed. It retains pending movement on a busy graphics gate.
Coordinates remain stack/register scalars: the one-page user loader still
does not need C globals or a separate data image. Button events are consumed
and retained by the mouse driver; window clicking/dragging is not implemented.
Command-ring reuse for general drawing and GPU interrupts remain future work.

## Firmware Handoff Requirements

There are no BIOS video calls or VBE calls. This first test nevertheless preserves
the already-running firmware display timing, PLL, LVDS power/backlight and panel
fitter. It does not initialize a cold/off display or choose a new mode. It uses the
pipe's reported dimensions, not a presumed panel resolution.

The PCI memory decoder, a suitable active LVDS/CRT pipe, and the firmware GTT must
already be enabled. Bus mastering is enabled by the host after buffer preparation.
The static RAM map is the same laptop-specific map as the PMM: usable RAM ends at
`0x3F780000`, and graphics stolen/GTT memory must lie above it and below 1 GiB.
Unsupported state produces a panic rather than guessing register values.

This assumes exclusive ownership immediately after firmware boot, not attachment
to a running GPU driver. It uses the final 16 MiB of the 128/256 MiB graphics
aperture as its graphics-address allocation window, outside the firmware's stolen
region. Enabled display-plane bases are checked conservatively for conflicts.
Do not run BIOS video services or another GPU driver after takeover. Multi-pipe
display configurations, SDVO/TV outputs, and dynamic graphics allocation are not
implemented. Test with the laptop panel and no external display.

CPU mappings for the ring, hardware status page, cursor, framebuffer and MMIO/GTT are UC
(`PWT|PCD` with the kernel's default PAT). Other driver RAM is WB. Do not access
these DMA pages through a conflicting cached alias or free them while the GPU can
reference them. Ring-1 drivers are trusted: on this CPU, ring 1 and ring 2 both
have supervisor paging access; these descriptors are not a security sandbox.

## Memory Layout

| Linear address | Purpose |
| --- | --- |
| `0x71000000..0x71003FFF` | Driver image; ring-1 CS base is `0x71000000` |
| `0x71008000` | Shared diagnostic header, layout in `Intel915.h` |
| `0x71009000` | Physical-addressed hardware status page |
| `0x7100A000` | 4 KiB primary command ring |
| `0x71010000..0x71013FFF` | 64x64 ARGB hardware cursor, four contiguous physical pages |
| `0x71014000..0x71015FFF` | Framebuffer physical-page list |
| `0x71020000..0x710E7FFF` | 100 separate 8 KiB ring-1 process stacks |
| `0x71400000..0x7147FFFF` | PCI MMIO BAR alias |
| `0x71500000..0x7153FFFF` | PCI GTT BAR alias |
| `0x71800000..0x71FFFFFF` | Framebuffer CPU alias, only needed pages mapped |

Ring-1 DS/SS use base `0x71000000` with a 16 MiB limit. C is linked at offset
`0x1000`; the driver entry gate targets offset zero. `Intel915DriverEntry.asm`
contains the ring-1 entry stub and embeds the C payload. `intel915-driver.ld`
controls that payload's segment-relative layout. The former
`Intel915RingTransition.asm` has been removed.

i915GM cursor fetch uses a CPU physical address, not a GTT offset. Startup
searches up to 64 allocated PMM pages for a four-page contiguous run, releases
unchosen pages, and keeps the selected run pinned. This limited boot-time
allocator fails visibly rather than assuming separately allocated pages are
contiguous. Both cursor planes are disabled by the emergency VGA panic path.

`KernelMain.c` is the OS-wide startup file, not a graphics driver file; it only
calls `intel915_initialize()` and then `desktop_create()`. The graphics
implementation remains in `Intel915.c`; the user task is under `Development/Desktop`.

The disk image remains unchanged in size/layout: VirtualKernel is still 256
sectors. The C region ends before `0x80010000`, the 16 KiB graphics image is embedded
at `0x80010000`, the desktop image at `0x80014000`, keyboard remains at
`0x80018000`, and the gateway at `0x8001C000`.

## Testing / Diagnostics

`build-image.bat` already invokes `build-kernel-c.ps1`; there is no separate driver
build step to run manually. The component build only recompiles binaries; it does
not update `myos.img`, write USB or launch a VM.

Run `tests/Intel915/test-intel915.ps1` for native command-buffer tests. They check
dimensions, pitch, background packets, transparent cursor pixels, contiguous
page runs, coordinate validation, signed movement/clamping and the report-to-
position pipeline. They do not emulate a GPU
or validate a CPU privilege transition.

On the laptop, connect a supported USB boot mouse before boot. Expect a full-screen
teal background and a small arrow that moves without leaving trails. Without a
supported mouse it stays centered. Host setup errors panic; driver-call errors return to the desktop, which
prints `G` and the hexadecimal error via the VGA syscall. Photograph that code or
any panic message. The earlier kernel-driven test was verified on hardware; the
new desktop/call-gate path still needs a hardware run.

| Status | Meaning |
| --- | --- |
| `02` | GPU commands/readback completed; display registers programmed |
| `11` | Firmware GTT backing memory conflicts with the known RAM map |
| `12` | Firmware GTT is not enabled |
| `13` | No supported active display / conflicting active pipes |
| `14` | Unsupported dimensions or framebuffer larger than 8 MiB |
| `15` | Existing command ring did not become idle |
| `17` | GDT descriptor allocation failed |
| `18` | Existing scanout conflicts with our GTT window |
| `19` | Command buffer cannot hold the scene |
| `1A` | GPU completion/idle timeout |
| `1B` | GPU framebuffer readback did not match |
| `1C` | Driver did not execute at CPL1 |
| `1D` | One-shot entry invoked again / invalid initial state |
| `1E` | Caller is not the designated desktop TSS |
| `1F` | Another submission owns the non-spinning lock |
| `20` | Unsupported graphics operation |
| `21` | Hardware cursor initialization/readback failed |

`0x71008014` is status; `0x71008018` is stage; `0x7100801C` records actual CS.
Stages: 1 host ready, 2 ring-1 entered, 3 submitted, 4 GPU completed,
5 display programming, 6 finished. The completion word should be `0x915D0001`.
The panic text-mode path also disables the native planes and re-enables the VGA
plane. This is a best-effort recovery using the preserved firmware timing, not a
complete native modesetter or GPU reset.

`0x71008000` holds the desktop TSS selector and `0x71008004` the accepted request
count. The ring-3 process cannot read this supervisor-only header. The published
kernel selector slots are `0x8000233C` (gate), `0x80002340` (data),
`0x80002344` (code), and `0x80002348` (ready).

Cursor diagnostics are appended without moving the old fields: physical base
at `0x71008088`, X/Y at `0x7100808C/90`, enabled at `0x71008094`, and movement
submission count at `0x71008098`. An error after graphics takeover may not show
the VGA `G` message; inspect status/stage in this header in that case.

## References

- Local Intel Mobile 915/910 datasheet, document 305264-002, especially device-2
  configuration registers (MMADR, GMADR, GTTADR, BSM, MSAC), display and 2D sections:
  `DocumentationForDrivers/mobile-915-910-express-chipset-datasheet.pdf`.
- Haiku's MIT-licensed [intel_extreme driver](https://github.com/haiku/haiku/tree/master/src/add-ons/accelerants/intel_extreme),
  particularly `commands.h`, `engine.cpp`, and the register definitions in
  `headers/private/graphics/intel_extreme/intel_extreme.h`.
- Linux i915 [register definitions](https://kernel.googlesource.com/pub/scm/linux/kernel/git/ralf/linux/+/1008ebb61e01e3152901b4c5f58bd01a60ed115b/drivers/gpu/drm/i915/i915_reg.h)
  and [breadcrumb submission](https://android.googlesource.com/kernel/tegra/+/8c70aac04e01a08b7eca204312946206d1c1baac/drivers/gpu/drm/i915/i915_dma.c).
- Linux i915 [ring stop/start ordering](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux-stable/+/1300dc689f66e7fafcbc28e88c0c839161a3507d/drivers/gpu/drm/i915/intel_ringbuffer.c).
- Linux i915 [cursor register definitions](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/i915/display/intel_cursor_regs.h),
  [cursor update ordering](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/i915/display/intel_cursor.c),
  and [i915GM physical-cursor requirement](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/i915/display/intel_display_device.c).

The implementation here is intentionally small and independent; no external
driver source or library is linked into the kernel.
