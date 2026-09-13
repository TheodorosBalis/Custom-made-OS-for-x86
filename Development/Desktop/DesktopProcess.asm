format binary
use32
org 0
desktop_main = 100h             ; Desktop.c entry, relative to the process CS base

; Entered at the process's logical TSS CS:EIP, not by the kernel calling C.
desktop_process_entry:
    cld
    and esp, 0FFFFFFF0h
    call desktop_main
    ud2

times desktop_main - ($-$$) db 0
file 'Development\Desktop\DesktopUser.bin'
times 1000h - ($-$$) db 0
