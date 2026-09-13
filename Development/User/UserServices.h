#ifndef USER_SERVICES_H
#define USER_SERVICES_H

#include "Include/KernelTypes.h"

/* Applications link UserServices.o. Both calls preserve stdcall registers. */
uint32_t __attribute__((stdcall)) keyboard_is_key_down(uint32_t key);
uint32_t __attribute__((stdcall)) keyboard_is_any_key_down(void);
uint32_t __attribute__((stdcall)) display_write_cell(uint32_t cell, uint32_t character);
uint32_t __attribute__((stdcall)) graphics_initialize_desktop(void);
/* Screen size: width in low 16 bits, height in high 16 bits. */
uint32_t __attribute__((stdcall)) graphics_get_screen_size(void);
/* Query-only audio bring-up. No DMA playback, volume changes or raw codec verbs. */
uint32_t __attribute__((stdcall)) audio_initialize(void);
uint32_t __attribute__((stdcall)) audio_get_status(void);
uint32_t __attribute__((stdcall)) audio_get_codec_id(void);

/* One desktop consumes events. Buttons reflect reports it has consumed. */
uint32_t __attribute__((stdcall)) mouse_read_event(void);
uint32_t __attribute__((stdcall)) mouse_get_buttons(void);
uint32_t __attribute__((stdcall)) mouse_get_devices(void);
#define USER_MOUSE_VALID 0x80000000u
#define USER_MOUSE_LOST  0x40000000u
#define USER_MOUSE_BUSY  0x20000000u
#define USER_MOUSE_BAD_REQUEST 0x10000000u
#define USER_MOUSE_BUTTONS(event) ((event) & 7u)
#define USER_MOUSE_DX(event) ((int32_t)(int8_t)((event) >> 8))
#define USER_MOUSE_DY(event) ((int32_t)(int8_t)((event) >> 16))
#define USER_MOUSE_CONTROLLER(event) (((event) >> 24) & 3u)


struct storage_device_info;
/* Requires a kernel raw-read grant. Buffers must reside in the user DATA
 * segment, not the separately based stack segment. No disk write service. */
uint32_t __attribute__((stdcall)) disk_devices(void);
uint32_t __attribute__((stdcall)) disk_get_info(uint32_t device, struct storage_device_info *buffer);
uint32_t __attribute__((stdcall)) disk_read_sector(uint32_t device, uint32_t lba, void *buffer);

/* Set-1 key IDs; extended keys use 0x80 + their base scan code. */
#define KEY_A            0x1Eu
#define KEY_B            0x30u
#define KEY_LEFT_SHIFT   0x2Au
#define KEY_RIGHT_CTRL   0x9Du

#endif
