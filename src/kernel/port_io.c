/* port_io.c — externally-linkable wrappers around the inlined port-I/O
 * helpers in io.h. Modules (.kmd) link against these; the kernel itself
 * continues to use the `static inline` versions in io.h for speed.
 *
 * Names collide with io.h's inline definitions — but `static inline`
 * does not export a symbol, so there's no link-time conflict. Any
 * caller that includes io.h gets the inline; any caller (e.g. module
 * code that only sees an `extern uint8_t inb(uint16_t)` declaration)
 * links to these.
 */

#include "types.h"

uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void outl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}
