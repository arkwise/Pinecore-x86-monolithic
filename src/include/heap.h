#ifndef HEAP_H
#define HEAP_H

#include "types.h"

/* Simple kernel heap -- linked-list allocator
 * Good enough for kernel data structures.
 */

void  heap_init(uint32_t start, uint32_t size);
void *kmalloc(size_t size);
void  kfree(void *ptr);

#endif
