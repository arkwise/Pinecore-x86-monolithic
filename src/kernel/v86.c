/* v86.c -- Virtual 8086 Mode Monitor
 *
 * Creates and manages V86 tasks. When a V86 task executes a sensitive
 * instruction (INT, CLI, STI, PUSHF, POPF, IRET, IN, OUT), the CPU
 * generates GPF #13. We decode the instruction and emulate it.
 *
 * (ch-14, ch-02, 386-bible p.217-223)
 */

#include "types.h"
#include "v86.h"
#include "tss.h"
#include "dos.h"
#include "dpmi.h"
#include "pmm.h"
#include "heap.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "serial.h"
#include "vga.h"
#include "sched.h"
#include "vt.h"
#include "keyboard.h"
#include "dpmi.h"
#include "vcpi.h"

static struct v86_task v86_tasks[V86_MAX_TASKS];

/* Current V86 task ID (-1 = none) */
static int current_v86 = -1;


/* Flag: set when EXEC redirects the V86 frame to a child entry point.
 * When set, v86_gpf_handler must NOT advance EIP after the INT. */
static int v86_frame_redirected = 0;

/* Per-task jmp_buf is now in v86_task struct (v86.h) */

/* ================================================================
 * V86 stack helpers — read/write the V86 task's stack
 * (V86 addresses are identity-mapped: linear = seg*16 + off)
 * ================================================================ */

static uint16_t v86_stack_pop16(struct v86_frame *frame) {
    uint32_t addr = (frame->ss << 4) + (frame->esp & 0xFFFF);
    uint16_t val = *(uint16_t *)addr;
    frame->esp = (frame->esp & 0xFFFF0000) | ((frame->esp + 2) & 0xFFFF);
    return val;
}

static void v86_stack_push16(struct v86_frame *frame, uint16_t val) {
    frame->esp = (frame->esp & 0xFFFF0000) | ((frame->esp - 2) & 0xFFFF);
    uint32_t addr = (frame->ss << 4) + (frame->esp & 0xFFFF);
    *(uint16_t *)addr = val;
}

/* ================================================================
 * Read a byte from V86 memory (seg:off → linear)
 * ================================================================ */

static uint8_t v86_read_byte(uint16_t seg, uint16_t off) {
    uint32_t addr = (seg << 4) + off;
    return *(uint8_t *)addr;
}

/* Read a null-terminated string from V86 memory */
static void v86_read_string_until(uint16_t seg, uint16_t off, char terminator,
                                   char *buf, uint32_t max) {
    uint32_t i;
    for (i = 0; i < max - 1; i++) {
        char c = v86_read_byte(seg, off + i);
        if (c == terminator) break;
        buf[i] = c;
    }
    buf[i] = '\0';
}

/* ================================================================
 * Instruction emulation
 * ================================================================ */

