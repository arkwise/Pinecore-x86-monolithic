# CWSDPMI Internals — What the DPMI Host Provides

> Understanding what we can and can't do from Ring 3 under CWSDPMI r7.

**Date:** 2026-04-28
**Status:** Complete — reference only, not using CWSDPMI but valuable for PM patterns

---

## Findings

### Architecture Overview

CWSDPMI is a DPMI 0.9 server (with some 1.0 extensions) that hosts DJGPP applications in 32-bit protected mode.

**Execution model:**
- CWSDPMI runs at Ring 0 (cwsdpmi: gdt.h — g_rcode selector is Ring 0)
- DJGPP applications run at Ring 3 (cwsdpmi: gdt.h — g_pcode has RPL=3)
- All communication via INT 31h (DPMI services) (cwsdpmi: exphdlr.c:580+)
- Interrupts trap to Ring 0, then reflect to application if handler installed

### TSS Setup

Four TSS instances (cwsdpmi: tss.h):

| TSS | Selector | Purpose |
|-----|----------|---------|
| `_c_tss` | g_ctss (104) | CWSDPMI real-mode context |
| `_a_tss` | g_atss (96) | Application running context |
| `_i_tss` | g_itss (112) | Interrupt handler context |
| `_f_tss` | (separate) | Page fault handler context |

Each TSS includes: all GPRs, segment regs, EFLAGS, CR3, Ring 0 stack (SS0:ESP0), I/O bitmap pointer, error code, CR2, IRQ number. (cwsdpmi: tss.h)

### Mode Switching

**Protected → Real (cwsdpmi: mswitch.asm `_go_real_mode`):**
1. Save debug registers DR0-DR7
2. If VCPI: call INT 67h/0xDE0C (VCPI mode change)
3. If bare hardware: clear PE+PG bits in CR0, far jump to real-mode segment
4. Restore real-mode IDT (1024 bytes)
5. Execute real-mode code
6. Return via reverse process

**Real → Protected (cwsdpmi: mswitch.asm `_go32`):**
1. LGDT, LIDT
2. Set PE bit in CR0, far jump to flush prefetch
3. Set CR3 (page directory), enable PG bit
4. Load TSS via `jmpt g_atss`

### DPMI Services We Can Use (INT 31h)

**Memory Management:**
- 0x0501: Allocate memory block — returns linear address + handle (cwsdpmi: exphdlr.c)
- 0x0502: Free memory block (cwsdpmi: exphdlr.c)
- 0x0503: Resize memory block (cwsdpmi: exphdlr.c)
- 0x0600: Lock linear region (prevents paging) (cwsdpmi: exphdlr.c)
- 0x0800: Physical address mapping — map physical memory to linear address (cwsdpmi: exphdlr.c)

**Descriptor Management:**
- 0x0000: Allocate LDT descriptors (cwsdpmi: exphdlr.c)
- 0x0001: Free LDT descriptor (cwsdpmi: exphdlr.c)
- 0x0007: Set segment base address (cwsdpmi: exphdlr.c)
- 0x0008: Set segment limit (cwsdpmi: exphdlr.c)
- 0x0009: Set descriptor access rights (cwsdpmi: exphdlr.c)

**Interrupt Management:**
- 0x0200: Get real-mode interrupt vector (cwsdpmi: exphdlr.c)
- 0x0201: Set real-mode interrupt vector (cwsdpmi: exphdlr.c)
- 0x0204: Get protected-mode interrupt vector (cwsdpmi: exphdlr.c)
- 0x0205: Set protected-mode interrupt vector (cwsdpmi: exphdlr.c)

**Real-Mode Simulation:**
- 0x0300: Simulate real-mode interrupt (cwsdpmi: exphdlr.c)
- 0x0301: Call real-mode far procedure with IRET frame (cwsdpmi: exphdlr.c)
- 0x0302: Call real-mode far procedure (cwsdpmi: exphdlr.c)
- 0x0303: Allocate real-mode callback (cwsdpmi: exphdlr.c)
- 0x0304: Free real-mode callback (cwsdpmi: exphdlr.c)

**Exception Handling:**
- 0x0202: Get processor exception handler (cwsdpmi: exphdlr.c)
- 0x0203: Set processor exception handler (cwsdpmi: exphdlr.c)

### Interrupt Handling Architecture

**Flow for hardware interrupts (cwsdpmi: tables.asm, exphdlr.c):**
1. Hardware interrupt → IDT lookup (256 entries)
2. `_ivecN` handler: push DS, call `ivec_common`
3. `ivec_common`: compute vector number, task-jump to `_i_tss`
4. `interrupt_common`: extract error code, save context
5. Check for user handler: `user_interrupt_handler[irqn]`
6. If user handler exists: task-jump to application TSS, call handler at Ring 3
7. If no user handler: reflect to real mode via `_go_real_mode`

**For timer interrupt (IRQ 0, INT 8):**
Allegro hooks this via DPMI INT 31h/0x0205 to get its timer callbacks. This is how Allegro's `install_timer()` works. (allegro: src/dos/dtimer.c)

### Page Table / Memory Mapping

**Page entry flags (cwsdpmi: paging.h):**
- PT_P (0x001): Present
- PT_W (0x002): Writable
- PT_U (0x004): User accessible
- PT_A (0x020): Accessed
- PT_D (0x040): Dirty

**We cannot directly access page tables from Ring 3.** But DPMI provides:
- Memory allocation (which sets up page mappings internally)
- Physical address mapping (0x0800) — maps physical addresses to our linear space
- This is how Allegro maps the video framebuffer

### What This Means for Our Project

**We CAN do from Ring 3:**
- Allocate/free memory
- Map physical addresses (e.g., video memory)
- Install interrupt handlers (timer, keyboard, mouse)
- Simulate real-mode interrupts (call DOS/BIOS functions)
- Allocate real-mode callbacks (PM code called from RM interrupts)

**We CANNOT do from Ring 3:**
- Create new TSS structures or task gates
- Modify the GDT/IDT directly
- Enable/disable paging or change CR3
- Run V86 mode tasks (that's a Ring 0 operation)
- Preemptively schedule tasks (we don't control the timer at Ring 0)

**Practical implication:** Our multitasking must be cooperative (coroutine-style) or use Allegro's timer interrupt for lightweight preemption of our own code.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| CWSDPMI exphdlr.c | cwsdpmi-master/src/exphdlr.c | All DPMI INT 31h function implementations |
| CWSDPMI tss.h | cwsdpmi-master/src/tss.h | TSS structure, 4 instances |
| CWSDPMI mswitch.asm | cwsdpmi-master/src/mswitch.asm | Mode switching, task gate jumps |
| CWSDPMI tables.asm | cwsdpmi-master/src/tables.asm | IDT, interrupt dispatch |
| CWSDPMI paging.c/h | cwsdpmi-master/src/paging.c | Page table management, flags |
| CWSDPMI gdt.h | cwsdpmi-master/src/gdt.h | GDT selectors, Ring 0 vs Ring 3 |

---

*Last updated: 2026-04-28*
