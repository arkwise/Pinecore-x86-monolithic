/*
 * apps/prompt.h — Pinecone Prompt (DOS command-line in a window)
 *
 * Tier 1 implementation: built-in commands resolved by string match
 * (DIR, CD, CLS, ECHO, TYPE, HELP, VER, EXIT), file ops go through
 * DJGPP libc wrappers (which use DPMI 0x300 → INT 21h under the hood).
 * Walks the real FAT from the desktop.
 *
 * Tier 3 (real COMMAND.COM in V86 with a tty bridge) lands after the
 * headless-VT DPMI services in the kernel. See pinecone/ROADMAP.md.
 */
#ifndef PINECONE_APPS_PROMPT_H
#define PINECONE_APPS_PROMPT_H

#include "core/window.h"   /* window_t */

extern int  g_prompt_open;     /* visible+active flag */
extern window_t g_prompt_win;

void prompt_open(void);
void prompt_close(void);
void prompt_draw(BITMAP *bmp, unsigned long ms);
void prompt_feed_char(int ascii);   /* called by main loop with each typed char */

#endif
