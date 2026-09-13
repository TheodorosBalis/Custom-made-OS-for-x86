ORG 0x80000000

use32

; High-half kernel stage.
; This code is meant to execute only after paging maps its physical load
; address into the 0x80000000 + virtual range.

    ; TODO:
    ; high-half kernel code starts here

kernel_high_entry:
    cli
    call pmm_init
    call map_kernel_vga
    jc kernel_high_hang
    call heap_init
    call heap_validate
    call gdt_allocator_init
    call process_slot_allocator_init
    call process_table_init
    call scheduler_init
    call create_kernel_tss
    jc kernel_high_hang
    call enable_systemcall
    jc kernel_high_hang
    call enable_VME
    jc kernel_high_hang

    ; Enter the freestanding C layer with a 16-byte-aligned caller stack.
    push ebx
    mov ebx, esp
    and esp, 0FFFFFFF0h
    call kernel_main
    mov esp, ebx
    pop ebx

    sti

kernel_high_scheduler_wait:
    call kernel_idle_service
    hlt
    jmp kernel_high_scheduler_wait

kernel_high_hang:
    cli
    pause
    hlt
    jmp kernel_high_hang

VGA_TEXT_PHYS          = 000B8000h
KERNEL_VGA_VIRT        = 80B80000h
VGA_FONT_PHYS          = 000A0000h
KERNEL_VGA_FONT_VIRT   = 80A00000h
VGA_TEXT_CELL_COUNT    = 80 * 25
VGA_TEST_ATTRIBUTE     = 01Fh

C_KERNEL_API_WRAPPERS  = 80002100h
C_KERNEL_API_TABLE     = 80002200h
; Addresses of runtime-published function pointers, not the C code itself.
exception_dispatch_ptr       = 80002320h
lapic_interrupt_dispatch_ptr = 80002324h
ps2_keyboard_interrupt_ptr   = 80002328h
KEYBOARD_GATE_SELECTOR = 8000232Ch
KEYBOARD_DATA_SELECTOR = 80002330h
KEYBOARD_CODE_SELECTOR = 80002334h
KEYBOARD_READY         = 80002338h
GRAPHICS_GATE_SELECTOR = 8000233Ch
GRAPHICS_DATA_SELECTOR = 80002340h
GRAPHICS_CODE_SELECTOR = 80002344h
GRAPHICS_READY         = 80002348h
storage_syscall_ptr    = 8000234Ch ; Runtime-published Storage.c entry point.
usb_host_interrupt_ptr = 80002350h ; UsbHost.c callback.
usb_host_status_ptr    = 80002354h ; Pointer to kernel-only UHCI diagnostics.
MOUSE_GATE_SELECTOR   = 80002358h
MOUSE_READY           = 8000235Ch
kernel_idle_service_ptr = 80002360h
C_KERNEL_OFFSET        = 00002400h
kernel_main            = 80000000h + C_KERNEL_OFFSET ; KernelMain.c, placed by kernel-c.ld
C_KERNEL_LIMIT         = 80010000h
KERNEL_API_MAGIC       = 4B415049h
KERNEL_API_VERSION     = 1

SYSCALL_VGA_WRITE      = 1

USER_SYSCALL_GATEWAY_PAGE   = 07FFFF000h
USER_SYSCALL_GATEWAY_ENTRY  = USER_SYSCALL_GATEWAY_PAGE
USER_SYSCALL_GATEWAY_RETURN = USER_SYSCALL_GATEWAY_PAGE + (user_syscall_gateway_blob_return - user_syscall_gateway_blob)

; Install a supervisor-only VGA alias before creating process directories.
; create_page_directory copies this kernel mapping into every process CR3.
map_kernel_vga:
    push PAGE_PRESENT_RW
    push VGA_TEXT_PHYS
    push KERNEL_VGA_VIRT
    call map_page
    jc map_kernel_vga_done

    ; Plane 2 stores 256 glyphs at 32 bytes each, spanning two pages.
    push PAGE_PRESENT_RW
    push VGA_FONT_PHYS
    push KERNEL_VGA_FONT_VIRT
    call map_page
    jc map_kernel_vga_done

    push PAGE_PRESENT_RW
    push VGA_FONT_PHYS + PAGE_SIZE
    push KERNEL_VGA_FONT_VIRT + PAGE_SIZE
    call map_page

map_kernel_vga_done:
    ret

; The gateway blob is defined in DriverGateway.asm below the C reservation.

;============Process create stuff=========

PROCESS_TYPE_PROTECTED32 = 0
PROCESS_TYPE_V86         = 1

; process_create(process_type)
; [ebp+8] = PROCESS_TYPE_PROTECTED32 or PROCESS_TYPE_V86
; returns:
;   EAX = process object pointer, CF clear on success
;   EAX = 0, CF set on failure
process_create:
    push ebp
    mov ebp, esp

    mov eax, [ebp + 8]
    cmp eax, PROCESS_TYPE_PROTECTED32
    je process_create_protected32_call
    cmp eax, PROCESS_TYPE_V86
    je process_create_v86_call

    xor eax, eax
    stc
    jmp process_create_dispatch_done

process_create_protected32_call:
    call process_create_protected32
    jmp process_create_dispatch_done

process_create_v86_call:
    call v86_process_create

process_create_dispatch_done:
    pop ebp
    ret 4

; Internal constructor for the existing paged, LDT-based ring-3 process.
process_create_protected32:
    push ebx
    push esi

    call process_memory_create
    jc process_create_fail
    test eax, eax
    jz process_create_fail

    mov esi, eax
    mov dword [esi + PROC_TYPE], PROCESS_TYPE_PROTECTED32

    call process_slot_alloc
    jc process_create_destroy_memory

    mov [esi + PROC_SLOT_INDEX], ebx
    mov [esi + PROC_SLOT_BASE], eax

    push eax                    ; process slot/linear base
    push esi                    ; process object
    call process_create_random_ldt
    test eax, eax
    jz process_create_free_slot

    push esi
    call process_create_tss
    jc process_create_destroy_process

    push esi
    call process_install_syscall_gateway
    test eax, eax
    jz process_create_destroy_process

    push esi
    call process_register
    test eax, eax
    jz process_create_destroy_process

    push esi
    call scheduler_add_process
    test eax, eax
    jz process_create_destroy_process

    mov eax, esi
    pop esi
    pop ebx
    clc
    ret

process_create_destroy_process:
    push esi
    call process_destroy
    jmp process_create_fail

process_create_free_slot:
    push dword [esi + PROC_SLOT_INDEX]
    call process_slot_free

process_create_destroy_memory:
    push esi
    call process_memory_destroy

process_create_fail:
    xor eax, eax
    pop esi
    pop ebx
    stc
    ret

; Internal constructor for a private-address-space VM86 task.
v86_process_create:
    push esi

    call v86_process_memory_create
    jc v86_process_create_fail
    test eax, eax
    jz v86_process_create_fail

    mov esi, eax

    push esi
    call v86_process_create_tss
    jc v86_process_create_destroy

    push esi
    call process_register
    test eax, eax
    jz v86_process_create_destroy

    push esi
    call scheduler_add_process
    test eax, eax
    jz v86_process_create_destroy

    mov eax, esi
    pop esi
    clc
    ret

v86_process_create_destroy:
    push esi
    call process_destroy

v86_process_create_fail:
    xor eax, eax
    pop esi
    stc
    ret

; process_destroy(process)
; [ebp+8] = process object pointer
process_destroy:
    push ebp
    mov ebp, esp
    push eax
    push esi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_destroy_done

    ; A running hardware task cannot destroy its own TSS. Removal
    ; succeeds immediately when the process was never made runnable.
    push esi
    call scheduler_remove_process
    test eax, eax
    jz process_destroy_done

    push esi
    call process_unregister

    call graphics_release_process
    mov eax, [esi + PROC_TSS_SELECTOR]
    test eax, eax
    jz process_destroy_skip_tss_selector
    push eax
    call gdt_free_selector
    mov dword [esi + PROC_TSS_SELECTOR], 0

process_destroy_skip_tss_selector:
    mov eax, [esi + PROC_TSS_BASE]
    test eax, eax
    jz process_destroy_skip_tss_base
    push eax
    call heap_free
    mov dword [esi + PROC_TSS_BASE], 0

process_destroy_skip_tss_base:
    mov eax, [esi + PROC_KSTACK_TOP]
    test eax, eax
    jz process_destroy_skip_kstack
    sub eax, PAGE_SIZE
    push eax
    call heap_free
    mov dword [esi + PROC_KSTACK_TOP], 0

process_destroy_skip_kstack:
    mov eax, [esi + PROC_LDT_SELECTOR]
    test eax, eax
    jz process_destroy_skip_ldt_selector
    push eax
    call gdt_free_selector
    mov dword [esi + PROC_LDT_SELECTOR], 0

process_destroy_skip_ldt_selector:
    mov eax, [esi + PROC_LDT_BITMAP]
    test eax, eax
    jz process_destroy_skip_ldt_bitmap
    push eax
    call heap_free
    mov dword [esi + PROC_LDT_BITMAP], 0

process_destroy_skip_ldt_bitmap:
    mov eax, [esi + PROC_LDT_BASE]
    test eax, eax
    jz process_destroy_skip_ldt_base
    push eax
    call heap_free
    mov dword [esi + PROC_LDT_BASE], 0

process_destroy_skip_ldt_base:
    mov eax, [esi + PROC_SLOT_INDEX]
    cmp eax, 0FFFFFFFFh
    je process_destroy_skip_slot
    push eax
    call process_slot_free
    mov dword [esi + PROC_SLOT_INDEX], 0FFFFFFFFh
    mov dword [esi + PROC_SLOT_BASE], 0

process_destroy_skip_slot:
    mov eax, [esi + PROC_PAGE_DIR]
    test eax, eax
    jz process_destroy_skip_memory
    push eax
    call destroy_page_directory
    mov dword [esi + PROC_PAGE_DIR], 0

process_destroy_skip_memory:
    push esi
    call heap_free

process_destroy_done:
    pop esi
    pop eax
    pop ebp
    ret 4


process_slot_allocator_init:
    push eax
    push ecx
    push edi

    mov edi, process_slot_bitmap
    xor eax, eax
    mov ecx, PROCESS_SLOT_BITMAP_SIZE / 4
    cld
    rep stosd

    pop edi
    pop ecx
    pop eax
    ret
; process_slot_alloc()
; returns:
;   EAX = slot linear base
;   EBX = slot index
;   CF clear on success
; failure:
;   EAX = 0
;   EBX = 0
;   CF set
process_slot_alloc:
    push ecx
    push edx

    xor ecx, ecx

process_slot_alloc_scan:
    cmp ecx, PROCESS_MAX_COUNT
    jae process_slot_alloc_fail

    bt dword [process_slot_bitmap], ecx
    jnc process_slot_alloc_found

    inc ecx
    jmp process_slot_alloc_scan

process_slot_alloc_found:
    bts dword [process_slot_bitmap], ecx

    mov ebx, ecx
    mov eax, PROCESS_SLOT_SIZE
    mul ecx
    add eax, PROCESS_AREA_BASE

    pop edx
    pop ecx
    clc
    ret

process_slot_alloc_fail:
    xor eax, eax
    xor ebx, ebx

    pop edx
    pop ecx
    stc
    ret

; process_slot_free(slot_index)
; [ebp+8] = slot index
process_slot_free:
    push ebp
    mov ebp, esp
    push eax

    mov eax, [ebp + 8]
    cmp eax, PROCESS_MAX_COUNT
    jae process_slot_free_done

    btr dword [process_slot_bitmap], eax

process_slot_free_done:
    pop eax
    pop ebp
    ret 4


