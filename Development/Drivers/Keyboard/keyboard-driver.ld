OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(keyboard_driver_service)
SECTIONS
{
    . = 0x1000;
    .text : {
        KEEP(*(.text.keyboard_driver_service))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
ASSERT(. <= 0x4000, "Keyboard driver exceeds its 16 KiB image")
ASSERT(keyboard_driver_service == 0x1000, "Keyboard C entry differs from its assembly constant")
