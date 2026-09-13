#include "Intel915.h"

#define PGTBL_CTL       0x02020u
#define RING_TAIL       0x02030u
#define RING_HEAD       0x02034u
#define RING_START      0x02038u
#define RING_CTL        0x0203Cu
#define HWS_PGA         0x02080u
#define MI_MODE         0x0209Cu
#define PIPE_SOURCE     0x6001Cu
#define ADPA            0x61100u
#define LVDS            0x61180u
#define PIPE_CONF       0x70008u
#define PLANE_CONTROL   0x70180u
#define PLANE_BASE      0x70184u
#define PLANE_STRIDE    0x70188u
#define PLANE_POSITION  0x7018Cu
#define PLANE_SIZE      0x70190u
#define VGA_CONTROL     0x71400u
#define ENABLE          0x80000000u
#define XY_COLOR_BLT    0x54300004u
#define MI_FLUSH        0x02000000u
#define MI_STORE_INDEX  0x10800001u
#define COMPLETION_WORD 0x20u
#define COMPLETION_TAG  0x915D0001u
#define POLL_LIMIT      10000000u
#define RING_MASK       0x001FFFFCu
#define STOP_RING       (1u << 8)
#define MODE_IDLE       (1u << 9)
#define CURSOR_CONTROL  0x70080u
#define CURSOR_BASE     0x70084u
#define CURSOR_POSITION 0x70088u
#define CURSOR_STRIDE   0x40u
#define CURSOR_ARGB64   0x27u

_Static_assert(sizeof(struct intel915_shared) == I915_HEADER_SIZE,
               "Intel915 assembly/header ABI changed");
_Static_assert(I915_CURSOR_OFFSET + I915_CURSOR_BYTES <= I915_PAGE_LIST_OFFSET,
               "Cursor overlaps framebuffer page list");

uint32_t intel915_cursor_position(uint32_t width, uint32_t height, uint32_t x, uint32_t y)
{
    if (!width || !height || width > 2048 || height > 2048 || x >= width || y >= height)
        return 0xFFFFFFFFu;
    return (y << 16) | x;
}

uint32_t intel915_move_axis(uint32_t position, int32_t delta, uint32_t limit)
{
    if (!limit || limit > 2048u) return 0;
    if (position >= limit) position = limit - 1;
    if (delta < 0) {
        uint32_t distance = 0u - (uint32_t)delta;
        return distance > position ? 0 : position - distance;
    }
    return (uint32_t)delta >= limit - position ? limit - 1 : position + (uint32_t)delta;
}

uint32_t intel915_cursor_flush(volatile struct intel915_shared *cursor,
    volatile uint32_t *registers, const struct cursor_motion *motions, uint32_t count)
{
    if (!cursor || !registers || !motions || !count || count > I915_CURSOR_BATCH)
        return 0;
    if (__sync_lock_test_and_set(&cursor->busy, 1u)) return 0;
    uint32_t result = 0;
    if (cursor->status == I915_DRAWN && cursor->cursor_enabled && cursor->pipe <= 1 &&
        cursor->cursor_physical && !(cursor->cursor_physical & 4095u) &&
        cursor->width && cursor->width <= 2048 && cursor->height && cursor->height <= 2048) {
        uint32_t x = cursor->cursor_x, y = cursor->cursor_y;
        for (uint32_t i = 0; i < count; ++i) {
            x = intel915_move_axis(x, motions[i].dx, cursor->width);
            y = intel915_move_axis(y, motions[i].dy, cursor->height);
        }
        if (x != cursor->cursor_x || y != cursor->cursor_y) {
            uint32_t offset = cursor->pipe * CURSOR_STRIDE;
            registers[(CURSOR_POSITION + offset) / 4] = (y << 16) | x;
            (void)registers[(CURSOR_POSITION + offset) / 4];
            registers[(CURSOR_BASE + offset) / 4] = cursor->cursor_physical;
            (void)registers[(CURSOR_BASE + offset) / 4];
            cursor->cursor_x = x;
            cursor->cursor_y = y;
            ++cursor->cursor_updates;
        }
        result = 1;
    }
    __sync_lock_release(&cursor->busy);
    return result;
}

