#include "Drivers/Keyboard/Ps2Keyboard.h"
#include "Include/KernelRuntime.h"
#include "Drivers/Keyboard/KeyboardShared.h"

#define PS2_DATA_PORT             0x0060u
#define PS2_STATUS_PORT           0x0064u
#define PS2_COMMAND_PORT          0x0064u

#define PS2_STATUS_OUTPUT_FULL    0x01u
#define PS2_STATUS_INPUT_FULL     0x02u
#define PS2_STATUS_AUX_DATA       0x20u

#define PS2_DISABLE_FIRST_PORT    0xADu
#define PS2_DISABLE_SECOND_PORT   0xA7u
#define PS2_ENABLE_FIRST_PORT     0xAEu
#define PS2_READ_CONFIG           0x20u
#define PS2_WRITE_CONFIG          0x60u
#define PS2_TEST_CONTROLLER       0xAAu
#define PS2_TEST_FIRST_PORT       0xABu

#define PS2_CONTROLLER_OK         0x55u
#define PS2_FIRST_PORT_OK         0x00u
#define PS2_KEYBOARD_ENABLE_SCAN  0xF4u
#define PS2_DEVICE_ACK            0xFAu

#define PS2_CONFIG_FIRST_IRQ      0x01u
#define PS2_CONFIG_SECOND_IRQ     0x02u
#define PS2_CONFIG_FIRST_CLOCK    0x10u
#define PS2_CONFIG_SECOND_CLOCK   0x20u
#define PS2_CONFIG_TRANSLATION    0x40u

#define PS2_IO_TIMEOUT            1000000u
#define PS2_QUEUE_SIZE            256u
#define PS2_QUEUE_MASK            (PS2_QUEUE_SIZE - 1u)

#define IOAPIC_BASE_ADDRESS       0xFEC00000u
#define IOAPIC_REG_SELECT_OFFSET  0x00000000u
#define IOAPIC_WINDOW_OFFSET      0x00000010u
#define IOAPIC_VERSION_REGISTER   0x01u
#define IOAPIC_REDTBL_BASE        0x10u
#define IOAPIC_KEYBOARD_GSI       1u

#define LAPIC_BASE_ADDRESS        0xFEE00000u
#define LAPIC_ID_OFFSET           0x00000020u

typedef void (*ps2_keyboard_callback_t)(const struct exception_frame *frame);

volatile uint32_t ps2_keyboard_interrupt_count;
volatile uint32_t ps2_keyboard_dropped_count;
volatile uint8_t ps2_keyboard_last_scancode;

static uint32_t ioapic_read(uint32_t index)
{
    volatile uint32_t *select =
        (volatile uint32_t *)(IOAPIC_BASE_ADDRESS + IOAPIC_REG_SELECT_OFFSET);
    volatile uint32_t *window =
        (volatile uint32_t *)(IOAPIC_BASE_ADDRESS + IOAPIC_WINDOW_OFFSET);

    *select = index;
    return *window;
}

static void ioapic_write(uint32_t index, uint32_t value)
{
    volatile uint32_t *select =
        (volatile uint32_t *)(IOAPIC_BASE_ADDRESS + IOAPIC_REG_SELECT_OFFSET);
    volatile uint32_t *window =
        (volatile uint32_t *)(IOAPIC_BASE_ADDRESS + IOAPIC_WINDOW_OFFSET);

    *select = index;
    *window = value;
}

static uint32_t ps2_route_keyboard_interrupt(void)
{
    volatile uint32_t *lapic_id =
        (volatile uint32_t *)(LAPIC_BASE_ADDRESS + LAPIC_ID_OFFSET);
    uint32_t maximum_redirection;
    uint32_t low_register;

    maximum_redirection = (ioapic_read(IOAPIC_VERSION_REGISTER) >> 16) & 0xFFu;
    if (maximum_redirection < IOAPIC_KEYBOARD_GSI) {
        return 0u;
    }

    low_register = IOAPIC_REDTBL_BASE + IOAPIC_KEYBOARD_GSI * 2u;

    /* Set the physical destination before unmasking the input. */
    ioapic_write(low_register + 1u, *lapic_id & 0xFF000000u);

    /* Fixed delivery, active-high, edge-triggered, vector 0x31. */
    ioapic_write(low_register, PS2_KEYBOARD_VECTOR);
    return 1u;
}

