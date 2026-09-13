#include "Include/KernelRuntime.h"

void *memset(void *destination, int value, size_t count)
{
    uint8_t *output = (uint8_t *)destination;
    uint8_t byte = (uint8_t)value;
    size_t index;

    for (index = 0; index < count; ++index) {
        output[index] = byte;
    }

    return destination;
}

void *memcpy(void *destination, const void *source, size_t count)
{
    uint8_t *output = (uint8_t *)destination;
    const uint8_t *input = (const uint8_t *)source;
    size_t index;

    for (index = 0; index < count; ++index) {
        output[index] = input[index];
    }

    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    uint8_t *output = (uint8_t *)destination;
    const uint8_t *input = (const uint8_t *)source;
    size_t index;

    if (output == input || count == 0u) {
        return destination;
    }

    if (output < input) {
        for (index = 0; index < count; ++index) {
            output[index] = input[index];
        }
    } else {
        for (index = count; index != 0u; --index) {
            output[index - 1u] = input[index - 1u];
        }
    }

    return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const uint8_t *left_bytes = (const uint8_t *)left;
    const uint8_t *right_bytes = (const uint8_t *)right;
    size_t index;

    for (index = 0; index < count; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return (int)left_bytes[index] - (int)right_bytes[index];
        }
    }

    return 0;
}
