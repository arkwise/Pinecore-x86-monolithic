/* dos.c -- DOS INT 21h emulation layer
 *
 * Handles ~40 INT 21h subfunctions needed for FREECOM compatibility.
 * Each task has its own file handles, CWD, and DTA.
 * (ch-09)
 */

#include "types.h"
#include "dos.h"
#include "fat.h"
#include "serial.h"
#include "vga.h"
#include "keyboard.h"
#include "comload.h"
#include "exeload.h"
#include "rtc.h"
#include "sched.h"
#include "v86.h"
#include "dpmi.h"
#include "vt.h"

/* ================================================================
 * V86 memory access helpers
 *
 * First 1MB is identity-mapped with PTE_USER, so seg:off -> linear
 * is just (seg << 4) + (off & 0xFFFF). We can read/write directly.
 * ================================================================ */

static inline uint8_t *v86_ptr(uint16_t seg, uint16_t off) {
    return (uint8_t *)((uint32_t)(seg << 4) + off);
}

/* Read a $-terminated string from V86 memory */
static void v86_read_dollar_string(uint16_t seg, uint16_t off, char *buf, uint32_t max) {
    uint8_t *src = v86_ptr(seg, off);
    uint32_t i;
    for (i = 0; i < max - 1; i++) {
        if (src[i] == '$') break;
        buf[i] = src[i];
    }
    buf[i] = '\0';
}

/* Read a null-terminated string from V86 memory */
static void v86_read_asciiz(uint16_t seg, uint16_t off, char *buf, uint32_t max) {
    uint8_t *src = v86_ptr(seg, off);
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        buf[i] = src[i];
    buf[i] = '\0';
}

static struct dos_task tasks[DOS_MAX_TASKS];

/* Console I/O function pointers -- set by window system later */
static dos_putchar_fn console_putchar;

/* Phase 4.7 M4 — route output to the V86MT shadow buffer when the
 * current V86 task is V86MT-owned; else fall through to console_putchar
 * (which goes to the kernel-managed VT system). Called from every
 * INT 21h AH=02/05/09/0A/40 site that previously called console_putchar
 * directly. */
static void dos_console_putchar(int task_id, char c) {
    int client_id = v86_current_v86mt_client();
    int handle    = v86_current_v86mt_handle();
    if (client_id >= 0 && handle > 0) {
        struct dpmi_v86mt_vt *v = v86mt_vt_get(client_id, (uint16_t)handle);
        if (v) {
            v86mt_vt_putc(v, (uint8_t)c, 0x07);
            return;
        }
    }
    console_putchar(task_id, c);
}
static dos_getchar_fn console_getchar;
static dos_kbhit_fn   console_kbhit;

/* Default console I/O -- serial port fallback */
static void default_putchar(int task_id, char c) {
    (void)task_id;
    serial_putc(c);
    vga_putc(c);
}

/* s42 — pending scancode for the two-call extended-key protocol.
 * INT 21h AH=01/06/07/08 returns 0 first when an extended key (arrow,
 * F-key, etc.) is pressed, then the scancode on the next call. DOOM
 * SETUP.EXE uses INT 21h AH=08h for navigation; without this
 * protocol the arrows are silently dropped because ascii=0. */
static uint8_t pending_extended_scancode = 0;

static char default_getchar(int task_id) {
    struct key_event ev;

    /* Two-call protocol: if we deferred a scancode last call, return
     * it now without blocking. */
    if (pending_extended_scancode) {
        char sc = (char)pending_extended_scancode;
        pending_extended_scancode = 0;
        return sc;
    }

    /* s42 — when running with VTs, keys are routed to VT buffers
     * (see keyboard.c:271-275), NOT the global ring buffer that
     * keyboard_poll reads. Find the VT associated with this DOS
     * task (via its V86 task → sched task → VT) and poll that.
     * Fall back to the global buffer if no VT is associated (boot
     * shell, headless tools). DOOM SETUP.EXE polls via INT 21h
     * AH=07h — without this, arrows never reach it. */
    /* Use the currently-scheduled task's VT (dos_int21 runs in the
     * V86 task's kernel-thread context). */
    int vt_num = -1;
    {
        struct task *cur = sched_get_task(sched_current());
        if (cur) vt_num = cur->vt;
    }

    __asm__ volatile ("sti");
    while (1) {
        int got = 0;
        if (vt_num >= 0) {
            got = vt_poll_key(vt_num, &ev);
        } else {
            got = keyboard_poll(&ev);
        }
        if (got) {
            if (!ev.pressed) continue;
            if (ev.ascii) return ev.ascii;
            /* Extended key (arrows, F-keys, etc.): ascii=0 but
             * scancode set. First call returns 0; next call returns
             * the scancode. Strip the KEY_EXTENDED flag (bit 7) — DOS
             * expects the raw BIOS scancode. */
            if (ev.scancode) {
                pending_extended_scancode = ev.scancode & ~KEY_EXTENDED;
                return 0;
            }
            continue;
        }
        __asm__ volatile ("hlt");
    }
}

static int default_kbhit(int task_id) {
    /* s42 — also report "key pending" if we have a deferred extended
     * scancode from the previous getchar call. Otherwise the caller
     * skips the second half of the two-call protocol and the
     * scancode never gets delivered. */
    if (pending_extended_scancode) return 1;

    /* s42 — match default_getchar's buffer source: VT of current task
     * if available, fall back to global ring buffer. */
    (void)task_id;
    int vt_num = -1;
    {
        struct task *cur = sched_get_task(sched_current());
        if (cur) vt_num = cur->vt;
    }
    if (vt_num >= 0) {
        struct vt *v = vt_get(vt_num);
        return v && v->key_head != v->key_tail;
    }
    extern volatile uint8_t key_head;
    extern volatile uint8_t key_tail;
    return key_head != key_tail;
}

/* ================================================================
 * Helpers
 * ================================================================ */

/* Set carry flag (error) */
static void set_cf(struct dos_regs *regs) {
    regs->eflags |= 0x01;
}

/* Clear carry flag (success) */
static void clear_cf(struct dos_regs *regs) {
    regs->eflags &= ~0x01;
}

/* Map a DOS handle (0-19) to the global FAT handle */
static int dos_to_fat_handle(struct dos_task *t, int dos_handle) {
    if (dos_handle < 0 || dos_handle >= DOS_MAX_HANDLES)
        return -1;
    if (t->handle_map[dos_handle] == 0xFF)
        return -1;
    return t->handle_map[dos_handle];
}

/* Allocate a DOS handle slot */
static int alloc_dos_handle(struct dos_task *t, int fat_handle) {
    int i;
    /* Skip handles 0-4 (stdin/stdout/stderr/stdaux/stdprn) */
    for (i = 5; i < DOS_MAX_HANDLES; i++) {
        if (t->handle_map[i] == 0xFF) {
            t->handle_map[i] = fat_handle;
            return i;
        }
    }
    return -1;
}

/* ================================================================
 * Public API
 * ================================================================ */

void dos_init(void) {
    int i;
    for (i = 0; i < DOS_MAX_TASKS; i++)
        tasks[i].active = 0;

    console_putchar = default_putchar;
    console_getchar = default_getchar;
    console_kbhit   = default_kbhit;

    /* Set up the initial MCB chain and system PSP, like real DOS.
     * MEM.EXE walks the PSP parent chain to find the first MCB,
     * so we need a proper system PSP at a known segment. */

    /* System MCB at segment 0x0040: owns the BIOS/system area up to our env */
    {
        uint8_t *sys_mcb = (uint8_t *)(0x0040 << 4);  /* linear 0x400 */
        sys_mcb[0] = 'M';                              /* more blocks follow */
        *(uint16_t *)(sys_mcb + 1) = 0x0008;           /* owner: system PSP */
        *(uint16_t *)(sys_mcb + 3) = 0x0FFF - 0x0041;  /* size: up to env MCB */
    }

    /* Fake system PSP at segment 0x0050 (inside the system MCB block).
     * MEM walks parents until parent==self or parent==0x0008. */
    {
        uint8_t *sys_psp = (uint8_t *)(0x0050 << 4);  /* linear 0x500 */
        int k;
        for (k = 0; k < 256; k++) sys_psp[k] = 0;
        sys_psp[0x00] = 0xCD;  /* INT 20h */
        sys_psp[0x01] = 0x20;
        *(uint16_t *)(sys_psp + 0x02) = 0xA000;  /* top of memory */
        *(uint16_t *)(sys_psp + 0x16) = 0x0050;  /* parent = self */
    }

    serial_puts("DOS: INT 21h emulation ready\n");
}

void dos_set_console(dos_putchar_fn putc_fn, dos_getchar_fn getc_fn, dos_kbhit_fn kbhit_fn) {
    console_putchar = putc_fn;
    console_getchar = getc_fn;
    console_kbhit   = kbhit_fn;
}

int dos_create_task(void) {
    int i;
    for (i = 0; i < DOS_MAX_TASKS; i++) {
        if (!tasks[i].active) {
            int j;
            tasks[i].active = 1;
            for (j = 0; j < DOS_MAX_HANDLES; j++)
                tasks[i].handle_map[j] = 0xFF;
            /* Handles 0-4 are reserved (console devices) */
            tasks[i].dta_seg = 0;
            tasks[i].dta_off = 0x80;  /* default DTA at PSP+0x80 */
            tasks[i].psp_seg = 0;
            tasks[i].cwd[0] = '/';
            tasks[i].cwd[1] = '\0';
            tasks[i].return_code = 0;
            tasks[i].next_alloc_seg = 0x2000; /* default */
            return i;
        }
    }
    return -1;
}

struct dos_task *dos_get_task(int task_id) {
    if (task_id < 0 || task_id >= DOS_MAX_TASKS) return 0;
    if (!tasks[task_id].active) return 0;
    return &tasks[task_id];
}

uint16_t dos_get_psp(int task_id) {
    if (task_id < 0 || task_id >= DOS_MAX_TASKS) return 0;
    if (!tasks[task_id].active) return 0;
    return tasks[task_id].psp_seg;
}