uint32_t intel915_find_cursor_pages(const uint32_t *pages, uint32_t count)
{
    if (!pages || count < 4) return 0xFFFFFFFFu;
    for (uint32_t i = 0; i <= count - 4; ++i) {
        uint32_t base = pages[i];
        if (base && !(base & 4095u) && base <= 0xFFFFC000u &&
            pages[i + 1] == base + 4096 && pages[i + 2] == base + 8192 &&
            pages[i + 3] == base + 12288) return i;
    }
    return 0xFFFFFFFFu;
}

uint32_t intel915_build_cursor(volatile uint32_t *pixels, uint32_t capacity)
{
    if (!pixels || capacity < I915_CURSOR_BYTES / 4) return 0;
    for (uint32_t y = 0; y < I915_CURSOR_SIDE; ++y) {
        for (uint32_t x = 0; x < I915_CURSOR_SIDE; ++x) {
            uint32_t color = 0;
            if (y < 12 && x <= y) {
                color = 0xFF000000u;
                if (y > 1 && y < 11 && x > 0 && x < y) color = 0xFFFFFFFFu;
            } else if (y >= 12 && y < 16) {
                uint32_t left = 5 + (y - 12) / 2;
                if (x >= left && x < left + 4) {
                    color = 0xFF000000u;
                    if (y < 15 && x > left && x < left + 3) color = 0xFFFFFFFFu;
                }
            }
            pixels[y * I915_CURSOR_SIDE + x] = color;
        }
    }
    return 1;
}

uint32_t intel915_mode_layout(uint32_t width, uint32_t height,
                             uint32_t *pitch, uint32_t *bytes)
{
    uint32_t row;
    uint32_t size;

    if (width < 320u || width > 2048u || height < 200u || height > 2048u) {
        return 0u;
    }
    row = (width * 4u + 63u) & ~63u;
    size = (row * height + 4095u) & ~4095u;
    if (size > I915_FB_MAX_SIZE) {
        return 0u;
    }
    *pitch = row;
    *bytes = size;
    return 1u;
}

struct command_buffer {
    volatile uint32_t *words;
    uint32_t count;
    uint32_t capacity;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t destination;
    uint32_t failed;
};

static void fill_rectangle(struct command_buffer *buffer, uint32_t x,
                           uint32_t y, uint32_t width, uint32_t height,
                           uint32_t color)
{
    volatile uint32_t *command;

    if (x >= buffer->width || y >= buffer->height || width == 0u || height == 0u) {
        return;
    }
    if (width > buffer->width - x) {
        width = buffer->width - x;
    }
    if (height > buffer->height - y) {
        height = buffer->height - y;
    }
    if (buffer->capacity - buffer->count < 6u) {
        buffer->failed = 1u;
        return;
    }
    command = buffer->words + buffer->count;
    command[0] = XY_COLOR_BLT;
    command[1] = (3u << 24) | (0xF0u << 16) | buffer->pitch;
    command[2] = (y << 16) | x;
    command[3] = ((y + height) << 16) | (x + width);
    command[4] = buffer->destination;
    command[5] = color;
    buffer->count += 6u;
}

