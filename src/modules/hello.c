/* hello.kmd — smallest possible Pinecore kernel module.
 * Proves the module loader pipeline works end-to-end.
 *
 * License: GPL-2.0 (matches the kernel; module author's choice).
 *
 * Build: i686-elf-gcc -c -fno-common -ffreestanding -Iinclude \
 *                     -nostdinc -m32 hello.c -o hello.kmd
 */

#include "module.h"

extern void serial_puts(const char *s);
extern void vga_puts(const char *s);

static int hello_init(void) {
    serial_puts("hello.kmd: alive — init ran successfully\n");
    vga_puts("  [OK] hello.kmd loaded via module loader\n");
    return 0;
}

static void hello_exit(void) {
    serial_puts("hello.kmd: exit\n");
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("Smoke-test kernel module");
MODULE_NAME("hello");
