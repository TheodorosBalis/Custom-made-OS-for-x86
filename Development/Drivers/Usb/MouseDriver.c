#include "MouseDriver.h"

void mouse_cursor_enqueue(struct mouse_cursor_queue *queue, uint32_t controller,
                          const uint8_t *data, uint32_t length)
{
    if (controller >= MOUSE_CONTROLLER_COUNT || !data || length < 3 ||
        length > MOUSE_REPORT_BYTES || (!data[1] && !data[2])) return;
    uint32_t next = (queue->head + 1) & (MOUSE_CURSOR_QUEUE_SIZE - 1);
    if (next == queue->tail) {
        ++queue->dropped;
        return;
    }
    struct mouse_cursor_packet *packet = &queue->packets[queue->head];
    packet->dx = (int8_t)data[1];
    packet->dy = (int8_t)data[2];
    packet->controller = (uint8_t)controller;
    queue->head = next;
    uint32_t pending = (next - queue->tail) & (MOUSE_CURSOR_QUEUE_SIZE - 1);
    if (pending > queue->peak_pending) queue->peak_pending = pending;
    if (data[1] == 0x7Fu || data[1] == 0x80u || data[2] == 0x7Fu || data[2] == 0x80u)
        ++queue->saturated_reports;
}

void mouse_cursor_cancel(struct mouse_cursor_queue *queue, uint32_t controller)
{
    for (uint32_t i = queue->tail; i != queue->head; i = (i + 1) & (MOUSE_CURSOR_QUEUE_SIZE - 1))
        if (queue->packets[i].controller == controller) queue->packets[i].controller = 255;
}

uint32_t mouse_cursor_drain(struct mouse_cursor_queue *queue, uint32_t connected,
    uint32_t (*submit)(const struct cursor_motion *, uint32_t))
{
    struct cursor_motion motions[I915_CURSOR_BATCH];
    uint32_t tail = queue->tail, count = 0;
    for (uint32_t scanned = 0; scanned < I915_CURSOR_BATCH && tail != queue->head; ++scanned) {
        const struct mouse_cursor_packet *packet = &queue->packets[tail];
        if (packet->controller < MOUSE_CONTROLLER_COUNT && (connected & (1u << packet->controller))) {
            motions[count].dx = packet->dx;
            motions[count++].dy = packet->dy;
        }
        tail = (tail + 1) & (MOUSE_CURSOR_QUEUE_SIZE - 1);
    }
    /* Leave the batch queued if initialization was preempted holding the graphics lock. */
    if (count && (!submit || !submit(motions, count))) {
        ++queue->retry_count;
        return 0;
    }
    if (count) ++queue->flushes;
    queue->tail = tail;
    return count;
}

/* Ring 0 produces raw packets; ring 2 alone advances tail and decodes them. */
void mouse_queue_report(struct mouse_shared *state, uint32_t controller,
                        const uint8_t *data, uint32_t length)
{
    if (controller >= MOUSE_CONTROLLER_COUNT || !data || length > MOUSE_REPORT_BYTES) return;
    uint32_t head = state->head;
    uint32_t next = (head + 1u) & (MOUSE_QUEUE_SIZE - 1u);
    if (next == state->tail) {
        ++state->dropped;
        return;
    }
    volatile struct mouse_packet *packet = &state->packets[head];
    packet->controller = controller;
    packet->length = length;
    for (uint32_t byte = 0; byte < length; ++byte) packet->data[byte] = data[byte];
    __asm__ volatile ("" ::: "memory");
    state->head = next;
}

uint32_t mouse_process_request(struct mouse_shared *state, uint32_t operation)
{
    if (operation > MOUSE_GET_DEVICES) return MOUSE_BAD_REQUEST;
    for (uint32_t id = 0; id < MOUSE_CONTROLLER_COUNT; ++id) {
        if (!(state->connected & (1u << id))) state->buttons[id] = 0;
    }
    if (state->observed_dropped != state->dropped) {
        state->observed_dropped = state->dropped;
        state->tail = state->head;
        for (uint32_t id = 0; id < MOUSE_CONTROLLER_COUNT; ++id) state->buttons[id] = 0;
        return MOUSE_EVENT_LOST;
    }
    if (operation == MOUSE_GET_DEVICES) return state->connected;
    if (operation == MOUSE_GET_BUTTONS) {
        uint32_t buttons = 0;
        for (uint32_t id = 0; id < MOUSE_CONTROLLER_COUNT; ++id) buttons |= state->buttons[id];
        return buttons;
    }
    for (uint32_t remaining = MOUSE_QUEUE_SIZE; remaining && state->tail != state->head; --remaining) {
        uint32_t tail = state->tail;
        volatile struct mouse_packet *packet = &state->packets[tail];
        uint32_t id = packet->controller;
        uint32_t length = packet->length;
        uint32_t buttons = packet->data[0] & 7u;
        uint32_t dx = packet->data[1], dy = packet->data[2];
        __asm__ volatile ("" ::: "memory");
        state->tail = (tail + 1u) & (MOUSE_QUEUE_SIZE - 1u);
        if (id >= MOUSE_CONTROLLER_COUNT || length < 3 || length > MOUSE_REPORT_BYTES) {
            ++state->malformed;
            continue;
        }
        if (!(state->connected & (1u << id))) continue;
        state->buttons[id] = (uint8_t)buttons;
        ++state->decoded;
        return MOUSE_EVENT_VALID | (id << 24) | (dy << 16) | (dx << 8) | buttons;
    }
    return 0;
}