/* This builds GPU commands, not framebuffer pixels. Also used by native tests. */
uint32_t intel915_build_scene(volatile uint32_t *commands, uint32_t capacity,
                             uint32_t width, uint32_t height, uint32_t pitch,
                             uint32_t framebuffer_gtt)
{
    struct command_buffer buffer;
    uint32_t expected_pitch;
    uint32_t bytes;

    if (intel915_mode_layout(width, height, &expected_pitch, &bytes) == 0u ||
        pitch != expected_pitch || (framebuffer_gtt & 4095u) != 0u ||
        framebuffer_gtt > 0x10000000u - bytes || capacity < 8u ||
        capacity > I915_RING_DWORDS) {
        return 0u;
    }
    buffer.words = commands;
    buffer.count = 0u;
    buffer.capacity = capacity - 8u; /* Completion packet plus ring's empty gap. */
    buffer.width = width;
    buffer.height = height;
    buffer.pitch = pitch;
    buffer.destination = framebuffer_gtt;
    buffer.failed = 0u;

    fill_rectangle(&buffer, 0u, 0u, width, height, I915_RECTANGLE);

    if (buffer.failed != 0u) {
        return 0u;
    }
    commands[buffer.count++] = MI_FLUSH;
    commands[buffer.count++] = MI_STORE_INDEX;
    commands[buffer.count++] = COMPLETION_WORD << 2;
    commands[buffer.count++] = COMPLETION_TAG;
    if ((buffer.count & 1u) != 0u) {
        commands[buffer.count++] = 0u;
    }
    return buffer.count * 4u;
}

#if defined(INTEL915_RING1)

static volatile struct intel915_shared *const state =
    (volatile struct intel915_shared *)I915_SHARED_OFFSET;

static uint32_t read_register(uint32_t offset)
{
    return *(volatile uint32_t *)(I915_MMIO_OFFSET + offset);
}

static void write_register(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(I915_MMIO_OFFSET + offset) = value;
    (void)read_register(offset);
}

static uint32_t enable_cursor(void)
{
    if (!state->cursor_physical || (state->cursor_physical & 4095u) || state->pipe > 1 ||
        !intel915_build_cursor((volatile uint32_t *)I915_CURSOR_OFFSET, I915_CURSOR_BYTES / 4))
        return I915_CURSOR_FAILED;
    for (uint32_t pipe = 0; pipe < 2; ++pipe) {
        write_register(CURSOR_CONTROL + pipe * CURSOR_STRIDE, 0);
        write_register(CURSOR_BASE + pipe * CURSOR_STRIDE, 0);
    }
    uint32_t offset = state->pipe * CURSOR_STRIDE;
    state->cursor_x = (state->width - I915_CURSOR_WIDTH) / 2;
    state->cursor_y = (state->height - I915_CURSOR_HEIGHT) / 2;
    __asm__ volatile ("lock; addl $0, (%%esp)" ::: "memory", "cc");
    /* i915GM uses a physical cursor base, not a GTT address. */
    write_register(CURSOR_BASE + offset, state->cursor_physical);
    write_register(CURSOR_CONTROL + offset, CURSOR_ARGB64 | (state->pipe << 28));
    write_register(CURSOR_POSITION + offset, (state->cursor_y << 16) | state->cursor_x);
    write_register(CURSOR_BASE + offset, state->cursor_physical);
    if ((read_register(CURSOR_CONTROL + offset) & 0x27u) != CURSOR_ARGB64 ||
        read_register(CURSOR_BASE + offset) != state->cursor_physical) return I915_CURSOR_FAILED;
    state->cursor_enabled = 1;
    return I915_DRAWN;
}

static uint32_t driver_error(uint32_t error)
{
    state->status = error;
    return error;
}

static uint32_t wait_for_ring(void)
{
    uint32_t tries;
    for (tries = 0u; tries < POLL_LIMIT; ++tries) {
        state->ring_head = read_register(RING_HEAD) & RING_MASK;
        state->ring_tail = read_register(RING_TAIL) & RING_MASK;
        if (state->ring_head == state->ring_tail) {
            return 1u;
        }
        __asm__ volatile ("pause");
    }
    return 0u;
}