static inline void ps2_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t ps2_in8(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t ps2_wait_input_empty(void)
{
    uint32_t remaining = PS2_IO_TIMEOUT;

    while (remaining != 0u) {
        if ((ps2_in8(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0u) {
            return 1u;
        }
        --remaining;
    }
    return 0u;
}

static uint32_t ps2_wait_output_full(void)
{
    uint32_t remaining = PS2_IO_TIMEOUT;

    while (remaining != 0u) {
        if ((ps2_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0u) {
            return 1u;
        }
        --remaining;
    }
    return 0u;
}

static uint32_t ps2_write_command(uint8_t command)
{
    if (ps2_wait_input_empty() == 0u) {
        return 0u;
    }
    ps2_out8(PS2_COMMAND_PORT, command);
    return 1u;
}

static uint32_t ps2_write_data(uint8_t value)
{
    if (ps2_wait_input_empty() == 0u) {
        return 0u;
    }
    ps2_out8(PS2_DATA_PORT, value);
    return 1u;
}

static uint32_t ps2_read_data(uint8_t *value)
{
    if (value == NULL || ps2_wait_output_full() == 0u) {
        return 0u;
    }
    *value = ps2_in8(PS2_DATA_PORT);
    return 1u;
}

static void ps2_flush_output(void)
{
    uint32_t remaining = PS2_QUEUE_SIZE;

    while (remaining != 0u &&
           (ps2_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0u) {
        (void)ps2_in8(PS2_DATA_PORT);
        --remaining;
    }
}

static uint32_t ps2_write_config(uint8_t configuration)
{
    return ps2_write_command(PS2_WRITE_CONFIG) != 0u &&
           ps2_write_data(configuration) != 0u;
}

uint32_t ps2_keyboard_initialize(void)
{
    volatile ps2_keyboard_callback_t *callback =
        (volatile ps2_keyboard_callback_t *)PS2_KEYBOARD_CALLBACK_ADDRESS;
    uint8_t configuration;
    uint8_t response;

    ps2_keyboard_interrupt_count = 0u;
    ps2_keyboard_dropped_count = 0u;
    ps2_keyboard_last_scancode = 0u;
    KASSERT_MSG(*(volatile uint32_t *)KEYBOARD_READY_ADDRESS != 0u,
                "Keyboard driver queue is not mapped");
    KERNEL_KEYBOARD_QUEUE->head = 0u;
    KERNEL_KEYBOARD_QUEUE->tail = 0u;
    *callback = ps2_keyboard_interrupt;

    if (ps2_write_command(PS2_DISABLE_FIRST_PORT) == 0u ||
        ps2_write_command(PS2_DISABLE_SECOND_PORT) == 0u) {
        return 0u;
    }
    ps2_flush_output();

    if (ps2_write_command(PS2_READ_CONFIG) == 0u ||
        ps2_read_data(&configuration) == 0u) {
        return 0u;
    }

    configuration &= (uint8_t)~(PS2_CONFIG_FIRST_IRQ |
                                PS2_CONFIG_SECOND_IRQ |
                                PS2_CONFIG_TRANSLATION);
    if (ps2_write_config(configuration) == 0u) {
        return 0u;
    }

    if (ps2_write_command(PS2_TEST_CONTROLLER) == 0u ||
        ps2_read_data(&response) == 0u || response != PS2_CONTROLLER_OK) {
        return 0u;
    }

    if (ps2_write_command(PS2_TEST_FIRST_PORT) == 0u ||
        ps2_read_data(&response) == 0u || response != PS2_FIRST_PORT_OK) {
        return 0u;
    }

    if (ps2_write_command(PS2_ENABLE_FIRST_PORT) == 0u) {
        return 0u;
    }

    configuration |= PS2_CONFIG_FIRST_IRQ | PS2_CONFIG_TRANSLATION;
    configuration &= (uint8_t)~(PS2_CONFIG_SECOND_IRQ |
                                PS2_CONFIG_FIRST_CLOCK);
    configuration |= PS2_CONFIG_SECOND_CLOCK;

    if (ps2_write_data(PS2_KEYBOARD_ENABLE_SCAN) == 0u ||
        ps2_read_data(&response) == 0u || response != PS2_DEVICE_ACK ||
        ps2_write_config(configuration) == 0u ||
        ps2_route_keyboard_interrupt() == 0u) {
        return 0u;
    }

    return 1u;
}

void ps2_keyboard_interrupt(const struct exception_frame *frame)
{
    struct keyboard_shared *queue = KERNEL_KEYBOARD_QUEUE;
    uint8_t status;
    uint8_t scancode;
    uint32_t next_head;

    if (frame == NULL || frame->vector != PS2_KEYBOARD_VECTOR) {
        KPANIC("Invalid PS/2 keyboard interrupt frame");
    }

    ++ps2_keyboard_interrupt_count;
    status = ps2_in8(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_OUTPUT_FULL) == 0u) {
        return;
    }

    scancode = ps2_in8(PS2_DATA_PORT);
    if ((status & PS2_STATUS_AUX_DATA) != 0u) {
        return;
    }
    if ((status & 0xC0u) != 0u) {
        ++ps2_keyboard_dropped_count;
        ++queue->dropped;
        return;
    }

    ps2_keyboard_last_scancode = scancode;
    next_head = (queue->head + 1u) & KEYBOARD_QUEUE_MASK;
    if (next_head == queue->tail) {
        ++ps2_keyboard_dropped_count;
        ++queue->dropped;
        return;
    }

    queue->bytes[queue->head] = scancode;
    queue->head = next_head;
}
