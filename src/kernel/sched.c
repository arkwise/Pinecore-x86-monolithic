/* sched.c — Pinecore Preemptive Scheduler
 *
 * Round-robin scheduler driven by PIT timer (100Hz).
 * Context switch by swapping ESP in the timer ISR.
 *
 * The ISR stub (isr_common) saves all registers to the current
 * task's kernel stack. We swap ESP to the next task's stack.
 * When isr_common restores registers and IRETs, it resumes
 * the new task exactly where it left off.
 *
 * (ch-18, ch-01, 386-bible p.130-144)
 */

#include "types.h"
#include "sched.h"
#include "tss.h"
#include "pmm.h"
#include "serial.h"
#include "v86.h"
#include "dos.h"
#include "exeload.h"
#include "comload.h"
#include "vt.h"
#include "fat.h"

extern void *memset(void *s, int c, uint32_t n);

static struct task tasks[SCHED_MAX_TASKS];
static int current = -1;
static int task_count = 0;
static int scheduler_active = 0;

void sched_diag_dump(void) {
    int i;
    static const char *state_str[] = {"U", "RDY", "RUN", "BLK", "DEAD"};
    static const char *block_str[] = {"-", "KBD", "SLEEP", "VT", "FDC", "VTHID"};
    serial_puts("[sched] cur=");
    serial_puthex(current);
    serial_puts(" cnt=");
    serial_puthex(task_count);
    serial_puts("\n");
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) continue;
        serial_puts("  ");
        serial_puthex(i);
        serial_puts(" ");
        serial_puts(tasks[i].name);
        serial_puts(" st=");
        serial_puts(state_str[tasks[i].state]);
        serial_puts(" blk=");
        serial_puts(block_str[tasks[i].block_reason]);
        serial_puts(" type=");
        serial_puthex(tasks[i].type);
        serial_puts(" v86=");
        serial_puthex(tasks[i].v86_task_id);
        serial_puts("\n");
    }
}

/* ================================================================
 * Task lifecycle
 * ================================================================ */

void sched_init(void) {
    int i;
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
        tasks[i].vt = -1;
    }
    current = -1;
    task_count = 0;
    scheduler_active = 0;

    serial_puts("Scheduler: init (");
    serial_puthex(SCHED_MAX_TASKS);
    serial_puts(" task slots)\n");
}

/* Stack frame for a new kernel task.
 * Must match what isr_common pushes:
 *   gs, fs, es, ds, pusha (edi,esi,ebp,esp,ebx,edx,ecx,eax),
 *   int_no, err_code, eip, cs, eflags
 *
 * When isr_common does pop gs/fs/es/ds + popa + add esp,8 + iret,
 * it will land at the task's entry function.
 */
struct init_stack_frame {
    /* Pushed by our stub (bottom of stack = popped last) */
    uint32_t gs, fs, es, ds;
    /* pusha order: edi, esi, ebp, esp(ignored), ebx, edx, ecx, eax */
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    /* Pushed by stub */
    uint32_t int_no, err_code;
    /* Pushed by CPU (or faked for new task) */
    uint32_t eip, cs, eflags;
};

int sched_create_kernel_task(const char *name, void (*entry)(void), int vt) {
    int i;
    uint32_t stack_page;
    uint32_t stack_top;
    struct init_stack_frame *frame;

    /* Find free slot */
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED)
            break;
    }
    if (i == SCHED_MAX_TASKS) return -1;

    /* Allocate kernel stack */
    stack_page = pmm_alloc_page();
    if (!stack_page) return -1;

    stack_top = stack_page + SCHED_STACK_SIZE;
    tasks[i].stack_page = stack_page;
    tasks[i].esp0 = stack_top;

    /* Build initial stack frame so isr_common can "restore" into it */
    frame = (struct init_stack_frame *)(stack_top - sizeof(struct init_stack_frame));

    memset(frame, 0, sizeof(*frame));
    frame->eip = (uint32_t)entry;
    frame->cs  = 0x08;        /* kernel code selector */
    frame->eflags = 0x202;    /* IF=1, bit1=1 (interrupts enabled) */
    frame->ds  = 0x10;        /* kernel data selector */
    frame->es  = 0x10;
    frame->fs  = 0x10;
    frame->gs  = 0x10;

    tasks[i].esp = (uint32_t)frame;
    tasks[i].state = TASK_READY;
    tasks[i].type = TASK_KERNEL;
    tasks[i].vt = vt;
    tasks[i].block_reason = BLOCK_NONE;
    tasks[i].v86_task_id = -1;
    tasks[i].dos_task_id = -1;
    tasks[i].fat_drive = fat_get_drive();  /* inherit current drive */

    /* Copy name */
    {
        int j;
        for (j = 0; j < 15 && name[j]; j++)
            tasks[i].name[j] = name[j];
        tasks[i].name[j] = '\0';
    }

    task_count++;

    serial_puts("Scheduler: created task '");
    serial_puts(tasks[i].name);
    serial_puts("' (id=");
    serial_puthex(i);
    serial_puts(", stack=");
    serial_puthex(stack_page);
    serial_puts(")\n");

    return i;
}