; process_prepare_test_task(process, source, size)
; [ebp+8]  = process object
; [ebp+12] = kernel address of position-independent user code
; [ebp+16] = code size, from 1 through PAGE_SIZE bytes
; returns EAX = 1 on success, EAX = 0 on failure
process_prepare_test_task:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_prepare_test_task_fail

    mov eax, [ebp + 12]
    test eax, eax
    jz process_prepare_test_task_fail

    mov eax, [ebp + 16]
    test eax, eax
    jz process_prepare_test_task_fail
    cmp eax, PAGE_SIZE
    ja process_prepare_test_task_fail

    ; Resolve the logical TSS entry through the process's actual LDT CS.
    mov edi, [esi + PROC_TSS_BASE]
    test edi, edi
    jz process_prepare_test_task_fail

    mov eax, [edi + TSS_EIP]
    cmp eax, USER_CODE_SIZE
    jae process_prepare_test_task_fail

    movzx ecx, word [edi + TSS_CS]
    test ecx, 4                    ; selector must reference the LDT
    jz process_prepare_test_task_fail

    and ecx, 0FFF8h                ; selector converted to LDT byte offset
    cmp ecx, LDT_SIZE - 8
    ja process_prepare_test_task_fail

    mov edi, [esi + PROC_LDT_BASE]
    test edi, edi
    jz process_prepare_test_task_fail
    add edi, ecx

    test byte [edi + 5], 080h      ; present descriptor
    jz process_prepare_test_task_fail
    test byte [edi + 5], 008h      ; executable code descriptor
    jz process_prepare_test_task_fail

    xor edx, edx
    mov dx, [edi + 2]
    movzx eax, byte [edi + 4]
    shl eax, 16
    or edx, eax
    movzx eax, byte [edi + 7]
    shl eax, 24
    or edx, eax                    ; EDX = LDT CS linear base

    mov edi, [esi + PROC_TSS_BASE]
    add edx, [edi + TSS_EIP]       ; EDX = exact linear entry address
    jc process_prepare_test_task_fail
    cmp edx, USER_SPACE_LIMIT
    jae process_prepare_test_task_fail

    mov eax, edx
    and eax, PAGE_SIZE - 1
    add eax, [ebp + 16]
    cmp eax, PAGE_SIZE             ; test image currently occupies one page
    ja process_prepare_test_task_fail

    ; Allocate and initialize the page containing the TSS entry point.
    call pmm_alloc_page
    test eax, eax
    jz process_prepare_test_task_fail
    mov ebx, eax

    push PAGE_PRESENT_RW
    push ebx
    push KERNEL_TEMP_PAGE
    call map_page
    jc process_prepare_test_task_free_code_phys

    mov edi, KERNEL_TEMP_PAGE
    xor eax, eax
    mov ecx, PAGE_SIZE / 4
    cld
    rep stosd

    mov esi, [ebp + 12]
    mov edi, KERNEL_TEMP_PAGE
    mov eax, edx
    and eax, PAGE_SIZE - 1
    add edi, eax
    mov ecx, [ebp + 16]
    rep movsb

    push KERNEL_TEMP_PAGE
    push PAGE_DIR_BASE
    call unmap_page
    jc process_prepare_test_task_free_code_phys

    mov esi, [ebp + 8]
    mov eax, edx
    and eax, PAGE_ADDR_MASK

    push PAGE_PRESENT_RW_USER or PAGE_OWNED
    push ebx
    push eax
    push dword [esi + PROC_PAGE_DIR]
    call map_user_page
    jc process_prepare_test_task_free_code_phys

    ; Allocate and clear the page immediately below logical SS:ESP.
    call pmm_alloc_page
    test eax, eax
    jz process_prepare_test_task_fail
    mov ebx, eax

    push PAGE_PRESENT_RW
    push ebx
    push KERNEL_TEMP_PAGE
    call map_page
    jc process_prepare_test_task_free_stack_phys

    mov edi, KERNEL_TEMP_PAGE
    xor eax, eax
    mov ecx, PAGE_SIZE / 4
    cld
    rep stosd

    push KERNEL_TEMP_PAGE
    push PAGE_DIR_BASE
    call unmap_page
    jc process_prepare_test_task_free_stack_phys

    mov esi, [ebp + 8]
    mov eax, [esi + PROC_SLOT_BASE]
    add eax, USER_STACK_OFFSET + USER_STACK_SIZE - PAGE_SIZE

    push PAGE_PRESENT_RW_USER or PAGE_OWNED
    push ebx
    push eax
    push dword [esi + PROC_PAGE_DIR]
    call map_user_page
    jc process_prepare_test_task_free_stack_phys

    mov eax, 1
    jmp process_prepare_test_task_done

process_prepare_test_task_free_stack_phys:
    push ebx
    call pmm_free_page
    jmp process_prepare_test_task_fail

process_prepare_test_task_free_code_phys:
    push ebx
    call pmm_free_page

process_prepare_test_task_fail:
    xor eax, eax

process_prepare_test_task_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 12

; process_install_syscall_gateway(process)
; [ebp+8] = process object pointer
; returns EAX = 1 on success, EAX = 0 on failure
process_install_syscall_gateway:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push esi
    push edi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_install_syscall_gateway_fail

    mov eax, [esi + PROC_PAGE_DIR]
    test eax, eax
    jz process_install_syscall_gateway_fail

    call pmm_alloc_page
    test eax, eax
    jz process_install_syscall_gateway_fail
    mov ebx, eax

    push PAGE_PRESENT_RW
    push ebx
    push KERNEL_TEMP_PAGE
    call map_page
    jc process_install_syscall_gateway_free_phys

    mov edi, KERNEL_TEMP_PAGE
    xor eax, eax
    mov ecx, PAGE_SIZE / 4
    cld
    rep stosd

    mov esi, user_syscall_gateway_blob
    mov edi, KERNEL_TEMP_PAGE
    mov ecx, user_syscall_gateway_blob_end - user_syscall_gateway_blob
    rep movsb

    call driver_patch_gateway

    push KERNEL_TEMP_PAGE
    push PAGE_DIR_BASE
    call unmap_page
    jc process_install_syscall_gateway_fail

    mov esi, [ebp + 8]
    push PAGE_PRESENT_USER or PAGE_OWNED
    push ebx
    push USER_SYSCALL_GATEWAY_PAGE
    push dword [esi + PROC_PAGE_DIR]
    call map_user_page
    jc process_install_syscall_gateway_free_phys

    mov eax, 1
    jmp process_install_syscall_gateway_done

process_install_syscall_gateway_free_phys:
    push ebx
    call pmm_free_page

process_install_syscall_gateway_fail:
    xor eax, eax

process_install_syscall_gateway_done:
    pop edi
    pop esi
    pop ecx
    pop ebx
    pop ebp
    ret 4


; ======== function for ensuring kernel heap is stable =======

test_heap:
    push 0x100
    call heap_alloc
    test eax,eax
    jz heap_failed
    mov edi,eax
    mov byte [edi],'A'
    mov byte [edi+0xFF],'Z'
    push edi
    call heap_free
    push 80h
    call heap_alloc
    test eax,eax
    jz heap_failed
    cmp eax,edi
    jne heap_failed
    push 900h
    call heap_alloc
    test eax,eax
    jz heap_failed
    push 03000h
    call heap_alloc
    test eax,eax
    jz heap_failed
    mov byte [eax],'B'
    mov byte [eax+08ffh],'Y'
    push heap_success
    call ptext
    ret

heap_failed:
    push heap_failure
    call ptext
    ret

; ================= SYSENTER ring-3 system-call entry =================

IA32_SYSENTER_CS       = 174h
IA32_SYSENTER_ESP      = 175h
IA32_SYSENTER_EIP      = 176h
CPUID_FEATURE_SEP      = 1 shl 11
CPUID_FEATURE_VME      = 1 shl 1
CR4_VME                = 1 shl 0
KERNEL_DS              = 010h
SYSENTER_KERNEL_CS     = 028h
SYSENTER_KERNEL_DS     = 030h
SYSEXIT_USER_CS        = 03Bh
SYSEXIT_USER_SS        = 043h

sysenter_boot_stack_base dd 0

; Ring-3 gateway SYSENTER convention:
;   EBP = current process LDT SS selector
;   ECX = logical address of an m16:32 LSS frame on that stack
;   EDX = gateway return address (replaced with the trusted value below)
; Other registers are preserved for now; EAX is returned as zero.
;
; IA32_SYSENTER_ESP points to a shared single-core trampoline stack. This
; entry immediately moves the saved state onto the current task's private
; TSS.ESP0 stack, validates the user gateway frame, and converts only its
; segmented stack address to the flat address required by SYSEXIT.
sysenter_entry:
    cli
    cld

    ; Save user registers on the short-lived trampoline stack.
    pushad

    mov ax, SYSENTER_KERNEL_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Locate the currently running 32-bit TSS through its GDT descriptor.
    str ax
    movzx ebx, ax
    and ebx, 0FFF8h
    add ebx, GDT_BASE

    movzx edi, word [ebx + 2]
    movzx eax, byte [ebx + 4]
    shl eax, 16
    or edi, eax
    movzx eax, byte [ebx + 7]
    shl eax, 24
    or edi, eax

    mov ebp, edi                  ; current TSS base

    ; Copy the PUSHAD image to the process's private kernel stack.
    mov esi, esp
    mov ebx, [ebp + TSS_ESP0]
    sub ebx, 32
    mov edi, ebx
    mov ecx, 8
    rep movsd
    mov esp, ebx

    ; Process TSS objects carry a kernel-only owner pointer immediately after
    ; the architectural 0x68-byte TSS. The descriptor limit remains 0x67.
    mov esi, [ebp + TSS_OWNER_PROCESS]
    test esi, esi
    jz sysenter_entry_bad_frame

    ; The gateway's saved SS must be this process's randomized LDT stack.
    mov eax, [esp + 8]             ; PUSHAD saved EBP
    test eax, 0FFFF0000h
    jnz sysenter_entry_bad_frame

    movzx ecx, word [esi + PROC_USER_SS]
    cmp ax, cx
    jne sysenter_entry_bad_frame

    and eax, 7                     ; TI=1 and RPL=3
    cmp eax, 7
    jne sysenter_entry_bad_frame

    ; The frame is an offset within the process's fixed 1 MiB stack segment.
    mov eax, [esp + 24]            ; PUSHAD saved ECX
    mov ecx, eax
    add ecx, 7
    jc sysenter_entry_bad_frame
    cmp ecx, USER_STACK_SIZE
    jae sysenter_entry_bad_frame

    mov ebx, [esi + PROC_SLOT_BASE]
    add ebx, USER_STACK_OFFSET
    jc sysenter_entry_bad_frame
    add eax, ebx                   ; flat address used temporarily by SS=0x43
    jc sysenter_entry_bad_frame
    mov ebx, eax

    mov ecx, ebx
    add ecx, 7
    jc sysenter_entry_bad_frame
    cmp ecx, USER_SPACE_LIMIT
    jae sysenter_entry_bad_frame

    mov ecx, ebx
    and ecx, PAGE_SIZE - 1
    cmp ecx, PAGE_SIZE - 8
    ja sysenter_entry_bad_frame

    ; Refuse an unmapped frame before dereferencing it in ring 0.
    push ebx
    push dword [esi + PROC_PAGE_DIR]
    call get_physical_address
    test eax, eax
    jz sysenter_entry_bad_frame

    ; The LSS frame must restore ESP just above itself and the same LDT SS.
    mov eax, [esp + 24]
    add eax, 8
    cmp dword [ebx], eax
    jne sysenter_entry_bad_frame

    movzx eax, word [ebx + 4]
    movzx ecx, word [esi + PROC_USER_SS]
    cmp eax, ecx
    jne sysenter_entry_bad_frame
    cmp word [ebx + 6], 0
    jne sysenter_entry_bad_frame

    mov [esp + 24], ebx            ; SYSEXIT ECX = flat gateway-frame address
    mov dword [esp + 20], USER_SYSCALL_GATEWAY_RETURN

    movzx eax, word [esi + PROC_USER_DS]
    mov [esp + 12], eax           ; PUSHAD's ignored saved-ESP slot

    cmp dword [esp + 28], 5
    jb sysenter_entry_not_storage
    cmp dword [esp + 28], 7
    ja sysenter_entry_unknown_syscall
    mov eax, [storage_syscall_ptr]
    test eax, eax
    jz sysenter_entry_unknown_syscall
    mov ebx, esp                 ; PUSHAD image, preserved by the C ABI.
    and esp, 0FFFFFFF0h
    sub esp, 8
    push esi                     ; Trusted current process object.
    push ebx
    call eax                     ; storage_syscall(registers, process), CPL0.
    mov esp, ebx
    mov [esp + 28], eax
    jmp sysenter_entry_dispatch_done

sysenter_entry_not_storage:
    ; Dispatch EAX=1: write BL to VGA text cell EDI through the supervisor
    ; alias. The low-memory linear address 0xB8000 stays unmapped.
    cmp dword [esp + 28], SYSCALL_VGA_WRITE
    jne sysenter_entry_unknown_syscall

    mov eax, [esp]                ; PUSHAD saved EDI = VGA cell index
    cmp eax, VGA_TEXT_CELL_COUNT
    jae sysenter_entry_invalid_argument
    shl eax, 1

    mov edx, [esp + 16]           ; PUSHAD saved EBX, DL = character
    mov byte [KERNEL_VGA_VIRT + eax], dl
    mov byte [KERNEL_VGA_VIRT + eax + 1], VGA_TEST_ATTRIBUTE
    mov dword [esp + 28], 0
    jmp sysenter_entry_dispatch_done

sysenter_entry_unknown_syscall:
    mov dword [esp + 28], 0FFFFFFFEh
    jmp sysenter_entry_dispatch_done

sysenter_entry_invalid_argument:
    mov dword [esp + 28], 0FFFFFFFFh

sysenter_entry_dispatch_done:

    ; The current process uses one user data selector for DS/ES/FS/GS.
    mov ax, [esp + 12]
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popad

    ; STI's one-instruction interrupt shadow lasts through SYSEXIT, so
    ; the process resumes with timer interrupts enabled.
    sti
    sysexit

sysenter_entry_bad_frame:
    cli
    hlt
    jmp sysenter_entry_bad_frame

; enable_systemcall()
; returns CF clear when SYSENTER is available and configured
; returns CF set when unsupported or the bootstrap stack cannot be allocated
enable_systemcall:
    push eax
    push ebx
    push ecx
    push edx

    mov eax, 1
    cpuid
    test edx, CPUID_FEATURE_SEP
    jz enable_systemcall_fail

    push PAGE_SIZE
    call heap_alloc
    test eax, eax
    jz enable_systemcall_fail
    mov [sysenter_boot_stack_base], eax

    xor edx, edx
    mov eax, SYSENTER_KERNEL_CS
    mov ecx, IA32_SYSENTER_CS
    wrmsr

    mov eax, [sysenter_boot_stack_base]
    add eax, PAGE_SIZE
    mov ecx, IA32_SYSENTER_ESP
    wrmsr

    mov eax, sysenter_entry
    mov ecx, IA32_SYSENTER_EIP
    wrmsr

    pop edx
    pop ecx
    pop ebx
    pop eax
    clc
    ret