/* INT nn — the most important: route to our DOS/BIOS emulation */
static void v86_emulate_int(struct v86_frame *frame, uint8_t int_num) {
    struct dos_regs regs;

    /* Trace all INTs for debugging extender sequences */
    if (int_num != 0x21 && int_num != 0x10 && int_num != 0x16) {
        serial_puts("V86: INT ");
        serial_puthex(int_num);
        serial_puts("h AX=");
        serial_puthex(frame->eax & 0xFFFF);
        serial_puts("\n");
    }

    switch (int_num) {

    case 0x20: {
        /* DOS terminate — like INT 21h/4Ch but with return code 0 */
        serial_puts("V86: INT 20h (terminate)\n");
        if (current_v86 >= 0) {
            int dtid = v86_tasks[current_v86].dos_task_id;
            extern struct dos_task *dos_get_task(int id);
            struct dos_task *dt = dos_get_task(dtid);
            if (dt && dt->parent.active) {
                /* Child process — restore parent */
                /* DON'T set v86_frame_redirected — parent needs +2
                 * to skip past the INT 21h (EXEC) instruction */
                serial_puts("V86: child exit via INT 20h -> parent\n");
                dt->return_code = 0;
                dt->parent.active = 0;
                frame->cs     = dt->parent.cs;
                frame->eip    = dt->parent.eip;
                frame->ss     = dt->parent.ss;
                frame->esp    = dt->parent.esp;
                frame->v86_ds = dt->parent.ds;
                frame->v86_es = dt->parent.es;
                frame->eax    = dt->parent.eax & 0xFFFF0000;
                frame->ebx    = dt->parent.ebx;
                frame->ecx    = dt->parent.ecx;
                frame->edx    = dt->parent.edx;
                frame->esi    = dt->parent.esi;
                frame->edi    = dt->parent.edi;
                frame->eflags = dt->parent.eflags | 0x20000 | 0x02;
                dt->psp_seg = dt->parent.psp_seg;
                dt->dta_seg = dt->parent.dta_seg;
                dt->dta_off = dt->parent.dta_off;
                dt->next_alloc_seg = dt->parent.next_alloc_seg;
                /* Clear carry = EXEC succeeded */
                frame->eflags &= ~1;
                break;
            }
        }
        /* Top-level — kill V86 task via scheduler */
        if (current_v86 >= 0) {
            current_v86 = -1;
            sched_v86_exit();  /* never returns */
        }
        break;
    }

    case 0x21: {
        /* DOS API — route to our INT 21h emulation */
        uint8_t ah_val = (frame->eax >> 8) & 0xFF;
        int result;

        regs.eax = frame->eax;
        regs.ebx = frame->ebx;
        regs.ecx = frame->ecx;
        regs.edx = frame->edx;
        regs.esi = frame->esi;
        regs.edi = frame->edi;
        regs.ebp = frame->ebp;
        regs.ds  = frame->v86_ds;
        regs.es  = frame->v86_es;
        regs.eflags = frame->eflags;
        /* Pass frame context for EXEC save/restore */
        regs.cs  = frame->cs;
        regs.eip = frame->eip;
        regs.ss  = frame->ss;
        regs.esp = frame->esp;

        result = DOS_RESULT_NORMAL;
        if (current_v86 >= 0)
            result = dos_int21(v86_tasks[current_v86].dos_task_id, &regs);

        if (result == DOS_RESULT_EXEC) {
            /* EXEC: redirect frame to child's entry point */
            struct dos_task *dt = 0;
            {
                int dtid = v86_tasks[current_v86].dos_task_id;
                /* Access the task's child entry info via dos_get_task */
                extern struct dos_task *dos_get_task(int id);
                dt = dos_get_task(dtid);
            }
            if (dt) {
                frame->cs     = dt->child.cs;
                frame->eip    = dt->child.ip;
                frame->ss     = dt->child.ss;
                frame->esp    = dt->child.sp;
                frame->v86_ds = dt->child.ds;
                frame->v86_es = dt->child.es;
                frame->eax    = 0;
                frame->ebx    = 0;
                frame->ecx    = 0;
                frame->edx    = 0;
                frame->esi    = 0;
                frame->edi    = 0;
                frame->eflags = 0x20202; /* VM=1, IF=1, bit1=1 */
                v86_frame_redirected = 1;
                serial_puts("V86: EXEC -> ");
                serial_puthex(frame->cs);
                serial_puts(":");
                serial_puthex(frame->eip);
                serial_puts("\n");
            }
            break;
        }

        if (result == DOS_RESULT_CHILD_EXIT) {
            /* Child exiting — restore parent's frame from saved state */
            struct dos_task *dt = 0;
            {
                int dtid = v86_tasks[current_v86].dos_task_id;
                extern struct dos_task *dos_get_task(int id);
                dt = dos_get_task(dtid);
            }
            if (dt) {
                frame->cs     = dt->parent.cs;
                frame->eip    = dt->parent.eip;
                frame->ss     = dt->parent.ss;
                frame->esp    = dt->parent.esp;
                frame->v86_ds = dt->parent.ds;
                frame->v86_es = dt->parent.es;
                frame->eax    = regs.eax;
                frame->ebx    = regs.ebx;
                frame->ecx    = regs.ecx;
                frame->edx    = regs.edx;
                frame->esi    = regs.esi;
                frame->edi    = regs.edi;
                frame->eflags = regs.eflags | 0x20000 | 0x02;
                /* DON'T set v86_frame_redirected — parent needs +2
                 * to skip past the INT 21h (EXEC) instruction */
                serial_puts("V86: child exit -> parent at ");
                serial_puthex(frame->cs);
                serial_puts(":");
                serial_puthex(frame->eip);
                serial_puts("\n");
            }
            break;
        }

        /* Check if task wants to exit (top-level) */
        if (ah_val == 0x4C && result == DOS_RESULT_NORMAL) {
            serial_puts("V86: task exiting via INT 21h/4Ch\n");
            if (current_v86 >= 0) {
                current_v86 = -1;
                sched_v86_exit();  /* never returns */
            }
        }

        /* Copy results back */
        frame->eax = regs.eax;
        frame->ebx = regs.ebx;
        frame->ecx = regs.ecx;
        frame->edx = regs.edx;
        frame->esi = regs.esi;
        frame->edi = regs.edi;
        frame->eflags = regs.eflags | 0x20000 | 0x02; /* keep VM bit + reserved */
        break;
    }

    case 0x10: {
        /* BIOS Video services */
        uint8_t ah_vid = (frame->eax >> 8) & 0xFF;
        switch (ah_vid) {
        case 0x00: {
            /* Set video mode: AL=mode */
            uint8_t mode = frame->eax & 0xFF;
            mode &= 0x7F; /* strip "don't clear" bit */
            if (mode == 0x13) {
                serial_puts("V86: set mode 13h (320x200x256)\n");
                vga_set_mode_13h();
            } else {
                serial_puts("V86: set mode ");
                serial_puthex(mode);
                serial_puts("h (text)\n");
                if (vga_get_mode() == 0x13)
                    vga_set_mode_03h();
                else
                    vga_clear();
            }
            *(volatile uint8_t *)0x449 = mode;
            break;
        }
        case 0x01:
            /* Set cursor shape — ignore */
            break;
        case 0x02:
            /* Set cursor position: DH=row, DL=col, BH=page */
            vga_set_cursor(frame->edx & 0xFF, (frame->edx >> 8) & 0xFF);
            break;
        case 0x03:
            /* Get cursor position: BH=page → DH=row, DL=col, CH/CL=shape */
            {
                uint8_t cx_out, cy_out;
                vga_get_cursor(&cx_out, &cy_out);
                frame->edx = (frame->edx & 0xFFFF0000) | (cy_out << 8) | cx_out;
                frame->ecx = (frame->ecx & 0xFFFF0000) | 0x0607; /* cursor shape */
            }
            break;
        case 0x06: {
            /* Scroll up: AL=lines (0=clear), BH=attr, CH/CL=top/left, DH/DL=bot/right */
            uint8_t lines = frame->eax & 0xFF;
            uint8_t attr  = (frame->ebx >> 8) & 0xFF;
            uint8_t top   = (frame->ecx >> 8) & 0xFF;
            uint8_t left  = frame->ecx & 0xFF;
            uint8_t bot   = (frame->edx >> 8) & 0xFF;
            uint8_t right = frame->edx & 0xFF;
            vga_scroll_up(lines, attr, top, left, bot, right);
            break;
        }
        case 0x07: {
            /* Scroll down: AL=lines (0=clear), BH=attr, CH/CL=top/left, DH/DL=bot/right */
            uint8_t lines = frame->eax & 0xFF;
            uint8_t attr  = (frame->ebx >> 8) & 0xFF;
            uint8_t top   = (frame->ecx >> 8) & 0xFF;
            uint8_t left  = frame->ecx & 0xFF;
            uint8_t bot   = (frame->edx >> 8) & 0xFF;
            uint8_t right = frame->edx & 0xFF;
            vga_scroll_down(lines, attr, top, left, bot, right);
            break;
        }
        case 0x08:
            /* Read char+attr at cursor → AH=attr, AL=char */
            frame->eax = (frame->eax & 0xFFFF0000) | 0x0720; /* space, white on black */
            break;
        case 0x09: {
            /* Write char+attr at cursor WITHOUT advancing cursor.
             * AL=char, BL=attr, CX=count. Write directly to VGA buffer. */
            uint8_t ch = frame->eax & 0xFF;
            uint8_t attr = frame->ebx & 0xFF;
            uint16_t count = frame->ecx & 0xFFFF;
            uint16_t k;
            uint8_t cx_pos, cy_pos;
            volatile uint16_t *vga_buf = (volatile uint16_t *)0xB8000;
            vga_get_cursor(&cx_pos, &cy_pos);
            for (k = 0; k < count && (cy_pos * 80 + cx_pos + k) < 80 * 25; k++)
                vga_buf[cy_pos * 80 + cx_pos + k] = (attr << 8) | ch;
            break;
        }
        case 0x0C: {
            /* Write pixel: AL=color, BH=page, CX=x, DX=y */
            if (vga_get_mode() == 0x13) {
                uint16_t x = frame->ecx & 0xFFFF;
                uint16_t y = frame->edx & 0xFFFF;
                if (x < 320 && y < 200) {
                    uint8_t *fb = (uint8_t *)0xA0000;
                    fb[y * 320 + x] = frame->eax & 0xFF;
                }
            }
            break;
        }
        case 0x0D: {
            /* Read pixel: BH=page, CX=x, DX=y → AL=color */
            if (vga_get_mode() == 0x13) {
                uint16_t x = frame->ecx & 0xFFFF;
                uint16_t y = frame->edx & 0xFFFF;
                if (x < 320 && y < 200) {
                    uint8_t *fb = (uint8_t *)0xA0000;
                    frame->eax = (frame->eax & 0xFFFFFF00) | fb[y * 320 + x];
                }
            }
            break;
        }
        case 0x0E:
            /* Teletype output: AL=char, BL=color (in graphics modes) */
            vga_putc(frame->eax & 0xFF);
            serial_putc(frame->eax & 0xFF);
            break;
        case 0x0F:
            /* Get current video mode → AH=columns, AL=mode, BH=page */
            {
                uint8_t m = vga_get_mode();
                uint8_t cols = (m == 0x13) ? 40 : 80;
                frame->eax = (frame->eax & 0xFFFF0000) | (cols << 8) | m;
                frame->ebx = (frame->ebx & 0xFFFF0000); /* page 0 */
            }
            break;
        case 0x10: {
            /* Palette / DAC functions */
            uint8_t al = frame->eax & 0xFF;
            if (al == 0x10) {
                /* Set individual DAC register: BX=register, DH=R, CH=G, CL=B */
                outb(0x3C8, frame->ebx & 0xFF);
                outb(0x3C9, (frame->edx >> 8) & 0x3F);
                outb(0x3C9, (frame->ecx >> 8) & 0x3F);
                outb(0x3C9, frame->ecx & 0x3F);
            } else if (al == 0x12) {
                /* Set block of DAC registers: BX=first, CX=count, ES:DX=table */
                uint16_t first = frame->ebx & 0xFFFF;
                uint16_t count = frame->ecx & 0xFFFF;
                uint8_t *tbl = (uint8_t *)((frame->v86_es << 4) + (frame->edx & 0xFFFF));
                uint16_t k;
                outb(0x3C8, first);
                for (k = 0; k < count * 3; k++)
                    outb(0x3C9, tbl[k] & 0x3F);
            } else if (al == 0x15) {
                /* Read individual DAC register: BX=register → DH=R, CH=G, CL=B */
                outb(0x3C7, frame->ebx & 0xFF);
                uint8_t r = inb(0x3C9);
                uint8_t g = inb(0x3C9);
                uint8_t b = inb(0x3C9);
                frame->edx = (frame->edx & 0xFFFF00FF) | (r << 8);
                frame->ecx = (frame->ecx & 0xFFFF0000) | (g << 8) | b;
            } else if (al == 0x17) {
                /* Read block of DAC registers: BX=first, CX=count, ES:DX=buffer */
                uint16_t first = frame->ebx & 0xFFFF;
                uint16_t count = frame->ecx & 0xFFFF;
                uint8_t *tbl = (uint8_t *)((frame->v86_es << 4) + (frame->edx & 0xFFFF));
                uint16_t k;
                outb(0x3C7, first);
                for (k = 0; k < count * 3; k++)
                    tbl[k] = inb(0x3C9);
            } else if (al == 0x00) {
                /* Set individual palette register — ignore for mode 13h */
            } else if (al == 0x02) {
                /* Set all palette registers — ignore for mode 13h */
            }
            break;
        }
        case 0x11:
            /* Character generator — ignore */
            break;
        case 0x12:
            /* Alternate select — return EGA/VGA info */
            frame->ebx = (frame->ebx & 0xFFFF0000) | 0x0003; /* 256K, color */
            break;
        case 0x1A:
            /* Get/set display combination code → BL=active display */
            frame->eax = (frame->eax & 0xFFFF0000) | 0x1A; /* function supported */
            frame->ebx = (frame->ebx & 0xFFFF0000) | 0x08; /* VGA color */
            break;
        default: {
            /* Rate-limited trace: which subfn did we silently drop? Each unique
             * AH value will cause repeats, so we throttle by AH so different
             * subfns aren't masked by the first one we see. */
            static uint16_t seen = 0;
            uint16_t bit = (uint16_t)1u << (ah_vid & 0x0F);
            if (!(seen & bit)) {
                seen |= bit;
                serial_puts("V86: INT 10h AH=");
                serial_puthex(ah_vid);
                serial_puts(" unhandled (AX=");
                serial_puthex(frame->eax & 0xFFFF);
                serial_puts(" BX=");
                serial_puthex(frame->ebx & 0xFFFF);
                serial_puts(" CX=");
                serial_puthex(frame->ecx & 0xFFFF);
                serial_puts(" DX=");
                serial_puthex(frame->edx & 0xFFFF);
                serial_puts(")\n");
            }
            break;
        }
        }
        break;
    }

    case 0x11:
        /* BIOS Equipment List — return equipment flags in AX
         * Bit 0: floppy present, Bits 4-5: initial video mode (2=80x25 color)
         * Bit 1: math coprocessor */
        frame->eax = (frame->eax & 0xFFFF0000) | 0x0021; /* floppy + 80x25 color */
        break;

    case 0x12:
        /* BIOS Memory Size — return conventional memory in KB in AX */
        frame->eax = (frame->eax & 0xFFFF0000) | 640;
        break;

    case 0x15: {
        /* BIOS INT 15h — miscellaneous services */
        uint8_t ah_15 = (frame->eax >> 8) & 0xFF;
        if (ah_15 == 0x88) {
            /* Get extended memory size (KB above 1MB) */
            /* We have 32MB total, minus 1MB conventional = 31MB = 31744 KB */
            /* Cap at 0xFFFF (65535 KB ≈ 64MB) since AX is 16-bit */
            frame->eax = (frame->eax & 0xFFFF0000) | (31 * 1024);
            frame->eflags &= ~1;  /* clear CF = success */
        } else if (ah_15 == 0x87) {
            /* Block move (copy memory via GDT) — used by some extenders.
             * CX = word count, ES:SI → GDT for source/dest descriptors.
             * For now: perform the copy directly (we're Ring 0, can access all memory) */
            uint32_t count = (frame->ecx & 0xFFFF) * 2;  /* word count → byte count */
            uint8_t *gdt_buf = (uint8_t *)((frame->v86_es << 4) + (frame->esi & 0xFFFF));
            /* GDT entries: [0]=dummy, [1]=dummy, [2]=source, [3]=dest, [4]=BIOS CS, [5]=BIOS SS */
            uint32_t src_base = gdt_buf[0x12] | (gdt_buf[0x13] << 8) |
                                (gdt_buf[0x14] << 16) | (gdt_buf[0x17] << 24);
            uint32_t dst_base = gdt_buf[0x1A] | (gdt_buf[0x1B] << 8) |
                                (gdt_buf[0x1C] << 16) | (gdt_buf[0x1F] << 24);
            uint8_t *src = (uint8_t *)src_base;
            uint8_t *dst = (uint8_t *)dst_base;
            uint32_t i;
            for (i = 0; i < count; i++)
                dst[i] = src[i];
            frame->eax = (frame->eax & 0xFFFF0000);  /* AH=0 success */
            frame->eflags &= ~1;
        } else if (ah_15 == 0xE8 && (frame->eax & 0xFF) == 0x01) {
            /* E801h: Get memory size for large configurations */
            /* Return: AX=CX=KB between 1-16MB, BX=DX=64KB blocks above 16MB */
            frame->eax = (frame->eax & 0xFFFF0000) | (15 * 1024); /* 15MB below 16MB */
            frame->ecx = (frame->ecx & 0xFFFF0000) | (15 * 1024);
            frame->ebx = (frame->ebx & 0xFFFF0000) | ((32 - 16) * 16); /* 16MB above 16MB in 64K blocks */
            frame->edx = (frame->edx & 0xFFFF0000) | ((32 - 16) * 16);
            frame->eflags &= ~1;
        } else if (ah_15 == 0xBF) {
            /* DOS/16M / DOS/4GW proprietary host API.
             * BFDEh, BF02h, BF00h, BF01h = installation check.
             * Must return DX=0 explicitly to say "not installed".
             * If DX is left non-zero from stale regs, extender thinks
             * a host is present and jumps to garbage. */
            frame->edx = frame->edx & 0xFFFF0000;  /* DX=0: not installed */
            frame->esi = frame->esi & 0xFFFF0000;  /* SI=0 */
            frame->eflags |= 1;  /* CF=1: not supported */
        } else {
            /* Unknown INT 15h — return error.
             * Clear AH to avoid garbage in status byte. */
            frame->eflags |= 1;  /* CF=1 = not supported */
        }
        break;
    }

    case 0x16: {
        /* BIOS Keyboard */
        uint8_t ah_kb = (frame->eax >> 8) & 0xFF;
        /* s42 diag: log every INT 16h entry so we can see polls
         * (AH=0x01/0x11) and unsupported sub-functions, not just
         * blocking reads. */
        {
            static uint32_t int16_count = 0;
            int16_count++;
            if (int16_count < 50 || (int16_count & 0x3F) == 0) {
                serial_puts("V86: INT 16h AH=");
                serial_puthex(ah_kb);
                serial_puts(" #");
                serial_puthex(int16_count);
                serial_puts("\n");
            }
        }
        if (ah_kb == 0x00 || ah_kb == 0x10) {
            /* Blocking read: AH=scan code, AL=ASCII */
            int vt_num = -1;
            if (current_v86 >= 0) {
                /* Find the VT for this V86 task */
                int si;
                for (si = 0; si < 16; si++) {
                    struct task *st = sched_get_task(si);
                    if (st && st->v86_task_id == current_v86 && st->vt >= 0) {
                        vt_num = st->vt;
                        break;
                    }
                }
            }
            {
                struct key_event kev;
                serial_puts("V86: INT 16h AH=");
                serial_puthex(ah_kb);
                serial_puts(" wait on vt=");
                serial_puthex(vt_num);
                serial_puts("\n");
                while (1) {
                    if (vt_num >= 0 && vt_poll_key(vt_num, &kev)) {
                        if (kev.pressed && (kev.ascii || kev.scancode)) {
                            uint8_t sc = kev.scancode & 0x7F;
                            uint8_t ch = kev.ascii;
                            frame->eax = (frame->eax & 0xFFFF0000) | (sc << 8) | ch;
                            frame->eflags &= ~0x40;  /* clear ZF */
                            serial_puts("V86: INT 16h key sc=");
                            serial_puthex(sc);
                            serial_puts(" ch=");
                            serial_puthex(ch);
                            serial_puts("\n");
                            break;
                        }
                    }
                    /* Can't use sched_block inside GPF handler — use sti;hlt;cli */
                    __asm__ volatile("sti; hlt; cli");
                }
            }
        } else if (ah_kb == 0x01 || ah_kb == 0x11) {
            /* Check key: ZF=1 if no key, ZF=0 if key ready */
            int vt_num = -1;
            if (current_v86 >= 0) {
                int si;
                for (si = 0; si < 16; si++) {
                    struct task *st = sched_get_task(si);
                    if (st && st->v86_task_id == current_v86 && st->vt >= 0) {
                        vt_num = st->vt;
                        break;
                    }
                }
            }
            if (vt_num >= 0) {
                struct vt *v = vt_get(vt_num);
                if (v && v->key_head != v->key_tail) {
                    /* Key available — peek without consuming */
                    struct key_event *kev = &v->key_buf[v->key_tail];
                    uint8_t sc = kev->scancode & 0x7F;
                    uint8_t ch = kev->ascii;
                    frame->eax = (frame->eax & 0xFFFF0000) | (sc << 8) | ch;
                    frame->eflags &= ~0x40;  /* clear ZF = key ready */
                } else {
                    frame->eflags |= 0x40;  /* ZF = 1 (no key) */
                }
            } else {
                frame->eflags |= 0x40;  /* ZF = 1 (no key) */
            }
        } else if (ah_kb == 0x02) {
            /* Get shift flags — AL = BIOS 0x40:0x17 layout */
            frame->eax = (frame->eax & 0xFFFFFF00) | keyboard_get_shift_flags();
        } else if (ah_kb == 0x12) {
            /* Extended shift flags (enhanced keyboards): AL = 0x17 layout,
             * AH = 0x18 layout (L/R Ctrl + L/R Alt distinction).
             * DFlat+'s getshift() uses this when BDA 0x40:0x96 bit 4 is set. */
            uint8_t al = keyboard_get_shift_flags();
            uint8_t ah = keyboard_get_ext_flags();
            frame->eax = (frame->eax & 0xFFFF0000) | ((uint32_t)ah << 8) | al;
        }
        break;
    }

    case 0x1A: {
        /* BIOS Timer — get/set tick count */
        uint8_t ah_tmr = (frame->eax >> 8) & 0xFF;
        if (ah_tmr == 0x00) {
            /* Get tick count — read from BIOS data area at 0040:006C */
            uint32_t ticks = *(volatile uint32_t *)0x46C;
            frame->ecx = (frame->ecx & 0xFFFF0000) | (ticks >> 16);
            frame->edx = (frame->edx & 0xFFFF0000) | (ticks & 0xFFFF);
            frame->eax = (frame->eax & 0xFFFFFF00); /* midnight flag = 0 */
        }
        break;
    }

    case 0x2F: {
        /* DOS Multiplex */
        uint16_t ax_2f = frame->eax & 0xFFFF;
        if (ax_2f == 0x1687) {
            /* DPMI detection — announce host.
             *
             * s42 exploratory — Fix B: bumped DX 0x005A (DPMI 0.90)
             * → 0x0100 (DPMI 1.0). Theory: DOS/16M probes a different
             * (shorter / less-iterative) vendor list when it sees a
             * 1.0 host, possibly avoiding the cascade that leads to
             * the 0x276F:0x0008 crash. Paired with Fix A in v86.c
             * INT 31h unhandled fallback. UNCONFIRMED — exploratory
             * pass, may need to revert if it breaks other clients. */
            frame->eax = frame->eax & 0xFFFF0000;  /* AX=0: DPMI present */
            frame->ebx = (frame->ebx & 0xFFFF0000) | 0x0001; /* 32-bit supported */
            frame->ecx = (frame->ecx & 0xFFFF0000) | 0x0003; /* CPU = i386 */
            frame->edx = (frame->edx & 0xFFFF0000) | 0x0100; /* DPMI 1.00 [s42-B] */
            frame->esi = (frame->esi & 0xFFFF0000) | 0x0000; /* 0 paragraphs — we manage state in kernel */
            /* Entry point — use a well-known address in low memory.
             * We'll place a special INT instruction there that our GPF handler catches. */
            frame->v86_es = 0x0000;
            frame->edi = (frame->edi & 0xFFFF0000) | 0x0500; /* ES:DI = 0000:0500 */
            /* Write INT 0xF1 (unused) + IRET at 0x500 as the entry stub */
            {
                uint8_t *entry = (uint8_t *)0x500;
                entry[0] = 0xCD;  /* INT */
                entry[1] = 0xF1;  /* our private "DPMI enter" vector */
                entry[2] = 0xCB;  /* RETF */
            }
            serial_puts("V86: INT 2Fh/1687h — DPMI detected by client\n");
        } else if (ax_2f == 0x1686) {
            /* "Get CPU mode" — AX=0 means already in PM */
            /* We're in V86, so return non-zero (still in RM from client's perspective) */
            frame->eax = (frame->eax & 0xFFFF0000) | 0x0001;
        } else {
            frame->eax = frame->eax & 0xFFFFFF00; /* AL=0: not installed */
        }
        break;
    }

    case 0x31: {
        /* DPMI INT 31h from V86. The big spec audience is PM but DOS/4GW
         * (and friends) issue 0x0200 (Get RM IVT) and 0x0201 (Set RM IVT)
         * during their real-mode bring-up to discover/install handlers
         * BEFORE the PM transition. We need to service those here or the
         * extender thinks every IVT entry is whatever junk it read back.
         *
         * After PM exit, DOS/4GW does a *second* V86 INT 31h sequence to
         * unwind state (0x000B/0C descriptor save-restore, 0x0A00 vendor
         * API, 0x0305/0306 save-state). Returning CF=1 + AX unchanged
         * left AX = the function number (e.g. 0x000B) which DOS/4GW
         * then used as an index — out-of-range → BOUND #5 exception
         * mid-cleanup → conventional memory never freed → second EXEC
         * fails with DOS/16M "not enough memory". So for these we
         * return CF=0 + zeroed result regs (safe "no-op success"). */
        uint16_t ax = frame->eax & 0xFFFF;
        /* s42 — one-shot env-MCB corruption tripwire at INT 31h entry.
         * Catches the V86 INT 31h call that the env-MCB byte-write
         * happens between. */
        {
            static int v86_env_corrupt_logged = 0;
            if (!v86_env_corrupt_logged) {
                uint8_t *emcb = (uint8_t *)0xFFF0;
                uint16_t e_owner = *(uint16_t *)(emcb + 1);
                uint16_t e_size  = *(uint16_t *)(emcb + 3);
                if (emcb[0] == 'M' && (e_owner != 0x1100 || e_size != 0x00FF)) {
                    serial_puts("!!! env-MCB corrupt AT v86 INT 31h AX=");
                    serial_puthex(ax);
                    serial_puts(" owner=");
                    serial_puthex(e_owner);
                    serial_puts(" size=");
                    serial_puthex(e_size);
                    serial_puts("\n");
                    v86_env_corrupt_logged = 1;
                }
            }
        }
        if (ax == 0x0200) {
            uint8_t int_num = frame->ebx & 0xFF;
            uint16_t *ivt = (uint16_t *)((uint32_t)int_num * 4);
            frame->ecx = (frame->ecx & 0xFFFF0000) | ivt[1];
            frame->edx = (frame->edx & 0xFFFF0000) | ivt[0];
            frame->eflags &= ~1;
        } else if (ax == 0x0201) {
            uint8_t int_num = frame->ebx & 0xFF;
            uint16_t *ivt = (uint16_t *)((uint32_t)int_num * 4);
            ivt[0] = frame->edx & 0xFFFF;
            ivt[1] = frame->ecx & 0xFFFF;
            frame->eflags &= ~1;
        } else if (ax == 0x0204 || ax == 0x0202) {
            /* Get PM Interrupt Vector / Get PM Exception Handler from
             * V86 mode. DPMI 1.0 allows this; DOS/4GW relies on it
             * during its post-PM-exit unwind to read back the handlers
             * IT INSTALLED during its PM session. Look up the still-
             * pm_exited client owned by the current V86 task and return
             * the recorded selector:offset. */
            extern struct dpmi_client *dpmi_find_client_for_v86(int);
            struct dpmi_client *cc = dpmi_find_client_for_v86(current_v86);
            uint8_t int_num = frame->ebx & 0xFF;
            if (cc) {
                if (ax == 0x0204) {
                    frame->ecx = (frame->ecx & 0xFFFF0000) |
                                 cc->pm_vectors[int_num].selector;
                    frame->edx = cc->pm_vectors[int_num].offset;
                } else { /* 0x0202 */
                    int e = int_num & 0x1F;
                    frame->ecx = (frame->ecx & 0xFFFF0000) |
                                 cc->pm_exc_vectors[e].selector;
                    frame->edx = cc->pm_exc_vectors[e].offset;
                }
                frame->eax    = frame->eax & 0xFFFF0000;   /* AX = 0  */
                frame->eflags &= ~1;                        /* CF = 0  */
            } else {
                /* No client found — fall back to fake success with 0:0 */
                frame->eax    = frame->eax & 0xFFFF0000;
                frame->ecx    = frame->ecx & 0xFFFF0000;
                frame->edx    = 0;
                frame->eflags &= ~1;
            }
        } else if (ax == 0x000B) {
            /* Get Descriptor: copy the 8-byte LDT entry of selector BX
             * to ES:EDI. Use the surviving pm_exited client's LDT. */
            extern struct dpmi_client *dpmi_find_client_for_v86(int);
            struct dpmi_client *cc = dpmi_find_client_for_v86(current_v86);
            uint16_t sel = frame->ebx & 0xFFFF;
            uint16_t idx = sel / 8;
            uint32_t dst = ((uint32_t)(frame->v86_es) << 4) +
                           (frame->edi & 0xFFFF);
            uint8_t *dst_p = (uint8_t *)dst;
            if (cc && idx < DPMI_LDT_ENTRIES) {
                uint8_t *src = (uint8_t *)&cc->ldt[idx];
                int k;
                for (k = 0; k < 8; k++) dst_p[k] = src[k];
            } else {
                int k;
                for (k = 0; k < 8; k++) dst_p[k] = 0;
            }
            frame->eax    = frame->eax & 0xFFFF0000;
            frame->eflags &= ~1;
        } else if (ax == 0x0006 || ax == 0x0007 ||
                   ax == 0x0008 || ax == 0x0009) {
            /* s41a — LDT field ops. Operate on cc->ldt[idx] for the
             * surviving (active or pm_exited) client of current_v86.
             * Conventions (DPMI 0.9 / DOS/32A where refs disagree):
             *   0x0006 Get Base:    CX:DX = base[31:16]:base[15:0]
             *   0x0007 Set Base:    CX:DX in, write to descriptor
             *   0x0008 Set Limit:   CX:DX as 32-bit; desc_set_limit
             *                       auto-sets G=1 above 1 MB
             *   0x0009 Set Access:  CL → byte 5, CH bits 7/6/4 → byte 6
             *                       high nibble (G/D-B/AVL); L bit 5
             *                       filtered, limit[19:16] preserved. */
            extern struct dpmi_client *dpmi_find_client_for_v86(int);
            struct dpmi_client *cc = dpmi_find_client_for_v86(current_v86);
            uint16_t sel = frame->ebx & 0xFFFF;
            uint16_t idx = sel / 8;
            /* s44 — log first 4 of each op so we can correlate AX=0x0008
             * Set Limit with the [32] error that follows it. */
            {
                static int n_06=0, n_07=0, n_08=0, n_09=0;
                int *np = (ax==0x0006)?&n_06:(ax==0x0007)?&n_07:
                          (ax==0x0008)?&n_08:&n_09;
                if ((*np)++ < 4) {
                    serial_puts("V86: 0x000");
                    serial_puthex(ax & 0xF);
                    serial_puts(" BX=");
                    serial_puthex(sel);
                    serial_puts(" CX:DX=");
                    serial_puthex(frame->ecx & 0xFFFF);
                    serial_puts(":");
                    serial_puthex(frame->edx & 0xFFFF);
                    serial_puts(" cc=");
                    serial_puthex((uint32_t)cc);
                    serial_puts(" idx=");
                    serial_puthex(idx);
                    serial_puts("\n");
                }
            }
            if (!cc || idx == 0 || idx >= DPMI_LDT_ENTRIES) {
                frame->eax = (frame->eax & 0xFFFF0000) | 0x8022;
                frame->eflags |= 1;
            } else {
                struct seg_descriptor *d = &cc->ldt[idx];
                if (ax == 0x0006) {
                    uint32_t base = desc_get_base(d);
                    frame->ecx = (frame->ecx & 0xFFFF0000) |
                                 ((base >> 16) & 0xFFFF);
                    frame->edx = (frame->edx & 0xFFFF0000) |
                                 (base & 0xFFFF);
                } else if (ax == 0x0007) {
                    uint32_t base =
                        ((uint32_t)(frame->ecx & 0xFFFF) << 16) |
                        (frame->edx & 0xFFFF);
                    desc_set_base(d, base);
                } else if (ax == 0x0008) {
                    uint32_t limit =
                        ((uint32_t)(frame->ecx & 0xFFFF) << 16) |
                        (frame->edx & 0xFFFF);
                    /* DPMI 0.9 §3.4 / DOSEMU2 dpmi.c:1101: limits
                     * > 1MB MUST have low 12 bits = 0xFFF. Silently
                     * auto-shifting (s41a shortcut) corrupts the
                     * descriptor — DOS/16M trusts the spec-mandated
                     * rejection and retries with page-granular
                     * rounding. Without this, descriptor widens
                     * silently → DOS/16M pointer arithmetic drifts
                     * → crash at V86 0x276F:0x0008. */
                    if (limit > 0xFFFFF && (~limit & 0xFFF)) {
                        frame->eax = (frame->eax & 0xFFFF0000) | 0x8025;
                        frame->eflags |= 1;
                        break;
                    }
                    desc_set_limit(d, limit);
                } else { /* 0x0009 */
                    uint8_t cl = frame->ecx & 0xFF;
                    uint8_t ch = (frame->ecx >> 8) & 0xFF;
                    d->access = cl;
                    d->limit_hi = (d->limit_hi & 0x0F) | (ch & 0xD0);
                }
                frame->eax = frame->eax & 0xFFFF0000;
                frame->eflags &= ~1;
            }
        } else if (ax == 0x0040) {
            /* Vendor probe (DOS/16M issues this; not DPMI standard).
             * Observed s41a behavior: returning HDPMI's CF=1/AX=0x8001
             * unsupported response makes DOS/4GW use 0x8001 as the AX
             * for its NEXT INT 31h, spinning ~575 calls before bailing
             * with "DOS/16M error [32] DPMI host error". Returning
             * clean success (CF=0, AX=0) lets DOS/4GW conclude
             * "no vendor extension here" and proceed. */
            frame->eax = frame->eax & 0xFFFF0000;
            frame->eflags &= ~1;
        } else if (ax == 0x0000) {
            /* Allocate LDT Descriptors. CX = count → AX = base selector.
             * Allocate against the still-active client. If none, fake
             * a sentinel selector (DOS/16M sometimes calls this just to
             * probe — it doesn't always use the returned value). */
            extern int dpmi_alloc_ldt_v86(int, int);
            int count = frame->ecx & 0xFFFF;
            int base = dpmi_alloc_ldt_v86(current_v86, count);
            /* s44 — log first 4 + every 64th + first failure so we can
             * see which selectors DOS/16M actually got. */
            {
                static int alloc_n = 0;
                static int sentinel_n = 0;
                int n = alloc_n++;
                if (n < 4 || (n & 0x3F) == 0 ||
                    (base <= 0 && sentinel_n++ < 2)) {
                    serial_puts("V86: 0x0000 #");
                    serial_puthex(n);
                    serial_puts(" cnt=");
                    serial_puthex(count);
                    serial_puts(" cur_v86=");
                    serial_puthex(current_v86);
                    serial_puts(" base=");
                    serial_puthex(base);
                    serial_puts("\n");
                }
            }
            if (base > 0) {
                uint16_t sel = (uint16_t)((base * 8) | 4 | 3);
                frame->eax = (frame->eax & 0xFFFF0000) | sel;
                frame->eflags &= ~1;
            } else {
                /* No client / no space — fake-succeed with a plausible
                 * value so DOS/16M doesn't bail. (If DOS/16M actually
                 * uses this selector later we'll #GP, but in practice
                 * it's a probe.) */
                frame->eax = (frame->eax & 0xFFFF0000) | 0x40;
                frame->eflags &= ~1;
            }
        } else if (ax == 0x0001) {
            /* Free Descriptor — no-op success */
            frame->eax = frame->eax & 0xFFFF0000;
            frame->eflags &= ~1;
        } else if (ax == 0x0003) {
            /* Get Selector Increment Value: AX = 8 (one selector slot) */
            frame->eax = (frame->eax & 0xFFFF0000) | 8;
            frame->eflags &= ~1;
        } else if (ax == 0x000C ||
                   ax == 0x012F ||
                   ax == 0x09E7 ||
                   ax == 0x129F ||
                   ax == 0x0A00 ||
                   ax == 0x0304 ||
                   ax == 0x0305 || ax == 0x0306 ||
                   ax == 0x0203 || ax == 0x0205 ||
                   ax == 0x0212 || ax == 0x0213) {
            /* Two classes of "safe success" calls from V86:
             *   - Post-PM-exit cleanup queries (0x000B/0C/0A00/0305/0306):
             *     DOS/4GW unwinds via these AFTER returning from PM.
             *   - Pre-PM-transition handler installation (0x0202/0203/
             *     0204/0205/0212/0213): DOS/4GW pre-registers PM
             *     exception/IRQ handlers from V86 before the mode
             *     switch — DPMI 1.0 allows this. We don't yet have a
             *     client struct to record them in; returning success
             *     lets the loader proceed (DOS/4GW typically re-installs
             *     from PM after transition anyway).
             * Both classes: CF=0, AX=0, output regs zeroed. */
            frame->eax = frame->eax & 0xFFFF0000;        /* AX = 0 */
            frame->eflags &= ~1;                          /* CF = 0 */
            if (ax == 0x0A00 || ax == 0x0305) {
                /* Far-pointer returns — zero so DOS/4GW doesn't deref
                 * garbage. Note: V86 ES lives in v86_es. */
                frame->v86_es = 0;
                frame->edi    = frame->edi & 0xFFFF0000;
                frame->esi    = frame->esi & 0xFFFF0000;
            }
        } else if (ax == 0x0303) {
            /* s42 — V86-mode Allocate Real-Mode Callback (real impl).
             *
             * DOS/16M issues 0x0303 from V86 during its post-PM-exit
             * cleanup with DS:SI = V86 handler address (inside its
             * own segment 0x276F) and ES:DI = register save buffer.
             * Earlier attempts to fake the response (return zero,
             * sentinel IRET-pad, or echo DS:SI) all triggered the
             * "DOS/16M error [32] DPMI host error" — DOS/16M
             * validates the returned CS:IP against its own state
             * and rejects bogus values.
             *
             * Real implementation: allocate a slot in the surviving
             * client's rmcb[] table, write a 4-byte stub at
             * 0x0070:(id*4) containing INT F2 + id + IRET, and
             * return CX:DX = 0x0070:(id*4). Mark rmcb[id].rm_mode=1
             * so dpmi_rmcb_dispatch knows to skip the PM transition
             * and just IRET back (since the handler is already in
             * V86 land). DOS/16M can then verify and use the stub. */
            extern struct dpmi_client *dpmi_find_client_for_v86(int);
            struct dpmi_client *cc = dpmi_find_client_for_v86(current_v86);
            if (cc && cc->next_rmcb < DPMI_MAX_RMCB) {
                int id = cc->next_rmcb;
                uint16_t seg = 0x0070;
                uint16_t off = (uint16_t)(id * 4);
                uint8_t *stub = (uint8_t *)((uint32_t)seg * 16 + off);
                stub[0] = 0xCD;   /* INT */
                stub[1] = 0xF2;   /* private RM callback vector */
                stub[2] = (uint8_t)id;
                stub[3] = 0xCF;   /* IRET fallback */

                cc->rmcb[id].pm_sel   = frame->v86_ds & 0xFFFF;   /* RM seg */
                cc->rmcb[id].pm_off   = frame->esi & 0xFFFF;       /* RM off */
                cc->rmcb[id].regs_sel = frame->v86_es & 0xFFFF;
                cc->rmcb[id].regs_off = frame->edi & 0xFFFF;
                cc->rmcb[id].active   = 1;
                cc->rmcb[id].rm_mode  = 1;
                cc->next_rmcb++;

                serial_puts("V86: rmcb ");
                serial_puthex(id);
                serial_puts(" (RM-mode) = ");
                serial_puthex(seg);
                serial_puts(":");
                serial_puthex(off);
                serial_puts(" → RM ");
                serial_puthex(frame->v86_ds & 0xFFFF);
                serial_puts(":");
                serial_puthex(frame->esi & 0xFFFF);
                serial_puts("\n");

                frame->eax    = frame->eax & 0xFFFF0000;       /* AX = 0 */
                frame->ecx    = (frame->ecx & 0xFFFF0000) | seg;
                frame->edx    = (frame->edx & 0xFFFF0000) | off;
                frame->eflags &= ~1;                            /* CF = 0 */
            } else {
                /* No client or no free slot — return CF=1 + 0x8015
                 * "callbacks unavailable" per DPMI spec. */
                serial_puts("V86: 0x0303 — no client/slot\n");
                frame->eax    = (frame->eax & 0xFFFF0000) | 0x8015;
                frame->eflags |= 1;
            }
        } else if (ax >= 0x1000 ||
                   (ax > 0x0102 && ax < 0x0200) ||
                   (ax > 0x0213 && ax < 0x0300) ||
                   (ax > 0x0306 && ax < 0x0400) ||
                   (ax > 0x0401 && ax < 0x0500) ||
                   (ax > 0x050B && ax < 0x0600) ||
                   (ax > 0x0604 && ax < 0x0700) ||
                   (ax > 0x0703 && ax < 0x0800) ||
                   (ax > 0x0801 && ax < 0x0900) ||
                   (ax > 0x0902 && ax < 0x0A00) ||
                   (ax > 0x0A00 && ax < 0x0B00) ||
                   (ax > 0x0B03 && ax < 0x0C00) ||
                   (ax > 0x0C01 && ax < 0x0D00) ||
                   (ax > 0x0D03 && ax < 0x0E00) ||
                   (ax > 0x0E01)) {
            /* s42 — Fix A (narrowed): silent-succeed ONLY for AX
             * values OUTSIDE every DPMI 0.9/1.0 spec-defined range.
             * These are vendor-specific probes (0x012F, 0x09E7,
             * 0x129F, 0x1B57, 0x240F, 0x2CC7, 0x357F, …). DOS/16M
             * iterates them looking for a recognised host vendor;
             * CF=1 to all makes it fall back to its panic handler
             * which lives at uninitialised 0x276F:0x0008.
             *
             * In-spec AX values still fall through to the original
             * CF=1 behaviour below (DOS/16M can handle legitimate
             * "function not supported" responses for those). */
            serial_puts("V86: INT 31h AX=");
            serial_puthex(ax);
            serial_puts(" (vendor probe, silent-succeed [s42-A])\n");
            frame->eax    = frame->eax & 0xFFFF0000;  /* AX = 0  */
            frame->eflags &= ~1;                       /* CF = 0  */
        } else {
            serial_puts("V86: INT 31h AX=");
            serial_puthex(ax);
            serial_puts(" (unhandled in V86)\n");
            frame->eflags |= 1;  /* CF=1: unsupported */
        }
        break;
    }

    case 0x33:
        /* Mouse — stub: not installed (AX=0) */
        frame->eax = frame->eax & 0xFFFF0000;
        break;

    case 0x67: {
        /* EMS / VCPI services */
        uint8_t ah_ems = (frame->eax >> 8) & 0xFF;

        serial_puts("V86: INT 67h AH=");
        serial_puthex(ah_ems);
        serial_puts(" AL=");
        serial_puthex(frame->eax & 0xFF);
        serial_puts("\n");

        if (ah_ems == 0xDE) {
            /* VCPI subfunctions */
            uint8_t al_vcpi = frame->eax & 0xFF;

#ifndef KERNEL_MODE_PURE
            /* DOS mode: VCPI not available. Tell the extender "not present"
             * and print a user-visible message. */
            serial_puts("V86: VCPI call DE");
            serial_puthex(al_vcpi);
            serial_puts("h blocked (DOS mode)\n");
            if (al_vcpi == 0x00) {
                /* Print notice to VGA */
                vga_puts("\n  Protected mode DOS apps require the Pure kernel.\n");
                vga_puts("  Boot with pinecore-pure to run DOOM and other PM apps.\n\n");
            }
            frame->eax = (frame->eax & 0xFFFF0000) | 0x8400; /* AH=84 not found */
            break;
#endif

            switch (al_vcpi) {
            case 0x00:
                /* s45: VCPI detect now returns "NOT PRESENT" (AH=0x84). The
                 * full VCPI server (DE01 PM-Interface + DE0C mode-switch)
                 * below is preserved for historical/research reference, but
                 * is unreachable from clients because they won't probe past
                 * a failed DE00. Reason: VCPI requires the client to run at
                 * Ring 0 in PM (load its own GDT/IDT/CR3), which conflicts
                 * fundamentally with our DPMI architecture (we are Ring 0,
                 * clients are Ring 3). When DOS/32A's detect.asm sees VCPI
                 * present, it picks the VCPI path and skips the INT 2Fh
                 * AX=1687h DPMI probe — bypassing our actual DPMI host.
                 * Returning "not present" forces it to fall through to the
                 * DPMI detector, which already works.
                 *
                 * --- ORIGINAL "VCPI present v1.0" reply (kept commented):
                 *   frame->eax = (frame->eax & 0xFFFF0000);
                 *   frame->ebx = (frame->ebx & 0xFFFF0000) | 0x0100;
                 *   serial_puts("V86: VCPI detect → present v1.0\n");
                 */
                frame->eax = (frame->eax & 0xFFFF0000) | 0x8400;
                serial_puts("V86: VCPI detect → NOT PRESENT (forcing DPMI path)\n");
                break;
            case 0x01: {
                /* Get Protected Mode Interface
                 * ES:DI → buffer for 3 GDT descriptors (24 bytes)
                 * DS:SI → buffer for page table entries (client's first MB mapping)
                 * Returns: EBX = PM entry point offset (in first GDT code segment)
                 *
                 * The 3 GDT entries are:
                 *   [0] = VCPI code segment (Ring 0, our kernel code)
                 *   [1] = VCPI data segment (Ring 0, our kernel data)
                 *   [2] = client code segment (Ring 3)
                 *
                 * We provide a PM entry point that the client can CALL from PM
                 * to invoke VCPI services (mainly DE0Ch for mode switching).
                 * For simplicity, we point it to a known location. */
                uint8_t *gdt_buf = (uint8_t *)((frame->v86_es << 4) + (frame->edi & 0xFFFF));
                uint8_t *pt_buf = (uint8_t *)((frame->v86_ds << 4) + (frame->esi & 0xFFFF));
                uint32_t i;

                /* GDT entry 0: VCPI code (Ring 0, flat, exec/read) */
                gdt_buf[0] = 0xFF; gdt_buf[1] = 0xFF;  /* limit 0xFFFF */
                gdt_buf[2] = 0x00; gdt_buf[3] = 0x00;  /* base 0 */
                gdt_buf[4] = 0x00;
                gdt_buf[5] = 0x9A;  /* P=1, DPL=0, code exec/read */
                gdt_buf[6] = 0xCF;  /* G=1, D=1, limit 0xF */
                gdt_buf[7] = 0x00;

                /* GDT entry 1: VCPI data (Ring 0, flat, read/write) */
                gdt_buf[8]  = 0xFF; gdt_buf[9]  = 0xFF;
                gdt_buf[10] = 0x00; gdt_buf[11] = 0x00;
                gdt_buf[12] = 0x00;
                gdt_buf[13] = 0x92;
                gdt_buf[14] = 0xCF;
                gdt_buf[15] = 0x00;

                /* GDT entry 2: client code (Ring 3, flat, exec/read) */
                gdt_buf[16] = 0xFF; gdt_buf[17] = 0xFF;
                gdt_buf[18] = 0x00; gdt_buf[19] = 0x00;
                gdt_buf[20] = 0x00;
                gdt_buf[21] = 0xFA;  /* P=1, DPL=3, code exec/read */
                gdt_buf[22] = 0xCF;
                gdt_buf[23] = 0x00;

                /* Fill page table with identity mapping for first 1MB (256 entries) */
                for (i = 0; i < 256; i++) {
                    *(uint32_t *)(pt_buf + i * 4) = (i * 4096) | 0x07;  /* P+RW+User */
                }

                /* PM entry point — return 0 (the client will FAR CALL this
                 * with AX=DE0Ch to switch modes. We'll handle it via GPF
                 * since the client's GDT has Ring 0 code at a fake address). */
                frame->ebx = 0;

                frame->eax = (frame->eax & 0xFFFF0000);  /* AH=0 success */
                serial_puts("V86: VCPI DE01h — PM interface provided\n");
                break;
            }

            case 0x02:
                /* Get max physical address */
                frame->eax = (frame->eax & 0xFFFF0000);
                frame->edx = 0x02000000;  /* 32MB */
                break;
            case 0x03:
                /* Get free 4KB pages */
                frame->eax = (frame->eax & 0xFFFF0000);
                frame->edx = pmm_get_free_count();
                break;
            case 0x04: {
                /* Allocate 4KB page → EDX=physical address */
                uint32_t page = pmm_alloc_page();
                if (page) {
                    frame->eax = (frame->eax & 0xFFFF0000);  /* AH=0 */
                    frame->edx = page;
                } else {
                    frame->eax = (frame->eax & 0xFFFF0000) | 0x8800; /* AH=88 */
                }
                break;
            }
            case 0x05:
                /* Free 4KB page: EDX=physical address */
                pmm_free_page(frame->edx);
                frame->eax = (frame->eax & 0xFFFF0000);
                break;
            case 0x06:
                /* Get physical address of page in first MB: CX=page# → EDX=phys */
                frame->eax = (frame->eax & 0xFFFF0000);
                frame->edx = (frame->ecx & 0xFFFF) * 4096;  /* identity mapped */
                break;
            case 0x07:
                /* Read CR0 */
                frame->eax = (frame->eax & 0xFFFF0000);
                {
                    uint32_t cr0;
                    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
                    frame->ebx = cr0;
                }
                break;
            case 0x0A:
                /* Get PIC vectors: BX=master, CX=slave */
                frame->eax = (frame->eax & 0xFFFF0000);
                frame->ebx = (frame->ebx & 0xFFFF0000) | 0x0020; /* master at INT 20h */
                frame->ecx = (frame->ecx & 0xFFFF0000) | 0x0028; /* slave at INT 28h */
                break;
            case 0x0B:
                /* Set PIC vectors — accept and ignore */
                frame->eax = (frame->eax & 0xFFFF0000);
                break;
            case 0x0C: {
                /* SWITCH TO PROTECTED MODE — the big one.
                 * ESI = linear address of data structure (in first MB):
                 *   +00: CR3 (page directory physical address)
                 *   +04: linear address of 6-byte GDTR pseudo-descriptor
                 *   +08: linear address of 6-byte IDTR pseudo-descriptor
                 *   +0C: LDTR selector (word)
                 *   +0E: TR selector (word)
                 *   +10: EIP of PM entry point (dword)
                 *   +14: CS of PM entry point (word)
                 *
                 * We DON'T load client's CR3/GDT/IDT — that would kill our kernel.
                 * Instead we parse the client's GDT, create LDT entries, and
                 * transition like DPMI (clear VM, set LDT selectors). */
                uint32_t struct_lin = frame->esi;

                uint32_t client_cr3 = *(uint32_t *)(struct_lin + 0x00);
                uint32_t client_gdt_ptr = *(uint32_t *)(struct_lin + 0x04);
                uint32_t client_idt_ptr = *(uint32_t *)(struct_lin + 0x08);
                uint16_t client_ldt = *(uint16_t *)(struct_lin + 0x0C);
                uint16_t client_tr  = *(uint16_t *)(struct_lin + 0x0E);
                uint32_t client_eip = *(uint32_t *)(struct_lin + 0x10);
                uint16_t client_cs  = *(uint16_t *)(struct_lin + 0x14);

                /* Dereference the GDTR pseudo-descriptor to get the actual GDT base */
                uint32_t client_gdt = *(uint32_t *)(client_gdt_ptr + 2);
                uint16_t client_gdt_limit = *(uint16_t *)(client_gdt_ptr);

                (void)client_cr3; (void)client_idt_ptr; (void)client_ldt;
                (void)client_tr; (void)client_gdt_limit;

                serial_puts("V86: VCPI DE0Ch — PM switch request\n");
                serial_puts("  ESI="); serial_puthex(struct_lin);
                serial_puts("  CR3="); serial_puthex(client_cr3);
                serial_puts("  GDT="); serial_puthex(client_gdt);
                serial_puts(" (limit="); serial_puthex(client_gdt_limit);
                serial_puts(")\n  CS="); serial_puthex(client_cs);
                serial_puts(":EIP="); serial_puthex(client_eip);
                serial_puts("  LDT="); serial_puthex(client_ldt);
                serial_puts("  TR="); serial_puthex(client_tr);
                serial_puts("\n");

                /* Parse client's GDT to find the CS descriptor */
                {
                    uint8_t *gdt_base = (uint8_t *)client_gdt;
                    int cs_idx = client_cs / 8;
                    uint8_t *cs_desc = gdt_base + cs_idx * 8;
                    uint32_t cs_base = cs_desc[2] | (cs_desc[3] << 8) |
                                       (cs_desc[4] << 16) | (cs_desc[7] << 24);
                    uint32_t cs_limit = cs_desc[0] | (cs_desc[1] << 8) |
                                        ((cs_desc[6] & 0x0F) << 16);
                    uint8_t cs_access = cs_desc[5];
                    int cs_is32 = (cs_desc[6] & 0x40) ? 1 : 0;

                    if (cs_desc[6] & 0x80)  /* G bit */
                        cs_limit = (cs_limit << 12) | 0xFFF;

                    serial_puts("  Client CS: base=");
                    serial_puthex(cs_base);
                    serial_puts(" limit=");
                    serial_puthex(cs_limit);
                    serial_puts(cs_is32 ? " 32-bit\n" : " 16-bit\n");

                    /* Create DPMI client and transition */
                    {
                        int cid = dpmi_enter_pm(current_v86, cs_is32);
                        if (cid >= 0) {
                            struct dpmi_client *dc = dpmi_get_client(cid);
                            if (dc) {
                                int ldt_cs = SEL_TO_IDX(dc->client_cs);
                                int ldt_ds = SEL_TO_IDX(dc->client_ds);

                                /* Set up code segment from client's GDT CS entry */
                                dpmi_ldt_setup(cid, ldt_cs, cs_base, cs_limit,
                                    0xFA, cs_is32);  /* exec/read DPL=3 */

                                /* Data segment: use flat (base=0, limit=4GB) for DOS/16M */
                                dpmi_ldt_setup(cid, ldt_ds, 0, 0xFFFFFFFF,
                                    0xF2, 1);  /* data r/w DPL=3, 32-bit */

                                /* Perform the transition */
                                frame->eip = client_eip;
                                frame->cs  = frame->cs;  /* will be overwritten by transition */

                                if (dpmi_transition_to_pm(cid, frame) == 0) {
                                    /* Override EIP to client's requested entry */
                                    frame->eip = client_eip;
                                    v86_frame_redirected = 1;
                                    serial_puts("V86: VCPI → PM transition complete\n");
                                }
                            }
                        }
                    }
                }
                break;
            }
            default:
                serial_puts("V86: VCPI unhandled func DE");
                serial_puthex(al_vcpi);
                serial_puts("h\n");
                frame->eax = (frame->eax & 0xFFFF0000) | 0x8400; /* AH=84 not found */
                break;
            }
        } else if (ah_ems == 0x40) {
#ifdef KERNEL_MODE_PURE
            /* EMS: Get Status → AH=0 (OK, EMS present) */
            frame->eax = (frame->eax & 0xFFFF0000);
#else
            /* DOS mode: EMS not available */
            frame->eax = (frame->eax & 0xFFFF0000) | 0x8400;
#endif
        } else if (ah_ems == 0x41) {
            /* EMS: Get Page Frame Address → AH=0, BX=segment (fake) */
            frame->eax = (frame->eax & 0xFFFF0000);
            frame->ebx = (frame->ebx & 0xFFFF0000) | 0xE000;
        } else if (ah_ems == 0x42) {
            /* EMS: Get Number of Pages → AH=0, BX=free, DX=total */
            frame->eax = (frame->eax & 0xFFFF0000);
            frame->ebx = (frame->ebx & 0xFFFF0000) | 0x0000; /* 0 free EMS pages */
            frame->edx = (frame->edx & 0xFFFF0000) | 0x0000; /* 0 total EMS pages */
        } else if (ah_ems == 0x46) {
            /* EMS: Get Version → AH=0, AL=version (4.0 = 0x40) */
            frame->eax = (frame->eax & 0xFFFF0000) | 0x0040;
        } else {
            /* Other EMS functions — return error */
            frame->eax = (frame->eax & 0xFFFF0000) | 0x8400;
        }
        break;
    }

    case 0xF1: {
        /* Private DPMI mode switch vector.
         * Called when DOS extender calls our entry point at 0000:0500.
         * AX bit 0 = 1 for 32-bit client.
         * We need to transition this V86 task to Ring 3 PM. */
        int is_32bit = frame->eax & 1;
        int client_id;

        serial_puts("V86: DPMI mode switch requested (");
        serial_puts(is_32bit ? "32-bit" : "16-bit");
        serial_puts(")\n");

        /* One-time IVT dump so we can verify what the client will see
         * if it reads IVT directly to save chain targets. */
        {
            static int dumped = 0;
            if (!dumped) {
                dumped = 1;
                uint16_t *ivt = (uint16_t *)0;
                serial_puts("  IVT dump: ");
                uint8_t vecs[] = {0x08, 0x09, 0x10, 0x16, 0x1B, 0x21, 0x23, 0x24,
                                  0x28, 0x2A, 0x2F, 0x33};
                unsigned i;
                for (i = 0; i < sizeof(vecs); i++) {
                    serial_puthex(vecs[i]);
                    serial_puts("=");
                    serial_puthex(ivt[vecs[i]*2 + 1]);
                    serial_puts(":");
                    serial_puthex(ivt[vecs[i]*2 + 0]);
                    serial_puts(" ");
                }
                serial_puts("\n");
            }
        }

        /* The DOS extender did CALL FAR [entry], which pushed CS:IP
         * on the V86 stack. Pop the return address — this is where
         * execution should resume after the mode switch. */
        {
            uint32_t stack_addr = (frame->ss << 4) + (frame->esp & 0xFFFF);
            uint16_t ret_ip = *(uint16_t *)(stack_addr);
            uint16_t ret_cs = *(uint16_t *)(stack_addr + 2);
            frame->esp = (frame->esp & 0xFFFF0000) | ((frame->esp + 4) & 0xFFFF);

            /* Restore the original CS (frame->cs is 0x0000 from the entry stub) */
            frame->cs = ret_cs;
            frame->eip = ret_ip;

            serial_puts("  Return to ");
            serial_puthex(ret_cs);
            serial_puts(":");
            serial_puthex(ret_ip);
            serial_puts("\n");
        }

        client_id = dpmi_enter_pm(current_v86, is_32bit);
        if (client_id >= 0) {
            if (dpmi_transition_to_pm(client_id, frame) == 0) {
                v86_frame_redirected = 1;  /* don't advance EIP */
            } else {
                frame->eflags |= 1;
            }
        } else {
            frame->eflags |= 1;
        }
        break;
    }

    case 0xF5:
    case 0xF6:
    case 0xF7: {
        /* DOS/BIOS service stubs invoked via 0x302 trampoline.
         * IVT[0x21/0x10/0x16] points at 0:0x550/0x554/0x558 which contain
         * CD F[5/6/7] CF — INT 0xFn ; IRET. When DOS/4GW (or any DOS app)
         * uses INT 31h/0302 to chain to the saved RM INT 21h handler, the
         * trampoline lands here; we route to the existing emulator just
         * like a plain V86 INT 21h/10h/16h would. */
        uint8_t routed = (int_num == 0xF5) ? 0x21 :
                         (int_num == 0xF6) ? 0x10 : 0x16;
        v86_emulate_int(frame, routed);
        break;
    }

    case 0xF8: {
        /* Path B (s51 Vortex86 USB) — direct BIOS INT 16h call.
         *
         * Used by the V86 keyboard polling task in src/kernel/v86_kbd.c
         * to call the real BIOS INT 16h handler (which knows about USB
         * legacy emulation) instead of our INT 16h emulator at case
         * 0x16 above (which reads from our kernel queue — empty on
         * Vortex86 because the BIOS's INT 09h hook can't fire under
         * our PM IDT). Effectively a one-vector escape hatch for
         * code that wants the real BIOS keyboard.
         *
         * Reflection: push flags/CS/(EIP+2) onto the V86 stack — the
         * +2 is the return address PAST the `CD F8` instruction so
         * the BIOS handler's IRET resumes at the next instruction.
         * Set frame->cs:eip to IVT[0x16] handler. Clear IF/TF (matches
         * real INT instruction). Set v86_frame_redirected so the
         * dispatcher doesn't double-advance EIP. */
        uint16_t *ivt = (uint16_t *)(0x16 * 4);
        uint16_t handler_off = ivt[0];
        uint16_t handler_seg = ivt[1];

        if (handler_seg == 0 && handler_off == 0) {
            /* IVT[0x16] empty — BIOS didn't install a keyboard handler.
             * Return ZF=1 (no key) to the caller so it loops harmlessly. */
            frame->eflags |= 0x40;
            break;
        }

        v86_stack_push16(frame, frame->eflags & 0xFFFF);
        v86_stack_push16(frame, frame->cs & 0xFFFF);
        v86_stack_push16(frame, (frame->eip + 2) & 0xFFFF);
        frame->cs  = handler_seg;
        frame->eip = handler_off;
        frame->eflags &= ~0x0200;          /* clear IF (real INT does this) */
        frame->eflags &= ~0x0100;          /* clear TF */
        frame->eflags |= 0x20000 | 0x02;   /* keep VM + reserved */

        v86_frame_redirected = 1;          /* dispatcher must NOT advance EIP */
        break;
    }

    case 0xF4: {
        /* Synchronous-RM-call sentinel (PM↔RM trampoline, see
         * docs/research/29-dpmi-host.md).
         * When a PM client invokes INT 31h AX=0x0301/0x0302, dpmi.c
         * stashes the RmCallStruct address, switches the task to V86
         * mode at the requested RM target, and pushes a return frame
         * onto the V86 stack pointing at `CD F4 CF` at linear 0x50C.
         * The RM proc executes naturally; when it IRETs/RETFs back to
         * our sentinel, this case fires. The unwind helper writes the
         * post-call V86 regs into the caller's RmCallStruct and
         * restores the saved PM frame so IRET from the V86 monitor
         * lands the task back at the PM instruction after INT 31h. */
        dpmi_rm_call_unwind(frame);
        v86_frame_redirected = 1;  /* dispatcher rewrote frame, don't advance */
        break;
    }

    case 0xF2: {
        /* Private RM callback vector.
         * Fired when V86 code executes INT 0xF2 at a callback stub
         * (written by INT 31h/0303). The byte AFTER the 2-byte INT
         * instruction is the callback ID.
         * Stub layout: CD F2 xx CF  (INT 0xF2 / id / IRET)
         * frame->eip still points at the CD byte (not yet advanced). */
        uint32_t stub_linear = ((frame->cs & 0xFFFF) << 4) +
                               ((frame->eip + 2) & 0xFFFF);
        uint8_t rmcb_id = *(uint8_t *)stub_linear;
        /* Advance EIP past the 3-byte sequence (CD F2 xx) so the
         * normal +2 advancement in the caller lands on the IRET (CF). */
        frame->eip += 1;  /* caller will add 2 more → total +3 */

        dpmi_busy++;
        dpmi_rmcb_dispatch(rmcb_id, (uint32_t)frame);
        dpmi_busy--;
        break;
    }

    default: {
        /* Reflect to IVT handler installed by the V86 program.
         * Reflect if the handler is in program memory (>= 0x0070) but
         * not in ROM/stale BIOS area (< 0xA000). Segment 0x0070+ covers
         * our RM callback stubs (0x0700+) and user program memory.
         * Segment 0x0000 with offset 0x0600 (IRET pad) is a no-op. */
        uint16_t *ivt = (uint16_t *)(int_num * 4);
        uint16_t handler_off = ivt[0];
        uint16_t handler_seg = ivt[1];

        if (handler_seg >= 0x0070 && handler_seg < 0xA000) {
            /* Program-installed handler — simulate INT: push flags, cs, ip */
            v86_stack_push16(frame, frame->eflags & 0xFFFF);
            v86_stack_push16(frame, frame->cs & 0xFFFF);
            v86_stack_push16(frame, frame->eip & 0xFFFF);
            /* Jump to handler */
            frame->cs = handler_seg;
            frame->eip = handler_off;
            /* Clear IF and TF (like real INT) */
            frame->eflags &= ~0x0200;
            frame->eflags &= ~0x0100;
            frame->eflags |= 0x20000 | 0x02; /* keep VM + reserved */
        }
        /* Otherwise silently ignore */
        break;
    }
    }
}

