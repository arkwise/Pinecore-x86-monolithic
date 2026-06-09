/* keyboard.c -- PS/2 keyboard driver
 *
 * IRQ 1 (INT 33), reads scancodes from port 0x60
 * Scancode Set 1, converts to ASCII
 * Extended keys (0xE0 prefix) supported for arrows, Home/End, etc.
 * VT-mode routing: Alt+F1-F6 switches VTs, other keys go to active VT.
 *
 * (ch-13, ch-17)
 */

#include "types.h"
#include "io.h"
#include "pic.h"
#include "idt.h"
#include "keyboard.h"
#include "serial.h"
#include "vt.h"
#include "sched.h"

/* =========================================================================
 * Phase 4.6.5 — Keyboard layouts
 *
 * Tables for each layout are stored as 128-entry scancode→ASCII arrays.
 * The ISR uses `active_layout->normal[sc]` / `active_layout->shift[sc]`
 * (and `altgr` for AltGr-modified keys, future). Adding a layout = add
 * one more `static const char` array pair + `static const struct
 * keyboard_layout` instance below; register it in `layouts[]`.
 *
 * M1: US baked in as default (this file). M2 adds DE etc.
 * ========================================================================= */

/* ---- US English (QWERTY) ---- */
static const unsigned char layout_us_normal[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', 8,   /* 00-0E */
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\r',     /* 0F-1C */
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',         /* 1D-29 */
    0,   '\\','z','x','c','v','b','n','m',',','.','/',0,           /* 2A-36 */
    '*', 0,  ' ',                                                   /* 37-39 */
    0,0,0,0,0,0,0,0,0,0,                                           /* 3A-43: Caps,F1-F10 */
    0,0,                                                            /* 44-45: Num, Scroll */
    '7','8','9','-','4','5','6','+','1','2','3','0','.',            /* 46-53: KP */
    0,0,0,0,0,                                                      /* 54-58 */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                               /* 59-68 */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                               /* 69-78 */
    0,0,0,0,0,0,0                                                   /* 79-7F */
};

static const unsigned char layout_us_shift[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', 8,
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\r',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?',0,
    '*', 0,  ' ',
    0,0,0,0,0,0,0,0,0,0,
    0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0
};

static const struct keyboard_layout layout_us = {
    .id     = "us",
    .name   = "US English (QWERTY)",
    .normal = layout_us_normal,
    .shift  = layout_us_shift,
    .altgr  = 0,
};

/* ---- German (QWERTZ, CP-437 encoding) ----
 * Differences from US (notable):
 *   scancode 0x15: y → z          0x2C: z → y          (Y/Z swap)
 *   0x0C: -/_ → ß/?  (ß = 0xE1 CP-437)
 *   0x1A: [/{ → ü/Ü  (ü=0x81, Ü=0x9A)
 *   0x1B: ]/} → +/*
 *   0x27: ;/: → ö/Ö  (ö=0x94, Ö=0x99)
 *   0x28: '/" → ä/Ä  (ä=0x84, Ä=0x8E)
 *   0x29: `/~ → ^/°  (^ is a dead key on real DE, output literal for now)
 *   0x2B: \/| → #/'
 *   0x0D: =/+ → ´/`  (acute/grave dead keys, output literal for now)
 *   0x33-0x35: ,. — same; - moved to 0x35
 * AltGr layer (M3+ work): {/[/]/}/\@/€/µ etc. — not implemented in M2. */
static const unsigned char layout_de_normal[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0',0xE1,'\'', 8, /* 00-0E (ß, ´ as ') */
    '\t','q','w','e','r','t','z','u','i','o','p',0x81,'+','\r',    /* 0F-1C (ü, +) */
    0,   'a','s','d','f','g','h','j','k','l',0x94,0x84,'^',        /* 1D-29 (ö, ä, ^) */
    0,   '#','y','x','c','v','b','n','m',',','.','-',0,            /* 2A-36 (#, swapped y) */
    '*', 0,  ' ',
    0,0,0,0,0,0,0,0,0,0,
    0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0
};

