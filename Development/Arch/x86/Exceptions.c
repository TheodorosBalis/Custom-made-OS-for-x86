#include "Arch/x86/Exceptions.h"
#include "Include/KernelRuntime.h"

typedef void (*exception_callback_t)(const struct exception_frame *frame);

static const char *const exception_names[32] = {
    "Division Error",
    "Debug",
    "Non-maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "BOUND Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

_Static_assert(sizeof(struct exception_frame) == 76u,
               "Assembly exception frame layout mismatch");

void exception_initialize(void)
{
    volatile exception_callback_t *callback =
        (volatile exception_callback_t *)EXCEPTION_CALLBACK_ADDRESS;

    *callback = exception_dispatch;
}

void exception_dispatch(const struct exception_frame *frame)
{
    uint32_t privilege;

    if (frame == NULL || frame->vector >= 32u) {
        KPANIC("Invalid CPU exception frame");
    }

    privilege = frame->cs & 3u;
    if ((frame->eflags & EFLAGS_VM) == 0u &&
        (privilege == 1u || privilege == 2u)) {
        /* A driver can hold shared state needed by every process. */
        kernel_exception_panic(frame, exception_names[frame->vector]);
    }
    if ((frame->eflags & EFLAGS_VM) != 0u || privilege != 0u) {
        return;
    }

    kernel_exception_panic(frame, exception_names[frame->vector]);
}
