ORG 0xFF000000
use32

;=========== useful APIC constants===================

LOCAL_APIC_PHYS   = 0xFEE00000
IO_APIC_PHYS      = 0xFEC00000

LAPIC_EOI         = 0x000000B0
LAPIC_SVR         = 0x000000F0
LAPIC_LVT_TIMER   = 0x00000320
LAPIC_LVT_LINT0   = 0x00000350
LAPIC_LVT_LINT1   = 0x00000360
LAPIC_LVT_ERROR   = 0x00000370
LAPIC_TIMER_INIT  = 0x00000380
LAPIC_TIMER_CURR  = 0x00000390
LAPIC_TIMER_DIV   = 0x000003E0

END_OF_INTERRUPT  = LOCAL_APIC_PHYS + LAPIC_EOI
SPURIOUS_REG      = LOCAL_APIC_PHYS + LAPIC_SVR
TIMER_LVT_REG     = LOCAL_APIC_PHYS + LAPIC_LVT_TIMER
LINT0_LVT_REG     = LOCAL_APIC_PHYS + LAPIC_LVT_LINT0
LINT1_LVT_REG     = LOCAL_APIC_PHYS + LAPIC_LVT_LINT1
ERROR_LVT_REG     = LOCAL_APIC_PHYS + LAPIC_LVT_ERROR
TIMER_INIT_REG    = LOCAL_APIC_PHYS + LAPIC_TIMER_INIT
TIMER_CURR_REG    = LOCAL_APIC_PHYS + LAPIC_TIMER_CURR
TIMER_DIV_REG     = LOCAL_APIC_PHYS + LAPIC_TIMER_DIV

IOREGSEL          = 0x00
IOWIN             = 0x10
IOAPIC_REG_ID     = 0x00
IOAPIC_REG_VER    = 0x01
IOAPIC_REDTBL     = 0x10

MAX_TASKS              = 100
SCHEDULER_ENTRY_SIZE   = 2

INTERRUPT_DATA_SELECTOR = 020h
KERNEL_VGA_TEXT         = 080B80000h
GDT_BASE                = 030010000h
TSS_OWNER_PROCESS       = 068h
; Addresses of runtime-published function pointers, not the C code itself.
exception_dispatch_ptr       = 080002320h ; Exceptions.c
lapic_interrupt_dispatch_ptr = 080002324h ; LapicInterrupts.c
ps2_keyboard_interrupt_ptr   = 080002328h ; Ps2Keyboard.c
usb_host_interrupt_ptr       = 080002350h ; UsbHost.c
mouse_cursor_service_ptr     = 080002364h ; MouseDriver.c
KERNEL_API_TABLE        = 080002200h
KERNEL_API_HEADER_SIZE  = 16
KERNEL_API_PROCESS_DESTROY_INDEX = 4
KERNEL_API_PROCESS_DESTROY_PTR = KERNEL_API_TABLE + KERNEL_API_HEADER_SIZE + KERNEL_API_PROCESS_DESTROY_INDEX * 4

macro exception_no_error vector {
    cli
    push 0
    push vector
    jmp exception_common
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0
}

macro exception_with_error vector {
    cli
    push vector
    jmp exception_common
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0
}


;==============INT 0 DIVISION ERROR FAULT==================
int0_handler:
    cli
    push 0
    push 0
    jmp exception_common

; Entry stack before our saves:
;   vector, error, EIP, CS, EFLAGS, [user ESP, user SS]
exception_common:
    cld
    push ds
    push es
    push fs
    push gs
    pushad

    mov ax, INTERRUPT_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, [exception_dispatch_ptr]
    test eax, eax
    jz exception_callback_missing

    ; Preserve the unmodified frame pointer while satisfying the C stack ABI.
    mov ebx, esp
    and esp, -16
    sub esp, 12
    push ebx
    call eax
    mov esp, ebx

    ; Kernel exceptions never return from C. A return means that the faulting
    ; context was a user or VM86 process and must be removed from scheduling.
    jmp exception_terminate_current

exception_callback_missing:
    mov word [KERNEL_VGA_TEXT], 04F45h
    cli
exception_callback_missing_hang:
    hlt
    jmp exception_callback_missing_hang

; The active hardware task cannot free its own TSS or stack. Remove it from
; the runnable table, defer destruction, and switch away permanently.
exception_terminate_current:
    call scheduler_reap_terminated

    str ax
    movzx edx, ax
    mov eax, edx
    and eax, 0FFF8h
    add eax, GDT_BASE

    movzx ebx, word [eax + 2]
    movzx ecx, byte [eax + 4]
    shl ecx, 16
    or ebx, ecx
    movzx ecx, byte [eax + 7]
    shl ecx, 24
    or ebx, ecx

    mov esi, [ebx + TSS_OWNER_PROCESS]
    test esi, esi
    jz exception_terminate_current_fail

    mov [scheduler_reap_process], esi

    mov eax, [scheduler_current_index]
    cmp eax, MAX_TASKS
    jae exception_terminate_current_fail

    lea ebx, [scheduler_task_table + eax*2]
    cmp word [ebx], dx
    jne exception_terminate_current_fail
    mov word [ebx], 0

    cmp dword [scheduler_task_count], 0
    je exception_terminate_current_find_next
    dec dword [scheduler_task_count]