static const unsigned char layout_de_shift[128] = {
    0,   27, '!','"',0xF5,'$','%','&','/','(',')','=','?','`', 8,  /* (§=0xF5 CP-437 alt, `) */
    '\t','Q','W','E','R','T','Z','U','I','O','P',0x9A,'*','\r',    /* (Ü, *) */
    0,   'A','S','D','F','G','H','J','K','L',0x99,0x8E,0xF8,       /* (Ö, Ä, °=0xF8) */
    0,   '\'','Y','X','C','V','B','N','M',';',':','_',0,           /* (Y, _ etc.) */
    '*', 0,  ' ',
    0,0,0,0,0,0,0,0,0,0,
    0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0
};

static const struct keyboard_layout layout_de = {
    .id     = "de",
    .name   = "German (QWERTZ)",
    .normal = layout_de_normal,
    .shift  = layout_de_shift,
    .altgr  = 0,
};

/* ---- Registry: keyboard_set_layout() looks up by ID here ---- */
static const struct keyboard_layout *const layouts[] = {
    &layout_us,
    &layout_de,
};
#define N_LAYOUTS (sizeof(layouts) / sizeof(layouts[0]))

/* ---- Active layout pointer — swapped at runtime by keyboard_set_layout() ---- */
static const struct keyboard_layout *active_layout = &layout_us;

int keyboard_set_layout(const char *id) {
    unsigned i;
    if (!id) return -1;
    for (i = 0; i < N_LAYOUTS; i++) {
        const char *lid = layouts[i]->id;
        if (lid[0] == id[0] && lid[1] == id[1] &&
            (id[2] == 0 || id[2] == ' ' || id[2] == '\n')) {
            active_layout = layouts[i];
            return 0;
        }
    }
    return -1;
}

const char *keyboard_get_layout_id(void)   { return active_layout->id; }
const char *keyboard_get_layout_name(void) { return active_layout->name; }

int keyboard_layout_count(void) { return (int)N_LAYOUTS; }

const struct keyboard_layout *keyboard_layout_at(int i) {
    if (i < 0 || (unsigned)i >= N_LAYOUTS) return 0;
    return layouts[i];
}

/* Key state array */
static uint8_t key_state[256];
static uint8_t shift_flags;      /* BIOS 0x40:0x17 — see keyboard.h MOD_* */
static uint8_t ext_shift_flags;  /* BIOS 0x40:0x18 — L/R Ctrl/Alt distinction */
static uint8_t extended_prefix;  /* 1 = next scancode is extended (0xE0) */
static int vt_mode;              /* 1 = route keys to VT system */

/* s42 — last raw scancode read from port 0x60, exposed for the V86
 * monitor's port 0x60 IN emulation. SETUP.EXE and similar 1994-era
 * apps that bypass INT 16h read this port directly inside their
 * hooked INT 9 ISR. Since the real port has already been consumed by
 * our kernel ISR, we cache here and the V86 trap returns this. */
volatile uint8_t v86_last_scancode = 0;

/* s50 Vortex86 USB-kbd diagnostic — counts IRQ 1 entries so we can
 * tell from the bottom-right of the VGA screen whether the BIOS's
 * USB-legacy emulation is actually delivering scancodes via IRQ 1.
 * If `kbd_irq_count` ticks when a USB key is pressed on real
 * hardware, the IRQ path works (and our routing has the bug). If it
 * doesn't tick, the BIOS isn't injecting (then the INT-09-hook-in-PM
 * theory wins and we need V86 or native USB). */
static volatile uint16_t kbd_irq_count;

/* Mirror state into BDA so V86 apps that peek 0x40:0x17 / 0x18 directly
 * (instead of calling INT 16h AH=0x02/0x12) see the same value. */