/* Allocate `paragraphs` worth of conventional memory in the task's free
 * region. Returns the segment, or 0 on failure. Mirrors INT 21h/AH=0x48
 * but is callable from places (like DPMI 0x0100) where we don't have a
 * V86 round-trip available. Cap at 0xA000 (640 KB) — anything above is
 * upper-memory / BIOS / video and not ours to hand out. */
uint16_t dos_alloc_paragraphs(int task_id, uint16_t paragraphs) {
    if (task_id < 0 || task_id >= DOS_MAX_TASKS) return 0;
    if (!tasks[task_id].active) return 0;
    if (paragraphs == 0) return 0;
    uint16_t seg = tasks[task_id].next_alloc_seg;
    uint32_t end = (uint32_t)seg + paragraphs;
    if (end > 0xA000) return 0;
    tasks[task_id].next_alloc_seg = (uint16_t)end;
    return seg;
}

void dos_set_psp(int task_id, uint16_t psp_seg) {
    if (task_id < 0 || task_id >= DOS_MAX_TASKS) return;
    tasks[task_id].psp_seg = psp_seg;
    tasks[task_id].dta_seg = psp_seg;
    tasks[task_id].dta_off = 0x80;
    /* Set alloc pointer after the program segment (PSP + 64KB) */
    tasks[task_id].next_alloc_seg = psp_seg + 0x1000;
}

void dos_destroy_task(int task_id) {
    int i;
    if (task_id < 0 || task_id >= DOS_MAX_TASKS) return;

    /* Close all open file handles */
    for (i = 5; i < DOS_MAX_HANDLES; i++) {
        if (tasks[task_id].handle_map[i] != 0xFF) {
            fat_close(tasks[task_id].handle_map[i]);
            tasks[task_id].handle_map[i] = 0xFF;
        }
    }

    tasks[task_id].active = 0;
}

/* ================================================================
 * INT 21h dispatcher
 * ================================================================ */

