# The DOS Reentrancy Problem — Why We Need Our Own Drivers

> DOS is not reentrant. Sharing it between preemptive tasks WILL lock up. We need our own everything.

**Date:** 2026-04-28
**Status:** Complete — this settles the architecture

---

## The Problem

DOS (including FreeDOS) was designed for a single-tasking system. Its kernel uses:
- **Global variables** for internal state (current DTA, current PSP, error codes)
- **A single stack** for kernel operations
- **No locking or mutual exclusion** — there are no mutexes, semaphores, or critical sections inside the DOS kernel

If Task A calls INT 21h to read a file, and we preempt it mid-call to give Task B a timeslice, and Task B also calls INT 21h:
1. Task B overwrites Task A's DOS stack frame
2. Task B overwrites global state (current DTA, PSP)
3. When Task A resumes, it's working with corrupted state
4. Result: data corruption, crashes, lockups

### This Was Windows 95/98's Achilles Heel

Windows 95/98 used a "DOS critical section" to work around this:
- A global flag `InDOS` (INT 21h/34h returns its address) indicates DOS is busy
- Only ONE V86 task can be inside INT 21h at a time
- Other tasks that try to call INT 21h are BLOCKED until the first one finishes
- This means: if one DOS box is doing a slow file read, ALL other DOS boxes freeze

This was the #1 reason Windows 95/98 was unreliable. The non-reentrant DOS kernel was a bottleneck that caused:
- System freezes when DOS programs did heavy disk I/O
- "Not Responding" for other DOS boxes waiting for the DOS critical section
- The infamous Windows 98 instability that everyone remembers

### The InDOS Flag

DOS provides INT 21h/AH=34h which returns ES:BX pointing to the InDOS flag. When nonzero, DOS is currently processing a request. There's also a "critical error" flag at the byte before InDOS.

Even with these flags, you can only DETECT the problem, not solve it. The only safe approach is: don't share DOS between preemptive tasks.

---

## The Solution: Own Everything

For a truly preemptive desktop that doesn't lock up, we cannot share the DOS kernel between tasks. We need:

### What We Must Write

| Component | Why We Can't Share DOS | Complexity |
|-----------|----------------------|-----------|
| **FAT filesystem driver** | INT 21h file ops are not reentrant | Medium — FAT12/16/32 are well-documented |
| **ATA/IDE disk driver** | BIOS INT 13h is not reentrant either | Medium — PIO mode is straightforward |
| **Keyboard driver (PS/2)** | INT 16h shares state with INT 9h handler | Low — direct 8042 port I/O |
| **Mouse driver (PS/2)** | INT 33h is not reentrant | Low — direct 8042 port I/O |
| **Timer driver (PIT)** | We need it for preemption anyway | Low — direct PIT port I/O |
| **Display driver (VGA/VESA)** | We're already doing this for Allegro | Already planned |
| **Memory manager** | We need our own paging regardless | Already planned |
| **Console I/O for V86** | FREECOM's INT 21h/02h, 09h go to DOS | We handle — redirect to text buffer |

### What We DON'T Need to Write

| Component | Why Not |
|-----------|---------|
| Boot code | FreeDOS boots us — we just take over from a running DOS |
| BIOS | BIOS is in ROM — we can call it via V86 for one-time init (VESA mode set) |
| Hardware detection | BIOS/FreeDOS already detected hardware during boot |

### The FREECOM Integration Changes

With our own file system and drivers, FREECOM in V86 mode talks to US, not FreeDOS:

```
FREECOM in V86 → executes INT 21h → GPF trap to our Ring 0 handler
  |
  ├── AH=02h/09h (write to screen) → our text buffer (already planned)
  ├── AH=0Ah (read input) → our keyboard queue (already planned)
  ├── AH=3Dh (open file) → our FAT driver
  ├── AH=3Eh (close file) → our FAT driver
  ├── AH=3Fh (read file) → our FAT driver
  ├── AH=40h (write file) → our FAT driver
  ├── AH=41h (delete file) → our FAT driver
  ├── AH=4Bh (EXEC) → our process manager (new V86 task)
  ├── AH=4Ch (EXIT) → our process manager (destroy V86 task)
  ├── AH=4Eh/4Fh (find first/next) → our FAT driver
  └── All other INT 21h → our DOS emulation layer
```

