#include "Drivers/Keyboard/KeyboardDriverHost.h"
#include "Drivers/Keyboard/KeyboardShared.h"
#include "Include/KernelApi.h"
#include "Include/KernelRuntime.h"

#define PAGE_DIRECTORY_BASE 0x30030000u
#define DRIVER_IMAGE_SOURCE 0x80018000u
#define DRIVER_IMAGE_SIZE   0x00004000u

static uint32_t driver_code;
static uint32_t driver_data;
static uint32_t driver_gate;

static uint32_t create_driver_call_gate(uint32_t code_selector)
{
    uint32_t gate = KERNEL_API->gdt_alloc_selector();

    if (gate == 0u) {
        return 0u;
    }
    /* Entry offset zero, DPL3 32-bit call gate, no copied parameters. */
    KERNEL_API->gdt_write_raw_descriptor(gate, code_selector << 16, 0xEC00u);
    return gate | 3u;
}

void keyboard_driver_initialize(void)
{
    uint32_t offset;
    uint32_t physical;

    KASSERT_MSG(*(volatile uint32_t *)KEYBOARD_READY_ADDRESS == 0u,
                "Keyboard driver initialized twice");

    /* Build this supervisor PDE before any process copies kernel mappings. */
    for (offset = 0u; offset < KEYBOARD_DRIVER_SIZE; offset += PAGE_SIZE) {
        KASSERT_MSG(KERNEL_API->get_physical_address(PAGE_DIRECTORY_BASE,
                    KEYBOARD_DRIVER_BASE + offset) == 0u,
                    "Keyboard driver address already mapped");
        physical = KERNEL_API->pmm_alloc_page();
        KASSERT_MSG(physical != 0u, "No physical memory for keyboard driver");
        KASSERT_MSG(KERNEL_API->map_page(KEYBOARD_DRIVER_BASE + offset,
                    physical, PAGE_PRESENT | PAGE_RW) != 0u,
                    "Cannot map keyboard driver");
    }
    memset((void *)KEYBOARD_DRIVER_BASE, 0, KEYBOARD_DRIVER_SIZE);
    memcpy((void *)KEYBOARD_DRIVER_BASE, (const void *)DRIVER_IMAGE_SOURCE,
           DRIVER_IMAGE_SIZE);

    driver_code = KERNEL_API->gdt_create_code_descriptor(
        KEYBOARD_DRIVER_BASE, DRIVER_IMAGE_SIZE - 1u, DESCRIPTOR_DPL2);
    driver_data = KERNEL_API->gdt_create_data_descriptor(
        KEYBOARD_DRIVER_BASE, KEYBOARD_DRIVER_SIZE - 1u, DESCRIPTOR_DPL2);
    KASSERT_MSG(driver_code != 0u && driver_data != 0u,
                "Cannot allocate keyboard driver segments");
    driver_gate = create_driver_call_gate(driver_code);
    KASSERT_MSG(driver_gate != 0u, "Cannot allocate keyboard call gate");

    *(volatile uint32_t *)KEYBOARD_GATE_ADDRESS = driver_gate;
    *(volatile uint32_t *)KEYBOARD_DATA_ADDRESS = driver_data | 2u;
    *(volatile uint32_t *)KEYBOARD_CODE_ADDRESS = driver_code | 2u;
    *(volatile uint32_t *)KEYBOARD_READY_ADDRESS = 1u;
}
