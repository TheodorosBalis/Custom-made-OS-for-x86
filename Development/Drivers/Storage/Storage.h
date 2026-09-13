#ifndef STORAGE_H
#define STORAGE_H

#include "Include/KernelTypes.h"

#define STORAGE_SECTOR_SIZE 512u
#define STORAGE_MAX_BLOCKS 32u
#define STORAGE_DEVICE_COUNT 5u
#define STORAGE_USB 1u
#define STORAGE_ATA 2u
#define STORAGE_READ_ONLY 1u
#define STORAGE_MMIO 0x80C00000u

#define SYSCALL_STORAGE_INFO 5u
#define SYSCALL_STORAGE_READ 6u
#define SYSCALL_STORAGE_DEVICES 7u

enum storage_result {
    STORAGE_OK = 0,
    STORAGE_NO_DEVICE = 0xFFFF0001u,
    STORAGE_BAD_ARGUMENT,
    STORAGE_IO_ERROR,
    STORAGE_ACCESS_DENIED,
    STORAGE_BAD_BUFFER,
    STORAGE_NOT_READY
};

struct storage_device_info {
    uint32_t type;
    uint32_t sector_size;
    uint32_t block_count; /* Addressable blocks; ATA is limited to LBA28 for now. */
    uint32_t flags;
    char model[40];      /* Space padded, not NUL terminated. */
};

/* Ring 0 APIs. Initialize before creating processes. Discovery is lazy. */
void storage_initialize(void);
/* Host coordinator only: finish EHCI discovery/handoff before starting UHCI.
 * Returns calibrated TSC ticks/ms, or zero if handoff was not completed. */
uint32_t storage_prepare_usb_companions(uint32_t ehci_pci);
void storage_refresh_usb_companions(void);
uint32_t storage_device_mask(void); /* Bit 0 USB, bits 1..4 ATA channel/device. */
uint32_t storage_get_info(uint32_t device, struct storage_device_info *info);
uint32_t storage_read(uint32_t device, uint32_t lba, uint32_t blocks, void *buffer);
void storage_allow_process_reads(void *process, uint32_t allow);

/* PUSHAD layout used only by the SYSENTER assembly dispatcher. */
struct storage_registers {
    uint32_t edi, esi, ebp, ignored_esp, ebx, edx, ecx, eax;
};
uint32_t storage_syscall(struct storage_registers *registers, void *process);

/* Pure validation shared with host tests. */
uint32_t storage_valid_range(uint32_t capacity, uint32_t lba, uint32_t blocks);
uint32_t storage_identify_capacity(const uint16_t *identify);
uint32_t storage_user_range(uint32_t slot, uint32_t offset, uint32_t size);
uint32_t storage_user_page_writable(uint32_t pde, uint32_t pte);

typedef uint32_t (__attribute__((stdcall)) *storage_map_page_fn)(uint32_t virtual_address,
                                                               uint32_t physical, uint32_t flags);
uint32_t storage_map_mmio_window(uint32_t physical, uint32_t *mapped_page, storage_map_page_fn map);

#endif
