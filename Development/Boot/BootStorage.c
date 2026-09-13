#include "BootStorage.h"

/* Shared read-only transport; KERNEL_STORAGE supplies the post-paging hooks. */
int storage_read_cdb_valid(const uint8_t *command, uint32_t command_size, uint32_t bytes)
{
    if (!command) return 0;
    if (command_size == 6) {
        if (command[0] == 0x00) return bytes == 0;
        if (command[0] == 0x03) return bytes == 18 && command[4] == 18;
    }
    if (command_size == 10) {
        if (command[0] == 0x25) return bytes == 8;
        if (command[0] == 0x28) {
            uint32_t blocks = ((uint32_t)command[7] << 8) | command[8];
            return blocks > 0 && blocks <= 32 && bytes == blocks * 512;
        }
    }
    return 0;
}

uint32_t boot_crc32(const void *memory, uint32_t size)
{
    const uint8_t *bytes = memory;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

int boot_manifest_valid(const struct boot_manifest *m)
{
    return m->magic[0] == 0x4253554Eu && m->magic[1] == 0x544F4F42u &&
           m->version == 1 && m->setup_sectors == 64 &&
           m->high_lba == 65 && m->high_sectors == 256 &&
           m->interrupts_lba == 321 && m->interrupts_sectors == 2049;
}

int boot_parse_usb_configuration(const uint8_t *data, uint32_t size,
                                 struct usb_storage_interface *result)
{
    struct usb_storage_interface candidate = {0, 0, 0, 0};
    int storage = 0;
    if (size < 9 || data[0] != 9 || data[1] != 2 ||
        ((uint32_t)data[2] | ((uint32_t)data[3] << 8)) != size || data[5] == 0) {
        return 0;
    }
    candidate.configuration = data[5];
    for (uint32_t offset = 9; offset < size;) {
        uint32_t length = data[offset];
        if (length < 2 || length > size - offset) return 0;
        const uint8_t *d = data + offset;
        if (d[1] == 4) {
            if (length < 9) return 0;
            if (storage && candidate.bulk_in && candidate.bulk_out) {
                *result = candidate;
                return 1;
            }
            storage = d[3] == 0 && d[4] == 2 && d[5] == 8 && d[6] == 6 && d[7] == 0x50;
            candidate.interface_number = d[2];
            candidate.bulk_in = candidate.bulk_out = 0;
        } else if (d[1] == 5 && storage) {
            if (length < 7) return 0;
            uint32_t packet = (uint32_t)d[4] | ((uint32_t)d[5] << 8);
            if ((d[3] & 3) != 2 || (d[2] & 15) == 0 || (d[2] & 0x70) || packet != 512) return 0;
            if (d[2] & 0x80) {
                if (candidate.bulk_in) return 0;
                candidate.bulk_in = d[2];
            } else {
                if (candidate.bulk_out) return 0;
                candidate.bulk_out = d[2];
            }
        }
        offset += length;
    }
    if (!storage || !candidate.bulk_in || !candidate.bulk_out) return 0;
    *result = candidate;
    return 1;
}

#ifndef BOOT_STORAGE_TEST

#define DMA_BASE 0x30050000u
#define TRANSFER_BUFFER ((uint8_t *)0x30052000u)
#define CONTROL_BUFFER ((uint8_t *)0x30051000u)
#define CBW_BUFFER ((uint8_t *)0x30051400u)
#define CSW_BUFFER ((uint8_t *)0x30051440u)
#define SETUP_BUFFER ((uint8_t *)0x30051480u)
#define MANIFEST ((const struct boot_manifest *)0x30000800u)
#define BOOT_REPORT ((volatile uint32_t *)0x3004F000u)
#define BLOCK_SIZE 512u
#define BLOCKS_PER_READ 32u

struct qtd {
    volatile uint32_t next, alternate, token, buffer[5], high[5];
    uint32_t padding[3];
} __attribute__((aligned(32)));

struct queue_head {
    volatile uint32_t link, endpoint, capabilities, current;
    volatile uint32_t next, alternate, token, buffer[5], high[5];
    uint32_t padding[7];
} __attribute__((aligned(32)));

_Static_assert(sizeof(struct qtd) == 64, "qTD stride");
_Static_assert(sizeof(struct queue_head) == 96, "QH stride");
_Static_assert(sizeof(struct boot_manifest) == 40, "On-disk manifest ABI");

static struct queue_head *const queue = (struct queue_head *)DMA_BASE;
static struct qtd *const descriptors = (struct qtd *)(DMA_BASE + 0x100);
static volatile uint32_t *ehci;
static uint32_t ticks_per_ms;
static uint32_t usb_address, usb_tag, last_block;
static uint32_t toggle_in, toggle_out;
static uint32_t transport_dead;
static struct usb_storage_interface usb_interface;
static uint16_t ata_port;
static uint8_t ata_device;
#ifndef KERNEL_STORAGE
static uint32_t backend;
#endif

extern uint8_t __boot_bss_start[], __boot_bss_end[];

static void zero(void *destination, uint32_t size)
{
    uint8_t *d = destination;
    while (size--) *d++ = 0;
}

static void copy(void *destination, const void *source, uint32_t size)
{
    uint8_t *d = destination;
    const uint8_t *s = source;
    while (size--) *d++ = *s++;
}

#ifndef KERNEL_STORAGE
static int equal(const void *a, const void *b, uint32_t size)
{
    const uint8_t *left = a, *right = b;
    while (size--) if (*left++ != *right++) return 0;
    return 1;
}
#endif

static uint8_t in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %w1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %w1" :: "a"(value), "Nd"(port));
}