enable_systemcall_fail:
    pop edx
    pop ecx
    pop ebx
    pop eax
    stc
    ret

; enable_VME()
; Enables virtual-8086 mode enhancements globally when CPUID reports them.
; Per-task IVT redirection and I/O permissions are configured in each VM86 TSS.
enable_VME:
    push eax
    push ebx
    push ecx
    push edx

    mov eax, 1
    cpuid
    test edx, CPUID_FEATURE_VME
    jz enable_VME_fail

    mov eax, cr4
    or eax, CR4_VME
    mov cr4, eax

    mov eax, cr4
    test eax, CR4_VME
    jz enable_VME_fail

    pop edx
    pop ecx
    pop ebx
    pop eax
    clc
    ret

enable_VME_fail:
    pop edx
    pop ecx
    pop ebx
    pop eax
    stc
    ret

;==========physical address manager + allocation==========

; ======== constants ========

PAGE_SIZE              = 1000h
PAGE_PRESENT_RW        = 003h
PAGE_ADDR_MASK         = 0FFFFF000h

MAIN_RAM_START         = 00100000h
MAIN_RAM_END           = 3F780000h

PMM_BITMAP_BASE        = 30400000h
PMM_BITMAP_SIZE        = 20000h

PAGE_TABLE_POOL_BASE   = 30500000h
PAGE_TABLE_POOL_END    = 30800000h

KHEAP_BASE             = 80400000h
KHEAP_MAX_SIZE         = 00400000h

;======= variables for kernel heap =========

HEAP_MAGIC      = 0C0DEF00Dh
HEAP_USED       = 1
HEAP_FREE       = 0
HEAP_HDR_SIZE   = 16
HEAP_MIN_SPLIT  = HEAP_HDR_SIZE + 8

heap_head       dd 0
heap_current    dd 0
heap_committed  dd 0
heap_limit      dd 0

; ========== PAGING info ==============
BOOTSTRAP_PHYS     = 0x30000000
HIGH_KERNEL_PHYS   = 0x34000000
PAGE_DIR_BASE      = 0x30030000
PDE_PRESENT_RW_PS  = 0x00000083
HIGH_KERNEL_VIRT   = 0x80000000

pmm_next_page          dd MAIN_RAM_START shr 12
page_table_next        dd PAGE_TABLE_POOL_BASE

;===== code ======

pmm_init:
    cld
    mov edi, PMM_BITMAP_BASE
    mov eax, 0FFFFFFFFh
    mov ecx, PMM_BITMAP_SIZE / 4
    rep stosd
    mov eax, MAIN_RAM_START
    mov ebx, MAIN_RAM_END
    push ebx
    push eax
    call pmm_mark_range_free

    mov eax, 30000000h
    mov ebx, 30800000h
    push ebx
    push eax
    call pmm_mark_range_used

    mov eax, 32000000h
    mov ebx, 32400000h
    push ebx
    push eax
    call pmm_mark_range_used

    mov eax, 34000000h
    mov ebx, 34400000h
    push ebx
    push eax
    call pmm_mark_range_used
    ret

    int3
    int3
; ====mark_free======
; EAX = start phys, EBX = end phys exclusive
pmm_mark_range_free:
    push ebp
    mov ebp,esp
    push ecx
    mov eax,[ebp+8]
    mov ebx,[ebp+12]
    mov ecx, eax
    shr ecx, 12
    shr ebx, 12
.pmm_free_next:
    cmp ecx, ebx
    jae .pmm_free_done
    btr dword [PMM_BITMAP_BASE], ecx
    inc ecx
    jmp .pmm_free_next
.pmm_free_done:
    pop ecx
    pop ebp
    ret 8

   int3
   int3

;==== mark_used=======
; EAX = start phys, EBX = end phys exclusive
pmm_mark_range_used:
    push ebp
    mov ebp,esp
    push ecx
    mov eax,dword [ebp+8]
    mov ebx,dword [ebp+12]
    mov ecx, eax
    shr ecx, 12
    shr ebx, 12
.pmm_used_next:
    cmp ecx, ebx
    jae .pmm_used_done
    bts dword [PMM_BITMAP_BASE], ecx
    inc ecx
    jmp .pmm_used_next
.pmm_used_done:
    pop ecx
    pop ebp
    ret 8

    int3
    int3
;===== allocate page =====

pmm_alloc_page:
    mov eax, [pmm_next_page]
.pmm_alloc_scan:
    cmp eax, MAIN_RAM_END shr 12
    jae .pmm_alloc_fail
    bt dword [PMM_BITMAP_BASE], eax
    jnc .pmm_alloc_found
    inc eax
    jmp .pmm_alloc_scan

.pmm_alloc_found:
    bts dword [PMM_BITMAP_BASE], eax
    mov [pmm_next_page], eax
    inc dword [pmm_next_page]
    shl eax, 12
    ret

.pmm_alloc_fail:
    xor eax, eax
    ret

    int3
    int3

;====== free_page======

; EAX = physical page
pmm_free_page:
    push ebp
    mov ebp,esp
    mov eax,dword [ebp+8]
    test eax, eax
    jz .pmm_free_page_done
    shr eax, 12
    btr dword [PMM_BITMAP_BASE], eax
    cmp eax, [pmm_next_page]
    jae .pmm_free_page_done
    mov [pmm_next_page], eax
.pmm_free_page_done:
    pop ebp
    ret 4

    int3
    int3

;====== page mapper=======


; EAX = virtual addr, EBX = physical addr, ECX = flags
map_page:

    push ebp
    mov ebp,esp
    pushad
    mov eax,dword [ebp+8]
    mov ebx,dword [ebp+12]
    mov ecx,dword [ebp+16]

    mov esi, eax
    mov edi, ebx
    mov ebp, ecx

    mov edx, esi
    shr edx, 22
    lea ebx, [PAGE_DIR_BASE + edx*4]

    mov eax, [ebx]
    test eax, 1
    jnz .map_page_have_table

    call alloc_page_table
    jc .map_page_fail
    mov edx, eax
    or eax, PAGE_PRESENT_RW
    mov [ebx], eax
    jmp .map_page_table_ready

.map_page_have_table:
    mov edx, eax
    and edx, PAGE_ADDR_MASK

.map_page_table_ready:
    mov eax, esi
    shr eax, 12
    and eax, 03FFh
    lea edx, [edx + eax*4]

    mov eax, edi
    and eax, PAGE_ADDR_MASK
    or eax, ebp
    or eax, PAGE_PRESENT_RW
    mov [edx], eax

    invlpg [esi]
    popad
    pop ebp
    clc
    ret 12

.map_page_fail:
    popad
    pop ebp
    stc
    ret 12

;====== heap_init and heap_alloc =======
heap_init:
    push ebp
    mov ebp, esp

    mov dword [heap_head], 0
    mov dword [heap_current], KHEAP_BASE
    mov dword [heap_committed], KHEAP_BASE
    mov dword [heap_limit], KHEAP_BASE + KHEAP_MAX_SIZE

    call heap_grow_one_page

    pop ebp
    ret

    int3
    int3

heap_grow_one_page:
    push ebx
    push ecx
    push edx
    push esi

    mov eax, [heap_committed]
    cmp eax, [heap_limit]
    jae .grow_one_page_fail

    call pmm_alloc_page
    test eax, eax
    jz .grow_one_page_fail

    mov ebx, eax
    mov eax, [heap_committed]
    mov ecx, PAGE_PRESENT_RW
    push ecx
    push ebx
    push eax
    call map_page
    jc .grow_one_page_fail

    mov esi, [heap_committed]       ; new block header
    mov dword [esi], HEAP_MAGIC
    mov dword [esi + 4], PAGE_SIZE - HEAP_HDR_SIZE
    mov dword [esi + 8], HEAP_FREE
    mov dword [esi + 12], 0

    cmp dword [heap_head], 0
    jne .grow_one_page_append
    mov [heap_head], esi
    jmp .grow_one_page_commit

.grow_one_page_append:
    mov edx,dword [heap_head]

.grow_one_page_find_tail:
     cmp dword [edx + 12], 0
    je .grow_one_page_link_tail
    mov edx, [edx + 12]
    jmp .grow_one_page_find_tail

.grow_one_page_link_tail:
    mov [edx + 12], esi


.grow_one_page_commit:
    add dword [heap_committed], PAGE_SIZE
    call heap_coalesce
    mov eax, 1
    jmp .grow_one_page_done

.grow_one_page_fail:
    xor eax, eax

.grow_one_page_done:
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret

    int3
    int3

; heap_grow_pages(count)
; [ebp+8] = number of pages to add
; returns EAX = 1 on success, or 0 on failure
heap_grow_pages:
    push ebp
    mov ebp, esp
    push ecx

    mov ecx, [ebp + 8]
    test ecx, ecx
    jz heap_grow_pages_fail

heap_grow_pages_next:
    push ecx
    call heap_grow_one_page
    pop ecx
    test eax, eax
    jz heap_grow_pages_fail

    dec ecx
    jnz heap_grow_pages_next

    mov eax, 1
    jmp heap_grow_pages_done

heap_grow_pages_fail:
    xor eax, eax

heap_grow_pages_done:
    pop ecx
    pop ebp
    ret 4
int3
int3
; EAX = bytes requested
; returns EAX = virtual pointer, or 0
; heap_alloc(size)
; [ebp+8] = requested bytes
; returns EAX = pointer, or 0
heap_alloc:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov ebx, [ebp + 8]
    test ebx, ebx
    jz heap_alloc_fail

    add ebx, 7
    and ebx, 0FFFFFFF8h

heap_alloc_retry_scan:
    mov esi, [heap_head]

heap_alloc_scan:
    test esi, esi
    jz heap_alloc_need_grow

    cmp dword [esi + 0], HEAP_MAGIC
    jne heap_alloc_fail

    cmp dword [esi + 8], HEAP_FREE
    jne heap_alloc_next

    mov eax, [esi + 4]
    cmp eax, ebx
    jb heap_alloc_next

    mov ecx, eax
    sub ecx, ebx
    cmp ecx, HEAP_MIN_SPLIT
    jb heap_alloc_use_whole

    lea edi, [esi + HEAP_HDR_SIZE]
    add edi, ebx

    sub ecx, HEAP_HDR_SIZE
    mov dword [edi + 0], HEAP_MAGIC
    mov dword [edi + 4], ecx
    mov dword [edi + 8], HEAP_FREE

    mov edx, [esi + 12]
    mov [edi + 12], edx
    mov [esi + 12], edi

    mov [esi + 4], ebx

heap_alloc_use_whole:
    mov dword [esi + 8], HEAP_USED
    lea eax, [esi + HEAP_HDR_SIZE]
    jmp heap_alloc_done

heap_alloc_next:
    mov esi, [esi + 12]
    jmp heap_alloc_scan

heap_alloc_need_grow:
    mov eax, ebx
    add eax, HEAP_HDR_SIZE
    add eax, PAGE_SIZE - 1
    shr eax, 12
    push eax
    call heap_grow_pages
    test eax, eax
    jz heap_alloc_fail
    jmp heap_alloc_retry_scan

heap_alloc_fail:
    xor eax, eax

heap_alloc_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 4
int3
int3
; heap free

; heap_free(ptr)
; [ebp+8] = allocation pointer
heap_free:
    push ebp
    mov ebp, esp
    push eax
    push ebx
    push esi

    mov esi, [ebp + 8]
    test esi, esi
    jz heap_free_done

    sub esi, HEAP_HDR_SIZE

    cmp dword [esi + 0], HEAP_MAGIC
    jne heap_free_done

    cmp dword [esi + 8], HEAP_USED
    jne heap_free_done

    mov dword [esi + 8], HEAP_FREE
    call heap_coalesce

heap_free_done:
    pop esi
    pop ebx
    pop eax
    pop ebp
    ret 4
int3
int3
; heap coalesce

heap_coalesce:
    push ebp
    mov ebp, esp
    push eax
    push ebx
    push esi

    mov esi, [heap_head]

heap_coalesce_next:
    test esi, esi
    jz heap_coalesce_done

    mov ebx, [esi + 12]
    test ebx, ebx
    jz heap_coalesce_done

    cmp dword [esi + 8], HEAP_FREE
    jne heap_coalesce_advance

    cmp dword [ebx + 8], HEAP_FREE
    jne heap_coalesce_advance

    mov eax, esi
    add eax, HEAP_HDR_SIZE
    add eax, [esi + 4]
    cmp eax, ebx
    jne heap_coalesce_advance

    mov eax, [ebx + 4]
    add eax, HEAP_HDR_SIZE
    add [esi + 4], eax

    mov eax, [ebx + 12]
    mov [esi + 12], eax
    jmp heap_coalesce_next

heap_coalesce_advance:
    mov esi, [esi + 12]
    jmp heap_coalesce_next

heap_coalesce_done:
    pop esi
    pop ebx
    pop eax
    pop ebp
    ret
int3
int3
; heap metadata validate

; heap_validate()
; returns EAX = 1 if valid, EAX = 0 if corrupt
heap_validate:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi

    mov esi, [heap_head]