static uint32_t draw_desktop(void)
{
    volatile uint32_t *gtt = (volatile uint32_t *)I915_GTT_OFFSET;
    volatile uint32_t *pages = (volatile uint32_t *)I915_PAGE_LIST_OFFSET;
    volatile uint32_t *hws = (volatile uint32_t *)I915_HWS_OFFSET;
    volatile uint32_t *pixels = (volatile uint32_t *)I915_FB_OFFSET;
    uint32_t index;
    uint32_t tries;
    uint32_t plane;
    uint32_t bytes;
    if (state->status != I915_READY || state->stage != 1u) {
        return driver_error(I915_ALREADY_STARTED);
    }
    state->stage = 2u;
    if ((read_register(RING_CTL) & 1u) != 0u && wait_for_ring() == 0u) {
        return driver_error(I915_ENGINE_BUSY);
    }

    bytes = intel915_build_scene((volatile uint32_t *)I915_RING_OFFSET,
                                 I915_RING_DWORDS, state->width, state->height,
                                 state->pitch, state->framebuffer_gtt);
    if (bytes == 0u) {
        return driver_error(I915_COMMAND_OVERFLOW);
    }
    state->command_bytes = bytes;

    /* MI_MODE uses upper-half write masks. Stop the parser before replacing RAM. */
    write_register(MI_MODE, (STOP_RING << 16) | STOP_RING);
    for (tries = 0u; tries < POLL_LIMIT; ++tries) {
        if ((read_register(MI_MODE) & MODE_IDLE) != 0u) {
            break;
        }
        __asm__ volatile ("pause");
    }
    if (tries == POLL_LIMIT && wait_for_ring() == 0u) {
        return driver_error(I915_ENGINE_BUSY);
    }
    /* Boot-time ownership: all resources remain pinned even on a GPU timeout. */
    write_register(RING_CTL, 0u);
    for (index = 0u; index < state->framebuffer_bytes / 4096u; ++index) {
        gtt[state->framebuffer_gtt / 4096u + index] = pages[index] | 1u;
    }
    gtt[state->ring_gtt / 4096u] = state->ring_physical | 1u;
    (void)gtt[state->ring_gtt / 4096u];
    write_register(PGTBL_CTL, state->pgtbl_control);
    hws[COMPLETION_WORD] = 0u;
    write_register(HWS_PGA, state->hws_physical);
    write_register(RING_TAIL, 0u);
    write_register(RING_HEAD, 0u);
    write_register(RING_START, state->ring_gtt);
    write_register(MI_MODE, STOP_RING << 16);
    write_register(RING_CTL, 1u); /* One 4 KiB ring. */
    state->stage = 3u;
    __asm__ volatile ("lock; addl $0, (%%esp)" ::: "memory", "cc");
    write_register(RING_TAIL, bytes);

    for (tries = 0u; tries < POLL_LIMIT; ++tries) {
        state->completion = hws[COMPLETION_WORD];
        if (state->completion == COMPLETION_TAG) {
            break;
        }
        __asm__ volatile ("pause");
    }
    state->ring_head = read_register(RING_HEAD);
    state->ring_tail = read_register(RING_TAIL);
    if (tries == POLL_LIMIT || wait_for_ring() == 0u) {
        return driver_error(I915_GPU_TIMEOUT);
    }
    state->stage = 4u;
    /* Do not replace VGA until an actual GPU write has been observed. */
    index = ((state->height - I915_CURSOR_HEIGHT) / 2u + 4u) * (state->pitch / 4u)
            + (state->width - I915_CURSOR_WIDTH) / 2u + 2u;
    if ((pixels[0] & 0xFFFFFFu) != I915_RECTANGLE ||
        (pixels[index] & 0xFFFFFFu) != I915_RECTANGLE) {
        return driver_error(I915_BAD_PIXELS);
    }

    /* Preserve the running pipe, PLL, LVDS power sequence and panel fitter. */
    plane = state->pipe * 0x1000u;
    state->stage = 5u;
    write_register(PLANE_CONTROL, state->old_plane_a & ~ENABLE);
    write_register(PLANE_BASE, state->old_plane_base_a);
    write_register(PLANE_CONTROL + 0x1000u, state->old_plane_b & ~ENABLE);
    write_register(PLANE_BASE + 0x1000u, state->old_plane_base_b);
    write_register(PLANE_STRIDE + plane, state->pitch);
    write_register(PLANE_POSITION + plane, 0u);
    write_register(PLANE_SIZE + plane,
                   ((state->height - 1u) << 16) | (state->width - 1u));
    write_register(PLANE_CONTROL + plane,
                   ENABLE | (6u << 26) | (state->pipe << 24));
    write_register(PLANE_BASE + plane, state->framebuffer_gtt);
    write_register(VGA_CONTROL, state->old_vga_control | ENABLE);
    if (enable_cursor() != I915_DRAWN) return driver_error(I915_CURSOR_FAILED);
    state->stage = 6u;
    state->status = I915_DRAWN;
    return I915_DRAWN;
}

