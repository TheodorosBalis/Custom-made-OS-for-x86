use32
ORG 0x30000000             ; entry point
include 'BootLayout.inc'
bootstrap_load_images = 30001000h ; Boot/BootStorage.c entry, placed by boot-storage.ld


; ==================kernel setup section=============================

SETUP_LOAD_PHYS:

cli
cld
mov ebp, 3000F000h
mov esp, ebp
sub esp,4

call init_gdt
jmp far 0008:PmodeMain

PmodeMain:

mov ax, 010h
mov ds, ax
mov es, ax
mov fs, ax
mov gs, ax
mov ss, ax
and esp, -16
call bootstrap_install_idt
call setiopl
call disable_8259_pic
mov eax, bootstrap_load_images
call eax                   ; returns after both payload CRC checks
call enable_local_apic
call read_ioapic_info
call mask_all_ioapic_entries
call route_all_irqs
call setup_paging
call init_idt              ; full handlers are now loaded AND virtually mapped
mov eax, HIGH_KERNEL_VIRT
jmp eax

; InterruptHandlers.bin is not in RAM yet. Any early CPU fault must stop locally.
bootstrap_install_idt:
    xor ecx, ecx
bootstrap_install_idt_next:
    mov eax, bootstrap_fault
    mov bx, KERNEL_CS
    mov dl, INT_GATE_ATTR
    push ecx
    call set_idt_entry
    pop ecx
    inc ecx
    cmp ecx, 256
    jne bootstrap_install_idt_next
    lidt [idtr_]
    ret

bootstrap_fault:
    cli
    mov word [0B8000h], 4F21h ; early bootstrap exception, before C panic is available
    hlt
    jmp bootstrap_fault















;============== Set Up Paging =================
setup_paging:
    cld
    mov edi, PAGE_DIR_BASE
    xor eax, eax
    mov ecx, 1024
    rep stosd

    ; identity map current bootstrap region
    mov dword [PAGE_DIR_BASE + ((BOOTSTRAP_PHYS shr 22) * 4)], BOOTSTRAP_PHYS or PDE_PRESENT_RW_PS

    ; map high-half kernel virtual -> physical
    mov dword [PAGE_DIR_BASE + ((HIGH_KERNEL_VIRT shr 22) * 4)], HIGH_KERNEL_PHYS or PDE_PRESENT_RW_PS

    ; map 0-0x3FFFFF to physical 0-0x3FFFFF
    mov dword [PAGE_DIR_BASE + ((0x00000000 shr 22) * 4)], 0x00000000 or 0x00000083

    ; map 1:1 physical-virtual page at 0xFEC0000
    mov dword [PAGE_DIR_BASE + ((0xFEC00000 shr 22) * 4)], 0xFEC00000 or PDE_PRESENT_RW_PS

    ; map interrupt handlers at 0xFF000000 -> physical 0x32000000
    mov dword [PAGE_DIR_BASE + ((INTERRUPT_VIRT_BASE shr 22) * 4)], INTERRUPT_PHYS_BASE or PDE_PRESENT_RW_PS

    ; map kernel bookkeeping memory at 0x30400000
    mov dword [PAGE_DIR_BASE + ((0x30400000 shr 22) * 4)], 0x30400000 or PDE_PRESENT_RW_PS

    ; enable 4 MiB pages
    mov eax, cr4
    or eax, 0x10
    mov cr4, eax

    mov eax, PAGE_DIR_BASE
    mov cr3, eax

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ret


; set IOPL (io port instructions privilege level) to 0

setiopl:
pushfd
pop eax
and eax,0FFFFCFFFh
push eax
popfd
ret

; Set up the full 8192-entry GDT with only kernel code and data entries at first