/* s42 — Reflect a hardware IRQ to the V86 task's IVT handler.
 * Called from idt.c isr_dispatch when an IRQ fires while V86 is
 * running. Mirrors the "INT instruction redirect" pattern (see
 * `default` arm of v86_emulate_int): push CS:IP:FLAGS, jump to
 * IVT handler, clear IF/TF. CPU IRETs into the V86 ISR; the ISR
 * EOIs the PIC itself (BIOS convention) and IRETs back to the
 * interrupted V86 code which pops our pushed frame.
 *
 * The IVT handler must be in V86 program memory (seg ≥ 0x0070,
 * < 0xA000) — otherwise it's a stale/default vector and we
 * return 0 so the caller falls through to kernel handling. */
int v86_deliver_irq_to_handler(struct v86_frame *frame, uint8_t vector) {
    uint16_t *ivt = (uint16_t *)((uint32_t)vector * 4);
    uint16_t handler_off = ivt[0];
    uint16_t handler_seg = ivt[1];

    /* s42 diag: log IVT[vector] so we know whether SETUP hooked it */
    {
        static uint32_t irq_diag_count = 0;
        if (irq_diag_count++ < 30) {
            serial_puts("V86 IRQ deliver vec=");
            serial_puthex(vector);
            serial_puts(" IVT=");
            serial_puthex(handler_seg);
            serial_puts(":");
            serial_puthex(handler_off);
            serial_puts("\n");
        }
    }

    if (handler_seg < 0x0070 || handler_seg >= 0xA000) {
        return 0;
    }

    v86_stack_push16(frame, frame->eflags & 0xFFFF);
    v86_stack_push16(frame, frame->cs & 0xFFFF);
    v86_stack_push16(frame, frame->eip & 0xFFFF);
    frame->cs     = handler_seg;
    frame->eip    = handler_off;
    frame->eflags &= ~0x0200;            /* IF=0: ISR runs with interrupts off */
    frame->eflags &= ~0x0100;            /* TF=0 */
    frame->eflags |= 0x20000 | 0x02;     /* keep VM + reserved */
    return 1;
}

