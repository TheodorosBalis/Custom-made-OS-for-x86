format ELF
use32

section '.text' executable
public keyboard_is_key_down
public keyboard_is_any_key_down
public display_write_cell
public graphics_initialize_desktop
public disk_devices
public disk_get_info
public disk_read_sector
public mouse_read_event
public mouse_get_buttons
public mouse_get_devices
public graphics_get_screen_size
public audio_initialize
public audio_get_status
public audio_get_codec_id
audio_initialize:
    push ebx
    xor ebx, ebx
    jmp audio_request
audio_get_status:
    push ebx
    mov ebx, 1
    jmp audio_request
audio_get_codec_id:
    push ebx
    mov ebx, 2
audio_request:
    mov eax, 13
    call far 003Bh:07FFFF000h
    pop ebx
    ret

graphics_get_screen_size:
    mov eax, 11
    call far 003Bh:07FFFF000h
    ret

mouse_read_event:
    mov eax, 8
    call far 003Bh:07FFFF000h
    ret

mouse_get_buttons:
    mov eax, 9
    call far 003Bh:07FFFF000h
    ret

mouse_get_devices:
    mov eax, 10
    call far 003Bh:07FFFF000h
    ret

keyboard_is_key_down:
    push ebx
    mov ebx, [esp + 8]
    mov eax, 2
    call far 003Bh:07FFFF000h
    pop ebx
    ret 4

keyboard_is_any_key_down:
    mov eax, 3
    call far 003Bh:07FFFF000h
    ret

display_write_cell:
    push ebx
    push edi
    mov edi, [esp + 12]
    mov ebx, [esp + 16]
    mov eax, 1
    call far 003Bh:07FFFF000h
    pop edi
    pop ebx
    ret 8

graphics_initialize_desktop:
    mov eax, 4
    call far 003Bh:07FFFF000h
    ret

disk_devices:
    mov eax, 7
    call far 003Bh:07FFFF000h
    ret

disk_get_info:
    push ebx
    push edi
    mov ebx, [esp + 12]
    mov edi, [esp + 16]
    mov eax, 5
    call far 003Bh:07FFFF000h
    pop edi
    pop ebx
    ret 8

disk_read_sector:
    push ebx
    push esi
    push edi
    mov ebx, [esp + 16]
    mov esi, [esp + 20]
    mov edi, [esp + 24]
    mov eax, 6
    call far 003Bh:07FFFF000h
    pop edi
    pop esi
    pop ebx
    ret 12
