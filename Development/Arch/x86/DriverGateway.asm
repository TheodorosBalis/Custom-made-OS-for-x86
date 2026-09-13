; Kernel-side integration, included after the C payload reservation.

kernel_idle_service:
    mov eax, [kernel_idle_service_ptr]
    test eax, eax
    jz kernel_idle_service_done
    push ebp
    mov ebp, esp
    and esp, -16
    call eax                    ; kernel_background_service(), installed by kernel_main
    mov esp, ebp
    pop ebp
kernel_idle_service_done:
    ret

driver_patch_gateway:
    mov ax, [KEYBOARD_GATE_SELECTOR]
    mov [KERNEL_TEMP_PAGE + (user_gateway_keyboard_pointer - user_syscall_gateway_blob) + 4], ax
    mov ax, [GRAPHICS_GATE_SELECTOR]
    mov [KERNEL_TEMP_PAGE + (user_gateway_graphics_pointer - user_syscall_gateway_blob) + 4], ax
    mov ax, [MOUSE_GATE_SELECTOR]
    mov [KERNEL_TEMP_PAGE + (user_gateway_mouse_pointer - user_syscall_gateway_blob) + 4], ax
    mov ax, [AZALIA_GATE_SELECTOR]
    mov [KERNEL_TEMP_PAGE + (user_gateway_audio_pointer - user_syscall_gateway_blob) + 4], ax
    ret

; ESI = process, EDI = TSS. Each allocated slot owns a distinct 8 KiB stack.
keyboard_prepare_process:
    pushad
    cmp dword [KEYBOARD_READY], 1
    jne keyboard_prepare_process_done
    mov eax, [esi + PROC_SLOT_INDEX]
    cmp eax, PROCESS_MAX_COUNT
    jae keyboard_prepare_process_done
    shl eax, 13
    add eax, 10000h
    push edi
    lea edi, [eax + 70000000h]
    mov ecx, 2000h / 4
    push eax
    xor eax, eax
    cld
    rep stosd
    pop eax
    pop edi
    add eax, 2000h
    mov [edi + TSS_ESP2], eax
    mov ax, [KEYBOARD_DATA_SELECTOR]
    mov [edi + TSS_SS2], ax
keyboard_prepare_process_done:
    popad
    ret

; ESI = process, EDI = TSS. Graphics owns one 8 KiB ring-1 stack per slot.
graphics_prepare_process:
    pushad
    cmp dword [GRAPHICS_READY], 1
    je graphics_prepare_process_graphics
    call azalia_prepare_process
    jmp graphics_prepare_process_done
graphics_prepare_process_graphics:
    mov eax, [esi + PROC_SLOT_INDEX]
    cmp eax, PROCESS_MAX_COUNT
    jae graphics_prepare_process_done
    shl eax, 13
    add eax, 20000h
    push edi
    lea edi, [eax + 71000000h]
    mov ecx, 2000h / 4
    push eax
    xor eax, eax
    cld
    rep stosd
    pop eax
    pop edi
    add eax, 2000h
    mov [edi + TSS_ESP1], eax
    mov ax, [GRAPHICS_DATA_SELECTOR]
    mov [edi + TSS_SS1], ax
graphics_prepare_process_done:
    popad
    ret

; ESI = process. Revoke ownership before its TSS selector can be recycled.
graphics_release_process:
    push eax
    cmp dword [GRAPHICS_READY], 1
    jne graphics_release_process_done
    mov eax, [esi + PROC_TSS_SELECTOR]
    test eax, eax
    jz graphics_release_process_done
    cmp eax, [71008000h]
    jne graphics_release_process_done
    mov dword [71008000h], 0
graphics_release_process_done:
    pop eax
    ret

; Copied read-only into each protected process at 0x7FFFF000.
INTEL915_INITIALIZE_DESKTOP = 0 ; intel915_driver_service() operation in Intel915.c

user_syscall_gateway_blob:
    push edx
    cmp eax, 13
    ja user_gateway_unknown
    mov edx, eax
    jmp dword [cs:USER_SYSCALL_GATEWAY_PAGE + (user_gateway_table - user_syscall_gateway_blob) + edx*4]

user_gateway_unknown:
    pop edx
    mov eax, 0FFFFFFFEh
    retf

