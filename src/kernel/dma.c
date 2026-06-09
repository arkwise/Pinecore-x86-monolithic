/* dma.c — bus-master DMA allocator over a 256 KB identity-mapped region.
 *
 * See docs/research/54-usbcore-env-synthesis.md §3-§4 for the rationale
 * (region placement, sizing budget, API contract). Used by usbcore.kmd,
 * uhci.kmd, msc.kmd, future ehci/xhci/NIC.
 *
 * Allocator: bitmap with 16-byte granule. For each allocation we also
 * remember the run length (in granules) in a parallel size table so
 * dma_free knows how much to clear without a header in the data block
 * (data blocks must be device-accessible and not have hidden footers).
 *
 * Out-of-scope: per-CPU caches, free-list coalescing, defragmentation.
 * The allocator runs in interrupt context (uhci IRQ may alloc TDs);
 * keeps it lock-free by being non-preemptible during the bitmap walk.
 */

#include "types.h"
#include "pmm.h"
#include "dma.h"
#include "serial.h"

#define UNITS         (DMA_REGION_SIZE / DMA_GRANULE)   /* 16384 units */
#define BITMAP_BYTES  (UNITS / 8)                        /* 2048 bytes */

/* 1 bit per 16-byte unit. Bit set = in use. */
static uint8_t dma_bitmap[BITMAP_BYTES];

/* Size table: index = first unit of an allocation, value = run length
 * in units. 0 means "not the start of an allocation." Lets dma_free
 * recover the run length without a hidden header in the data. */
static uint16_t dma_run_units[UNITS];

static uint32_t dma_units_used;

static inline int  bit_get(uint32_t u)  { return dma_bitmap[u >> 3] & (1u << (u & 7)); }
static inline void bit_set(uint32_t u)  { dma_bitmap[u >> 3] |=  (1u << (u & 7)); }
static inline void bit_clr(uint32_t u)  { dma_bitmap[u >> 3] &= ~(1u << (u & 7)); }

/* Are [u, u+n) all free? */
static int range_free(uint32_t u, uint32_t n) {
    uint32_t i;
    if (u + n > UNITS) return 0;
    for (i = 0; i < n; i++)
        if (bit_get(u + i)) return 0;
    return 1;
}

static void range_mark_used(uint32_t u, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) bit_set(u + i);
}

static void range_mark_free(uint32_t u, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) bit_clr(u + i);
}

void dma_init(void) {
    uint32_t i;

    /* Reserve the region from PMM so pmm_alloc_page never hands it out
     * for kernel-heap or DPMI client zones. */
    pmm_mark_region_used(DMA_REGION_BASE, DMA_REGION_SIZE);

    /* Bitmap starts clear (all free). */
    for (i = 0; i < BITMAP_BYTES; i++) dma_bitmap[i] = 0;
    for (i = 0; i < UNITS; i++)        dma_run_units[i] = 0;
    dma_units_used = 0;

    serial_puts("DMA: ");
    serial_puthex(DMA_REGION_SIZE);
    serial_puts(" bytes at ");
    serial_puthex(DMA_REGION_BASE);
    serial_puts(" reserved + identity-mapped\n");
}

void *dma_alloc(uint32_t size, uint32_t align) {
    uint32_t units, align_units, u, i;
    uint32_t flags;

    if (size == 0) return 0;
    if (align == 0) align = DMA_GRANULE;
    /* Align must be a power of 2 and at least DMA_GRANULE. */
    if (align < DMA_GRANULE || (align & (align - 1))) return 0;

    units       = (size + DMA_GRANULE - 1) / DMA_GRANULE;
    align_units = align / DMA_GRANULE;
    if (units == 0 || units > UNITS) return 0;
    if (units > 0xFFFFu) return 0;     /* run length must fit in uint16 */

    /* Non-preemptible search — IRQ handlers can hit dma_alloc. */
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));

    for (u = 0; u <= UNITS - units; u += align_units) {
        if (range_free(u, units)) {
            range_mark_used(u, units);
            dma_run_units[u] = (uint16_t)units;
            dma_units_used  += units;
            __asm__ volatile("pushl %0; popfl" : : "r"(flags));
            return (void *)(DMA_REGION_BASE + u * DMA_GRANULE);
        }
    }

    __asm__ volatile("pushl %0; popfl" : : "r"(flags));
    (void)i;
    return 0;
}

void dma_free(void *p) {
    uint32_t addr = (uint32_t)p;
    uint32_t u, n;
    uint32_t flags;

    if (!p) return;
    if (addr < DMA_REGION_BASE || addr >= DMA_REGION_BASE + DMA_REGION_SIZE)
        return;                                     /* not from us */
    if ((addr - DMA_REGION_BASE) % DMA_GRANULE)
        return;                                     /* misaligned */

    u = (addr - DMA_REGION_BASE) / DMA_GRANULE;

    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    n = dma_run_units[u];
    if (n) {
        range_mark_free(u, n);
        dma_run_units[u] = 0;
        dma_units_used  -= n;
    }
    __asm__ volatile("pushl %0; popfl" : : "r"(flags));
}

uint32_t dma_virt_to_phys(void *p) {
    /* Region is identity-mapped — virt == phys. Out-of-region pointers
     * return 0 so callers can detect "this isn't a DMA buffer." */
    uint32_t addr = (uint32_t)p;
    if (addr < DMA_REGION_BASE || addr >= DMA_REGION_BASE + DMA_REGION_SIZE)
        return 0;
    return addr;
}

uint32_t dma_free_bytes(void) {
    return (UNITS - dma_units_used) * DMA_GRANULE;
}
