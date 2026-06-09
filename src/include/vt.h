#ifndef VT_H
#define VT_H

#include "types.h"
#include "keyboard.h"

/* Pinecore Virtual Terminal System
 *
 * Up to 6 VTs, each with its own 80x25 screen buffer and keyboard queue.
 * Alt+F1-F6 switches between VTs.
 * Alt+C creates a new COMMAND.COM VT.
 * Alt+X closes the current VT.
 *
 * (ch-17)
 */

#define VT_MAX       6
#define VT_COLS      80
#define VT_ROWS      25
#define VT_BUF_SIZE  (VT_COLS * VT_ROWS * 2)  /* 4000 bytes: char+attr pairs */
#define VT_KEY_BUF   64

enum vt_type { VT_UNUSED = 0, VT_SHELL, VT_DOS };

/* The video mode this VT is currently displaying. Updated by the DPMI/V86
 * INT 10h hooks whenever the bound app sets a new mode; consulted by
 * vt_switch (Phase 4.6) to decide how to save/restore the framebuffer on
 * switch-away / switch-toward. */
enum vt_video {
    VT_VID_TEXT_03H = 0,    /* 80×25 text in 0xB8000              */
    VT_VID_GFX_13H,         /* 320×200×8 VGA planar at 0xA0000    */
    VT_VID_GFX_LFB          /* VBE LFB graphics at vbe_lfb_phys() */
};

struct vt {
    enum vt_type type;
    uint8_t      screen[VT_BUF_SIZE];
    uint8_t      cursor_x, cursor_y;
    uint8_t      color;

    /* Per-VT keyboard ring buffer */
    struct key_event key_buf[VT_KEY_BUF];
    volatile uint8_t key_head, key_tail;

    /* Linked scheduler task */
    int          task_id;       /* scheduler task ID */
    int          v86_task_id;   /* V86 task ID (for VT_DOS, -1 otherwise) */

    /* Video-state record (Phase 4.6 multi-mode VT switching) */
    enum vt_video video;
    uint16_t     gfx_w, gfx_h;       /* set when video == VT_VID_GFX_LFB */
    uint8_t      gfx_bpp;
    uint32_t    *gfx_save_pages;     /* array of phys page addrs; NULL until first bg */
    uint32_t     gfx_save_npages;
    uint8_t      dac_save[768];      /* DAC slots 0-255, R/G/B 6-bit each */
};

void vt_init(void);
int  vt_create(enum vt_type type);
void vt_destroy(int vt_num);
void vt_switch(int vt_num);
void vt_repaint(void);
int  vt_get_active(void);
struct vt *vt_get(int vt_num);
int  vt_count_active(void);

/* Console I/O for a specific VT */
void vt_putc(int vt_num, char c);
void vt_puts(int vt_num, const char *s);
void vt_set_color(int vt_num, uint8_t fg, uint8_t bg);
void vt_clear(int vt_num);
void vt_set_cursor(int vt_num, uint8_t col, uint8_t row);
void vt_get_cursor(int vt_num, uint8_t *col, uint8_t *row);

/* Per-VT keyboard */
void vt_enqueue_key(int vt_num, struct key_event *ev);
int  vt_poll_key(int vt_num, struct key_event *ev);

/* Create a new COMMAND.COM VT (called from shell 'dos' command or Alt+C) */
int vt_create_dos(void);

/* Create a new V86 DOS VT running a specific binary (e.g. DOOM.EXE) */
int vt_create_dos_exec(const char *binary, const char *args);

/* Create a new Pinecore Shell VT (called from shell 'shell' command or Ctrl+N) */
int vt_create_shell(void);

/* VT manager task entry (handles Alt+C / Alt+X requests) */
void vt_manager_entry(void);

/* Hotkey request flags (set by keyboard ISR, processed by VT manager task) */
extern volatile int vt_request;
#define VT_REQ_NONE      0
#define VT_REQ_NEW_DOS   1
#define VT_REQ_CLOSE     2
#define VT_REQ_NEW_SHELL 3

#endif