int dos_int21(int task_id, struct dos_regs *regs) {
    struct dos_task *t;
    uint8_t ah;

    if (task_id < 0 || task_id >= DOS_MAX_TASKS) return DOS_RESULT_NORMAL;
    t = &tasks[task_id];
    if (!t->active) return DOS_RESULT_NORMAL;

    ah = (regs->eax >> 8) & 0xFF;

    /* Trace key calls */
    if (ah == 0x48 || ah == 0x49 || ah == 0x4A || ah == 0x4C) {
        serial_puts("[21h/");
        serial_puthex(ah);
        if (ah == 0x48) { serial_puts(" BX="); serial_puthex(regs->ebx & 0xFFFF); }
        if (ah == 0x4A) { serial_puts(" ES="); serial_puthex(regs->es & 0xFFFF); serial_puts(" BX="); serial_puthex(regs->ebx & 0xFFFF); }
        serial_puts("] ");
    }
    /* s42 — quick env-MCB integrity check on every INT 21h entry.
     * Tells us which INT 21h call first encounters the corruption,
     * narrowing the corrupter to the V86 instructions between the
     * previous clean check and this one. */
    {
        static int env_corrupted_logged = 0;
        if (!env_corrupted_logged) {
            uint8_t *emcb = (uint8_t *)0xFFF0;
            uint16_t owner = *(uint16_t *)(emcb + 1);
            uint16_t size  = *(uint16_t *)(emcb + 3);
            if (emcb[0] == 'M' && (owner != 0x1100 || size != 0x00FF)) {
                serial_puts("!!! env-MCB corrupted before INT 21h AH=");
                serial_puthex(ah);
                serial_puts(" owner=");
                serial_puthex(owner);
                serial_puts(" size=");
                serial_puthex(size);
                serial_puts("\n");
                env_corrupted_logged = 1;
            }
        }
    }
    /* s42 diag: log console-input AH values (rate-limited) so we can
     * see whether SETUP is calling INT 21h for keyboard. */
    if (ah == 0x01 || ah == 0x06 || ah == 0x07 || ah == 0x08 ||
        ah == 0x0A || ah == 0x0B || ah == 0x0C) {
        static uint32_t cons_cnt = 0;
        if (cons_cnt++ < 60) {
            serial_puts("[21h/kbd AH=");
            serial_puthex(ah);
            if (ah == 0x06) { serial_puts(" DL="); serial_puthex(regs->edx & 0xFF); }
            serial_puts("]\n");
        }
    }

    switch (ah) {

    /* ---- Console I/O ---- */

    case 0x01: {
        /* Read character with echo */
        char c = console_getchar(task_id);
        dos_console_putchar(task_id, c);
        regs->eax = (regs->eax & 0xFFFFFF00) | (uint8_t)c;
        break;
    }

    case 0x02: {
        /* Write character */
        char c = regs->edx & 0xFF;
        dos_console_putchar(task_id, c);
        break;
    }

    case 0x06: {
        /* Direct console I/O */
        uint8_t dl = regs->edx & 0xFF;
        if (dl == 0xFF) {
            /* Input */
            if (console_kbhit(task_id)) {
                char c = console_getchar(task_id);
                regs->eax = (regs->eax & 0xFFFFFF00) | (uint8_t)c;
                regs->eflags &= ~0x40;  /* clear ZF = char available */
            } else {
                regs->eax = (regs->eax & 0xFFFFFF00);
                regs->eflags |= 0x40;   /* set ZF = no char */
            }
        } else {
            /* Output */
            dos_console_putchar(task_id, dl);
        }
        break;
    }

    case 0x09: {
        /* Write '$'-terminated string from DS:DX */
        uint8_t *str = v86_ptr(regs->ds & 0xFFFF, regs->edx & 0xFFFF);
        uint32_t i;
        for (i = 0; str[i] != '$' && i < 4096; i++)
            dos_console_putchar(task_id, str[i]);
        break;
    }

    case 0x0A: {
        /* Buffered input: DS:DX -> buffer
         * Byte 0 = max chars, byte 1 = filled by DOS with actual count
         * Bytes 2+ = the input characters, terminated by CR */
        uint8_t *buf = v86_ptr(regs->ds & 0xFFFF, regs->edx & 0xFFFF);
        uint8_t max_len = buf[0];
        uint8_t count = 0;
        char c;

        while (count < max_len - 1) {
            c = console_getchar(task_id);
            if (c == '\r' || c == '\n') {
                dos_console_putchar(task_id, '\r');
                dos_console_putchar(task_id, '\n');
                break;
            }
            if (c == 8 && count > 0) {  /* backspace */
                count--;
                dos_console_putchar(task_id, 8);
                dos_console_putchar(task_id, ' ');
                dos_console_putchar(task_id, 8);
                continue;
            }
            buf[2 + count] = c;
            dos_console_putchar(task_id, c);
            count++;
        }
        buf[2 + count] = '\r';
        buf[1] = count;
        break;
    }

    case 0x0B: {
        /* Check input status */
        regs->eax = (regs->eax & 0xFFFFFF00) | (console_kbhit(task_id) ? 0xFF : 0x00);
        break;
    }

    case 0x0C: {
        /* Flush input buffer and read */
        uint8_t subfn = regs->eax & 0xFF;
        /* Flush, then call the specified function */
        if (subfn == 0x01 || subfn == 0x06 || subfn == 0x0A) {
            regs->eax = (regs->eax & 0xFFFF0000) | (subfn << 8) | subfn;
            dos_int21(task_id, regs);  /* recurse for the sub-function */
        }
        break;
    }

    case 0x19: {
        /* Get current drive → AL=drive (0=A:, 2=C:) */
        regs->eax = (regs->eax & 0xFFFFFF00) | fat_get_drive();
        break;
    }

    case 0x1A: {
        /* Set DTA: DS:DX=new DTA address */
        t->dta_seg = regs->ds & 0xFFFF;
        t->dta_off = regs->edx & 0xFFFF;
        break;
    }

    case 0x2F: {
        /* Get DTA → ES:BX */
        regs->es = t->dta_seg;
        regs->ebx = (regs->ebx & 0xFFFF0000) | t->dta_off;
        break;
    }

    case 0x33: {
        uint8_t al = regs->eax & 0xFF;
        if (al == 0x00) {
            /* Get break-check flag */
            regs->edx = (regs->edx & 0xFFFFFF00);  /* break=off */
        } else if (al == 0x06) {
            /* Get true (un-SETVER'd) DOS version. We return 5.50 — the
             * value Windows NT/2000/XP's NTVDM reports — because Allegro's
             * dsystem.c detect_os checks _get_dos_version(1) == 0x0532
             * (BL=5, BH=50) as one of the two signals that switch os_type
             * to OSTYPE_WINNT. That in turn makes Allegro's auto-selected
             * mouse driver be mousedrv_winnt (polling, no RM callback) and
             * its timer driver fixed-rate — both of which match what we
             * can deliver today. Without this, install_mouse picks
             * mousedrv_mickeys, registers an RM callback we don't async-
             * dispatch, and mouse_x stays pinned at center.
             *
             * Returning BL=5, BH=50 is honest: we're not pure DOS, we're a
             * DPMI host that runs DJGPP binaries — semantically closer to
             * NTVDM than to MS-DOS 7.10 (which AH=30 still reports). */
            regs->ebx = (regs->ebx & 0xFFFF0000) | 0x3205; /* BL=5, BH=0x32 */
            regs->edx = (regs->edx & 0xFFFF0000) | 0x0000; /* DH=0 rev, DL=0 flags */
        }
        /* AL=01/05/etc — leave unhandled; no client we ship needs them. */
        break;
    }

    case 0x34: {
        /* Get InDOS flag address → ES:BX
         *
         * Critical: DON'T return 0x0050:0x0000. Linear 0x500 holds the
         * DPMI entry stub (CD F1 CF). A DOS extender that calls AH=0x34
         * once and then polls that byte forever would see 0xCD (not 0)
         * and wait for DOS to "stop being busy" indefinitely.
         *
         * Park it at a quiet low-memory address we don't otherwise use.
         * 0x0080:0x0000 = linear 0x800 (inside the IRET-pad region 0x600-
         * 0xFFF — pad is filled with 0xCF, but we overwrite the first byte
         * with 0 here, and the IRET pad doesn't need byte 0x800
         * specifically). */
        regs->es = 0x0080;
        regs->ebx = (regs->ebx & 0xFFFF0000) | 0x0000;
        *v86_ptr(0x0080, 0x0000) = 0;  /* InDOS = 0 (not busy) */
        break;
    }

    /* ---- Date/Time ---- */

    case 0x2A: {
        /* Get date: CX=year, DH=month, DL=day, AL=day of week */
        uint16_t year;
        uint8_t month, day;
        rtc_read_date(&year, &month, &day);
        regs->ecx = (regs->ecx & 0xFFFF0000) | year;
        regs->edx = (regs->edx & 0xFFFF0000) | (month << 8) | day;
        regs->eax = (regs->eax & 0xFFFFFF00) | 0;  /* day of week — 0 for now */
        break;
    }

    case 0x2C: {
        /* Get time: CH=hour, CL=min, DH=sec, DL=hundredths */
        uint8_t hour, min, sec;
        rtc_read_time(&hour, &min, &sec);
        regs->ecx = (regs->ecx & 0xFFFF0000) | (hour << 8) | min;
        regs->edx = (regs->edx & 0xFFFF0000) | (sec << 8);  /* hundredths=0 */
        break;
    }

    /* ---- Interrupt vectors ---- */

    case 0x25: {
        /* Set interrupt vector: AL=int number, DS:DX=handler address */
        uint8_t int_num = regs->eax & 0xFF;
        uint16_t *ivt = (uint16_t *)(int_num * 4);
        ivt[0] = regs->edx & 0xFFFF;  /* offset */
        ivt[1] = regs->ds & 0xFFFF;   /* segment */
        break;
    }

    case 0x35: {
        /* Get interrupt vector: AL=int number → ES:BX */
        uint8_t int_num = regs->eax & 0xFF;
        uint16_t *ivt = (uint16_t *)(int_num * 4);
        regs->ebx = (regs->ebx & 0xFFFF0000) | ivt[0];
        regs->es = ivt[1];
        break;
    }

    /* ---- DOS version ---- */

    case 0x30: {
        /* Get DOS version — return 7.10 (FreeDOS/Win98 compatible) */
        regs->eax = (regs->eax & 0xFFFF0000) | 0x0A07; /* AL=7, AH=10 */
        regs->ebx = 0;
        regs->ecx = 0;
        break;
    }

    /* ---- File operations ---- */

    case 0x3C: {
        /* Create file: DS:DX=ASCIIZ filename, CX=attributes → AX=handle */
        char path[128];
        int fat_h, dos_h;
        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, path, 128);
        fat_h = fat_open(path, 1);  /* mode 1 = write/create */
        if (fat_h < 0) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x03;
        } else {
            dos_h = alloc_dos_handle(t, fat_h);
            if (dos_h < 0) { fat_close(fat_h); set_cf(regs); break; }
            clear_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | dos_h;
        }
        break;
    }

    case 0x3D: {
        /* Open file: DS:DX=ASCIIZ filename, AL=mode → AX=handle */
        char path[128];
        int fat_h, dos_h;
        uint8_t mode = regs->eax & 0x03;
        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, path, 128);
        serial_puts("DOS: open(\"");
        serial_puts(path);
        serial_puts("\") mode=");
        serial_puthex(mode);
        fat_h = fat_open(path, mode);
        if (fat_h < 0) {
            serial_puts(" FAIL\n");
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x02;
        } else {
            dos_h = alloc_dos_handle(t, fat_h);
            if (dos_h < 0) { fat_close(fat_h); set_cf(regs); break; }
            serial_puts(" -> handle ");
            serial_puthex(dos_h);
            serial_puts("\n");
            clear_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | dos_h;
        }
        break;
    }

    case 0x3E: {
        /* Close file: BX=handle */
        int dos_h = regs->ebx & 0xFFFF;
        int fat_h = dos_to_fat_handle(t, dos_h);
        if (fat_h < 0) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x06; /* invalid handle */
        } else {
            fat_close(fat_h);
            t->handle_map[dos_h] = 0xFF;
            clear_cf(regs);
        }
        break;
    }

    case 0x3F: {
        /* Read file: BX=handle, CX=count, DS:DX=buffer → AX=bytes read */
        int dos_h = regs->ebx & 0xFFFF;
        uint16_t count = regs->ecx & 0xFFFF;
        uint8_t *buf = v86_ptr(regs->ds & 0xFFFF, regs->edx & 0xFFFF);

        /* Handle 0 = stdin — read line from console */
        if (dos_h == 0) {
            uint16_t i = 0;
            while (i < count) {
                char c = console_getchar(task_id);
                buf[i++] = c;
                if (c == '\r') {
                    /* Also add LF */
                    if (i < count) buf[i++] = '\n';
                    break;
                }
                dos_console_putchar(task_id, c);  /* echo */
            }
            regs->eax = (regs->eax & 0xFFFF0000) | i;
            clear_cf(regs);
            break;
        }

        {
            int fat_h = dos_to_fat_handle(t, dos_h);
            if (fat_h < 0) {
                serial_puts("DOS: read FAIL bad handle dos_h=");
                serial_puthex(dos_h);
                serial_puts(" count=");
                serial_puthex(count);
                serial_puts("\n");
                set_cf(regs);
                regs->eax = (regs->eax & 0xFFFF0000) | 0x06;
            } else {
                int n = fat_read(fat_h, buf, count);
                if (n < 0) {
                    serial_puts("DOS: read FAIL fat_read dos_h=");
                    serial_puthex(dos_h);
                    serial_puts("\n");
                    set_cf(regs);
                    regs->eax = (regs->eax & 0xFFFF0000) | 0x05;
                } else {
                    serial_puts("DOS: read OK dos_h=");
                    serial_puthex(dos_h);
                    serial_puts(" req=");
                    serial_puthex(count);
                    serial_puts(" got=");
                    serial_puthex(n);
                    serial_puts("\n");
                    clear_cf(regs);
                    regs->eax = (regs->eax & 0xFFFF0000) | (n & 0xFFFF);
                }
            }
        }
        break;
    }

    case 0x40: {
        /* Write file: BX=handle, CX=count, DS:DX=buffer → AX=bytes written */
        int dos_h = regs->ebx & 0xFFFF;
        uint16_t count = regs->ecx & 0xFFFF;
        uint8_t *buf = v86_ptr(regs->ds & 0xFFFF, regs->edx & 0xFFFF);

        /* s43 — log entry for stdout/stderr writes with first ~48 chars,
         * to confirm whether DOS/16M's AX=0x40DE call is its error-message
         * print path. Bytes are emitted as printable ASCII (with newline
         * marker) so the log shows the message verbatim. */
        if (dos_h == 1 || dos_h == 2) {
            uint16_t i, lim = count < 48 ? count : 48;
            serial_puts("DOS: 40h h=");
            serial_puthex(dos_h);
            serial_puts(" cx=");
            serial_puthex(count);
            serial_puts(" AL=");
            serial_puthex(regs->eax & 0xFF);
            serial_puts(" \"");
            for (i = 0; i < lim; i++) {
                uint8_t b = buf[i];
                if (b == '\n') { serial_puts("\\n"); }
                else if (b == '\r') { serial_puts("\\r"); }
                else if (b >= 0x20 && b < 0x7F) {
                    char s[2]; s[0] = b; s[1] = 0; serial_puts(s);
                } else { serial_puts("?"); }
            }
            serial_puts("\"\n");
        }

        /* Handle 1 = stdout, 2 = stderr */
        if (dos_h == 1 || dos_h == 2) {
            uint16_t i;
            for (i = 0; i < count; i++)
                dos_console_putchar(task_id, buf[i]);
            regs->eax = (regs->eax & 0xFFFF0000) | count;
            clear_cf(regs);
            break;
        }

        {
            int fat_h = dos_to_fat_handle(t, dos_h);
            if (fat_h < 0) {
                set_cf(regs);
                regs->eax = (regs->eax & 0xFFFF0000) | 0x06;
            } else {
                int n = fat_write(fat_h, buf, count);
                if (n < 0) {
                    set_cf(regs);
                    regs->eax = (regs->eax & 0xFFFF0000) | 0x05;
                } else {
                    clear_cf(regs);
                    regs->eax = (regs->eax & 0xFFFF0000) | (n & 0xFFFF);
                }
            }
        }
        break;
    }

    case 0x41: {
        /* Delete file: DS:DX=ASCIIZ filename */
        char path[128];
        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, path, 128);
        if (fat_delete(path) < 0) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x02;
        } else {
            clear_cf(regs);
        }
        break;
    }

    case 0x42: {
        /* Seek: BX=handle, AL=origin(0=start,1=cur,2=end), CX:DX=offset → DX:AX=new position */
        int dos_h = regs->ebx & 0xFFFF;
        int fat_h = dos_to_fat_handle(t, dos_h);
        uint8_t origin = regs->eax & 0xFF;
        if (fat_h < 0) {
            serial_puts("DOS: seek FAIL bad handle dos_h=");
            serial_puthex(dos_h);
            serial_puts("\n");
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x06;
        } else {
            /* DOS LSEEK offset is SIGNED 32-bit for origin 1 (SEEK_CUR)
             * and 2 (SEEK_END) — clients (Watcom/DJGPP fseek with -4)
             * pass negative values to walk back from end/current. */
            int32_t offset = (int32_t)(((regs->ecx & 0xFFFF) << 16) | (regs->edx & 0xFFFF));
            uint32_t new_pos;

            if (origin == 2) {
                new_pos = (uint32_t)((int32_t)fat_get_size(fat_h) + offset);
                fat_seek(fat_h, new_pos);
            } else if (origin == 1) {
                new_pos = (uint32_t)((int32_t)fat_get_position(fat_h) + offset);
                fat_seek(fat_h, new_pos);
            } else {
                /* SEEK_SET: offset is unsigned positive in practice */
                new_pos = (uint32_t)offset;
                fat_seek(fat_h, new_pos);
            }

            regs->edx = (regs->edx & 0xFFFF0000) | (new_pos >> 16);
            regs->eax = (regs->eax & 0xFFFF0000) | (new_pos & 0xFFFF);
            serial_puts("DOS: seek OK dos_h=");
            serial_puthex(dos_h);
            serial_puts(" origin=");
            serial_puthex(origin);
            serial_puts(" -> pos=");
            serial_puthex(new_pos);
            serial_puts("\n");
            clear_cf(regs);
        }
        break;
    }

    case 0x43: {
        /* Get/set file attributes */
        uint8_t sub = regs->eax & 0xFF;
        char attrpath[128];
        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, attrpath, 128);
        if (sub == 0x00) {
            /* Get attributes — check if file actually exists */
            int fd = fat_open(attrpath, 0);
            if (fd >= 0) {
                fat_close(fd);
                regs->ecx = (regs->ecx & 0xFFFF0000) | FAT_ATTR_ARCHIVE;
                clear_cf(regs);
            } else {
                set_cf(regs);
                regs->eax = (regs->eax & 0xFFFF0000) | 0x02; /* file not found */
            }
        } else {
            /* Set attributes — accept and ignore */
            clear_cf(regs);
        }
        break;
    }

    /* ---- Directory operations ---- */

    case 0x39: {
        /* Mkdir: DS:DX=ASCIIZ path */
        char path[128];
        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, path, 128);
        if (fat_mkdir(path) < 0) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x03;
        } else {
            clear_cf(regs);
        }
        break;
    }

    case 0x3A: {
        /* Rmdir: DS:DX=ASCIIZ path */
        char path[128];
        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, path, 128);
        if (fat_rmdir(path) < 0) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x03;
        } else {
            clear_cf(regs);
        }
        break;
    }

    case 0x3B: {
        /* Chdir: DS:DX=ASCIIZ path */
        char path[128];
        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, path, 128);
        if (fat_chdir(path) < 0) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x03;
        } else {
            clear_cf(regs);
        }
        break;
    }

    case 0x47: {
        /* Get current directory: DL=drive, DS:SI=buffer (64 bytes) */
        uint8_t *buf = v86_ptr(regs->ds & 0xFFFF, regs->esi & 0xFFFF);
        char cwd[128];
        fat_getcwd(cwd, 128);
        /* DOS expects path without leading slash and drive letter */
        {
            const char *src = cwd;
            uint32_t i;
            if (*src == '/' || *src == '\\') src++;
            for (i = 0; src[i] && i < 63; i++)
                buf[i] = src[i];
            buf[i] = '\0';
        }
        clear_cf(regs);
        break;
    }

    /* ---- Find first/next ---- */

    case 0x4E: {
        /* Find first: DS:DX=ASCIIZ pattern, CX=attr → DTA filled
         * DTA format (43 bytes):
         *   0x00-0x14: reserved (search state)
         *   0x15:      attribute
         *   0x16-0x17: file time
         *   0x18-0x19: file date
         *   0x1A-0x1D: file size
         *   0x1E-0x2A: filename (13 bytes, null-terminated) */
        char pattern[128];
        char *filepart;
        struct fat_find ff;
        uint8_t *dta = v86_ptr(t->dta_seg, t->dta_off);
        uint16_t search_attr = regs->ecx & 0xFFFF;

        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, pattern, 128);

        /* Strip drive letter and path — find last \ or / */
        filepart = pattern;
        {
            char *p;
            for (p = pattern; *p; p++) {
                if (*p == '\\' || *p == '/')
                    filepart = p + 1;
            }
            /* Strip drive letter if filepart is still at start */
            if (filepart == pattern && pattern[1] == ':')
                filepart = pattern + 2;
        }

        /* Volume label search */
        if (search_attr == 0x08) {
            /* No volume label support — return not found */
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x12;
            break;
        }

        if (fat_find_first(filepart, &ff) == 0) {
            /* Fill DTA */
            /* Store search state in reserved area */
            *(uint32_t *)(dta + 0x00) = ff._dir_cluster;
            *(uint32_t *)(dta + 0x04) = ff._dir_index;
            dta[0x08] = ff._drive;
            {
                uint32_t k;
                for (k = 0; k < 12 && filepart[k]; k++)
                    dta[0x09 + k] = filepart[k];
                dta[0x09 + k] = '\0';
            }
            dta[0x15] = ff.attr;
            *(uint16_t *)(dta + 0x16) = ff.time;
            *(uint16_t *)(dta + 0x18) = ff.date;
            *(uint32_t *)(dta + 0x1A) = ff.size;
            {
                uint32_t k;
                for (k = 0; ff.name[k] && k < 12; k++)
                    dta[0x1E + k] = ff.name[k];
                dta[0x1E + k] = '\0';
            }
            clear_cf(regs);
        } else {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x12;
        }
        break;
    }

    case 0x4F: {
        /* Find next: uses state in DTA */
        uint8_t *dta = v86_ptr(t->dta_seg, t->dta_off);
        struct fat_find ff;
        char pattern[13];
        uint32_t k;

        /* Restore search state from DTA */
        ff._dir_cluster = *(uint32_t *)(dta + 0x00);
        ff._dir_index = *(uint32_t *)(dta + 0x04);
        ff._drive = dta[0x08];
        for (k = 0; k < 11 && dta[0x09 + k]; k++)
            pattern[k] = dta[0x09 + k];
        pattern[k] = '\0';
        for (k = 0; k < 13; k++)
            ff._pattern[k] = pattern[k];

        if (fat_find_next(&ff) == 0) {
            /* Update DTA */
            *(uint32_t *)(dta + 0x04) = ff._dir_index;
            dta[0x15] = ff.attr;
            *(uint16_t *)(dta + 0x16) = ff.time;
            *(uint16_t *)(dta + 0x18) = ff.date;
            *(uint32_t *)(dta + 0x1A) = ff.size;
            for (k = 0; ff.name[k] && k < 12; k++)
                dta[0x1E + k] = ff.name[k];
            dta[0x1E + k] = '\0';
            clear_cf(regs);
        } else {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x12;
        }
        break;
    }

    /* ---- Process management ---- */

    case 0x29: {
        /* Parse filename into FCB: DS:SI=string, ES:DI=FCB
         * Returns AL: 0=no wildcard, 1=wildcards, FF=drive letter invalid */
        uint8_t *src = v86_ptr(regs->ds & 0xFFFF, regs->esi & 0xFFFF);
        uint8_t *fcb = v86_ptr(regs->es & 0xFFFF, regs->edi & 0xFFFF);
        int si_off = 0, has_wild = 0;
        int k;

        /* Skip leading separators */
        while (src[si_off] == ' ' || src[si_off] == '\t') si_off++;

        /* Parse drive letter */
        if (src[si_off] && src[si_off + 1] == ':') {
            uint8_t d = src[si_off];
            if (d >= 'a' && d <= 'z') d -= 32;
            if (d >= 'A' && d <= 'Z') {
                fcb[0] = d - 'A' + 1;
                si_off += 2;
            } else {
                fcb[0] = 0;
            }
        } else {
            fcb[0] = 0;  /* default drive */
        }

        /* Fill filename (8 chars) and extension (3 chars) with spaces */
        for (k = 1; k <= 11; k++) fcb[k] = ' ';

        /* Parse filename */
        k = 1;
        while (k <= 8 && src[si_off] && src[si_off] != '.' &&
               src[si_off] != ' ' && src[si_off] != '/' && src[si_off] != '\\') {
            uint8_t c = src[si_off++];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == '*') { while (k <= 8) fcb[k++] = '?'; has_wild = 1; continue; }
            if (c == '?') has_wild = 1;
            fcb[k++] = c;
        }
        /* Skip to dot */
        while (src[si_off] && src[si_off] != '.' && src[si_off] != ' ' &&
               src[si_off] != '/' && src[si_off] != '\\') si_off++;

        /* Parse extension */
        if (src[si_off] == '.') {
            si_off++;
            k = 9;
            while (k <= 11 && src[si_off] && src[si_off] != ' ' &&
                   src[si_off] != '/' && src[si_off] != '\\') {
                uint8_t c = src[si_off++];
                if (c >= 'a' && c <= 'z') c -= 32;
                if (c == '*') { while (k <= 11) fcb[k++] = '?'; has_wild = 1; continue; }
                if (c == '?') has_wild = 1;
                fcb[k++] = c;
            }
        }

        /* Update SI to point past parsed text */
        regs->esi = (regs->esi & 0xFFFF0000) | ((regs->esi + si_off) & 0xFFFF);
        regs->eax = (regs->eax & 0xFFFFFF00) | (has_wild ? 0x01 : 0x00);
        break;
    }

    case 0x4B: {
        /* EXEC: load and execute program
         * AL=0: load and execute, DS:DX=program name, ES:BX=param block
         * The param block at ES:BX contains:
         *   +0x00: WORD  environment segment (0 = inherit parent's)
         *   +0x02: DWORD far pointer to command tail
         *   +0x06: DWORD far pointer to FCB1
         *   +0x0A: DWORD far pointer to FCB2
         */
        char progname[128];
        char cmdtail[128];
        uint8_t *pb;
        uint16_t child_env_seg, child_psp_seg, child_load_seg;
        uint16_t cmdtail_seg, cmdtail_off;
        uint8_t cmdtail_len;

        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, progname, 128);
        serial_puts("DOS: EXEC(\"");
        serial_puts(progname);
        serial_puts("\")\n");

        /* Read param block from ES:BX */
        pb = v86_ptr(regs->es & 0xFFFF, regs->ebx & 0xFFFF);
        cmdtail_off = *(uint16_t *)(pb + 2);
        cmdtail_seg = *(uint16_t *)(pb + 4);

        /* Read command tail: byte 0 = length, then the string, terminated by 0x0D */
        {
            uint8_t *ct = v86_ptr(cmdtail_seg, cmdtail_off);
            int k;
            cmdtail_len = ct[0];
            if (cmdtail_len > 126) cmdtail_len = 126;
            for (k = 0; k < cmdtail_len; k++)
                cmdtail[k] = ct[1 + k];
            cmdtail[cmdtail_len] = '\0';
            /* Trim leading space */
            {
                char *p = cmdtail;
                while (*p == ' ') p++;
                if (p != cmdtail) {
                    int j = 0;
                    while (p[j]) { cmdtail[j] = p[j]; j++; }
                    cmdtail[j] = '\0';
                }
            }
        }

        /* Check if file exists */
        {
            int fd = fat_open(progname, 0);
            if (fd < 0) {
                serial_puts("DOS: EXEC file not found\n");
                set_cf(regs);
                regs->eax = (regs->eax & 0xFFFF0000) | 0x02;
                break;
            }

            /* Check MZ signature to determine COM vs EXE */
            uint8_t sig[2];
            fat_read(fd, sig, 2);
            fat_close(fd);

            /* Find largest free block in MCB chain for child.
             * Walk chain, find the biggest free block. Child env+PSP+code
             * all go in that block. */
            {
                uint16_t best_seg = 0, best_size = 0;
                uint16_t ws = 0x0040;
                while (1) {
                    uint8_t *wm = (uint8_t *)((uint32_t)ws << 4);
                    uint16_t wsz = *(uint16_t *)(wm + 3);
                    uint16_t wowner = *(uint16_t *)(wm + 1);
                    if (wowner == 0 && wsz > best_size) {
                        best_seg = ws;
                        best_size = wsz;
                    }
                    if (wm[0] == 'Z') break;
                    if (wm[0] != 'M') break;
                    ws = ws + 1 + wsz;
                }
                if (best_size < 0x30) {
                    /* Not enough memory */
                    serial_puts("DOS: EXEC out of memory\n");
                    set_cf(regs);
                    regs->eax = (regs->eax & 0xFFFF0000) | 0x08;
                    break;
                }
                /* Env goes at start of free block + 1 (MCB takes 1 para) */
                child_env_seg = best_seg + 1;
            }
            child_psp_seg = child_env_seg + 0x10 + 1; /* env(0x10) + MCB(1) */
            child_load_seg = child_psp_seg + 0x10;    /* PSP is 0x10 paras */

            /* Calculate available memory for child program block */
            {
                /* Find the free block we're carving from */
                uint16_t free_mcb_seg = child_env_seg - 1;
                uint8_t *free_mcb = (uint8_t *)((uint32_t)free_mcb_seg << 4);
                uint16_t free_size = *(uint16_t *)(free_mcb + 3);
                uint8_t free_type = free_mcb[0];
                /* Child program block size = free block - env overhead */
                uint16_t env_overhead = 0x10 + 1; /* env(0x10) + psp MCB(1) */
                uint16_t child_block_size = free_size - env_overhead;

            /* Save parent state (including frame context for restore) */
            t->parent.active = 1;
            t->parent.eax = regs->eax;
            t->parent.ebx = regs->ebx;
            t->parent.ecx = regs->ecx;
            t->parent.edx = regs->edx;
            t->parent.esi = regs->esi;
            t->parent.edi = regs->edi;
            t->parent.ebp = regs->ebp;
            t->parent.eflags = regs->eflags;
            t->parent.cs = regs->cs;
            t->parent.eip = regs->eip;
            t->parent.ss = regs->ss;
            t->parent.esp = regs->esp;
            t->parent.ds = regs->ds;
            t->parent.es = regs->es;
            t->parent.psp_seg = t->psp_seg;
            t->parent.dta_seg = t->dta_seg;
            t->parent.dta_off = t->dta_off;
            t->parent.next_alloc_seg = t->next_alloc_seg;

            /* Carve env + program blocks out of the free block.
             * The free MCB at child_env_seg-1 becomes the env MCB.
             * We write: env MCB(M) → PSP MCB(M or Z) [→ leftover free(Z)] */

            /* Set up environment for child */
            env_setup(child_env_seg, progname);
            /* Fix env MCB: size to reach PSP MCB, owner = child PSP */
            {
                uint8_t *emcb = (uint8_t *)((uint32_t)(child_env_seg - 1) << 4);
                emcb[0] = 'M';
                *(uint16_t *)(emcb + 3) = child_psp_seg - 1 - child_env_seg;
                *(uint16_t *)(emcb + 1) = child_psp_seg; /* owner = child PSP */
            }

            if (sig[0] == 'M' && sig[1] == 'Z') {
                /* MZ EXE file */
                struct mz_header mzhdr;
                uint32_t header_size, file_size, image_size;
                uint16_t image_paras, total_paras;
                int efd, k;

                efd = fat_open(progname, 0);
                fat_read(efd, &mzhdr, sizeof(mzhdr));

                header_size = (uint32_t)mzhdr.header_paras * 16;
                file_size = (uint32_t)mzhdr.pages * 512;
                if (mzhdr.last_page)
                    file_size = file_size - 512 + mzhdr.last_page;
                image_size = file_size - header_size;
                image_paras = (image_size + 15) / 16;

                /* Allocate: image + max_extra, capped at available memory */
                total_paras = image_paras + 0x10; /* image + PSP */
                if (mzhdr.max_extra == 0xFFFF) {
                    /* Wants all memory */
                    total_paras = child_block_size;
                } else {
                    total_paras += mzhdr.max_extra;
                    if (total_paras > child_block_size)
                        total_paras = child_block_size;
                }

                /* Set up child program MCB. If child doesn't use all
                 * available space, create a free block after it. */
                if (total_paras < child_block_size && child_block_size - total_paras > 1) {
                    mcb_setup(child_psp_seg, child_psp_seg, total_paras, 'M');
                    {
                        uint16_t free_seg = child_psp_seg + total_paras;
                        uint8_t *fm = (uint8_t *)((uint32_t)free_seg << 4);
                        fm[0] = free_type; /* inherit M or Z from original */
                        *(uint16_t *)(fm + 1) = 0;
                        *(uint16_t *)(fm + 3) = child_block_size - total_paras - 1;
                    }
                } else {
                    mcb_setup(child_psp_seg, child_psp_seg, child_block_size, free_type);
                    total_paras = child_block_size;
                }

                /* Set up PSP with parent link */
                psp_setup(child_psp_seg, child_env_seg,
                          child_psp_seg + total_paras, cmdtail);
                {
                    uint8_t *psp = (uint8_t *)((uint32_t)child_psp_seg << 4);
                    *(uint16_t *)(psp + 0x16) = t->parent.psp_seg;
                }

                /* Load EXE image */
                fat_seek(efd, header_size);
                fat_read(efd, (uint8_t *)((uint32_t)child_load_seg << 4),
                         image_size);

                /* Apply relocations */
                if (mzhdr.num_relocs > 0) {
                    struct { uint16_t off, seg; } reloc;
                    fat_seek(efd, mzhdr.reloc_offset);
                    for (k = 0; k < mzhdr.num_relocs; k++) {
                        fat_read(efd, &reloc, 4);
                        uint32_t ra = (uint32_t)(reloc.seg + child_load_seg) * 16
                                      + reloc.off;
                        *(uint16_t *)ra += child_load_seg;
                    }
                }
                fat_close(efd);

                /* Set child entry */
                t->child.is_exe = 1;
                t->child.cs = mzhdr.init_cs + child_load_seg;
                t->child.ip = mzhdr.init_ip;
                t->child.ss = mzhdr.init_ss + child_load_seg;
                t->child.sp = mzhdr.init_sp;
                t->child.ds = child_psp_seg;
                t->child.es = child_psp_seg;

                serial_puts("DOS: EXEC EXE ");
                serial_puthex(t->child.cs);
                serial_puts(":");
                serial_puthex(t->child.ip);
                serial_puts(" mem=");
                serial_puthex(total_paras);
                serial_puts(" paras\n");

                /* s42 — dump MCB chain after EXEC to trace corruption */
                serial_puts("DOS: MCB chain after EXEC:\n");
                {
                    uint16_t ms_dbg = 0x0040;
                    int safety = 0;
                    while (safety++ < 32) {
                        uint8_t *m = (uint8_t *)((uint32_t)ms_dbg << 4);
                        serial_puts("  seg=");
                        serial_puthex(ms_dbg);
                        serial_puts(" type=");
                        serial_puthex(m[0]);
                        serial_puts(" owner=");
                        serial_puthex(*(uint16_t *)(m + 1));
                        serial_puts(" size=");
                        serial_puthex(*(uint16_t *)(m + 3));
                        serial_puts("\n");
                        if (m[0] == 'Z') break;
                        if (m[0] != 'M') { serial_puts("  (corrupt!)\n"); break; }
                        ms_dbg = ms_dbg + 1 + *(uint16_t *)(m + 3);
                    }
                }
            } else {
                /* COM file — gets all available memory (like real DOS) */
                int cfd, nbytes;

                mcb_setup(child_psp_seg, child_psp_seg, child_block_size, free_type);
                psp_setup(child_psp_seg, child_env_seg,
                          child_psp_seg + child_block_size, cmdtail);
                {
                    uint8_t *psp = (uint8_t *)((uint32_t)child_psp_seg << 4);
                    *(uint16_t *)(psp + 0x16) = t->parent.psp_seg;
                }

                cfd = fat_open(progname, 0);
                nbytes = fat_read(cfd,
                    (uint8_t *)((uint32_t)child_psp_seg * 16 + 0x100),
                    0xFF00);
                fat_close(cfd);

                t->child.is_exe = 0;
                t->child.cs = child_psp_seg;
                t->child.ip = 0x0100;
                t->child.ss = child_psp_seg;
                t->child.sp = 0xFFFE;
                t->child.ds = child_psp_seg;
                t->child.es = child_psp_seg;

                serial_puts("DOS: EXEC COM ");
                serial_puthex(child_psp_seg);
                serial_puts(":0100 (");
                serial_puthex(nbytes);
                serial_puts(" bytes) mem=");
                serial_puthex(child_block_size);
                serial_puts(" paras\n");
            }

            /* Update DOS task state for child */
            t->psp_seg = child_psp_seg;
            t->dta_seg = child_psp_seg;
            t->dta_off = 0x80;
            /* Child gets all memory up to 640KB — alloc pointer past the block */
            t->next_alloc_seg = child_psp_seg + child_block_size;
            } /* end avail_paras block */

            clear_cf(regs);
            return DOS_RESULT_EXEC;
        }
    }

    case 0x4C: {
        /* EXIT: terminate with return code */
        t->return_code = regs->eax & 0xFF;
        serial_puts("DOS: EXIT code=");
        serial_puthex(t->return_code);

        if (t->parent.active) {
            /* Child exiting — free child's memory, restore parent state */
            serial_puts(" (returning to parent)\n");

            /* Free all MCBs owned by the child PSP */
            {
                uint16_t child_psp = t->psp_seg;
                uint16_t ms = 0x0040;
                while (1) {
                    uint8_t *m = (uint8_t *)((uint32_t)ms << 4);
                    uint16_t msz = *(uint16_t *)(m + 3);
                    uint16_t mowner = *(uint16_t *)(m + 1);
                    uint8_t mtype = m[0];

                    if (mowner == child_psp) {
                        /* Free this block */
                        *(uint16_t *)(m + 1) = 0;

                        /* Merge with next block if also free */
                        if (mtype == 'M') {
                            uint16_t ns = ms + 1 + msz;
                            uint8_t *nm = (uint8_t *)((uint32_t)ns << 4);
                            if (*(uint16_t *)(nm + 1) == 0) {
                                *(uint16_t *)(m + 3) = msz + 1 + *(uint16_t *)(nm + 3);
                                m[0] = nm[0]; /* inherit M or Z */
                                continue; /* re-check this block (might merge more) */
                            }
                        }
                    }

                    if (mtype == 'Z') break;
                    if (mtype != 'M') break;
                    ms = ms + 1 + msz;
                }

                /* Second pass: merge any adjacent free blocks we created */
                ms = 0x0040;
                while (1) {
                    uint8_t *m = (uint8_t *)((uint32_t)ms << 4);
                    uint16_t msz = *(uint16_t *)(m + 3);
                    uint16_t mowner = *(uint16_t *)(m + 1);

                    if (m[0] == 'M' && mowner == 0) {
                        uint16_t ns = ms + 1 + msz;
                        uint8_t *nm = (uint8_t *)((uint32_t)ns << 4);
                        if (*(uint16_t *)(nm + 1) == 0) {
                            *(uint16_t *)(m + 3) = msz + 1 + *(uint16_t *)(nm + 3);
                            m[0] = nm[0];
                            continue; /* re-check for more merges */
                        }
                    }

                    if (m[0] == 'Z') break;
                    if (m[0] != 'M') break;
                    ms = ms + 1 + msz;
                }
            }

            t->parent.active = 0;

            /* Restore parent DOS state */
            regs->eax = t->parent.eax;
            regs->ebx = t->parent.ebx;
            regs->ecx = t->parent.ecx;
            regs->edx = t->parent.edx;
            regs->esi = t->parent.esi;
            regs->edi = t->parent.edi;
            regs->ebp = t->parent.ebp;
            regs->eflags = t->parent.eflags;

            t->psp_seg = t->parent.psp_seg;
            t->dta_seg = t->parent.dta_seg;
            t->dta_off = t->parent.dta_off;
            t->next_alloc_seg = t->parent.next_alloc_seg;

            /* EXEC returns success to parent */
            clear_cf(regs);
            regs->eax = regs->eax & 0xFFFF0000; /* AX=0 on success */

            return DOS_RESULT_CHILD_EXIT;
        }

        serial_puts(" (top-level exit)\n");
        /* Top-level exit — handled by v86 layer (longjmp) */
        break;
    }

    case 0x4D: {
        /* Get return code of child process */
        regs->eax = (regs->eax & 0xFFFF0000) | t->return_code;
        break;
    }

    /* ---- Memory management ---- */

    case 0x48: {
        /* Allocate memory: BX=paragraphs needed → AX=segment
         * First-fit: walk MCB chain, find first free block big enough.
         *
         * s42 — chain self-heal: when the walker hits an MCB whose
         * type byte is neither 'M' nor 'Z' (corruption — observed
         * during DOS/16M V86 cleanup, where direct memory writes
         * smash our env MCB at 0x0FFF), synthesise a fresh free 'Z'
         * MCB at that point covering the remainder of conventional
         * memory (up to 0xA000). Any memory previously allocated in
         * that range is effectively abandoned, which is acceptable
         * post-extender-cleanup since those owners are gone. */
        uint16_t paras = regs->ebx & 0xFFFF;
        uint16_t ms = 0x0040;
        uint16_t largest_free = 0;
        int found = 0;

        while (1) {
            uint8_t *m = (uint8_t *)((uint32_t)ms << 4);
            uint16_t msz = *(uint16_t *)(m + 3);
            uint16_t owner = *(uint16_t *)(m + 1);
            uint8_t type = m[0];

            if (type != 'M' && type != 'Z') {
                /* Corrupt — heal by stamping a 'Z' free block here */
                if (ms < 0xA000) {
                    uint16_t free_sz = 0xA000 - ms - 1;
                    serial_puts("DOS: AH=48 heal corrupt MCB at seg=");
                    serial_puthex(ms);
                    serial_puts(" → 'Z' free size=");
                    serial_puthex(free_sz);
                    serial_puts("\n");
                    m[0] = 'Z';
                    *(uint16_t *)(m + 1) = 0;
                    *(uint16_t *)(m + 3) = free_sz;
                    type = 'Z';
                    msz = free_sz;
                    owner = 0;
                } else {
                    break;
                }
            }

            if (owner == 0 && msz >= paras) {
                /* Free block big enough — allocate from it */
                *(uint16_t *)(m + 1) = t->psp_seg;

                if (msz > paras + 1) {
                    /* Split: shrink this block, create free remainder */
                    *(uint16_t *)(m + 3) = paras;
                    uint8_t *nm = (uint8_t *)((uint32_t)(ms + 1 + paras) << 4);
                    nm[0] = type;  /* inherit M or Z */
                    *(uint16_t *)(nm + 1) = 0;
                    *(uint16_t *)(nm + 3) = msz - paras - 1;
                    m[0] = 'M';
                } else {
                    /* Exact fit or 1 para slack — give it all */
                    *(uint16_t *)(m + 3) = msz;
                }

                regs->eax = (regs->eax & 0xFFFF0000) | (ms + 1);
                clear_cf(regs);
                found = 1;
                break;
            }

            /* Track largest free block for error reporting */
            if (owner == 0 && msz > largest_free)
                largest_free = msz;

            if (type == 'Z') break;
            ms = ms + 1 + msz;
        }

        if (!found) {
            /* s42 — dump MCB chain on allocation failure to diagnose
             * the conventional-memory leak that surfaces after DOOM's
             * V86 cleanup unwinds. SESSION-STATE s40 flagged this as
             * the parked MCB leak. */
            serial_puts("DOS: AH=48 FAIL want=");
            serial_puthex(paras);
            serial_puts(" para — MCB chain:\n");
            {
                uint16_t ms_dbg = 0x0040;
                int safety = 0;
                while (safety++ < 64) {
                    uint8_t *m = (uint8_t *)((uint32_t)ms_dbg << 4);
                    serial_puts("  seg=");
                    serial_puthex(ms_dbg);
                    serial_puts(" type=");
                    serial_puthex(m[0]);
                    serial_puts(" owner=");
                    serial_puthex(*(uint16_t *)(m + 1));
                    serial_puts(" size=");
                    serial_puthex(*(uint16_t *)(m + 3));
                    serial_puts("\n");
                    if (m[0] == 'Z') break;
                    if (m[0] != 'M') { serial_puts("  (bad chain)\n"); break; }
                    ms_dbg = ms_dbg + 1 + *(uint16_t *)(m + 3);
                }
            }
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x08; /* insufficient memory */
            regs->ebx = (regs->ebx & 0xFFFF0000) | largest_free;
        }
        break;
    }

    case 0x49: {
        /* Free memory: ES=segment of block to free.
         * Set owner=0, then merge with adjacent free blocks. */
        uint16_t seg = regs->es & 0xFFFF;
        uint16_t mcb_seg = seg - 1;
        uint8_t *mcb = (uint8_t *)((uint32_t)mcb_seg << 4);

        if (mcb[0] != 'M' && mcb[0] != 'Z') {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x09; /* invalid block */
            break;
        }

        /* Mark as free */
        *(uint16_t *)(mcb + 1) = 0;

        /* Merge with next block if also free */
        if (mcb[0] == 'M') {
            uint16_t this_sz = *(uint16_t *)(mcb + 3);
            uint16_t next_seg = mcb_seg + 1 + this_sz;
            uint8_t *nm = (uint8_t *)((uint32_t)next_seg << 4);
            uint16_t next_owner = *(uint16_t *)(nm + 1);
            if (next_owner == 0) {
                /* Absorb the next free block */
                uint16_t next_sz = *(uint16_t *)(nm + 3);
                *(uint16_t *)(mcb + 3) = this_sz + 1 + next_sz;
                mcb[0] = nm[0]; /* inherit M or Z */
            }
        }

        clear_cf(regs);
        break;
    }

    case 0x4A: {
        /* Resize memory: ES=segment, BX=new paragraphs */
        uint16_t seg = regs->es & 0xFFFF;
        uint16_t new_size = regs->ebx & 0xFFFF;
        uint16_t mcb_seg = seg - 1;
        uint8_t *mcb = (uint8_t *)((uint32_t)mcb_seg << 4);

        /* Validate the MCB before touching anything. A bad ES (e.g. 0,
         * or pointing at non-MCB memory) used to walk past the IVT and
         * write a fake MCB header into the V86 task's data segment,
         * smashing FreeCom into a wild jump. (Repro: FreeCom dir/w after
         * directory got bigger from the multi-COMMAND.COM drop.) */
        if (seg < 2 || (mcb[0] != 'M' && mcb[0] != 'Z')) {
            serial_puts("DOS: AH=4A bad MCB at seg=");
            serial_puthex(mcb_seg);
            serial_puts(" sig=");
            serial_puthex(mcb[0]);
            serial_puts("\n");
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x09;  /* invalid block */
            break;
        }
        uint16_t old_size = *(uint16_t *)(mcb + 3);

        if (new_size < old_size) {
            /* Shrinking — create free block in the freed space */
            uint16_t freed = old_size - new_size - 1;
            uint16_t free_seg = seg + new_size;
            uint8_t *fm = (uint8_t *)((uint32_t)free_seg << 4);

            *(uint16_t *)(mcb + 3) = new_size;

            if (mcb[0] == 'Z') {
                /* Was last block — new free block becomes Z */
                mcb[0] = 'M';
                fm[0] = 'Z';
                *(uint16_t *)(fm + 1) = 0;
                *(uint16_t *)(fm + 3) = freed;
            } else {
                /* Middle block — check if next block is also free, merge */
                uint16_t next_seg = mcb_seg + 1 + old_size;
                uint8_t *nm = (uint8_t *)((uint32_t)next_seg << 4);
                uint16_t next_owner = *(uint16_t *)(nm + 1);
                if (next_owner == 0) {
                    /* Merge freed space + next free block */
                    uint16_t next_sz = *(uint16_t *)(nm + 3);
                    fm[0] = nm[0]; /* inherit M or Z */
                    *(uint16_t *)(fm + 1) = 0;
                    *(uint16_t *)(fm + 3) = freed + 1 + next_sz;
                } else {
                    fm[0] = 'M';
                    *(uint16_t *)(fm + 1) = 0;
                    *(uint16_t *)(fm + 3) = freed;
                }
            }
            clear_cf(regs);
        } else if (new_size > old_size) {
            /* Growing — need to absorb adjacent free block */
            if (mcb[0] == 'M') {
                uint16_t next_mcb_seg = mcb_seg + 1 + old_size;
                uint8_t *nm = (uint8_t *)((uint32_t)next_mcb_seg << 4);
                uint16_t next_owner = *(uint16_t *)(nm + 1);
                uint16_t next_sz = *(uint16_t *)(nm + 3);
                uint16_t total = old_size + 1 + next_sz;

                if (next_owner == 0 && total >= new_size) {
                    /* Absorb the free block */
                    *(uint16_t *)(mcb + 3) = new_size;
                    if (new_size < total) {
                        /* Leftover free space */
                        uint8_t *fm = (uint8_t *)((uint32_t)(seg + new_size) << 4);
                        fm[0] = nm[0]; /* inherit M or Z */
                        *(uint16_t *)(fm + 1) = 0;
                        *(uint16_t *)(fm + 3) = total - new_size - 1;
                        mcb[0] = 'M';
                    } else {
                        /* Exact fit — inherit type */
                        mcb[0] = nm[0];
                    }
                    clear_cf(regs);
                } else {
                    /* Not enough — return max available */
                    set_cf(regs);
                    regs->eax = (regs->eax & 0xFFFF0000) | 0x08;
                    regs->ebx = (regs->ebx & 0xFFFF0000) |
                                (next_owner == 0 ? total : old_size);
                }
            } else {
                /* Last block (Z) — can't grow past end of memory */
                set_cf(regs);
                regs->eax = (regs->eax & 0xFFFF0000) | 0x08;
                regs->ebx = (regs->ebx & 0xFFFF0000) | old_size;
            }
        } else {
            clear_cf(regs);  /* same size, no-op */
        }
        break;
    }

    /* ---- Misc ---- */

    case 0x44: {
        /* IOCTL */
        uint8_t al = regs->eax & 0xFF;
        if (al == 0x00) {
            /* Get device info for handle */
            int dos_h = regs->ebx & 0xFFFF;
            if (dos_h <= 2) {
                /* Console device: bit 7 = char device, bit 6 = not EOF, bit 0 = stdin, bit 1 = stdout */
                regs->edx = (regs->edx & 0xFFFF0000) | 0x00C0 | (dos_h <= 1 ? 0x03 : 0x02);
            } else {
                regs->edx = (regs->edx & 0xFFFF0000) | 0x0000; /* disk file */
            }
            clear_cf(regs);
        } else {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x01; /* invalid function */
        }
        break;
    }

    case 0x56: {
        /* Rename: DS:DX=old name, ES:DI=new name */
        char old_path[128], new_path[128];
        v86_read_asciiz(regs->ds & 0xFFFF, regs->edx & 0xFFFF, old_path, 128);
        v86_read_asciiz(regs->es & 0xFFFF, regs->edi & 0xFFFF, new_path, 128);
        if (fat_rename(old_path, new_path) < 0) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x02;
        } else {
            clear_cf(regs);
        }
        break;
    }

    case 0x57: {
        /* Get/set file date/time */
        uint8_t sub = regs->eax & 0xFF;
        int dos_h = regs->ebx & 0xFFFF;
        if (sub == 0x00) {
            /* Get: read timestamps from FAT directory entry */
            int fat_h = dos_to_fat_handle(t, dos_h);
            if (fat_h >= 0) {
                uint16_t ftime, fdate;
                fat_get_datetime(fat_h, &ftime, &fdate);
                regs->ecx = (regs->ecx & 0xFFFF0000) | ftime;
                regs->edx = (regs->edx & 0xFFFF0000) | fdate;
            } else {
                regs->ecx = (regs->ecx & 0xFFFF0000);
                regs->edx = (regs->edx & 0xFFFF0000);
            }
            clear_cf(regs);
        } else if (sub == 0x01) {
            /* Set — accept and ignore for now */
            clear_cf(regs);
        } else {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x01;
        }
        break;
    }

    case 0x62: {
        /* Get PSP address */
        regs->ebx = (regs->ebx & 0xFFFF0000) | t->psp_seg;
        break;
    }

    case 0x07: {
        /* Direct char input without echo */
        char c = console_getchar(task_id);
        regs->eax = (regs->eax & 0xFFFFFF00) | (uint8_t)c;
        break;
    }

    case 0x08: {
        /* Char input without echo (checks Ctrl-C) */
        char c = console_getchar(task_id);
        regs->eax = (regs->eax & 0xFFFFFF00) | (uint8_t)c;
        break;
    }

    case 0x0E: {
        /* Select disk: DL=drive number → AL=number of drives */
        uint8_t dl = regs->edx & 0xFF;
        if (fat_is_mounted(dl))
            fat_set_drive(dl);
        regs->eax = (regs->eax & 0xFFFFFF00) | 3;  /* 3 drives: A, B, C */
        break;
    }

    case 0x38: {
        /* Get/set country info */
        uint8_t al = regs->eax & 0xFF;
        if (al == 0x00 || al == 0x01) {
            /* Get country info: DS:DX → buffer */
            uint8_t *buf = v86_ptr(regs->ds & 0xFFFF, regs->edx & 0xFFFF);
            /* US country code (1), code page 437 */
            *(uint16_t *)(buf + 0) = 0;     /* date format: MDY */
            buf[2] = '$'; buf[3] = 0; buf[4] = 0; buf[5] = 0; buf[6] = 0; /* currency */
            buf[7] = ','; buf[8] = 0;  /* thousands sep */
            buf[9] = '.'; buf[10] = 0; /* decimal sep */
            buf[11] = '/'; buf[12] = 0; /* date sep */
            buf[13] = ':'; buf[14] = 0; /* time sep */
            buf[15] = 0; buf[16] = 2; buf[17] = 0; /* currency format, digits */
            regs->ebx = (regs->ebx & 0xFFFF0000) | 1; /* country code */
            clear_cf(regs);
        } else {
            clear_cf(regs);
        }
        break;
    }

    case 0x37: {
        /* Get/set switch character */
        uint8_t al = regs->eax & 0xFF;
        if (al == 0x00) {
            /* Get switch char */
            regs->edx = (regs->edx & 0xFFFFFF00) | '/';
        }
        /* Set (al=1) — ignore */
        clear_cf(regs);
        break;
    }

    case 0x52: {
        /* Get list of lists */
        serial_puts("DOS: INT 21h/52h (Get LoL)\n");
        /* Return a dummy pointer to a minimal structure.
         * FREECOM checks this for CDS (Current Directory Structure).
         * We put it at 0x0060:0x0026 so the -2 access (offset 0x24) works
         * without segment wrap. */
        uint8_t *lol = v86_ptr(0x0060, 0x0026);
        uint32_t k;
        for (k = 0; k < 64; k++) lol[k] = 0;
        /* Offset -2 from returned pointer: DOS segment of first MCB */
        *(uint16_t *)(lol - 2) = 0x0040;
        /* Offset 0x16: CDS pointer (far ptr) — point to dummy area */
        *(uint16_t *)(lol + 0x16) = 0x00;
        *(uint16_t *)(lol + 0x18) = 0x0070;
        /* Offset 0x21: last drive */
        lol[0x21] = 3;  /* A, B, C */
        /* Offset 0x22: NUL device header pointer */

        regs->es = 0x0060;
        regs->ebx = (regs->ebx & 0xFFFF0000) | 0x0026;
        break;
    }

    case 0x58: {
        /* Get/set memory allocation strategy */
        uint8_t al = regs->eax & 0xFF;
        if (al == 0x00) {
            /* Get strategy */
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0000; /* first fit */
            clear_cf(regs);
        } else if (al == 0x01) {
            /* Set strategy — accept and ignore */
            clear_cf(regs);
        } else if (al == 0x02) {
            /* Get UMB link state */
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0000; /* not linked */
            clear_cf(regs);
        } else if (al == 0x03) {
            /* Set UMB link state — ignore */
            clear_cf(regs);
        } else {
            set_cf(regs);
        }
        break;
    }

    case 0x65: {
        /* Get extended country info */
        uint8_t al = regs->eax & 0xFF;
        uint8_t *buf = v86_ptr(regs->es & 0xFFFF, regs->edi & 0xFFFF);
        uint16_t cx = regs->ecx & 0xFFFF;

        if (al == 0x01 && cx >= 41) {
            /* Extended country info */
            buf[0] = 0x01;  /* info ID */
            *(uint16_t *)(buf + 1) = 38;  /* size of following data */
            *(uint16_t *)(buf + 3) = 1;   /* country code (1=US) */
            *(uint16_t *)(buf + 5) = 437; /* code page */
            /* Date format: 0=MDY */
            *(uint16_t *)(buf + 7) = 0;
            /* Currency symbol */
            buf[9] = '$'; buf[10] = 0; buf[11] = 0; buf[12] = 0; buf[13] = 0;
            /* Thousands sep, decimal sep */
            buf[14] = ','; buf[15] = 0;
            buf[16] = '.'; buf[17] = 0;
            /* Date sep, time sep */
            buf[18] = '/'; buf[19] = 0;
            buf[20] = ':'; buf[21] = 0;
            /* Currency format, digits after decimal, time format */
            buf[22] = 0; buf[23] = 2; buf[24] = 0;
            /* Remaining fields — zero fill */
            {
                uint32_t j;
                for (j = 25; j < 41; j++) buf[j] = 0;
            }
            regs->ecx = (regs->ecx & 0xFFFF0000) | 41;
            clear_cf(regs);
        } else if (al == 0x02 || al == 0x04 || al == 0x05 || al == 0x06 || al == 0x07) {
            /* Uppercase/filename tables — return error so caller uses
             * built-in fallback.  These subfunctions must return a FAR
             * pointer to a table in DOS memory; we don't have one, and
             * returning a bogus pointer causes FREECOM's is_fnchar() to
             * dereference garbage and reject all ASCII letters. */
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x01;
        } else {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x01;
        }
        break;
    }

    case 0x69: {
        /* Get/set disk serial number */
        uint8_t sub = regs->eax & 0xFF;
        if (sub == 0x00) {
            /* Get: BL=drive (0=default). Return info in DS:DX buffer */
            uint8_t *info = v86_ptr(regs->ds & 0xFFFF, regs->edx & 0xFFFF);
            *(uint16_t *)(info + 0) = 0;            /* info level */
            *(uint32_t *)(info + 2) = 0x12345678;   /* serial number */
            /* Volume label (11 bytes) */
            {
                int k;
                const char *lbl = "NO NAME    ";
                for (k = 0; k < 11; k++) info[6 + k] = lbl[k];
            }
            /* FS type (8 bytes) */
            {
                int k;
                const char *fs = "FAT16   ";
                for (k = 0; k < 8; k++) info[17 + k] = fs[k];
            }
            clear_cf(regs);
        } else {
            /* Set serial — ignore */
            clear_cf(regs);
        }
        break;
    }

    case 0x26: {
        /* Create new PSP — stub */
        clear_cf(regs);
        break;
    }

    case 0x50: {
        /* Set current PSP */
        t->psp_seg = regs->ebx & 0xFFFF;
        break;
    }

    case 0x51: {
        /* Get current PSP → BX */
        regs->ebx = (regs->ebx & 0xFFFF0000) | t->psp_seg;
        break;
    }

    case 0x36: {
        /* Get disk free space: DL=drive (0=default, 1=A:, 2=B:, 3=C:)
         * Returns: AX=sectors/cluster, BX=free clusters, CX=bytes/sector, DX=total clusters
         * On error: AX=0xFFFF */
        uint8_t dl = regs->edx & 0xFF;
        int drive = (dl == 0) ? fat_get_drive() : (dl - 1);
        int prev_drive = fat_get_drive();

        if (!fat_is_mounted(drive)) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0xFFFF;
            break;
        }

        fat_set_drive(drive);
        {
            uint32_t total = fat_get_total_clusters();
            uint32_t spc = fat_get_sec_per_clus();
            uint32_t free_clus = fat_count_free_clusters();

            regs->eax = (regs->eax & 0xFFFF0000) | (spc & 0xFFFF);
            regs->ebx = (regs->ebx & 0xFFFF0000) | (free_clus & 0xFFFF);
            regs->ecx = (regs->ecx & 0xFFFF0000) | 512;
            regs->edx = (regs->edx & 0xFFFF0000) | (total & 0xFFFF);
        }
        fat_set_drive(prev_drive);
        break;
    }

    case 0x0F: {
        /* FCB Open — stub: return AL=0xFF (file not found).
         * DOS/4GW may call this but handles failure gracefully. */
        regs->eax = (regs->eax & 0xFFFFFF00) | 0xFF;
        break;
    }

    case 0x10: {
        /* FCB Close — stub: return AL=0 (success) */
        regs->eax = (regs->eax & 0xFFFFFF00) | 0x00;
        break;
    }

    case 0x14: {
        /* FCB Sequential Read — stub: return AL=1 (end of file) */
        regs->eax = (regs->eax & 0xFFFFFF00) | 0x01;
        break;
    }

    case 0x15: {
        /* FCB Sequential Write — stub: return AL=1 (disk full/error) */
        regs->eax = (regs->eax & 0xFFFFFF00) | 0x01;
        break;
    }

    case 0x16: {
        /* FCB Create — stub: return AL=0xFF (cannot create) */
        regs->eax = (regs->eax & 0xFFFFFF00) | 0xFF;
        break;
    }

    case 0x71: {
        /* Windows 95 LFN API — not supported, return error */
        set_cf(regs);
        regs->eax = (regs->eax & 0xFFFF0000) | 0x7100; /* function not supported */
        break;
    }

    case 0x73: {
        /* Extended drive/free-space API (FAT32-aware, INT 21h/73h).
         * AL=03h: Get extended free-space info.
         *   DS:DX → ASCIIZ drive root ("C:\")
         *   ES:DI → ExtFreeSpaceInfo buffer (0x2C bytes)
         *   CX    = buffer size (must be ≥ 0x2C)
         * Returns CF=0 + buffer filled, or CF=1 + AX=error.
         * FreeCOM probes this on every `dir`. */
        uint8_t al = regs->eax & 0xFF;
        if (al != 0x03) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x7300;
            break;
        }

        /* Resolve drive from DS:DX path (first char A-Z/a-z = drive letter) */
        uint8_t *path = v86_ptr(regs->ds & 0xFFFF, regs->edx & 0xFFFF);
        int drive;
        if (path[0] >= 'A' && path[0] <= 'Z')      drive = path[0] - 'A';
        else if (path[0] >= 'a' && path[0] <= 'z') drive = path[0] - 'a';
        else                                       drive = fat_get_drive();

        if (!fat_is_mounted(drive) || (regs->ecx & 0xFFFF) < 0x2C) {
            set_cf(regs);
            regs->eax = (regs->eax & 0xFFFF0000) | 0x000F; /* invalid drive */
            break;
        }

        int prev_drive = fat_get_drive();
        fat_set_drive(drive);
        uint32_t spc        = fat_get_sec_per_clus();
        uint32_t total_clus = fat_get_total_clusters();
        uint32_t free_clus  = fat_count_free_clusters();
        fat_set_drive(prev_drive);

        uint8_t *buf = v86_ptr(regs->es & 0xFFFF, regs->edi & 0xFFFF);
        uint32_t i;
        for (i = 0; i < 0x2C; i++) buf[i] = 0;
        *(uint16_t *)(buf + 0x00) = 0x2C;       /* size returned */
        *(uint16_t *)(buf + 0x02) = 0x0000;     /* version */
        *(uint32_t *)(buf + 0x04) = spc;        /* sectors/cluster */
        *(uint32_t *)(buf + 0x08) = 512;        /* bytes/sector */
        *(uint32_t *)(buf + 0x0C) = free_clus;  /* available clusters */
        *(uint32_t *)(buf + 0x10) = total_clus; /* total clusters */
        *(uint32_t *)(buf + 0x14) = spc;        /* physical sectors/cluster */
        *(uint32_t *)(buf + 0x18) = 512;        /* physical bytes/sector */
        *(uint32_t *)(buf + 0x1C) = free_clus;  /* physical available */
        *(uint32_t *)(buf + 0x20) = total_clus; /* physical total */
        clear_cf(regs);
        break;
    }

    case 0xFF: {
        /* s45: DOS/4GW-style detection extensions.
         *
         * DOS/4GW recognizes specific (EAX, EDX) probes and returns magic
         * sentinels to identify itself. Watt32's dpmi_is_dos4gw() in
         * wdpmi.c:168 uses:
         *
         *   INT 21h EAX=0x0000FF00 EDX=0x78 → returns EAX=0xFFFF3447
         *
         * Other (EDX) values are reserved for DOS/4GW-internal control
         * APIs. We respond only to the well-known detection probe and
         * default to "unhandled" for the rest (which is a strict-DPMI
         * stance — DOS/4GW emulation isn't claimed). */
        if ((regs->eax & 0xFFFF) == 0xFF00 && (regs->edx & 0xFFFF) == 0x78) {
            serial_puts("DOS: INT 21h AX=FF00 EDX=78 → DOS/4GW detect, returning 0xFFFF3447\n");
            regs->eax = 0xFFFF3447;
            clear_cf(regs);
            break;
        }
        serial_puts("DOS: INT 21h AX=FF");
        serial_puthex(regs->eax & 0xFF);
        serial_puts(" EDX=");
        serial_puthex(regs->edx & 0xFFFF);
        serial_puts(" (unknown DOS/4GW subfn)\n");
        set_cf(regs);
        regs->eax = (regs->eax & 0xFFFF0000) | 0x01;
        break;
    }

    default:
        serial_puts("DOS: unhandled INT 21h AH=");
        serial_puthex(ah);
        serial_puts("\n");
        /* Return error but don't crash */
        set_cf(regs);
        regs->eax = (regs->eax & 0xFFFF0000) | 0x01;
        break;
    } /* switch */

    return DOS_RESULT_NORMAL;
}
