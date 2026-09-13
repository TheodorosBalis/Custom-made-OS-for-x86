# Desktop process

## Normal Desktop Startup

`Desktop.c` is compiled twice: its host half creates the task at boot; its
`DESKTOP_USER` half is the ring-3 `desktop_main()` program. The process is a first
desktop-shell foundation, not yet a window manager, file browser or compositor.

Startup:

1. `KernelMain.c` calls `intel915_initialize()` to prepare supervisor GPU mappings
   and publish a DPL3 graphics call gate targeting ring 1. It does not draw.
2. `desktop_create()` calls the existing `process_create()` and one-page code/stack
   loader (`process_prepare_test_task`, its historical API name).
3. Process creation installs the shared `03Bh:0x7FFFF000` gateway and its own
   ring-1 stack in TSS `SS1:ESP1`. The kernel assigns graphics ownership to this TSS.
4. After the kernel enables interrupts, the existing LAPIC scheduler starts the task.
5. The user program calls `graphics_initialize_desktop()` in `UserServices.asm`.
   Gateway request 4 enters the graphics driver through its call gate. Two `RETF`
   instructions return through the gateway to the desktop's randomized LDT CS.

Graphics dispatch is `graphics_initialize_desktop()` -> public gateway request 4
-> `call far [cs:ebx]` through `intel915_gate_entry_ptr` -> ring-1
`intel915_gate_entry` -> `intel915_driver_service(INTEL915_INITIALIZE_DESKTOP)`
-> `draw_desktop()` on first initialization. EBX is preserved by the gateway.
The CS override is necessary because the caller's DS has a process-specific base.
The pointer's selector identifies the allocated call-gate descriptor; that
descriptor holds the target code selector and entry offset, not the pointer's
offset dword. The keyboard gateway similarly uses EBX for its pointer, forwarding
the public EBX key argument in EDX to the keyboard stub and restoring both
registers on return. Calls to the public 003Bh:7FFFF000h entry are unchanged.

The old graphics-only IRETD trampoline and ring-0 return gate are removed. The
normal interrupt handlers still need their IRETD instructions. Graphics calls run
in the desktop's task/address space, with interrupts enabled and a separate
supervisor driver stack. The kernel task's TSS/stack is not temporarily replaced.

The initial surface is a full-screen teal fill with a centered 12x16 white/black
cursor. The desktop stays resident afterward using `PAUSE`; it is still scheduled
and can be preempted. It polls the ring-2 mouse driver, applies signed movement
deltas, clamps the position to the screen, and requests hardware-cursor updates
through the ring-1 graphics gate. Busy updates are retried. Efficient sleep/wakeup
and windows are not implemented yet. No movement panic, timeout, or debug overlay
is active.

The GPU remains in ring 1; only the desktop task can initialize this surface.
Requests are validated, duplicate initialization succeeds without rerunning GPU
setup, and a busy request returns immediately rather than spinning behind a
preempted task. No MMIO or framebuffer pages are made user-accessible.
Process destruction revokes ownership before freeing the TSS selector, so a later
task cannot inherit graphics access by reusing that selector. Desktop restart and
GPU teardown are not implemented yet; the last frame and pinned buffers remain.

`DesktopProcess.asm` supplies the aligned user entry, and `desktop-user.ld` links
the C program at CS-relative offset 0x100. The loader currently supports one code
page and one stack page. Because the process has different CS/DS bases, the linker
rejects C globals/constant data until a proper data-segment image loader is added.

The 4 KiB desktop image is embedded at `0x80014000`, inside the existing 128 KiB
VirtualKernel reservation. It runs from the process's own code page, not from that
kernel address. Build it through the existing build script/batch workflow.

If drawing fails, the desktop requests a VGA diagnostic through SYSENTER:
`G` followed by the eight-digit graphics error code. The driver status/stage still
live at `0x71008014`/`0x71008018`. An absent Intel915 GPU skips desktop creation.
