#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include "Include/KernelTypes.h"

#define EXCEPTION_CALLBACK_ADDRESS 0x80002320u
#define EFLAGS_VM                   0x00020000u

struct exception_frame {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t saved_esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;
    uint32_t vector;
    uint32_t error_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t user_esp;
    uint32_t user_ss;
};

void exception_initialize(void);
void exception_dispatch(const struct exception_frame *frame);

#endif
