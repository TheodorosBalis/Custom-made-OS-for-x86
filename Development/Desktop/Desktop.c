#include "Desktop.h"

#if defined(DESKTOP_USER)

#include "User/UserServices.h"
#include "Drivers/Intel915/Intel915.h"

__attribute__((noreturn))
static void desktop_graphics_error(uint32_t result)
{
    display_write_cell(0u, 'G');
    for (uint32_t index = 0; index < 8; ++index) {
        uint32_t digit = (result >> (28u - index * 4u)) & 15u;
        display_write_cell(index + 1u, digit < 10u ? '0' + digit : 'A' + digit - 10u);
    }
    for (;;) __asm__ volatile ("pause");
}

__attribute__((section(".text.desktop_main"), used, noreturn))
void desktop_main(void)
{
    uint32_t result = graphics_initialize_desktop();

    if (result != I915_DRAWN) desktop_graphics_error(result);
    uint32_t screen = graphics_get_screen_size();
    uint32_t width = screen & 0xFFFFu, height = screen >> 16;
    if (width < 320 || width > 2048 || height < 200 || height > 2048)
        desktop_graphics_error(screen);
    audio_initialize(); /* Query the codec in ring 1; unavailable audio is nonfatal. */
    for (;;) {
        for (uint32_t count = 0; count < 32; ++count) {
            uint32_t event = mouse_read_event();
            if (!event || event == USER_MOUSE_BUSY) break;
            if (event == USER_MOUSE_LOST) continue;
            if (!(event & USER_MOUSE_VALID)) break;
            /* Cursor positioning is kernel-owned; desktop event handling goes here. */
        }
        __asm__ volatile ("pause");
    }
}

#elif !defined(DESKTOP_TEST)

#include "Include/KernelApi.h"
#include "Include/KernelRuntime.h"
#include "Drivers/Intel915/Intel915.h"

#define DESKTOP_IMAGE_SOURCE 0x80014000u
#define DESKTOP_IMAGE_SIZE   0x1000u
#define PROCESS_TSS_SELECTOR_OFFSET 44u /* PROC_TSS_SELECTOR in VirtualKernel.asm */

void desktop_create(void)
{
    kernel_process_t process;
    uint32_t selector;
    uint32_t flags;

    __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
    if ((flags & 0x200u) != 0u) {
        KPANIC("Create the boot desktop before enabling interrupts");
    }
    /* process_create also registers and queues it; IF=0 prevents early execution. */
    process = KERNEL_API->process_create(PROCESS_TYPE_PROTECTED32);
    if (process == NULL) {
        KPANIC("Cannot create desktop process");
    }
    if (KERNEL_API->process_prepare_test_task(process,
            (const void *)DESKTOP_IMAGE_SOURCE, DESKTOP_IMAGE_SIZE) == 0u) {
        KERNEL_API->process_destroy(process);
        KPANIC("Cannot load desktop code and stack");
    }
    selector = *(const uint32_t *)((const uint8_t *)process + PROCESS_TSS_SELECTOR_OFFSET);
    if (intel915_claim_desktop(selector) == 0u) {
        KERNEL_API->process_destroy(process);
        KPANIC("Cannot assign graphics ownership to the desktop");
    }
}

#endif
