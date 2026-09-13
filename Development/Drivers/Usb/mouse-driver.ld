OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(mouse_driver_service)
SECTIONS
{
    . = 0x5000;
    .text : {
        KEEP(*(.text.mouse_driver_service))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
ASSERT(. <= 0x7000, "Mouse driver exceeds its image reservation")
ASSERT(mouse_driver_service == 0x5000, "Mouse C entry differs from its assembly constant")