heap_validate_loop:
    test esi, esi
    jz heap_validate_ok

    cmp esi, KHEAP_BASE
    jb heap_validate_fail

    cmp esi, [heap_committed]
    jae heap_validate_fail

    cmp dword [esi + 0], HEAP_MAGIC
    jne heap_validate_fail

    mov eax, [esi + 4]
    test eax, eax
    jz heap_validate_fail

    mov eax, [esi + 8]
    cmp eax, HEAP_FREE
    je heap_validate_flag_ok

    cmp eax, HEAP_USED
    jne heap_validate_fail

heap_validate_flag_ok:
    mov eax, esi
    add eax, HEAP_HDR_SIZE
    add eax, [esi + 4]
    cmp eax, [heap_committed]
    ja heap_validate_fail

    mov ebx, [esi + 12]
    test ebx, ebx
    jz heap_validate_ok

    cmp ebx, esi
    jbe heap_validate_fail

    cmp ebx, [heap_committed]
    ja heap_validate_fail

    cmp dword [esi + 8], HEAP_FREE
    jne heap_validate_advance

    cmp dword [ebx + 8], HEAP_FREE
    jne heap_validate_advance

    mov eax, esi
    add eax, HEAP_HDR_SIZE
    add eax, [esi + 4]
    cmp eax, ebx
    je heap_validate_fail

heap_validate_advance:
    mov esi, ebx
    jmp heap_validate_loop

heap_validate_ok:
    mov eax, 1
    jmp heap_validate_done

heap_validate_fail:
    xor eax, eax

heap_validate_done:
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret
int3
int3
;========= General Paging/Heap management =================

PAGE_PRESENT          = 001h
PAGE_RW               = 002h
PAGE_USER             = 004h
PAGE_PS               = 080h
PAGE_PRESENT_USER     = 005h
PAGE_PRESENT_RW_USER  = 007h
PAGE_OWNED            = 200h       ; software PTE bit: PMM owns this page
PAGE_4M_ADDR_MASK     = 0FFC00000h
PAGE_4M_OFFSET_MASK   = 003FFFFFh
PAGE_4K_OFFSET_MASK   = 00000FFFh

USER_SPACE_LIMIT      = 80000000h
USER_HEAP_BASE        = 00400000h
USER_HEAP_MAX_SIZE    = 00400000h
KERNEL_TEMP_PAGE      = 80800000h
KERNEL_TEMP_SOURCE    = 80801000h

V86_PRIVATE_END       = 000A0000h
V86_DEVICE_START      = 000A0000h
V86_DEVICE_END        = 000C0000h
V86_ROM_START         = 000C0000h
V86_ADDRESS_LIMIT     = 00100000h
V86_ENTRY_LINEAR      = 00010000h

page_table_free_list  dd 0
; ===== process object fields =====
PROC_PAGE_DIR         = 0
PROC_HEAP_BASE        = 4
PROC_HEAP_CURRENT     = 8
PROC_HEAP_LIMIT       = 12
PROC_LDT_BASE         = 16
PROC_LDT_BITMAP       = 20
PROC_LDT_SELECTOR     = 24
PROC_USER_CS          = 28
PROC_USER_DS          = 32
PROC_USER_SS          = 36
PROC_TSS_BASE         = 40
PROC_TSS_SELECTOR     = 44
PROC_KSTACK_TOP       = 48
PROC_SLOT_INDEX       = 52
PROC_SLOT_BASE        = 56
PROC_TYPE             = 60
PROC_TABLE_INDEX      = 64
PROC_STORAGE_ACCESS   = 68         ; Raw disk read permission; zero on creation.
PROC_MEM_SIZE         = 72

process_slot_bitmap:
    times PROCESS_SLOT_BITMAP_SIZE db 0

; allocate page table in kernel's  heap and page directory:

alloc_page_table:
    push ebx
    push ecx
    push edi

    mov eax, [page_table_free_list]
    test eax, eax
    jz alloc_page_table_from_pool

    mov ebx, [eax]
    mov [page_table_free_list], ebx
    jmp alloc_page_table_zero

alloc_page_table_from_pool:
    mov eax, [page_table_next]
    cmp eax, PAGE_TABLE_POOL_END
    jae alloc_page_table_fail
    add dword [page_table_next], PAGE_SIZE

alloc_page_table_zero:
    push eax
    mov edi, eax
    xor eax, eax
    mov ecx, 1024
    cld
    rep stosd
    pop eax

    pop edi
    pop ecx
    pop ebx
    clc
    ret

alloc_page_table_fail:
    xor eax, eax
    pop edi
    pop ecx
    pop ebx
    stc
    ret
int3
int3
; Free page table from kernel's heap

; free_page_table(page)
; [ebp+8] = page-table/pagedir page from PAGE_TABLE_POOL
free_page_table:
    push ebp
    mov ebp, esp
    push eax
    push ebx

    mov eax, [ebp + 8]
    test eax, eax
    jz free_page_table_done
    cmp eax, PAGE_TABLE_POOL_BASE
    jb free_page_table_done
    cmp eax, PAGE_TABLE_POOL_END
    jae free_page_table_done
    test eax, PAGE_SIZE - 1
    jnz free_page_table_done

    mov ebx, [page_table_free_list]
    mov [eax], ebx
    mov [page_table_free_list], eax

free_page_table_done:
    pop ebx
    pop eax
    pop ebp
    ret 4
int3
int3
; create_page_directory()
; returns EAX = new page directory physical/linear address, CF clear
; returns EAX = 0, CF set on failure
create_page_directory:
    push ebx
    push ecx
    push esi
    push edi

    call alloc_page_table
    jc create_page_directory_fail

    mov edi, eax
    mov edx, eax
    mov esi, PAGE_DIR_BASE
    xor ecx, ecx

create_page_directory_copy_loop:
    cmp ecx, 1024
    jae create_page_directory_done

    ; Leave the first 4 MiB unmapped in process directories. Slot zero
    ; begins at 1 MiB, so copying the bootstrap's 4 MiB identity PDE
    ; would prevent its user code from receiving normal 4 KiB mappings.
    test ecx, ecx
    jz create_page_directory_next

    mov eax, [esi + ecx*4]
    test eax, PAGE_PRESENT
    jz create_page_directory_next

    ; Copy supervisor/kernel mappings only.
    test eax, PAGE_USER
    jnz create_page_directory_next

    mov [edi + ecx*4], eax

create_page_directory_next:
    inc ecx
    jmp create_page_directory_copy_loop

create_page_directory_done:
    mov eax, edx
    pop edi
    pop esi
    pop ecx
    pop ebx
    clc
    ret

create_page_directory_fail:
    xor eax, eax
    pop edi
    pop esi
    pop ecx
    pop ebx
    stc
    ret
int3
int3
; destroy_page_directory(page_dir)
; [ebp+8] = page directory
; Frees user physical pages and user page tables.
destroy_page_directory:
    push ebp
    mov ebp, esp
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov edi, [ebp + 8]
    test edi, edi
    jz destroy_page_directory_done
    cmp edi, PAGE_DIR_BASE
    je destroy_page_directory_done

    xor esi, esi

destroy_page_directory_pde_loop:
    cmp esi, 512
    jae destroy_page_directory_free_pd

    mov eax, [edi + esi*4]
    test eax, PAGE_PRESENT
    jz destroy_page_directory_next_pde
    test eax, PAGE_USER
    jz destroy_page_directory_next_pde
    test eax, PAGE_PS
    jnz destroy_page_directory_next_pde

    mov ebx, eax
    and ebx, PAGE_ADDR_MASK
    xor ecx, ecx

destroy_page_directory_pte_loop:
    cmp ecx, 1024
    jae destroy_page_directory_free_pt

    mov eax, [ebx + ecx*4]
    test eax, PAGE_PRESENT
    jz destroy_page_directory_next_pte
    test eax, PAGE_USER
    jz destroy_page_directory_next_pte

    test eax, PAGE_OWNED
    jz destroy_page_directory_clear_pte

    mov edx, eax
    and edx, PAGE_ADDR_MASK
    push edx
    call pmm_free_page

destroy_page_directory_clear_pte:
    mov dword [ebx + ecx*4], 0

destroy_page_directory_next_pte:
    inc ecx
    jmp destroy_page_directory_pte_loop

destroy_page_directory_free_pt:
    push ebx
    call free_page_table
    mov dword [edi + esi*4], 0

destroy_page_directory_next_pde:
    inc esi
    jmp destroy_page_directory_pde_loop

destroy_page_directory_free_pd:
    push edi
    call free_page_table

destroy_page_directory_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    pop ebp
    ret 4
int3
int3
; map_user_page(page_dir, virt, phys, flags)
; [ebp+8]  = page directory
; [ebp+12] = user virtual address, 4 KiB aligned
; [ebp+16] = physical page, 4 KiB aligned
; [ebp+20] = extra flags
map_user_page:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov esi, [ebp + 8]
    mov edi, [ebp + 12]
    mov ebx, [ebp + 16]
    mov ecx, [ebp + 20]

    test esi, esi
    jz map_user_page_fail
    cmp edi, USER_SPACE_LIMIT
    jae map_user_page_fail
    test edi, PAGE_SIZE - 1
    jnz map_user_page_fail
    test ebx, PAGE_SIZE - 1
    jnz map_user_page_fail

    mov edx, edi
    shr edx, 22
    lea edx, [esi + edx*4]

    mov eax, [edx]
    test eax, PAGE_PRESENT
    jnz map_user_page_have_pde

    push edx
    push ecx
    push ebx
    push edi
    call alloc_page_table
    pop edi
    pop ebx
    pop ecx
    pop edx
    jc map_user_page_fail

    mov esi, eax
    or eax, PAGE_PRESENT_RW_USER
    mov [edx], eax
    jmp map_user_page_table_ready

map_user_page_have_pde:
    test eax, PAGE_PS
    jnz map_user_page_fail
    test eax, PAGE_USER
    jz map_user_page_fail

    mov esi, eax
    and esi, PAGE_ADDR_MASK

map_user_page_table_ready:
    mov eax, edi
    shr eax, 12
    and eax, 03FFh
    lea edx, [esi + eax*4]

    mov eax, ebx
    and eax, PAGE_ADDR_MASK
    and ecx, 00000FFFh
    or ecx, PAGE_PRESENT_USER
    or eax, ecx
    mov [edx], eax

    invlpg [edi]

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    clc
    ret 16

map_user_page_fail:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    stc
    ret 16
int3
int3
; unmap_page(page_dir, virt)
; [ebp+8]  = page directory
; [ebp+12] = virtual address
; returns EAX = unmapped physical page, or 0 on failure
unmap_page:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi

    mov esi, [ebp + 8]
    mov ebx, [ebp + 12]

    test esi, esi
    jz unmap_page_fail

    mov edx, ebx
    shr edx, 22
    mov eax, [esi + edx*4]
    test eax, PAGE_PRESENT
    jz unmap_page_fail
    test eax, PAGE_PS
    jnz unmap_page_fail

    mov esi, eax
    and esi, PAGE_ADDR_MASK

    mov ecx, ebx
    shr ecx, 12
    and ecx, 03FFh

    mov eax, [esi + ecx*4]
    test eax, PAGE_PRESENT
    jz unmap_page_fail

    mov edx, eax
    and edx, PAGE_ADDR_MASK
    mov dword [esi + ecx*4], 0
    invlpg [ebx]

    mov eax, edx

    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    clc
    ret 8

unmap_page_fail:
    xor eax, eax
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    stc
    ret 8
int3
int3
; get_physical_address(page_dir, virt)
; [ebp+8]  = page directory
; [ebp+12] = virtual address
; returns EAX = physical address, or 0 on failure
get_physical_address:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi

    mov esi, [ebp + 8]
    mov ebx, [ebp + 12]

    test esi, esi
    jz get_physical_address_fail

    mov edx, ebx
    shr edx, 22
    mov eax, [esi + edx*4]
    test eax, PAGE_PRESENT
    jz get_physical_address_fail

    test eax, PAGE_PS
    jz get_physical_address_4k

    and eax, PAGE_4M_ADDR_MASK
    mov edx, ebx
    and edx, PAGE_4M_OFFSET_MASK
    or eax, edx
    jmp get_physical_address_done

get_physical_address_4k:
    mov esi, eax
    and esi, PAGE_ADDR_MASK

    mov ecx, ebx
    shr ecx, 12
    and ecx, 03FFh

    mov eax, [esi + ecx*4]
    test eax, PAGE_PRESENT
    jz get_physical_address_fail

    and eax, PAGE_ADDR_MASK
    mov edx, ebx
    and edx, PAGE_4K_OFFSET_MASK
    or eax, edx
    jmp get_physical_address_done

get_physical_address_fail:
    xor eax, eax
    stc
    jmp get_physical_address_exit

get_physical_address_done:
    clc

get_physical_address_exit:
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 8

 int3
 int3
; ====== process virtual memory helpers ============

; process_memory_init_heap_page(phys)
; [ebp+8] = physical page that will become a user heap page
; returns EAX = 1 on success, EAX = 0 on failure
process_memory_init_heap_page:
    push ebp
    mov ebp, esp
    push ecx
    push edi

    push PAGE_PRESENT_RW
    push dword [ebp + 8]
    push KERNEL_TEMP_PAGE
    call map_page
    jc process_memory_init_heap_page_fail

    mov edi, KERNEL_TEMP_PAGE
    xor eax, eax
    mov ecx, PAGE_SIZE / 4
    cld
    rep stosd

    mov dword [KERNEL_TEMP_PAGE + 0], HEAP_MAGIC
    mov dword [KERNEL_TEMP_PAGE + 4], PAGE_SIZE - HEAP_HDR_SIZE
    mov dword [KERNEL_TEMP_PAGE + 8], HEAP_FREE
    mov dword [KERNEL_TEMP_PAGE + 12], 0

    push KERNEL_TEMP_PAGE
    push PAGE_DIR_BASE
    call unmap_page
    jc process_memory_init_heap_page_fail

    mov eax, 1
    jmp process_memory_init_heap_page_done