static void publish_shift_state(void) {
    *(volatile uint8_t *)0x417 = shift_flags;
    *(volatile uint8_t *)0x418 = ext_shift_flags;
}

/* Global ring buffer (used before VT mode is active) */
#define KEY_BUF_SIZE 64
static struct key_event key_buffer[KEY_BUF_SIZE];
volatile uint8_t key_head;
volatile uint8_t key_tail;

/* Modifier scancodes */
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL   0x1D   /* LCtrl if plain; RCtrl if preceded by 0xE0 */
#define SC_ALT    0x38   /* LAlt if plain; RAlt (AltGr) if preceded by 0xE0 */
#define SC_CAPS   0x3A
#define SC_NUM    0x45
#define SC_SCROLL 0x46
#define SC_INS    0x52   /* Insert (also extended on enhanced kbd) */
#define SC_C      0x2E   /* 'C' key */
#define SC_X      0x2D   /* 'X' key */
#define SC_N      0x31   /* 'N' key */

/* Returns 1 if scancode is a pure modifier/toggle key — should not
 * be enqueued as a deliverable INT 16h event (real BIOS doesn't either). */
static int is_modifier_sc(uint8_t sc) {
    switch (sc) {
    case SC_LSHIFT: case SC_RSHIFT:
    case SC_CTRL:   case SC_ALT:
    case SC_CAPS:   case SC_NUM:   case SC_SCROLL:
        return 1;
    }
    return 0;
}

static void update_modifiers(uint8_t sc, int pressed, int is_extended) {
    switch (sc) {
    case SC_LSHIFT:
        if (pressed) shift_flags |= MOD_LSHIFT;
        else         shift_flags &= ~MOD_LSHIFT;
        break;
    case SC_RSHIFT:
        if (pressed) shift_flags |= MOD_RSHIFT;
        else         shift_flags &= ~MOD_RSHIFT;
        break;
    case SC_CTRL:
        if (is_extended) {
            if (pressed) ext_shift_flags |= MOD_EXT_RCTRL;
            else         ext_shift_flags &= ~MOD_EXT_RCTRL;
        } else {
            if (pressed) ext_shift_flags |= MOD_EXT_LCTRL;
            else         ext_shift_flags &= ~MOD_EXT_LCTRL;
        }
        if (ext_shift_flags & (MOD_EXT_LCTRL | MOD_EXT_RCTRL))
            shift_flags |= MOD_ANY_CTRL;
        else
            shift_flags &= ~MOD_ANY_CTRL;
        break;
    case SC_ALT:
        if (is_extended) {
            if (pressed) ext_shift_flags |= MOD_EXT_RALT;
            else         ext_shift_flags &= ~MOD_EXT_RALT;
        } else {
            if (pressed) ext_shift_flags |= MOD_EXT_LALT;
            else         ext_shift_flags &= ~MOD_EXT_LALT;
        }
        if (ext_shift_flags & (MOD_EXT_LALT | MOD_EXT_RALT))
            shift_flags |= MOD_ANY_ALT;
        else
            shift_flags &= ~MOD_ANY_ALT;
        break;
    case SC_CAPS:
        if (pressed) shift_flags ^= MOD_CAPS_TOG;
        break;
    case SC_NUM:
        if (pressed) shift_flags ^= MOD_NUM_TOG;
        break;
    case SC_SCROLL:
        if (pressed) shift_flags ^= MOD_SCROLL_TOG;
        break;
    case SC_INS:
        if (pressed) shift_flags ^= MOD_INS_TOG;
        break;
    }
    publish_shift_state();
}

static void enqueue_global(uint8_t scancode, uint8_t ascii, uint8_t mods, uint8_t pressed) {
    uint8_t next = (key_head + 1) % KEY_BUF_SIZE;
    if (next == key_tail) return;
    key_buffer[key_head].scancode  = scancode;
    key_buffer[key_head].ascii     = ascii;
    key_buffer[key_head].modifiers = mods;
    key_buffer[key_head].pressed   = pressed;
    key_head = next;
}

