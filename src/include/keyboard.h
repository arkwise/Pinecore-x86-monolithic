#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

/* Modifier flags — BIOS 0x40:0x17 layout (low byte) */
#define MOD_RSHIFT     0x01
#define MOD_LSHIFT     0x02
#define MOD_ANY_CTRL   0x04
#define MOD_ANY_ALT    0x08
#define MOD_SCROLL_TOG 0x10
#define MOD_NUM_TOG    0x20
#define MOD_CAPS_TOG   0x40
#define MOD_INS_TOG    0x80

/* Legacy aliases — preserve "any of L/R is held" checks elsewhere */
#define MOD_SHIFT      (MOD_LSHIFT | MOD_RSHIFT)
#define MOD_CTRL       MOD_ANY_CTRL
#define MOD_ALT        MOD_ANY_ALT

/* Extended modifier flags — BIOS 0x40:0x18 layout */
#define MOD_EXT_LCTRL  0x01
#define MOD_EXT_LALT   0x02
#define MOD_EXT_RCTRL  0x04
#define MOD_EXT_RALT   0x08

/* Key event from the keyboard queue */
struct key_event {
    uint8_t scancode;
    uint8_t ascii;
    uint8_t modifiers;
    uint8_t pressed;   /* 1 = press, 0 = release */
};

/* Keyboard layout — scan-code → character tables for each shift state.
 * Phase 4.6.5 M1: structural refactor only; M2 adds DE; M3+ adds AltGr,
 * dead keys, etc. The ISR (`keyboard.c`) holds an `active_layout` pointer
 * that defaults to US and is swapped by `keyboard_set_layout()`. */
struct keyboard_layout {
    char                  id[3];   /* "us\0", "de\0", "uk\0", ... */
    const char           *name;    /* display name, e.g. "US English (QWERTY)" */
    const unsigned char  *normal;  /* 128-entry table, unshifted per scancode */
    const unsigned char  *shift;   /* 128-entry table, shifted per scancode */
    const unsigned char  *altgr;   /* 128-entry table for AltGr — NULL if unused */
};

/* Public layout API (Phase 4.6.5 M2+) */
int  keyboard_set_layout(const char *id);          /* 0 on success, -1 if not found */
const char *keyboard_get_layout_id(void);
const char *keyboard_get_layout_name(void);
int  keyboard_layout_count(void);
const struct keyboard_layout *keyboard_layout_at(int i);  /* NULL if i out of range */

/* Extended key flag — OR'd into scancode for 0xE0-prefixed keys */
#define KEY_EXTENDED 0x80

/* F-key scancodes (NOT extended, standard set 1) */
#define KEY_F1  0x3B
#define KEY_F2  0x3C
#define KEY_F3  0x3D
#define KEY_F4  0x3E
#define KEY_F5  0x3F
#define KEY_F6  0x40

void keyboard_init(void);
int  keyboard_poll(struct key_event *event);
int  keyboard_key_down(uint8_t scancode);

/* Path B (s51 Vortex86 USB) — inject a synthetic keystroke into the
 * kernel queue from a non-IRQ-1 source (currently the V86 BIOS-INT-16h
 * polling task in src/kernel/v86_kbd.c). Uses the same enqueue path
 * the IRQ handler uses so all existing consumers (shell, VTs, V86
 * INT 16h emulator) see the key regardless of whether it came from a
 * PS/2 IRQ or BIOS USB-legacy. Must NOT be called from inside the
 * PS/2 IRQ handler — that path enqueues directly. */
void keyboard_inject_key(uint8_t scancode, uint8_t ascii);

/*  — multi-byte scancode injection for USB HID (doc 52 §10).
 *
 * Some keys in AT scancode set 1 are E0-prefixed (PrtSc, arrows, KP
 * Enter, etc.) or E1-prefixed (Pause). hid.kmd looks up each HID Usage
 * in its 256-entry table and may produce a 1-, 2-, or 3-byte sequence.
 * This helper enqueues the sequence in order so V86 INT 16h apps and
 * the BDA buffer see the full prefix-trailer pair.
 *
 *   seq : pointer to N scancode bytes (1..3)
 *   n   : number of bytes
 *
 * ASCII is computed by the active layout from the trailing byte; all
 * leading prefix bytes are enqueued with ascii=0.
 *
 * Like keyboard_inject_key, this is the make/press path — the high
 * bit of the trailing scancode encodes break per AT Set 1 (bit 7 = 1).
 * hid.kmd ORs 0x80 into the trailing byte to emit a key release. */
void keyboard_inject_scancode_sequence(const uint8_t *seq, int n);

/* Modifier state — used by V86 INT 16h AH=0x02 / AH=0x12 */
uint8_t keyboard_get_shift_flags(void);
uint8_t keyboard_get_ext_flags(void);

/* Enable/disable VT-mode keyboard routing */
void keyboard_set_vt_mode(int enabled);

#endif