__attribute__((section(".text.intel915_driver_service"), used))
uint32_t intel915_driver_service(uint32_t operation, uint32_t x, uint32_t y)
{
    (void)x;
    (void)y;
    uint16_t cs;
    uint16_t task;
    uint32_t result;

    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile ("str %0" : "=r"(task));
    if ((cs & 3u) != 1u) {
        return I915_WRONG_RING;
    }
    if (state->owner_tss_selector == 0u || task != state->owner_tss_selector) {
        return I915_ACCESS_DENIED;
    }
    if (operation > INTEL915_GET_SCREEN_SIZE) {
        return I915_BAD_REQUEST;
    }
    /* Timer preemption stays enabled. Never spin behind a preempted owner. */
    if (__sync_lock_test_and_set(&state->busy, 1u) != 0u) {
        return I915_BUSY;
    }
    state->observed_cs = cs;
    ++state->request_count;
    if (operation == INTEL915_INITIALIZE_DESKTOP) {
        result = state->status == I915_DRAWN ? I915_DRAWN : draw_desktop();
    } else if (state->status != I915_DRAWN) {
        result = state->status;
    } else {
        result = (state->height << 16) | state->width;
    }
    __sync_lock_release(&state->busy);
    return result;
}

#elif !defined(INTEL915_TEST)

#include "Include/KernelApi.h"
#include "Include/KernelRuntime.h"

#define KERNEL_PD       0x30030000u
#define KNOWN_RAM_END   0x3F780000u
#define UNCACHED_RW     0x1Bu

static volatile struct intel915_shared *panic_state;

uint32_t intel915_update_cursor(const struct cursor_motion *motions, uint32_t count)
{
    return intel915_cursor_flush(panic_state,
        (volatile uint32_t *)(I915_BASE + I915_MMIO_OFFSET), motions, count);
}

uint32_t intel915_claim_desktop(uint32_t tss_selector)
{
    if (panic_state == NULL || panic_state->status != I915_READY ||
        panic_state->owner_tss_selector != 0u || tss_selector == 0u ||
        tss_selector > 0xFFF8u || (tss_selector & 7u) != 0u) {
        return 0u;
    }
    panic_state->owner_tss_selector = tss_selector;
    return 1u;
}

void intel915_restore_text_display(void)
{
    volatile uint32_t *registers;
    if (panic_state == NULL || panic_state->stage < 5u) {
        return;
    }
    /* Emergency ring-0 path: no gate, allocation, GPU command or BIOS call. */
    registers = (volatile uint32_t *)(I915_BASE + I915_MMIO_OFFSET);
    for (uint32_t pipe = 0; pipe < 2; ++pipe) {
        registers[(CURSOR_CONTROL + pipe * CURSOR_STRIDE) / 4u] = 0;
        registers[(CURSOR_BASE + pipe * CURSOR_STRIDE) / 4u] = 0;
    }
    panic_state->cursor_enabled = 0;
    registers[PLANE_CONTROL / 4u] &= ~ENABLE;
    registers[PLANE_BASE / 4u] = panic_state->old_plane_base_a;
    registers[(PLANE_CONTROL + 0x1000u) / 4u] &= ~ENABLE;
    registers[(PLANE_BASE + 0x1000u) / 4u] = panic_state->old_plane_base_b;
    registers[VGA_CONTROL / 4u] = panic_state->old_vga_control & ~ENABLE;
    (void)registers[VGA_CONTROL / 4u];
}