static uint32_t pci_read(uint32_t device, uint32_t offset)
{
    uint32_t value, address = 0x80000000u | device | offset;
    __asm__ volatile ("outl %0, %w1" :: "a"(address), "Nd"((uint16_t)0xCF8));
    __asm__ volatile ("inl %w1, %0" : "=a"(value) : "Nd"((uint16_t)0xCFC));
    return value;
}

static void pci_write(uint32_t device, uint32_t offset, uint32_t value)
{
    uint32_t address = 0x80000000u | device | offset;
    __asm__ volatile ("outl %0, %w1" :: "a"(address), "Nd"((uint16_t)0xCF8));
    __asm__ volatile ("outl %0, %w1" :: "a"(value), "Nd"((uint16_t)0xCFC));
}

static uint32_t tsc(void)
{
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return low;
}

static void screen(const char *message, uint32_t detail)
{
#ifdef KERNEL_STORAGE
    (void)message;
    (void)detail;
#else
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    for (uint32_t i = 0; i < 80; ++i) vga[i] = 0x1F20;
    uint32_t column = 0;
    while (*message && column < 65) vga[column++] = 0x1F00u | (uint8_t)*message++;
    vga[column++] = 0x1F20;
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t digit = (detail >> (28 - i * 4)) & 15;
        vga[column++] = 0x1F00u | (digit < 10 ? '0' + digit : 'A' + digit - 10);
    }
#endif
}

__attribute__((noreturn)) static void fail(const char *message, uint32_t detail)
{
#ifdef KERNEL_STORAGE
    (void)detail;
    KPANIC(message);
#else
    screen(message, detail);
    for (;;) __asm__ volatile ("cli; hlt");
#endif
}

static void calibrate_timer(void)
{
    /* PIT channel 2, mode 0: 23864 clocks ~= 20 ms. Do not change IRQ0. */
    uint8_t original = in8(0x61);
    out8(0x61, original & ~3u);
    out8(0x43, 0xB0);
    out8(0x42, (uint8_t)23864);
    out8(0x42, (uint8_t)(23864 >> 8));
    uint32_t start = tsc();
    out8(0x61, (original & ~2u) | 1u);
    uint32_t limit = 10000000;
    while (!(in8(0x61) & 0x20) && --limit) __asm__ volatile ("pause");
    ticks_per_ms = (tsc() - start) / 20;
    out8(0x61, original);
    if (!limit || ticks_per_ms < 10000 || ticks_per_ms > 4000000) fail("Boot timer failed", ticks_per_ms);
}

