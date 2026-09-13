OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(desktop_main)
SECTIONS
{
    . = 0x100;
    .text : {
        KEEP(*(.text.desktop_main))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
ASSERT(. <= 0x1000, "Desktop entry image exceeds one page")
ASSERT(desktop_main == 0x100, "Desktop C entry differs from its assembly constant")
/* The current one-page loader initializes code and stack, not a C data image. */
ASSERT(SIZEOF(.rodata) + SIZEOF(.data) + SIZEOF(.bss) == 0,
       "Desktop globals require a data-segment loader; use stack/register state for now")
