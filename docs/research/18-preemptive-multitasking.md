# Preemptive Multitasking — Scheduler, Context Switching, Task Model

> How Pinecore runs multiple tasks simultaneously: kernel-mode shells, V86 COMMAND.COM instances, and background services — all preemptively scheduled.

**Date:** 2026-04-30
**Status:** Architecture designed, research based on ch-01 (i386 multitasking) and 386 bible

---

## Why This Matters

Currently Pinecore is single-tasking: COMMAND.COM runs in V86 mode and blocks everything. The kernel only regains control when COMMAND.COM executes an INT (trapped by GPF handler). For multiple VTs with simultaneous shells, we need:

1. **Preemptive scheduling** — the PIT timer fires every 10ms and forcibly switches tasks
2. **Context switching** — save/restore complete CPU state per task
3. **Multiple task types** — kernel-mode tasks (Pinecore shell) and V86 tasks (COMMAND.COM)
4. **Per-task resources** — stack, screen buffer, keyboard queue, file handles

Once achieved: Pinecore is a real preemptive multitasking monolithic kernel.

---

## Task Model

### Task Types

```
┌──────────────────────────────────────────────────────┐
│                    Pinecore Kernel                     │
│                                                        │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐      │
│  │ Kernel Task│  │ Kernel Task│  │ V86 Task   │      │
│  │ Pine Shell │  │ Pine Shell │  │ COMMAND.COM│      │
│  │ Ring 0     │  │ Ring 0     │  │ Ring 3 V86 │      │
│  │ Own stack  │  │ Own stack  │  │ Own stack  │      │
│  └────────────┘  └────────────┘  └────────────┘      │
│         ↕               ↕               ↕              │
│  ┌────────────────────────────────────────────────┐   │
│  │          Scheduler (Round Robin, 10ms)          │   │
│  │          Triggered by PIT IRQ 0 (INT 32)       │   │
│  └────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────┘
```

### Task States

```
    READY ←──────── RUNNING
      │                 │
      │    (timer IRQ    │  (I/O wait:
      │     preempts)    │   keyboard,
      │                 │   disk, etc.)
      │                 ▼
      └────────── BLOCKED
```

- **RUNNING** — currently executing on the CPU
- **READY** — can run, waiting for its time slice
- **BLOCKED** — waiting for I/O (keyboard input, disk read, etc.)

---

## Two Approaches to Context Switching

### Option A: Hardware Task Switching (i386 TSS)

The CPU does it automatically (386-bible p.133-134):
1. Each task has its own TSS in the GDT
2. Timer IDT entry is a **task gate** pointing to the scheduler's TSS
3. On timer IRQ: CPU saves all registers to current TSS, loads scheduler TSS
4. Scheduler picks next task, JMP to its TSS
5. CPU loads all registers from new TSS, including CR3 (address space switch)

**Pros:** CPU handles register save/restore, supports V86 mode (VM bit in EFLAGS)
**Cons:** Slow on modern CPUs (hardware TSS switch is deprecated), one GDT entry per task, inflexible

### Option B: Software Context Switching (Linux/Windows approach)

We do it ourselves in the timer ISR:
1. Timer ISR is a normal interrupt gate (not task gate)
2. ISR saves all registers to current task's kernel stack (pushad + segment regs)
3. Call scheduler to pick next task
4. Switch ESP to new task's kernel stack
5. Pop new task's registers, IRET

**Pros:** Fast, flexible, no GDT entry per task, widely used
**Cons:** Must handle V86 mode manually (CPU pushes extra segments on V86→Ring 0 transition)

### Decision: Option B (Software Context Switching)

**Why:** More flexible, faster, matches how Linux/Windows do it. Our ISR stubs already save all registers via `pusha`. The V86 frame already captures the extra segment registers. We just need to switch the stack pointer.

---

## Context Switch Mechanism

### Task Control Block (TCB)

```c
#define TASK_MAX 16

enum task_state { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_DEAD };
enum task_type  { TASK_KERNEL, TASK_V86 };

struct task {
    uint32_t      esp;          /* saved kernel stack pointer */
    uint32_t      esp0;         /* Ring 0 stack top (for TSS.ESP0) */
    uint32_t      cr3;          /* page directory (for address space isolation, future) */
    enum task_state state;
    enum task_type  type;
    int           vt;           /* which VT this task is bound to */
    int           priority;     /* scheduling priority */
    uint32_t      ticks;        /* time slice counter */

    /* For V86 tasks */
    int           v86_task_id;  /* index into v86_tasks[] */
    int           dos_task_id;  /* index into dos tasks[] */

    /* Task stack (allocated from PMM) */
    uint32_t      stack_page;   /* physical page for kernel stack */
};

static struct task tasks[TASK_MAX];
static int current_task = 0;
```

### Timer ISR Context Switch

The PIT fires every 10ms (100 Hz). The ISR:

```
timer_isr:                         ; (isr_stubs.asm, INT 32)
    ; CPU already pushed: SS, ESP, EFLAGS, CS, EIP (+ V86 segs if V86 mode)
    ; Our stub pushes: error_code, int_no, pusha, push ds/es/fs/gs

    call schedule                  ; C function: picks next task

    ; schedule() switches ESP to new task's kernel stack
    ; The new stack has the saved registers of the new task

    ; Pop the new task's registers
    pop gs / fs / es / ds
    popa
    add esp, 8                     ; skip int_no, error_code
    iret                           ; restores EIP, CS, EFLAGS (+ V86 segs)
```

### The schedule() Function