exception_terminate_current_find_next:
    mov ecx, MAX_TASKS

exception_terminate_current_scan:
    inc eax
    cmp eax, MAX_TASKS
    jb exception_terminate_current_index_valid
    xor eax, eax

exception_terminate_current_index_valid:
    lea ebx, [scheduler_task_table + eax*2]
    movzx edx, word [ebx]
    test edx, edx
    jnz exception_terminate_current_switch_process
    dec ecx
    jnz exception_terminate_current_scan

    ; No user task remains. Resume the kernel task that launched the scheduler.
    mov dword [scheduler_current_index], 0FFFFFFFFh
    movzx edx, word [scheduler_kernel_tss_selector]
    test edx, edx
    jz exception_terminate_current_fail
    mov word [scheduler_far_ptr + 4], dx
    mov ebx, scheduler_far_ptr
    jmp far [ebx]

exception_terminate_current_switch_process:
    mov [scheduler_current_index], eax
    mov word [scheduler_far_ptr + 4], dx
    mov ebx, scheduler_far_ptr
    jmp far [ebx]

exception_terminate_current_fail:
    mov word [KERNEL_VGA_TEXT], 04F58h
    cli
exception_terminate_current_fail_hang:
    hlt
    jmp exception_terminate_current_fail_hang

    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0

int1_handler:
    exception_no_error 1
int2_handler:
    exception_no_error 2
int3_handler:
    exception_no_error 3
int4_handler:
    exception_no_error 4
int5_handler:
    exception_no_error 5
int6_handler:
    exception_no_error 6
int7_handler:
    exception_no_error 7
int8_handler:
    exception_with_error 8
int9_handler:
    exception_no_error 9
intA_handler:
    exception_with_error 10
intB_handler:
    exception_with_error 11
intC_handler:
    exception_with_error 12
intD_handler:
    exception_with_error 13
intE_handler:
    exception_with_error 14
intF_handler:
    exception_no_error 15
int10_handler:
    exception_no_error 16
int11_handler:
    exception_with_error 17
int12_handler:
    exception_no_error 18
int13_handler:
    exception_no_error 19
int14_handler:
    exception_no_error 20
int15_handler:
    exception_no_error 21
int16_handler:
    exception_no_error 22
int17_handler:
    exception_no_error 23
int18_handler:
    exception_no_error 24
int19_handler:
    exception_no_error 25
int1A_handler:
    exception_no_error 26
int1B_handler:
    exception_no_error 27
int1C_handler:
    exception_no_error 28
int1D_handler:
    exception_no_error 29
int1E_handler:
    exception_no_error 30
int1F_handler:
    exception_no_error 31


    ; APIC timer vector 0x20 handler
int20_handler:
    cli
    cld

    push ds
    push es
    push fs
    push gs
    pushad

    mov ax, INTERRUPT_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call scheduler_reap_terminated

    mov dword [0FEE000B0h], 0       ; LAPIC EOI
    call service_pending_cursor
    call scheduler_switch_next

    popad
    pop gs
    pop fs
    pop es
    pop ds
    iretd

; scheduler_switch_next()
; Called from APIC timer handler.
; It resumes here later when this task is scheduled again.
scheduler_switch_next:
    push eax
    push ebx
    push ecx
    push edx

    mov ecx, [scheduler_task_count]
    test ecx, ecx
    jz scheduler_switch_next_no_switch

    mov eax, [scheduler_current_index]
    mov ecx, MAX_TASKS

scheduler_switch_next_scan:
    inc eax
    cmp eax, MAX_TASKS
    jb scheduler_switch_next_index_in_range
    ; Give the existing kernel idle task a turn without consuming a process slot.
    mov eax, 0FFFFFFFFh
    movzx edx, word [scheduler_kernel_tss_selector]
    test edx, edx
    jnz scheduler_switch_next_found
    xor eax, eax

scheduler_switch_next_index_in_range:
    lea ebx, [scheduler_task_table + eax*2]
    movzx edx, word [ebx]
    test edx, edx
    jnz scheduler_switch_next_found

    dec ecx
    jnz scheduler_switch_next_scan
    jmp scheduler_switch_next_no_switch

scheduler_switch_next_found:
    mov [scheduler_current_index], eax
    str bx
    cmp bx, dx
    je scheduler_switch_next_no_switch
    mov word [scheduler_far_ptr + 4], dx

    ; These pushes intentionally remain on this task's kernel stack.
    ; When the task is scheduled again, execution resumes below and
    ; removes them exactly once.
    mov ebx, scheduler_far_ptr
    jmp far [ebx]

scheduler_switch_next_resumed:
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