static void delay(uint32_t ms)
{
    uint32_t start = tsc(), duration = ms * ticks_per_ms;
    while ((uint32_t)(tsc() - start) < duration) __asm__ volatile ("pause");
}

static int wait_register(uint32_t offset, uint32_t mask, uint32_t value)
{
    uint32_t start = tsc();
    do {
        if ((ehci[offset / 4] & mask) == value) return 1;
        __asm__ volatile ("pause");
    } while ((uint32_t)(tsc() - start) < ticks_per_ms * 1000u);
    return 0;
}

static void stop_controller(void)
{
    if (!ehci) return;
    ehci[0] = 0;
    if (!wait_register(4, 1u << 12, 1u << 12)) fail("EHCI cannot stop DMA", ehci[1]);
    ehci[2] = 0;
}

static int start_controller(uint32_t device)
{
    BOOT_REPORT[1] = 2;
    BOOT_REPORT[2] = device;
    uint32_t bar = pci_read(device, 0x10);
    if ((bar & 7) != 0 || (bar & ~15u) < 0x80000000u || bar == 0xFFFFFFFFu) return 0;
    /* Command's upper word is W1C PCI status: never write it back as ones. */
    pci_write(device, 4, (pci_read(device, 4) & 0xFFFFu) | 6u);
#ifdef KERNEL_STORAGE
    volatile uint32_t *capability = storage_map_mmio(bar & ~15u);
    if (!capability) return 0;
#else
    volatile uint32_t *capability = (volatile uint32_t *)(bar & ~15u);
#endif
    uint32_t length = capability[0] & 255;
    if (length < 0x10 || length > 0x80 || (length & 3)) return 0;
    uint32_t extended = (capability[2] >> 8) & 255;
    for (uint32_t count = 0; extended && count < 48; ++count) {
        if (extended < 0x40 || extended > 0xF8 || (extended & 3)) return 0;
        uint32_t legacy = pci_read(device, extended);
        if ((legacy & 255) == 1) {
            pci_write(device, extended, legacy | (1u << 24));
            uint32_t start = tsc();
            while (pci_read(device, extended) & (1u << 16)) {
                if ((uint32_t)(tsc() - start) >= ticks_per_ms * 1000u) return 0;
            }
            pci_write(device, extended + 4, 0); /* Disable legacy USB SMI enables. */
        }
        extended = (legacy >> 8) & 255;
        if (count == 47 && extended) return 0;
    }
    ehci = (volatile uint32_t *)((uint32_t)capability + length);
    stop_controller();
    ehci[0] = 2;
    if (!wait_register(0, 2, 0)) fail("EHCI reset timeout", device);
    ehci[2] = 0;
    ehci[1] = 0x3F;
    if (capability[2] & 1) ehci[4] = 0;
    volatile uint32_t *periodic = (volatile uint32_t *)0x30056000u;
    for (uint32_t i = 0; i < 1024; ++i) periodic[i] = 1;
    ehci[5] = (uint32_t)periodic;
    ehci[6] = (uint32_t)queue;
    ehci[0] = 1;
    if (!wait_register(4, 1u << 12, 0)) fail("EHCI run timeout", device);
    ehci[0x40 / 4] = 1;
    uint32_t ports = capability[1] & 15;
    for (uint32_t port = 0; port < ports; ++port) {
        uint32_t value = ehci[(0x44 / 4) + port] & ~0x2Au;
        if (capability[1] & 16) value |= 1u << 12;
        ehci[(0x44 / 4) + port] = value;
    }
    delay(100);
    return (int)ports;
}

