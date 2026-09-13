ORG 4000h
use32
mouse_driver_service = 5000h

; EAX = operation. The TSS supplies the existing per-process ring-2 stack.
mouse_gate_entry:
    pushfd
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
    and esp, -16
    sub esp, 12
    push dword [ss:ebp + 28]
    call mouse_driver_service
    mov esp, ebp
    mov [ss:esp + 28], eax
    popad
    pop gs
    pop fs
    pop es
    pop ds
    popfd
    retf

times mouse_driver_service - $ db 0
file 'Development\Drivers\Usb\MouseDriverC.bin'
times 7000h - $ db 0
