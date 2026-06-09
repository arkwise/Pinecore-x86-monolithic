#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

void mouse_init(void);
int  mouse_get_x(void);
int  mouse_get_y(void);
int  mouse_get_buttons(void);
void mouse_set_bounds(int width, int height);
void mouse_set_position(int x, int y);

/* Inject a synthetic mouse event into the kernel mouse state. Used by
 * the USB HID class driver (doc 52 §9) so a USB mouse plugs into the
 * same `mouse_get_x/y/buttons` poll API the PS/2 driver feeds.
 *
 *   buttons : low 3 bits = L/R/M (MOUSE_LEFT / RIGHT / MIDDLE)
 *   dx, dy  : relative motion since last report (signed 8-bit)
 *   wheel   : signed wheel delta (positive = scroll up). v1 ignores.
 *
 * Y is fed straight (no PS/2 invert); USB HID's Y already increases
 * downward per HID 1.11 §5.9. */
void mouse_inject(uint8_t buttons, int dx, int dy, int wheel);

#endif
