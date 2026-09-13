#include "Azalia.h"

uint32_t azalia_verb(uint32_t codec, uint32_t node, uint32_t verb, uint32_t payload)
{
    if (codec > 14 || node > 255 || verb > 0xFFF || payload > 255) return 0xFFFFFFFFu;
    return (codec << 28) | (node << 20) | (verb << 8) | payload;
}

static uint32_t parameter(struct azalia_state *state, azalia_send_verb send, void *context,
                          uint32_t node, uint32_t id, uint32_t *value)
{
    return send(context, azalia_verb(state->codec_address, node, 0xF00, id), value);
}

uint32_t azalia_probe_codec(struct azalia_state *state, azalia_send_verb send, void *context)
{
    if (!state || !send) return AZALIA_BAD_REQUEST;
    state->codec_id = state->audio_group = state->widget_count = state->revision = 0;
    for (uint32_t codec = 0; codec < 15; ++codec) {
        if (!(state->codec_mask & (1u << codec))) continue;
        uint32_t vendor;
        state->codec_address = codec;
        if (!parameter(state, send, context, 0, 0, &vendor))
            return state->status = AZALIA_COMMAND_TIMEOUT;
        if (vendor == ALC269_CODEC_ID) { state->codec_id = vendor; break; }
    }
    if (!state->codec_id) return state->status = AZALIA_CODEC_NOT_FOUND;
    uint32_t nodes, type;
    if (!parameter(state, send, context, 0, 2, &state->revision) ||
        !parameter(state, send, context, 0, 4, &nodes)) goto timeout;
    uint32_t start = (nodes >> 16) & 255, count = nodes & 255;
    if (!start || !count || start + count > 256) goto bad_topology;
    for (uint32_t node = start; node < start + count; ++node) {
        if (!parameter(state, send, context, node, 5, &type)) goto timeout;
        if ((type & 255) == 1) { state->audio_group = node; break; }
    }
    if (!state->audio_group) goto bad_topology;
    if (!parameter(state, send, context, state->audio_group, 4, &nodes)) goto timeout;
    start = (nodes >> 16) & 255; count = nodes & 255;
    if (!start || !count || count > AZALIA_MAX_WIDGETS || start + count > 256) goto bad_topology;
    for (uint32_t i = 0; i < count; ++i) {
        struct azalia_widget *widget = &state->widgets[i];
        widget->node = start + i;
        widget->pin_default = widget->pcm = widget->formats = 0;
        if (!parameter(state, send, context, widget->node, 9, &widget->capabilities)) goto timeout;
        type = (widget->capabilities >> 20) & 15;
        if (type == 4 && !send(context,
            azalia_verb(state->codec_address, widget->node, 0xF1C, 0), &widget->pin_default)) goto timeout;
        if (type <= 1) {
            uint32_t format_node = (widget->capabilities & (1u << 4)) ? widget->node : state->audio_group;
            if (!parameter(state, send, context, format_node, 0xA, &widget->pcm) ||
                !parameter(state, send, context, format_node, 0xB, &widget->formats)) goto timeout;
        }
        ++state->widget_count;
    }
    return state->status = AZALIA_CODEC_READY;
timeout:
    return state->status = AZALIA_COMMAND_TIMEOUT;
bad_topology:
    return state->status = AZALIA_BAD_TOPOLOGY;
}

#ifndef AZALIA_TEST
#include "Include/KernelApi.h"
#include "Include/KernelRuntime.h"

static struct azalia_state audio;
static uint32_t initialized;

static uint32_t pci_read(uint32_t pci, uint32_t reg)
{
    uint32_t value;
    __asm__ volatile ("outl %0, %%dx" :: "a"(0x80000000u | pci | reg), "d"((uint16_t)0xCF8));
    __asm__ volatile ("inl %%dx, %0" : "=a"(value) : "d"((uint16_t)0xCFC));
    return value;
}

static uint32_t find_controller(void)
{
    for (uint32_t bus = 0; bus < 256; ++bus)
        for (uint32_t device = 0; device < 32; ++device) {
            uint32_t base = (bus << 16) | (device << 11);
            if ((pci_read(base, 0) & 0xFFFF) == 0xFFFF) continue;
            uint32_t functions = (pci_read(base, 0xC) & 0x800000u) ? 8 : 1;
            for (uint32_t function = 0; function < functions; ++function) {
                uint32_t pci = base | (function << 8);
                if ((pci_read(pci, 0) & 0xFFFF) != 0xFFFF && (pci_read(pci, 8) >> 8) == 0x040300)
                    return pci;
            }
        }
    return 0xFFFFFFFFu;
}

