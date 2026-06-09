/* idt.c -- Interrupt Descriptor Table setup and dispatch
 *
 * 256 entries, interrupt gates, Ring 0
 * Exceptions 0-31, IRQs 32-47 (after PIC remap)
 * (386-bible p.101, ch-13)
 */

#include "types.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "serial.h"
#include "vga.h"
#include "v86.h"
#include "sched.h"
#include "dpmi.h"
#include "net.h"

/* IDT entry (386-bible p.101) */
struct idt_entry {
    uint16_t offset_lo;   /* offset bits 0-15 */
    uint16_t selector;    /* code segment selector */
    uint8_t  zero;        /* unused, must be 0 */
    uint8_t  flags;       /* type and attributes */
    uint16_t offset_hi;   /* offset bits 16-31 */
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

/* Change an existing IDT gate from DPL=0 to DPL=3 (for DPMI Ring 3 access) */
void idt_set_gate_dpl3(uint8_t num) {
    if (idt[num].flags)
        idt[num].flags |= 0x60;  /* set DPL bits 6:5 = 11 */
}

/* C handlers registered by drivers */
static isr_handler_t handlers[256];

/* Exception names for debug output */
static const char *exception_names[] = {
    "Division by zero",  "Debug",           "NMI",
    "Breakpoint",        "Overflow",        "Bound range",
    "Invalid opcode",    "Device not avail","Double fault",
    "Coproc seg overrun","Invalid TSS",     "Seg not present",
    "Stack seg fault",   "General protect", "Page fault",
    "Reserved",          "x87 FPU",         "Alignment check",
    "Machine check",     "SIMD FPU",        "Virtualization"
};

/* ASM stubs declared in isr_stubs.asm */
extern void isr_stub_0(void);  extern void isr_stub_1(void);
extern void isr_stub_2(void);  extern void isr_stub_3(void);
extern void isr_stub_4(void);  extern void isr_stub_5(void);
extern void isr_stub_6(void);  extern void isr_stub_7(void);
extern void isr_stub_8(void);  extern void isr_stub_9(void);
extern void isr_stub_10(void); extern void isr_stub_11(void);
extern void isr_stub_12(void); extern void isr_stub_13(void);
extern void isr_stub_14(void); extern void isr_stub_15(void);
extern void isr_stub_16(void); extern void isr_stub_17(void);
extern void isr_stub_18(void); extern void isr_stub_19(void);
extern void isr_stub_20(void); extern void isr_stub_21(void);
extern void isr_stub_22(void); extern void isr_stub_23(void);
extern void isr_stub_24(void); extern void isr_stub_25(void);
extern void isr_stub_26(void); extern void isr_stub_27(void);
extern void isr_stub_28(void); extern void isr_stub_29(void);
extern void isr_stub_30(void); extern void isr_stub_31(void);
extern void isr_stub_32(void); extern void isr_stub_33(void);
extern void isr_stub_34(void); extern void isr_stub_35(void);
extern void isr_stub_36(void); extern void isr_stub_37(void);
extern void isr_stub_38(void); extern void isr_stub_39(void);
extern void isr_stub_40(void); extern void isr_stub_41(void);
extern void isr_stub_42(void); extern void isr_stub_43(void);
extern void isr_stub_44(void); extern void isr_stub_45(void);
extern void isr_stub_46(void); extern void isr_stub_47(void);

void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags) {
    idt[num].offset_lo = handler & 0xFFFF;
    idt[num].offset_hi = (handler >> 16) & 0xFFFF;
    idt[num].selector  = selector;
    idt[num].zero      = 0;
    idt[num].flags     = flags;
}

void isr_register(uint8_t num, isr_handler_t handler) {
    handlers[num] = handler;
}

/* Validate the IRET frame about to be popped at `esp`. Rate-limited log
 * fires when the saved CS/EIP/EFLAGS look invalid for an IRET — used to
 * catch the kernel #GP-at-IRET storm during DPMI fixup (ses18). */