process_memory_init_heap_page_fail:
    xor eax, eax

process_memory_init_heap_page_done:
    pop edi
    pop ecx
    pop ebp
    ret 4

int3
int3
; process_memory_create()
; returns EAX = process memory object pointer, CF clear
; returns EAX = 0, CF set on failure
process_memory_create:
    push ebx
    push ecx
    push edx
    push esi
    push edi

    push PROC_MEM_SIZE
    call heap_alloc
    test eax, eax
    jz process_memory_create_fail

    mov esi, eax
    xor eax, eax
    mov ecx, PROC_MEM_SIZE / 4
    mov edi, esi
    cld
    rep stosd

    mov dword [esi + PROC_SLOT_INDEX], 0FFFFFFFFh
    mov dword [esi + PROC_TABLE_INDEX], 0FFFFFFFFh

    call create_page_directory
    jc process_memory_create_free_object

    mov [esi + PROC_PAGE_DIR], eax
    mov dword [esi + PROC_HEAP_BASE], USER_HEAP_BASE
    mov dword [esi + PROC_HEAP_CURRENT], USER_HEAP_BASE
    mov dword [esi + PROC_HEAP_LIMIT], USER_HEAP_BASE + USER_HEAP_MAX_SIZE

    call pmm_alloc_page
    test eax, eax
    jz process_memory_create_destroy_directory

    mov ebx, eax

    push ebx
    call process_memory_init_heap_page
    test eax, eax
    jz process_memory_create_free_phys

    push PAGE_PRESENT_RW_USER or PAGE_OWNED
    push ebx
    push dword [esi + PROC_HEAP_CURRENT]
    push dword [esi + PROC_PAGE_DIR]
    call map_user_page
    jc process_memory_create_free_phys

    add dword [esi + PROC_HEAP_CURRENT], PAGE_SIZE

    mov eax, esi
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    clc
    ret

process_memory_create_free_phys:
    push ebx
    call pmm_free_page

process_memory_create_destroy_directory:
    push dword [esi + PROC_PAGE_DIR]
    call destroy_page_directory

process_memory_create_free_object:
    push esi
    call heap_free

process_memory_create_fail:
    xor eax, eax
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    stc
    ret

int3
int3

; v86_process_memory_create()
; Creates a process object and a private CR3 with a VM86 low-memory view.
; returns EAX = process object, CF clear; EAX = 0, CF set on failure
v86_process_memory_create:
    push ecx
    push esi
    push edi

    push PROC_MEM_SIZE
    call heap_alloc
    test eax, eax
    jz v86_process_memory_create_fail

    mov esi, eax
    mov edi, eax
    xor eax, eax
    mov ecx, PROC_MEM_SIZE / 4
    cld
    rep stosd

    mov dword [esi + PROC_TYPE], PROCESS_TYPE_V86
    mov dword [esi + PROC_SLOT_INDEX], 0FFFFFFFFh
    mov dword [esi + PROC_TABLE_INDEX], 0FFFFFFFFh

    call create_page_directory
    jc v86_process_memory_create_free_object
    mov [esi + PROC_PAGE_DIR], eax

    push esi
    call v86_map_low_memory
    test eax, eax
    jz v86_process_memory_create_destroy_directory

    mov eax, esi
    pop edi
    pop esi
    pop ecx
    clc
    ret

v86_process_memory_create_destroy_directory:
    push dword [esi + PROC_PAGE_DIR]
    call destroy_page_directory

v86_process_memory_create_free_object:
    push esi
    call heap_free

v86_process_memory_create_fail:
    xor eax, eax
    pop edi
    pop esi
    pop ecx
    stc
    ret

; v86_map_low_memory(process)
; Private conventional RAM: 0x00000-0x9FFFF, initialized from real low RAM.
; Shared device/VGA aperture: 0xA0000-0xBFFFF, user read/write.
; Shared BIOS/option ROM: 0xC0000-0xFFFFF, user read-only.
v86_map_low_memory:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov esi, [ebp + 8]
    test esi, esi
    jz v86_map_low_memory_fail

    xor edx, edx

v86_map_low_memory_private_loop:
    cmp edx, V86_PRIVATE_END
    jae v86_map_low_memory_device_begin

    call pmm_alloc_page
    test eax, eax
    jz v86_map_low_memory_fail
    mov ebx, eax

    push PAGE_PRESENT_RW
    push edx
    push KERNEL_TEMP_SOURCE
    call map_page
    jc v86_map_low_memory_free_private_page

    push PAGE_PRESENT_RW
    push ebx
    push KERNEL_TEMP_PAGE
    call map_page
    jc v86_map_low_memory_unmap_source

    mov esi, KERNEL_TEMP_SOURCE
    mov edi, KERNEL_TEMP_PAGE
    mov ecx, PAGE_SIZE / 4
    cld
    rep movsd

    cmp edx, V86_ENTRY_LINEAR
    jne v86_map_low_memory_private_copied
    mov byte [KERNEL_TEMP_PAGE], 0EBh
    mov byte [KERNEL_TEMP_PAGE + 1], 0FEh

v86_map_low_memory_private_copied:
    push KERNEL_TEMP_PAGE
    push PAGE_DIR_BASE
    call unmap_page
    jc v86_map_low_memory_unmap_source

    push KERNEL_TEMP_SOURCE
    push PAGE_DIR_BASE
    call unmap_page
    jc v86_map_low_memory_free_private_page

    mov esi, [ebp + 8]
    push PAGE_PRESENT_RW_USER or PAGE_OWNED
    push ebx
    push edx
    push dword [esi + PROC_PAGE_DIR]
    call map_user_page
    jc v86_map_low_memory_free_private_page

    add edx, PAGE_SIZE
    jmp v86_map_low_memory_private_loop

v86_map_low_memory_unmap_source:
    push KERNEL_TEMP_SOURCE
    push PAGE_DIR_BASE
    call unmap_page

v86_map_low_memory_free_private_page:
    push ebx
    call pmm_free_page
    jmp v86_map_low_memory_fail

v86_map_low_memory_device_begin:
    mov edx, V86_DEVICE_START

v86_map_low_memory_device_loop:
    cmp edx, V86_DEVICE_END
    jae v86_map_low_memory_rom_begin

    mov esi, [ebp + 8]
    push PAGE_PRESENT_RW_USER
    push edx
    push edx
    push dword [esi + PROC_PAGE_DIR]
    call map_user_page
    jc v86_map_low_memory_fail

    add edx, PAGE_SIZE
    jmp v86_map_low_memory_device_loop

v86_map_low_memory_rom_begin:
    mov edx, V86_ROM_START

v86_map_low_memory_rom_loop:
    cmp edx, V86_ADDRESS_LIMIT
    jae v86_map_low_memory_success

    mov esi, [ebp + 8]
    push PAGE_PRESENT_USER
    push edx
    push edx
    push dword [esi + PROC_PAGE_DIR]
    call map_user_page
    jc v86_map_low_memory_fail

    add edx, PAGE_SIZE
    jmp v86_map_low_memory_rom_loop

v86_map_low_memory_success:
    mov eax, 1
    jmp v86_map_low_memory_done

v86_map_low_memory_fail:
    xor eax, eax

v86_map_low_memory_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 4

; ==============================
; GDT dynamic descriptor manager
; ==============================

GDT_BASE             = 030010000h
GDT_ENTRY_COUNT      = 8192
GDT_BITMAP_SIZE      = GDT_ENTRY_COUNT / 8

; Change this if you already use more fixed GDT entries.
; Entries:
; 0 = null
; 1 = 0x08 kernel code
; 2 = 0x10 kernel data
; 3 = 0x18 interrupt code
; 4 = 0x20 interrupt data
; 5 = 0x28 SYSENTER kernel code
; 6 = 0x30 SYSENTER kernel data/stack
; 7 = 0x38 SYSEXIT user code (selector 0x3B)
; 8 = 0x40 SYSEXIT user data/stack (selector 0x43)
GDT_FIRST_DYNAMIC    = 9

DESC_PRESENT         = 080h
DESC_DPL0            = 000h
DESC_DPL1            = 020h
DESC_DPL2            = 040h
DESC_DPL3            = 060h
DESC_SYSTEM          = 000h
DESC_CODEDATA        = 010h

DESC_CODE_RX         = 00Ah
DESC_DATA_RW         = 002h

DESC_TYPE_LDT        = 002h
DESC_TYPE_TSS32_AVL  = 009h
DESC_TYPE_CALL_GATE  = 00Ch

DESC_FLAG_32BIT      = 040h
DESC_FLAG_GRAN_4K    = 080h

gdt_bitmap:
    times GDT_BITMAP_SIZE db 0


gdt_allocator_init:
    push eax
    push ecx
    push edi

    mov edi, gdt_bitmap
    xor eax, eax
    mov ecx, GDT_BITMAP_SIZE / 4
    cld
    rep stosd

    xor ecx, ecx

gdt_allocator_init_mark_fixed:
    cmp ecx, GDT_FIRST_DYNAMIC
    jae gdt_allocator_init_done

    bts dword [gdt_bitmap], ecx
    inc ecx
    jmp gdt_allocator_init_mark_fixed

gdt_allocator_init_done:
    pop edi
    pop ecx
    pop eax
    ret

gdt_alloc_selector:
    push ecx

    mov ecx, GDT_FIRST_DYNAMIC

gdt_alloc_selector_scan:
    cmp ecx, GDT_ENTRY_COUNT
    jae gdt_alloc_selector_fail

    bt dword [gdt_bitmap], ecx
    jnc gdt_alloc_selector_found

    inc ecx
    jmp gdt_alloc_selector_scan

gdt_alloc_selector_found:
    bts dword [gdt_bitmap], ecx

    mov eax, ecx
    shl eax, 3

    pop ecx
    clc
    ret

gdt_alloc_selector_fail:
    xor eax, eax
    pop ecx
    stc
    ret

; gdt_write_raw_descriptor(selector, low_dword, high_dword)
; [ebp+8]  = selector
; [ebp+12] = descriptor low dword
; [ebp+16] = descriptor high dword
gdt_write_raw_descriptor:
    push ebp
    mov ebp, esp
    push eax
    push edi

    mov eax, [ebp + 8]
    and eax, 0FFF8h

    lea edi, [GDT_BASE + eax]

    mov eax, [ebp + 12]
    mov [edi + 0], eax

    mov eax, [ebp + 16]
    mov [edi + 4], eax

    pop edi
    pop eax
    pop ebp
    ret 12


; gdt_free_selector(selector)
; [ebp+8] = selector
gdt_free_selector:
    push ebp
    mov ebp, esp
    push eax
    push ecx
    push edi

    mov eax, [ebp + 8]

    ; Must be GDT selector, not LDT selector.
    test eax, 4
    jnz gdt_free_selector_done

    shr eax, 3
    cmp eax, GDT_FIRST_DYNAMIC
    jb gdt_free_selector_done

    cmp eax, GDT_ENTRY_COUNT
    jae gdt_free_selector_done

    btr dword [gdt_bitmap], eax

    lea edi, [GDT_BASE + eax*8]
    mov dword [edi + 0], 0
    mov dword [edi + 4], 0

gdt_free_selector_done:
    pop edi
    pop ecx
    pop eax
    pop ebp
    ret 4


; gdt_write_segment_descriptor(selector, base, limit, access_base)
; [ebp+8]  = selector
; [ebp+12] = base
; [ebp+16] = inclusive byte limit
; [ebp+20] = access byte, for example 09Ah / 092h / 0FAh / 0F2h
;
; Auto uses byte granularity if limit <= 0xFFFFF.
; Auto uses 4K granularity if limit > 0xFFFFF.
gdt_write_segment_descriptor:
    push ebp
    mov ebp, esp
    push eax
    push ebx
    push ecx
    push edx

    mov ebx, [ebp + 12]     ; base
    mov ecx, [ebp + 16]     ; limit
    mov edx, DESC_FLAG_32BIT

    cmp ecx, 0FFFFFh
    jbe gdt_write_segment_descriptor_limit_ready

    ; For 4K granularity, limit should end in 0xFFF.
    test ecx, 0FFFh
    cmp ecx, ecx
    ; If you want strict checking, insert failure handling here.
    shr ecx, 12
    or edx, DESC_FLAG_GRAN_4K

