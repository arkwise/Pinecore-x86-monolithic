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
#include "io.h"

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

/* Called from kernel_main early in boot. Seeds the canary with the
 * best-available entropy WITHOUT using RDTSC — the original
 * implementation #UD'd on 486-class CPUs (Vortex86SX) that don't
 * implement RDTSC, triple-faulting the kernel before vga_init could
 * run. Replacement uses CMOS RTC seconds + the boot-time PIC ISR
 * snapshot, both 386-safe.
 *
 * Lower entropy than RDTSC (seconds changes once per second; PIC ISR
 * is mostly zero at boot), but enough to defeat the trivial
 * "predict and overwrite with itself" bypass. Acceptable trade-off:
 * boots on every i386-compatible CPU vs hardens against attackers
 * with a specific timing oracle. */
void stack_chk_init(void) {
    uint8_t  rtc_sec;
    uint8_t  pic_isr_m, pic_isr_s;

    /* CMOS RTC seconds: index reg 0x00, preserve NMI-disable bit. */
    outb(0x70, (inb(0x70) & 0x80) | 0x00);
    rtc_sec = inb(0x71);

    /* PIC ISR snapshot — read via OCW3, port 0x0A. */
    outb(0x20, 0x0B); pic_isr_m = inb(0x20);
    outb(0xA0, 0x0B); pic_isr_s = inb(0xA0);

    {
        uint32_t g = ((uint32_t)rtc_sec   << 24) |
                     ((uint32_t)pic_isr_m << 16) |
                     ((uint32_t)pic_isr_s <<  8) |
                     0xCEu;
        g ^= 0xDEADC0DEu;
        g = (g & 0xFFFFFF00u);            /* guarantee low byte = 0 */
        if (g == 0) g = 0xCAFE0100u;      /* never leave all-zero */
        __stack_chk_guard = g;
    }

    serial_puts("stack-canary: guard = ");
    serial_puthex(__stack_chk_guard);
    serial_puts("\n");
}
