/* vt.c — Pinecore Virtual Terminal Manager
 *
 * Multiple 80x25 text terminals with independent screen buffers
 * and keyboard queues. Alt+F1-F6 switches between VTs.
 *
 * (ch-17)
 */

#include "types.h"
#include "vt.h"
#include "vga.h"
#include "sched.h"
#include "serial.h"
#include "dos.h"
#include "keyboard.h"
#include "shell.h"
#include "heap.h"
#include "pmm.h"

static struct vt vts[VT_MAX];
static int active_vt = -1;

volatile int vt_request = VT_REQ_NONE;

extern void *memset(void *s, int c, uint32_t n);

/* ================================================================
 * Graphics-mode framebuffer save buffers
 *
 * When a graphics-mode VT is switched into the background, we copy its
 * framebuffer (LFB or mode-13h planar) into a chain of physical pages
 * so the task can be paused and the screen reused. Pages live in the
 * 32 MB identity-mapped zone, so we can write to them via their physical
 * address directly. The array of page addresses lives on the kernel
 * heap (~kB per VT, easily covered by the 256 KB heap).
 * ================================================================ */

static int vt_gfx_save_ensure(int vt_num, uint32_t bytes) {
    struct vt *v;
    uint32_t need;
    uint32_t i;

    if (vt_num < 0 || vt_num >= VT_MAX) return -1;
    v = &vts[vt_num];

    need = (bytes + 0xFFF) >> 12;   /* round up to 4 KB pages */
    if (need == 0) need = 1;

    if (v->gfx_save_pages && v->gfx_save_npages >= need)
        return 0;   /* already big enough */

    /* Free any old (too-small) allocation first */
    if (v->gfx_save_pages) {
        for (i = 0; i < v->gfx_save_npages; i++)
            if (v->gfx_save_pages[i]) pmm_free_page(v->gfx_save_pages[i]);
        kfree(v->gfx_save_pages);
        v->gfx_save_pages = 0;
        v->gfx_save_npages = 0;
    }

    v->gfx_save_pages = (uint32_t *)kmalloc(need * sizeof(uint32_t));
    if (!v->gfx_save_pages) {
        serial_puts("VT: gfx_save_ensure OOM (array)\n");
        return -1;
    }
    for (i = 0; i < need; i++) v->gfx_save_pages[i] = 0;

    for (i = 0; i < need; i++) {
        uint32_t p = pmm_alloc_page();
        if (!p) {
            /* OOM: roll back, return failure */
            uint32_t j;
            for (j = 0; j < i; j++) pmm_free_page(v->gfx_save_pages[j]);
            kfree(v->gfx_save_pages);
            v->gfx_save_pages = 0;
            v->gfx_save_npages = 0;
            serial_puts("VT: gfx_save_ensure OOM (page)\n");
            return -1;
        }
        v->gfx_save_pages[i] = p;
    }
    v->gfx_save_npages = need;
    return 0;
}

static void vt_gfx_save_free(int vt_num) {
    struct vt *v;
    uint32_t i;

    if (vt_num < 0 || vt_num >= VT_MAX) return;
    v = &vts[vt_num];

    if (!v->gfx_save_pages) return;
    for (i = 0; i < v->gfx_save_npages; i++)
        if (v->gfx_save_pages[i]) pmm_free_page(v->gfx_save_pages[i]);
    kfree(v->gfx_save_pages);
    v->gfx_save_pages = 0;
    v->gfx_save_npages = 0;
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* DOS console callbacks — route I/O to the task's VT */

static int dos_task_to_vt(int task_id) {
    /* Find which VT has this DOS task */
    int i;
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        struct task *t = sched_get_task(i);
        if (t && t->dos_task_id == task_id && t->vt >= 0)
            return t->vt;
    }
    /* Fallback: use active VT */
    return active_vt;
}

static void vt_dos_putchar(int task_id, char c) {
    int vt_num = dos_task_to_vt(task_id);
    if (vt_num >= 0) vt_putc(vt_num, c);
    serial_putc(c);
}