init_gdt:
    cld
    mov edi, GDT_BASE
    xor eax, eax
    mov ecx, GDT_SIZE / 4
    rep stosd                    ; zero all 8192 entries

    ; entry 0 stays null

    ; kernel code selector = 0x08 (index 1)
    mov ecx, 1
    mov eax, 0                   ; base
    mov edx, 0x000FFFFF          ; 4 GiB with granularity
    mov bl, 10011010b            ; access
    mov bh, 11000000b            ; flags in high nibble: G=1, D=1
    call set_gdt_entry

    ; kernel data selector = 0x10 (index 2)
    mov ecx, 2
    mov eax, 0
    mov edx, 0x000FFFFF
    mov bl, 10010010b
    mov bh, 11000000b
    call set_gdt_entry

    ; interrupt code selector = 0x18 (index 3)
    mov ecx, 3
    mov eax, 0
    mov edx, 0x000FFFFF
    mov bl, 10011010b
    mov bh, 11000000b
    call set_gdt_entry

   ; interrupt data selector = 0x20 (index 4)
    mov ecx, 4
    mov eax, 0
    mov edx, 0x000FFFFF
    mov bl, 10010010b
    mov bh, 11000000b
    call set_gdt_entry

    ; SYSENTER kernel code selector = 0x28 (index 5)
    mov ecx, 5
    mov eax, 0
    mov edx, 0x000FFFFF
    mov bl, 10011010b
    mov bh, 11000000b
    call set_gdt_entry

    ; SYSENTER kernel stack/data selector = 0x30 (index 6)
    mov ecx, 6
    mov eax, 0
    mov edx, 0x000FFFFF
    mov bl, 10010010b
    mov bh, 11000000b
    call set_gdt_entry

    ; SYSEXIT user code selector = 0x3B (index 7, RPL 3)
    mov ecx, 7
    mov eax, 0
    mov edx, 0x000FFFFF
    mov bl, 11111010b
    mov bh, 11000000b
    call set_gdt_entry

    ; SYSEXIT user stack/data selector = 0x43 (index 8, RPL 3)
    mov ecx, 8
    mov eax, 0
    mov edx, 0x000FFFFF
    mov bl, 11110010b
    mov bh, 11000000b
    call set_gdt_entry

    mov ebx, gdtr_
    lgdt [ebx]
    ret


    ret

; ==== creates an entry in the GDT at specified position ======
; ECX = GDT index
; EAX = base
; EDX = limit (20-bit effective limit if granularity used)
; BL  = access byte
; BH  = flags high nibble already positioned, e.g. 11000000b
    set_gdt_entry:
    lea edi, [GDT_BASE + ecx*8]

    mov word [edi+0], dx         ; limit 15:0
    mov word [edi+2], ax         ; base 15:0

    shr eax, 16
    mov byte [edi+4], al         ; base 23:16

    mov byte [edi+5], bl         ; access

    mov al, dh                   ; limit 23:16 is now in low nibble of DH
    and al, 0x0F
    or  al, bh                   ; add flags in high nibble
    mov byte [edi+6], al

    mov byte [edi+7], ah         ; base 31:24

    ret

; =======init IDT code ==========

init_idt:
    cld
    mov edi, IDT_BASE
    xor eax, eax
    mov ecx, IDT_SIZE / 4
    rep stosd
        xor ecx, ecx                  ; vector = 0

.make_entries:
    mov eax, ecx
    shl eax, 12                   ; * 0x1000
    add eax, HANDLER_BASE         ; handler linear address

    mov bx, INTERRUPT_CS
    mov dl, INT_GATE_ATTR
    push ecx
    call set_idt_entry
    pop ecx

    inc ecx
    cmp ecx, 256
    jne .make_entries
    mov ebx, idtr_
    lidt [ebx]
    ret

      ; ECX = vector
      ; EAX = handler linear address
      ; BX  = selector
      ; DL  = attribute
set_idt_entry:
    lea edi, [IDT_BASE + ecx*8]

    mov word [edi+0], ax
    mov word [edi+2], bx
    mov byte [edi+4], 0
    mov byte [edi+5], dl
    shr eax, 16
    mov word [edi+6], ax
    ret

;==================== local APIC==============================

enable_local_apic:
    mov eax, 1
    cpuid
    test edx, 1 shl 9
    jz .no_apic

    ; enable APIC globally in IA32_APIC_BASE MSR
    mov ecx, APIC_BASE_MSR
    rdmsr
    or eax, APIC_ENABLE_BIT
    wrmsr

    ; accept all priorities for now
    mov dword [LOCAL_APIC_PHYS + LAPIC_TPR], 0

    ; software-enable LAPIC, spurious vector = 0xFF
    mov eax, SPURIOUS_VECTOR or APIC_SW_ENABLE
    mov dword [LOCAL_APIC_PHYS + LAPIC_SVR], eax

    ; keep legacy local interrupt pins masked during bring-up
    mov dword [LOCAL_APIC_PHYS + LAPIC_LVT_LINT0], LVT_MASKED
    mov dword [LOCAL_APIC_PHYS + LAPIC_LVT_LINT1], LVT_MASKED

    ; clear any pending in-service state
    mov dword [LOCAL_APIC_PHYS + LAPIC_EOI], 0
    ret