/* PUSHF — push flags to V86 stack */
static void v86_emulate_pushf(struct v86_frame *frame) {
    /* Push flags with IOPL=3 so CPU detection code sees 386+ behavior.
     * Clear VM bit (bit 17 won't fit in 16-bit push anyway). */
    uint16_t flags = frame->eflags & 0xFFFF;
    flags |= 0x3000;  /* force IOPL=3 visible to V86 code */
    v86_stack_push16(frame, flags);
}

/* POPF — pop flags from V86 stack, protect system bits */
static void v86_emulate_popf(struct v86_frame *frame) {
    uint16_t flags = v86_stack_pop16(frame);
    /* V86 task can modify: CF, PF, AF, ZF, SF, IF, DF, OF
     * Protected: IOPL (stays 0 for I/O trapping), NT, TF (no single-step),
     * VM (stays 1). We virtualize IOPL: PUSHF shows 3, real stays 0. */
    uint32_t mask = 0x00000ED5;  /* bits 0-7,9-11: no TF(8), no IOPL(12-13), no NT(14) */
    frame->eflags = (frame->eflags & ~mask) | (flags & mask);
    frame->eflags |= 0x20000 | 0x02;  /* keep VM bit + reserved bit 1 */
}

/* IRET — pop IP, CS, FLAGS from V86 stack */
static void v86_emulate_iret(struct v86_frame *frame) {
    frame->eip    = v86_stack_pop16(frame);
    frame->cs     = v86_stack_pop16(frame);
    uint16_t flags = v86_stack_pop16(frame);

    uint32_t mask = 0x00000FD5;
    frame->eflags = (frame->eflags & ~mask) | (flags & mask);
    frame->eflags |= 0x20000 | 0x02;
}