static char vt_dos_getchar(int task_id) {
    int vt_num = dos_task_to_vt(task_id);
    struct key_event ev;
    while (1) {
        if (vt_num >= 0 && vt_poll_key(vt_num, &ev)) {
            if (ev.pressed && ev.ascii)
                return ev.ascii;
        }
        /* Enable interrupts and halt — the keyboard IRQ will wake us,
         * and the timer IRQ will fire the scheduler.
         * We can't use sched_block here because we're inside the GPF handler
         * (V86 INT 21h → dos_int21 → console_getchar). */
        __asm__ volatile("sti; hlt; cli");
    }
}

static int vt_dos_kbhit(int task_id) {
    int vt_num = dos_task_to_vt(task_id);
    struct vt *v;
    if (vt_num < 0) return 0;
    v = vt_get(vt_num);
    if (!v) return 0;
    return v->key_head != v->key_tail;
}

void vt_init(void) {
    int i;
    for (i = 0; i < VT_MAX; i++) {
        vts[i].type = VT_UNUSED;
        vts[i].task_id = -1;
        vts[i].v86_task_id = -1;
        vts[i].video = VT_VID_TEXT_03H;
        vts[i].gfx_w = 0;
        vts[i].gfx_h = 0;
        vts[i].gfx_bpp = 0;
        vts[i].gfx_save_pages = 0;
        vts[i].gfx_save_npages = 0;
    }
    active_vt = -1;

    /* Set DOS console callbacks to use VT routing */
    dos_set_console(vt_dos_putchar, vt_dos_getchar, vt_dos_kbhit);

    serial_puts("VT: init (");
    serial_puthex(VT_MAX);
    serial_puts(" slots)\n");
}

int vt_create(enum vt_type type) {
    int i, j;

    for (i = 0; i < VT_MAX; i++) {
        if (vts[i].type == VT_UNUSED)
            break;
    }
    if (i == VT_MAX) return -1;

    vts[i].type = type;
    vts[i].cursor_x = 0;
    vts[i].cursor_y = 0;
    vts[i].color = 0x07;  /* light grey on black */
    vts[i].key_head = 0;
    vts[i].key_tail = 0;
    vts[i].task_id = -1;
    vts[i].v86_task_id = -1;
    vts[i].video = VT_VID_TEXT_03H;
    vts[i].gfx_w = 0;
    vts[i].gfx_h = 0;
    vts[i].gfx_bpp = 0;
    vts[i].gfx_save_pages = 0;
    vts[i].gfx_save_npages = 0;

    /* Clear screen buffer to spaces with default attribute */
    for (j = 0; j < VT_BUF_SIZE; j += 2) {
        vts[i].screen[j]     = ' ';
        vts[i].screen[j + 1] = 0x07;
    }

    /* Start cursor at row 1 (row 0 = status bar) */
    vts[i].cursor_y = 1;

    serial_puts("VT: created vt");
    serial_puthex(i);
    serial_puts(" type=");
    serial_puthex(type);
    serial_puts("\n");

    return i;
}

void vt_destroy(int vt_num) {
    if (vt_num < 0 || vt_num >= VT_MAX) return;
    if (vts[vt_num].type == VT_UNUSED) return;

    serial_puts("VT: destroying vt");
    serial_puthex(vt_num);
    serial_puts("\n");

    /* If this is the active VT, switch to another */
    if (active_vt == vt_num) {
        int next = -1, j;
        for (j = 0; j < VT_MAX; j++) {
            if (j != vt_num && vts[j].type != VT_UNUSED) {
                next = j;
                break;
            }
        }
        if (next >= 0)
            vt_switch(next);
        else
            active_vt = -1;  /* no VTs left */
    }

    /* Free any backgrounded-framebuffer save pages; defensively unblock
     * the bound task (idempotent when no task is BLOCKED on this VT). */
    vt_gfx_save_free(vt_num);
    sched_unblock(BLOCK_VT_HIDDEN, vt_num);

    vts[vt_num].type = VT_UNUSED;
    vts[vt_num].task_id = -1;
    vts[vt_num].v86_task_id = -1;
    vts[vt_num].video = VT_VID_TEXT_03H;
}