static void keyboard_isr(uint32_t int_no, uint32_t err_code,
                          uint32_t eip, uint32_t cs, uint32_t eflags) {
    uint8_t scancode, sc, ascii;
    int pressed;
    struct key_event ev;

    (void)int_no; (void)err_code; (void)eip; (void)cs; (void)eflags;

    scancode = inb(0x60);

    /* s50 Vortex86 diagnostic — paint a "IRQ=NNNN sc=XX" badge in the
     * bottom-right corner of the VGA text screen on every IRQ 1.
     * Direct VGA write avoids any dependency on the kernel's higher
     * layers. White-on-red so it's unmissable. */
    {
        volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
        static const char hex[] = "0123456789ABCDEF";
        int pos = 24 * 80 + 60;
        uint16_t c = ++kbd_irq_count;
        vga[pos+0]  = 0x4F00 | 'I';
        vga[pos+1]  = 0x4F00 | 'R';
        vga[pos+2]  = 0x4F00 | 'Q';
        vga[pos+3]  = 0x4F00 | '=';
        vga[pos+4]  = 0x4F00 | hex[(c>>12)&0xF];
        vga[pos+5]  = 0x4F00 | hex[(c>>8)&0xF];
        vga[pos+6]  = 0x4F00 | hex[(c>>4)&0xF];
        vga[pos+7]  = 0x4F00 | hex[c&0xF];
        vga[pos+8]  = 0x4F00 | ' ';
        vga[pos+9]  = 0x4F00 | 's';
        vga[pos+10] = 0x4F00 | 'c';
        vga[pos+11] = 0x4F00 | '=';
        vga[pos+12] = 0x4F00 | hex[(scancode>>4)&0xF];
        vga[pos+13] = 0x4F00 | hex[scancode&0xF];
    }
    serial_puts("KBD:IRQ sc=");
    serial_puthex(scancode);
    serial_puts("\n");

    /* s42 — cache the raw scancode for V86 port 0x60 read trap.
     * SETUP.EXE and similar 1994-era apps hook INT 9 and execute
     * `IN AL, 0x60` to read the scancode directly. Since we've
     * already consumed it here, the V86 monitor's port 0x60 IN
     * emulation returns this cached value instead. Single-slot
     * buffer; fast typing past one V86 ISR pass will overwrite. */
    v86_last_scancode = scancode;

    /* Handle 0xE0 extended key prefix */
    if (scancode == 0xE0) {
        extended_prefix = 1;
        pic_eoi(IRQ_KEYBOARD);
        return;
    }

    pressed = !(scancode & 0x80);
    sc = scancode & 0x7F;

    {
        int is_extended = extended_prefix;
        extended_prefix = 0;

        /* Update key state and modifiers — needs is_extended to disambiguate
         * LCtrl (plain 1D) from RCtrl (E0 1D), and LAlt from RAlt (AltGr). */
        key_state[sc] = pressed;
        update_modifiers(sc, pressed, is_extended);

        /* Mark extended keys in the scancode delivered to the VT/global queue */
        if (is_extended) sc |= KEY_EXTENDED;
    }

    /* Get ASCII (0 for non-printable/extended keys).
     * Lookup uses raw scancode (no KEY_EXTENDED bit) on shifted vs unshifted
     * table, then apply CapsLock, then Alt suppression / Ctrl combining.
     * Table source: active_layout — defaults to US, swapped via
     * keyboard_set_layout() (M2+). */
    if ((sc & ~KEY_EXTENDED) < 128 && !(sc & KEY_EXTENDED)) {
        uint8_t raw = sc;
        int use_shift = !!(shift_flags & MOD_SHIFT);
        ascii = use_shift ? active_layout->shift[raw]
                          : active_layout->normal[raw];

        /* CapsLock affects letters only — invert the shift selection for a-z */
        if (shift_flags & MOD_CAPS_TOG) {
            if (!use_shift && ascii >= 'a' && ascii <= 'z') ascii -= 32;
            else if (use_shift && ascii >= 'A' && ascii <= 'Z') ascii += 32;
        }

        /* Alt+key: deliver scancode + ASCII=0 (menu activation) */
        if (shift_flags & MOD_ANY_ALT) {
            ascii = 0;
        }
        /* Ctrl+letter: deliver ASCII = letter & 0x1F (Ctrl+A=0x01 ... Ctrl+Z=0x1A) */
        else if (shift_flags & MOD_ANY_CTRL) {
            if (ascii >= 'a' && ascii <= 'z')      ascii = ascii - 'a' + 1;
            else if (ascii >= 'A' && ascii <= 'Z') ascii = ascii - 'A' + 1;
        }
    } else {
        ascii = 0;
    }

    /* === VT-mode hotkey interception ===
     * Use raw scancode for the comparison (strip KEY_EXTENDED). */
    if (vt_mode && pressed) {
        uint8_t raw = sc & ~KEY_EXTENDED;
        /* Ctrl+1-6: switch VT directly */
        if ((shift_flags & MOD_ANY_CTRL) && raw >= 0x02 && raw <= 0x07) {
            int target_vt = raw - 0x02;
            struct vt *v = vt_get(target_vt);
            if (v) vt_switch(target_vt);
            pic_eoi(IRQ_KEYBOARD);
            return;
        }

        /* Ctrl+C or Alt+C: request new COMMAND.COM VT */
        if ((shift_flags & (MOD_ANY_CTRL | MOD_ANY_ALT)) && raw == SC_C) {
            vt_request = VT_REQ_NEW_DOS;
            sched_unblock(BLOCK_VT_REQUEST, 0);
            pic_eoi(IRQ_KEYBOARD);
            return;
        }

        /* Ctrl+N or Alt+N: request new Pinecore Commando VT */
        if ((shift_flags & (MOD_ANY_CTRL | MOD_ANY_ALT)) && raw == SC_N) {
            vt_request = VT_REQ_NEW_SHELL;
            sched_unblock(BLOCK_VT_REQUEST, 0);
            pic_eoi(IRQ_KEYBOARD);
            return;
        }

        /* Ctrl+X or Alt+X: request close current VT */
        if ((shift_flags & (MOD_ANY_CTRL | MOD_ANY_ALT)) && raw == SC_X) {
            vt_request = VT_REQ_CLOSE;
            sched_unblock(BLOCK_VT_REQUEST, 0);
            pic_eoi(IRQ_KEYBOARD);
            return;
        }
    }

    /* Don't enqueue pure modifier/toggle scancodes — they only update state.
     * Real BIOS doesn't deliver these via INT 16h either. Was causing EDIT
     * to see scancode 0x2A (LShift) as a real keypress (session 24). */
    if (is_modifier_sc(sc & ~KEY_EXTENDED)) {
        pic_eoi(IRQ_KEYBOARD);
        return;
    }

    /* Build event */
    ev.scancode = sc;
    ev.ascii = ascii;
    ev.modifiers = shift_flags;
    ev.pressed = pressed;

    /* Route to VT or global buffer */
    if (vt_mode) {
        int active = vt_get_active();
        if (active >= 0)
            vt_enqueue_key(active, &ev);
    } else {
        enqueue_global(ev.scancode, ev.ascii, ev.modifiers, ev.pressed);
    }

    /* s42 — also write to BIOS Data Area keyboard buffer at
     * 0040:001E. Many DOS apps (DOOM SETUP.EXE, vintage games) bypass
     * INT 16h and poll the BDA buffer head/tail at 0040:001A/0040:001C
     * directly. Standard layout (Phoenix BIOS / IBM PC AT):
     *   0040:001A  head pointer (offset within BDA segment)
     *   0040:001C  tail pointer
     *   0040:001E  buffer start (32-byte circular, 16 word entries)
     *   each entry = (AH=scancode, AL=ASCII)
     * We only enqueue PRESS events with a non-zero scancode (matches
     * BIOS behaviour — release events aren't buffered). Extended keys
     * use the base scancode in AH (e.g. Up arrow → AH=0x48, AL=0). */
    if (pressed && (ev.scancode || ev.ascii)) {
        volatile uint16_t *bda_head = (volatile uint16_t *)0x41A;
        volatile uint16_t *bda_tail = (volatile uint16_t *)0x41C;
        uint16_t head = *bda_head;
        uint16_t tail = *bda_tail;
        uint16_t next_tail = tail + 2;
        if (next_tail >= 0x3E) next_tail = 0x1E;  /* wrap */
        if (next_tail != head) {  /* not full */
            uint8_t bda_sc = ev.scancode & ~KEY_EXTENDED;
            uint8_t bda_ch = ev.ascii;
            volatile uint16_t *slot = (volatile uint16_t *)(0x400 + tail);
            *slot = ((uint16_t)bda_sc << 8) | bda_ch;
            *bda_tail = next_tail;
        }
    }

    pic_eoi(IRQ_KEYBOARD);
}

