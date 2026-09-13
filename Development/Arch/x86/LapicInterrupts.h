#ifndef LAPIC_INTERRUPTS_H
#define LAPIC_INTERRUPTS_H

#include "Arch/x86/Exceptions.h"

#define LAPIC_CALLBACK_ADDRESS 0x80002324u

#define LAPIC_LINT0_VECTOR     0x21u
#define LAPIC_LINT1_VECTOR     0x22u
#define LAPIC_ERROR_VECTOR     0x23u
#define LAPIC_SPURIOUS_VECTOR  0xFFu

extern volatile uint32_t lapic_lint0_count;
extern volatile uint32_t lapic_lint1_count;
extern volatile uint32_t lapic_error_count;
extern volatile uint32_t lapic_spurious_count;
extern volatile uint32_t lapic_last_error_status;

void lapic_interrupt_initialize(void);
void lapic_interrupt_dispatch(const struct exception_frame *frame);

#endif