int vt_count_active(void) {
    int i, count = 0;
    for (i = 0; i < VT_MAX; i++) {
        if (vts[i].type != VT_UNUSED)
            count++;
    }
    return count;
}

/* ================================================================
 * Switching
 * ================================================================ */

/* Draw the status bar on row 0 of the VGA display */
static void draw_status_bar(void) {
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    int i, col;
    uint16_t attr = 0x70;  /* black on light grey */

    /* Clear row 0 */
    for (i = 0; i < VT_COLS; i++)
        vga[i] = (attr << 8) | ' ';

    /* Write " Pinecore " at left */
    {
        const char *label = " Pinecore ";
        col = 0;
        while (*label && col < VT_COLS)
            vga[col++] = (0x74 << 8) | *label++;  /* red on grey */
    }

    /* Write VT tabs */
    col = 12;
    for (i = 0; i < VT_MAX && col < VT_COLS - 5; i++) {
        if (vts[i].type == VT_UNUSED) continue;

        uint16_t tab_attr;
        if (i == active_vt)
            tab_attr = 0x0F;  /* white on black (active) */
        else
            tab_attr = 0x78;  /* dark grey on light grey */

        /* Write " N:type " */
        vga[col++] = (tab_attr << 8) | ' ';
        vga[col++] = (tab_attr << 8) | ('1' + i);
        vga[col++] = (tab_attr << 8) | ':';

        if (vts[i].type == VT_SHELL) {
            const char *t = "Commando";
            while (*t && col < VT_COLS)
                vga[col++] = (tab_attr << 8) | *t++;
        } else if (vts[i].type == VT_DOS) {
            const char *t = "DOS";
            while (*t && col < VT_COLS)
                vga[col++] = (tab_attr << 8) | *t++;
        }
        vga[col++] = (tab_attr << 8) | ' ';
    }

    /* Write "Ctrl+1-6" at right */
    {
        const char *hint = " ^1-6:VT ^C:DOS ^N:Cmdo ^X:Close ";
        int hlen = 0;
        const char *p = hint;
        while (*p++) hlen++;
        col = VT_COLS - hlen;
        if (col < 0) col = 0;
        p = hint;
        while (*p && col < VT_COLS)
            vga[col++] = (0x78 << 8) | *p++;
    }
}

void vt_switch(int vt_num) {
    uint32_t flags;

    if (vt_num < 0 || vt_num >= VT_MAX) return;
    if (vts[vt_num].type == VT_UNUSED) return;
    if (vt_num == active_vt && active_vt >= 0) return;

    /* Disable interrupts during switch to prevent races */
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));

    /* Save current VT's screen (rows 1-24 only, skip status bar) */
    if (active_vt >= 0 && vts[active_vt].type != VT_UNUSED) {
        vga_save(vts[active_vt].screen, &vts[active_vt].cursor_x,
                 &vts[active_vt].cursor_y);
    }

    /* Restore target VT's screen */
    vga_restore(vts[vt_num].screen, vts[vt_num].cursor_x,
                vts[vt_num].cursor_y);

    active_vt = vt_num;

    /* Redraw status bar on row 0 */
    draw_status_bar();

    /* Make sure VGA cursor is below status bar */
    {
        uint8_t cx, cy;
        vga_get_cursor(&cx, &cy);
        if (cy == 0) vga_set_cursor(0, 1);
    }

    /* Restore interrupt state */
    __asm__ volatile("push %0; popf" : : "r"(flags));
}

int vt_get_active(void) {
    return active_vt;
}

/* Redraw the currently active VT to the VGA framebuffer. Useful after
 * something stomped on text-mode memory (e.g. a DPMI client used the
 * VESA LFB and just exited — the chars at 0xB8000 are intact but the
 * VGA controller was reprogrammed; without this the user sees a blank
 * text-mode screen). */
