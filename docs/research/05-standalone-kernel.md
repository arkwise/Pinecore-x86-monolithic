# Standalone Monolithic Kernel — Replacing DOS/CWSDPMI Entirely

> Why building our own Ring 0 kernel is actually MORE feasible than hacking CWSDPMI.

**Date:** 2026-04-28
**Status:** Complete — architecture settled, confirmed by reentrancy analysis (ch-09)

---

## The Core Argument

**Modifying CWSDPMI is fighting its design:**
- CWSDPMI assumes DOS is underneath — it calls INT 21h for disk I/O, uses DOS memory allocation, needs DOS for real-mode support (cwsdpmi: mswitch.asm, control.c)
- It's ~7500 lines of tightly coupled ASM+C that was never designed to be a general-purpose kernel
- Adding V86 task creation for user code would mean rewriting its interrupt handling, TSS management, and mode switching
- INT 21h reentrancy is a real problem — DOS is not reentrant, so our shell and desktop can't both make DOS calls

**Building our own kernel gives us:**
- Ring 0 access — we own GDT, IDT, TSS, page tables, everything
- V86 mode — we can create proper V86 tasks with their own virtual 1MB address spaces
- Our own drivers — no INT 21h reentrancy because we don't use INT 21h
- Preemptive multitasking — we control the timer interrupt at Ring 0
- Clean architecture — designed for our exact use case from day one

**The cost:**
- Need an i386-elf cross-compiler (DJGPP can't produce bare-metal code — see ch-06)
- Need to write: bootloader integration, GDT/IDT/paging setup, PIT/PIC/PS2 drivers
- Need to port Allegro's software renderer (but it's 95% portable — see ch-07)
- No standard C library — need freestanding or minimal libc

---

## What The Monolithic Kernel Needs

### Boot → Protected Mode (386-bible p.174-186)

The 386 bible documents the complete sequence:

1. **Real mode startup** — CLI, set up temp GDT in RAM, LGDT (386-bible p.175-176)
2. **Enable protected mode** — set PE bit in CR0, far JMP to flush prefetch (386-bible p.176)
3. **Set segment registers** — point DS/ES/SS/FS/GS to flat 4GB descriptor (386-bible p.182)
4. **Create real GDT** — kernel code/data descriptors, TSS descriptor, user descriptors (386-bible p.177, 180-182)
5. **Create IDT** — 256 entries, interrupt gates for exceptions + hardware IRQs (386-bible p.157, 177)
6. **Enable paging** — page directory + page tables, identity-map kernel, set CR3, set PG bit (386-bible p.98-101, 177)
7. **Create initial TSS** — 104 bytes, set ESP0/SS0 for Ring 0 stack, LTR to load task register (386-bible p.130-134, 178)
8. **STI** — enable interrupts (386-bible p.176)

### Hardware Drivers We Need

| Driver | Hardware | Mechanism | Replaces |
|--------|----------|-----------|----------|
| PIT Timer | 8254 chip, ports 0x40-0x43 | Program channel 0, hook IRQ 0 | Allegro's dtimer.c / DOS INT 8 |
| PIC | 8259A, ports 0x20-0x21 (master), 0xA0-0xA1 (slave) | Remap IRQs to vectors 32-47 (avoid collision with CPU exceptions 0-31) | DOS default PIC mapping |
| PS/2 Keyboard | 8042 controller, port 0x60/0x64 | Hook IRQ 1, scan code decoding | Allegro's dkeybd.c / DOS INT 9 |
| PS/2 Mouse | 8042 controller, port 0x60/0x64 | Hook IRQ 12, PS/2 mouse protocol | Allegro's dmouse.c / DOS INT 33 |
| VGA/VESA | VGA ports 0x3C0-0x3DA, VESA via V86 INT 10h | Direct port I/O for VGA, V86 for VESA mode setting | Allegro's vesa.c |

### Memory Management

- **Physical page allocator** — bitmap or free-list tracking which 4KB pages are free
- **Virtual memory mapper** — create/modify page tables, map physical pages to linear addresses
- **Kernel heap** — simple malloc for kernel data structures (slab allocator or linked-list)
- **User memory** — separate address space per task (different CR3 / page directory)