static void make_qtd(uint32_t index, uint32_t pid, uint32_t toggle,
                     void *buffer, uint32_t length, uint32_t next)
{
    struct qtd *q = &descriptors[index];
    zero(q, sizeof(*q));
    q->next = next;
    q->alternate = 1;
    q->token = (toggle << 31) | (length << 16) | (3u << 10) | (pid << 8) | 0x80;
    q->buffer[0] = (uint32_t)buffer;
    for (uint32_t i = 1; i < 5; ++i) q->buffer[i] = ((uint32_t)buffer & ~4095u) + i * 4096;
}

static int transfer(uint32_t endpoint, uint32_t packet_size, uint32_t last)
{
    if (transport_dead) return 0;
    zero(queue, sizeof(*queue));
    queue->link = (uint32_t)queue | 2;
    queue->endpoint = usb_address | (endpoint << 8) | (2u << 12) |
                      (1u << 14) | (1u << 15) | (packet_size << 16) | (8u << 28);
    queue->capabilities = 1u << 30;
    queue->next = (uint32_t)&descriptors[0];
    queue->alternate = 1;
    __asm__ volatile ("lock; addl $0, (%%esp)" ::: "memory", "cc");
    ehci[6] = (uint32_t)queue;
    ehci[0] = 1u | (1u << 5);
    uint32_t start = tsc();
    int success = 0;
    do {
        if (ehci[1] & (1u << 4)) break;
        if (queue->token & 0x7C) break;
        if (!(descriptors[last].token & 0x80)) {
            success = !(descriptors[last].token & 0x7C);
            break;
        }
        __asm__ volatile ("pause" ::: "memory");
    } while ((uint32_t)(tsc() - start) < ticks_per_ms * 1000u);
    ehci[0] = 1;
    /* No descriptor/buffer may be reused while the controller can still see it. */
    if (!wait_register(4, 1u << 15, 0)) fail("EHCI schedule will not stop", ehci[1]);
    __asm__ volatile ("" ::: "memory");
    if (!success) {
        BOOT_REPORT[4] = ehci[1];
        BOOT_REPORT[5] = queue->token;
        transport_dead = 1;
    }
    return success;
}

static int control(uint8_t type, uint8_t request, uint16_t value,
                   uint16_t index, uint16_t length)
{
    uint8_t *s = SETUP_BUFFER;
    s[0] = type; s[1] = request; s[2] = (uint8_t)value; s[3] = value >> 8;
    s[4] = (uint8_t)index; s[5] = index >> 8; s[6] = (uint8_t)length; s[7] = length >> 8;
    uint32_t status = length ? 2 : 1;
    make_qtd(0, 2, 0, s, 8, (uint32_t)&descriptors[1]);
    if (length) {
        make_qtd(1, type & 0x80 ? 1 : 0, 1, CONTROL_BUFFER, length, (uint32_t)&descriptors[2]);
        descriptors[1].alternate = (uint32_t)&descriptors[2];
    }
    make_qtd(status, length && (type & 0x80) ? 0 : 1, 1, NULL, 0, 1);
    if (!transfer(0, 64, status)) return 0;
    return !length || ((descriptors[1].token >> 16) & 0x7FFFu) == 0;
}

static int enumerate_storage(void)
{
    BOOT_REPORT[1] = 4;
    usb_address = 0;
    transport_dead = toggle_in = toggle_out = 0;
    if (!control(0x80, 6, 0x100, 0, 18) || CONTROL_BUFFER[7] != 64) return 0;
    uint32_t configurations = CONTROL_BUFFER[17];
    if (CONTROL_BUFFER[0] != 18 || CONTROL_BUFFER[1] != 1 || !configurations) return 0;
    if (!control(0, 5, 1, 0, 0)) return 0;
    usb_address = 1;
    delay(2);
    BOOT_REPORT[1] = 5;
    for (uint32_t index = 0; index < configurations; ++index) {
        if (!control(0x80, 6, (uint16_t)(0x200 | index), 0, 9)) return 0;
        uint32_t size = CONTROL_BUFFER[2] | ((uint32_t)CONTROL_BUFFER[3] << 8);
        if (size < 9 || size > 1024) continue;
        if (!control(0x80, 6, (uint16_t)(0x200 | index), 0, (uint16_t)size)) return 0;
        if (!boot_parse_usb_configuration(CONTROL_BUFFER, size, &usb_interface)) continue;
        if (!control(0, 9, usb_interface.configuration, 0, 0)) return 0;
        delay(10);
        return 1;
    }
    return 0;
}

