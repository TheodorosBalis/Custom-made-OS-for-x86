#ifndef KERNEL_API_H
#define KERNEL_API_H

#include "Include/KernelTypes.h"

#define KERNEL_STDCALL __attribute__((stdcall))

#define KERNEL_API_ADDRESS 0x80002200u
#define KERNEL_API_MAGIC   0x4B415049u
#define KERNEL_API_VERSION 1u
#define KERNEL_IDLE_CALLBACK_ADDRESS 0x80002360u

#define PROCESS_TYPE_PROTECTED32 0u
#define PROCESS_TYPE_V86         1u

#define PAGE_SIZE             0x1000u
#define PAGE_PRESENT          0x001u
#define PAGE_RW               0x002u
#define PAGE_USER             0x004u
#define PAGE_OWNED            0x200u
#define PAGE_PRESENT_USER     (PAGE_PRESENT | PAGE_USER)
#define PAGE_PRESENT_RW_USER  (PAGE_PRESENT | PAGE_RW | PAGE_USER)

#define DESCRIPTOR_DPL0 0x00u
#define DESCRIPTOR_DPL1 0x20u
#define DESCRIPTOR_DPL2 0x40u
#define DESCRIPTOR_DPL3 0x60u

typedef void *kernel_process_t;
typedef uint32_t kernel_page_directory_t;

struct kernel_api {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;

    uint32_t (*map_kernel_vga)(void);
    kernel_process_t (KERNEL_STDCALL *process_create)(uint32_t process_type);
    kernel_process_t (*process_create_protected32)(void);
    kernel_process_t (*v86_process_create)(void);
    void (KERNEL_STDCALL *process_destroy)(kernel_process_t process);
    void (*process_slot_allocator_init)(void);
    uint32_t (KERNEL_STDCALL *process_slot_alloc)(uint32_t *slot_index);
    void (KERNEL_STDCALL *process_slot_free)(uint32_t slot_index);
    uint32_t (KERNEL_STDCALL *process_prepare_test_task)(kernel_process_t process,
                                                         const void *source,
                                                         uint32_t size);
    uint32_t (KERNEL_STDCALL *process_install_syscall_gateway)(kernel_process_t process);
    void (*test_heap)(void);
    uint32_t (*enable_systemcall)(void);
    uint32_t (*enable_vme)(void);
    void (*pmm_init)(void);
    void (KERNEL_STDCALL *pmm_mark_range_free)(uint32_t start, uint32_t end);
    void (KERNEL_STDCALL *pmm_mark_range_used)(uint32_t start, uint32_t end);
    uint32_t (*pmm_alloc_page)(void);
    void (KERNEL_STDCALL *pmm_free_page)(uint32_t physical_page);
    uint32_t (KERNEL_STDCALL *map_page)(uint32_t virtual_address,
                                        uint32_t physical_address,
                                        uint32_t flags);
    void (*heap_init)(void);
    uint32_t (*heap_grow_one_page)(void);
    uint32_t (KERNEL_STDCALL *heap_grow_pages)(uint32_t count);
    void *(KERNEL_STDCALL *heap_alloc)(uint32_t size);
    void (KERNEL_STDCALL *heap_free)(void *allocation);
    void (*heap_coalesce)(void);
    uint32_t (*heap_validate)(void);
    uint32_t (*alloc_page_table)(void);
    void (KERNEL_STDCALL *free_page_table)(uint32_t page_table);
    kernel_page_directory_t (*create_page_directory)(void);
    void (KERNEL_STDCALL *destroy_page_directory)(kernel_page_directory_t page_directory);
    uint32_t (KERNEL_STDCALL *map_user_page)(kernel_page_directory_t page_directory,
                                             uint32_t virtual_address,
                                             uint32_t physical_address,
                                             uint32_t flags);
    uint32_t (KERNEL_STDCALL *unmap_page)(kernel_page_directory_t page_directory,
                                          uint32_t virtual_address);
    uint32_t (KERNEL_STDCALL *get_physical_address)(kernel_page_directory_t page_directory,
                                                    uint32_t virtual_address);
    uint32_t (KERNEL_STDCALL *process_memory_init_heap_page)(uint32_t physical_page);
    kernel_process_t (*process_memory_create)(void);
    kernel_process_t (*v86_process_memory_create)(void);
    uint32_t (KERNEL_STDCALL *v86_map_low_memory)(kernel_process_t process);
    void (*gdt_allocator_init)(void);
    uint32_t (*gdt_alloc_selector)(void);
    void (KERNEL_STDCALL *gdt_write_raw_descriptor)(uint32_t selector,
                                                    uint32_t low,
                                                    uint32_t high);
    void (KERNEL_STDCALL *gdt_free_selector)(uint32_t selector);
    void (KERNEL_STDCALL *gdt_write_segment_descriptor)(uint32_t selector,
                                                        uint32_t base,
                                                        uint32_t limit,
                                                        uint32_t access);
    uint32_t (KERNEL_STDCALL *gdt_create_code_descriptor)(uint32_t base,
                                                          uint32_t limit,
                                                          uint32_t dpl);
    uint32_t (KERNEL_STDCALL *gdt_create_data_descriptor)(uint32_t base,
                                                          uint32_t limit,
                                                          uint32_t dpl);
    uint32_t (KERNEL_STDCALL *gdt_create_ldt_descriptor)(uint32_t base,
                                                         uint32_t limit);
    uint32_t (KERNEL_STDCALL *gdt_create_tss_descriptor)(uint32_t base);
    uint32_t (KERNEL_STDCALL *gdt_create_tss_descriptor_with_limit)(uint32_t base,
                                                                    uint32_t limit);
    uint32_t (KERNEL_STDCALL *ldt_alloc_selector)(void *bitmap, uint32_t entry_count);
    void (KERNEL_STDCALL *ldt_write_raw_descriptor)(void *ldt,
                                                    uint32_t selector,
                                                    uint32_t low,
                                                    uint32_t high);
    void (KERNEL_STDCALL *ldt_write_segment_descriptor)(void *ldt,
                                                        uint32_t selector,
                                                        uint32_t base,
                                                        uint32_t limit,
                                                        uint32_t access);
    uint32_t (KERNEL_STDCALL *process_create_random_ldt)(kernel_process_t process,
                                                         uint32_t linear_base);
    uint32_t (KERNEL_STDCALL *process_create_tss)(kernel_process_t process);
    uint32_t (KERNEL_STDCALL *v86_process_create_tss)(kernel_process_t process);
    uint32_t (*create_kernel_tss)(void);
    uint32_t (KERNEL_STDCALL *process_heap_grow)(kernel_process_t process);
    void (KERNEL_STDCALL *process_memory_destroy)(kernel_process_t process);
    void (KERNEL_STDCALL *ptext)(const char *text);
    void (*scheduler_init)(void);
    uint32_t (KERNEL_STDCALL *scheduler_add_process)(kernel_process_t process);
    uint32_t (KERNEL_STDCALL *scheduler_remove_process)(kernel_process_t process);
    void (*process_table_init)(void);
    uint32_t (KERNEL_STDCALL *process_register)(kernel_process_t process);
    void (KERNEL_STDCALL *process_unregister)(kernel_process_t process);
    kernel_process_t (KERNEL_STDCALL *process_get_by_id)(uint32_t table_index);
};

#define KERNEL_API ((const struct kernel_api *)KERNEL_API_ADDRESS)

#endif
