#ifndef V86_H
#define V86_H

#include "types.h"

/* V86 Monitor — runs real-mode programs in Virtual 8086 mode
 *
 * When a V86 task hits a sensitive instruction (INT, CLI, STI, etc.),
 * the CPU generates GPF #13. Our handler decodes and emulates it.
 * (ch-14, 386-bible p.217-223)
 */

#define V86_MAX_TASKS    8
#define V86_STACK_SIZE   4096   /* Ring 0 stack per V86 task */
#define V86_MEM_SIZE     0x100000  /* 1MB V86 address space */

/* V86 stack frame pushed by CPU on GPF from V86 mode
 * This matches what the CPU pushes, NOT our isr_stubs additions */
struct v86_frame {
    /* Pushed by our isr_stubs.asm */
    uint32_t gs, fs, es_stub, ds_stub;
    uint32_t edi, esi, ebp, esp_stub, ebx, edx, ecx, eax; /* pusha */
    uint32_t int_no, err_code;
    /* Pushed by CPU */
    uint32_t eip, cs, eflags;
    uint32_t esp, ss;
    /* Pushed by CPU for V86 mode only */
    uint32_t v86_es, v86_ds, v86_fs, v86_gs;
};

/* V86 task state */
struct v86_task {
    uint8_t  active;
    int      dos_task_id;    /* corresponding DOS emulation task */
    uint32_t ring0_stack;    /* top of Ring 0 stack for this task */
    /* V86 text capture buffer (80x25 for console) */
    uint8_t  text_buf[80 * 25 * 2];  /* char + attr pairs */
    uint8_t  cursor_x, cursor_y;
    /* V86MT ownership (Phase 4.7 M4). -1 / 0 = not V86MT-owned. */
    int      v86mt_client_id;
    int      v86mt_vt_handle;
};

void v86_init(void);
int  v86_create_task(void);
void v86_destroy_task(int task_id);
void v86_gpf_handler(struct v86_frame *frame);

/* Start a V86 task: COM style (CS=DS=ES=SS=seg, IP=off, SP=0xFFFE) */
void v86_start(int task_id, uint16_t seg, uint16_t off);

/* Start a V86 task: EXE style (separate CS:IP, SS:SP, DS=ES=PSP seg) */
void v86_start_exe(int task_id, uint16_t cs, uint16_t ip,
                   uint16_t ss, uint16_t sp, uint16_t ds);

/* Get a V86 task's DOS task ID */
int v86_get_dos_task(int task_id);

/* Set/get current V86 task (called by scheduler on context switch) */
void v86_set_current(int task_id);

/* V86MT M4 — tag a V86 task as owned by a V86MT VT. The DOS shim
 * checks this on every INT 21h dispatch and routes character output
 * to the V86MT shadow buffer instead of the kernel VT. */
void v86_set_v86mt_owner(int task_id, int client_id, int vt_handle);
int  v86_current_v86mt_client(void);
int  v86_current_v86mt_handle(void);

/* s42 — Deliver a hardware IRQ to a V86 task's installed real-mode
 * ISR. Called from isr_dispatch when an IRQ fires while a V86 task
 * is running. If IVT[vector] points into V86 program memory
 * (segment in 0x0070..0xA000), pushes CS:IP:FLAGS to the V86 stack
 * and redirects the IRET frame to the handler — CPU IRETs into the
 * V86 ISR. Returns 1 if delivered, 0 if no user handler installed
 * (caller falls through to kernel handling). */
int v86_deliver_irq_to_handler(struct v86_frame *frame, uint8_t vector);

#endif
