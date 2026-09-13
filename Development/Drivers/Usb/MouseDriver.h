#ifndef MOUSE_DRIVER_H
#define MOUSE_DRIVER_H

#include "Include/KernelTypes.h"
#include "../Intel915/Intel915.h"

#define MOUSE_DRIVER_BASE 0x70000000u
#define MOUSE_ENTRY_OFFSET 0x4000u
#define MOUSE_IMAGE_SOURCE 0x8001D000u
#define MOUSE_IMAGE_SIZE 0x3000u
#define MOUSE_SHARED_OFFSET 0xA000u
#define MOUSE_GATE_ADDRESS 0x80002358u
#define MOUSE_READY_ADDRESS 0x8000235Cu
#define MOUSE_CURSOR_CALLBACK_ADDRESS 0x80002364u
#define MOUSE_CURSOR_QUEUE_SIZE 64u
#define MOUSE_QUEUE_SIZE 32u
#define MOUSE_REPORT_BYTES 64u
#define MOUSE_CONTROLLER_COUNT 4u

#define MOUSE_READ_EVENT 0u
#define MOUSE_GET_BUTTONS 1u
#define MOUSE_GET_DEVICES 2u
#define MOUSE_EVENT_VALID 0x80000000u
#define MOUSE_EVENT_LOST 0x40000000u
#define MOUSE_BUSY 0x20000000u
#define MOUSE_BAD_REQUEST 0x10000000u

struct mouse_packet {
    uint32_t controller, length;
    uint8_t data[MOUSE_REPORT_BYTES];
};

struct mouse_shared {
    volatile uint32_t lock, head, tail, dropped, connected;
    volatile uint32_t last_cpl, calls, decoded, malformed;
    uint32_t observed_dropped;
    uint8_t buttons[MOUSE_CONTROLLER_COUNT];
    volatile struct mouse_packet packets[MOUSE_QUEUE_SIZE];
};

struct mouse_cursor_packet {
    int8_t dx, dy;
    uint8_t controller;
};

/* Kernel-only queue, independent of the userspace event consumer. IF must be zero. */
struct mouse_cursor_queue {
    uint32_t head, tail, dropped;
    uint32_t peak_pending, saturated_reports, flushes, retry_count;
    struct mouse_cursor_packet packets[MOUSE_CURSOR_QUEUE_SIZE];
};
_Static_assert(I915_CURSOR_BATCH >= MOUSE_CURSOR_QUEUE_SIZE - 1u,
               "A cursor flush must consume the entire pending queue");

void mouse_cursor_enqueue(struct mouse_cursor_queue *queue, uint32_t controller,
                          const uint8_t *data, uint32_t length);
void mouse_cursor_cancel(struct mouse_cursor_queue *queue, uint32_t controller);
uint32_t mouse_cursor_drain(struct mouse_cursor_queue *queue, uint32_t connected,
    uint32_t (*submit)(const struct cursor_motion *, uint32_t));
void mouse_cursor_service(void);

void mouse_driver_initialize(void);
void mouse_queue_report(struct mouse_shared *state, uint32_t controller,
                        const uint8_t *data, uint32_t length);
uint32_t mouse_process_request(struct mouse_shared *state, uint32_t operation);

_Static_assert(sizeof(struct mouse_shared) <= 4096u, "Mouse shared state exceeds one page");
_Static_assert(MOUSE_ENTRY_OFFSET + MOUSE_IMAGE_SIZE <= 0x8000u, "Mouse image overlaps keyboard state");
_Static_assert(MOUSE_SHARED_OFFSET + 4096u <= 0x10000u, "Mouse state overlaps ring-2 stacks");

#endif