static int bulk(int input, void *buffer, uint32_t length)
{
    uint32_t *toggle = input ? &toggle_in : &toggle_out;
    uint32_t endpoint = (input ? usb_interface.bulk_in : usb_interface.bulk_out) & 15;
    make_qtd(0, input ? 1 : 0, *toggle, buffer, length, 1);
    if (!transfer(endpoint, 512, 0)) return 0;
    uint32_t remaining = (descriptors[0].token >> 16) & 0x7FFFu;
    /* These commands require full lengths; reject short payloads instead of loading garbage. */
    if (remaining) { transport_dead = 1; return 0; }
    *toggle ^= ((length + 511) / 512) & 1;
    return 1;
}

static uint32_t be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void put_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = value >> 24; bytes[1] = value >> 16;
    bytes[2] = value >> 8; bytes[3] = (uint8_t)value;
}

/* 1 = success, 0 = SCSI check condition, -1 = dead transport (no unsafe retries). */
static int scsi(const uint8_t *command, uint32_t command_size, uint32_t length)
{
    /* No caller can issue a write, format, erase or vendor-specific CDB. */
    if (!storage_read_cdb_valid(command, command_size, length)) return -1;
    zero(CBW_BUFFER, 31);
    *(uint32_t *)(CBW_BUFFER + 0) = 0x43425355;
    *(uint32_t *)(CBW_BUFFER + 4) = ++usb_tag;
    *(uint32_t *)(CBW_BUFFER + 8) = length;
    CBW_BUFFER[12] = 0x80;
    CBW_BUFFER[14] = (uint8_t)command_size;
    copy(CBW_BUFFER + 15, command, command_size);
    if (!bulk(0, CBW_BUFFER, 31)) return -1;
    if (length && !bulk(1, TRANSFER_BUFFER, length)) return -1;
    if (!bulk(1, CSW_BUFFER, 13)) return -1;
    if (*(uint32_t *)CSW_BUFFER != 0x53425355 || *(uint32_t *)(CSW_BUFFER + 4) != usb_tag ||
        *(uint32_t *)(CSW_BUFFER + 8) > length || CSW_BUFFER[12] > 1) {
        transport_dead = 1;
        return -1;
    }
    if (CSW_BUFFER[12] == 1) return 0;
    if (*(uint32_t *)(CSW_BUFFER + 8) != 0) { transport_dead = 1; return -1; }
    return 1;
}

static int storage_ready(void)
{
    BOOT_REPORT[1] = 6;
    const uint8_t ready[6] = {0};
    const uint8_t sense[6] = {3, 0, 0, 0, 18, 0};
    const uint8_t capacity[10] = {0x25};
    for (uint32_t attempt = 0; attempt < 30; ++attempt) {
        int result = scsi(ready, 6, 0);
        if (result < 0) return 0;
        if (result == 1) {
            if (scsi(capacity, 10, 8) != 1 || be32(TRANSFER_BUFFER + 4) != 512) return 0;
            last_block = be32(TRANSFER_BUFFER);
            return last_block >= 4095 && last_block != 0xFFFFFFFFu;
        }
        if (scsi(sense, 6, 18) < 0) return 0;
        delay(100);
    }
    return 0;
}

static int usb_read(uint32_t lba, uint32_t blocks)
{
    uint8_t command[10] = {0x28};
    if (!blocks || blocks > BLOCKS_PER_READ || lba > last_block || blocks - 1 > last_block - lba) return 0;
    put_be32(command + 2, lba);
    command[7] = (uint8_t)(blocks >> 8);
    command[8] = (uint8_t)blocks;
    return scsi(command, 10, blocks * BLOCK_SIZE) == 1;
}

