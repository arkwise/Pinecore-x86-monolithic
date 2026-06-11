/* vmm.c -- Virtual Memory Manager
 *
 * Two-level paging: page directory (1024 entries) -> page tables (1024 entries each)
 * Each entry maps a 4KB page.
 * (386-bible p.98-101)
 *
 * Linear address breakdown:
 *   [31:22] = page directory index (10 bits)
 *   [21:12] = page table index (10 bits)
 *   [11:0]  = offset within page (12 bits)
 */

#include "types.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "v86.h"

/* Page directory -- 1024 entries, must be 4KB aligned */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

/* Pre-allocated page tables identity-mapping the first 32MB (kernel + PMM range)
 * so we don't need the heap to set up paging, and so vmm_map_page can safely
 * dereference a freshly-allocated page table by its physical address — pmm_alloc_page
 * may return any frame in [0, 32MB), and we read/write the new PT through that
 * linear==phys window. PTE_USER on each page lets Ring 3 read identity-mapped
 * low memory (IVT, V86 segs, PSP); per-page protection still goes through PTE.
 *
 * Eight tables × 1024 entries × 4KB/entry = 32 MB. Matches QEMU `-m 32`. */
#define KERNEL_PT_COUNT 8
static uint32_t kernel_page_tables[KERNEL_PT_COUNT][1024] __attribute__((aligned(4096)));

extern uint32_t _kernel_end;  /* from linker script */

void vmm_init(void) {
    uint32_t i, t;

    /* Zero out page directory */
    for (i = 0; i < 1024; i++)
        page_directory[i] = 0;

    /* Identity-map all 32MB. Each kernel page table covers one 4MB region. */
    for (t = 0; t < KERNEL_PT_COUNT; t++) {
        for (i = 0; i < 1024; i++) {
            uint32_t addr = (t * 4 * 1024 * 1024) + (i * PAGE_SIZE);
            kernel_page_tables[t][i] = addr | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        page_directory[t] = (uint32_t)kernel_page_tables[t] |
                            PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    /* Load page directory into CR3 */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory));

    /* Enable paging: set PG bit (bit 31) in CR0 */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    serial_puts("VMM: paging enabled, first 32MB identity-mapped\n");
}

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;
    uint32_t *page_table;

    /* Check if page table exists for this directory entry */
    if (!(page_directory[dir_idx] & PTE_PRESENT)) {
        /* Allocate a new page table */
        uint32_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return;

        /* Zero the new page table */
        page_table = (uint32_t *)pt_phys;
        uint32_t j;
        for (j = 0; j < 1024; j++)
            page_table[j] = 0;

        /* PDE must permit user access for Ring 3 (DPMI clients); per-page
         * protection is enforced by the PTE's U bit. P|W|U here means
         * "delegate to PTE"; PDE U=0 would block user access regardless. */
        page_directory[dir_idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

        /* Mirror the new PDE into every V86 task's private page directory
         * so DPMI / kernel dynamic mappings remain visible no matter which
         * task's CR3 is currently loaded. */
        v86_propagate_pde(dir_idx, page_directory[dir_idx]);
    }

    page_table = (uint32_t *)(page_directory[dir_idx] & 0xFFFFF000);
    page_table[table_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF);

    /* Invalidate TLB entry for this virtual address */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_unmap_page(uint32_t virt) {
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;
    uint32_t *page_table;

    if (!(page_directory[dir_idx] & PTE_PRESENT))
        return;

    page_table = (uint32_t *)(page_directory[dir_idx] & 0xFFFFF000);
    page_table[table_idx] = 0;

    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

uint32_t vmm_get_physical(uint32_t virt) {
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;
    uint32_t *page_table;

    if (!(page_directory[dir_idx] & PTE_PRESENT))
        return 0;

    page_table = (uint32_t *)(page_directory[dir_idx] & 0xFFFFF000);
    if (!(page_table[table_idx] & PTE_PRESENT))
        return 0;

    return (page_table[table_idx] & 0xFFFFF000) | (virt & 0xFFF);
}

uint32_t vmm_kernel_pd_phys(void) {
    return (uint32_t)page_directory;
}

const uint32_t *vmm_kernel_pt0(void) {
    return kernel_page_tables[0];
}

void vmm_load_cr3(uint32_t pd_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}
