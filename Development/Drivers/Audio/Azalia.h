#ifndef AZALIA_H
#define AZALIA_H

#include "Include/KernelTypes.h"

#define AZALIA_MMIO 0x72400000u
#define AZALIA_STACKS 0x72020000u
#define AZALIA_STACK_BYTES (100u * 8192u)
#define AZALIA_GATE_ADDRESS 0x80002368u
#define AZALIA_DATA_ADDRESS 0x8000236Cu
#define AZALIA_SERVICE_ADDRESS 0x80002370u
#define AZALIA_STATE_ADDRESS 0x80002374u
#define AZALIA_ENTRY_ADDRESS 0x80002378u
#define ALC269_CODEC_ID 0x10EC0269u
#define AZALIA_MAX_WIDGETS 64u

enum azalia_status {
    AZALIA_ABSENT, AZALIA_CONTROLLER_READY, AZALIA_CODEC_READY,
    AZALIA_BAD_BAR = 0x10, AZALIA_LINK_STOPPED, AZALIA_COMMAND_DMA_ACTIVE,
    AZALIA_COMMAND_TIMEOUT, AZALIA_CODEC_NOT_FOUND, AZALIA_BAD_TOPOLOGY,
    AZALIA_BUSY, AZALIA_WRONG_RING, AZALIA_BAD_REQUEST
};

struct azalia_widget {
    uint32_t node, capabilities, pin_default, pcm, formats;
};

struct azalia_state {
    uint32_t status, pci, controller_id, mmio_physical;
    uint32_t capabilities, codec_mask, codec_address, codec_id, revision;
    uint32_t audio_group, widget_count, code_selector, data_selector, gate_selector;
    uint32_t busy, observed_cpl, commands, last_command;
    struct azalia_widget widgets[AZALIA_MAX_WIDGETS];
};

typedef uint32_t (*azalia_send_verb)(void *context, uint32_t command, uint32_t *response);
uint32_t azalia_verb(uint32_t codec, uint32_t node, uint32_t verb, uint32_t payload);
uint32_t azalia_probe_codec(struct azalia_state *state, azalia_send_verb send, void *context);
void azalia_initialize(void);
uint32_t azalia_driver_service(uint32_t operation);

#endif