gdt_write_segment_descriptor_limit_ready:
    ; low dword = limit[15:0] | base[15:0] << 16
    mov eax, ebx
    shl eax, 16
    mov ax, cx

    ; high dword:
    ; base[16:23] at bits 0..7
    ; access at bits 8..15
    ; limit[16:19] at bits 16..19
    ; flags at bits 20..23
    ; base[24:31] at bits 24..31
    push eax

    mov eax, ebx
    shr eax, 16
    and eax, 000000FFh

    mov ebx, [ebp + 20]
    shl ebx, 8
    or eax, ebx

    mov ebx, ecx
    shr ebx, 16
    and ebx, 0000000Fh
    shl ebx, 16
    or eax, ebx

    mov ebx, edx
    and ebx, 000000F0h
    shl ebx, 16
    or eax, ebx

    mov ebx, [ebp + 12]
    and ebx, 0FF000000h
    or eax, ebx

    mov edx, eax
    pop eax

    push edx
    push eax
    push dword [ebp + 8]
    call gdt_write_raw_descriptor

    pop edx
    pop ecx
    pop ebx
    pop eax
    pop ebp
    ret 16

    ; gdt_create_code_descriptor(base, limit, dpl_access)
    ; [ebp+8]  = base
    ; [ebp+12] = limit
    ; [ebp+16] = DPL bits: DESC_DPL0 / DESC_DPL1 / DESC_DPL2 / DESC_DPL3
    ; returns EAX = selector
gdt_create_code_descriptor:
    push ebp
    mov ebp, esp

    call gdt_alloc_selector
    jc gdt_create_code_descriptor_fail

    push eax

    mov edx, DESC_PRESENT or DESC_CODEDATA or DESC_CODE_RX
    or edx, [ebp + 16]

    push edx
    push dword [ebp + 12]
    push dword [ebp + 8]
    push eax
    call gdt_write_segment_descriptor

    pop eax
    clc
    pop ebp
    ret 12

gdt_create_code_descriptor_fail:
    xor eax, eax
    stc
    pop ebp
    ret 12


  ; gdt_create_data_descriptor(base, limit, dpl_access)
  ; [ebp+8]  = base
  ; [ebp+12] = limit
  ; [ebp+16] = DPL bits
  ; returns EAX = selector
gdt_create_data_descriptor:
    push ebp
    mov ebp, esp

    call gdt_alloc_selector
    jc gdt_create_data_descriptor_fail

    push eax

    mov edx, DESC_PRESENT or DESC_CODEDATA or DESC_DATA_RW
    or edx, [ebp + 16]

    push edx
    push dword [ebp + 12]
    push dword [ebp + 8]
    push eax
    call gdt_write_segment_descriptor

    pop eax
    clc
    pop ebp
    ret 12

gdt_create_data_descriptor_fail:
    xor eax, eax
    stc
    pop ebp
    ret 12


  ; build_system_descriptor(base, limit, access)
; [ebp+8]  = base
; [ebp+12] = byte limit
; [ebp+16] = access byte
; returns:
;   EAX = low dword
;   EDX = high dword
build_system_descriptor:
    push ebp
    mov ebp, esp
    push ebx
    push ecx

    mov ebx, [ebp + 8]      ; base
    mov ecx, [ebp + 12]     ; limit

    ; low = limit[15:0] | base[15:0] << 16
    mov eax, ebx
    shl eax, 16
    mov ax, cx

    ; high =
    ; base[16:23]   bits 0..7
    ; access         bits 8..15
    ; limit[16:19]   bits 16..19
    ; flags          bits 20..23, keep 0 for LDT/TSS
    ; base[24:31]    bits 24..31
    mov edx, ebx
    shr edx, 16
    and edx, 000000FFh

    mov ebx, [ebp + 16]
    shl ebx, 8
    or edx, ebx

    mov ebx, ecx
    shr ebx, 16
    and ebx, 0000000Fh
    shl ebx, 16
    or edx, ebx

    mov ebx, [ebp + 8]
    and ebx, 0FF000000h
    or edx, ebx

    pop ecx
    pop ebx
    pop ebp
    ret 12

  ; gdt_create_ldt_descriptor(base, limit)
; [ebp+8]  = LDT base
; [ebp+12] = LDT byte limit, usually LDT_SIZE - 1
; returns EAX = selector, CF clear
gdt_create_ldt_descriptor:
    push ebp
    mov ebp, esp
    push ebx
    push edx

    call gdt_alloc_selector
    jc gdt_create_ldt_descriptor_fail

    mov ebx, eax

    push DESC_PRESENT or DESC_TYPE_LDT
    push dword [ebp + 12]
    push dword [ebp + 8]
    call build_system_descriptor

    push edx
    push eax
    push ebx
    call gdt_write_raw_descriptor

    mov eax, ebx
    pop edx
    pop ebx
    pop ebp
    clc
    ret 8

gdt_create_ldt_descriptor_fail:
    xor eax, eax
    pop edx
    pop ebx
    pop ebp
    stc
    ret 8


; gdt_create_tss_descriptor(base)
; [ebp+8] = TSS base
; returns EAX = selector, CF clear
gdt_create_tss_descriptor:
    push ebp
    mov ebp, esp
    push ebx
    push edx

    call gdt_alloc_selector
    jc gdt_create_tss_descriptor_fail

    mov ebx, eax

    push DESC_PRESENT or DESC_TYPE_TSS32_AVL
    push 067h
    push dword [ebp + 8]
    call build_system_descriptor

    push edx
    push eax
    push ebx
    call gdt_write_raw_descriptor

    mov eax, ebx
    pop edx
    pop ebx
    pop ebp
    clc
    ret 4

gdt_create_tss_descriptor_fail:
    xor eax, eax
    pop edx
    pop ebx
    pop ebp
    stc
    ret 4

; gdt_create_tss_descriptor_with_limit(base, limit)
; [ebp+8]  = TSS base
; [ebp+12] = inclusive descriptor limit
; returns EAX = selector, CF clear
gdt_create_tss_descriptor_with_limit:
    push ebp
    mov ebp, esp
    push ebx
    push edx

    call gdt_alloc_selector
    jc gdt_create_tss_descriptor_with_limit_fail

    mov ebx, eax

    push DESC_PRESENT or DESC_TYPE_TSS32_AVL
    push dword [ebp + 12]
    push dword [ebp + 8]
    call build_system_descriptor

    push edx
    push eax
    push ebx
    call gdt_write_raw_descriptor

    mov eax, ebx
    pop edx
    pop ebx
    pop ebp
    clc
    ret 8

gdt_create_tss_descriptor_with_limit_fail:
    xor eax, eax
    pop edx
    pop ebx
    pop ebp
    stc
    ret 8

  ;=========LDT CREATORS============

LDT_ENTRY_COUNT       = 512
LDT_SIZE              = LDT_ENTRY_COUNT * 8
LDT_BITMAP_SIZE       = LDT_ENTRY_COUNT / 8

LDT_SELECTOR_BITS     = 7        ; TI=1 + RPL=3

PROCESS_AREA_BASE     = 00100000h     ; reserve low 1 MiB for future v86/BIOS
PROCESS_SLOT_SIZE     = 00A00000h     ; 10 MiB per process slot
PROCESS_MAX_COUNT     = 100
PROCESS_SLOT_BITMAP_SIZE = 16         ; 100 bits rounded up/aligned

USER_CODE_OFFSET      = 00000000h
USER_CODE_SIZE        = 00400000h     ; 4 MiB
USER_CODE_LIMIT       = USER_CODE_SIZE - 1

USER_DATA_OFFSET      = 00400000h
USER_DATA_SIZE        = 00500000h     ; 5 MiB
USER_DATA_LIMIT       = USER_DATA_SIZE - 1

USER_STACK_OFFSET     = 00900000h
USER_STACK_SIZE       = 00100000h     ; 1 MiB
USER_STACK_LIMIT      = USER_STACK_SIZE - 1


USER_CODE_ACCESS      = 0FAh     ; present, DPL3, code RX
USER_DATA_ACCESS      = 0F2h     ; present, DPL3, data RW
USER_STACK_ACCESS     = 0F2h

; ldt_alloc_selector(ldt_bitmap, ldt_entry_count)
; [ebp+8]  = bitmap base
; [ebp+12] = number of LDT entries
; returns EAX = randomized LDT selector, CF clear
; returns EAX = 0, CF set on failure
ldt_alloc_selector:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov edi, [ebp + 8]
    mov ecx, [ebp + 12]

    cmp ecx, 2
    jb ldt_alloc_selector_fail

    rdtsc
    xor edx, edx
    dec ecx
    div ecx
    inc edx                 ; random start index in 1..entry_count-1

    mov esi, edx
    mov ebx, [ebp + 12]

ldt_alloc_selector_scan:
    cmp ebx, 0
    je ldt_alloc_selector_fail

    cmp esi, [ebp + 12]
    jb ldt_alloc_selector_test
    mov esi, 1

ldt_alloc_selector_test:
    bt dword [edi], esi
    jnc ldt_alloc_selector_found

    inc esi
    dec ebx
    jmp ldt_alloc_selector_scan

ldt_alloc_selector_found:
    bts dword [edi], esi

    mov eax, esi
    shl eax, 3
    or eax, LDT_SELECTOR_BITS

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    clc
    ret 8

ldt_alloc_selector_fail:
    xor eax, eax
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    stc
    ret 8

    ; ldt_write_raw_descriptor(ldt_base, selector, low, high)
    ; [ebp+8]  = LDT base
    ; [ebp+12] = LDT selector
    ; [ebp+16] = descriptor low dword
    ; [ebp+20] = descriptor high dword
    ldt_write_raw_descriptor:
    push ebp
    mov ebp, esp
    push eax
    push edi

    mov edi, [ebp + 8]

    mov eax, [ebp + 12]
    and eax, 0FFF8h
    add edi, eax

    mov eax, [ebp + 16]
    mov [edi + 0], eax

    mov eax, [ebp + 20]
    mov [edi + 4], eax

    pop edi
    pop eax
    pop ebp
    ret 16


  ; ldt_write_segment_descriptor(ldt_base, selector, base, limit, access)
; [ebp+8]  = LDT base
; [ebp+12] = LDT selector
; [ebp+16] = segment base
; [ebp+20] = inclusive byte limit
; [ebp+24] = access byte
ldt_write_segment_descriptor:
    push ebp
    mov ebp, esp
    push eax
    push ebx
    push ecx
    push edx

    mov ebx, [ebp + 16]
    mov ecx, [ebp + 20]
    mov edx, DESC_FLAG_32BIT

    cmp ecx, 0FFFFFh
    jbe ldt_write_segment_descriptor_limit_ready

    shr ecx, 12
    or edx, DESC_FLAG_GRAN_4K

ldt_write_segment_descriptor_limit_ready:
    mov eax, ebx
    shl eax, 16
    mov ax, cx
    push eax

    mov eax, ebx
    shr eax, 16
    and eax, 000000FFh

    mov ebx, [ebp + 24]
    shl ebx, 8
    or eax, ebx

    mov ebx, ecx
    shr ebx, 16
    and ebx, 0000000Fh
    shl ebx, 16
    or eax, ebx

    mov ebx, edx
    and ebx, 000000F0h
    shl ebx, 16
    or eax, ebx

    mov ebx, [ebp + 16]
    and ebx, 0FF000000h
    or eax, ebx

    mov edx, eax
    pop eax

    push edx
    push eax
    push dword [ebp + 12]
    push dword [ebp + 8]
    call ldt_write_raw_descriptor

    pop edx
    pop ecx
    pop ebx
    pop eax
    pop ebp
    ret 20


   ; process_create_random_ldt(process, process_linear_base)
; [ebp+8]  = process memory/object pointer
; [ebp+12] = process linear base for its 10 MiB segmented layout
; returns EAX = 1 on success, EAX = 0 on failure
process_create_random_ldt:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_create_random_ldt_fail

    push LDT_SIZE
    call heap_alloc
    test eax, eax
    jz process_create_random_ldt_fail

    mov [esi + PROC_LDT_BASE], eax
    mov edi, eax
    xor eax, eax
    mov ecx, LDT_SIZE / 4
    cld
    rep stosd

    push LDT_BITMAP_SIZE
    call heap_alloc
    test eax, eax
    jz process_create_random_ldt_free_ldt

    mov [esi + PROC_LDT_BITMAP], eax
    mov edi, eax
    xor eax, eax
    mov ecx, LDT_BITMAP_SIZE / 4
    cld
    rep stosd

    mov edi, [esi + PROC_LDT_BITMAP]
    bts dword [edi], 0

    push LDT_SIZE - 1
    push dword [esi + PROC_LDT_BASE]
    call gdt_create_ldt_descriptor
    jc process_create_random_ldt_free_bitmap

    mov [esi + PROC_LDT_SELECTOR], eax

    push LDT_ENTRY_COUNT
    push dword [esi + PROC_LDT_BITMAP]
    call ldt_alloc_selector
    jc process_create_random_ldt_free_gdt_ldt

    mov [esi + PROC_USER_CS], eax

    push LDT_ENTRY_COUNT
    push dword [esi + PROC_LDT_BITMAP]
    call ldt_alloc_selector
    jc process_create_random_ldt_free_gdt_ldt

    mov [esi + PROC_USER_DS], eax

    push LDT_ENTRY_COUNT
    push dword [esi + PROC_LDT_BITMAP]
    call ldt_alloc_selector
    jc process_create_random_ldt_free_gdt_ldt

    mov [esi + PROC_USER_SS], eax

    mov ebx, [ebp + 12]

    mov eax, ebx
    add eax, USER_CODE_OFFSET
    push USER_CODE_ACCESS
    push USER_CODE_SIZE - 1
    push eax
    push dword [esi + PROC_USER_CS]
    push dword [esi + PROC_LDT_BASE]
    call ldt_write_segment_descriptor

    mov eax, ebx
    add eax, USER_DATA_OFFSET
    push USER_DATA_ACCESS
    push USER_DATA_SIZE - 1
    push eax
    push dword [esi + PROC_USER_DS]
    push dword [esi + PROC_LDT_BASE]
    call ldt_write_segment_descriptor

    mov eax, ebx
    add eax, USER_STACK_OFFSET
    push USER_STACK_ACCESS
    push USER_STACK_SIZE - 1
    push eax
    push dword [esi + PROC_USER_SS]
    push dword [esi + PROC_LDT_BASE]
    call ldt_write_segment_descriptor

    mov eax, 1
    jmp process_create_random_ldt_done

