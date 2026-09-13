ORG 0
use32
keyboard_driver_service = 1000h ; KeyboardDriver.c entry, relative to the driver CS base

; Called through a 32-bit call gate with no copied stack parameters.
; EAX = operation, EDX = key, EBX = gateway far pointer.
; Return EAX; preserve other GPRs/segments/flags. The public wrapper still uses EBX = key.
keyboard_gate_entry:
    pushfd
    call keyboard_invoke_c
    popfd
    retf

keyboard_invoke_c:
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
    sub esp, 8
    push dword [ss:ebp + 20]      ; saved EDX = key, forwarded by DriverGateway.asm
    push dword [ss:ebp + 28]      ; saved EAX = operation
    call keyboard_driver_service
    mov esp, ebp
    mov [ss:esp + 28], eax
    popad
    pop gs
    pop fs
    pop es
    pop ds
    ret

times keyboard_driver_service - ($-$$) db 0
file 'Development\Drivers\Keyboard\KeyboardDriverC.bin'
times 4000h - ($-$$) db 0