/* Protect A20 gate — V86 programs must NOT disable A20.
 * Port 0x92 bit 1 = A20 enable. Ensure it stays set.
 * Port 0x64 commands 0xDD (disable A20) / 0xDF (enable A20).
 * We intercept both methods. */
static uint8_t v86_a20_kbc_cmd = 0;  /* last keyboard controller command */

static uint8_t v86_filter_port_out(uint16_t port, uint8_t val) {
    if (port == 0x92) {
        val |= 0x02;   /* force A20 enabled */
        val &= 0xFE;   /* don't trigger reset (bit 0) */
    }
    if (port == 0x64) {
        v86_a20_kbc_cmd = val;
        if (val == 0xDD) {
            /* Disable A20 via KBC — block it */
            return val;  /* let the command through but... */
        }
    }
    if (port == 0x60 && v86_a20_kbc_cmd == 0xD1) {
        /* KBC output port write — bit 1 = A20.
         * Force bit 1 set to keep A20 enabled. */
        val |= 0x02;
    }
    return val;
}

/* s42 — port 0x60 IN returns the cached scancode (consumed by our
 * kernel ISR). port 0x64 IN: emulate the keyboard controller status
 * byte; bit 0 = output buffer full (data available). Most V86 ISRs
 * read 0x64 first then 0x60 if data ready. We always claim "data
 * ready" since v86_last_scancode is meaningful and the V86 ISR is
 * running in direct response to IRQ 1 anyway. */