user_gateway_audio:
    pop edx
    mov eax, ebx
    push ebx
    mov ebx, USER_SYSCALL_GATEWAY_PAGE + (user_gateway_audio_pointer - user_syscall_gateway_blob)
    cmp word [cs:ebx + 4], 0
    je user_gateway_audio_missing
    call far [cs:ebx]          ; azalia_gate_entry -> azalia_driver_service
    pop ebx
    retf
user_gateway_audio_missing:
    pop ebx
    xor eax, eax              ; AZALIA_ABSENT
    retf

user_gateway_keyboard:
    pop edx
    push ebx
    push edx
    mov edx, ebx                ; internal gate ABI: EDX = key, EBX = far pointer
    mov ebx, keyboard_gate_entry_ptr
    sub eax, 2                  ; public request 2/3 -> driver operation 0/1
    call far [cs:ebx]            ; keyboard_gate_entry -> keyboard_driver_service
    pop edx
    pop ebx
    retf

user_gateway_graphics:
    pop edx
    push ebx
    mov ebx, intel915_gate_entry_ptr
    cmp word [cs:ebx + 4], 0
    je user_gateway_graphics_missing
    cmp eax, 4
    je user_gateway_graphics_initialize
    sub eax, 10
    jmp user_gateway_graphics_invoke
user_gateway_graphics_initialize:
    mov eax, INTEL915_INITIALIZE_DESKTOP
user_gateway_graphics_invoke:
    call far [cs:ebx]            ; intel915_gate_entry -> intel915_driver_service
    pop ebx
    retf
user_gateway_graphics_missing:
    pop ebx
    mov eax, 0FFFFFFFEh
    retf

user_gateway_mouse:
    pop edx
    push ebx
    mov ebx, mouse_gate_entry_ptr
    cmp word [cs:ebx + 4], 0
    je user_gateway_mouse_missing
    sub eax, 8
    call far [cs:ebx]
    pop ebx
    retf
user_gateway_mouse_missing:
    pop ebx
    xor eax, eax
    retf

user_gateway_kernel:
    pop edx
    push ebp
    push eax
    mov ax, ss
    movzx ebp, ax
    pop eax
    mov ecx, esp
    sub esp, 8
    mov dword [ss:esp], ecx
    mov word [ss:esp + 4], bp
    mov word [ss:esp + 6], 0
    mov ecx, esp
    mov edx, USER_SYSCALL_GATEWAY_RETURN
    sysenter

user_syscall_gateway_blob_return:
    lss esp, [ss:esp]
    pop ebp
    retf

align 4
user_gateway_table:
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_unknown - user_syscall_gateway_blob)
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_kernel - user_syscall_gateway_blob)
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_keyboard - user_syscall_gateway_blob)
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_keyboard - user_syscall_gateway_blob)
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_graphics - user_syscall_gateway_blob)
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_kernel - user_syscall_gateway_blob) ; 5: disk info
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_kernel - user_syscall_gateway_blob) ; 6: read sector
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_kernel - user_syscall_gateway_blob) ; 7: device mask
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_mouse - user_syscall_gateway_blob) ; 8: next event
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_mouse - user_syscall_gateway_blob) ; 9: buttons
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_mouse - user_syscall_gateway_blob) ; 10: mouse mask
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_graphics - user_syscall_gateway_blob) ; 11: screen size
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_unknown - user_syscall_gateway_blob) ; 12: removed cursor API
    dd USER_SYSCALL_GATEWAY_PAGE + (user_gateway_audio - user_syscall_gateway_blob) ; 13: audio query
user_gateway_keyboard_pointer:
    dd 0                        ; offset ignored when the selector names a call gate
    dw 0                        ; patched gate targets keyboard_gate_entry, driver CS:0
user_gateway_graphics_pointer:
    dd 0                        ; offset ignored when the selector names a call gate
    dw 0                        ; patched gate targets intel915_gate_entry, driver CS:0
user_gateway_mouse_pointer:
    dd 0
    dw 0
user_gateway_audio_pointer:
    dd 0
    dw 0
user_syscall_gateway_blob_end:

; Define after the blob so both endpoint labels are resolved in this pass.
keyboard_gate_entry_ptr = USER_SYSCALL_GATEWAY_PAGE + (user_gateway_keyboard_pointer - user_syscall_gateway_blob)
intel915_gate_entry_ptr = USER_SYSCALL_GATEWAY_PAGE + (user_gateway_graphics_pointer - user_syscall_gateway_blob)
mouse_gate_entry_ptr = USER_SYSCALL_GATEWAY_PAGE + (user_gateway_mouse_pointer - user_syscall_gateway_blob)