static uint32_t pci_read(uint32_t device, uint32_t offset)
{
    uint32_t value;
    uint32_t address = 0x80000000u | device | (offset & 0xFCu);
    __asm__ volatile ("outl %0, %w1" :: "a"(address), "Nd"((uint16_t)0xCF8));
    __asm__ volatile ("inl %w1, %0" : "=a"(value) : "Nd"((uint16_t)0xCFC));
    return value;
}

static void enable_bus_master(uint32_t device)
{
    uint32_t address = 0x80000000u | device | 4u;
    uint16_t command = (uint16_t)pci_read(device, 4u) | 4u;
    __asm__ volatile ("outl %0, %w1" :: "a"(address), "Nd"((uint16_t)0xCF8));
    /* Word access avoids clearing the adjacent PCI status write-one-to-clear bits. */
    __asm__ volatile ("outw %0, %w1" :: "a"(command), "Nd"((uint16_t)0xCFC));
    (void)pci_read(device, 4u);
}

static uint32_t find_gpu(void)
{
    uint32_t bus;
    uint32_t device;
    /* The primary VGA function is function zero, not the 2792 secondary head. */
    for (bus = 0u; bus < 256u; ++bus) {
        for (device = 0u; device < 32u; ++device) {
            uint32_t address = (bus << 16) | (device << 11);
            if (pci_read(address, 0u) == 0x25928086u &&
                (pci_read(address, 8u) >> 16) == 0x0300u) {
                return address;
            }
        }
    }
    return 0xFFFFFFFFu;
}

static void map_checked(uint32_t address, uint32_t physical, uint32_t flags)
{
    if (KERNEL_API->get_physical_address(KERNEL_PD, address) != 0u) {
        KPANIC("Intel915 virtual address is already mapped");
    }
    if (KERNEL_API->map_page(address, physical, flags) == 0u) {
        KPANIC("Intel915 page mapping failed");
    }
}

static uint32_t allocate_page(uint32_t address, uint32_t flags)
{
    uint32_t physical = KERNEL_API->pmm_alloc_page();
    if (physical == 0u) {
        KPANIC("No physical memory for Intel915");
    }
    map_checked(address, physical, flags);
    return physical;
}

static uint32_t allocate_cursor_pages(void)
{
    uint32_t pages[64], count = 0, found = 0xFFFFFFFFu, base = 0;
    while (count < 64) {
        uint32_t physical = KERNEL_API->pmm_alloc_page();
        if (!physical) break;
        pages[count++] = physical;
        found = intel915_find_cursor_pages(pages, count);
        if (found != 0xFFFFFFFFu) { base = pages[found]; break; }
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!base || i < found || i >= found + 4) KERNEL_API->pmm_free_page(pages[i]);
    }
    return base;
}

static uint32_t host_register(uint32_t offset)
{
    return *(volatile uint32_t *)(I915_BASE + I915_MMIO_OFFSET + offset);
}

static uint32_t create_gate(uint32_t code, uint32_t offset)
{
    uint32_t selector = KERNEL_API->gdt_alloc_selector();
    if (selector != 0u) {
        KERNEL_API->gdt_write_raw_descriptor(selector,
            (code << 16) | (offset & 0xFFFFu), (offset & 0xFFFF0000u) | 0xEC00u);
    }
    return selector;
}

static void write_hex(char *destination, uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    uint32_t index;
    for (index = 0u; index < 8u; ++index) {
        destination[index] = digits[(value >> (28u - index * 4u)) & 15u];
    }
}

__attribute__((noreturn))
static void graphics_failure(volatile struct intel915_shared *state, uint32_t error)
{
    char message[] = "Intel915 status=00000000 stage=00000000 head=00000000 tail=00000000 done=00000000";
    state->status = error;
    write_hex(message + 16u, error);
    write_hex(message + 31u, state->stage);
    write_hex(message + 45u, state->ring_head);
    write_hex(message + 59u, state->ring_tail);
    write_hex(message + 73u, state->completion);
    KPANIC(message);
}

