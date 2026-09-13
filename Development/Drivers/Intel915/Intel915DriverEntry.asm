; Ring-1 entry stubs and embedded C payload for the Intel915 graphics driver.
format binary
use32
org 0
intel915_driver_service = 1000h ; Intel915.c entry, relative to the driver CS base

; DPL3 gate targeting ring-1 CS. The CPU uses the caller's TSS SS1:ESP1.
intel915_gate_entry:
    pushfd
    call intel915_invoke_c
    popfd
    retf

intel915_invoke_c:
    push ds
    push es
    push fs
    push gs
    pushad
    mov ebp, esp
    mov ax, ss
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    and esp, 0FFFFFFF0h
    sub esp, 4
    push dword [ss:ebp+24]       ; ECX = y
    push dword [ss:ebp+20]       ; EDX = x
    push dword [ss:ebp+28]       ; operation supplied in EAX
    call intel915_driver_service
    mov esp, ebp
    mov [ss:esp+28], eax
    popad
    pop gs
    pop fs
    pop es
    pop ds
    ret

times intel915_driver_service - ($-$$) db 0
file 'Development\Drivers\Intel915\Intel915Ring1.bin'
times 4000h - ($-$$) db 0