static int ata_wait(uint8_t mask, uint8_t wanted)
{
    uint32_t start = tsc();
    do {
        uint8_t status = in8(ata_port + 7);
        if (status == 0 || status == 255) return 0;
        if (!(status & 0x80)) {
            if (status & 0x21) return 0;
            if ((status & mask) == wanted) return 1;
        }
    } while ((uint32_t)(tsc() - start) < ticks_per_ms * 1000u);
    return 0;
}

static int ata_read(uint32_t lba, uint32_t blocks)
{
    if (!blocks || blocks > BLOCKS_PER_READ || lba > 0x0FFFFFFFu ||
        blocks - 1 > 0x0FFFFFFFu - lba) return 0;
    out8(ata_port + 0x206, 2); /* nIEN: polling only, no IRQ14/15 before the IDT. */
    out8(ata_port + 6, (uint8_t)(0xE0 | (ata_device << 4) | (lba >> 24)));
    for (uint32_t i = 0; i < 4; ++i) (void)in8(ata_port + 0x206);
    if (!ata_wait(0xC0, 0x40)) return 0;
    out8(ata_port + 2, (uint8_t)blocks);
    out8(ata_port + 3, (uint8_t)lba);
    out8(ata_port + 4, (uint8_t)(lba >> 8));
    out8(ata_port + 5, (uint8_t)(lba >> 16));
    out8(ata_port + 7, 0x20);
    for (uint32_t sector = 0; sector < blocks; ++sector) {
        for (uint32_t i = 0; i < 4; ++i) (void)in8(ata_port + 0x206);
        if (!ata_wait(8, 8)) return 0;
        uint16_t *destination = (uint16_t *)(TRANSFER_BUFFER + sector * 512);
        for (uint32_t word = 0; word < 256; ++word) {
            uint16_t value;
            __asm__ volatile ("inw %w1, %0" : "=a"(value) : "Nd"(ata_port));
            destination[word] = value;
        }
    }
    for (uint32_t i = 0; i < 4; ++i) (void)in8(ata_port + 0x206);
    return ata_wait(0x88, 0);
}

#ifndef KERNEL_STORAGE
static int read_blocks(uint32_t lba, uint32_t blocks)
{
    return backend == 1 ? usb_read(lba, blocks) : ata_read(lba, blocks);
}

static int matches_boot_image(void)
{
    if (backend == 1) BOOT_REPORT[1] = 7;
    /* BIOS DL is not a native-device ID. Match immutable code and the build manifest. */
    if (!read_blocks(0, 1) || !equal(TRANSFER_BUFFER, (const void *)0x7C00, 16) ||
        TRANSFER_BUFFER[510] != 0x55 || TRANSFER_BUFFER[511] != 0xAA) return 0;
    return read_blocks(5, 1) && equal(TRANSFER_BUFFER, MANIFEST, sizeof(*MANIFEST));
}
#endif

static int find_usb_disk(void)
{
#ifndef KERNEL_STORAGE
    backend = 1;
#endif
    for (uint32_t device = 0; device < 0x1000000; device += 0x100) {
#ifdef KERNEL_STORAGE
        if (!storage_usb_candidate(device)) continue;
#endif
        if ((pci_read(device, 0) & 0xFFFFu) == 0xFFFFu ||
            (pci_read(device, 8) >> 8) != 0x0C0320) continue;
        screen("Native USB: EHCI", device);
        int ports = start_controller(device);
        for (uint32_t port = 0; port < (uint32_t)ports; ++port) {
            uint32_t offset = 0x44 + port * 4;
            uint32_t value = ehci[offset / 4];
            if (!(value & 1) || (value & (1u << 13))) continue;
            BOOT_REPORT[1] = 3;
            BOOT_REPORT[3] = port + 1;
            screen("Native USB: resetting port", port + 1);
            ehci[offset / 4] = (value & ~(0x2Au | 4u)) | (1u << 8);
            delay(50);
            ehci[offset / 4] = ehci[offset / 4] & ~(0x2Au | (1u << 8));
            if (!wait_register(offset, 1u << 8, 0)) fail("USB port reset timeout", port + 1);
            delay(20);
            value = ehci[offset / 4];
            if ((value & 5) == 5 && !(value & (1u << 13))) {
                if (enumerate_storage() && storage_ready()) {
#ifdef KERNEL_STORAGE
                    storage_handoff_ports((uint32_t)ports, port);
                    return 1; /* Runtime discovery is not restricted to the boot image. */
#else
                    if (matches_boot_image()) return 1;
#endif
                }
            }
            /* Only one port is enumerated at address 1 at a time. */
            ehci[offset / 4] = ehci[offset / 4] & ~(0x2Au | 4u);
        }
#ifdef KERNEL_STORAGE
        if (ports > 0) storage_handoff_ports((uint32_t)ports, 0xFFFFFFFFu);
#endif
        stop_controller();
        ehci = NULL;
    }
    return 0;
}

