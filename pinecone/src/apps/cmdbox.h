/*
 * apps/cmdbox.h — Pinecone MS-DOS Box (Tier 3 target: V86 COMMAND.COM
 * in a window via the V86MT vendor API).
 *
 * Current state: placeholder window with "host pending" body.  The
 * eventual purpose is to host a real COMMAND.COM in a V86 task,
 * rendered through the V86MT vendor API.  See docs/design/V86MT-API.md.
 *
 * Once the kernel V86MT dispatcher + libv86mt wrapper land, cmdbox.c's
 * body is swapped out for vt_alloc / vt_spawn / vt_kbd_inject calls.
 * The public surface declared here stays stable across that swap so
 * main.c's wiring (icon click, taskbar tile, z-order, keystroke route)
 * doesn't move.
 *
 * prompt.c (Tier 1) is the working mock-shell window for the
 * meantime.  cmdbox vs. prompt stays a host-capability distinction.
 */
#ifndef PINECONE_APPS_CMDBOX_H
#define PINECONE_APPS_CMDBOX_H

#include "core/window.h"   /* window_t */

extern int      g_cmdbox_open;
extern window_t g_cmdbox_win;

void cmdbox_open(void);
void cmdbox_close(void);
void cmdbox_draw(BITMAP *bmp, unsigned long ms);
void cmdbox_feed_char(int ascii);

#endif
