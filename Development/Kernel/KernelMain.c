#include "Include/KernelApi.h"
#include "Include/KernelRuntime.h"
#include "Arch/x86/Exceptions.h"
#include "Arch/x86/LapicInterrupts.h"
#include "Drivers/Keyboard/Ps2Keyboard.h"
#include "Drivers/Keyboard/KeyboardDriverHost.h"
#include "Drivers/Vga/VgaTextMode.h"
#include "Drivers/Intel915/Intel915.h"
#include "Desktop/Desktop.h"
#include "Drivers/Storage/Storage.h"
#include "Drivers/Usb/UsbHost.h"
#include "Drivers/Usb/UsbMouse.h"
#include "Drivers/Usb/MouseDriver.h"
#include "Drivers/Audio/Azalia.h"

static void kernel_runtime_self_test(void);
static void kernel_check_task_address_space(void);

static void kernel_background_service(void)
{
    usb_mouse_service();
    mouse_cursor_service();
}

__attribute__((section(".text.kernel_main"), used))
void kernel_main(void)
{
    if (KERNEL_API->magic != KERNEL_API_MAGIC) {
        KPANIC("Assembly API magic mismatch");
    }
    if (KERNEL_API->version != KERNEL_API_VERSION) {
        KPANIC("Assembly API version mismatch");
    }
    if (KERNEL_API->entry_count < 64u) {
        KPANIC("Assembly API table is incomplete");
    }

    vga_capture_startup_font();
    exception_initialize();
    kernel_check_task_address_space();
    lapic_interrupt_initialize();
    keyboard_driver_initialize();
    mouse_driver_initialize();
    if (ps2_keyboard_initialize() == 0u) {
        KPANIC("PS/2 keyboard initialization failed");
    }

    kernel_runtime_self_test();
    storage_initialize();
    usb_host_initialize();
    usb_mouse_initialize();
    azalia_initialize();
    *(void (**)(void))KERNEL_IDLE_CALLBACK_ADDRESS = kernel_background_service;
    if (intel915_initialize() != 0u) {
        desktop_create();
    }
}

static void kernel_check_task_address_space(void)
{
    uint16_t selector, ldt;
    uint32_t cr3;
    __asm__ volatile ("str %0" : "=r"(selector));
    __asm__ volatile ("sldt %0" : "=r"(ldt));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    if (!selector || (selector & 7u)) KPANIC("Invalid kernel task selector");
    const uint32_t *descriptor = (const uint32_t *)(0x30010000u + selector);
    uint32_t low = descriptor[0], high = descriptor[1];
    if ((high & 0x0000FF00u) != 0x00008B00u)
        KPANIC("Kernel task descriptor is not a present busy 32-bit TSS");
    uint32_t base = (low >> 16) | ((high & 255u) << 16) | (high & 0xFF000000u);
    if (!base) KPANIC("Kernel TSS has no base address");
    if (*(const uint32_t *)(base + 0x1Cu) != cr3)
        KPANIC("Kernel TSS CR3 does not match the active page directory");
    if (*(const uint16_t *)(base + 0x60u) != ldt)
        KPANIC("Kernel TSS LDTR does not match the active LDT");
}

static void kernel_runtime_self_test(void)
{
    void *allocation;
    uint8_t expected[32];

    KASSERT_MSG(KERNEL_API->heap_validate() != 0u,
                "Heap was corrupt before C runtime test");

    allocation = KERNEL_API->heap_alloc(32u);
    KASSERT_MSG(allocation != NULL, "C runtime test allocation failed");

    memset(allocation, 0xA5, 32u);
    memset(expected, 0xA5, sizeof(expected));
    KASSERT(memcmp(allocation, expected, sizeof(expected)) == 0);
    KASSERT(strlen("kernel") == 6u);
    KASSERT(strcmp("kernel", "kernel") == 0);
    KASSERT(strncmp("kernel", "kern", 4u) == 0);

    KERNEL_API->heap_free(allocation);
    KASSERT_MSG(KERNEL_API->heap_validate() != 0u,
                "Heap was corrupt after C runtime test");
}
