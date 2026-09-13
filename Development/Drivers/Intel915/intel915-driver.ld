/* Intel915 ring-1 C payload, linked relative to the driver segment base. */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(intel915_driver_service)
SECTIONS
{
    . = 0x1000;
    .text : {
        KEEP(*(.text.intel915_driver_service))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
ASSERT(. <= 0x4000, "Intel915 driver exceeds its 16 KiB image")
ASSERT(intel915_driver_service == 0x1000, "Intel915 C entry differs from its assembly constant")