#ifndef KERNEL_STORAGE
static int find_ide_disk(void)
{
    backend = 2;
    /* Only PCI IDE channels explicitly in compatibility mode, not AHCI/native IDE. */
    for (uint32_t device = 0; device < 0x1000000; device += 0x100) {
        if ((pci_read(device, 0) & 0xFFFFu) == 0xFFFFu) continue;
        uint32_t class_code = pci_read(device, 8);
        if ((class_code >> 16) != 0x0101 || !(pci_read(device, 4) & 1)) continue;
        uint32_t mode = (class_code >> 8) & 255;
        for (uint32_t channel = 0; channel < 2; ++channel) {
            if (mode & (channel ? 4u : 1u)) continue;
            ata_port = channel ? 0x170 : 0x1F0;
            for (ata_device = 0; ata_device < 2; ++ata_device) {
                if (matches_boot_image()) return 1;
            }
        }
    }
    return 0;
}

static void load_image(uint32_t lba, uint32_t sectors, uint32_t destination, uint32_t crc)
{
    uint32_t loaded = 0;
    while (loaded < sectors) {
        uint32_t count = sectors - loaded;
        if (count > BLOCKS_PER_READ) count = BLOCKS_PER_READ;
        BOOT_REPORT[6] = lba + loaded;
        screen(backend == 1 ? "USB loading LBA" : "IDE loading LBA", lba + loaded);
        if (!read_blocks(lba + loaded, count)) fail("Native disk read failed at LBA", lba + loaded);
        copy((void *)(destination + loaded * 512), TRANSFER_BUFFER, count * 512);
        loaded += count;
    }
    if (boot_crc32((const void *)destination, sectors * 512) != crc) fail("Kernel payload CRC mismatch", destination);
}

__attribute__((section(".text.bootstrap_load_images"), used))
void bootstrap_load_images(void)
{
    zero(__boot_bss_start, (uint32_t)(__boot_bss_end - __boot_bss_start));
    zero((void *)BOOT_REPORT, 32);
    BOOT_REPORT[0] = 0x42535452;
    BOOT_REPORT[1] = 1;
    zero((void *)DMA_BASE, 0x7000);
    if (!boot_manifest_valid(MANIFEST)) fail("Invalid bootstrap manifest", MANIFEST->version);
    screen("Starting native storage loader", 0);
    calibrate_timer();
    if (!find_usb_disk() && !find_ide_disk()) fail("No matching USB2/IDE boot image; USB stage", BOOT_REPORT[1]);
    BOOT_REPORT[7] = backend;
    load_image(MANIFEST->high_lba, MANIFEST->high_sectors, 0x34000000, MANIFEST->high_crc);
    load_image(MANIFEST->interrupts_lba, MANIFEST->interrupts_sectors, 0x32000000, MANIFEST->interrupts_crc);
    stop_controller();
    BOOT_REPORT[1] = 8;
    screen("Native kernel loading complete", backend);
}
#endif

#endif