extern volatile uint8_t v86_last_scancode;
static uint8_t v86_read_kbd_port(uint16_t port) {
    if (port == 0x60) return v86_last_scancode;
    if (port == 0x64) return 0x01;  /* output buffer full */
    return inb(port);
}

/* I/O port emulation — pass through with A20 protection */
static void v86_emulate_io(struct v86_frame *frame, uint8_t opcode) {
    uint16_t port = frame->edx & 0xFFFF;

    switch (opcode) {
    case 0xEC: /* IN AL, DX */
        frame->eax = (frame->eax & 0xFFFFFF00) | v86_read_kbd_port(port);
        break;
    case 0xED: /* IN AX, DX */
        frame->eax = (frame->eax & 0xFFFF0000) | inw(port);
        break;
    case 0xEE: /* OUT DX, AL */
        outb(port, v86_filter_port_out(port, frame->eax & 0xFF));
        break;
    case 0xEF: /* OUT DX, AX */
        outw(port, frame->eax & 0xFFFF);
        break;
    }
}

static void v86_emulate_io_imm(struct v86_frame *frame, uint8_t opcode, uint8_t port) {
    switch (opcode) {
    case 0xE4: /* IN AL, imm8 */
        frame->eax = (frame->eax & 0xFFFFFF00) | v86_read_kbd_port(port);
        break;
    case 0xE5: /* IN AX, imm8 */
        frame->eax = (frame->eax & 0xFFFF0000) | inw(port);
        break;
    case 0xE6: /* OUT imm8, AL */
        outb(port, v86_filter_port_out(port, frame->eax & 0xFF));
        break;
    case 0xE7: /* OUT imm8, AX */
        outw(port, frame->eax & 0xFFFF);
        break;
    }
}

/* ================================================================
 * GPF handler — main dispatch
 * ================================================================ */