/* ================================================================
 * V86 task entry — scheduler task that runs COMMAND.COM
 * ================================================================ */

static void v86_task_entry(void) {
    struct task *self = sched_get_task(sched_current());
    int v86_id = self->v86_task_id;
    struct exe_info einfo;
    const char *binary = self->exec_binary[0] ? self->exec_binary : "COMMAND.COM";
    const char *args   = self->exec_args[0]   ? self->exec_args   : "";

    serial_puts("V86 task: loading ");
    serial_puts(binary);
    serial_puts("\n");

    if (exe_load(binary, args, &einfo) == 0) {
        dos_set_psp(self->dos_task_id, einfo.psp_seg);

        serial_puts("V86 task: starting ");
        serial_puts(binary);
        serial_puts("\n");
        v86_start_exe(v86_id, einfo.entry_cs, einfo.entry_ip,
                      einfo.init_ss, einfo.init_sp, einfo.psp_seg);
        /* v86_start_exe never returns — exit is handled by
         * GPF handler calling sched_v86_exit() */
    }

    /* Only reached on load failure */
    serial_puts("V86 task: failed to load ");
    serial_puts(binary);
    serial_puts("\n");
    v86_destroy_task(v86_id);
    if (self->vt >= 0)
        vt_destroy(self->vt);
    sched_exit();
}

int sched_create_v86_exec(const char *name, int vt,
                          const char *binary, const char *args) {
    int id = sched_create_v86_task(name, vt);
    if (id < 0) return id;
    /* Block scheduler briefly so we can populate fields before entry runs. */
    tasks[id].state = TASK_BLOCKED;
    if (binary) {
        unsigned k;
        for (k = 0; k < sizeof(tasks[id].exec_binary) - 1 && binary[k]; k++)
            tasks[id].exec_binary[k] = binary[k];
        tasks[id].exec_binary[k] = '\0';
    } else {
        tasks[id].exec_binary[0] = '\0';
    }
    if (args) {
        unsigned k;
        for (k = 0; k < sizeof(tasks[id].exec_args) - 1 && args[k]; k++)
            tasks[id].exec_args[k] = args[k];
        tasks[id].exec_args[k] = '\0';
    } else {
        tasks[id].exec_args[0] = '\0';
    }
    tasks[id].state = TASK_READY;
    return id;
}

int sched_create_v86_task_with_entry(const char *name,
                                     void (*entry)(void), int vt) {
    int task_id, v86_id;

    /* Create the kernel task as BLOCKED so the scheduler won't run it
     * before we finish setting up the V86 fields. */
    task_id = sched_create_kernel_task(name, entry, vt);
    if (task_id < 0) return -1;

    tasks[task_id].state = TASK_BLOCKED;
    tasks[task_id].block_reason = BLOCK_NONE;

    v86_id = v86_create_task();
    if (v86_id < 0) {
        tasks[task_id].state = TASK_DEAD;
        task_count--;
        return -1;
    }

    tasks[task_id].type = TASK_V86;
    tasks[task_id].v86_task_id = v86_id;
    tasks[task_id].dos_task_id = v86_get_dos_task(v86_id);

    serial_puts("Scheduler: V86 task '");
    serial_puts(name);
    serial_puts("' created (v86=");
    serial_puthex(v86_id);
    serial_puts(")\n");

    tasks[task_id].state = TASK_READY;
    return task_id;
}

