#ifndef PS2_KEYBOARD_H
#define PS2_KEYBOARD_H

#include "Arch/x86/Exceptions.h"

#define PS2_KEYBOARD_CALLBACK_ADDRESS 0x80002328u
#define PS2_KEYBOARD_VECTOR           0x31u

extern volatile uint32_t ps2_keyboard_interrupt_count;
extern volatile uint32_t ps2_keyboard_dropped_count;
extern volatile uint8_t ps2_keyboard_last_scancode;

uint32_t ps2_keyboard_initialize(void);
void ps2_keyboard_interrupt(const struct exception_frame *frame);

#endif