void keyboard_init(void) {
    int i;
    key_head = 0;
    key_tail = 0;
    shift_flags = 0;
    ext_shift_flags = 0;
    extended_prefix = 0;
    vt_mode = 0;
    publish_shift_state();

    /* s42 — initialise BIOS Data Area keyboard buffer pointers so
     * direct-BDA-polling apps (DOOM SETUP, etc.) see a valid empty
     * buffer at boot. Layout (offsets within seg 0x40):
     *   0x80 / 0x82  — buffer-start / buffer-end pointers
     *   0x1A / 0x1C  — head / tail (both = start when empty) */
    *(volatile uint16_t *)0x480 = 0x001E;
    *(volatile uint16_t *)0x482 = 0x003E;
    *(volatile uint16_t *)0x41A = 0x001E;
    *(volatile uint16_t *)0x41C = 0x001E;

    for (i = 0; i < 256; i++)
        key_state[i] = 0;

    while (inb(0x64) & 0x01)
        inb(0x60);

    isr_register(33, keyboard_isr);
    pic_unmask(IRQ_KEYBOARD);

    serial_puts("Keyboard: PS/2 driver installed (IRQ 1)\n");
}

/* Path B (s51 Vortex86 USB) — inject a synthetic keystroke from the
 * V86 BIOS-INT-16h polling task into the kernel queue. The BIOS has
 * already cooked the scancode + ASCII (handled USB-legacy translation,
 * shift state, etc.), so we don't re-process — just enqueue.
 *
 * Routes the same way the IRQ-1 path routes: VT-mode → active VT,
 * else → global queue. Also writes to the BDA keyboard buffer so V86
 * apps that peek the buffer directly (DOOM SETUP etc, see s42) still
 * see the key when BIOS USB-legacy is the source. */
