; Included in the high kernel. Audio uses flat ring-1 CS/DS, unlike graphics.
AZALIA_GATE_SELECTOR = 080002368h
AZALIA_FLAT_DATA = 08000236Ch
azalia_driver_service_ptr = 080002370h

azalia_gate_entry:
    pushfd
    push ds
    push es
    push fs
    push gs
    pushad
    mov ebp, esp
    push ss
    push ebp                   ; Original SS:ESP for restoring the gate frame.
    movzx edx, word [cs:AZALIA_FLAT_DATA]
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx
    cld
    mov ax, ss
    movzx eax, ax
    and eax, 0FFF8h
    mov ecx, [030010000h + eax]
    mov eax, [030010004h + eax]
    shr ecx, 16
    mov ebx, eax
    and ebx, 0FFh
    shl ebx, 16
    and eax, 0FF000000h
    or eax, ebx
    or eax, ecx                ; Current SS base, possibly the graphics arena.
    lea ebx, [esp + eax]
    push edx
    push ebx
    lss esp, [ss:esp]          ; Same physical stack, now addressed through flat SS.
    and esp, -16
    sub esp, 12
    push dword [ebx + 36]      ; Saved EAX: audio operation.
    call dword [azalia_driver_service_ptr]
    mov esp, ebx
    mov [esp + 36], eax
    lss esp, [esp]
    popad
    pop gs
    pop fs
    pop es
    pop ds
    popfd
    retf

; Used only when graphics has not already supplied the process's SS1:ESP1.
azalia_prepare_process:
    cmp dword [AZALIA_GATE_SELECTOR], 0
    je azalia_prepare_process_done
    mov eax, [esi + PROC_SLOT_INDEX]
    cmp eax, PROCESS_MAX_COUNT
    jae azalia_prepare_process_done
    shl eax, 13
    add eax, 72020000h
    push edi
    mov edi, eax
    mov ecx, 2000h / 4
    xor eax, eax
    cld
    rep stosd
    mov eax, edi
    pop edi
    mov [edi + TSS_ESP1], eax
    mov ax, [AZALIA_FLAT_DATA]
    mov [edi + TSS_SS1], ax
azalia_prepare_process_done:
    ret
