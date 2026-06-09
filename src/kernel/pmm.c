/* pmm.c -- Physical Memory Manager
 *
 * Bitmap allocator: 1 bit per 4KB page.
 * Bit 0 = free, bit 1 = used.
 * (386-bible p.98: pages are 4096 bytes, aligned to 4KB boundaries)
 */

#include "types.h"
#include "pmm.h"
#include "serial.h"

/* Bitmap stored in BSS -- 128KB supports up to 4GB RAM */
/* (4GB / 4KB = 1M pages, 1M bits = 128KB) */
#define BITMAP_SIZE (128 * 1024)
static uint8_t bitmap[BITMAP_SIZE];

static uint32_t total_pages;
static uint32_t used_pages;

static void set_bit(uint32_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static void clear_bit(uint32_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static int test_bit(uint32_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

void pmm_init(uint32_t mem_size_kb) {
    uint32_t i;

    total_pages = mem_size_kb / 4;  /* 4KB per page */
    used_pages = total_pages;

    /* Mark everything as used initially */
    for (i = 0; i < BITMAP_SIZE; i++)
        bitmap[i] = 0xFF;

    serial_puts("PMM: ");
    serial_puthex(total_pages);
    serial_puts(" pages (");
    serial_puthex(mem_size_kb);
    serial_puts(" KB)\n");
}

void pmm_mark_used(uint32_t phys_addr) {
    uint32_t page = phys_addr / PAGE_SIZE;
    if (!test_bit(page)) {
        set_bit(page);
        used_pages++;
    }
}

void pmm_free_page(uint32_t phys_addr) {
    uint32_t page = phys_addr / PAGE_SIZE;
    if (test_bit(page)) {
        clear_bit(page);
        used_pages--;
    }
}

void pmm_mark_region_used(uint32_t base, uint32_t length) {
    uint32_t page = base / PAGE_SIZE;
    uint32_t end_page = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (; page < end_page; page++) {
        if (!test_bit(page)) {
            set_bit(page);
            used_pages++;
        }
    }
}

void pmm_mark_region_free(uint32_t base, uint32_t length) {
    uint32_t page = base / PAGE_SIZE;
    uint32_t end_page = (base + length) / PAGE_SIZE;
    for (; page < end_page; page++) {
        if (test_bit(page)) {
            clear_bit(page);
            used_pages--;
        }
    }
}

uint32_t pmm_alloc_page(void) {
    uint32_t i, j;

    for (i = 0; i < total_pages / 8; i++) {
        if (bitmap[i] == 0xFF)
            continue;  /* all 8 pages used */
        for (j = 0; j < 8; j++) {
            if (!(bitmap[i] & (1 << j))) {
                uint32_t page = i * 8 + j;
                set_bit(page);
                used_pages++;
                return page * PAGE_SIZE;
            }
        }
    }

    serial_puts("PMM: OUT OF MEMORY\n");
    return 0;
}

uint32_t pmm_get_free_count(void) {
    return total_pages - used_pages;
}
