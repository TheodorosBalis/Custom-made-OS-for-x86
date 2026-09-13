ORG 0x7C00
include 'BootLayout.inc'

use16

 Main:

 jmp far 0000:start       ; set CS to 0 for safe addressing

 start:
  xor ax,ax               ; set all segments to 0000
  mov ds,ax
  mov es,ax
  mov ss,ax
  mov sp,08000h           ; set the stack above code

 call enter_unreal        ; enter this mode to call BIOS APIs
                          ; while keeping 32 bit addressing
 unreal:
  xor bx,bx
  mov ds,bx
  mov es,bx
  mov ss,bx
  cli
  call load_from_drive    ; load kernel in memory
  call exit_unreal        ; leave unreal mode

 call enter_kernel        ; go to kernel



;============== Enter unreal Mode =======================

  enter_unreal:

  call enter_pmode
  and al,0FEh
  mov cr0,eax
  add sp,2
  jmp far 0000:unreal



  enter_pmode:

  lgdt [gdtinfo]
  mov eax,cr0
  or al,1
  mov cr0,eax

  jmp far 0008:pmode

  pmode:

  mov bx,010h
  mov ds,bx
  mov es,bx
  mov ss,bx
  mov fs,bx
  mov gs,bx
  ret


  exit_unreal:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov byte [gdt + 8 + 5], 10011010b
    mov byte [gdt + 8 + 6], 11001111b

    lgdt [gdtinfo]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    mov eax, dword [0x30000000]
    test eax,eax
    jz error
    ret

    error: hlt
      jmp error

    enter_kernel:
     jmp far 0008:goto_kernel


  ; GDT for brief protected and unreal mode
  gdtinfo:
  dw gdt_eend-gdt-1
  dd gdt

  gdt dd 0,0
  flatcode db 0xff,0xff,0,0,0,10011010b,10001111b,0
  flatdata db 0xff,0xff,0,0,0,10010010b,11001111b,0
  gdt_eend:


; ======== Load From Drive Main Function ==============

; ================= constants =================

SETUP_LOAD_PHYS        = 0x30000000

SECTOR_SIZE            = 512
BOUNCE_BUFFER          = 0x9000

boot_drive             db 0
sectors_per_track      dw 0
head_count             dw 0




 load_from_drive:

    call is_A20_on
    mov [boot_drive], dl
    call detect_drive_geometry

    push dword SETUP_LOAD_PHYS
    push word  SETUP_SECTORS
    push word  SETUP_START_LBA
    call LoadFromDrive

    cli
    ret


detect_drive_geometry:
    mov dl, [boot_drive]
    mov ah, 08h
    int 13h
    jc disk_error

    xor ax, ax
    mov al, cl
    and al, 3Fh
    mov [sectors_per_track], ax

    xor ax, ax
    mov al, dh
    inc ax
    mov [head_count], ax
    ret
   disk_error: hlt
   jmp disk_error
;=================LoadFrontDrive Univeral function=================

; [bp+4]  = start LBA      (word)
; [bp+6]  = sector count   (word)
; [bp+8]  = dest phys addr (dword)

LoadFromDrive:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di

    mov bx, [bp+4]            ; current LBA
    mov dx, [bp+6]            ; remaining sectors
    mov edi, dword [bp+8]     ; destination address

.next_sector:
    test dx, dx
    jz .done

    push bx
    call ReadOneSectorToBounce
    pop bx

    push dx
    push si
    mov si, BOUNCE_BUFFER
    mov cx, SECTOR_SIZE / 4
.copy_sector:
    mov eax, [si]
    mov [gs:edi], eax
    add si, 4
    add edi, 4
    loop .copy_sector
    pop si
    pop dx

    inc bx
    dec dx
    jmp .next_sector

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret 8

; BX = current LBA
ReadOneSectorToBounce:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    ; LBA -> CHS
    mov ax, bx
    xor dx, dx
    div word [sectors_per_track]    ; AX=temp, DX=sector_index
    mov cl, dl
    inc cl                          ; sector = (LBA % spt) + 1

    xor dx, dx
    div word [head_count]           ; AX=cylinder, DX=head
    mov dh, dl
    mov ch, al
    and ah, 03h
    shl ah, 6
    or cl, ah

    xor ax, ax
    mov es, ax
    mov bx, BOUNCE_BUFFER

    mov di, 3
.retry:
    mov ah, 02h
    mov al, 1
    mov dl, [boot_drive]
    int 13h
    jnc .ok

    mov ah, 00h
    mov dl, [boot_drive]
    int 13h
    dec di
    jnz .retry
    jmp disk_error

.ok:
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

    is_A20_on:

    pusha
    mov edi,0x112345  ;odd megabyte address.
    mov esi,0x012345  ;even megabyte address.
    mov [esi],esi     ;making sure that both addresses contain diffrent values.
    mov [edi],edi     ;(if A20 line is cleared the two pointers would point to the address 0x012345 that would contain 0x112345 (edi))
    cmpsd             ;compare addresses to see if the're equivalent.
    popa
    jne A20_on        ;if not equivalent , A20 line is set.
    in al, 0x92
    or al, 2
    out 0x92, al     ;if equivalent , the A20 line is cleared.
    A20_on: ret



use32
   goto_kernel:
   mov eax,SETUP_LOAD_PHYS
   jmp eax                ; Kernel Setup Entry



times 510 - ($-$$) db 0
dw 0aa55h
