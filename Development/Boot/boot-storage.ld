OUTPUT_FORMAT(elf32-i386)
ENTRY(bootstrap_load_images)
SECTIONS
{
    . = 0x30001000;
    .text : { KEEP(*(.text.bootstrap_load_images)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    ASSERT(. <= 0x30008000, "Native boot loader exceeds 64-sector bootstrap")
    . = 0x30040000;
    __boot_bss_start = .;
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    __boot_bss_end = .;
    ASSERT(. <= 0x3004F000, "Boot state overlaps diagnostic report")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
