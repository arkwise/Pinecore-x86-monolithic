#ifndef VMM_H
#define VMM_H

#include "types.h"

/* Virtual Memory Manager -- page directory and page tables
 *
 * Two-level paging: 10-bit dir index, 10-bit table index, 12-bit offset
 * (386-bible p.98-101)
 */

/* Page directory/table entry flags */
#define PTE_PRESENT   0x001
#define PTE_WRITABLE  0x002
#define PTE_USER      0x004
#define PTE_PWT       0x008  /* write-through */
#define PTE_PCD       0x010  /* cache disable */
#define PTE_ACCESSED  0x020
#define PTE_DIRTY     0x040
#define PTE_4MB       0x080  /* page size (in PDE only) */

void vmm_init(void);
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_unmap_page(uint32_t virt);
uint32_t vmm_get_physical(uint32_t virt);

#endif
