#include "Drivers/Keyboard/KeyboardShared.h"

/* DS and SS have the same nonzero base. All C pointers are driver offsets. */
#define KEYBOARD_STATE ((struct keyboard_shared *)KEYBOARD_QUEUE_OFFSET)

void keyboard_decode_byte(struct keyboard_shared *state, uint8_t byte)
{
    uint32_t key;
    uint32_t pressed;

    if (state->pause_remaining != 0u) {
        --state->pause_remaining;
        return;
    }
    if (byte == 0xE1u) {
        /* Set-1 Pause has no release code; do not leave it stuck down. */
        state->pause_remaining = 5u;
        state->extended = 0u;
        return;
    }
    if (byte == 0xE0u) {
        state->extended = 1u;
        return;
    }

    key = byte & 0x7Fu;
    pressed = (byte & 0x80u) == 0u;
    if (state->extended != 0u) {
        state->extended = 0u;
        /* Print Screen includes fake extended Shift bytes. */
        if (key == 0x2Au || key == 0x36u) {
            return;
        }
        key += 128u;
    }
    if (key == 0u || key == 0x7Fu) {
        return;
    }
    if (pressed != 0u && state->down[key] == 0u) {
        state->down[key] = 1u;
        ++state->down_count;
    } else if (pressed == 0u && state->down[key] != 0u) {
        state->down[key] = 0u;
        --state->down_count;
    }
}

static void keyboard_process_queue(struct keyboard_shared *state)
{
    uint32_t remaining = KEYBOARD_QUEUE_SIZE;
    uint32_t index;
    uint32_t tail;
    uint8_t byte;

    if (state->observed_dropped != state->dropped) {
        /* A lost release must not leave a key permanently pressed. */
        state->observed_dropped = state->dropped;
        for (index = 0u; index < 256u; ++index) {
            state->down[index] = 0u;
        }
        state->down_count = 0u;
        state->extended = 0u;
        state->pause_remaining = 0u;
        state->tail = state->head;
    }
    while (remaining != 0u && state->tail != state->head) {
        tail = state->tail;
        byte = state->bytes[tail];
        state->tail = (tail + 1u) & KEYBOARD_QUEUE_MASK;
        keyboard_decode_byte(state, byte);
        --remaining;
    }
}

__attribute__((section(".text.keyboard_driver_service")))
uint32_t keyboard_driver_service(uint32_t operation, uint32_t key)
{
    struct keyboard_shared *state = KEYBOARD_STATE;
    uint32_t busy;
    uint32_t result = 0xFFFFFFFFu;
    uint16_t cs;

    /* Calls can be preempted by the timer. The owner resumes on its next tick. */
    do {
        busy = 1u;
        __asm__ volatile ("xchgl %0, %1"
                          : "+r"(busy), "+m"(state->lock) : : "memory");
        if (busy != 0u) {
            __asm__ volatile ("pause");
        }
    } while (busy != 0u);

    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    state->last_cpl = cs & 3u;
    ++state->calls;
    keyboard_process_queue(state);

    if (operation == KEYBOARD_QUERY_KEY && key < 256u) {
        result = state->down[key];
    } else if (operation == KEYBOARD_QUERY_ANY) {
        result = state->down_count != 0u;
    } else if (operation == KEYBOARD_PROCESS_INPUT) {
        result = 0u;
    }

    __asm__ volatile ("" : : : "memory");
    state->lock = 0u;
    return result;
}
