#include "Include/KernelRuntime.h"

size_t strlen(const char *text)
{
    size_t length = 0u;

    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }

    return (int)(uint8_t)*left - (int)(uint8_t)*right;
}

int strncmp(const char *left, const char *right, size_t count)
{
    size_t index;

    for (index = 0; index < count; ++index) {
        uint8_t left_byte = (uint8_t)left[index];
        uint8_t right_byte = (uint8_t)right[index];

        if (left_byte != right_byte) {
            return (int)left_byte - (int)right_byte;
        }
        if (left_byte == 0u) {
            return 0;
        }
    }

    return 0;
}

size_t kstrlcpy(char *destination, const char *source, size_t capacity)
{
    size_t source_length = strlen(source);
    size_t copy_length = source_length;
    size_t index;

    if (capacity == 0u) {
        return source_length;
    }

    if (copy_length >= capacity) {
        copy_length = capacity - 1u;
    }

    for (index = 0; index < copy_length; ++index) {
        destination[index] = source[index];
    }
    destination[copy_length] = '\0';

    return source_length;
}