process_create_random_ldt_free_gdt_ldt:
    push dword [esi + PROC_LDT_SELECTOR]
    call gdt_free_selector

process_create_random_ldt_free_bitmap:
    push dword [esi + PROC_LDT_BITMAP]
    call heap_free

process_create_random_ldt_free_ldt:
    push dword [esi + PROC_LDT_BASE]
    call heap_free

process_create_random_ldt_fail:
    xor eax, eax

process_create_random_ldt_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 8


;========== KERNEL AND PROCESS TSS ========================

; ==============================
; 32-bit TSS offsets
; ==============================

TSS_PREV_TASK    = 000h
TSS_ESP0         = 004h
TSS_SS0          = 008h
TSS_ESP1         = 00Ch
TSS_SS1          = 010h
TSS_ESP2         = 014h
TSS_SS2          = 018h
TSS_CR3          = 01Ch
TSS_EIP          = 020h
TSS_EFLAGS       = 024h
TSS_EAX          = 028h
TSS_ECX          = 02Ch
TSS_EDX          = 030h
TSS_EBX          = 034h
TSS_ESP          = 038h
TSS_EBP          = 03Ch
TSS_ESI          = 040h
TSS_EDI          = 044h
TSS_ES           = 048h
TSS_CS           = 04Ch
TSS_SS           = 050h
TSS_DS           = 054h
TSS_FS           = 058h
TSS_GS           = 05Ch
TSS_LDTR         = 060h
TSS_TRACE        = 064h
TSS_IO_MAP_BASE  = 066h

TSS32_SIZE       = 068h
TSS_OWNER_PROCESS = 068h          ; kernel metadata, outside TSS descriptor limit
TSS_OBJECT_SIZE   = 06Ch

EFLAGS_IF        = 00000200h
EFLAGS_RESERVED  = 00000002h
EFLAGS_VM        = 00020000h
EFLAGS_VIF       = 00080000h

USER_ENTRY_EIP   = 00000000h
USER_STACK_ESP   = USER_STACK_SIZE

V86_ENTRY_CS              = V86_ENTRY_LINEAR shr 4
V86_ENTRY_IP              = 0000h
V86_STACK_SS              = 9000h
V86_STACK_SP              = 0FFF0h
V86_REDIR_BITMAP_BASE     = TSS32_SIZE
V86_REDIR_BITMAP_SIZE     = 20h
V86_IO_BITMAP_BASE        = V86_REDIR_BITMAP_BASE + V86_REDIR_BITMAP_SIZE
V86_IO_BITMAP_SIZE        = 2000h
V86_IO_BITMAP_TERMINATOR  = V86_IO_BITMAP_BASE + V86_IO_BITMAP_SIZE
V86_TSS_LIMIT             = V86_IO_BITMAP_TERMINATOR
V86_TSS_OWNER_PROCESS     = (V86_TSS_LIMIT + 4) and 0FFFFFFFCh
V86_TSS_OBJECT_SIZE       = V86_TSS_OWNER_PROCESS + 4

kernel_tss_base      dd 0
kernel_tss_selector  dd 0


; process_create_tss(process)
; [ebp+8] = process object pointer
; returns EAX = TSS selector, CF clear
; returns EAX = 0, CF set
process_create_tss:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push esi
    push edi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_create_tss_fail
    push TSS_OBJECT_SIZE
    call heap_alloc
    test eax, eax
    jz process_create_tss_fail

    mov [esi + PROC_TSS_BASE], eax
    mov edi, eax
    xor eax, eax
    mov ecx, TSS_OBJECT_SIZE / 4
    cld
    rep stosd

    mov edi, [esi + PROC_TSS_BASE]
    mov [edi + TSS_OWNER_PROCESS], esi

    push PAGE_SIZE
    call heap_alloc
    test eax, eax
    jz process_create_tss_free_tss

    add eax, PAGE_SIZE
    mov [esi + PROC_KSTACK_TOP], eax

    mov edi, [esi + PROC_TSS_BASE]

    mov eax, [esi + PROC_KSTACK_TOP]
    mov [edi + TSS_ESP0], eax
    mov word [edi + TSS_SS0], KERNEL_DS

    call keyboard_prepare_process
    call graphics_prepare_process

    mov eax, [esi + PROC_PAGE_DIR]
    mov [edi + TSS_CR3], eax

    mov dword [edi + TSS_EIP], USER_ENTRY_EIP
    mov dword [edi + TSS_EFLAGS], EFLAGS_RESERVED or EFLAGS_IF

    mov dword [edi + TSS_ESP], USER_STACK_ESP
    mov dword [edi + TSS_EBP], USER_STACK_ESP

    mov ax, [esi + PROC_USER_CS]
    mov [edi + TSS_CS], ax

    mov ax, [esi + PROC_USER_DS]
    mov [edi + TSS_DS], ax
    mov [edi + TSS_ES], ax
    mov [edi + TSS_FS], ax
    mov [edi + TSS_GS], ax

    mov ax, [esi + PROC_USER_SS]
    mov [edi + TSS_SS], ax

    mov ax, [esi + PROC_LDT_SELECTOR]
    mov [edi + TSS_LDTR], ax
    ; No IO permission bitmap for now.
    mov word [edi + TSS_IO_MAP_BASE], TSS32_SIZE

    push dword [esi + PROC_TSS_BASE]
    call gdt_create_tss_descriptor
    jc process_create_tss_free_stack

    mov [esi + PROC_TSS_SELECTOR], eax

    pop edi
    pop esi
    pop ecx
    pop ebx
    pop ebp
    clc
    ret 4

process_create_tss_free_stack:
    mov eax, [esi + PROC_KSTACK_TOP]
    sub eax, PAGE_SIZE
    push eax
    call heap_free
    mov dword [esi + PROC_KSTACK_TOP], 0

process_create_tss_free_tss:
    push dword [esi + PROC_TSS_BASE]
    call heap_free
    mov dword [esi + PROC_TSS_BASE], 0

process_create_tss_fail:
    xor eax, eax
    pop edi
    pop esi
    pop ecx
    pop ebx
    pop ebp
    stc
    ret 4

; v86_process_create_tss(process)
; Creates a hardware-task TSS with VME redirection and unrestricted port IO.
; The 32-byte interrupt bitmap is clear, so INT n uses the process's VM86 IVT.
v86_process_create_tss:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push esi
    push edi

    mov esi, [ebp + 8]
    test esi, esi
    jz v86_process_create_tss_fail

    push V86_TSS_OBJECT_SIZE
    call heap_alloc
    test eax, eax
    jz v86_process_create_tss_fail

    mov [esi + PROC_TSS_BASE], eax
    mov edi, eax
    xor eax, eax
    mov ecx, V86_TSS_OBJECT_SIZE / 4
    cld
    rep stosd

    mov edi, [esi + PROC_TSS_BASE]
    mov [edi + V86_TSS_OWNER_PROCESS], esi
    mov byte [edi + V86_IO_BITMAP_TERMINATOR], 0FFh

    push PAGE_SIZE
    call heap_alloc
    test eax, eax
    jz v86_process_create_tss_free_tss

    add eax, PAGE_SIZE
    mov [esi + PROC_KSTACK_TOP], eax

    mov edi, [esi + PROC_TSS_BASE]

    mov eax, [esi + PROC_KSTACK_TOP]
    mov [edi + TSS_ESP0], eax
    mov word [edi + TSS_SS0], KERNEL_DS

    mov eax, [esi + PROC_PAGE_DIR]
    mov [edi + TSS_CR3], eax

    mov dword [edi + TSS_EIP], V86_ENTRY_IP
    mov dword [edi + TSS_EFLAGS], EFLAGS_RESERVED or EFLAGS_IF or EFLAGS_VM or EFLAGS_VIF

    mov dword [edi + TSS_ESP], V86_STACK_SP
    mov dword [edi + TSS_EBP], V86_STACK_SP

    mov word [edi + TSS_CS], V86_ENTRY_CS
    mov word [edi + TSS_SS], V86_STACK_SS
    mov word [edi + TSS_DS], 0
    mov word [edi + TSS_ES], 0
    mov word [edi + TSS_FS], 0
    mov word [edi + TSS_GS], 0
    mov word [edi + TSS_LDTR], 0
    mov word [edi + TSS_IO_MAP_BASE], V86_IO_BITMAP_BASE

    push V86_TSS_LIMIT
    push dword [esi + PROC_TSS_BASE]
    call gdt_create_tss_descriptor_with_limit
    jc v86_process_create_tss_free_stack

    mov [esi + PROC_TSS_SELECTOR], eax

    pop edi
    pop esi
    pop ecx
    pop ebx
    pop ebp
    clc
    ret 4

v86_process_create_tss_free_stack:
    mov eax, [esi + PROC_KSTACK_TOP]
    sub eax, PAGE_SIZE
    push eax
    call heap_free
    mov dword [esi + PROC_KSTACK_TOP], 0

v86_process_create_tss_free_tss:
    push dword [esi + PROC_TSS_BASE]
    call heap_free
    mov dword [esi + PROC_TSS_BASE], 0

v86_process_create_tss_fail:
    xor eax, eax
    pop edi
    pop esi
    pop ecx
    pop ebx
    pop ebp
    stc
    ret 4

; create_kernel_tss()
; returns EAX = kernel TSS selector, CF clear
; returns EAX = 0, CF set
create_kernel_tss:
    push ebx
    push ecx
    push edi

    push TSS32_SIZE
    call heap_alloc
    test eax, eax
    jz create_kernel_tss_fail

    mov [kernel_tss_base], eax

    mov edi, eax
    xor eax, eax
    mov ecx, TSS32_SIZE / 4
    cld
    rep stosd

    mov edi, [kernel_tss_base]

    ; CR3 and LDTR are static TSS fields: a task switch does not save them.
    mov eax, cr3
    mov [edi + TSS_CR3], eax
    sldt ax
    mov [edi + TSS_LDTR], ax

    mov dword [edi + TSS_ESP0], 03000F000h
    mov word  [edi + TSS_SS0], KERNEL_DS

    mov word [edi + TSS_IO_MAP_BASE], TSS32_SIZE

    push dword [kernel_tss_base]
    call gdt_create_tss_descriptor
    jc create_kernel_tss_free_tss

    mov [kernel_tss_selector], eax
    mov [SCHEDULER_KERNEL_TSS_SELECTOR], eax

    ltr ax

    pop edi
    pop ecx
    pop ebx
    clc
    ret

create_kernel_tss_free_tss:
    push dword [kernel_tss_base]
    call heap_free

create_kernel_tss_fail:
    xor eax, eax
    pop edi
    pop ecx
    pop ebx
    stc
    ret


; ================================= HEAP_STUFF ====================================



; process_heap_grow(process_memory)
; [ebp+8] = process memory object pointer
; returns EAX = newly mapped user heap virtual page, CF clear
; returns EAX = 0, CF set on failure
process_heap_grow:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_heap_grow_fail

    mov edx, [esi + PROC_HEAP_CURRENT]
    cmp edx, [esi + PROC_HEAP_LIMIT]
    jae process_heap_grow_fail

    call pmm_alloc_page
    test eax, eax
    jz process_heap_grow_fail

    mov ebx, eax

    push ebx
    call process_memory_init_heap_page
    test eax, eax
    jz process_heap_grow_free_phys

    push PAGE_PRESENT_RW_USER or PAGE_OWNED
    push ebx
    push edx
    push dword [esi + PROC_PAGE_DIR]
    call map_user_page
    jc process_heap_grow_free_phys

    mov eax, edx
    add dword [esi + PROC_HEAP_CURRENT], PAGE_SIZE

    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    clc
    ret 4

process_heap_grow_free_phys:
    push ebx
    call pmm_free_page

process_heap_grow_fail:
    xor eax, eax
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    stc
    ret 4

int3
int3
; process_memory_destroy(process_memory)
; [ebp+8] = process memory object pointer
process_memory_destroy:
    push ebp
    mov ebp, esp
    push esi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_memory_destroy_done

    push dword [esi + PROC_PAGE_DIR]
    call destroy_page_directory

    push esi
    call heap_free

process_memory_destroy_done:
    pop esi
    pop ebp
    ret 4

 int3
 int3
; ====== print debug text in vga text mode ============

ptext:
      push ebp
      mov ebp,esp
      push eax
      push ebx
      push edx
      xor eax,eax
      mov ebx,dword [ebp+8]  ; ebx now has the address of the string to be printed
      mov edx,0b8000h        ; edx points to the vga text buffer
ptext_loop:
      mov al,byte [ebx]
      mov byte [edx],al
      add ebx,1
      cmp byte [ebx],0
      je ptext_end
      add edx,2
      jmp ptext_loop
ptext_end:
      pop edx
      pop ebx
      pop eax
      pop ebp
      ret 4


;====== Miscellaneous ==========

