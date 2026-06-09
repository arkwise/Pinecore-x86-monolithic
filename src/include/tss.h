#ifndef TSS_H
#define TSS_H

#include "types.h"

/* Task State Segment (386-bible p.130-131)
 *
 * We use a single TSS for the kernel. When V86 tasks trap to Ring 0,
 * the CPU loads SS0:ESP0 from this TSS.
 */

struct tss {
    uint32_t prev_tss;
    uint32_t esp0;       /* Ring 0 stack pointer */
    uint32_t ss0;        /* Ring 0 stack segment (0x10 = kernel data) */
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

void tss_init(void);
void tss_set_stack(uint32_t esp0);

#endif