void keyboard_inject_key(uint8_t scancode, uint8_t ascii) {
    if (vt_mode) {
        struct key_event ev;
        int active = vt_get_active();
        ev.scancode  = scancode;
        ev.ascii     = ascii;
        ev.modifiers = shift_flags;
        ev.pressed   = 1;
        if (active >= 0) {
            vt_enqueue_key(active, &ev);
        }
    } else {
        enqueue_global(scancode, ascii, shift_flags, 1);
    }

    /* BDA mirror — same logic as in keyboard_isr (post-scancode path). */
    if (scancode || ascii) {
        volatile uint16_t *bda_head = (volatile uint16_t *)0x41A;
        volatile uint16_t *bda_tail = (volatile uint16_t *)0x41C;
        uint16_t head = *bda_head;
        uint16_t tail = *bda_tail;
        uint16_t next_tail = tail + 2;
        if (next_tail >= 0x3E) next_tail = 0x1E;
        if (next_tail != head) {
            volatile uint16_t *slot = (volatile uint16_t *)(0x400 + tail);
            *slot = ((uint16_t)scancode << 8) | ascii;
            *bda_tail = next_tail;
        }
    }
}

/*  — multi-byte scancode sequence injection for USB HID.
 *
 * Walks the byte sequence applying the same E0/break-bit logic the
 * IRQ-1 path uses, then resolves ASCII via the active layout for the
 * trailing non-prefix byte. Each byte is enqueued in order with the
 * correct pressed flag. (doc 52 §10) */