static void iret_frame_check(uint32_t esp) {
    static int logged = 0;
    if (logged >= 24) return;
    struct isr_frame *f = (struct isr_frame *)esp;

    uint32_t cs    = f->cs;
    uint32_t eip   = f->eip;
    uint32_t flags = f->eflags;
    int vm   = (flags & 0x20000) != 0;
    int rpl  = cs & 3;
    int ti   = cs & 4;

    const char *bad = 0;
    if (vm) {
        /* V86: any 16-bit RM CS is legal (CS=0 = segment 0 = IVT region,
         * which is a valid RM segment). Don't flag null-CS here. */
    }
    else if ((cs & 0xFFFC) == 0)        bad = "CS=null";
    else if (rpl == 0 && eip == 0)      bad = "ring0 EIP=0";
    else if (rpl == 0 && cs != 0x08)    bad = "ring0 CS!=0x08";
    else if (rpl == 3 && !ti)           bad = "ring3 CS no LDT bit";
    else if (rpl == 1 || rpl == 2)      bad = "CS RPL=1/2";
    else if ((flags & 2) == 0)          bad = "EFL bit1 clear";
    else if ((flags & (1<<15)) != 0)    bad = "EFL bit15 set";

    /* For ring-3 PM (non-V86) LDT CS, validate the descriptor against the
     * checks IRET will perform: present, code (X=1), DPL=3. IRET to a
     * data descriptor (S=1, X=0) #GPs with the selector as error code.
     * In V86 mode CS is a real-mode segment, not an LDT selector — skip. */
    struct dpmi_client *cs_client = 0;
    int cs_idx = -1;
    if (!bad && !vm && rpl == 3 && ti) {
        cs_idx = SEL_TO_IDX(cs & 0xFFFF);
        int ci;
        for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
            struct dpmi_client *p = dpmi_get_client(ci);
            if (p && p->active && cs_idx < DPMI_LDT_ENTRIES) {
                cs_client = p;
                break;
            }
        }
        if (cs_client) {
            uint8_t a = cs_client->ldt[cs_idx].access;
            if (!(a & 0x80))      bad = "CS desc not present";
            else if (!(a & 0x10)) bad = "CS desc S=0 (system seg)";
            else if (!(a & 0x08)) bad = "CS desc not code (X=0)";
            else if (((a >> 5) & 3) != 3) bad = "CS DPL != 3";
        }
    }

    if (!bad) return;

    serial_puts("!!! IRET frame bad (");
    serial_puts(bad);
    serial_puts("):\n  EIP=");
    serial_puthex(eip);
    serial_puts(" CS=");     serial_puthex(cs);
    serial_puts(" EFL=");    serial_puthex(flags);
    if (rpl != 0 || vm) {
        serial_puts("\n  SS=");
        serial_puthex(f->ss);
        serial_puts(":");
        serial_puthex(f->esp);
    }
    serial_puts("\n  int=");
    serial_puthex(f->int_no);
    serial_puts(" err=");
    serial_puthex(f->err_code);
    serial_puts(" entryCS=");
    serial_puthex(((struct isr_frame *)esp)->cs);
    serial_puts("\n");

    /* If we located the CS descriptor, dump its 8 bytes so we can see
     * exactly what the CPU will see at IRET. */
    if (cs_client && cs_idx >= 0) {
        uint8_t *d = (uint8_t *)&cs_client->ldt[cs_idx];
        serial_puts("  LDT[");
        serial_puthex(cs_idx);
        serial_puts("] desc:");
        int i;
        for (i = 0; i < 8; i++) {
            serial_puts(" ");
            serial_puthex(d[i]);
        }
        serial_puts("\n");
    }

    /* Ring-3 bad frame: dump 4 dwords from the about-to-be-IRETed
     * user stack so we can see who called/jumped to NULL. Walk active
     * DPMI clients to find one whose LDT contains the SS selector. */
    if (rpl == 3 && (f->ss & 4)) {
        int ss_idx = SEL_TO_IDX(f->ss & 0xFFFF);
        int ci;
        struct dpmi_client *c = 0;
        for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
            struct dpmi_client *p = dpmi_get_client(ci);
            if (p && p->active && ss_idx < DPMI_LDT_ENTRIES && LDT_USED(p, ss_idx)) {
                c = p;
                break;
            }
        }
        if (c) {
            uint32_t ss_base = desc_get_base(&c->ldt[ss_idx]);
            uint32_t *ustk = (uint32_t *)(ss_base + f->esp);
            serial_puts("  ustk:");
            int i;
            for (i = 0; i < 4; i++) {
                serial_puts(" ");
                serial_puthex(ustk[i]);
            }
            serial_puts("\n");
        }
    }
    logged++;
}

