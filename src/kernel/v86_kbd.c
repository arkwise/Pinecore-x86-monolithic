/*
 * v86_kbd.c — Path B: V86 keyboard polling task implementation.
 *
 * See v86_kbd.h for full design rationale. Quick summary:
 *
 *   - Vortex86 BIOS USB-legacy delivers via INT 09h hook in the
 *     real-mode IVT. Once we enter PM and our IDT replaces vector 9,
 *     the hook can never fire. IRQ 1 is silent for USB keystrokes.
 *   - But BIOS INT 16h still works when called from V86 (because
 *     V86 INT instructions traverse the real-mode IVT).
 *   - This module: a V86 task that loops calling BIOS INT 16h
 *     (via our custom INT 0xF8 monitor escape — IOPL is virtualized
 *     so we cannot bypass the monitor by other means) and pumps
 *     keystrokes into the kernel keyboard queue.
 *
 * Architecture:
 *   - Stub bytes copied to low memory at KBD_STUB_SEG:0
 *   - V86 task created via sched_create_v86_task_with_entry with a
 *     custom entry that calls v86_start(v86_id, KBD_STUB_SEG, 0)
 *   - Stub loops: INT 0xF8 (BIOS INT 16h check) → if key, INT 0xF8
 *     (BIOS INT 16h read) → write to mailbox → HLT (sleep until PIT
 *     wakes us) → repeat
 *   - v86_kbd_poll() drains the mailbox into the kernel queue;
 *     called from the PIT tick handler.
 */

#include "types.h"
#include "v86_kbd.h"
#include "keyboard.h"
#include "serial.h"
#include "sched.h"
#include "v86.h"

/* ============================================================
 * Real-mode stub  (loaded at KBD_STUB_SEG:0)
 * ============================================================
 *
 * Source (assembled by hand → bytes below):
 *
 *   bits 16
 *   org 0x0000
 *
 *   start:
 *       mov ax, KBD_STUB_SEG     ; 0x0080
 *       mov ds, ax
 *       mov es, ax
 *       mov ss, ax
 *       mov sp, 0x00F0           ; stack top, below mailbox
 *
 *   poll:
 *       mov ah, 0x11              ; INT 16h check-key (BIOS)
 *       int 0xF8                  ; monitor: reflect to IVT[0x16]
 *       jz  no_key
 *       mov ah, 0x10              ; INT 16h read-key (BIOS)
 *       int 0xF8                  ; monitor: reflect to IVT[0x16]
 *       mov [0x00F0], ax          ; mailbox AX = (scancode<<8)|ascii
 *       mov byte [0x00F2], 1      ; mailbox status = key ready
 *
 *   no_key:
 *       sti
 *       hlt                       ; sleep until next interrupt
 *       jmp poll
 *
 * Encoded bytes (~32 bytes — fits well inside the 256-byte segment):
 */
static const uint8_t kbd_stub[] = {
    /* mov ax, 0x0080      */  0xB8, 0x80, 0x00,
    /* mov ds, ax          */  0x8E, 0xD8,
    /* mov es, ax          */  0x8E, 0xC0,
    /* mov ss, ax          */  0x8E, 0xD0,
    /* mov sp, 0x00F0      */  0xBC, 0xF0, 0x00,

    /* poll: mov ah, 0x11  */  0xB4, 0x11,
    /* int 0xF8            */  0xCD, 0xF8,
    /* jz no_key (+7)      */  0x74, 0x07,
    /* mov ah, 0x10        */  0xB4, 0x10,
    /* int 0xF8            */  0xCD, 0xF8,
    /* mov [0x00F0], ax    */  0xA3, 0xF0, 0x00,
    /* mov [0x00F2], 1     */  0xC6, 0x06, 0xF2, 0x00, 0x01,

    /* no_key: sti         */  0xFB,
    /* hlt                 */  0xF4,
    /* jmp poll (-18)      */  0xEB, 0xEE
};

/* Set when v86_kbd_init successfully created the polling task. Poll
 * is a no-op until then so the PIT-tick hook is harmless even if
 * init failed or wasn't called. */
static int g_v86_kbd_ready;

/* ============================================================
 * V86 task entry — runs once when scheduler first picks the task.
 * v86_start() never returns (IRETs into V86 mode).
 * ============================================================ */
static void v86_kbd_task_entry(void) {
    struct task *self = sched_get_task(sched_current());
    int v86_id = self->v86_task_id;

    serial_puts("v86_kbd: task entry, v86_id=");
    serial_puthex(v86_id);
    serial_puts(" → V86 at ");
    serial_puthex(KBD_STUB_SEG);
    serial_puts(":0000\n");

    v86_start(v86_id, KBD_STUB_SEG, 0x0000);
    /* never returns — V86 task loops in the stub forever */
}

/* ============================================================
 * Public API
 * ============================================================ */

void v86_kbd_init(void) {
    int task_id;

    /* Copy stub into KBD_STUB_SEG. Low memory is identity-mapped. */
    {
        volatile uint8_t *dst = (volatile uint8_t *)KBD_STUB_LIN;
        unsigned i;
        for (i = 0; i < sizeof(kbd_stub); i++) {
            dst[i] = kbd_stub[i];
        }
    }

    /* Zero the mailbox. Status = 0 means "no key pending". */
    *(volatile uint16_t *)(KBD_MAILBOX_LIN + KBD_MAILBOX_AX)     = 0;
    *(volatile uint8_t  *)(KBD_MAILBOX_LIN + KBD_MAILBOX_STATUS) = 0;

    serial_puts("v86_kbd: stub placed at seg=");
    serial_puthex(KBD_STUB_SEG);
    serial_puts(" (");
    serial_puthex((uint32_t)sizeof(kbd_stub));
    serial_puts(" bytes) mailbox=");
    serial_puthex(KBD_MAILBOX_LIN);
    serial_puts("\n");

    /* Create the V86 task that will run the stub. */
    task_id = sched_create_v86_task_with_entry("kbd-poll", v86_kbd_task_entry, -1);
    if (task_id < 0) {
        serial_puts("v86_kbd: FAILED to create V86 task — USB keyboard polling will not work\n");
        return;
    }

    g_v86_kbd_ready = 1;
    serial_puts("v86_kbd: V86 BIOS-INT-16h polling task ready\n");
}

int v86_kbd_poll(void) {
    volatile uint8_t  *status = (volatile uint8_t  *)(KBD_MAILBOX_LIN + KBD_MAILBOX_STATUS);
    volatile uint16_t *ax     = (volatile uint16_t *)(KBD_MAILBOX_LIN + KBD_MAILBOX_AX);
    uint16_t v;
    uint8_t scancode, ascii;

    if (!g_v86_kbd_ready) return 0;
    if (*status == 0) return 0;

    v = *ax;
    scancode = (v >> 8) & 0xFF;
    ascii    = v & 0xFF;

    /* Clear mailbox BEFORE enqueue so the stub's next iteration
     * sees an empty slot and can write again. */
    *ax = 0;
    *status = 0;

    keyboard_inject_key(scancode, ascii);
    return 1;
}
