#ifndef DOS_H
#define DOS_H

#include "types.h"

/* DOS INT 21h emulation layer
 *
 * Called by V86 GPF handler when a V86 task executes INT 21h.
 * Each task has its own PSP, file handles, CWD, and DTA.
 * (ch-09)
 */

/* CPU register state passed to/from INT 21h handler */
struct dos_regs {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp;
    uint32_t ds, es;
    uint32_t eflags;
    /* Frame context — set by v86 layer for EXEC save/restore */
    uint32_t cs, eip, ss, esp;
};

/* Per-task DOS state */
#define DOS_MAX_HANDLES 20   /* per task, like real DOS */
#define DOS_MAX_TASKS   8

/* Saved parent state for EXEC (INT 21h/4Bh) */
struct exec_save {
    uint8_t  active;       /* 1 if a parent context is saved */
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t eip, cs, esp, ss, ds, es, eflags;
    uint16_t psp_seg;
    uint32_t dta_seg, dta_off;
    uint16_t next_alloc_seg;
};

/* EXEC result: child program entry point */
struct exec_entry {
    uint16_t cs, ip, ss, sp, ds, es;
    uint8_t  is_exe;  /* 1 = EXE (separate segs), 0 = COM (all same) */
};

/* Return codes from dos_int21() */
#define DOS_RESULT_NORMAL     0  /* normal return, copy regs back */
#define DOS_RESULT_EXEC       1  /* redirect frame to child entry */
#define DOS_RESULT_CHILD_EXIT 2  /* restore parent frame */

struct dos_task {
    uint8_t  active;
    uint8_t  handle_map[DOS_MAX_HANDLES]; /* maps DOS handle -> FAT handle */
    uint32_t dta_seg;     /* Disk Transfer Area segment */
    uint32_t dta_off;     /* Disk Transfer Area offset */
    uint32_t psp_seg;     /* Program Segment Prefix */
    char     cwd[260];    /* current working directory */
    uint8_t  return_code; /* last child process return code */
    uint16_t next_alloc_seg; /* next free segment for memory allocation */
    struct exec_save parent;   /* saved parent state for EXEC */
    struct exec_entry child;   /* child entry point (set by 4Bh) */
};

void dos_init(void);
int  dos_create_task(void);
void dos_destroy_task(int task_id);
void dos_set_psp(int task_id, uint16_t psp_seg);
uint16_t dos_get_psp(int task_id);
uint16_t dos_alloc_paragraphs(int task_id, uint16_t paragraphs);
int  dos_int21(int task_id, struct dos_regs *regs);

/* Console I/O callbacks -- set by shell/window system */
typedef void (*dos_putchar_fn)(int task_id, char c);
typedef char (*dos_getchar_fn)(int task_id);
typedef int  (*dos_kbhit_fn)(int task_id);

void dos_set_console(dos_putchar_fn putc_fn, dos_getchar_fn getc_fn, dos_kbhit_fn kbhit_fn);

#endif