static uint32_t isr_dispatch_inner(uint32_t esp);

/* Called from isr_common in isr_stubs.asm.
 * Returns ESP — may be different if scheduler switched tasks. */
uint32_t isr_dispatch(uint32_t esp) {
    /* Snapshot input frame's EIP/CS/INT so we can detect any handler
     * that zeroed EIP. Only meaningful when no task switch occurred
     * (new_esp == esp); after a switch, in/out are different tasks. */
    uint32_t in_eip = ((struct isr_frame *)esp)->eip;
    uint32_t in_cs  = ((struct isr_frame *)esp)->cs;
    uint32_t in_int = ((struct isr_frame *)esp)->int_no;
    uint32_t new_esp = isr_dispatch_inner(esp);
    if (new_esp == esp) {
        struct isr_frame *out = (struct isr_frame *)new_esp;
        static int zeroed = 0;
        if (zeroed < 8 && in_eip != 0 && out->eip == 0) {
            serial_puts("!!! DPMI handler zeroed EIP: int=");
            serial_puthex(in_int);
            serial_puts(" inCS:EIP=");
            serial_puthex(in_cs);
            serial_puts(":");
            serial_puthex(in_eip);
            serial_puts(" outCS=");
            serial_puthex(out->cs);
            serial_puts("\n");
            zeroed++;
        }
    }
    iret_frame_check(new_esp);
    return new_esp;
}