static uint32_t immediate_verb(void *context, uint32_t command, uint32_t *response)
{
    struct azalia_state *state = context;
    volatile uint16_t *status = (volatile uint16_t *)(AZALIA_MMIO + 0x68);
    if (*status & 1) return 0;
    *status = 2; /* Clear old IRV before submitting; CORB/RIRB must be stopped. */
    *(volatile uint32_t *)(AZALIA_MMIO + 0x60) = command;
    state->last_command = command;
    ++state->commands;
    *status = 1;
    for (uint32_t attempts = 0; attempts < 100000; ++attempts) {
        uint16_t value = *status;
        if (!(value & 1) && (value & 2)) {
            if ((value & 4) && ((value & 8) || (value >> 4) != (command >> 28))) return 0;
            *response = *(volatile uint32_t *)(AZALIA_MMIO + 0x64);
            *status = 2;
            return 1;
        }
        __asm__ volatile ("pause");
    }
    *status = 0;
    return 0;
}

uint32_t azalia_driver_service(uint32_t operation)
{
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    if ((cs & 3) != 1) return AZALIA_WRONG_RING;
    if (operation > 2) return AZALIA_BAD_REQUEST;
    if (__sync_lock_test_and_set(&audio.busy, 1)) return AZALIA_BUSY;
    audio.observed_cpl = cs & 3;
    if (operation == 0 && audio.status != AZALIA_CODEC_READY) {
        if (!(*(volatile uint32_t *)(AZALIA_MMIO + 8) & 1)) audio.status = AZALIA_LINK_STOPPED;
        else if ((*(volatile uint8_t *)(AZALIA_MMIO + 0x4C) & 2) ||
                 (*(volatile uint8_t *)(AZALIA_MMIO + 0x5C) & 2)) audio.status = AZALIA_COMMAND_DMA_ACTIVE;
        else {
            audio.codec_mask = *(volatile uint16_t *)(AZALIA_MMIO + 0xE) & 0x7FFF;
            azalia_probe_codec(&audio, immediate_verb, &audio);
        }
    }
    uint32_t result = operation == 2 ? audio.codec_id : audio.status;
    __sync_lock_release(&audio.busy);
    return result;
}

static void map_page(uint32_t address, uint32_t physical, uint32_t flags)
{
    if (KERNEL_API->get_physical_address(0x30030000, address) ||
        !KERNEL_API->map_page(address, physical, flags)) KPANIC("Cannot map Azalia driver memory");
}

void azalia_initialize(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
    if (flags & 0x200) KPANIC("Initialize Azalia before enabling interrupts");
    if (initialized) return;
    initialized = 1;
    *(uint32_t *)AZALIA_STATE_ADDRESS = (uint32_t)&audio;
    uint32_t pci = find_controller();
    if (pci == 0xFFFFFFFFu) return;
    audio.pci = pci;
    audio.controller_id = pci_read(pci, 0);
    uint32_t bar = pci_read(pci, 0x10), type = bar & 7;
    uint32_t base = bar & ~0xFu;
    if ((type != 0 && type != 4) || (type == 4 && pci_read(pci, 0x14)) ||
        base < 0x40000000u || base > 0xFFFFC000u || (base & 0x3FFF) || !(pci_read(pci, 4) & 2)) {
        audio.status = AZALIA_BAD_BAR;
        return;
    }
    audio.mmio_physical = base;
    for (uint32_t offset = 0; offset < 0x4000; offset += PAGE_SIZE)
        map_page(AZALIA_MMIO + offset, base + offset, 0x1B);
    audio.capabilities = *(volatile uint16_t *)AZALIA_MMIO;
    audio.codec_mask = *(volatile uint16_t *)(AZALIA_MMIO + 0xE) & 0x7FFF;
    if (audio.capabilities == 0xFFFF) { audio.status = AZALIA_BAD_BAR; return; }
    for (uint32_t offset = 0; offset < AZALIA_STACK_BYTES; offset += PAGE_SIZE) {
        uint32_t physical = KERNEL_API->pmm_alloc_page();
        if (!physical) KPANIC("No memory for Azalia ring-1 stacks");
        map_page(AZALIA_STACKS + offset, physical, PAGE_PRESENT | PAGE_RW);
    }
    memset((void *)AZALIA_STACKS, 0, AZALIA_STACK_BYTES);
    audio.code_selector = KERNEL_API->gdt_create_code_descriptor(0, 0xFFFFFFFFu, DESCRIPTOR_DPL1) | 1;
    audio.data_selector = KERNEL_API->gdt_create_data_descriptor(0, 0xFFFFFFFFu, DESCRIPTOR_DPL1) | 1;
    audio.gate_selector = KERNEL_API->gdt_alloc_selector();
    if (audio.code_selector == 1 || audio.data_selector == 1 || !audio.gate_selector)
        KPANIC("Cannot create Azalia ring-1 descriptors");
    uint32_t entry = *(const uint32_t *)AZALIA_ENTRY_ADDRESS;
    KERNEL_API->gdt_write_raw_descriptor(audio.gate_selector,
        (audio.code_selector << 16) | (entry & 0xFFFF), (entry & 0xFFFF0000) | 0xEC00);
    *(uint32_t *)AZALIA_DATA_ADDRESS = audio.data_selector;
    *(uint32_t *)AZALIA_SERVICE_ADDRESS = (uint32_t)azalia_driver_service;
    *(uint32_t *)AZALIA_GATE_ADDRESS = audio.gate_selector | 3;
    audio.status = AZALIA_CONTROLLER_READY;
}
#endif
