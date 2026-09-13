#include "Include/KernelRuntime.h"
#include "Arch/x86/Exceptions.h"
#include "Include/KernelApi.h"
#include "Drivers/Vga/VgaTextMode.h"

#define VGA_TEXT_ADDRESS 0x80B80000u
#define VGA_WIDTH        80u
#define VGA_HEIGHT       25u
#define PANIC_ATTRIBUTE  0x4Fu
#define PANIC_BACKTRACE_DEPTH 10u

static volatile uint16_t *const panic_vga =
    (volatile uint16_t *)VGA_TEXT_ADDRESS;
static size_t panic_cursor;

static void panic_put_character(char character)
{
    if (character == '\n') {
        panic_cursor += VGA_WIDTH - (panic_cursor % VGA_WIDTH);
        return;
    }

    if (panic_cursor < VGA_WIDTH * VGA_HEIGHT) {
        panic_vga[panic_cursor] =
            (uint16_t)((uint8_t)character | (PANIC_ATTRIBUTE << 8));
        ++panic_cursor;
    }
}

__attribute__((noinline))
static void panic_put_text(const char *text)
{
    if (text == NULL) {
        return;
    }

    while (*text != '\0') {
        panic_put_character(*text);
        ++text;
    }
}

static void panic_put_decimal(uint32_t value)
{
    char digits[10];
    size_t count = 0u;

    do {
        digits[count] = (char)('0' + (value % 10u));
        value /= 10u;
        ++count;
    } while (value != 0u);

    while (count != 0u) {
        --count;
        panic_put_character(digits[count]);
    }
}

__attribute__((noinline))
static void panic_put_hex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    uint32_t shift;

    panic_put_text("0x");
    for (shift = 28u; shift <= 28u; shift -= 4u) {
        panic_put_character(digits[(value >> shift) & 0x0Fu]);
    }
}

static void panic_put_hex_byte(uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    panic_put_character(digits[value >> 4]);
    panic_put_character(digits[value & 0x0Fu]);
}

static uint32_t panic_read_cr3(void)
{
    uint32_t cr3;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static uint32_t panic_range_is_mapped(uint32_t cr3,
                                      uint32_t address,
                                      uint32_t size)
{
    uint32_t last;

    if (size == 0u) {
        return 1u;
    }

    last = address + size - 1u;
    if (last < address) {
        return 0u;
    }

    if (KERNEL_API->get_physical_address(cr3, address) == 0u) {
        return 0u;
    }

    return KERNEL_API->get_physical_address(cr3, last) != 0u;
}

static void panic_put_instruction_bytes(uint32_t eip)
{
    uint32_t cr3 = panic_read_cr3();
    uint32_t index;

    panic_put_text("\nCODE: ");
    for (index = 0u; index < 16u; ++index) {
        uint32_t address = eip + index;

        if (address < eip ||
            KERNEL_API->get_physical_address(cr3, address) == 0u) {
            panic_put_text("??");
        } else {
            panic_put_hex_byte(*(const volatile uint8_t *)address);
        }
        panic_put_character(' ');
    }
}

static void panic_put_backtrace(const struct exception_frame *frame)
{
    uint32_t cr3 = panic_read_cr3();
    uint32_t frame_pointer = frame->ebp;
    uint32_t depth;

    panic_put_text("\nCALL STACK:");
    panic_put_text("\n#0 ");
    panic_put_hex(frame->eip);

    for (depth = 1u; depth < PANIC_BACKTRACE_DEPTH; ++depth) {
        const volatile uint32_t *stack_frame;
        uint32_t next_frame;
        uint32_t return_address;

        if ((frame_pointer & 3u) != 0u ||
            !panic_range_is_mapped(cr3, frame_pointer, 8u)) {
            break;
        }

        stack_frame = (const volatile uint32_t *)frame_pointer;
        next_frame = stack_frame[0];
        return_address = stack_frame[1];

        if (return_address == 0u) {
            break;
        }

        panic_put_text("\n#");
        panic_put_decimal(depth);
        panic_put_character(' ');
        panic_put_hex(return_address);

        if (next_frame <= frame_pointer) {
            break;
        }
        frame_pointer = next_frame;
    }
}

static void panic_clear_screen(void)
{
    size_t index;
    uint16_t blank = (uint16_t)(' ' | (PANIC_ATTRIBUTE << 8));

    for (index = 0; index < VGA_WIDTH * VGA_HEIGHT; ++index) {
        panic_vga[index] = blank;
    }
    panic_cursor = 0u;
}

__attribute__((noreturn))
void kernel_panic(const char *message, const char *file, uint32_t line)
{
    __asm__ volatile ("cli" ::: "memory");

    vga_restore_text_mode();
    panic_clear_screen();
    panic_put_text("KERNEL PANIC\n");
    panic_put_text(message);
    panic_put_text("\n");
    panic_put_text(file);
    panic_put_character(':');
    panic_put_decimal(line);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

__attribute__((noreturn))
void kernel_exception_panic(const struct exception_frame *frame,
                            const char *name)
{
    uint32_t cr2 = 0u;

    __asm__ volatile ("cli" ::: "memory");
    if (frame->vector == 14u) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    }

    vga_restore_text_mode();
    panic_clear_screen();
    uint32_t privilege = frame->cs & 3u;
    panic_put_text(privilege == 3u ? "DESKTOP USER EXCEPTION\n" :
                   privilege ? "DRIVER EXCEPTION\n" : "KERNEL EXCEPTION\n");
    panic_put_text(name);
    panic_put_text("  VECTOR=");
    panic_put_hex(frame->vector);
    panic_put_text("  ERROR=");
    panic_put_hex(frame->error_code);
    panic_put_text("\nEIP=");
    panic_put_hex(frame->eip);
    panic_put_text("  CS=");
    panic_put_hex(frame->cs);
    panic_put_text("  EFLAGS=");
    panic_put_hex(frame->eflags);
    panic_put_text("\nEAX=");
    panic_put_hex(frame->eax);
    panic_put_text("  EBX=");
    panic_put_hex(frame->ebx);
    panic_put_text("  ECX=");
    panic_put_hex(frame->ecx);
    panic_put_text("  EDX=");
    panic_put_hex(frame->edx);
    panic_put_text("\nESI=");
    panic_put_hex(frame->esi);
    panic_put_text("  EDI=");
    panic_put_hex(frame->edi);
    panic_put_text("  EBP=");
    panic_put_hex(frame->ebp);
    panic_put_text("\nESP=");
    panic_put_hex(privilege ? frame->user_esp : frame->saved_esp + 36u);
    if (privilege) {
        panic_put_text("  SS=");
        panic_put_hex(frame->user_ss);
    }
    if (frame->vector == 14u) {
        panic_put_text("  CR2=");
        panic_put_hex(cr2);
    }
    if (!privilege) {
        panic_put_instruction_bytes(frame->eip);
        panic_put_backtrace(frame);
    } else {
        panic_put_text("\nEIP/ESP are segment offsets; flat code/stack walk skipped.");
    }

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

__attribute__((noreturn))
void kernel_assert_fail(const char *expression,
                        const char *message,
                        const char *file,
                        uint32_t line)
{
    if (message != NULL) {
        kernel_panic(message, file, line);
    }

    kernel_panic(expression, file, line);
}