void keyboard_inject_scancode_sequence(const uint8_t *seq, int n) {
    int    i;
    int    is_extended = 0;       /* local — don't disturb IRQ's persistent prefix */
    uint8_t ascii      = 0;

    if (!seq || n <= 0) return;

    for (i = 0; i < n; i++) {
        uint8_t  b = seq[i];
        uint8_t  sc;
        int      pressed;

        if (b == 0xE0) {
            is_extended = 1;
            continue;
        }

        pressed = !(b & 0x80);
        sc      = b & 0x7F;
        key_state[sc] = pressed;
        update_modifiers(sc, pressed, is_extended);

        ascii = 0;
        if (!is_extended && sc < 128) {
            int use_shift = !!(shift_flags & MOD_SHIFT);
            ascii = use_shift ? active_layout->shift[sc]
                              : active_layout->normal[sc];
            if (shift_flags & MOD_CAPS_TOG) {
                if (!use_shift && ascii >= 'a' && ascii <= 'z') ascii -= 32;
                else if (use_shift && ascii >= 'A' && ascii <= 'Z') ascii += 32;
            }
            if (shift_flags & MOD_ANY_ALT) {
                ascii = 0;
            } else if (shift_flags & MOD_ANY_CTRL) {
                if (ascii >= 'a' && ascii <= 'z')      ascii = ascii - 'a' + 1;
                else if (ascii >= 'A' && ascii <= 'Z') ascii = ascii - 'A' + 1;
            }
        }

        {
            uint8_t marked_sc = is_extended ? (sc | KEY_EXTENDED) : sc;
            if (vt_mode) {
                struct key_event ev;
                int active = vt_get_active();
                ev.scancode  = marked_sc;
                ev.ascii     = ascii;
                ev.modifiers = shift_flags;
                ev.pressed   = (uint8_t)pressed;
                if (active >= 0) vt_enqueue_key(active, &ev);
            } else {
                enqueue_global(marked_sc, ascii, shift_flags, (uint8_t)pressed);
            }
        }

        /* BDA mirror — makes only, V86 INT 16h reads {sc, ascii}. */
        if (pressed && (sc || ascii)) {
            volatile uint16_t *bda_head = (volatile uint16_t *)0x41A;
            volatile uint16_t *bda_tail = (volatile uint16_t *)0x41C;
            uint16_t head = *bda_head;
            uint16_t tail = *bda_tail;
            uint16_t next_tail = tail + 2;
            if (next_tail >= 0x3E) next_tail = 0x1E;
            if (next_tail != head) {
                volatile uint16_t *slot = (volatile uint16_t *)(0x400 + tail);
                *slot = ((uint16_t)sc << 8) | ascii;
                *bda_tail = next_tail;
            }
        }

        is_extended = 0;
    }
}

int keyboard_poll(struct key_event *event) {
    if (key_head == key_tail) return 0;
    *event = key_buffer[key_tail];
    key_tail = (key_tail + 1) % KEY_BUF_SIZE;
    return 1;
}

int keyboard_key_down(uint8_t scancode) {
    return key_state[scancode & 0x7F];
}

void keyboard_set_vt_mode(int enabled) {
    vt_mode = enabled;
}

uint8_t keyboard_get_shift_flags(void) {
    return shift_flags;
}

uint8_t keyboard_get_ext_flags(void) {
    return ext_shift_flags;
}
