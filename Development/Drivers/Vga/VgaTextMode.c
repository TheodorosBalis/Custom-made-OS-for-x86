#include "Drivers/Vga/VgaTextMode.h"
#include "Include/KernelTypes.h"
#include "Drivers/Intel915/Intel915.h"

#define VGA_FONT_MEMORY       ((volatile uint8_t *)0x80A00000u)
#define VGA_GLYPH_COUNT       256u
#define VGA_GLYPH_HEIGHT      16u
#define VGA_GLYPH_SLOT_SIZE   32u

#define VGA_MISC_WRITE        0x03C2u
#define VGA_SEQUENCER_INDEX   0x03C4u
#define VGA_SEQUENCER_DATA    0x03C5u
#define VGA_DAC_MASK          0x03C6u
#define VGA_DAC_WRITE_INDEX   0x03C8u
#define VGA_DAC_DATA          0x03C9u
#define VGA_GRAPHICS_INDEX    0x03CEu
#define VGA_GRAPHICS_DATA     0x03CFu
#define VGA_ATTRIBUTE_INDEX   0x03C0u
#define VGA_CRTC_INDEX        0x03D4u
#define VGA_CRTC_DATA         0x03D5u
#define VGA_INPUT_STATUS_1    0x03DAu

#define VGA_FONT_NOT_CAPTURED 0x56474100u
#define VGA_FONT_CAPTURED     0x56474101u

static uint8_t startup_font[VGA_GLYPH_COUNT * VGA_GLYPH_HEIGHT];
static uint32_t startup_font_state = VGA_FONT_NOT_CAPTURED;

static const uint8_t mode3_sequencer[4] = {
    0x00u, 0x03u, 0x00u, 0x02u
};

static const uint8_t mode3_crtc[25] = {
    0x5Fu, 0x4Fu, 0x50u, 0x82u, 0x55u, 0x81u, 0xBFu, 0x1Fu,
    0x00u, 0x4Fu, 0x0Du, 0x0Eu, 0x00u, 0x00u, 0x00u, 0x00u,
    0x9Cu, 0x8Eu, 0x8Fu, 0x28u, 0x1Fu, 0x96u, 0xB9u, 0xA3u,
    0xFFu
};

static const uint8_t mode3_graphics[9] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x10u, 0x0Eu, 0x0Fu, 0xFFu
};

static const uint8_t mode3_attribute[20] = {
    0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x14u, 0x07u,
    0x38u, 0x39u, 0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu,
    0x0Cu, 0x00u, 0x0Fu, 0x08u
};

static const uint8_t mode3_palette[16][3] = {
    {0x00u, 0x00u, 0x00u}, {0x00u, 0x00u, 0x2Au},
    {0x00u, 0x2Au, 0x00u}, {0x00u, 0x2Au, 0x2Au},
    {0x2Au, 0x00u, 0x00u}, {0x2Au, 0x00u, 0x2Au},
    {0x2Au, 0x15u, 0x00u}, {0x2Au, 0x2Au, 0x2Au},
    {0x15u, 0x15u, 0x15u}, {0x15u, 0x15u, 0x3Fu},
    {0x15u, 0x3Fu, 0x15u}, {0x15u, 0x3Fu, 0x3Fu},
    {0x3Fu, 0x15u, 0x15u}, {0x3Fu, 0x15u, 0x3Fu},
    {0x3Fu, 0x3Fu, 0x15u}, {0x3Fu, 0x3Fu, 0x3Fu}
};

static inline void vga_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t vga_in8(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void vga_write_indexed(uint16_t index_port,
                              uint16_t data_port,
                              uint8_t index,
                              uint8_t value)
{
    vga_out8(index_port, index);
    vga_out8(data_port, value);
}

static void vga_begin_font_access(void)
{
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x00u, 0x01u);
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x02u, 0x04u);
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x04u, 0x07u);
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x00u, 0x03u);
    vga_write_indexed(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_DATA, 0x04u, 0x02u);
    vga_write_indexed(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_DATA, 0x05u, 0x00u);
    vga_write_indexed(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_DATA, 0x06u, 0x04u);
}