uint32_t intel915_initialize(void)
{
    volatile struct intel915_shared *state;
    volatile uint32_t *pages;
    uint32_t pci;
    uint32_t mmio;
    uint32_t gtt;
    uint32_t aperture;
    uint32_t aperture_size;
    uint32_t stolen;
    uint32_t offset;
    uint32_t physical;
    uint32_t source;
    uint32_t output;
    uint32_t pitch;
    uint32_t bytes;
    uint32_t code;
    uint32_t data;
    uint32_t cursor_physical;

    pci = find_gpu();
    if (pci == 0xFFFFFFFFu) {
        return 0u; /* VirtualBox and other GPUs retain the existing text display. */
    }
    mmio = pci_read(pci, 0x10u);
    gtt = pci_read(pci, 0x1Cu);
    aperture = pci_read(pci, 0x18u);
    aperture_size = (pci_read(pci, 0x60u) & (2u << 16)) != 0u
                    ? 0x08000000u : 0x10000000u;
    stolen = pci_read(pci, 0x5Cu) & 0xFFF00000u;
    if ((pci_read(pci, 4u) & 2u) == 0u || (mmio & 7u) != 0u ||
        (gtt & 7u) != 0u || (aperture & 7u) != 0u) {
        KPANIC("Intel915 PCI memory decoding is disabled or BAR type is invalid");
    }
    mmio &= 0xFFF80000u;
    gtt &= 0xFFFC0000u;
    aperture &= ~(aperture_size - 1u);
    if (mmio < 0x40000000u || gtt < 0x40000000u ||
        aperture < 0x40000000u || stolen < KNOWN_RAM_END || stolen >= 0x40000000u ||
        (gtt & 0xFFF80000u) == mmio ||
        (mmio & ~(aperture_size - 1u)) == aperture ||
        (gtt & ~(aperture_size - 1u)) == aperture) {
        KPANIC("Intel915 BARs/stolen RAM do not match this laptop memory map");
    }

    /* These ranges are outside the PMM's usable RAM; never allocate them as RAM. */
    KERNEL_API->pmm_mark_range_used(stolen, 0x40000000u);
    cursor_physical = allocate_cursor_pages();
    if (!cursor_physical) KPANIC("No contiguous memory for Intel915 cursor");
    for (offset = 0u; offset < I915_RAM_SIZE; offset += PAGE_SIZE) {
        if (offset >= I915_CURSOR_OFFSET && offset < I915_CURSOR_OFFSET + I915_CURSOR_BYTES) {
            map_checked(I915_BASE + offset, cursor_physical + offset - I915_CURSOR_OFFSET, UNCACHED_RW);
            continue;
        }
        uint32_t flags = offset == I915_HWS_OFFSET || offset == I915_RING_OFFSET
                         ? UNCACHED_RW : PAGE_PRESENT | PAGE_RW;
        allocate_page(I915_BASE + offset, flags);
    }
    memset((void *)I915_BASE, 0, I915_RAM_SIZE);
    memcpy((void *)I915_BASE, (const void *)I915_IMAGE_SOURCE, I915_IMAGE_SIZE);
    state = (volatile struct intel915_shared *)(I915_BASE + I915_SHARED_OFFSET);
    pages = (volatile uint32_t *)(I915_BASE + I915_PAGE_LIST_OFFSET);
    state->pci_address = pci;
    state->cursor_physical = cursor_physical;
    state->mmio_physical = mmio;
    state->gtt_physical = gtt;
    state->aperture_physical = aperture;
    state->aperture_size = aperture_size;
    state->stolen_base = stolen;
    for (offset = 0u; offset < 0x80000u; offset += PAGE_SIZE) {
        map_checked(I915_BASE + I915_MMIO_OFFSET + offset, mmio + offset, UNCACHED_RW);
    }
    for (offset = 0u; offset < 0x40000u; offset += PAGE_SIZE) {
        map_checked(I915_BASE + I915_GTT_OFFSET + offset, gtt + offset, UNCACHED_RW);
    }
    state->pgtbl_control = host_register(PGTBL_CTL);
    if ((state->pgtbl_control & 1u) == 0u) {
        graphics_failure(state, I915_NO_GTT);
    }
    if ((state->pgtbl_control & 0xFFFFF000u) < KNOWN_RAM_END ||
        (state->pgtbl_control & 0xFFFFF000u) > 0x40000000u - aperture_size / 1024u) {
        graphics_failure(state, I915_BAD_MEMORY);
    }

    output = host_register(LVDS);
    if ((output & ENABLE) == 0u) {
        output = host_register(ADPA);
    } else if ((host_register(ADPA) & ENABLE) != 0u &&
               ((host_register(ADPA) ^ output) & (1u << 30)) != 0u) {
        graphics_failure(state, I915_NO_DISPLAY); /* No multi-pipe takeover yet. */
    }
    if ((output & ENABLE) == 0u) {
        graphics_failure(state, I915_NO_DISPLAY);
    }
    state->pipe = (output >> 30) & 1u;
    if ((host_register(PIPE_CONF + state->pipe * 0x1000u) & ENABLE) == 0u) {
        graphics_failure(state, I915_NO_DISPLAY);
    }
    source = host_register(PIPE_SOURCE + state->pipe * 0x1000u);
    state->width = ((source >> 16) & 0xFFFu) + 1u;
    state->height = (source & 0xFFFu) + 1u;
    if (intel915_mode_layout(state->width, state->height, &pitch, &bytes) == 0u) {
        graphics_failure(state, I915_UNSUPPORTED_MODE);
    }
    state->pitch = pitch;
    state->framebuffer_bytes = bytes;
    state->framebuffer_gtt = aperture_size - 0x01000000u;
    state->ring_gtt = state->framebuffer_gtt + I915_FB_MAX_SIZE;
    state->old_vga_control = host_register(VGA_CONTROL);
    state->old_plane_a = host_register(PLANE_CONTROL);
    state->old_plane_b = host_register(PLANE_CONTROL + 0x1000u);
    state->old_plane_base_a = host_register(PLANE_BASE);
    state->old_plane_base_b = host_register(PLANE_BASE + 0x1000u);
    panic_state = state;
    if (((state->old_plane_a & ENABLE) != 0u &&
          state->old_plane_base_a >= state->framebuffer_gtt - 0x01000000u) ||
        ((state->old_plane_b & ENABLE) != 0u &&
          state->old_plane_base_b >= state->framebuffer_gtt - 0x01000000u)) {
        graphics_failure(state, I915_GTT_CONFLICT);
    }
    for (offset = 0u; offset < bytes; offset += PAGE_SIZE) {
        physical = allocate_page(I915_BASE + I915_FB_OFFSET + offset, UNCACHED_RW);
        pages[offset / PAGE_SIZE] = physical;
    }
    state->hws_physical = KERNEL_API->get_physical_address(KERNEL_PD,
                                                          I915_BASE + I915_HWS_OFFSET);
    state->ring_physical = KERNEL_API->get_physical_address(KERNEL_PD,
                                                           I915_BASE + I915_RING_OFFSET);
    code = KERNEL_API->gdt_create_code_descriptor(I915_BASE, 0xFFFFu, DESCRIPTOR_DPL1);
    data = KERNEL_API->gdt_create_data_descriptor(I915_BASE, 0xFFFFFFu, DESCRIPTOR_DPL1);
    if (code == 0u || data == 0u) {
        graphics_failure(state, I915_NO_SELECTOR);
    }
    state->code_selector = code | 1u;
    state->data_selector = data | 1u;
    state->entry_gate = create_gate(code, 0u);
    if (state->entry_gate == 0u) {
        graphics_failure(state, I915_NO_SELECTOR);
    }
    state->status = I915_READY;
    state->stage = 1u;
    enable_bus_master(pci);
    *(volatile uint32_t *)I915_GATE_ADDRESS = state->entry_gate | 3u;
    *(volatile uint32_t *)I915_DATA_ADDRESS = state->data_selector;
    *(volatile uint32_t *)I915_CODE_ADDRESS = state->code_selector;
    *(volatile uint32_t *)I915_READY_ADDRESS = 1u;
    return 1u;
}

#endif
