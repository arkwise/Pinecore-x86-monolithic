#ifndef PMM_H
#define PMM_H

#include "types.h"

/* Physical Memory Manager -- bitmap-based page allocator
 *
 * Tracks which 4KB physical pages are free/used.
 * (386-bible p.98: page size is 4096 bytes)
 */

#define PAGE_SIZE 4096

void     pmm_init(uint32_t mem_size_kb);
uint32_t pmm_alloc_page(void);
void     pmm_free_page(uint32_t phys_addr);
void     pmm_mark_used(uint32_t phys_addr);
void     pmm_mark_region_used(uint32_t base, uint32_t length);
void     pmm_mark_region_free(uint32_t base, uint32_t length);
uint32_t pmm_get_free_count(void);

#endif
