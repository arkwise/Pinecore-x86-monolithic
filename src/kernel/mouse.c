/* mouse.c -- PS/2 mouse driver
 *
 * IRQ 12 (INT 44), shares 8042 controller with keyboard
 * 3-byte packet protocol
 * (ch-13)
 */

#include "types.h"
#include "io.h"
#include "pic.h"
#include "idt.h"
#include "mouse.h"
#include "serial.h"

static volatile int pos_x;
static volatile int pos_y;
static volatile int buttons;
static int bound_w = 640;
static int bound_h = 480;

static int cycle;
static uint8_t packet[3];

static void kbd_wait_write(void) {
    int timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout)
        ;
}

static void kbd_wait_read(void) {
    int timeout = 100000;
    while (!(inb(0x64) & 0x01) && --timeout)
        ;
}

static void mouse_write(uint8_t data) {
    kbd_wait_write();
    outb(0x64, 0xD4);   /* prefix: next byte goes to mouse */
    kbd_wait_write();
    outb(0x60, data);
}

static uint8_t mouse_read(void) {
    kbd_wait_read();
    return inb(0x60);
}

static void mouse_isr(uint32_t int_no, uint32_t err_code,
                       uint32_t eip, uint32_t cs, uint32_t eflags) {
    uint8_t data;
    int dx, dy;

    (void)int_no; (void)err_code; (void)eip; (void)cs; (void)eflags;

    /* Check that this is actually mouse data (bit 5 of status) */
    if (!(inb(0x64) & 0x20)) {
        pic_eoi(IRQ_MOUSE);
        return;
    }

    data = inb(0x60);
    packet[cycle] = data;
    cycle++;

    if (cycle == 1) {
        /* First byte: check "always 1" bit 3 for sync */
        if (!(data & 0x08)) {
            cycle = 0;  /* out of sync, reset */
        }
    }

    if (cycle == 3) {
        cycle = 0;

        buttons = packet[0] & 0x07;

        dx = (int)packet[1];
        dy = (int)packet[2];

        /* Sign-extend using status byte bits 4,5 */
        if (packet[0] & 0x10) dx |= (int)0xFFFFFF00;
        if (packet[0] & 0x20) dy |= (int)0xFFFFFF00;

        /* Discard if overflow */
        if (packet[0] & 0xC0) {
            pic_eoi(IRQ_MOUSE);
            return;
        }

        /* Update position (PS/2 Y is inverted) */
        pos_x += dx;
        pos_y -= dy;

        /* Clamp to bounds */
        if (pos_x < 0) pos_x = 0;
        if (pos_x >= bound_w) pos_x = bound_w - 1;
        if (pos_y < 0) pos_y = 0;
        if (pos_y >= bound_h) pos_y = bound_h - 1;
    }

    pic_eoi(IRQ_MOUSE);
}

void mouse_init(void) {
    uint8_t config;
    uint8_t ack;

    cycle = 0;
    pos_x = bound_w / 2;
    pos_y = bound_h / 2;
    buttons = 0;

    /* Enable auxiliary device (mouse port) */
    kbd_wait_write();
    outb(0x64, 0xA8);

    /* Enable auxiliary interrupt in controller config */
    kbd_wait_write();
    outb(0x64, 0x20);       /* read config */
    kbd_wait_read();
    config = inb(0x60);
    config |= 0x02;          /* enable aux interrupt (bit 1) */
    config &= ~0x20;         /* clear aux clock disable (bit 5) */
    kbd_wait_write();
    outb(0x64, 0x60);       /* write config */
    kbd_wait_write();
    outb(0x60, config);

    /* Reset mouse -- send 0xFF */
    mouse_write(0xFF);
    ack = mouse_read();      /* ACK (0xFA) */
    (void)ack;
    mouse_read();            /* Self-test result (0xAA) */
    mouse_read();            /* Mouse ID (0x00) */

    /* Set defaults */
    mouse_write(0xF6);
    mouse_read();            /* ACK */

    /* Enable data reporting */
    mouse_write(0xF4);
    mouse_read();            /* ACK */

    /* Install ISR and unmask */
    isr_register(44, mouse_isr);
    pic_unmask(IRQ_MOUSE);

    serial_puts("Mouse: PS/2 driver installed (IRQ 12)\n");
}

int mouse_get_x(void) { return pos_x; }
int mouse_get_y(void) { return pos_y; }
int mouse_get_buttons(void) { return buttons; }

void mouse_set_bounds(int width, int height) {
    bound_w = width;
    bound_h = height;
    if (pos_x >= width)  pos_x = width - 1;
    if (pos_y >= height) pos_y = height - 1;
}

void mouse_set_position(int x, int y) {
    if (x < 0) x = 0;
    if (x >= bound_w) x = bound_w - 1;
    if (y < 0) y = 0;
    if (y >= bound_h) y = bound_h - 1;
    pos_x = x;
    pos_y = y;
}

/* (doc 52 §9 — HID Boot Mouse → INT 33h state). HID 1.11 §5.9: Y
 * increases moving from far to near, i.e. screen-down — opposite sign
 * from PS/2 packets. */
void mouse_inject(uint8_t btns, int dx, int dy, int wheel) {
    (void)wheel;                       /* v1: ignore wheel */
    buttons = btns & 0x07;
    pos_x += dx;
    pos_y += dy;
    if (pos_x < 0) pos_x = 0;
    if (pos_x >= bound_w) pos_x = bound_w - 1;
    if (pos_y < 0) pos_y = 0;
    if (pos_y >= bound_h) pos_y = bound_h - 1;
}