static uint32_t isr_dispatch_inner(uint32_t esp) {
    struct isr_frame *frame = (struct isr_frame *)esp;
    uint32_t n = frame->int_no;

    /* INT 0x80 — pinecore network syscall vector. Caller's EBX held a
     * pointer to struct net_syscall_frame in *its own DS*. For Ring-3 PM
     * clients (e.g. DJGPP go32-v2 apps) DS has a non-zero base — EBX is
     * just an offset. Translate to a linear address through the caller's
     * LDT, the same way dpmi.c:2500 does for V86MT's DS:ESI argv.
     * Kernel callers (rare, future use) pass EBX = linear and hit the
     * fallback path. Dispatch is synchronous; no scheduler involvement. */
    if (n == 128) {
        struct net_syscall_frame *nf =
            (struct net_syscall_frame *)(unsigned long)frame->ebx;
        uint32_t ds_base = 0;
        uint16_t ds_sel = frame->ds & 0xFFFF;
        if (ds_sel & 4) {
            int ds_idx = SEL_TO_IDX(ds_sel);
            int ci;
            for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
                struct dpmi_client *p = dpmi_get_client(ci);
                if (p && p->active && ds_idx < DPMI_LDT_ENTRIES &&
                    LDT_USED(p, ds_idx)) {
                    ds_base = desc_get_base(&p->ldt[ds_idx]);
                    nf = (struct net_syscall_frame *)
                         (unsigned long)(ds_base + frame->ebx);
                    break;
                }
            }
        }
        net_dispatch(nf, ds_base);
        return esp;
    }

    /* Check if this came from V86 mode (VM bit set in EFLAGS) */
    if ((frame->eflags & 0x20000) && n < 32) {
        if (n == 13) {
            v86_gpf_handler((struct v86_frame *)frame);
            return esp;
        }
        if (n == 1) {
            /* Debug/Single-step exception — DOS extenders set TF
             * during CPU detection. Clear TF and resume. */
            frame->eflags &= ~0x100;  /* clear TF */
            return esp;
        }
        if (n == 0) {
            /* Divide error — skip the instruction (2 bytes for DIV/IDIV) */
            frame->eip += 2;
            return esp;
        }
        {
            /* s43 — one-shot byte dump per (cs,ip) so storm sites surface
             * their opcode bytes exactly once. */
            static uint32_t last_logged_csip = 0xFFFFFFFFu;
            uint32_t csip = ((frame->cs & 0xFFFF) << 16) | (frame->eip & 0xFFFF);
            serial_puts("V86: exception ");
            serial_puthex(n);
            serial_puts(" at CS:IP=");
            serial_puthex(frame->cs);
            serial_puts(":");
            serial_puthex(frame->eip);
            if (csip != last_logged_csip) {
                uint32_t lin = ((frame->cs & 0xFFFF) << 4) + (frame->eip & 0xFFFF);
                uint8_t *p = (uint8_t *)lin;
                int i;
                serial_puts(" bytes=");
                for (i = 0; i < 8; i++) {
                    serial_puthex(p[i]);
                    serial_puts(" ");
                }
                last_logged_csip = csip;
            }
            serial_puts("\n");
        }
        v86_gpf_handler((struct v86_frame *)frame);
        return esp;
    }

    /* Debug exception (INT 1) in kernel mode: likely TF propagated from PM client.
     * Just clear TF and continue. */
    if (n == 1 && (frame->cs & 3) == 0) {
        frame->eflags &= ~0x100;  /* clear TF */
        return esp;
    }

    /* Kernel-mode #PF inside the DPMI client linear zone. Happens when the
     * host itself touches a 0501-reserved-but-uncommitted client buffer
     * (e.g. 000B/000C descriptor memcpy through ES:EDI). Commit the page
     * and retry — same discipline as the Ring-3 demand-pager, just from
     * the kernel side. Anything outside the DPMI zone is a real kernel
     * bug and falls through. */
    if (n == 14 && (frame->cs & 3) == 0 && (frame->err_code & 1) == 0) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if (dpmi_kernel_pf_commit(cr2))
            return esp;
    }

    /* ---- PM DPMI client interrupt routing ----
     * Detect Ring 3 PM client by: VM=0, CS has LDT bit (bit 2) set, RPL=3.
     * This handles two cases:
     *   1. Software INT to a vector with an IDT entry (32-47 range —
     *      conflicts with IRQs). Check PIC ISR to distinguish.
     *   2. #GP from INT to a vector with no IDT entry or DPL=0 gate.
     *      Error code has bit 1 (IDT) set, vector in bits 15:3. */
    if (!(frame->eflags & 0x20000) && (frame->cs & 4)) {
        /* ---- PM DPMI client exception/interrupt routing ----
         *
         * For exceptions (0-31): check if client registered a handler
         * via INT 31h/0203h. If so, redirect there. This is critical —
         * DOS extenders like DOS/4GW rely on trapping their own #GPs
         * to translate real-mode segments to PM selectors.
         *
         * For software INTs (via #GP with IDT bit): route to DPMI handler.
         * For hardware IRQs: route normally. */

        /* Case 1: vector in IRQ range — is it a software INT or a real
         * hardware IRQ? Distinguish via the PIC ISR register; bit set ⇒ the
         * PIC is currently driving an interrupt (real IRQ), bit clear ⇒
         * client executed CD xx (software INT). */
        if (n >= 32 && n < 48) {
            uint8_t irq = n - 32;
            uint8_t isr_val;
            uint8_t mask;
            if (irq < 8) {
                outb(0x20, 0x0B);
                isr_val = inb(0x20);
                mask = 1 << irq;
            } else {
                outb(0xA0, 0x0B);
                isr_val = inb(0xA0);
                mask = 1 << (irq - 8);
            }
            int is_hw_irq = (isr_val & mask) != 0;

            if (!is_hw_irq) {
                /* Software INT in IRQ range — existing routing. */
                dpmi_busy++;
                uint32_t new_esp = dpmi_handle_pm_int((uint8_t)n, esp);
                dpmi_busy--;
                return new_esp ? new_esp : esp;
            }

            /* Real hardware IRQ delivered to a PM client. Translate the IDT
             * vector to its BIOS-style INT number (IRQ 0..7 → INT 0x08..0x0F,
             * IRQ 8..15 → INT 0x70..0x77 per CWSDPMI exphdlr.c:110-111) and,
             * if the client has hooked it, deliver to the PM handler. The
             * client handler EOIs (Allegro does outportb(0x20, 0x20) itself,
             * matching CWSDPMI's irq_common which also doesn't EOI). */
            uint8_t bios_vec = (irq < 8) ? (0x08 + irq) : (0x70 + (irq - 8));
            int hooked = dpmi_pm_has_handler(frame->cs, bios_vec);
            /* s38 PM-IRQ entry log. Diagnoses the s37 post-first-tick stall.
             * First 30 hits unconditional + every 16th per-vector hit after.
             * Reads both ISRs + both IMRs so we can see if EOI is lost or
             * the PIC masks itself between deliveries. */
            {
                static uint32_t pmirq_log_count = 0;
                static uint32_t pmirq_per_vector[256] = {0};
                pmirq_per_vector[bios_vec]++;
                if (pmirq_log_count < 30 ||
                    (pmirq_per_vector[bios_vec] & 0xF) == 0) {
                    uint8_t isr_m, isr_s;
                    if (irq < 8) {
                        isr_m = isr_val;
                        outb(0xA0, 0x0B);
                        isr_s = inb(0xA0);
                    } else {
                        outb(0x20, 0x0B);
                        isr_m = inb(0x20);
                        isr_s = isr_val;
                    }
                    uint8_t imr_m = inb(0x21);
                    uint8_t imr_s = inb(0xA1);
                    serial_puts("PMIRQ v=");
                    serial_puthex(bios_vec);
                    serial_puts(" hit=");
                    serial_puthex(pmirq_per_vector[bios_vec]);
                    serial_puts(" isr=");
                    serial_puthex(isr_m);
                    serial_puts("/");
                    serial_puthex(isr_s);
                    serial_puts(" imr=");
                    serial_puthex(imr_m);
                    serial_puts("/");
                    serial_puthex(imr_s);
                    serial_puts(" cs=");
                    serial_puthex(frame->cs);
                    serial_puts(":");
                    serial_puthex(frame->eip);
                    serial_puts(" hook=");
                    serial_puthex(hooked);
                    serial_puts("\n");
                    pmirq_log_count++;
                }
            }
            if (hooked) {
                dpmi_busy++;
                int rc = dpmi_deliver_pm_irq(bios_vec, esp);
                dpmi_busy--;
                if (rc == 0) {
                    /* s38: EOI the source PIC here, before IRETD launches
                     * Allegro's handler. Without this, master PIC stays in-
                     * service after the first delivery and blocks every
                     * subsequent IRQ (including cascade → slave). Allegro's
                     * handler EOIs at its tail, but that EOI doesn't appear
                     * to take effect under our host — leading hypothesis is
                     * the OUT 0x20,0x20 inside fixed_timer_handler runs but
                     * the PIC has already been re-armed; another hypothesis
                     * is the handler never reaches the EOI line. Either way
                     * a host-side EOI here makes Allegro's EOI a harmless
                     * no-op in steady state. */
                    pic_eoi(irq);
                    return esp;
                }
                /* rc < 0: client lookup failed despite has_handler success
                 * (shouldn't happen) — fall through to kernel handling. */
            }
            /* No PM handler: fall through to kernel-side IRQ handling below,
             * which runs the kernel handler (PIT counter, scheduler) and
             * EOIs the PIC. */
        }

        /* Case 2: #GP with IDT bit set — software INT to DPL=0 gate.
         * Could be CD xx (2 bytes), CC (INT3, 1 byte), or CE (INTO, 1 byte).
         * INTO generates vector 4, INT3 generates vector 3. */
        if (n == 13 && (frame->err_code & 2)) {
            uint8_t vector = (frame->err_code >> 3) & 0xFF;
            /* INTO (0xCE) and INT3 (0xCC) are 1-byte opcodes;
             * CD xx is 2 bytes. INTO always produces vector 4,
             * INT3 via CD 03 is 2 bytes (0xCC goes through IDT directly). */
            if (vector == 4)
                frame->eip += 1;  /* INTO (0xCE) is 1 byte */
            else
                frame->eip += 2;  /* CD xx is 2 bytes */
            dpmi_busy++;
            uint32_t new_esp = dpmi_handle_pm_int(vector, esp);
            dpmi_busy--;
            return new_esp ? new_esp : esp;
        }

        /* Case 3: CPU exception — redirect to client's exception handler. */
        if (n < 32) {
            dpmi_busy++;
            uint32_t new_esp = dpmi_handle_pm_exception(n, esp);
            dpmi_busy--;
            if (new_esp) return new_esp;
            /* No handler — halt with diagnostic */
            serial_puts("\n!!! DPMI EXCEPTION ");
            serial_puthex(n);
            serial_puts(" (no client handler)\n  ERR=");
            serial_puthex(frame->err_code);
            serial_puts(" EIP=");
            serial_puthex(frame->eip);
            serial_puts(" CS=");
            serial_puthex(frame->cs);
            serial_puts(" EAX=");
            serial_puthex(frame->eax);
            serial_puts("\n");
            __asm__ volatile ("cli; hlt");
        }
    }

    /* PIT Timer IRQ (INT 32 = IRQ 0) — kernel-side path.
     *
     * Only reached when PM was NOT running at IRQ time (V86 or kernel
     * was on the CPU). PM-resident delivery is handled above in the
     * PM-routing block. Here we just run the kernel PIT handler and
     * EOI; Allegro misses a tick during V86 execution, which is fine. */
    if (n == 32) {
        if (handlers[n])
            handlers[n](n, frame->err_code, frame->eip, frame->cs, frame->eflags);
        pic_eoi(0);
        return esp;
    }

    /* RTC IRQ (INT 40) — scheduler preemption at 8192 Hz */
    if (n == 40) {
        /* Sample where we were interrupted from — always-incrementing counter
         * so we see all execution modes (V86, PM ring3, kernel). */
        static uint32_t sample_count = 0;
        if ((++sample_count & 0x1FFF) == 0) {
            /* Dump task states whenever we sample so we can see when DOOM is
             * BLOCKED vs. simply not getting CPU. */
            extern void sched_diag_dump(void);
            sched_diag_dump();
            int in_v86 = !!(frame->eflags & 0x20000);
            int ring   = frame->cs & 7;
            serial_puts(in_v86 ? "SAMPLE V86 " : (ring == 7 ? "SAMPLE PM3 " : "SAMPLE K   "));
            serial_puthex(frame->cs);
            serial_puts(":");
            serial_puthex(frame->eip);
            if (in_v86) {
                /* Dump 6 bytes at CS:IP (linear = CS*16 + IP&0xFFFF) */
                uint32_t lin = (frame->cs << 4) + (frame->eip & 0xFFFF);
                uint8_t *c = (uint8_t *)lin;
                serial_puts(" code=");
                for (int i = 0; i < 6; i++) { serial_puthex(c[i]); serial_puts(" "); }
            }
            serial_puts("\n");
        }
        if (handlers[n])
            handlers[n](n, frame->err_code, frame->eip, frame->cs, frame->eflags);
        /* EOI to both slave and master PIC (IRQ 8 is on slave) */
        pic_eoi(8);
        /* RTC IRQ 8 maps to vector 0x28 in our PIC remap, but 0x28 is also
         * the DOS "idle yield" software INT. Delivering RTC as INT 0x28 to
         * a DOS extender that has hooked it expecting software-yield semantics
         * causes a yield storm at 8192 Hz that starves the client. Keep RTC
         * kernel-only; the client gets PIT (vector 0x20) as its timer signal. */
        /* Skip preemption while DPMI host is processing a client request.
         * Preempting during #GP fixups or INT 31h causes state corruption. */
        if (!dpmi_busy)
            sched_schedule(&esp);
        return esp;
    }

    /* s42 — Hardware IRQ reflection to V86. When IRQ 1 (vector 33,
     * keyboard) fires while a V86 task is running, attempt to
     * deliver INT 9 to the V86 task's installed handler. The kernel
     * handler still runs below (consumes port 0x60 and updates the
     * scancode cache + BDA + VT buffers); the V86 ISR sees the
     * cached scancode via the port 0x60 IN trap in v86.c. This
     * lets DOOM SETUP.EXE and other 1994-era apps that hook INT 9
     * directly receive keyboard input. Easy to extend to other IRQ
     * vectors (mouse INT 0x74, timer INT 8) when needed. */
    if ((frame->eflags & 0x20000) && n == 33) {
        v86_deliver_irq_to_handler((struct v86_frame *)frame, 0x09);
    }

    if (handlers[n]) {
        handlers[n](n, frame->err_code, frame->eip, frame->cs, frame->eflags);
        if (n >= 32 && n < 48)
            pic_eoi(n - 32);
        return esp;
    }

    /* Unhandled exception */
    if (n < 32) {
        /* s51 — paint a BSOD-style panic to VGA so the user has a
         * diagnostic even without serial COM1 wired (relevant for
         * Vortex86 boards where serial isn't accessible). Serial
         * dump below still fires for those who have COM1 — both
         * paths run from inside kernel_panic. */
        {
            extern void kernel_panic(const char *reason, void *isr_frame_ptr);
            const char *reason = (n < 21)
                ? "Unhandled CPU exception in kernel mode"
                : "Reserved CPU exception in kernel mode";
            kernel_panic(reason, frame);
            /* never returns */
        }
        serial_puts("\n!!! EXCEPTION: ");
        if (n < 21)
            serial_puts(exception_names[n]);
        serial_puts(" (INT ");
        serial_puthex(n);
        serial_puts(")\n  EIP=");
        serial_puthex(frame->eip);
        serial_puts(" ERR=");
        serial_puthex(frame->err_code);
        if (n == 14) {
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            serial_puts(" CR2=");
            serial_puthex(cr2);
        }
        serial_puts(" CS=");
        serial_puthex(frame->cs);
        serial_puts("\n");

        /* For #GP, the error code's selector index often points at the
         * descriptor that caused the fault. Dump it so we can see what
         * the CPU rejected. */
        if (n == 13 && frame->err_code) {
            uint32_t ec  = frame->err_code;
            int  ec_idx  = (ec >> 3) & 0x1FFF;
            int  ec_ti   = ec & 4;
            int  ec_idt  = ec & 2;
            serial_puts("  GPF sel idx=");
            serial_puthex(ec_idx);
            serial_puts(ec_ti ? " (LDT)" : " (GDT)");
            if (ec_idt) serial_puts(" [IDT]");
            serial_puts("\n");
            if (ec_ti && !ec_idt && ec_idx < DPMI_LDT_ENTRIES) {
                int ci;
                for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
                    struct dpmi_client *p = dpmi_get_client(ci);
                    if (!p || !p->active) continue;
                    uint8_t *d = (uint8_t *)&p->ldt[ec_idx];
                    serial_puts("  client ");
                    serial_puthex(ci);
                    serial_puts(" LDT[");
                    serial_puthex(ec_idx);
                    serial_puts("]:");
                    int i;
                    for (i = 0; i < 8; i++) {
                        serial_puts(" ");
                        serial_puthex(d[i]);
                    }
                    serial_puts("\n");
                }
            }
        }
        __asm__ volatile ("cli; hlt");
    }

    /* Unhandled IRQ — just send EOI */
    if (n >= 32 && n < 48)
        pic_eoi(n - 32);

    return esp;
}

void idt_init(void) {
    typedef void (*stub_fn)(void);
    stub_fn stubs[48] = {
        isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3,
        isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7,
        isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11,
        isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15,
        isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19,
        isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23,
        isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27,
        isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31,
        isr_stub_32, isr_stub_33, isr_stub_34, isr_stub_35,
        isr_stub_36, isr_stub_37, isr_stub_38, isr_stub_39,
        isr_stub_40, isr_stub_41, isr_stub_42, isr_stub_43,
        isr_stub_44, isr_stub_45, isr_stub_46, isr_stub_47,
    };
    int i;

    /* Zero out all entries */
    for (i = 0; i < 256; i++) {
        idt[i].offset_lo = 0;
        idt[i].selector  = 0;
        idt[i].zero      = 0;
        idt[i].flags     = 0;
        idt[i].offset_hi = 0;
        handlers[i] = NULL;
    }

    /* Install exception + IRQ stubs */
    for (i = 0; i < 48; i++)
        idt_set_gate(i, (uint32_t)stubs[i], 0x08, IDT_GATE_INT);

    /* Load IDT */
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtp));
}