int sched_create_v86_task(const char *name, int vt) {
    int task_id, v86_id;

    /* Create the kernel task as BLOCKED so the scheduler won't run it
     * before we finish setting up the V86 fields. */
    task_id = sched_create_kernel_task(name, v86_task_entry, vt);
    if (task_id < 0) return -1;

    tasks[task_id].state = TASK_BLOCKED;
    tasks[task_id].block_reason = BLOCK_NONE;

    v86_id = v86_create_task();
    if (v86_id < 0) {
        tasks[task_id].state = TASK_DEAD;
        task_count--;
        return -1;
    }

    tasks[task_id].type = TASK_V86;
    tasks[task_id].v86_task_id = v86_id;
    tasks[task_id].dos_task_id = v86_get_dos_task(v86_id);

    serial_puts("Scheduler: V86 task '");
    serial_puts(name);
    serial_puts("' created (v86=");
    serial_puthex(v86_id);
    serial_puts(")\n");

    /* Now unblock — all fields are initialized */
    tasks[task_id].state = TASK_READY;

    return task_id;
}

/* ================================================================
 * Context switch — called from timer ISR
 *
 * esp_ptr points to the current ESP on the ISR's stack.
 * We save it to the current task and load the next task's ESP.
 * When the ISR returns, it pops the new task's saved registers.
 * ================================================================ */

void sched_schedule(uint32_t *esp_ptr) {
    int next;
    int i;

    if (!scheduler_active) return;
    if (task_count < 2) return;  /* nothing to switch to */

    /* Save current task's stack pointer and drive state */
    if (current >= 0) {
        tasks[current].esp = *esp_ptr;
        tasks[current].fat_drive = fat_get_drive();
        if (tasks[current].state == TASK_RUNNING)
            tasks[current].state = TASK_READY;
        /* DEAD tasks → reclaim the slot */
        if (tasks[current].state == TASK_DEAD)
            tasks[current].state = TASK_UNUSED;
        /* BLOCKED tasks keep their state — ESP still saved for when unblocked */
    }

    /* Find next READY task (round-robin), but PREFER non-idle.
     *
     * Why: the idle task is always READY by design. With one productive task
     * (e.g. DOOM in PM) and idle as the only other ready task, plain
     * round-robin alternates between them every RTC tick — DOOM gets 50%
     * CPU and pays a full register save/restore per tick (8192/s). Tasks
     * with real work get crowded out by an empty `hlt` loop. Pass 1 scans
     * for a non-idle READY task; if none, fall back to idle (or stay on
     * current if it's still runnable). */
    next = current;
    int found_non_idle = 0;
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        next = (next + 1) % SCHED_MAX_TASKS;
        if (tasks[next].state == TASK_READY) {
            /* Skip the idle task on this pass. */
            const char *n = tasks[next].name;
            if (n[0] != 'i' || n[1] != 'd' || n[2] != 'l' || n[3] != 'e') {
                found_non_idle = 1;
                break;
            }
        }
    }
    if (!found_non_idle) {
        /* Only the idle task (or no task) is ready. If our currently
         * running task is still runnable, keep running it — no
         * context switch needed. */
        if (current >= 0 && tasks[current].state == TASK_READY) {
            tasks[current].state = TASK_RUNNING;
            return;
        }
        /* Current is BLOCKED/DEAD; fall through to idle. */
        next = current;
        for (i = 0; i < SCHED_MAX_TASKS; i++) {
            next = (next + 1) % SCHED_MAX_TASKS;
            if (tasks[next].state == TASK_READY) break;
        }
    }

    if (tasks[next].state != TASK_READY) {
        /* No ready tasks at all — stay on current if it's still runnable,
         * otherwise we'll just idle (timer will keep firing) */
        if (current >= 0 && tasks[current].state == TASK_READY) {
            tasks[current].state = TASK_RUNNING;
        }
        /* If current is BLOCKED, we have nothing to run.
         * Just return — the ISR will iret and hlt until next IRQ.
         * The keyboard IRQ will unblock a task, then the next timer
         * tick will switch to it. */
        return;
    }

    /* Switch to next task */
    current = next;
    tasks[current].state = TASK_RUNNING;

    /* Update TSS.ESP0 for Ring 3 → Ring 0 transitions (V86 tasks) */
    tss_set_stack(tasks[current].esp0);

    /* Track which V86 task is active (GPF handler needs this) */
    if (tasks[current].type == TASK_V86 && tasks[current].v86_task_id >= 0)
        v86_set_current(tasks[current].v86_task_id);
    else
        v86_set_current(-1);

    /* Restore incoming task's FAT drive */
    if (fat_is_mounted(tasks[current].fat_drive))
        fat_set_drive(tasks[current].fat_drive);

    /* Swap stack pointer — THE context switch */
    *esp_ptr = tasks[current].esp;
}

