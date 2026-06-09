/* heap.c -- Simple kernel heap allocator
 *
 * Linked-list first-fit allocator.
 * Each block has a header with size and free/used flag.
 * Adjacent free blocks are merged on free.
 */

#include "types.h"
#include "heap.h"
#include "serial.h"

struct block_header {
    uint32_t size;           /* size of data area (not including header) */
    uint32_t free;           /* 1 = free, 0 = used */
    struct block_header *next;
};

#define HEADER_SIZE sizeof(struct block_header)

static struct block_header *heap_start = NULL;
static uint32_t heap_end_addr = 0;

void heap_init(uint32_t start, uint32_t size) {
    /* Align start to 4 bytes */
    start = (start + 3) & ~3;
    size  = size - (size % 4);

    heap_start = (struct block_header *)start;
    heap_start->size = size - HEADER_SIZE;
    heap_start->free = 1;
    heap_start->next = NULL;
    heap_end_addr = start + size;

    serial_puts("HEAP: ");
    serial_puthex(size);
    serial_puts(" bytes at ");
    serial_puthex(start);
    serial_puts("\n");
}

void *kmalloc(size_t size) {
    struct block_header *curr;

    if (!size) return NULL;

    /* Align to 4 bytes */
    size = (size + 3) & ~3;

    curr = heap_start;
    while (curr) {
        if (curr->free && curr->size >= size) {
            /* Split block if there's enough room for another block */
            if (curr->size >= size + HEADER_SIZE + 16) {
                struct block_header *new_block;
                new_block = (struct block_header *)((uint8_t *)curr + HEADER_SIZE + size);
                new_block->size = curr->size - size - HEADER_SIZE;
                new_block->free = 1;
                new_block->next = curr->next;

                curr->size = size;
                curr->next = new_block;
            }
            curr->free = 0;
            return (void *)((uint8_t *)curr + HEADER_SIZE);
        }
        curr = curr->next;
    }

    serial_puts("HEAP: alloc failed, size=");
    serial_puthex(size);
    serial_puts("\n");
    return NULL;
}

void kfree(void *ptr) {
    struct block_header *header;
    struct block_header *curr;

    if (!ptr) return;

    header = (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);
    header->free = 1;

    /* Merge adjacent free blocks */
    curr = heap_start;
    while (curr) {
        if (curr->free && curr->next && curr->next->free) {
            curr->size += HEADER_SIZE + curr->next->size;
            curr->next = curr->next->next;
            continue;  /* check again in case of 3+ consecutive free blocks */
        }
        curr = curr->next;
    }
}
