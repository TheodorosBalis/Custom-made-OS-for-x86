#ifndef KEYBOARD_SHARED_H
#define KEYBOARD_SHARED_H

#include "Include/KernelTypes.h"

#define KEYBOARD_DRIVER_BASE       0x70000000u
#define KEYBOARD_DRIVER_SIZE       0x00100000u
#define KEYBOARD_QUEUE_OFFSET      0x00008000u
#define KEYBOARD_STACK_OFFSET      0x00010000u
#define KEYBOARD_STACK_SIZE        0x00002000u
#define KEYBOARD_QUEUE_SIZE        256u
#define KEYBOARD_QUEUE_MASK        255u

#define KEYBOARD_GATE_ADDRESS      0x8000232Cu
#define KEYBOARD_DATA_ADDRESS      0x80002330u
#define KEYBOARD_CODE_ADDRESS      0x80002334u
#define KEYBOARD_READY_ADDRESS     0x80002338u

#define KEYBOARD_QUERY_KEY         0u
#define KEYBOARD_QUERY_ANY         1u
#define KEYBOARD_PROCESS_INPUT     2u

/* Ring 0 owns head/dropped/bytes. Ring 2 owns tail and decoded state.
   The lock serializes ring-2 callers; the IRQ producer never acquires it. */
struct keyboard_shared {
    volatile uint32_t lock;
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t dropped;
    volatile uint32_t last_cpl;
    volatile uint32_t calls;
    uint32_t observed_dropped;
    uint32_t extended;
    uint32_t pause_remaining;
    uint32_t down_count;
    uint8_t down[256];
    volatile uint8_t bytes[KEYBOARD_QUEUE_SIZE];
};

#define KERNEL_KEYBOARD_QUEUE \
    ((struct keyboard_shared *)(KEYBOARD_DRIVER_BASE + KEYBOARD_QUEUE_OFFSET))

_Static_assert(sizeof(struct keyboard_shared) <= 4096u,
               "Keyboard queue must fit in one page");
_Static_assert(KEYBOARD_STACK_OFFSET + 100u * KEYBOARD_STACK_SIZE <=
               KEYBOARD_DRIVER_SIZE, "Driver stack slots exceed reservation");

#endif
