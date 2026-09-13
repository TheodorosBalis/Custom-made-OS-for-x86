#ifndef INTEL915_H
#define INTEL915_H

#include "Include/KernelTypes.h"

#define I915_BASE             0x71000000u
#define I915_RAM_SIZE         0x00100000u
#define I915_IMAGE_SOURCE     0x80010000u
#define I915_IMAGE_SIZE       0x00004000u
#define I915_SHARED_OFFSET    0x00008000u
#define I915_HWS_OFFSET       0x00009000u
#define I915_RING_OFFSET      0x0000A000u
#define I915_MMIO_OFFSET      0x00400000u
#define I915_GTT_OFFSET       0x00500000u
#define I915_FB_OFFSET        0x00800000u
#define I915_FB_MAX_SIZE      0x00800000u
#define I915_RING_DWORDS      1024u
#define I915_BACKGROUND       0x00202C38u
#define I915_RECTANGLE        0x00246C80u
#define I915_CURSOR_WHITE     0x00FFFFFFu
#define I915_CURSOR_BLACK     0x00000000u
#define I915_CURSOR_WIDTH     12u
#define I915_CURSOR_HEIGHT    16u
#define I915_CURSOR_OFFSET    0x00010000u
#define I915_CURSOR_SIDE      64u
#define I915_CURSOR_BYTES     0x4000u
#define I915_GATE_ADDRESS     0x8000233Cu
#define I915_DATA_ADDRESS     0x80002340u
#define I915_CODE_ADDRESS     0x80002344u
#define I915_READY_ADDRESS    0x80002348u
#define INTEL915_INITIALIZE_DESKTOP 0u /* DriverGateway.asm operation ABI */
#define INTEL915_GET_SCREEN_SIZE 1u
#define I915_CURSOR_BATCH 64u

struct cursor_motion {
    int32_t dx, dy;
};

enum intel915_status {
    I915_NOT_FOUND = 0,
    I915_READY = 1,
    I915_DRAWN = 2,
    I915_BAD_PCI = 0x10,
    I915_BAD_MEMORY,
    I915_NO_GTT,
    I915_NO_DISPLAY,
    I915_UNSUPPORTED_MODE,
    I915_ENGINE_BUSY,
    I915_NO_MEMORY,
    I915_NO_SELECTOR,
    I915_GTT_CONFLICT,
    I915_COMMAND_OVERFLOW,
    I915_GPU_TIMEOUT,
    I915_BAD_PIXELS,
    I915_WRONG_RING,
    I915_ALREADY_STARTED,
    I915_ACCESS_DENIED,
    I915_BUSY,
    I915_BAD_REQUEST,
    I915_CURSOR_FAILED
};

/* Keep diagnostic offsets stable; the old return-gate slots now identify the owner. */
struct intel915_shared {
    uint32_t owner_tss_selector;  /* +00: desktop task permitted to draw */
    uint32_t request_count;       /* +04 */
    uint32_t code_selector;       /* +08 */
    uint32_t data_selector;       /* +0C */
    uint32_t entry_gate;          /* +10 */
    uint32_t status;              /* +14 */
    uint32_t stage;               /* +18 */
    uint32_t observed_cs;         /* +1C */
    uint32_t pci_address;         /* +20 */
    uint32_t mmio_physical;       /* +24 */
    uint32_t gtt_physical;        /* +28 */
    uint32_t aperture_physical;   /* +2C */
    uint32_t aperture_size;       /* +30 */
    uint32_t stolen_base;         /* +34 */
    uint32_t width;               /* +38 */
    uint32_t height;              /* +3C */
    uint32_t pitch;               /* +40 */
    uint32_t framebuffer_bytes;   /* +44 */
    uint32_t framebuffer_gtt;     /* +48: GPU address, NOT CPU physical */
    uint32_t ring_gtt;            /* +4C */
    uint32_t hws_physical;        /* +50 */
    uint32_t ring_physical;       /* +54 */
    uint32_t pipe;                /* +58 */
    uint32_t command_bytes;       /* +5C */
    uint32_t ring_head;           /* +60 */
    uint32_t ring_tail;           /* +64 */
    uint32_t completion;          /* +68 */
    uint32_t pgtbl_control;       /* +6C */
    uint32_t old_vga_control;     /* +70 */
    uint32_t old_plane_a;         /* +74 */
    uint32_t old_plane_b;         /* +78 */
    uint32_t old_plane_base_a;    /* +7C */
    uint32_t old_plane_base_b;    /* +80 */
    uint32_t busy;                /* +84: non-spinning submission lock */
    uint32_t cursor_physical;
    uint32_t cursor_x, cursor_y, cursor_enabled, cursor_updates;
};

/* The separately stored physical-page array must not overlap HWS/ring pages. */
#define I915_PAGE_LIST_OFFSET 0x00014000u
#define I915_HEADER_SIZE      0x9Cu

uint32_t intel915_mode_layout(uint32_t width, uint32_t height,
                             uint32_t *pitch, uint32_t *bytes);
uint32_t intel915_build_scene(volatile uint32_t *commands, uint32_t capacity,
                             uint32_t width, uint32_t height, uint32_t pitch,
                             uint32_t framebuffer_gtt);
uint32_t intel915_initialize(void);
uint32_t intel915_build_cursor(volatile uint32_t *pixels, uint32_t capacity);
uint32_t intel915_cursor_position(uint32_t width, uint32_t height, uint32_t x, uint32_t y);
uint32_t intel915_find_cursor_pages(const uint32_t *pages, uint32_t count);
uint32_t intel915_claim_desktop(uint32_t tss_selector);
void intel915_restore_text_display(void);
uint32_t intel915_move_axis(uint32_t position, int32_t delta, uint32_t limit);
/* Kernel path: flat mappings, bounded work, no wait if the ring-1 driver is busy. */
uint32_t intel915_cursor_flush(volatile struct intel915_shared *state,
    volatile uint32_t *registers, const struct cursor_motion *motions, uint32_t count);
uint32_t intel915_update_cursor(const struct cursor_motion *motions, uint32_t count);

#endif