void vt_repaint(void) {
    uint32_t flags;
    if (active_vt < 0 || vts[active_vt].type == VT_UNUSED) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    vga_restore(vts[active_vt].screen, vts[active_vt].cursor_x,
                vts[active_vt].cursor_y);
    draw_status_bar();
    __asm__ volatile("push %0; popf" : : "r"(flags));
}

struct vt *vt_get(int vt_num) {
    if (vt_num < 0 || vt_num >= VT_MAX) return 0;
    if (vts[vt_num].type == VT_UNUSED) return 0;
    return &vts[vt_num];
}

/* ================================================================
 * Console output — writes to VT's buffer and optionally to VGA
 * ================================================================ */

/* Write a character to VT's screen buffer at its cursor position */
static void vt_buf_putc(int vt_num, char c) {
    struct vt *v = &vts[vt_num];
    uint8_t *scr = v->screen;

    if (c == '\n') {
        v->cursor_x = 0;
        v->cursor_y++;
    } else if (c == '\r') {
        v->cursor_x = 0;
    } else if (c == '\t') {
        v->cursor_x = (v->cursor_x + 8) & ~7;
    } else if (c == '\b') {
        if (v->cursor_x > 0) {
            v->cursor_x--;
            scr[(v->cursor_y * VT_COLS + v->cursor_x) * 2] = ' ';
            scr[(v->cursor_y * VT_COLS + v->cursor_x) * 2 + 1] = v->color;
        }
    } else {
        int pos = (v->cursor_y * VT_COLS + v->cursor_x) * 2;
        scr[pos] = (uint8_t)c;
        scr[pos + 1] = v->color;
        v->cursor_x++;
    }

    /* Wrap */
    if (v->cursor_x >= VT_COLS) {
        v->cursor_x = 0;
        v->cursor_y++;
    }

    /* Scroll */
    if (v->cursor_y >= VT_ROWS) {
        int i;
        /* Move lines 1-24 up to 0-23 */
        for (i = 0; i < (VT_ROWS - 1) * VT_COLS * 2; i++)
            scr[i] = scr[i + VT_COLS * 2];
        /* Clear last line */
        for (i = (VT_ROWS - 1) * VT_COLS * 2; i < VT_ROWS * VT_COLS * 2; i += 2) {
            scr[i] = ' ';
            scr[i + 1] = v->color;
        }
        v->cursor_y = VT_ROWS - 1;
    }
}

void vt_putc(int vt_num, char c) {
    if (vt_num < 0 || vt_num >= VT_MAX) return;
    if (vts[vt_num].type == VT_UNUSED) return;

    if (vt_num == active_vt) {
        /* Active VT: write to VGA directly (it handles cursor, scroll).
         * Then sync the VT buffer from VGA on next switch. */
        vga_putc(c);
    } else {
        /* Inactive VT: write to shadow buffer only */
        vt_buf_putc(vt_num, c);
    }
}

void vt_puts(int vt_num, const char *s) {
    while (*s)
        vt_putc(vt_num, *s++);
}

void vt_set_color(int vt_num, uint8_t fg, uint8_t bg) {
    if (vt_num < 0 || vt_num >= VT_MAX) return;
    vts[vt_num].color = (bg << 4) | (fg & 0x0F);
    /* Always set VGA color too — it applies to future vga_putc calls */
    vga_set_color(fg, bg);
}

void vt_clear(int vt_num) {
    int i;
    struct vt *v;
    if (vt_num < 0 || vt_num >= VT_MAX) return;
    v = &vts[vt_num];

    for (i = 0; i < VT_BUF_SIZE; i += 2) {
        v->screen[i] = ' ';
        v->screen[i + 1] = v->color;
    }
    v->cursor_x = 0;
    v->cursor_y = 0;

    if (vt_num == active_vt)
        vga_clear();
}

void vt_set_cursor(int vt_num, uint8_t col, uint8_t row) {
    if (vt_num < 0 || vt_num >= VT_MAX) return;
    vts[vt_num].cursor_x = col;
    vts[vt_num].cursor_y = row;
    if (vt_num == active_vt)
        vga_set_cursor(col, row);
}

