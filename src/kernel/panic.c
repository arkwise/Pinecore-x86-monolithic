/*
 * panic.c — Pinecore BSOD-style panic display.
 *
 * Writes directly to VGA text-mode memory at 0xB8000 (80x25, 2 bytes
 * per cell: char + attribute). Uses no other kernel subsystems so it
 * works even when serial / scheduler / VT / FAT etc. are in any
 * state. Halts forever after the display.
 *
 * VGA attribute byte layout:
 *   bits 0-3 = foreground colour
 *   bits 4-6 = background colour
 *   bit 7    = blink (we don't use)
 *
 * Colours used:
 *   white-on-red (0x4F) for the title bar
 *   white-on-blue (0x1F) for the body
 *   yellow-on-blue (0x1E) for register labels
 */

#include "types.h"
#include "panic.h"
#include "idt.h"
#include "serial.h"

#define VGA_BASE     ((volatile uint16_t *)0xB8000)
#define VGA_COLS     80
#define VGA_ROWS     25
#define ATTR_TITLE   0x4F   /* white on red */
#define ATTR_BODY    0x1F   /* white on blue */
#define ATTR_LABEL   0x1E   /* yellow on blue */
#define ATTR_BLANK   0x1F

static void put_char_at(int row, int col, uint8_t attr, char c) {
    if (row < 0 || row >= VGA_ROWS || col < 0 || col >= VGA_COLS) return;
    VGA_BASE[row * VGA_COLS + col] = ((uint16_t)attr << 8) | (uint8_t)c;
}

static void put_str_at(int row, int col, uint8_t attr, const char *s) {
    while (*s && col < VGA_COLS) {
        put_char_at(row, col, attr, *s);
        s++;
        col++;
    }
}

static void put_hex8_at(int row, int col, uint8_t attr, uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    put_char_at(row, col,     attr, hex[(v >> 4) & 0xF]);
    put_char_at(row, col + 1, attr, hex[v & 0xF]);
}

static void put_hex32_at(int row, int col, uint8_t attr, uint32_t v) {
    int i;
    for (i = 0; i < 4; i++) {
        put_hex8_at(row, col + i * 2, attr, (v >> ((3 - i) * 8)) & 0xFF);
    }
}

static void put_hex16_at(int row, int col, uint8_t attr, uint16_t v) {
    put_hex8_at(row, col,     attr, (v >> 8) & 0xFF);
    put_hex8_at(row, col + 2, attr, v & 0xFF);
}

static void fill_row(int row, uint8_t attr, char c) {
    int col;
    for (col = 0; col < VGA_COLS; col++) put_char_at(row, col, attr, c);
}

static void clear_screen(uint8_t attr) {
    int row;
    for (row = 0; row < VGA_ROWS; row++) fill_row(row, attr, ' ');
}

