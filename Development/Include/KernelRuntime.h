#ifndef KERNEL_RUNTIME_H
#define KERNEL_RUNTIME_H

#include "Include/KernelTypes.h"

struct exception_frame;

void *memset(void *destination, int value, size_t count);
void *memcpy(void *destination, const void *source, size_t count);
void *memmove(void *destination, const void *source, size_t count);
int memcmp(const void *left, const void *right, size_t count);

size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
size_t kstrlcpy(char *destination, const char *source, size_t capacity);

__attribute__((noreturn))
void kernel_panic(const char *message, const char *file, uint32_t line);

__attribute__((noreturn))
void kernel_exception_panic(const struct exception_frame *frame,
                            const char *name);

__attribute__((noreturn))
void kernel_assert_fail(const char *expression,
                        const char *message,
                        const char *file,
                        uint32_t line);

#define KASSERT(expression) \
    ((expression) ? (void)0 : \
     kernel_assert_fail(#expression, NULL, __FILE__, (uint32_t)__LINE__))

#define KASSERT_MSG(expression, message) \
    ((expression) ? (void)0 : \
     kernel_assert_fail(#expression, (message), __FILE__, (uint32_t)__LINE__))

#define KPANIC(message) \
    kernel_panic((message), __FILE__, (uint32_t)__LINE__)

#endif