static void vga_end_font_access(void)
{
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x00u, 0x01u);
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x02u, 0x03u);
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x04u, 0x03u);
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x00u, 0x03u);
    vga_write_indexed(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_DATA, 0x06u, 0x0Eu);
    vga_write_indexed(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_DATA, 0x04u, 0x00u);
    vga_write_indexed(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_DATA, 0x05u, 0x10u);
}

void vga_capture_startup_font(void)
{
    uint32_t glyph;
    uint32_t row;

    vga_begin_font_access();
    for (glyph = 0u; glyph < VGA_GLYPH_COUNT; ++glyph) {
        for (row = 0u; row < VGA_GLYPH_HEIGHT; ++row) {
            startup_font[glyph * VGA_GLYPH_HEIGHT + row] =
                VGA_FONT_MEMORY[glyph * VGA_GLYPH_SLOT_SIZE + row];
        }
    }
    vga_end_font_access();
    startup_font_state = VGA_FONT_CAPTURED;
}

static void vga_restore_startup_font(void)
{
    uint32_t glyph;
    uint32_t row;

    if (startup_font_state != VGA_FONT_CAPTURED) {
        return;
    }

    vga_begin_font_access();
    for (glyph = 0u; glyph < VGA_GLYPH_COUNT; ++glyph) {
        for (row = 0u; row < VGA_GLYPH_HEIGHT; ++row) {
            VGA_FONT_MEMORY[glyph * VGA_GLYPH_SLOT_SIZE + row] =
                startup_font[glyph * VGA_GLYPH_HEIGHT + row];
        }
    }
    vga_end_font_access();
}

static void vga_restore_palette(void)
{
    uint32_t color;
    uint32_t component;

    vga_out8(VGA_DAC_MASK, 0xFFu);
    vga_out8(VGA_DAC_WRITE_INDEX, 0x00u);
    for (color = 0u; color < 16u; ++color) {
        for (component = 0u; component < 3u; ++component) {
            vga_out8(VGA_DAC_DATA, mode3_palette[color][component]);
        }
    }
}

void vga_restore_text_mode(void)
{
    uint32_t index;

    intel915_restore_text_display();
    /* VGA 80x25 color text mode, equivalent to the legacy mode-3 state. */
    vga_out8(VGA_MISC_WRITE, 0x67u);

    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x00u, 0x01u);
    for (index = 0u; index < 4u; ++index) {
        vga_write_indexed(VGA_SEQUENCER_INDEX,
                          VGA_SEQUENCER_DATA,
                          (uint8_t)(index + 1u),
                          mode3_sequencer[index]);
    }
    vga_write_indexed(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_DATA, 0x00u, 0x03u);

    vga_write_indexed(VGA_CRTC_INDEX, VGA_CRTC_DATA, 0x11u, 0x00u);
    for (index = 0u; index < 25u; ++index) {
        vga_write_indexed(VGA_CRTC_INDEX,
                          VGA_CRTC_DATA,
                          (uint8_t)index,
                          mode3_crtc[index]);
    }

    for (index = 0u; index < 9u; ++index) {
        vga_write_indexed(VGA_GRAPHICS_INDEX,
                          VGA_GRAPHICS_DATA,
                          (uint8_t)index,
                          mode3_graphics[index]);
    }

    for (index = 0u; index < 20u; ++index) {
        (void)vga_in8(VGA_INPUT_STATUS_1);
        vga_out8(VGA_ATTRIBUTE_INDEX, (uint8_t)index);
        vga_out8(VGA_ATTRIBUTE_INDEX, mode3_attribute[index]);
    }
    (void)vga_in8(VGA_INPUT_STATUS_1);
    vga_out8(VGA_ATTRIBUTE_INDEX, 0x14u);
    vga_out8(VGA_ATTRIBUTE_INDEX, 0x00u);

    vga_restore_palette();
    vga_restore_startup_font();

    /* Hide the hardware cursor and unblank attribute-controller output. */
    vga_write_indexed(VGA_CRTC_INDEX, VGA_CRTC_DATA, 0x0Au, 0x20u);
    (void)vga_in8(VGA_INPUT_STATUS_1);
    vga_out8(VGA_ATTRIBUTE_INDEX, 0x20u);
}
