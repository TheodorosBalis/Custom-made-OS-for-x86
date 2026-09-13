OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(kernel_main)

MEMORY
{
    KERNEL_C (rwx) : ORIGIN = 0x80002400, LENGTH = 0xDC00
}

SECTIONS
{
    .text : ALIGN(16)
    {
        KEEP(*(.text.kernel_main))
        KEEP(*(.text.memset))
        KEEP(*(.text.memcpy))
        KEEP(*(.text.memmove))
        KEEP(*(.text.memcmp))
        KEEP(*(.text.strlen))
        KEEP(*(.text.strcmp))
        KEEP(*(.text.strncmp))
        KEEP(*(.text.kstrlcpy))
        KEEP(*(.text.kernel_panic))
        KEEP(*(.text.kernel_assert_fail))
        KEEP(*(.text.exception_dispatch))
        KEEP(*(.text.exception_initialize))
        KEEP(*(.text.ps2_keyboard_initialize))
        KEEP(*(.text.ps2_keyboard_interrupt))
        KEEP(*(.text.usb_uhci_*))
        KEEP(*(.text.storage_allow_process_reads))
        *(.text .text.*)
    } > KERNEL_C

    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    } > KERNEL_C

    .data : ALIGN(16)
    {
        *(.data .data.*)
    } > KERNEL_C

    .bss (NOLOAD) : ALIGN(16)
    {
        __kernel_c_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __kernel_c_bss_end = .;
    } > KERNEL_C

    __kernel_c_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.llvm_addrsig)
    }
}

ASSERT(__kernel_c_end <= ORIGIN(KERNEL_C) + LENGTH(KERNEL_C),
       "C kernel exceeds its reserved space")
ASSERT(kernel_main == 0x80002400, "Kernel C entry differs from its assembly constant")