scheduler_switch_next_no_switch:
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

service_pending_cursor:
    mov eax, [mouse_cursor_service_ptr]
    test eax, eax
    jz service_pending_cursor_done
    push ebx
    mov ebx, esp
    and esp, -16
    call eax                     ; Bounded ring-0 work; IF stays clear.
    mov esp, ebx
    pop ebx
service_pending_cursor_done:
    ret

; Free a process only after a hardware task switch has made its TSS inactive.
scheduler_reap_terminated:
    push eax
    mov eax, [scheduler_reap_process]
    test eax, eax
    jz scheduler_reap_terminated_done

    mov dword [scheduler_reap_process], 0
    push eax
    call dword [KERNEL_API_PROCESS_DESTROY_PTR]

scheduler_reap_terminated_done:
    pop eax
    ret
     times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0

int21_handler:
    cli
    push 0
    push 021h
    jmp lapic_interrupt_common

; Entry stack before our saves:
;   vector, zero, EIP, CS, EFLAGS, [user ESP, user SS]
lapic_interrupt_common:
    cld
    push ds
    push es
    push fs
    push gs
    pushad

    mov ax, INTERRUPT_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, [lapic_interrupt_dispatch_ptr]
    test eax, eax
    jz lapic_callback_missing

    mov ebx, esp
    and esp, -16
    sub esp, 12
    push ebx
    call eax
    mov esp, ebx

    ; A genuine spurious vector never enters the LAPIC in-service register.
    cmp dword [esp + 48], 0FFh
    je lapic_interrupt_restore

    ; Every other configured LAPIC source here requires exactly one EOI.
    mov dword [END_OF_INTERRUPT], 0

lapic_interrupt_restore:
    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 8
    iretd

lapic_callback_missing:
    mov word [KERNEL_VGA_TEXT], 04F4Ch
    cli
lapic_callback_missing_hang:
    hlt
    jmp lapic_callback_missing_hang

    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0

int22_handler:
    cli
    push 0
    push 022h
    jmp lapic_interrupt_common
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0

int23_handler:
    cli
    push 0
    push 023h
    jmp lapic_interrupt_common
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0

; Vectors 0x24 through 0x30 remain masked and unassigned.
repeat 13
    cli
    push eax
    mov dword [END_OF_INTERRUPT], 0
    pop eax
    iretd
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0
end repeat

int31_handler:
    cli
    push 0
    push 031h
    cld
    push ds
    push es
    push fs
    push gs
    pushad

    mov ax, INTERRUPT_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, [ps2_keyboard_interrupt_ptr]
    test eax, eax
    jz ps2_keyboard_callback_missing

    mov ebx, esp
    and esp, -16
    sub esp, 12
    push ebx
    call eax
    mov esp, ebx

    ; The driver has consumed port 0x60, so the source is safe to acknowledge.
    mov dword [END_OF_INTERRUPT], 0

    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 8
    iretd

ps2_keyboard_callback_missing:
    mov word [KERNEL_VGA_TEXT], 04F4Bh
    cli
ps2_keyboard_callback_missing_hang:
    hlt
    jmp ps2_keyboard_callback_missing_hang

    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0

; Vectors 0x32 through 0x3F remain masked and unassigned.
repeat 14
    cli
    push eax
    mov dword [END_OF_INTERRUPT], 0
    pop eax
    iretd
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0
end repeat

int40_handler:
    cli
    push 0
    push 040h
    jmp usb_interrupt_common

usb_interrupt_common:
    cld
    push ds
    push es
    push fs
    push gs
    pushad
    mov ax, INTERRUPT_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, [usb_host_interrupt_ptr]
    test eax, eax
    jz usb_callback_missing
    mov ebx, esp
    and esp, -16
    sub esp, 12
    push ebx
    call eax
    mov esp, ebx
    ; C services every UHCI on the line and acknowledges USBSTS first.
    mov dword [END_OF_INTERRUPT], 0
    call service_pending_cursor
    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 8
    iretd

usb_callback_missing:
    cli
    hlt
    jmp usb_callback_missing
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0

repeat 7
    cli
    push 0
    push 040h + %
    jmp usb_interrupt_common
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0
end repeat

; Vectors 0x48 through 0xFE remain masked and unassigned.
repeat 183
    cli
    push eax
    mov dword [END_OF_INTERRUPT], 0
    pop eax
    iretd
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0
end repeat

intFF_handler:
    cli
    push 0
    push 0FFh
    jmp lapic_interrupt_common
    times (1000h - (($ - $$) and 0FFFh)) and 0FFFh db 0

scheduler_task_count      dd 0
scheduler_current_index   dd 0FFFFFFFFh

scheduler_far_ptr:
    dd 0                    ; offset, ignored for TSS task switch
    dw 0                    ; selector
    dw 0                    ; keep task table dword-aligned

scheduler_task_table:
    times MAX_TASKS dw 0

scheduler_kernel_tss_selector:
    dd 0

scheduler_reap_process:
    dd 0
