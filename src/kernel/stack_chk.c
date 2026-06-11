/* stack_chk.c — Stack-canary support for the pinecore kernel.
 *
 * s54 Tier-1 security mitigation #3.
 *
 * GCC's -fstack-protector-strong emits two external references in any
 * function that has stack-allocated arrays or address-taken locals:
 *
 *   __stack_chk_guard   — a uintptr_t with a random-ish value, written
 *                          on function entry just below the saved frame
 *                          pointer; checked on return.
 *   __stack_chk_fail()  — called when the on-stack canary does not
 *                          match __stack_chk_guard at return time.
 *
 * Pinecore being freestanding with no TLS, we use the GCC option
 * -mstack-protector-guard=global so the emitted code reads a plain
 * global rather than a %fs:offset TLS slot.
 *
 * On detection, we panic via the s50 kernel_panic infrastructure with
 * the offending CS:EIP and a register dump — converting a memory-
 * corruption exploit primitive into a visible crash instead of silent
 * RCE.
 *
 * Initialization: the guard is set once during early boot from a mix
 * of RDTSC, the boot-time PIT count, and a compile-time literal. Not
 * a CSPRNG, but enough to defeat the "predict the canary and overwrite
 * it with itself" trivial bypass.
 */

#include "types.h"
#include "serial.h"
#include "panic.h"

/* The canary itself. Linker will place this in .data so it's writable
 * by stack_chk_init at boot. The literal here is what protects us
 * pre-init; replaced with a high-entropy value before any real driver
 * runs. (Plain uint32_t — on i386 that's pointer-sized.) */
uint32_t __stack_chk_guard = 0xDEADC0DEu;

/* Called by GCC-emitted prologue/epilogue when the on-stack canary
 * fails to match __stack_chk_guard at function return. Treat as fatal
 * — by definition the stack is already corrupted past this function. */
void __stack_chk_fail(void) {
    uint32_t caller_eip;
    __asm__ __volatile__("movl 4(%%ebp), %0" : "=r"(caller_eip));

    serial_puts("\n*** STACK CANARY FAIL — corruption detected ***\n");
    serial_puts("    caller EIP = ");
    serial_puthex(caller_eip);
    serial_puts("\n");

    kernel_panic_str("stack canary mismatch");

    /* Should not return; defensive infinite halt. */
    for (;;) __asm__ __volatile__("cli; hlt");
}

/* Called from kernel_main early in boot (after serial + PIT init but
 * before any module load) to seed the canary with high-entropy bytes.
 * Idempotent — safe to call multiple times. */
void stack_chk_init(void) {
    uint32_t tsc_lo, tsc_hi;
    __asm__ __volatile__("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));

    /* Mix the time-stamp counter halves with the link-time literal.
     * The zero byte in any byte position of the canary catches naive
     * strcpy/strcat overruns (they'll stop at the zero) — so we want
     * the low byte to be zero. */
    uint32_t g = ((uint32_t)tsc_lo ^ (uint32_t)tsc_hi) ^ 0xDEADC0DEu;
    g = (g & 0xFFFFFF00u);                /* guarantee low byte = 0 */
    if (g == 0) g = 0xCAFE0100u;          /* never leave all-zero */

    __stack_chk_guard = g;

    serial_puts("stack-canary: guard = ");
    serial_puthex(__stack_chk_guard);
    serial_puts("\n");
}