SCHEDULER_TASK_COUNT     = 0FF100000h
SCHEDULER_CURRENT_INDEX  = 0FF100004h
SCHEDULER_FAR_PTR        = 0FF100008h
SCHEDULER_FAR_PTR_SEL    = 0FF10000Ch
SCHEDULER_TASK_TABLE     = 0FF100010h
SCHEDULER_KERNEL_TSS_SELECTOR = 0FF1000D8h
SCHEDULER_REAP_PROCESS   = 0FF1000DCh
SCHEDULER_ENTRY_SIZE     = 2
MAX_TASKS                = PROCESS_MAX_COUNT
process_active_count dd 0

process_table:
    times PROCESS_MAX_COUNT dd 0


scheduler_init:
    push eax
    push ecx
    push edi

    mov dword [SCHEDULER_TASK_COUNT], 0
    mov dword [SCHEDULER_CURRENT_INDEX], 0FFFFFFFFh
    mov dword [SCHEDULER_FAR_PTR], 0
    mov dword [SCHEDULER_FAR_PTR + 4], 0
    mov dword [SCHEDULER_KERNEL_TSS_SELECTOR], 0
    mov dword [SCHEDULER_REAP_PROCESS], 0

    mov edi, SCHEDULER_TASK_TABLE
    xor eax, eax
    mov ecx, (MAX_TASKS * SCHEDULER_ENTRY_SIZE) / 4
    cld
    rep stosd

    pop edi
    pop ecx
    pop eax
    ret


; scheduler_add_process(process)
; [ebp+8] = process object pointer
; returns EAX = 1 on success, EAX = 0 on failure
scheduler_add_process:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi

    mov esi, [ebp + 8]
    test esi, esi
    jz scheduler_add_process_fail

    movzx edx, word [esi + PROC_TSS_SELECTOR]
    test edx, edx
    jz scheduler_add_process_fail

    mov ecx, [SCHEDULER_TASK_COUNT]
    cmp ecx, MAX_TASKS
    jae scheduler_add_process_fail

scheduler_add_process_scan_begin:
    xor ecx, ecx

scheduler_add_process_scan:
    cmp ecx, MAX_TASKS
    jae scheduler_add_process_fail

    lea ebx, [SCHEDULER_TASK_TABLE + ecx*2]
    cmp word [ebx], dx
    je scheduler_add_process_fail
    cmp word [ebx], 0
    je scheduler_add_process_found

    inc ecx
    jmp scheduler_add_process_scan

scheduler_add_process_found:
    mov [ebx], dx

    inc dword [SCHEDULER_TASK_COUNT]

    mov eax, 1
    jmp scheduler_add_process_done

scheduler_add_process_fail:
    xor eax, eax

scheduler_add_process_done:
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 4


; scheduler_remove_process(process)
; [ebp+8] = process object pointer
; returns EAX = 1 when removed or not runnable
; returns EAX = 0 when asked to remove the currently running task
scheduler_remove_process:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi

    mov esi, [ebp + 8]
    test esi, esi
    jz scheduler_remove_process_fail

    movzx edx, word [esi + PROC_TSS_SELECTOR]
    test edx, edx
    jz scheduler_remove_process_success

    xor ecx, ecx

scheduler_remove_process_scan:
    cmp ecx, MAX_TASKS
    jae scheduler_remove_process_success

    lea ebx, [SCHEDULER_TASK_TABLE + ecx*2]
    cmp word [ebx], dx
    je scheduler_remove_process_found

    inc ecx
    jmp scheduler_remove_process_scan

scheduler_remove_process_found:
    cmp ecx, [SCHEDULER_CURRENT_INDEX]
    je scheduler_remove_process_fail

    mov word [ebx], 0

    cmp dword [SCHEDULER_TASK_COUNT], 0
    je scheduler_remove_process_success
    dec dword [SCHEDULER_TASK_COUNT]

    cmp dword [SCHEDULER_TASK_COUNT], 0
    jne scheduler_remove_process_success
    mov dword [SCHEDULER_CURRENT_INDEX], 0FFFFFFFFh

scheduler_remove_process_success:
    mov eax, 1
    jmp scheduler_remove_process_done

scheduler_remove_process_fail:
    xor eax, eax

scheduler_remove_process_done:
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 4


process_table_init:
    push eax
    push ecx
    push edi

    mov dword [process_active_count], 0

    mov edi, process_table
    xor eax, eax
    mov ecx, PROCESS_MAX_COUNT
    cld
    rep stosd

    pop edi
    pop ecx
    pop eax
    ret

; process_register(process)
; [ebp+8] = process object pointer
; returns EAX = 1 on success, EAX = 0 on failure
process_register:
    push ebp
    mov ebp, esp
    push ebx
    push esi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_register_fail

    cmp dword [esi + PROC_TABLE_INDEX], 0FFFFFFFFh
    jne process_register_fail

    xor ebx, ebx

process_register_scan:
    cmp ebx, PROCESS_MAX_COUNT
    jae process_register_fail
    cmp dword [process_table + ebx*4], 0
    je process_register_found
    inc ebx
    jmp process_register_scan

process_register_found:
    mov [process_table + ebx*4], esi
    mov [esi + PROC_TABLE_INDEX], ebx
    inc dword [process_active_count]

    mov eax, 1
    jmp process_register_done

process_register_fail:
    xor eax, eax

process_register_done:
    pop esi
    pop ebx
    pop ebp
    ret 4

; process_unregister(process)
; [ebp+8] = process object pointer
process_unregister:
    push ebp
    mov ebp, esp
    push eax
    push ebx
    push esi

    mov esi, [ebp + 8]
    test esi, esi
    jz process_unregister_done

    mov ebx, [esi + PROC_TABLE_INDEX]
    cmp ebx, PROCESS_MAX_COUNT
    jae process_unregister_done

    cmp [process_table + ebx*4], esi
    jne process_unregister_done

    mov dword [process_table + ebx*4], 0
    mov dword [esi + PROC_TABLE_INDEX], 0FFFFFFFFh

    cmp dword [process_active_count], 0
    je process_unregister_done
    dec dword [process_active_count]

process_unregister_done:
    pop esi
    pop ebx
    pop eax
    pop ebp
    ret 4

; process_get_by_id(table_index)
; process_get_by_slot is retained as a compatibility alias.
; [ebp+8] = process-table index
; returns EAX = process pointer, or 0
process_get_by_id:
process_get_by_slot:
    push ebp
    mov ebp, esp
    push ebx

    mov ebx, [ebp + 8]
    cmp ebx, PROCESS_MAX_COUNT
    jae process_get_by_slot_fail

    mov eax, [process_table + ebx*4]
    jmp process_get_by_slot_done

process_get_by_slot_fail:
    xor eax, eax

process_get_by_slot_done:
    pop ebx
    pop ebp
    ret 4


; ==== DEBUG STRINGS =========

success_string db 'If you are here, it is a success',0
heap_success db 'heap_success',0
heap_failure db 'heap_failed',0
kernel_tss_val db '2','8',0

; C ABI adapters for assembly routines whose native interface uses CF or EBX.
times (C_KERNEL_API_WRAPPERS - 80000000h) - ($-$$) db 0

kernel_api_map_kernel_vga:
    call map_kernel_vga
    setnc al
    movzx eax, al
    ret

; kernel_api_process_slot_alloc(out_index)
kernel_api_process_slot_alloc:
    push ebp
    mov ebp, esp
    push ebx
    push edx

    mov edx, [ebp + 8]
    test edx, edx
    jz kernel_api_process_slot_alloc_fail

    call process_slot_alloc
    jc kernel_api_process_slot_alloc_fail
    mov [edx], ebx
    jmp kernel_api_process_slot_alloc_done

kernel_api_process_slot_alloc_fail:
    xor eax, eax

kernel_api_process_slot_alloc_done:
    pop edx
    pop ebx
    pop ebp
    ret 4

kernel_api_enable_systemcall:
    call enable_systemcall
    setnc al
    movzx eax, al
    ret

kernel_api_enable_vme:
    call enable_VME
    setnc al
    movzx eax, al
    ret

kernel_api_pmm_init:
    pushad
    call pmm_init
    popad
    ret

; kernel_api_pmm_mark_range_free(start, end)
kernel_api_pmm_mark_range_free:
    push ebp
    mov ebp, esp
    push ebx
    push dword [ebp + 12]
    push dword [ebp + 8]
    call pmm_mark_range_free
    pop ebx
    pop ebp
    ret 8

; kernel_api_pmm_mark_range_used(start, end)
kernel_api_pmm_mark_range_used:
    push ebp
    mov ebp, esp
    push ebx
    push dword [ebp + 12]
    push dword [ebp + 8]
    call pmm_mark_range_used
    pop ebx
    pop ebp
    ret 8

; kernel_api_map_page(virt, phys, flags)
kernel_api_map_page:
    push ebp
    mov ebp, esp
    push dword [ebp + 16]
    push dword [ebp + 12]
    push dword [ebp + 8]
    call map_page
    setnc al
    movzx eax, al
    pop ebp
    ret 12

; kernel_api_map_user_page(page_dir, virt, phys, flags)
kernel_api_map_user_page:
    push ebp
    mov ebp, esp
    push dword [ebp + 20]
    push dword [ebp + 16]
    push dword [ebp + 12]
    push dword [ebp + 8]
    call map_user_page
    setnc al
    movzx eax, al
    pop ebp
    ret 16

kernel_api_test_heap:
    push edi
    call test_heap
    pop edi
    ret

times (C_KERNEL_API_TABLE - 80000000h) - ($-$$) db 0

kernel_api_table:
    dd KERNEL_API_MAGIC
    dd KERNEL_API_VERSION
    dd (kernel_api_table_end - kernel_api_table_functions) / 4
    dd 0

kernel_api_table_functions:
    dd kernel_api_map_kernel_vga
    dd process_create
    dd process_create_protected32
    dd v86_process_create
    dd process_destroy
    dd process_slot_allocator_init
    dd kernel_api_process_slot_alloc
    dd process_slot_free
    dd process_prepare_test_task
    dd process_install_syscall_gateway
    dd kernel_api_test_heap
    dd kernel_api_enable_systemcall
    dd kernel_api_enable_vme
    dd kernel_api_pmm_init
    dd kernel_api_pmm_mark_range_free
    dd kernel_api_pmm_mark_range_used
    dd pmm_alloc_page
    dd pmm_free_page
    dd kernel_api_map_page
    dd heap_init
    dd heap_grow_one_page
    dd heap_grow_pages
    dd heap_alloc
    dd heap_free
    dd heap_coalesce
    dd heap_validate
    dd alloc_page_table
    dd free_page_table
    dd create_page_directory
    dd destroy_page_directory
    dd kernel_api_map_user_page
    dd unmap_page
    dd get_physical_address
    dd process_memory_init_heap_page
    dd process_memory_create
    dd v86_process_memory_create
    dd v86_map_low_memory
    dd gdt_allocator_init
    dd gdt_alloc_selector
    dd gdt_write_raw_descriptor
    dd gdt_free_selector
    dd gdt_write_segment_descriptor
    dd gdt_create_code_descriptor
    dd gdt_create_data_descriptor
    dd gdt_create_ldt_descriptor
    dd gdt_create_tss_descriptor
    dd gdt_create_tss_descriptor_with_limit
    dd ldt_alloc_selector
    dd ldt_write_raw_descriptor
    dd ldt_write_segment_descriptor
    dd process_create_random_ldt
    dd process_create_tss
    dd v86_process_create_tss
    dd create_kernel_tss
    dd process_heap_grow
    dd process_memory_destroy
    dd ptext
    dd scheduler_init
    dd scheduler_add_process
    dd scheduler_remove_process
    dd process_table_init
    dd process_register
    dd process_unregister
    dd process_get_by_id

kernel_api_table_end:

times (exception_dispatch_ptr - 80000000h) - ($-$$) db 0
c_exception_callback dd 0
c_lapic_callback dd 0
c_ps2_keyboard_callback dd 0
keyboard_gate_selector dd 0
keyboard_data_selector dd 0
keyboard_code_selector dd 0
keyboard_ready dd 0
graphics_gate_selector dd 0
graphics_data_selector dd 0
graphics_code_selector dd 0
graphics_ready dd 0
c_storage_callback dd 0
c_usb_host_callback dd 0
c_usb_host_status dd 0
mouse_gate_selector dd 0
mouse_ready dd 0
c_kernel_idle_callback dd 0
c_mouse_cursor_callback dd 0
azalia_gate_selector dd 0
azalia_flat_data_selector dd 0
c_azalia_service dd 0
c_azalia_state dd 0
azalia_gate_entry_pointer dd azalia_gate_entry

; Keep the existing assembly kernel intact and append the separately linked C
; payload at a fixed virtual address inside the 128 KiB reservation.
times C_KERNEL_OFFSET - ($-$$) db 0
file 'Development\Kernel\KernelMain.bin'
times 10000h - ($-$$) db 0
file 'Development\Drivers\Intel915\Intel915Driver.bin'
times 14000h - ($-$$) db 0
file 'Development\Desktop\DesktopProcess.bin'
times 18000h - ($-$$) db 0
file 'Development\Drivers\Keyboard\KeyboardDriver.bin'
times 1C000h - ($-$$) db 0
include 'DriverGateway.asm'
include '../../Drivers/Audio/AzaliaEntry.asm'

times 1D000h - ($-$$) db 0
file 'Development\Drivers\Usb\MouseDriver.bin'
times 256*512 - ($-$$) db 0     ; 128 KiB / 256 sectors
