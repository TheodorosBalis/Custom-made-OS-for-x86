#ifndef BOOT_STORAGE_H
#define BOOT_STORAGE_H

#include "Include/KernelTypes.h"

struct boot_manifest {
    uint32_t magic[2];
    uint32_t version;
    uint32_t setup_sectors;
    uint32_t high_lba;
    uint32_t high_sectors;
    uint32_t high_crc;
    uint32_t interrupts_lba;
    uint32_t interrupts_sectors;
    uint32_t interrupts_crc;
};

struct usb_storage_interface {
    uint8_t configuration;
    uint8_t interface_number;
    uint8_t bulk_in;
    uint8_t bulk_out;
};

uint32_t boot_crc32(const void *memory, uint32_t size);
int boot_parse_usb_configuration(const uint8_t *data, uint32_t size,
                                 struct usb_storage_interface *result);
int boot_manifest_valid(const struct boot_manifest *manifest);
int storage_read_cdb_valid(const uint8_t *command, uint32_t command_size, uint32_t bytes);

#endif
