#include "Storage.h"

uint32_t storage_map_mmio_window(uint32_t physical, uint32_t *mapped_page, storage_map_page_fn map)
{
    uint32_t page = physical & ~0xFFFu;
    uint32_t offset = physical & 0xFFFu;
    if (!page || !mapped_page || !map || (physical & 15u) || offset > 0xF00u) return 0;
    if (*mapped_page != page) {
        if (!map(STORAGE_MMIO, page, 0x1Bu)) return 0;
        *mapped_page = page;
    }
    return STORAGE_MMIO + offset;
}

uint32_t storage_valid_range(uint32_t capacity, uint32_t lba, uint32_t blocks)
{
    return blocks != 0u && blocks <= STORAGE_MAX_BLOCKS &&
           lba < capacity && blocks <= capacity - lba;
}

uint32_t storage_identify_capacity(const uint16_t *words)
{
    if ((words[0] & 0x8004u) != 0u || (words[49] & 0x200u) == 0u) return 0;
    /* Reject non-512-byte logical sectors; do not guess their transfer size. */
    if ((words[106] & 0xC000u) == 0x4000u && (words[106] & 0x1000u) != 0u &&
        (words[117] != 256u || words[118] != 0u)) return 0;
    uint32_t blocks = words[60] | ((uint32_t)words[61] << 16);
    return blocks > 0x10000000u ? 0x10000000u : blocks;
}

uint32_t storage_user_range(uint32_t slot, uint32_t offset, uint32_t size)
{
    /* User pointers are offsets in the process's 5 MiB data segment. */
    if (slot < 0x100000u || slot > 0x40000000u - 0xA00000u ||
        !size || offset >= 0x500000u || size > 0x500000u - offset) return 0;
    return slot + 0x400000u + offset;
}

uint32_t storage_user_page_writable(uint32_t pde, uint32_t pte)
{
    /* Present, user, writable at BOTH levels; no large pages or shared frames. */
    return (pde & 0x87u) == 7u && (pte & 0x207u) == 0x207u;
}

#ifndef STORAGE_TEST
#include "Include/KernelApi.h"
#include "Include/KernelRuntime.h"

static volatile uint32_t *storage_map_mmio(uint32_t physical);
static int storage_usb_candidate(uint32_t pci);
static void storage_handoff_ports(uint32_t ports, uint32_t keep_port);

/* Build the same tested transport for ring 0 as for the pre-paging bootstrap.
 * KERNEL_STORAGE removes boot-image matching/entry and supplies an MMIO alias. */
#define KERNEL_STORAGE
#include "../../Boot/BootStorage.c"

#define STORAGE_CALLBACK 0x8000234Cu
#define PROC_PD_WORD 0u
#define PROC_SLOT_WORD 14u
#define PROC_TYPE_WORD 15u
#define PROC_STORAGE_WORD 17u /* PROC_STORAGE_ACCESS = 68 in VirtualKernel.asm */
#define TABLE_POOL_START 0x30500000u
#define TABLE_POOL_END 0x30800000u

static struct storage_device_info devices[STORAGE_DEVICE_COUNT];
static uint32_t initialized, discovered, device_mask;
static uint32_t mapped_mmio;
static uint8_t ata_dead[4];
static uint32_t usb_discovered, companion_target, companions_ready;
static uint32_t companion_ports, companion_keep;

static int storage_usb_candidate(uint32_t pci)
{
    return !companion_target || pci == companion_target;
}

static void storage_handoff_ports(uint32_t ports, uint32_t keep_port)
{
    if (!companion_target || !ehci) return;
    companion_ports = ports;
    companion_keep = keep_port;
    for (uint32_t port = 0; port < ports; ++port) {
        if (port == keep_port) continue;
        uint32_t index = (0x44u + port * 4u) / 4u;
        /* Retain the selected high-speed disk; route other ports to UHCI.
         * Preserve unrelated controls and never echo W1C status bits. */
        ehci[index] = (ehci[index] & ~(0x2Au | 4u)) | (1u << 13);
        (void)ehci[index];
    }
    companions_ready = 1;
}

