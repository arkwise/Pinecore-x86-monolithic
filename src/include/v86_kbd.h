/*
 * v86_kbd.h — Path B: V86 keyboard polling task.
 *
 * Background (s50 Vortex86 USB-keyboard A1 finding):
 * --------------------------------------------------
 * On Vortex86SX (and probably other embedded boards), the BIOS's
 * USB-legacy keyboard emulation hooks INT 09h in the real-mode IVT
 * rather than using SMM injection. Once Pinecore enters PM and our
 * IDT replaces vector 9, USB keystrokes have nowhere to land — IRQ 1
 * never fires, our keyboard_isr never runs, the kernel queue stays
 * empty. FreeDOS on the same hardware works fine because it stays in
 * real mode and calls BIOS INT 16h which delivers BIOS-handled keys.
 *
 * Path B: run a small V86 task that polls BIOS INT 16h, captures
 * keystrokes, and feeds them into the kernel's keyboard queue. Same
 * BIOS path FreeDOS uses → same USB-legacy support, no native USB
 * driver required.
 *
 * Crucial detail: the V86 task runs at IOPL=3 so its INT 16h
 * executes via the real-mode IVT directly (BIOS handler) rather than
 * #GP-ing into our V86 monitor's INT 16h emulator at v86.c:514
 * (which would read from our empty queue and return nothing).
 *
 * Architecture:
 *   - RM stub (~30 bytes) at fixed low-memory segment KBD_STUB_SEG
 *   - Mailbox at KBD_MAILBOX_LIN: AX (2 bytes) + status byte
 *   - V86 task created with EFLAGS = 0x23202 (VM=1, IOPL=3, IF=1)
 *   - Stub loops: INT 16h AH=0x11 (check), if key INT 16h AH=0x10
 *     (read), write AX to mailbox, set status=1, HLT (waits for PIT
 *     wakeup, then jmp loop)
 *   - Kernel-side v86_kbd_poll() drains mailbox into the standard
 *     keyboard enqueue path so existing key consumers (shell, VTs,
 *     V86 DOS apps via our INT 16h emulation) all see USB keys.
 *
 * Called from the PIT tick or a dedicated kernel polling task.
 */
#ifndef PINECORE_V86_KBD_H
#define PINECORE_V86_KBD_H

#include "types.h"

/* Low-memory layout for the V86 keyboard polling task.
 * Segment 0x0080 (linear 0x800) is past the BIOS Data Area (0x400-
 * 0x4FF), the PINE.COM return-state save (0x500-0x50F), and well
 * below the first conventional-memory segment (0x1000+). Reserved
 * 256 bytes are plenty for the ~30-byte stub + stack + mailbox. */
#define KBD_STUB_SEG       0x0080
#define KBD_STUB_LIN       ((uint32_t)KBD_STUB_SEG << 4)
#define KBD_STUB_SIZE      256

/* Mailbox sits inside the same segment so the V86 stub can address
 * it as `[ds:offset]` without any segment-arithmetic. */
#define KBD_MAILBOX_OFF    0x00F0
#define KBD_MAILBOX_LIN    (KBD_STUB_LIN + KBD_MAILBOX_OFF)
#define KBD_MAILBOX_AX     0  /* +0..+1: scancode (hi) + ascii (lo) */
#define KBD_MAILBOX_STATUS 2  /* +2: 0 = empty, 1 = key ready */

/* Initialize the V86 keyboard polling task. Call once at kernel
 * boot, after keyboard_init and sched_init. */
void v86_kbd_init(void);

/* Drain any pending keystroke from the mailbox into the kernel's
 * keyboard queue. Safe to call from interrupt context (PIT tick) or
 * a polling kernel task. Returns 1 if a key was drained, 0 otherwise. */
int v86_kbd_poll(void);

#endif
