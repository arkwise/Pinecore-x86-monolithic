#ifndef IDT_H
#define IDT_H

#include "types.h"

/* IDT gate types (386-bible p.101) */
#define IDT_GATE_INT   0x8E  /* P=1, DPL=0, 32-bit interrupt gate */
#define IDT_GATE_INT3  0xEE  /* P=1, DPL=3, 32-bit interrupt gate (Ring 3 callable) */
#define IDT_GATE_TRAP  0x8F  /* P=1, DPL=0, 32-bit trap gate */

/* Stack frame pushed by isr_stubs.asm (matches CPU + our stub additions).
 * For Ring 3 PM interrupts, esp/ss are present (privilege change).
 * For V86 interrupts, use struct v86_frame which extends this. */
struct isr_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; /* pusha */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;  /* pushed by CPU */
    uint32_t esp, ss;          /* pushed by CPU on privilege change */
};

/* Install an ISR for a given interrupt number */
typedef void (*isr_handler_t)(uint32_t int_no, uint32_t err_code,
                              uint32_t eip, uint32_t cs, uint32_t eflags);

void idt_init(void);
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags);
void isr_register(uint8_t num, isr_handler_t handler);

#endif