```c
void schedule(void) {
    /* Save current task's ESP */
    tasks[current_task].esp = get_esp();  /* current kernel stack pointer */
    tasks[current_task].state = TASK_READY;

    /* Pick next READY task (round-robin) */
    int next = current_task;
    do {
        next = (next + 1) % task_count;
    } while (tasks[next].state != TASK_READY && next != current_task);

    if (next == current_task) return;  /* no other task to run */

    /* Switch to next task */
    current_task = next;
    tasks[current_task].state = TASK_RUNNING;

    /* Update TSS.ESP0 for Ring 3→0 transitions */
    tss_set_stack(tasks[current_task].esp0);

    /* Switch stack pointer — this is the actual context switch */
    set_esp(tasks[current_task].esp);

    /* When we return from schedule(), we'll pop the NEW task's registers
     * and IRET to the new task's code. */
}
```

### How V86 Tasks Are Preempted

When a V86 task (COMMAND.COM) is running and the timer fires:
1. CPU transitions from V86 (Ring 3) to Ring 0 via the IDT
2. CPU pushes V86 segments (GS, FS, DS, ES) + SS:ESP + EFLAGS + CS:EIP
3. Our ISR stub pushes general registers (pusha)
4. Now we have the complete V86 state on the kernel stack
5. schedule() saves ESP, switches to another task's stack
6. When this V86 task gets scheduled back:
7. Its kernel stack still has the saved V86 state
8. popa + iret restores everything including V86 mode (VM bit in EFLAGS)

**This already works!** Our ISR stubs and V86 frame handling already save/restore the full V86 state. We just need to switch the stack pointer.

### How Kernel Tasks Are Preempted

Kernel tasks (Pinecore shell) run at Ring 0:
1. Timer fires while in Ring 0
2. CPU pushes EFLAGS + CS:EIP (NO privilege change, NO extra segments)
3. Our ISR stub pushes general registers
4. schedule() saves ESP, switches to another task's stack
5. On restore: popa + iret continues the kernel task

**Key difference:** Ring 0 interrupts don't push SS:ESP (no privilege transition). Our ISR stubs must handle this. Currently, the V86 frame struct expects the extra pushes. For kernel tasks, the frame is smaller.

**Solution:** Use the ISR stub's existing pusha/popa which works for both. The IRET automatically knows whether to pop SS:ESP based on the saved CS's DPL.

---

## Blocked Tasks (I/O Wait)

When a task calls `keyboard_getchar()` and no key is available:
1. Set task state to BLOCKED
2. Record what it's waiting for (e.g., "keyboard input on VT 3")
3. Call schedule() to switch to another task
4. When a key arrives (keyboard IRQ), find the task waiting for that VT
5. Set it to READY

This means keyboard_getchar must NOT busy-wait. Instead:

```c
char keyboard_getchar_blocking(int vt) {
    while (!vt_has_key(vt)) {
        tasks[current_task].state = TASK_BLOCKED;
        tasks[current_task].block_reason = BLOCK_KEYBOARD;
        tasks[current_task].block_vt = vt;
        schedule();  /* switch away */
        /* We return here when unblocked */
    }
    return vt_read_key(vt);
}
```

---

## What Changes From Current Architecture

| Component | Current | With Multitasking |
|-----------|---------|-------------------|
| PIT timer handler | Increments tick counter | Calls schedule() for context switch |
| V86 tasks | Single task, blocks kernel | Multiple V86 tasks, preemptively scheduled |
| Keyboard | Global ring buffer, busy-wait | Per-VT queues, blocking with task switch |
| VGA output | Global, direct writes | Per-VT shadow buffers |
| Console I/O | Global callbacks | Per-task routing to bound VT |
| kernel_main | Runs one task then loops | Creates initial tasks, becomes idle task |
| TSS | Single TSS, updated per V86 | Single TSS, ESP0 updated per task switch |
| Stacks | Single kernel stack + V86 stack | Per-task kernel stack (4KB each from PMM) |

---

## Implementation Order

1. **Task Control Block + task_create/task_destroy** — basic task lifecycle
2. **Software context switch** — save/restore ESP in timer ISR
3. **Scheduler** — round-robin task selection
4. **Kernel task support** — Pinecore shell runs as a schedulable task
5. **V86 task integration** — existing V86 tasks become schedulable
6. **Blocked state + keyboard unblocking** — I/O wait without busy-spin
7. **Per-task TSS.ESP0** — correct Ring 3→0 transition per task
8. **Multiple COMMAND.COM instances** — spawn V86 tasks on demand

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| 386 Bible Ch.7 | i386-bible/pages/page_0130-0144 | TSS format, hardware task switching |
| 386 Bible Ch.15 | i386-bible/pages/page_0217-0223 | V86 mode, V86 interrupt handling |
| Research ch-01 | docs/research/01-i386-multitasking.md | TSS, context switching basics |
| Research ch-14 | docs/research/14-v86-monitor.md | V86 GPF handler, instruction emulation |
| isr_stubs.asm | src/boot/isr_stubs.asm | ISR entry: pusha + segment saves |
| pit.c | src/kernel/pit.c | 100Hz timer, tick counter |
| v86.c | src/kernel/v86.c | V86 task struct, text_buf per task |
| tss.c | src/kernel/tss.c | TSS setup, tss_set_stack() |

---

## What This Makes Pinecore

Once implemented:
- **Preemptive multitasking** — tasks can't starve each other
- **Multiple task types** — kernel-mode and V86 tasks coexist
- **I/O multiplexing** — multiple tasks share keyboard, screen, disk
- **Virtual terminals** — independent shells with hotkey switching
- **Real monolithic kernel** — scheduler, task management, IPC foundations

This is the difference between "a program that runs COMMAND.COM" and "an operating system kernel."

---

*Last updated: 2026-04-30*