.no_apic:
    cli
.hang:
    hlt
    jmp .hang


;============Disable legacy PIC  ==================

disable_8259_pic:
    ; start PIC initialization
    mov al, ICW1_INIT
    out PIC1_CMD, al
    out PIC2_CMD, al

    ; remap master to 0x50, slave to 0x58
    mov al, 050h
    out PIC1_DATA, al
    mov al, 058h
    out PIC2_DATA, al

    ; wiring: slave on IRQ2
    mov al, 04h
    out PIC1_DATA, al
    mov al, 02h
    out PIC2_DATA, al

    ; 8086 mode
    mov al, ICW4_8086
    out PIC1_DATA, al
    out PIC2_DATA, al

    ; mask all IRQs on both PICs
    mov al, 0FFh
    out PIC1_DATA, al
    out PIC2_DATA, al
    ret
;================IO APIC====================

IO_APIC_PHYS       = 0FEC00000h
IOREGSEL           = 00h
IOWIN              = 10h
IOAPIC_REG_ID      = 00h
IOAPIC_REG_VER     = 01h
IOAPIC_REDTBL_BASE = 10h



read_ioapic_info:
    mov eax, IOAPIC_REG_ID
    call ioapic_read
    mov esi, eax              ; IOAPIC ID register value

    mov eax, IOAPIC_REG_VER
    call ioapic_read
    mov edi, eax              ; IOAPIC version register value
    ret




ioapic_read:
    ; EAX = IOAPIC register index
    mov dword [IO_APIC_PHYS + IOREGSEL], eax
    mov eax, dword [IO_APIC_PHYS + IOWIN]
    ret

ioapic_write:
    ; EAX = IOAPIC register index
    ; EDX = value
    mov dword [IO_APIC_PHYS + IOREGSEL], eax
    mov dword [IO_APIC_PHYS + IOWIN], edx
    ret

;============= Mask IO APIC entries==========

mask_all_ioapic_entries:

    call get_ioapic_max_redir
    mov ecx, eax          ; max entry index
    xor ebx, ebx          ; current IRQ/redir index

.next:
    ; low dword register = 0x10 + irq*2
    mov eax, IOAPIC_REDTBL_BASE
    mov edx, ebx
    shl edx, 1
    add eax, edx

    ; masked, vector value doesn't matter much yet
    mov edx, 00010000h or 021h
    call ioapic_write

    ; high dword register = 0x10 + irq*2 + 1
    mov eax, IOAPIC_REDTBL_BASE
    mov edx, ebx
    shl edx, 1
    add eax, edx
    inc eax

    ; destination APIC ID = 0 (BSP)
    xor edx, edx
    call ioapic_write

    inc ebx
    cmp ebx, ecx
    jbe .next
    ret

get_ioapic_max_redir:
    mov eax, IOAPIC_REG_VER
    call ioapic_read
    shr eax, 16
    and eax, 0FFh
    ret

;========= map IRQ X -> Vector 20h+X==========================
LAPIC_TIMER_VECTOR   = 020h
LAPIC_TIMER_PERIODIC = 00020000h
LAPIC_LINT0_VECTOR = 021h
LAPIC_LINT1_VECTOR = 022h
LAPIC_ERROR_VECTOR = 023h

route_all_irqs:
    call route_local_apic_irqs
    ret

  map_one_irq:
    push eax
    push ebx
    push ecx
    push edx

    mov ebx, eax                ; EBX = irq line

    ; low dword register = 0x10 + irq*2
    mov eax, 10h
    lea eax, [eax + ebx*2]

    ; vector = 0x20 + irq, fixed delivery, physical mode, unmasked
    mov edx, 20h
    add edx, ebx
    call ioapic_write

    ; high dword register = low_reg + 1
    mov eax, 11h
    lea eax, [eax + ebx*2]

    ; destination APIC ID = 0 (BSP)
    xor edx, edx
    call ioapic_write

    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

