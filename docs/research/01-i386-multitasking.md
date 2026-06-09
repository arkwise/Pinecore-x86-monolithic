# i386 Multitasking — TSS, Context Switching, Task Gates

> How the 80386 CPU handles hardware-assisted task switching and what this means for our desktop.

**Date:** 2026-04-28
**Status:** Complete

---

## Findings

### Task State Segment (TSS)

The 386 provides hardware-assisted task switching via the Task State Segment. Each task has its own TSS containing:

- **All general registers:** EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP, EIP (386-bible p.130-131)
- **Segment registers:** CS, DS, ES, FS, GS, SS (386-bible p.131)
- **EFLAGS** — including the VM bit (bit 17) which enables V86 mode (386-bible p.131)
- **CR3** — page directory base, so each task can have its own address space (386-bible p.131)
- **Ring 0/1/2 stack pointers** — SS0:ESP0, SS1:ESP1, SS2:ESP2 for privilege transitions (386-bible p.131)
- **Back link** — selector of previous task's TSS for task chaining (386-bible p.131)
- **I/O permission bitmap** — controls which I/O ports the task can access (386-bible p.131)
- **LDT selector** — each task can have its own Local Descriptor Table (386-bible p.131)

### How Context Switching Works

1. A task switch is triggered by: JMP/CALL to a TSS descriptor, JMP/CALL to a task gate, an interrupt/exception through a task gate, or IRET when NT flag is set (386-bible p.133-134)
2. CPU automatically saves current task state to current TSS (386-bible p.134)
3. CPU loads new task state from target TSS (386-bible p.134)
4. CR3 is loaded — if different, TLB is flushed (new address space) (386-bible p.134)
5. If VM bit is set in new EFLAGS, CPU enters V86 mode (386-bible p.217)

**Critical insight:** The hardware does the context switch for you. You don't manually save/restore registers — JMP to a task gate and the CPU handles it.

### Task Gates

A task gate is a descriptor that points to a TSS. They can live in:
- The GDT (Global Descriptor Table) (386-bible p.135)
- The IDT (Interrupt Descriptor Table) — so interrupts can trigger task switches (386-bible p.135)
- An LDT (Local Descriptor Table) (386-bible p.135)

This means you can set up an interrupt (like the timer) to automatically task-switch by pointing its IDT entry at a task gate.

### Relevance to Our Project

**What CWSDPMI actually does:**
CWSDPMI uses 4+ TSS structures internally (cwsdpmi: tss.h):
- `_c_tss` — CWSDPMI's own real-mode context
- `_a_tss` — The DJGPP application's context
- `_i_tss` — Interrupt handler context
- `_f_tss` — Page fault handler context

**What we can do from Ring 3 (DJGPP user code):**
- We CANNOT directly create or manipulate TSS structures (that's Ring 0)
- We CAN use DPMI services (INT 31h) to allocate memory, set up descriptors, and request mode switches
- We CAN use DPMI to install interrupt handlers
- For preemptive multitasking, we'd need the timer interrupt to switch between tasks — but CWSDPMI owns the timer

**The constraint:** We run at Ring 3 under CWSDPMI. Hardware task switching is a Ring 0 operation. We must either:
1. Use cooperative multitasking (tasks yield voluntarily)
2. Use the DPMI timer interrupt to implement preemptive switching in software (save/restore registers ourselves)
3. Find a way to extend CWSDPMI (very difficult, it's the DPMI host)

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| 386 Bible Ch.7 | i386-bible/pages/page_0130.txt — page_0144.txt | TSS format, task switching, task gates |
| CWSDPMI TSS | cwsdpmi-master/src/tss.h | TSS struct layout, 4 TSS instances |
| CWSDPMI mswitch | cwsdpmi-master/src/mswitch.asm | Actual task gate jumps, CR0/CR3 manipulation |

---

*Last updated: 2026-04-28*
