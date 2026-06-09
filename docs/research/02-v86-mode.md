# Virtual 8086 Mode — Running Real-Mode Code from Protected Mode

> How V86 mode lets us run command.com (a real-mode program) while staying in protected mode.

**Date:** 2026-04-28
**Status:** Complete — V86 feasibility confirmed with own kernel (ch-05)

---

## Findings

### What V86 Mode Is

Virtual 8086 mode lets the 386 run 8086 (real mode) programs within a protected-mode task. (386-bible p.217)

- Enabled by setting the VM (Virtual Machine) bit (bit 17) in EFLAGS (386-bible p.217)
- The V86 task sees a 1MB address space (segments 0000-FFFF, offsets 0000-FFFF) (386-bible p.218)
- Address formation uses real-mode style: `segment << 4 + offset` (386-bible p.218)
- But paging is still active — the linear addresses go through page tables (386-bible p.218)
- This means each V86 task can have its own mapping of the first megabyte (386-bible p.218)

### The V86 Monitor

A protected-mode program (the "V86 monitor") supervises V86 tasks. (386-bible p.220)

- The monitor runs at Ring 0 (386-bible p.220)
- Sensitive instructions (CLI, STI, IN, OUT, INT, IRET, PUSHF, POPF) cause GPFs that trap to the monitor (386-bible p.221)
- The monitor emulates these instructions — it can virtualise hardware (386-bible p.221)
- INT instructions from V86 mode trap to the monitor, which can redirect them (386-bible p.221)

### How CWSDPMI Handles V86

CWSDPMI is NOT a V86 monitor. It runs DJGPP code in protected mode, not V86 mode. However:

- CWSDPMI detects if IT is running in V86 mode (under EMM386) via `cpumode()` (cwsdpmi: mswitch.asm)
- If in V86, it uses VCPI to get to protected mode (cwsdpmi: control.c:325)
- CWSDPMI's `_go_real_mode` function switches back to real mode for DOS calls (cwsdpmi: mswitch.asm)
- Real-mode interrupt reflection: PM interrupts → RM via saved_interrupt_vector table (cwsdpmi: mswitch.asm:271-327)

**Key insight: CWSDPMI does not provide V86 task creation for user code.** It provides real-mode interrupt simulation via DPMI INT 31h functions 0x0300-0x0302 (simulate real-mode interrupt, far call, with/without IRET frame).

### The Problem for Our Project

To run command.com in a window, we need to:

1. **Spawn command.com** — it's a real-mode program
2. **Capture its output** — it writes via INT 21h (DOS) and/or INT 10h (BIOS video) and/or direct video memory writes
3. **Send it keyboard input** — it reads via INT 21h or INT 16h (BIOS keyboard)
4. **Keep our desktop running** while the shell is active

### Possible Approaches

**Approach A: DPMI `system()` / `spawn()` with pipe redirection**
- DJGPP's `system()` calls INT 21h/4Bh (EXEC) to run programs
- Problem: This is synchronous — our desktop STOPS while command.com runs
- Problem: command.com may write directly to video memory, bypassing stdout

**Approach B: Background process with INT 21h hooking**
- Hook INT 21h to intercept command.com's file I/O (stdout writes)
- Hook INT 10h to intercept BIOS video calls
- Hook INT 16h to feed keyboard input from our buffer
- Problem: Still need to run command.com concurrently with our desktop

**Approach C: Cooperative shell — pseudo-terminal approach**
- Don't run actual command.com — implement a simple command interpreter ourselves
- Parse commands, execute them via `system()`, capture output
- Much simpler but less authentic (no TSRs, no real DOS environment)

**Approach D: Timer-based cooperative multitasking**
- Use the timer interrupt to periodically switch between desktop and shell
- Shell gets a timeslice to process one command, then desktop gets control back
- Requires saving/restoring the shell's state manually

**Approach E: Separate DPMI client (extremely complex)**
- Run command.com as a separate DPMI-hosted task
- Would need to act as a partial V86 monitor
- Probably requires modifying CWSDPMI — very advanced

### What Windows 3.1 Actually Did

Windows 3.1 ran in 386 Enhanced Mode which made it a V86 monitor at Ring 0. It could:
- Create multiple V86 tasks, each with their own virtual 1MB address space
- Trap all sensitive instructions and virtualise hardware per-VM
- Preemptively schedule between Windows apps and DOS boxes
- Redirect video output by mapping the V86 task's video memory to a capture buffer

**We cannot replicate this from Ring 3 under CWSDPMI.** Windows 3.1 WAS the operating system. We're a user program running under one.

### Recommended Path

Approach C (cooperative shell / pseudo-terminal) is the most feasible:
1. Build a command-line interpreter that understands basic DOS commands
2. For external commands, use DJGPP's `popen()` / `system()` with output capture
3. Render the output in the terminal window
4. This gives us a "DOS prompt in a window" without needing V86 mode at all

Approach B (INT hooking) is worth investigating as a stretch goal if we want to run actual command.com.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| 386 Bible Ch.15 | i386-bible/pages/page_0217.txt — page_0223.txt | V86 mode mechanics |
| 386 Bible Ch.7 | i386-bible/pages/page_0130.txt — page_0144.txt | TSS VM bit, task switching into V86 |
| CWSDPMI control | cwsdpmi-master/src/control.c | V86 detection, VCPI startup |
| CWSDPMI mswitch | cwsdpmi-master/src/mswitch.asm | Real mode switching, cpumode() |
| CWSDPMI exphdlr | cwsdpmi-master/src/exphdlr.c | DPMI INT 31h — real-mode simulation functions |

---

*Last updated: 2026-04-28*
