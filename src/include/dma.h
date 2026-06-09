#ifndef DMA_H
#define DMA_H

#include "types.h"

/* DMA region — identity-mapped physical memory for bus-master devices
 * (USB host controllers, NIC descriptor rings, audio buffers).
 *
 * Layout, per docs/research/54-usbcore-env-synthesis.md §3:
 *   Base:   0x00200000   (2 MB physical, well above kernel + V86 segments)
 *   Size:   256 KB
 *   Align:  4 KB (page-aligned, satisfies UHCI Frame List requirement)
 *
 * The region is reserved in PMM at boot and identity-mapped by VMM
 * (the existing 32 MB identity map already covers it). Inside the
 * region, dma_alloc serves bus-master allocations from a 16-byte-
 * granular bitmap allocator.
 *
 * Allocations are physically contiguous within the region — guaranteed
 * because we never grow the region or relocate blocks. virt == phys
 * everywhere in [DMA_REGION_BASE, DMA_REGION_BASE + DMA_REGION_SIZE).
 */

#define DMA_REGION_BASE   0x00200000U
#define DMA_REGION_SIZE   (256U * 1024U)
#define DMA_GRANULE       16U          /* alloc unit in bytes */

/* Reserve region in PMM, init bitmap. Call after pmm_init + vmm_init. */
void  dma_init(void);

/* Allocate `size` bytes, aligned to `align` (must be a power of 2 ≥ 16,
 * or 0 to default to DMA_GRANULE). Returns identity-mapped virtual
 * pointer (= physical address). NULL on out-of-region or bad align. */
void *dma_alloc(uint32_t size, uint32_t align);

/* Free a block previously returned by dma_alloc. */
void  dma_free(void *p);

/* Convert a dma_alloc'd virtual pointer to its physical address.
 * Trivial in our identity-mapped region; provided as an API so the
 * region implementation can move later without changing every caller. */
uint32_t dma_virt_to_phys(void *p);

/* Diagnostic: bytes still free in the region. */
uint32_t dma_free_bytes(void);

#endif
