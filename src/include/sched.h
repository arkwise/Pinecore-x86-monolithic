#ifndef SCHED_H
#define SCHED_H

#include "types.h"

/* Pinecore Preemptive Scheduler
 *
 * Tasks are scheduled round-robin at 100Hz (PIT timer).
 * Each task has its own kernel stack. Context switch happens
 * by swapping ESP in the timer ISR — the saved registers on
 * each task's stack are automatically restored by isr_common.
 *
 * Two task types:
 *   TASK_KERNEL — Ring 0, runs Pinecore shell or kernel services
 *   TASK_V86    — Ring 3 V86 mode, runs COMMAND.COM
 */

#define SCHED_MAX_TASKS  16
#define SCHED_STACK_SIZE 4096  /* 4KB kernel stack per task */

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
};

enum task_type {
    TASK_KERNEL = 0,
    TASK_V86
};

enum block_reason {
    BLOCK_NONE = 0,
    BLOCK_KEYBOARD,
    BLOCK_SLEEP,
    BLOCK_VT_REQUEST,
    BLOCK_FDC,         /* waiting for floppy IRQ */
    BLOCK_VT_HIDDEN,   /* graphics-mode VT not currently displayed */
    BLOCK_V86MT_KBD    /* v86mt-vt waiting on its kbd ring; data = VT handle */
};

struct task {
    uint32_t         esp;           /* saved kernel stack pointer */
    uint32_t         esp0;          /* top of kernel stack (for TSS.ESP0) */
    enum task_state  state;
    enum task_type   type;
    int              vt;            /* bound virtual terminal (-1 = none) */
    char             name[16];      /* task name for debug */

    /* Blocking */
    enum block_reason block_reason;
    int               block_data;   /* e.g., VT number for keyboard wait */

    /* For V86 tasks */
    int              v86_task_id;
    int              dos_task_id;
    char             exec_binary[64]; /* "COMMAND.COM" if empty */
    char             exec_args[64];   /* "/P" for COMMAND.COM */

    /* Per-task FAT drive (saved/restored on context switch) */
    int              fat_drive;

    /* Stack page (for cleanup) */
    uint32_t         stack_page;
};

/* Init scheduler — called from kernel_main */
void sched_init(void);

/* Create a new kernel task that runs the given function */
int sched_create_kernel_task(const char *name, void (*entry)(void), int vt);

/* Create a V86 task (wraps existing v86 infrastructure) */
int sched_create_v86_task(const char *name, int vt);

/* Create a V86 task with a custom kernel-task entry function. Used by
 * V86MT (Phase 4.7 M4) which provides its own entry that skips exe_load
 * (the program bytes are pre-written to low memory by the caller) and
 * calls v86_start_exe directly. Returns scheduler task ID, or -1. */
int sched_create_v86_task_with_entry(const char *name,
                                     void (*entry)(void), int vt);

/* Create a V86 task running a specific DOS binary + command tail.
 * Binary and args are copied into the task struct; v86_task_entry
 * uses them via exe_load instead of the hardcoded COMMAND.COM. */
int sched_create_v86_exec(const char *name, int vt,
                          const char *binary, const char *args);

/* Called from timer ISR — performs context switch */
void sched_schedule(uint32_t *esp_ptr);

/* Diagnostic — print one-line summary of every non-UNUSED task. */
void sched_diag_dump(void);

/* Yield CPU voluntarily (for blocking I/O) */
void sched_yield(void);

/* Block current task (waiting for I/O) */
void sched_block(enum block_reason reason, int data);

/* Unblock a task waiting for the given reason+data */
void sched_unblock(enum block_reason reason, int data);

/* Check if scheduler is running (for drivers that can optionally block) */
int sched_is_active(void);

/* Get current task ID */
int sched_current(void);

/* Get task struct */
struct task *sched_get_task(int id);

/* Find the VT number bound to the scheduler task that wraps a V86 task.
 * Returns the vt number, or -1 if no task wraps the given v86_task_id.
 * Used by the DPMI INT 10h hooks to update per-VT video state on mode-set. */
int sched_vt_for_v86(int v86_task_id);

/* Start scheduling — jumps to first task, never returns */
void sched_start(void);

/* Exit current task (mark DEAD, yield, never returns) */
void sched_exit(void);

/* Exit a V86 task — cleans up V86 + VT, then exits scheduler task.
 * Called from GPF handler when V86 task terminates. Never returns. */
void sched_v86_exit(void);

#endif
