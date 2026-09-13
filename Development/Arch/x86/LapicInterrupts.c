#include "Arch/x86/LapicInterrupts.h"
#include "Include/KernelRuntime.h"

#define LAPIC_BASE_ADDRESS 0xFEE00000u
#define LAPIC_ESR_OFFSET   0x00000280u

typedef void (*lapic_callback_t)(const struct exception_frame *frame);

volatile uint32_t lapic_lint0_count;
volatile uint32_t lapic_lint1_count;
volatile uint32_t lapic_error_count;
volatile uint32_t lapic_spurious_count;
volatile uint32_t lapic_last_error_status;

static volatile uint32_t *lapic_register(uint32_t offset)
{
    return (volatile uint32_t *)(LAPIC_BASE_ADDRESS + offset);
}

void lapic_interrupt_initialize(void)
{
    volatile lapic_callback_t *callback =
        (volatile lapic_callback_t *)LAPIC_CALLBACK_ADDRESS;

    lapic_lint0_count = 0u;
    lapic_lint1_count = 0u;
    lapic_error_count = 0u;
    lapic_spurious_count = 0u;
    lapic_last_error_status = 0u;
    *callback = lapic_interrupt_dispatch;
}

void lapic_interrupt_dispatch(const struct exception_frame *frame)
{
    if (frame == NULL) {
        KPANIC("Null LAPIC interrupt frame");
    }

    switch (frame->vector) {
    case LAPIC_LINT0_VECTOR:
        ++lapic_lint0_count;
        break;

    case LAPIC_LINT1_VECTOR:
        ++lapic_lint1_count;
        break;

    case LAPIC_ERROR_VECTOR:
        ++lapic_error_count;

        /* A write updates, clears, and rearms the xAPIC error status. */
        *lapic_register(LAPIC_ESR_OFFSET) = 0u;
        lapic_last_error_status = *lapic_register(LAPIC_ESR_OFFSET);
        break;

    case LAPIC_SPURIOUS_VECTOR:
        ++lapic_spurious_count;
        break;

    default:
        KPANIC("Unexpected LAPIC interrupt vector");
    }
}
