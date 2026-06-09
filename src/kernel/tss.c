/* tss.c -- Task State Segment setup
 *
 * Single TSS for the kernel. ESP0/SS0 are updated when switching
 * to a V86 task so GPFs land on the correct Ring 0 stack.
 * (386-bible p.130-131, ch-10)
 */

#include "types.h"
#include "tss.h"
#include "serial.h"

static struct tss kernel_tss __attribute__((aligned(4)));

/* GDT entry for TSS — must be filled in by us.
 * The TSS descriptor is at GDT index 5 (selector 0x28).
 * boot.asm reserved a zero qword there. */

extern uint8_t gdt_start[];  /* from boot.asm */

void tss_init(void) {
    uint32_t base = (uint32_t)&kernel_tss;
    uint32_t limit = sizeof(struct tss) - 1;
    uint8_t *entry;
    uint32_t i;

    /* Zero out TSS */
    uint8_t *p = (uint8_t *)&kernel_tss;
    for (i = 0; i < sizeof(struct tss); i++)
        p[i] = 0;

    /* Set Ring 0 stack */
    kernel_tss.ss0 = 0x10;   /* kernel data segment */
    kernel_tss.esp0 = 0;     /* will be set per-task */

    /* I/O map base beyond TSS limit = no I/O bitmap = all ports trapped */
    kernel_tss.iomap_base = sizeof(struct tss);

    /* Fill GDT entry 5 (TSS descriptor at offset 5*8 = 40) */
    entry = gdt_start + 5 * 8;

    entry[0] = limit & 0xFF;
    entry[1] = (limit >> 8) & 0xFF;
    entry[2] = base & 0xFF;
    entry[3] = (base >> 8) & 0xFF;
    entry[4] = (base >> 16) & 0xFF;
    entry[5] = 0x89;  /* P=1, DPL=0, type=1001 (32-bit TSS, available) */
    entry[6] = ((limit >> 16) & 0x0F);  /* G=0, no flags needed */
    entry[7] = (base >> 24) & 0xFF;

    /* Load the kernel's GDT (which now has the TSS entry).
     * This is needed when booted from PINE.COM, which uses its own GDT.
     * GDTR switches from PINE.COM's GDT to the kernel's GDT at gdt_start. */
    {
        extern uint8_t gdt_ptr[];  /* from boot.asm: dw limit, dd base */
        __asm__ volatile ("lgdt (%0)" : : "r"(gdt_ptr));
    }

    /* Load task register */
    __asm__ volatile ("ltr %%ax" : : "a"(0x28));

    serial_puts("TSS: loaded at ");
    serial_puthex(base);
    serial_puts("\n");
}

void tss_set_stack(uint32_t esp0) {
    kernel_tss.esp0 = esp0;
}