This is MORE code than the ch-08 "pass through to FreeDOS" approach, but it's the ONLY way to have:
- True preemptive multitasking (no DOS critical section bottleneck)
- Multiple simultaneous DOS boxes that don't freeze each other
- A system that doesn't lock up when one DOS program does disk I/O

### How Big Is a DOS Emulation Layer?

Not as big as you'd think. FREECOM uses a subset of INT 21h. The essential functions:

**File I/O (needs FAT driver):**
- 3Ch: Create file
- 3Dh: Open file
- 3Eh: Close file
- 3Fh: Read file
- 40h: Write file
- 41h: Delete file
- 42h: Seek in file
- 43h: Get/set file attributes
- 4Eh/4Fh: Find first/find next (directory search)
- 56h: Rename file
- 57h: Get/set file date/time
- 39h/3Ah/3Bh/47h: Mkdir/rmdir/chdir/getcwd

**Console I/O (already planned for V86 capture):**
- 01h/02h: Read/write character
- 06h: Direct console I/O
- 09h: Write string
- 0Ah: Buffered input
- 0Bh: Check input status
- 0Ch: Flush and read

**Process management (needs our scheduler):**
- 4Bh: EXEC (load and execute)
- 4Ch: EXIT (terminate)
- 4Dh: Get return code
- 31h: Terminate and stay resident

**Environment/misc:**
- 25h/35h: Set/get interrupt vector
- 2Ah/2Ch: Get date/time
- 30h: Get DOS version (just return 7.10 for FreeDOS compat)
- 44h: IOCTL
- 48h/49h/4Ah: Allocate/free/resize memory
- 62h: Get PSP address

That's roughly 40-50 INT 21h subfunctions. Each handler is typically 10-50 lines. The FAT driver is the biggest piece (~1000-2000 lines for read/write/directory support).

---

## Revised Architecture Summary

```
┌─────────────────────────────────────────────┐
│  FREECOM V86 Task  │  FREECOM V86 Task  │   │  ← Multiple DOS windows
│  (command.com)      │  (command.com)     │   │
├─────────────────────┴────────────────────┤   │
│            INT 21h / INT 10h / INT 16h       │  ← V86 GPF traps
├──────────────────────────────────────────────┤
│         Our Ring 0 Interrupt Handlers         │
│  ┌──────────┐ ┌──────────┐ ┌──────────────┐ │
│  │DOS Emu   │ │Video Cap │ │ Keyboard     │ │
│  │(INT 21h) │ │(INT 10h) │ │ (INT 16h)    │ │
│  └────┬─────┘ └──────────┘ └──────────────┘ │
│       │                                       │
│  ┌────┴─────┐ ┌──────────┐ ┌──────────────┐ │
│  │FAT Driver│ │ATA/IDE   │ │PS/2 Keyboard │ │
│  │          │ │Driver    │ │PS/2 Mouse    │ │
│  └──────────┘ └──────────┘ └──────────────┘ │
├──────────────────────────────────────────────┤
│  Allegro Software Renderer (ported src/c/)   │
│  Window Manager │ Desktop │ Event Loop       │
├──────────────────────────────────────────────┤
│  Memory Manager (paging) │ Scheduler (PIT)   │
├──────────────────────────────────────────────┤
│  GDT │ IDT │ TSS │ Page Tables               │  ← Ring 0 setup
├──────────────────────────────────────────────┤
│  FreeDOS (boot only — dormant after takeover)│
│  BIOS (ROM — used via V86 for one-time init) │
└──────────────────────────────────────────────┘
```

FreeDOS's role shrinks to just booting us. After we take over, it's dormant. We provide all services ourselves. FREECOM doesn't know the difference — it just calls INT 21h and gets answers.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| DOS InDOS flag | INT 21h/AH=34h | Reentrancy detection (not solution) |
| FREECOM I/O patterns | freecom-master/lib/cmdinput.c, inputdos.c | What INTs FREECOM actually calls |
| FREECOM EXEC | freecom-master/lib/exec.c, lowexec.asm | How external commands are launched |
| 386 Bible V86 | i386-bible/pages/page_0217-0223 | V86 GPF trapping for INT emulation |

---

*Last updated: 2026-04-28*