void vt_get_cursor(int vt_num, uint8_t *col, uint8_t *row) {
    if (vt_num < 0 || vt_num >= VT_MAX) return;
    *col = vts[vt_num].cursor_x;
    *row = vts[vt_num].cursor_y;
}

/* ================================================================
 * Per-VT keyboard queue
 * ================================================================ */

void vt_enqueue_key(int vt_num, struct key_event *ev) {
    struct vt *v;
    uint8_t next;

    if (vt_num < 0 || vt_num >= VT_MAX) return;
    v = &vts[vt_num];

    next = (v->key_head + 1) % VT_KEY_BUF;
    if (next == v->key_tail) return;  /* full, drop */

    v->key_buf[v->key_head] = *ev;
    v->key_head = next;

    /* Wake any task blocked on keyboard for this VT */
    sched_unblock(BLOCK_KEYBOARD, vt_num);
}

int vt_poll_key(int vt_num, struct key_event *ev) {
    struct vt *v;

    if (vt_num < 0 || vt_num >= VT_MAX) return 0;
    v = &vts[vt_num];

    if (v->key_head == v->key_tail) return 0;

    *ev = v->key_buf[v->key_tail];
    v->key_tail = (v->key_tail + 1) % VT_KEY_BUF;
    return 1;
}

/* ================================================================
 * VT manager task — handles Alt+C / Alt+X requests
 * ================================================================ */

void vt_manager_entry(void) {
    while (1) {
        sched_block(BLOCK_VT_REQUEST, 0);

        if (vt_request == VT_REQ_NEW_DOS) {
            vt_request = VT_REQ_NONE;
            /* Only allow one DOS VT at a time (they share conventional memory) */
            {
                int has_dos = 0, i;
                for (i = 0; i < VT_MAX; i++) {
                    struct vt *v = vt_get(i);
                    if (v && v->type == VT_DOS) { has_dos = 1; break; }
                }
                if (!has_dos && vt_count_active() < VT_MAX) {
                    vt_create_dos();
                }
            }
        } else if (vt_request == VT_REQ_NEW_SHELL) {
            vt_request = VT_REQ_NONE;
            if (vt_count_active() < VT_MAX) {
                vt_create_shell();
            }
        } else if (vt_request == VT_REQ_CLOSE) {
            int cur = vt_get_active();
            if (cur >= 0) {
                vt_destroy(cur);
                if (vt_count_active() == 0) {
                    serial_puts("VT: all terminals closed — halting\n");
                    __asm__ volatile("cli; hlt");
                }
            }
            vt_request = VT_REQ_NONE;
        }
    }
}

int vt_create_dos(void) {
    return vt_create_dos_exec("COMMAND.COM", "/P");
}

int vt_create_dos_exec(const char *binary, const char *args) {
    int vt_num, task_id;

    vt_num = vt_create(VT_DOS);
    if (vt_num < 0) {
        serial_puts("VT: no free VT slots\n");
        return -1;
    }

    task_id = sched_create_v86_exec(binary, vt_num, binary, args);
    if (task_id < 0) {
        vt_destroy(vt_num);
        serial_puts("VT: failed to create V86 task\n");
        return -1;
    }

    vts[vt_num].task_id = task_id;
    vt_switch(vt_num);

    serial_puts("VT: ");
    serial_puts(binary);
    serial_puts(" on vt");
    serial_puthex(vt_num);
    serial_puts("\n");

    return vt_num;
}

int vt_create_shell(void) {
    int vt_num, task_id;

    vt_num = vt_create(VT_SHELL);
    if (vt_num < 0) {
        serial_puts("VT: no free VT slots\n");
        return -1;
    }

    task_id = sched_create_kernel_task("pine-shell", shell_entry, vt_num);
    if (task_id < 0) {
        vt_destroy(vt_num);
        serial_puts("VT: failed to create shell task\n");
        return -1;
    }

    vts[vt_num].task_id = task_id;

    /* Switch to the new VT */
    vt_switch(vt_num);

    serial_puts("VT: Pinecore Commando on vt");
    serial_puthex(vt_num);
    serial_puts("\n");

    return vt_num;
}