route_local_apic_irqs:
    ; divide by 16
    mov dword [LOCAL_APIC_PHYS + 3E0h], 03h

    ; periodic timer on vector 0x20
    mov eax, LAPIC_TIMER_VECTOR or LAPIC_TIMER_PERIODIC
    mov dword [LOCAL_APIC_PHYS + LAPIC_LVT_TIMER], eax

    ; starting count
    mov dword [LOCAL_APIC_PHYS + 380h], 100000h
    ; timer already periodic on 0x20
    mov dword [LOCAL_APIC_PHYS + 3E0h], 03h
    mov dword [LOCAL_APIC_PHYS + 380h], 100000h
    mov dword [LOCAL_APIC_PHYS + LAPIC_LVT_TIMER], LAPIC_TIMER_VECTOR or LAPIC_TIMER_PERIODIC

    ; LINT0 -> 0x21
    mov dword [LOCAL_APIC_PHYS + LAPIC_LVT_LINT0], LAPIC_LINT0_VECTOR

    ; LINT1 -> 0x22
    mov dword [LOCAL_APIC_PHYS + LAPIC_LVT_LINT1], LAPIC_LINT1_VECTOR

    ; Clear stale status, install the APIC error vector, then rearm reporting.
    mov dword [LOCAL_APIC_PHYS + LAPIC_ESR], 0
    mov dword [LOCAL_APIC_PHYS + 370h], LAPIC_ERROR_VECTOR
    mov dword [LOCAL_APIC_PHYS + LAPIC_ESR], 0
    ret




; ====================================ptext=====================================================================

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
; ptext end


; ======== GDT metadata ========

GDT_BASE    = 0x30010000      ; safer than +0x1000
GDT_ENTRIES = 8192
GDT_SIZE    = GDT_ENTRIES * 8
GDT_LIMIT   = GDT_SIZE - 1

gdtr_:
    dw GDT_LIMIT
    dd GDT_BASE

; ========= IDT metadata ========
IDT_BASE    = 0x30020000
IDT_ENTRIES = 256
IDT_SIZE    = IDT_ENTRIES * 8
IDT_LIMIT   = IDT_SIZE - 1

idtr_:
    dw IDT_LIMIT
    dd IDT_BASE

; ========== IDT handler data ==========
IDT_BASE        equ 0x30020000
IDT_ENTRIES     = 256
IDT_SIZE        equ IDT_ENTRIES * 8

HANDLER_BASE    = 0xFF000000
HANDLER_STRIDE  = 0x1000

KERNEL_CS      = 0x08
KERNEL_DS      = 0x10
INTERRUPT_CS   = 0x18
INTERRUPT_DS   = 0x20

INT_GATE_ATTR   = 10001110b

; ========== PAGING info ==============
BOOTSTRAP_PHYS     = 0x30000000
HIGH_KERNEL_PHYS   = 0x34000000
PAGE_DIR_BASE      = 0x30030000
PDE_PRESENT_RW_PS  = 0x00000083
HIGH_KERNEL_VIRT   = 0x80000000
INTERRUPT_PHYS_BASE = 0x32000000
INTERRUPT_VIRT_BASE = 0xFF000000

;============== Local APIC info ==============
LOCAL_APIC_PHYS     = 0FEE00000h

APIC_BASE_MSR       = 1Bh
APIC_ENABLE_BIT     = 00000800h

LAPIC_ID            = 020h
LAPIC_EOI           = 0B0h
LAPIC_SVR           = 0F0h
LAPIC_LVT_TIMER     = 320h
LAPIC_LVT_LINT0     = 350h
LAPIC_LVT_LINT1     = 360h
LAPIC_ESR           = 280h
LAPIC_TPR           = 080h

SPURIOUS_VECTOR     = 0FFh
IRQ0_VECTOR         = 020h
KEYBOARD_VECTOR     = 021h
APIC_SW_ENABLE      = 00000100h
LVT_MASKED          = 00010000h


;=============Legacy PIC============================

PIC1_CMD            = 020h
PIC1_DATA           = 021h
PIC2_CMD            = 0A0h
PIC2_DATA           = 0A1h
PIC_EOI             = 020h
ICW1_INIT           = 011h
ICW4_8086           = 001h


times 800h - ($-$$) db 0
include 'BootManifest.inc'
times 1000h - ($-$$) db 0
if $ <> bootstrap_load_images
    err 'Embedded C bootstrap entry address changed'
end if
file 'Development/Boot/BootStorage.bin'
times SETUP_SECTORS*512 - ($-$$) db 0