/* ================================================================
 * Blocking I/O support
 * ================================================================ */

void sched_block(enum block_reason reason, int data) {
    if (current < 0) return;

    tasks[current].block_reason = reason;
    tasks[current].block_data = data;
    tasks[current].state = TASK_BLOCKED;

    /* Yield to let another task run */
    sched_yield();
}

void sched_unblock(enum block_reason reason, int data) {
    int i;
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_BLOCKED &&
            tasks[i].block_reason == reason &&
            tasks[i].block_data == data) {
            tasks[i].state = TASK_READY;
            tasks[i].block_reason = BLOCK_NONE;
        }
    }
}

void sched_yield(void) {
    /* Trigger a software interrupt to force a context switch.
     * INT 40 is the RTC interrupt — same ISR path as real preemption. */
    __asm__ volatile("int $40");
}

int sched_is_active(void) {
    return scheduler_active;
}

/* ================================================================
 * Start scheduling — called after all initial tasks are created
 * ================================================================ */

void sched_start(void) {
    if (task_count == 0) return;

    /* Disable interrupts during setup — prevent RTC ISR from running
     * sched_schedule before sched_switch_to. The IRET in sched_switch_to
     * will re-enable interrupts from the task's EFLAGS (IF=1). */
    __asm__ volatile("cli");

    scheduler_active = 1;

    /* Set current to first ready task */
    {
        int i;
        for (i = 0; i < SCHED_MAX_TASKS; i++) {
            if (tasks[i].state == TASK_READY) {
                current = i;
                tasks[i].state = TASK_RUNNING;
                break;
            }
        }
    }

    serial_puts("Scheduler: started with ");
    serial_puthex(task_count);
    serial_puts(" tasks\n");

    /* Jump to the first task via assembly (avoids compiler interference).
     * sched_switch_to does: mov esp, arg; pop segs; popa; add 8; iret */
    tss_set_stack(tasks[current].esp0);

    {
        extern void sched_switch_to(uint32_t esp);
        sched_switch_to(tasks[current].esp);
    }
    /* never reached */
}

void sched_exit(void) {
    if (current >= 0) {
        serial_puts("Scheduler: task '");
        serial_puts(tasks[current].name);
        serial_puts("' exited\n");
        tasks[current].state = TASK_DEAD;
        task_count--;
    }
    sched_yield();
    /* never returns */
    while (1) __asm__ volatile("hlt");
}

void sched_v86_exit(void) {
    /* Called from GPF handler when a V86 task terminates
     * (INT 20h / INT 21h AH=4Ch / unhandled opcode).
     * Cleans up V86 task + VT, then kills the scheduler task. */
    struct task *self = (current >= 0) ? &tasks[current] : 0;
    if (self) {
        serial_puts("Scheduler: V86 task '");
        serial_puts(self->name);
        serial_puts("' terminating\n");
        if (self->v86_task_id >= 0) {
            /* Release any DPMI client tied to this V86 task BEFORE we
             * destroy the V86 task. Otherwise the client.active flag
             * stays true; a subsequent EXEC creates a stale-LDT second
             * client whose mode switch eventually triple-faults via
             * isr_common re-entry. (lockup signature.) */
            extern void dpmi_release_client_for_v86(int v86_task_id);
            dpmi_release_client_for_v86(self->v86_task_id);
            v86_destroy_task(self->v86_task_id);
        }
        if (self->vt >= 0)
            vt_destroy(self->vt);
    }
    sched_exit();
    /* never returns */
}

int sched_current(void) {
    return current;
}

struct task *sched_get_task(int id) {
    if (id < 0 || id >= SCHED_MAX_TASKS) return 0;
    if (tasks[id].state == TASK_UNUSED) return 0;
    return &tasks[id];
}

int sched_vt_for_v86(int v86_task_id) {
    int i;
    if (v86_task_id < 0) return -1;
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED &&
            tasks[i].v86_task_id == v86_task_id)
            return tasks[i].vt;
    }
    return -1;
}