### The V86 Monitor

This is the key piece CWSDPMI couldn't give us:

1. Create a TSS with VM bit set in EFLAGS (386-bible p.217, 222)
2. Map lower 1MB of task's address space to physical RAM (386-bible p.218)
3. Set up I/O permission bitmap in TSS (386-bible p.149-151)
4. Trap sensitive instructions (CLI, STI, INT, IN, OUT) via GPF handler (386-bible p.221)
5. Emulate them — virtualise hardware per-task
6. **Intercept INT 10h** — redirect video output to a capture buffer instead of real VGA
7. **Intercept INT 16h** — feed keyboard input from our input queue
8. **Intercept INT 21h** — handle DOS API calls ourselves (or pass to real DOS via V86)

This is exactly what Windows 3.1 Enhanced Mode did. And since we're Ring 0, we CAN do it.

### Task Switching

**Preemptive, timer-driven:**
1. PIT fires IRQ 0 at our chosen frequency (e.g., 100 Hz)
2. Timer ISR saves current task state (registers → task's save area)
3. Scheduler picks next task
4. Load next task's state (restore registers from save area)
5. If next task is V86, ensure VM bit is set in EFLAGS on IRET
6. IRET to next task

Can use hardware TSS switching (JMP to task gate) or software switching (manual save/restore — more flexible, what Linux does).

---

## The Allegro IRQ 8 Problem — Solved

**The problem under DOS:**
- Allegro hooks INT 8 (PIT timer IRQ 0) via DPMI INT 31h/0x0205 (allegro: src/dos/dtimer.c)
- CWSDPMI owns the real IRQ handler, reflects to Allegro's PM handler
- If we're doing our own timer-driven preemption, we conflict with Allegro's timer

**The solution in standalone kernel:**
- We own IRQ 0 directly — no DPMI, no conflict
- Our timer ISR does preemptive switching AND calls Allegro's timer callbacks
- Or better: don't use Allegro's timer system at all — our kernel timer is simpler and more efficient
- Allegro's software rendering doesn't need timers — only the input/timer subsystem does

---

## CWSDPMI Modification vs Own Kernel — Decision Matrix

| Factor | Modify CWSDPMI | Own Kernel |
|--------|---------------|------------|
| Ring 0 access | Already have it (in CWSDPMI) but must modify CWSDPMI itself | We ARE Ring 0 |
| V86 tasks | Must add — CWSDPMI doesn't support creating them for user code | Build from scratch with 386 bible |
| INT 21h reentrancy | Still a problem — DOS underneath | No DOS — no reentrancy |
| Timer conflict | Must coordinate with CWSDPMI's timer handling | We own the timer |
| Drivers | Still using DOS drivers via INT reflection | Our own — clean, no legacy |
| Complexity of changes | Modifying ~7500 lines of unfamiliar ASM+C, fighting existing design | Building ~3000-5000 lines of new code, following our design |
| Allegro integration | Must keep DPMI layer working | Strip to software renderer, write thin platform layer |
| Debugging | Harder — breaking CWSDPMI breaks everything | Harder initially — but cleaner once working |
| Risk | Medium-high — could break DPMI for DJGPP apps | Medium — well-documented in 386 bible and OSDev community |

**Verdict: Own kernel wins.** The complexity is similar, but the architecture is cleaner, and we don't have to fight CWSDPMI's assumptions.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| 386 Bible Ch.10 (Init) | i386-bible/pages/page_0174-0186 | Complete init sequence with code examples |
| 386 Bible Ch.7 | i386-bible/pages/page_0130-0144 | TSS, task switching |
| 386 Bible Ch.15 | i386-bible/pages/page_0217-0223 | V86 mode, V86 monitor |
| 386 Bible Ch.5 | i386-bible/pages/page_0091-0105 | Paging, page tables |
| 386 Bible Ch.8 | i386-bible/pages/page_0145-0158 | Exceptions, IDT, interrupt gates |
| 386 Bible Ch.9 | i386-bible/pages/page_0149-0151 | I/O permission bitmap |

---

*Last updated: 2026-04-28*