#if defined(MOUSE_RING2)

__attribute__((section(".text.mouse_driver_service")))
uint32_t mouse_driver_service(uint32_t operation)
{
    struct mouse_shared *state = (struct mouse_shared *)MOUSE_SHARED_OFFSET;
    uint32_t busy = 1;
    __asm__ volatile ("xchgl %0, %1" : "+r"(busy), "+m"(state->lock) :: "memory");
    if (busy) return MOUSE_BUSY;
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    state->last_cpl = cs & 3u;
    ++state->calls;
    uint32_t result = state->last_cpl == 2 ? mouse_process_request(state, operation) : MOUSE_BAD_REQUEST;
    __asm__ volatile ("" ::: "memory");
    state->lock = 0;
    return result;
}

#elif !defined(MOUSE_DRIVER_TEST)

#include "Include/KernelApi.h"
#include "Include/KernelRuntime.h"
#include "Drivers/Keyboard/KeyboardShared.h"
#include "UsbHost.h"

#define MOUSE_STATE ((struct mouse_shared *)(MOUSE_DRIVER_BASE + MOUSE_SHARED_OFFSET))

static struct mouse_cursor_queue cursor_queue;

void mouse_cursor_service(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    mouse_cursor_drain(&cursor_queue, MOUSE_STATE->connected, intel915_update_cursor);
    __asm__ volatile ("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
}

static void mouse_receive(uint32_t controller, uint32_t event, const uint8_t *data, uint32_t length)
{
    if (controller >= MOUSE_CONTROLLER_COUNT) return;
    if (event == USB_REPORT_CONNECTED) MOUSE_STATE->connected |= 1u << controller;
    else if (event == USB_REPORT_DISCONNECTED) {
        MOUSE_STATE->connected &= ~(1u << controller);
        ++MOUSE_STATE->dropped; /* Invalidate queued movement across reconnect. */
        mouse_cursor_cancel(&cursor_queue, controller);
    }
    else if (event == USB_REPORT_DATA) {
        mouse_queue_report(MOUSE_STATE, controller, data, length);
        mouse_cursor_enqueue(&cursor_queue, controller, data, length);
    }
}

void mouse_driver_initialize(void)
{
    KASSERT_MSG(*(volatile uint32_t *)KEYBOARD_READY_ADDRESS == 1u,
                "Mouse driver requires ring-2 stack setup");
    KASSERT_MSG(*(volatile uint32_t *)MOUSE_READY_ADDRESS == 0u,
                "Mouse driver initialized twice");
    memcpy((void *)(MOUSE_DRIVER_BASE + MOUSE_ENTRY_OFFSET),
           (const void *)MOUSE_IMAGE_SOURCE, MOUSE_IMAGE_SIZE);
    memset(MOUSE_STATE, 0, sizeof(*MOUSE_STATE));
    memset(&cursor_queue, 0, sizeof(cursor_queue));
    uint32_t code = KERNEL_API->gdt_create_code_descriptor(MOUSE_DRIVER_BASE,
        MOUSE_ENTRY_OFFSET + MOUSE_IMAGE_SIZE - 1u, DESCRIPTOR_DPL2);
    KASSERT_MSG(code != 0, "Cannot allocate mouse code selector");
    uint32_t gate = KERNEL_API->gdt_alloc_selector();
    KASSERT_MSG(gate != 0, "Cannot allocate mouse call gate");
    KERNEL_API->gdt_write_raw_descriptor(gate, (code << 16) | MOUSE_ENTRY_OFFSET, 0xEC00u);
    KASSERT_MSG(usb_host_set_report_callback(mouse_receive) != 0, "Cannot attach mouse report queue");
    *(volatile uint32_t *)MOUSE_GATE_ADDRESS = gate | 3u;
    *(volatile uint32_t *)MOUSE_READY_ADDRESS = 1;
    *(void (**)(void))MOUSE_CURSOR_CALLBACK_ADDRESS = mouse_cursor_service;
}

#endif