static uint32_t enter_storage(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void leave_storage(uint32_t flags)
{
    if (flags & 0x200u) __asm__ volatile ("sti" ::: "memory");
}

static volatile uint32_t *storage_map_mmio(uint32_t physical)
{
    /* EHCI BAR alignment is not page alignment. Keep the offset in the alias. */
    return (volatile uint32_t *)storage_map_mmio_window(physical, &mapped_mmio, KERNEL_API->map_page);
}

static int identify_ata(void)
{
    out8(ata_port + 0x206, 2); /* nIEN. No interrupt-driven ATA path yet. */
    out8(ata_port + 6, (uint8_t)(0xA0u | (ata_device << 4)));
    for (uint32_t i = 0; i < 4; ++i) (void)in8(ata_port + 0x206);
    uint8_t status = in8(ata_port + 7);
    if (status == 0 || status == 255 || (status & 0x88u)) return 0;
    out8(ata_port + 2, 0);
    out8(ata_port + 3, 0);
    out8(ata_port + 4, 0);
    out8(ata_port + 5, 0);
    out8(ata_port + 7, 0xEC); /* IDENTIFY DEVICE: read-only, no media modification. */
    for (uint32_t i = 0; i < 4; ++i) (void)in8(ata_port + 0x206);
    if (!ata_wait(8, 8)) return 0;
    for (uint32_t word = 0; word < 256; ++word) {
        uint16_t value;
        __asm__ volatile ("inw %w1, %0" : "=a"(value) : "Nd"(ata_port));
        ((uint16_t *)TRANSFER_BUFFER)[word] = value;
    }
    for (uint32_t i = 0; i < 4; ++i) (void)in8(ata_port + 0x206);
    return ata_wait(0x88, 0);
}

static void discover_ata(void)
{
    uint32_t channels_seen = 0;
    for (uint32_t pci = 0; pci < 0x1000000u; pci += 0x100u) {
        if ((pci_read(pci, 0) & 0xFFFFu) == 0xFFFFu) continue;
        uint32_t code = pci_read(pci, 8);
        if ((code >> 16) != 0x0101u || !(pci_read(pci, 4) & 1u)) continue;
        uint32_t mode = (code >> 8) & 255u;
        for (uint32_t channel = 0; channel < 2; ++channel) {
            if ((mode & (channel ? 4u : 1u)) || (channels_seen & (1u << channel))) continue;
            channels_seen |= 1u << channel;
            ata_port = channel ? 0x170 : 0x1F0;
            for (ata_device = 0; ata_device < 2; ++ata_device) {
                uint32_t id = 1u + channel * 2u + ata_device;
                if (!identify_ata()) continue;
                const uint16_t *words = (const uint16_t *)TRANSFER_BUFFER;
                uint32_t capacity = storage_identify_capacity(words);
                if (!capacity) continue;
                devices[id].type = STORAGE_ATA;
                devices[id].sector_size = 512;
                devices[id].block_count = capacity;
                devices[id].flags = STORAGE_READ_ONLY;
                for (uint32_t i = 0; i < 20; ++i) {
                    devices[id].model[i * 2] = words[27 + i] >> 8;
                    devices[id].model[i * 2 + 1] = (uint8_t)words[27 + i];
                }
                device_mask |= 1u << id;
            }
        }
    }
}

static void discover_usb(void)
{
    if (usb_discovered) return;
    usb_discovered = 1;
    calibrate_timer();
    /* Boot stopped EHCI before entering the kernel; these pinned DMA pages
     * remain identity mapped, supervisor-only, and excluded from the PMM. */
    zero((void *)DMA_BASE, 0x7000);
    if (find_usb_disk()) {
        devices[0].type = STORAGE_USB;
        devices[0].sector_size = 512;
        devices[0].block_count = last_block + 1;
        devices[0].flags = STORAGE_READ_ONLY;
        memset(devices[0].model, ' ', sizeof(devices[0].model));
        memcpy(devices[0].model, "USB mass storage", 16);
        device_mask |= 1;
    }
}

uint32_t storage_prepare_usb_companions(uint32_t ehci_pci)
{
    uint32_t flags = enter_storage();
    if (!initialized || usb_discovered) {
        leave_storage(flags);
        return companions_ready ? ticks_per_ms : 0;
    }
    companion_target = ehci_pci;
    discover_usb();
    leave_storage(flags);
    return companions_ready ? ticks_per_ms : 0;
}

void storage_refresh_usb_companions(void)
{
    uint32_t flags = enter_storage();
    if (companions_ready && ehci) {
        for (uint32_t port = 0; port < companion_ports; ++port) {
            if (port == companion_keep) continue;
            uint32_t index = (0x44u + port * 4u) / 4u;
            uint32_t value = ehci[index];
            /* Disconnect returns ownership to EHCI. Never touch the disk port. */
            if ((value & 0x2001u) == 1u) {
                ehci[index] = (value & ~(0x2Au | 4u)) | 0x2000u;
                (void)ehci[index];
            }
        }
    }
    leave_storage(flags);
}

static void discover_storage(void)
{
    if (discovered) return;
    discovered = 1;
    discover_usb();
    discover_ata();
}

void storage_initialize(void)
{
    uint32_t flags = enter_storage();
    if (!initialized) {
        /* Allocate the shared MMIO page table now, before process creation.
         * The placeholder is never dereferenced; discovery replaces its PTE. */
        if (!KERNEL_API->map_page(STORAGE_MMIO, 0x3004F000u, PAGE_PRESENT | PAGE_RW)) {
            KPANIC("Cannot reserve storage MMIO mapping");
        }
        *(uint32_t *)STORAGE_CALLBACK = (uint32_t)storage_syscall;
        initialized = 1;
    }
    leave_storage(flags);
}

uint32_t storage_device_mask(void)
{
    uint32_t flags = enter_storage();
    if (initialized) discover_storage();
    uint32_t result = device_mask;
    leave_storage(flags);
    return result;
}

uint32_t storage_get_info(uint32_t device, struct storage_device_info *info)
{
    if (!initialized) return STORAGE_NOT_READY;
    if (!info || device >= STORAGE_DEVICE_COUNT) return STORAGE_BAD_ARGUMENT;
    uint32_t flags = enter_storage();
    discover_storage();
    uint32_t result = STORAGE_NO_DEVICE;
    if (device_mask & (1u << device)) {
        memcpy(info, &devices[device], sizeof(*info));
        result = STORAGE_OK;
    }
    leave_storage(flags);
    return result;
}

uint32_t storage_read(uint32_t device, uint32_t lba, uint32_t blocks, void *buffer)
{
    if (!initialized) return STORAGE_NOT_READY;
    if (!buffer || device >= STORAGE_DEVICE_COUNT || !blocks || blocks > STORAGE_MAX_BLOCKS) {
        return STORAGE_BAD_ARGUMENT;
    }
    uint32_t flags = enter_storage();
    discover_storage();
    uint32_t result = STORAGE_NO_DEVICE;
    if (!(device_mask & (1u << device))) goto done;
    result = STORAGE_BAD_ARGUMENT;
    if (!storage_valid_range(devices[device].block_count, lba, blocks)) goto done;
    result = STORAGE_IO_ERROR;
    int success;
    if (device == 0) {
        success = usb_read(lba, blocks);
    } else {
        if (ata_dead[device - 1]) goto done;
        ata_port = device > 2 ? 0x170 : 0x1F0;
        ata_device = (uint8_t)((device - 1) & 1u);
        success = ata_read(lba, blocks);
        if (!success) {
            /* Do not issue more commands on a channel whose state is unknown. */
            uint32_t first = (device - 1) & ~1u;
            ata_dead[first] = ata_dead[first + 1] = 1;
        }
    }
    if (success) {
        memcpy(buffer, TRANSFER_BUFFER, blocks * 512u);
        result = STORAGE_OK;
    }
done:
    leave_storage(flags);
    return result;
}

void storage_allow_process_reads(void *process, uint32_t allow)
{
    if (process) ((uint32_t *)process)[PROC_STORAGE_WORD] = allow ? 1u : 0u;
}

static int valid_table(uint32_t physical)
{
    return physical >= TABLE_POOL_START && physical < TABLE_POOL_END && !(physical & 4095u);
}

static void *user_output(uint32_t *process, uint32_t offset, uint32_t size)
{
    uint32_t linear = storage_user_range(process[PROC_SLOT_WORD], offset, size);
    uint32_t pd = process[PROC_PD_WORD];
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    if (!linear || process[PROC_TYPE_WORD] != 0 || !valid_table(pd) ||
        (cr3 & ~4095u) != pd) return NULL;
    uint32_t end = linear + size - 1;
    for (uint32_t page = linear & ~4095u; page <= (end & ~4095u); page += 4096) {
        uint32_t pde = ((const uint32_t *)pd)[page >> 22];
        /* Only normal, user-writable 4 KiB pages owned by this process. */
        if ((pde & 0x87u) != 7u || !valid_table(pde & ~4095u)) return NULL;
        uint32_t pte = ((const uint32_t *)(pde & ~4095u))[(page >> 12) & 1023u];
        if (!storage_user_page_writable(pde, pte)) return NULL;
    }
    return (void *)linear;
}

uint32_t storage_syscall(struct storage_registers *r, void *owner)
{
    if (!initialized) return STORAGE_NOT_READY;
    if (!owner) return STORAGE_ACCESS_DENIED;
    uint32_t *process = owner;
    /* Raw disk reads reveal files regardless of process address-space isolation. */
    if (process[PROC_STORAGE_WORD] != 1 || process[PROC_TYPE_WORD] != 0) {
        return STORAGE_ACCESS_DENIED;
    }
    if (r->eax == SYSCALL_STORAGE_DEVICES) return storage_device_mask();
    if (r->eax != SYSCALL_STORAGE_INFO && r->eax != SYSCALL_STORAGE_READ) return STORAGE_BAD_ARGUMENT;
    uint32_t size = r->eax == SYSCALL_STORAGE_INFO ? sizeof(struct storage_device_info) : 512u;
    void *buffer = user_output(process, r->edi, size);
    if (!buffer) return STORAGE_BAD_BUFFER;
    if (r->eax == SYSCALL_STORAGE_INFO) return storage_get_info(r->ebx, buffer);
    return storage_read(r->ebx, r->esi, 1u, buffer);
}
#endif
