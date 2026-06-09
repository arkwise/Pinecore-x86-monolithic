/* string.c -- Freestanding libc: memcpy, memset, strlen
 *
 * GCC requires these even with -ffreestanding
 */

#include "types.h"

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memset(void *dest, int c, size_t n) {
    uint8_t *d = dest;
    while (n--)
        *d++ = (uint8_t)c;
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    while (n--) {
        if (*p != *q)
            return *p - *q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++)
        len++;
    return len;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++; b++;
    }
    return *(unsigned char *)a - *(unsigned char *)b;
}