void v86_gpf_handler(struct v86_frame *frame) {
    static uint32_t gpf_count = 0;
    uint32_t code_addr;
    uint8_t *code;
    uint8_t opcode;
    int prefix_66 = 0;  /* operand size prefix */
    int prefix_len = 0; /* total prefix bytes to skip */

    gpf_count++;
    code_addr = (frame->cs << 4) + (frame->eip & 0xFFFF);
    code = (uint8_t *)code_addr;

    if (gpf_count <= 4 || gpf_count == 100 || gpf_count == 500 ||
        gpf_count == 1000 || (gpf_count & 0x3FFF) == 0) {
        serial_puts("V86: GPF #");
        serial_puthex(gpf_count);
        serial_puts(" CS:IP=");
        serial_puthex(frame->cs);
        serial_puts(":");
        serial_puthex(frame->eip);
        serial_puts(" code=");
        for (int i = 0; i < 6; i++) { serial_puthex(code[i]); serial_puts(" "); }
        serial_puts("\n  EAX="); serial_puthex(frame->eax);
        serial_puts(" EBX="); serial_puthex(frame->ebx);
        serial_puts(" ECX="); serial_puthex(frame->ecx);
        serial_puts(" EDX="); serial_puthex(frame->edx);
        serial_puts("\n  ESI="); serial_puthex(frame->esi);
        serial_puts(" EDI="); serial_puthex(frame->edi);
        serial_puts(" EBP="); serial_puthex(frame->ebp);
        serial_puts(" SS:ESP="); serial_puthex(frame->ss);
        serial_puts(":"); serial_puthex(frame->esp);
        serial_puts("\n  DS="); serial_puthex(frame->v86_ds);
        serial_puts(" ES="); serial_puthex(frame->v86_es);
        serial_puts(" EFL="); serial_puthex(frame->eflags);
        serial_puts("\n");
    }

    /* Skip prefixes: segment overrides, operand/address size, REP, LOCK */
    opcode = code[0];
    while (1) {
        if (opcode == 0x66) {
            prefix_66 = 1;
        } else if (opcode == 0x67 ||            /* address size */
                   opcode == 0x26 ||            /* ES: */
                   opcode == 0x2E ||            /* CS: */
                   opcode == 0x36 ||            /* SS: */
                   opcode == 0x3E ||            /* DS: */
                   opcode == 0x64 ||            /* FS: */
                   opcode == 0x65 ||            /* GS: */
                   opcode == 0xF0 ||            /* LOCK */
                   opcode == 0xF2 ||            /* REPNE */
                   opcode == 0xF3) {            /* REP/REPE */
            /* skip */
        } else {
            break;
        }
        prefix_len++;
        opcode = code[prefix_len];
    }

    switch (opcode) {

    case 0xCC: /* INT 3 (breakpoint) — 1-byte instruction */
    {
        uint32_t lin = (frame->cs << 4) + (frame->eip & 0xFFFF);
        serial_puts("\n!!! V86 BREAKPOINT at ");
        serial_puthex(frame->cs);
        serial_puts(":");
        serial_puthex(frame->eip);
        serial_puts(" (lin=");
        serial_puthex(lin);
        serial_puts(")\n  EAX="); serial_puthex(frame->eax);
        serial_puts(" EBX="); serial_puthex(frame->ebx);
        serial_puts(" ECX="); serial_puthex(frame->ecx);
        serial_puts(" EDX="); serial_puthex(frame->edx);
        serial_puts("\n  ESI="); serial_puthex(frame->esi);
        serial_puts(" EDI="); serial_puthex(frame->edi);
        serial_puts(" EBP="); serial_puthex(frame->ebp);
        serial_puts("\n  SS:ESP=");
        serial_puthex(frame->ss);
        serial_puts(":");
        serial_puthex(frame->esp);
        serial_puts(" DS=");
        serial_puthex(frame->v86_ds);
        serial_puts(" ES=");
        serial_puthex(frame->v86_es);
        serial_puts("\n");
        /* Don't advance EIP — halt for inspection */
        __asm__ volatile("cli; hlt");
        break;
    }

    case 0xCD: /* INT nn */
        v86_frame_redirected = 0;
        v86_emulate_int(frame, code[prefix_len + 1]);
        if (!v86_frame_redirected)
            frame->eip += 2 + prefix_len;
        break;

    case 0xFA: /* CLI */
        frame->eflags &= ~0x200; /* clear virtual IF */
        frame->eip += 1 + prefix_len;
        break;

    case 0xFB: /* STI */
        frame->eflags |= 0x200;  /* set virtual IF */
        frame->eip += 1 + prefix_len;
        break;

    case 0x9C: /* PUSHF */
        if (prefix_66) {
            /* PUSHFD — push 32-bit flags */
            frame->esp = (frame->esp & 0xFFFF0000) | ((frame->esp - 4) & 0xFFFF);
            uint32_t addr = (frame->ss << 4) + (frame->esp & 0xFFFF);
            *(uint32_t *)addr = frame->eflags & 0x00FFFFFF;
        } else {
            v86_emulate_pushf(frame);
        }
        frame->eip += 1 + prefix_len;
        break;

    case 0x9D: /* POPF */
        if (prefix_66) {
            /* POPFD — pop 32-bit flags. IOPL stays 0 (I/O traps).
             * TF stays 0 (no single-step exceptions). */
            uint32_t addr = (frame->ss << 4) + (frame->esp & 0xFFFF);
            uint32_t flags = *(uint32_t *)addr;
            frame->esp = (frame->esp & 0xFFFF0000) | ((frame->esp + 4) & 0xFFFF);
            uint32_t mask = 0x00000ED5;  /* bits 0-11, no IOPL/NT/TF */
            frame->eflags = (frame->eflags & ~mask) | (flags & mask);
            frame->eflags |= 0x20000 | 0x02;
        } else {
            v86_emulate_popf(frame);
        }
        frame->eip += 1 + prefix_len;
        break;

    case 0xCF: /* IRET */
        v86_emulate_iret(frame);
        if (prefix_66)
            frame->eip += 0; /* iret sets EIP */
        break;

    case 0xEC: case 0xED:  /* IN from DX */
    case 0xEE: case 0xEF:  /* OUT to DX */
        v86_emulate_io(frame, opcode);
        frame->eip += 1 + prefix_len;
        break;

    case 0xE4: case 0xE5:  /* IN from imm8 */
    case 0xE6: case 0xE7:  /* OUT to imm8 */
        v86_emulate_io_imm(frame, opcode, code[prefix_len + 1]);
        frame->eip += 2 + prefix_len;
        break;

    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        /* String I/O: INSB/INSW (6C/6D) and OUTSB/OUTSW (6E/6F).
         * V86 traps these without an IOPL bitmap. Emulate a single
         * iteration (REP is handled via the next GPF since the prefix
         * sits in front of the opcode and we already consumed it). */
        uint16_t port = frame->edx & 0xFFFF;
        uint16_t ds   = frame->v86_ds;
        uint16_t es   = frame->v86_es;
        uint32_t df   = (frame->eflags & 0x400) ? -1 : 1;  /* DF=1 means decrement */
        if (opcode == 0x6C) {
            /* INSB → [ES:DI] */
            uint8_t  v = inb(port);
            uint8_t *p = (uint8_t *)((uint32_t)es * 16 + (frame->edi & 0xFFFF));
            *p = v;
            frame->edi = (frame->edi & 0xFFFF0000) | ((frame->edi + df) & 0xFFFF);
        } else if (opcode == 0x6D) {
            uint16_t v = inw(port);
            uint16_t *p = (uint16_t *)((uint32_t)es * 16 + (frame->edi & 0xFFFF));
            *p = v;
            frame->edi = (frame->edi & 0xFFFF0000) | ((frame->edi + 2*df) & 0xFFFF);
        } else if (opcode == 0x6E) {
            /* OUTSB ← [DS:SI] */
            uint8_t *p = (uint8_t *)((uint32_t)ds * 16 + (frame->esi & 0xFFFF));
            outb(port, v86_filter_port_out(port, *p));
            frame->esi = (frame->esi & 0xFFFF0000) | ((frame->esi + df) & 0xFFFF);
        } else {
            uint16_t *p = (uint16_t *)((uint32_t)ds * 16 + (frame->esi & 0xFFFF));
            outw(port, *p);
            frame->esi = (frame->esi & 0xFFFF0000) | ((frame->esi + 2*df) & 0xFFFF);
        }
        frame->eip += 1 + prefix_len;
        break;
    }

    case 0x0F: {
        /* Two-byte opcodes — privileged instructions used by DOS extenders */
        uint8_t op2 = code[prefix_len + 1];

        if (op2 == 0x20) {
            /* MOV reg, CRn — read control register */
            uint8_t modrm = code[prefix_len + 2];
            uint8_t cr = (modrm >> 3) & 7;
            uint8_t reg = modrm & 7;
            uint32_t val = 0;
            if (cr == 0) __asm__ volatile("mov %%cr0, %0" : "=r"(val));
            else if (cr == 2) __asm__ volatile("mov %%cr2, %0" : "=r"(val));
            else if (cr == 3) __asm__ volatile("mov %%cr3, %0" : "=r"(val));
            switch (reg) {
            case 0: frame->eax = val; break;
            case 1: frame->ecx = val; break;
            case 2: frame->edx = val; break;
            case 3: frame->ebx = val; break;
            case 6: frame->esi = val; break;
            case 7: frame->edi = val; break;
            }
            frame->eip += 3 + prefix_len;
        } else if (op2 == 0x22) {
            /* MOV CRn, reg — write control register.
             * We can't let V86 code modify CR0/CR3 — just skip. */
            serial_puts("V86: MOV CR, reg ignored\n");
            frame->eip += 3 + prefix_len;
        } else if (op2 == 0x21) {
            /* MOV reg, DRn — read debug register */
            uint8_t modrm = code[prefix_len + 2];
            uint8_t reg = modrm & 7;
            /* Return 0 for all debug registers */
            switch (reg) {
            case 0: frame->eax = 0; break;
            case 1: frame->ecx = 0; break;
            case 2: frame->edx = 0; break;
            case 3: frame->ebx = 0; break;
            case 6: frame->esi = 0; break;
            case 7: frame->edi = 0; break;
            }
            frame->eip += 3 + prefix_len;
        } else if (op2 == 0x23) {
            /* MOV DRn, reg — write debug register. Ignore to prevent
             * V86 code from setting hardware breakpoints. */
            frame->eip += 3 + prefix_len;
        } else if (op2 == 0x01) {
            /* SGDT/SIDT/LGDT/LIDT */
            uint8_t modrm = code[prefix_len + 2];
            uint8_t func = (modrm >> 3) & 7;
            if (func == 0 || func == 1) {
                /* SGDT/SIDT — store GDT/IDT register (6 bytes)
                 * Fake it: return zeros so the extender thinks no GDT/IDT is loaded */
                frame->eip += 3 + prefix_len;  /* skip for now */
            } else if (func == 2 || func == 3) {
                /* LGDT/LIDT — load GDT/IDT. We can't let V86 code do this.
                 * Just skip the instruction — we maintain our own GDT/IDT. */
                serial_puts("V86: LGDT/LIDT ignored (we control GDT/IDT)\n");
                frame->eip += 3 + prefix_len;
            } else {
                frame->eip += 3 + prefix_len;
            }
        } else if (op2 == 0x06) {
            /* CLTS — clear task-switched flag. Skip it. */
            frame->eip += 2 + prefix_len;
        } else if (op2 == 0x09) {
            /* WBINVD — write-back and invalidate cache. Skip it. */
            frame->eip += 2 + prefix_len;
        } else if (op2 == 0x02 || op2 == 0x03 ||
                   (op2 == 0x00 &&
                    (((code[prefix_len + 2] >> 3) & 7) == 4 ||
                     ((code[prefix_len + 2] >> 3) & 7) == 5))) {
            /* s41d — LAR/LSL/VERR/VERW emulation in V86.
             *   0F 02 /r       LAR  r16/r32, r/m16    (load access rights)
             *   0F 03 /r       LSL  r16/r32, r/m16    (load segment limit)
             *   0F 00 /4 r/m16 VERR                   (verify read access)
             *   0F 00 /5 r/m16 VERW                   (verify write access)
             * The 386 raises #UD for these in V86 mode (they're PM-only
             * by CPU rules) — but DOS extenders (DOS/16M observed in s40
             * at CS:IP=0x276F:0x7051) issue LAR from V86 to probe LDT
             * selectors they just allocated via INT 31h. Emulate against
             * the surviving DPMI client's LDT. Register-source only
             * (mod=11); fall through to unhandled for mem-source. */
            uint8_t modrm = code[prefix_len + 2];
            uint8_t mod   = (modrm >> 6) & 3;
            uint8_t reg   = (modrm >> 3) & 7;
            uint8_t rm    = modrm & 7;
            {
                /* s43 debug — first 4 hits per (cs,ip) so we see what
                 * selector DOS/16M is probing. */
                static uint32_t last_lar_csip = 0;
                static int lar_repeats = 0;
                uint32_t csip = ((frame->cs & 0xFFFF) << 16) |
                                (frame->eip & 0xFFFF);
                if (csip != last_lar_csip) {
                    last_lar_csip = csip;
                    lar_repeats = 0;
                }
                if (lar_repeats++ < 4) {
                    serial_puts("V86: LAR/LSL hit op2=");
                    serial_puthex(op2);
                    serial_puts(" modrm=");
                    serial_puthex(code[prefix_len + 2]);
                    serial_puts(" mod=");
                    serial_puthex(mod);
                    serial_puts(" EAX=");
                    serial_puthex(frame->eax);
                    serial_puts(" EBX=");
                    serial_puthex(frame->ebx);
                    serial_puts(" ECX=");
                    serial_puthex(frame->ecx);
                    serial_puts(" EDX=");
                    serial_puthex(frame->edx);
                    serial_puts(" v86=");
                    serial_puthex(current_v86);
                    serial_puts("\n");
                }
            }
            if (mod != 3) {
                /* Memory source — defer. Fall through to skip+unhandled. */
                serial_puts("V86: LAR/LSL/VERR/VERW mem-source not impl op2=");
                serial_puthex(op2);
                serial_puts("\n");
                frame->eip += 3 + prefix_len;
            } else {
                uint16_t src_sel;
                switch (rm) {
                case 0: src_sel = frame->eax & 0xFFFF; break;
                case 1: src_sel = frame->ecx & 0xFFFF; break;
                case 2: src_sel = frame->edx & 0xFFFF; break;
                case 3: src_sel = frame->ebx & 0xFFFF; break;
                case 4: src_sel = frame->esp & 0xFFFF; break;
                case 5: src_sel = frame->ebp & 0xFFFF; break;
                case 6: src_sel = frame->esi & 0xFFFF; break;
                default: src_sel = frame->edi & 0xFFFF; break;
                }

                extern struct dpmi_client *dpmi_find_client_for_v86(int);
                struct dpmi_client *cc =
                    dpmi_find_client_for_v86(current_v86);
                uint16_t idx = src_sel / 8;
                int ti       = (src_sel & 4) != 0;
                int valid    = (cc && ti && idx != 0 &&
                                idx < DPMI_LDT_ENTRIES &&
                                LDT_USED(cc, idx));

                /* Default: ZF=0 (selector invalid / inaccessible) */
                int zf = 0;
                uint32_t result = 0;

                if (valid) {
                    struct seg_descriptor *d = &cc->ldt[idx];
                    uint8_t access  = d->access;
                    uint8_t limit_hi = d->limit_hi;

                    if (op2 == 0x02) {
                        /* LAR — load access rights. Intel format:
                         * dest[15:8] = byte 5 (access). For 32-bit
                         * dest, dest[23:16] = byte 6 upper nibble
                         * (G/D/B/L/AVL). dest[7:0] = 0. */
                        result = (uint32_t)access << 8;
                        if (prefix_66) {
                            result |= (uint32_t)(limit_hi & 0xF0) << 16;
                        }
                        zf = 1;
                    } else if (op2 == 0x03) {
                        /* LSL — load segment limit. 32-bit limit value;
                         * if G=1, expand to (limit << 12) | 0xFFF. */
                        uint32_t limit = d->limit_lo |
                                         ((uint32_t)(limit_hi & 0x0F) << 16);
                        if (limit_hi & 0x80) {
                            limit = (limit << 12) | 0xFFF;
                        }
                        result = prefix_66 ? limit : (limit & 0xFFFF);
                        zf = 1;
                    } else { /* op2 == 0x00 — VERR or VERW */
                        /* Memory segment: byte 5 bit 4 (S) = 1; system
                         * descriptors fail. For VERR: must be data
                         * (type bit 3 = 0) OR code with R bit set
                         * (type bit 1 = 1). For VERW: must be writable
                         * data (type bits 3=0, 1=1). DPL check vs
                         * effective CPL: V86 caller has CPL=3; LDT
                         * descriptors are DPL=3 → always accessible. */
                        if (access & 0x10) { /* S=1 memory */
                            uint8_t type = access & 0x0F;
                            if (reg == 4) { /* VERR */
                                if (!(type & 0x08))      zf = 1; /* data */
                                else if (type & 0x02)    zf = 1; /* readable code */
                            } else {        /* VERW */
                                if (!(type & 0x08) && (type & 0x02))
                                    zf = 1; /* writable data */
                            }
                        }
                    }
                }

                /* Write LAR/LSL result to destination register (reg field) */
                if (op2 == 0x02 || op2 == 0x03) {
                    uint32_t mask = prefix_66 ? 0 : 0xFFFF0000;
                    uint32_t val_keep_hi = (op2 == 0x02 && !prefix_66)
                                            ? 0xFFFF0000 : mask;
                    switch (reg) {
                    case 0: frame->eax = (frame->eax & val_keep_hi) |
                                          (result & ~val_keep_hi); break;
                    case 1: frame->ecx = (frame->ecx & val_keep_hi) |
                                          (result & ~val_keep_hi); break;
                    case 2: frame->edx = (frame->edx & val_keep_hi) |
                                          (result & ~val_keep_hi); break;
                    case 3: frame->ebx = (frame->ebx & val_keep_hi) |
                                          (result & ~val_keep_hi); break;
                    case 4: frame->esp = (frame->esp & val_keep_hi) |
                                          (result & ~val_keep_hi); break;
                    case 5: frame->ebp = (frame->ebp & val_keep_hi) |
                                          (result & ~val_keep_hi); break;
                    case 6: frame->esi = (frame->esi & val_keep_hi) |
                                          (result & ~val_keep_hi); break;
                    case 7: frame->edi = (frame->edi & val_keep_hi) |
                                          (result & ~val_keep_hi); break;
                    }
                }

                /* Set/clear ZF in EFLAGS (bit 6) */
                if (zf) frame->eflags |= 0x40;
                else    frame->eflags &= ~0x40;

                frame->eip += 3 + prefix_len;
            }
        } else {
            serial_puts("V86: unhandled 0F ");
            serial_puthex(op2);
            serial_puts("h at ");
            serial_puthex(frame->cs);
            serial_puts(":");
            serial_puthex(frame->eip);
            serial_puts("\n");
            frame->eip += 2 + prefix_len;
        }
        break;
    }

    case 0xF4: {
        /* HLT — privileged in V86 (always #GPs at IOPL<3). Real-mode
         * semantics: wait for next interrupt. In our cooperative
         * monitor, just advance EIP — the scheduler's PIT preemption
         * will wake the task on the next tick. Used by the s51 Path B
         * keyboard polling stub between INT 16h polls so it doesn't
         * burn the CPU in a tight loop. */
        frame->eip += 1 + prefix_len;
        break;
    }

    default:
        serial_puts("V86: unhandled opcode 0x");
        serial_puthex(opcode);
        serial_puts(" at ");
        serial_puthex(frame->cs);
        serial_puts(":");
        serial_puthex(frame->eip);
        serial_puts(" task=");
        serial_puthex(current_v86);
        serial_puts(" bytes=");
        {
            uint32_t lin = ((uint32_t)frame->cs << 4) + (frame->eip & 0xFFFF);
            uint8_t *cb = (uint8_t *)lin;
            int bi;
            for (bi = 0; bi < 8; bi++) {
                serial_puthex(cb[bi]);
                serial_puts(" ");
            }
        }
        serial_puts("\n");
        /* If running a child process, treat as crash — return to parent */
        if (current_v86 >= 0) {
            int dtid = v86_tasks[current_v86].dos_task_id;
            extern struct dos_task *dos_get_task(int id);
            struct dos_task *dt = dos_get_task(dtid);
            if (dt && dt->parent.active) {
                serial_puts("V86: child crashed, returning to parent\n");
                v86_frame_redirected = 1;
                dt->return_code = 0xFF;
                dt->parent.active = 0;
                /* Restore parent frame */
                frame->cs     = dt->parent.cs;
                frame->eip    = dt->parent.eip;
                frame->ss     = dt->parent.ss;
                frame->esp    = dt->parent.esp;
                frame->v86_ds = dt->parent.ds;
                frame->v86_es = dt->parent.es;
                frame->eax    = dt->parent.eax & 0xFFFF0000; /* AX=0 */
                frame->ebx    = dt->parent.ebx;
                frame->ecx    = dt->parent.ecx;
                frame->edx    = dt->parent.edx;
                frame->esi    = dt->parent.esi;
                frame->edi    = dt->parent.edi;
                frame->eflags = dt->parent.eflags | 0x20000 | 0x02;
                dt->psp_seg = dt->parent.psp_seg;
                dt->dta_seg = dt->parent.dta_seg;
                dt->dta_off = dt->parent.dta_off;
                dt->next_alloc_seg = dt->parent.next_alloc_seg;
                break;
            }
            serial_puts("V86: task killed\n");
            current_v86 = -1;
            sched_v86_exit();  /* never returns */
        }
        break;
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

void v86_set_current(int task_id) {
    current_v86 = task_id;
}

void v86_set_v86mt_owner(int task_id, int client_id, int vt_handle) {
    if (task_id < 0 || task_id >= V86_MAX_TASKS) return;
    v86_tasks[task_id].v86mt_client_id = client_id;
    v86_tasks[task_id].v86mt_vt_handle = vt_handle;
}

int v86_current_v86mt_client(void) {
    if (current_v86 < 0 || current_v86 >= V86_MAX_TASKS) return -1;
    if (!v86_tasks[current_v86].active) return -1;
    return v86_tasks[current_v86].v86mt_client_id;
}

int v86_current_v86mt_handle(void) {
    if (current_v86 < 0 || current_v86 >= V86_MAX_TASKS) return 0;
    if (!v86_tasks[current_v86].active) return 0;
    return v86_tasks[current_v86].v86mt_vt_handle;
}

void v86_init(void) {
    int i;
    for (i = 0; i < V86_MAX_TASKS; i++)
        v86_tasks[i].active = 0;

    current_v86 = -1;

    /* Set up V86 IVT — fill low memory with IRET landing pad,
     * then point IVT entries to safe stubs.
     *
     * Pure mode: clean ALL 256 vectors — no FreeDOS residue.
     * DOS mode:  clean only emulated vectors (00-0F, 10-1A). */
    {
        /* IRET landing pad at 0x0600-0x0FFF */
        uint8_t *pad = (uint8_t *)0x0600;
        uint32_t fi;
        for (fi = 0; fi < 0x1000 - 0x0600; fi++)
            pad[fi] = 0xCF;  /* IRET */

#ifdef KERNEL_MODE_PURE
        /* Pure mode: zero the IVT and set ALL 256 entries to IRET stubs.
         * This guarantees no stale BIOS/FreeDOS handlers exist.
         * Preserve: 0x400-0x4FF (BIOS data area), 0x480+ (EMS stub),
         * 0x500-0x503 (DPMI entry stub). */
        for (fi = 0; fi < 256; fi++) {
            uint16_t *ivt = (uint16_t *)(fi * 4);
            ivt[0] = 0x0600;  /* offset = IRET pad */
            ivt[1] = 0x0000;  /* segment 0 */
        }
        /* Also fill stale FreeDOS kernel area with IRETs.
         * FreeDOS kernel lives at ~0xD80-0xFFFF. DOS extenders save
         * handler addresses from this region and chain to them.
         * By filling with IRETs, any chain call returns immediately. */
        {
            uint8_t *stale = (uint8_t *)0x0D80;
            for (fi = 0; fi < 0x10000 - 0x0D80; fi++)
                stale[fi] = 0xCF;  /* IRET */
        }
        /* Populate BIOS Data Area (0x40:00xx). V86 DOS apps read screen
         * dimensions from BDA rather than via INT 10h (e.g. FreeDOS EDIT
         * via DFlat+ reads peekb(0x40,0x4A) for cols, peekb(0x40,0x84)
         * for rows-1). Without this they see zeros and silently skip
         * every draw. Word writes are byte-pair to avoid alignment-
         * rounding aliasing the next field. */
        {
            volatile uint8_t *bda = (volatile uint8_t *)0x400;
            bda[0x49] = 0x03;            /* current video mode = 80x25 color */
            bda[0x4A] = 80;  bda[0x4B] = 0;     /* screen columns = 80 */
            bda[0x4C] = 0x00; bda[0x4D] = 0x10; /* regen buffer size = 0x1000 */
            bda[0x4E] = 0;   bda[0x4F] = 0;     /* current page offset = 0 */
            bda[0x62] = 0;                /* active display page */
            bda[0x63] = 0xD4; bda[0x64] = 0x03; /* CRTC base port = 0x03D4 */
            bda[0x84] = 24;               /* rows on screen minus 1 = 24 */
            bda[0x85] = 16;  bda[0x86] = 0;     /* character cell height = 16 */
            bda[0x87] = 0x60;             /* EGA/VGA info: 256K VRAM, color */
            bda[0x96] = 0x10;             /* keyboard status flag 3: bit 4 = 101/102 enhanced kbd installed.
                                           * DFlat+'s getshift() reads this to choose AH=0x02 vs AH=0x12. */
        }
        /* Install a real RM INT 21h stub at 0:0x550 and point IVT[0x21]
         * at it. The bytes `CD F5 CF` mean: INT 0xF5 (our private "DOS
         * service" vector), then IRET. When DOS/4GW uses INT 31h/0302
         * to chain RM INT 21h, the trampoline lands here; INT 0xF5
         * traps to the V86 monitor which calls dos_int21() with the V86
         * regs, writes results back, then IRET pops the original frame.
         *
         * Also install stub at 0:0x554 for INT 0x10 (BIOS video) → INT
         * 0xF6, and 0:0x558 for INT 0x16 (keyboard) → INT 0xF7. Lets
         * DOS/4GW chain through BIOS via 0x302 with real service. */
        {
            uint8_t *s = (uint8_t *)0x550;
            s[0] = 0xCD; s[1] = 0xF5; s[2] = 0xCF;   /* INT 0xF5 ; IRET */
            s[4] = 0xCD; s[5] = 0xF6; s[6] = 0xCF;   /* INT 0xF6 ; IRET (BIOS video) */
            s[8] = 0xCD; s[9] = 0xF7; s[10] = 0xCF;  /* INT 0xF7 ; IRET (BIOS kbd) */

            uint16_t *ivt = (uint16_t *)0;
            ivt[0x21*2 + 0] = 0x0550; ivt[0x21*2 + 1] = 0;
            ivt[0x10*2 + 0] = 0x0554; ivt[0x10*2 + 1] = 0;
            ivt[0x16*2 + 0] = 0x0558; ivt[0x16*2 + 1] = 0;
        }
        serial_puts("V86: PURE mode — IVT [0x21/0x10/0x16] → emulated stubs, others → IRET, BDA stamped 80x25\n");
#else
        /* DOS mode: only fix vectors we emulate + old PIC vectors.
         * Preserve FreeDOS handlers for FREECOM compatibility. */
        {
            static const uint8_t fix_vecs[] = {
                0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
            };
            for (fi = 0; fi < sizeof(fix_vecs); fi++) {
                uint16_t *ivt = (uint16_t *)(fix_vecs[fi] * 4);
                ivt[0] = 0x0600;
                ivt[1] = 0x0000;
            }
        }
        serial_puts("V86: DOS mode — 26 IVT vectors fixed\n");
#endif
    }

    serial_puts("V86: monitor ready\n");
}

int v86_create_task(void) {
    int i;
    uint32_t stack_page;

    for (i = 0; i < V86_MAX_TASKS; i++) {
        if (!v86_tasks[i].active) {
            int dos_tid;

            /* Allocate Ring 0 stack for this task */
            stack_page = pmm_alloc_page();
            if (!stack_page) return -1;

            v86_tasks[i].active = 1;
            v86_tasks[i].ring0_stack = stack_page + PAGE_SIZE; /* top of stack */
            v86_tasks[i].cursor_x = 0;
            v86_tasks[i].cursor_y = 0;
            v86_tasks[i].v86mt_client_id = -1;
            v86_tasks[i].v86mt_vt_handle = 0;

            /* Create corresponding DOS task */
            dos_tid = dos_create_task();
            v86_tasks[i].dos_task_id = dos_tid;

            /* Clear text buffer */
            {
                int j;
                for (j = 0; j < 80 * 25; j++) {
                    v86_tasks[i].text_buf[j * 2] = ' ';
                    v86_tasks[i].text_buf[j * 2 + 1] = 0x07;
                }
            }

            serial_puts("V86: task ");
            serial_puthex(i);
            serial_puts(" created, ring0 stack=");
            serial_puthex(v86_tasks[i].ring0_stack);
            serial_puts("\n");

            return i;
        }
    }
    return -1;
}

void v86_destroy_task(int task_id) {
    if (task_id < 0 || task_id >= V86_MAX_TASKS) return;
    if (!v86_tasks[task_id].active) return;

    /* V86MT (M4) — dump the shadow buffer to serial so we can verify
     * the V86 task's INT 21h output reached the V86MT VT. The full
     * buffer view becomes visible to the PM client once M5 LDT-maps it. */
    if (v86_tasks[task_id].v86mt_client_id >= 0 &&
        v86_tasks[task_id].v86mt_vt_handle > 0) {
        extern struct dpmi_v86mt_vt *v86mt_vt_get(int client_id, uint16_t handle);
        struct dpmi_v86mt_vt *v = v86mt_vt_get(
            v86_tasks[task_id].v86mt_client_id,
            (uint16_t)v86_tasks[task_id].v86mt_vt_handle);
        if (v && v->char_buf) {
            serial_puts("V86MT: VT buffer at exit (cursor=");
            serial_puthex(v->cursor_x);
            serial_puts(",");
            serial_puthex(v->cursor_y);
            serial_puts(" dirty=");
            serial_puthex(v->screen_dirty);
            serial_puts("):\n\"");
            uint32_t end = (uint32_t)v->cursor_y * v->cols + v->cursor_x;
            for (uint32_t i = 0; i < end && i < 200; i++) {
                uint8_t b = v->char_buf[i];
                if (b == '\n') serial_puts("\\n");
                else if (b == '\r') serial_puts("\\r");
                else if (b >= 0x20 && b < 0x7F) { char s[2] = {(char)b,0}; serial_puts(s); }
                else serial_puts("?");
            }
            serial_puts("\"\n");
            /* M5 lifecycle mirror — flag the task as exited so vt_poll
             * reports it on the next client poll. */
            v->task_running = 0;
            v->exited       = 1;
            v->exit_code    = 0;
            v->screen_dirty++;
        }
    }

    dos_destroy_task(v86_tasks[task_id].dos_task_id);

    if (v86_tasks[task_id].ring0_stack)
        pmm_free_page(v86_tasks[task_id].ring0_stack - PAGE_SIZE);

    v86_tasks[task_id].active = 0;

    if (current_v86 == task_id)
        current_v86 = -1;

    serial_puts("V86: task ");
    serial_puthex(task_id);
    serial_puts(" destroyed\n");
}

/* Enter V86 mode at seg:off
 *
 * This works by crafting an IRET frame on the stack with VM bit set.
 * When IRET executes, the CPU enters V86 mode.
 *
 * WARNING: This does not return until the V86 task exits!
 * For preemptive multitasking, the timer ISR will switch away.
 */
struct v86_task *v86_get_task_ptr(int task_id) {
    if (task_id < 0 || task_id >= V86_MAX_TASKS) return 0;
    if (!v86_tasks[task_id].active) return 0;
    return &v86_tasks[task_id];
}

int v86_get_dos_task(int task_id) {
    if (task_id < 0 || task_id >= V86_MAX_TASKS) return -1;
    if (!v86_tasks[task_id].active) return -1;
    return v86_tasks[task_id].dos_task_id;
}

void v86_start(int task_id, uint16_t seg, uint16_t off) {
    if (task_id < 0 || task_id >= V86_MAX_TASKS) return;
    if (!v86_tasks[task_id].active) return;

    current_v86 = task_id;
    /* TSS.ESP0 is managed by the scheduler — don't set it here */

    serial_puts("V86: starting task at ");
    serial_puthex(seg);
    serial_puts(":");
    serial_puthex(off);
    serial_puts("\n");

    /* Enter V86 mode via IRET — never returns.
     * On exit, GPF handler calls sched_v86_exit().
     *
     * For a COM file: CS=DS=ES=SS=seg, IP=off, SP=0xFFFE
     * Push order: GS, FS, DS, ES, SS, ESP, EFLAGS(+VM), CS, EIP
     */
    {
        uint32_t v_seg = seg;
        uint32_t v_off = off;
        __asm__ volatile (
            "cli\n"
            "pushl %0\n"         /* GS = seg */
            "pushl %0\n"         /* FS = seg */
            "pushl %0\n"         /* DS = seg */
            "pushl %0\n"         /* ES = seg */
            "pushl %0\n"         /* SS = seg */
            "pushl $0xFFFE\n"    /* ESP */
            "pushl $0x20202\n"   /* EFLAGS: VM=1, IF=1, bit1=1 */
            "pushl %0\n"         /* CS = seg */
            "pushl %1\n"         /* EIP = off */
            "iret\n"
            :
            : "r"(v_seg), "r"(v_off)
            : "memory"
        );
    }
    /* never reached */
}

void v86_start_exe(int task_id, uint16_t cs, uint16_t ip,
                   uint16_t ss, uint16_t sp, uint16_t ds) {
    if (task_id < 0 || task_id >= V86_MAX_TASKS) return;
    if (!v86_tasks[task_id].active) return;

    current_v86 = task_id;
    /* TSS.ESP0 is managed by the scheduler — don't set it here */

    serial_puts("V86: starting EXE at ");
    serial_puthex(cs);
    serial_puts(":");
    serial_puthex(ip);
    serial_puts(" SS=");
    serial_puthex(ss);
    serial_puts(":"); serial_puthex(sp);
    serial_puts(" DS="); serial_puthex(ds);
    serial_puts("\n");

    /* Enter V86 mode via IRET — never returns.
     * On exit, GPF handler calls sched_v86_exit(). */
    {
        uint32_t v_cs = cs, v_ip = ip;
        uint32_t v_ss = ss, v_sp = sp;
        uint32_t v_ds = ds;
        __asm__ volatile (
            "cli\n"
            "pushl %4\n"         /* GS = ds */
            "pushl %4\n"         /* FS = ds */
            "pushl %4\n"         /* DS = ds (PSP segment) */
            "pushl %4\n"         /* ES = ds (PSP segment) */
            "pushl %2\n"         /* SS */
            "pushl %3\n"         /* ESP */
            "pushl $0x20202\n"   /* EFLAGS: VM=1, IF=1, bit1=1 */
            "pushl %0\n"         /* CS */
            "pushl %1\n"         /* EIP */
            "iret\n"
            :
            : "r"(v_cs), "r"(v_ip), "r"(v_ss), "r"(v_sp), "r"(v_ds)
            : "memory"
        );
    }
    /* never reached */
}