/* Read CR2 (page-fault address). Safe to call any time. */
static uint32_t read_cr2(void) {
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static const char *exc_name(uint32_t n) {
    static const char *names[] = {
        "Divide error",         "Debug",              "NMI",               "Breakpoint",
        "Overflow",             "BOUND",              "Invalid opcode",    "Device not available",
        "Double fault",         "Coproc seg overrun", "Invalid TSS",       "Segment not present",
        "Stack fault",          "General protection", "Page fault",        "Reserved",
        "x87 FP error",         "Alignment check",    "Machine check",     "SIMD FP error",
        "Virtualization",       "Control protection"
    };
    if (n < sizeof(names) / sizeof(names[0])) return names[n];
    return "Unknown";
}

void kernel_panic(const char *reason, void *isr_frame_ptr) {
    struct isr_frame *f = (struct isr_frame *)isr_frame_ptr;

    __asm__ volatile("cli");

    /* Also dump to serial for the lucky few with COM1 wired up. */
    serial_puts("\n\n*** PINECORE PANIC: ");
    if (reason) serial_puts(reason);
    serial_puts(" ***\n");
    if (f) {
        serial_puts("  INT="); serial_puthex(f->int_no);
        serial_puts(" ERR="); serial_puthex(f->err_code);
        serial_puts(" CS:EIP="); serial_puthex(f->cs);
        serial_puts(":"); serial_puthex(f->eip);
        serial_puts(" EFL="); serial_puthex(f->eflags);
        serial_puts("\n  EAX="); serial_puthex(f->eax);
        serial_puts(" EBX="); serial_puthex(f->ebx);
        serial_puts(" ECX="); serial_puthex(f->ecx);
        serial_puts(" EDX="); serial_puthex(f->edx);
        serial_puts("\n  ESI="); serial_puthex(f->esi);
        serial_puts(" EDI="); serial_puthex(f->edi);
        serial_puts(" EBP="); serial_puthex(f->ebp);
        if (f->int_no == 14) {
            serial_puts(" CR2="); serial_puthex(read_cr2());
        }
        serial_puts("\n");
    }

    /* Paint the screen. */
    clear_screen(ATTR_BODY);

    /* Title bar, row 0, red. */
    fill_row(0, ATTR_TITLE, ' ');
    put_str_at(0, 2,  ATTR_TITLE, "*** PINECORE PANIC ***");
    put_str_at(0, 50, ATTR_TITLE, "Kernel halted - cycle power");

    /* Reason, row 2. */
    if (reason) {
        put_str_at(2, 2, ATTR_BODY, "Reason: ");
        put_str_at(2, 10, ATTR_BODY, reason);
    }

    if (f) {
        /* Exception name, row 4. */
        put_str_at(4, 2, ATTR_LABEL, "Exception:");
        if (f->int_no < 32) {
            put_str_at(4, 14, ATTR_BODY, exc_name(f->int_no));
            put_str_at(4, 50, ATTR_LABEL, "INT 0x");
            put_hex8_at(4, 56, ATTR_BODY, (uint8_t)f->int_no);
            put_str_at(4, 60, ATTR_LABEL, "Err 0x");
            put_hex32_at(4, 66, ATTR_BODY, f->err_code);
        } else {
            put_str_at(4, 14, ATTR_BODY, "(non-CPU - kernel-raised)");
        }

        /* Code location, row 6. */
        put_str_at(6, 2, ATTR_LABEL, "CS:EIP =");
        put_hex16_at(6, 11, ATTR_BODY, (uint16_t)f->cs);
        put_str_at(6, 15, ATTR_BODY, ":");
        put_hex32_at(6, 16, ATTR_BODY, f->eip);
        put_str_at(6, 28, ATTR_LABEL, "EFLAGS =");
        put_hex32_at(6, 37, ATTR_BODY, f->eflags);

        /* Page fault CR2, row 7 if applicable. */
        if (f->int_no == 14) {
            put_str_at(7, 2, ATTR_LABEL, "CR2    =");
            put_hex32_at(7, 11, ATTR_BODY, read_cr2());
            put_str_at(7, 22, ATTR_BODY, "(faulting address)");
        }

        /* Register dump, rows 9-12. */
        put_str_at(9,  2, ATTR_LABEL, "EAX ="); put_hex32_at(9,  8, ATTR_BODY, f->eax);
        put_str_at(9, 20, ATTR_LABEL, "EBX ="); put_hex32_at(9, 26, ATTR_BODY, f->ebx);
        put_str_at(9, 38, ATTR_LABEL, "ECX ="); put_hex32_at(9, 44, ATTR_BODY, f->ecx);
        put_str_at(9, 56, ATTR_LABEL, "EDX ="); put_hex32_at(9, 62, ATTR_BODY, f->edx);

        put_str_at(10, 2, ATTR_LABEL, "ESI ="); put_hex32_at(10, 8,  ATTR_BODY, f->esi);
        put_str_at(10, 20, ATTR_LABEL, "EDI ="); put_hex32_at(10, 26, ATTR_BODY, f->edi);
        put_str_at(10, 38, ATTR_LABEL, "EBP ="); put_hex32_at(10, 44, ATTR_BODY, f->ebp);
        put_str_at(10, 56, ATTR_LABEL, "ESP ="); put_hex32_at(10, 62, ATTR_BODY, f->esp);

        put_str_at(11, 2, ATTR_LABEL, "DS  ="); put_hex32_at(11, 8,  ATTR_BODY, f->ds);
        put_str_at(11, 20, ATTR_LABEL, "ES  ="); put_hex32_at(11, 26, ATTR_BODY, f->es);
        put_str_at(11, 38, ATTR_LABEL, "FS  ="); put_hex32_at(11, 44, ATTR_BODY, f->fs);
        put_str_at(11, 56, ATTR_LABEL, "GS  ="); put_hex32_at(11, 62, ATTR_BODY, f->gs);

        put_str_at(12, 2, ATTR_LABEL, "SS  ="); put_hex32_at(12, 8, ATTR_BODY, f->ss);
    }

    /* Footer, row 23. */
    put_str_at(23, 2, ATTR_LABEL,
               "Pinecore halted. No automatic recovery. Cycle power or reset.");

    /* Halt forever. */
    while (1) __asm__ volatile("cli; hlt");
}

void kernel_panic_str(const char *reason) {
    kernel_panic(reason, 0);
}

/* Boot-watchdog panic. Paints reason + last klog_stage label on the
 * normal panic frame, leaves row 24 untouched (the live klog status
 * line already has the same label there). No register dump — this is
 * raised from inside the RTC IRQ where the frame is the RTC handler's,
 * not the wedged code's, so it would be misleading. */
void kernel_panic_watchdog(const char *last_stage) {
    __asm__ volatile("cli");

    serial_puts("\n\n*** PINECORE PANIC: WATCHDOG ***\n");
    serial_puts("  Kernel made no forward progress for the watchdog budget.\n");
    serial_puts("  Last stage: ");
    if (last_stage) serial_puts(last_stage);
    serial_puts("\n");

    clear_screen(ATTR_BODY);
    fill_row(0, ATTR_TITLE, ' ');
    put_str_at(0, 2,  ATTR_TITLE, "*** PINECORE PANIC ***");
    put_str_at(0, 50, ATTR_TITLE, "Kernel halted - cycle power");

    put_str_at(2, 2, ATTR_BODY, "Reason: WATCHDOG - no forward progress");
    put_str_at(4, 2, ATTR_LABEL, "Last stage:");
    if (last_stage) put_str_at(4, 14, ATTR_BODY, last_stage);

    put_str_at(6,  2, ATTR_BODY,
               "klog_stage() / klog_iter() did not fire within the timeout.");
    put_str_at(7,  2, ATTR_BODY,
               "The kernel is stuck in a loop or wedged interrupt handler.");
    put_str_at(9,  2, ATTR_LABEL, "Next step:");
    put_str_at(9, 14, ATTR_BODY,
               "row 24 still shows the live klog stage tag.");
    put_str_at(10, 14, ATTR_BODY,
               "Photograph row 24 + this banner before power-cycling.");

    put_str_at(23, 2, ATTR_LABEL,
               "Pinecore halted (watchdog). Cycle power.");

    while (1) __asm__ volatile("cli; hlt");
}
