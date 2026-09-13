# Azalia / Realtek ALC269 Bring-Up

This is the controller/codec discovery stage, not a PCM playback driver yet.

- `Azalia.c`: PCI discovery and supervisor MMIO mapping in ring 0; query-only codec service in ring 1.
- `AzaliaEntry.asm`: ring-1 call-gate entry, included in the high kernel. C stays in the existing C payload, with a flat ring-1 code selector.
- `Azalia.h`: statuses, addresses, and discovered codec/widget state.

Boot maps the PCI HDA controller at `0x72400000`, allocates fallback per-process ring-1 stacks, and publishes the gate. PCI BARs are read, never probed by writing all ones. The PCI memory-decode bit must already be enabled.

The desktop calls `audio_initialize()` once. User gateway request 13 uses EBX for operation: 0 probes, 1 returns status, 2 returns codec ID. Request 12 is the graphics cursor-move API. The gate loads flat DS/SS while preserving the caller's existing ring-1 stack memory, including a nonzero graphics SS base, and restores that stack before RETF. Graphics keeps its current gate and stack setup.

The driver accepts only codec ID `0x10EC0269`, enumerates its audio function group and widgets, and records pin defaults and PCM capabilities. No raw verb interface is exposed to applications.

The current probe requires a running HDA link and stopped CORB/RIRB engines. It uses the optional Immediate Command interface with bounded polling. It reports `AZALIA_LINK_STOPPED`, `AZALIA_COMMAND_DMA_ACTIVE`, or `AZALIA_COMMAND_TIMEOUT` rather than resetting firmware state or hanging. No streams, bus mastering, volume changes, amplifier power, audio interrupts, or DMA buffers are enabled yet.

Debugger: `dd 0x80002374 L1` returns the address of `struct azalia_state`. Status 2 is codec discovery success, codec ID is at state + 0x1C, and observed CPL at state + 0x3C should be 1 after a gate query. On systems without the Intel desktop, a user process can call the same query manually; the graphics demo is not created there.

Next: controlled link reset and CORB/RIRB transport if needed, enumerate connection lists, verify the board's speaker/headphone path, then implement PCM DMA and an interrupt queue before enabling output.

References:

- [Local board schematic](../../../docs/hardware/900SD.pdf), block diagram and sheets 33-35: ALC269 and board wiring.
- [Intel HD Audio specification](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf), sections 3.4 and 7: immediate commands and codec parameters.
- [Linux HD Audio notes](https://www.kernel.org/doc/html/latest/sound/hd-audio/notes.html): board-specific pin configuration and codec quirks.
