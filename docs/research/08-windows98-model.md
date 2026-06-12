# The Windows 98 Model — GUI Shell On Top of DOS

> We're not replacing FreeDOS. We're doing what Windows 98 did to MS-DOS.

**Date:** 2026-04-28
**Status:** Complete — partially superseded by ch-09 (reentrancy requires own drivers, not shared DOS)

---

## How Windows 95/98 Actually Worked

Windows 98 was NOT a standalone OS. It was a graphical shell that took over from DOS:

1. **MS-DOS boots normally** — real mode, loads COMMAND.COM, runs AUTOEXEC.BAT
2. **WIN.COM starts** — launched from DOS prompt or AUTOEXEC.BAT
3. **VMM32.VXD loads** — transitions to Ring 0 protected mode, becomes the "real" OS
4. **DOS continues in V86 mode** — the original DOS session becomes a V86 task
5. **GUI runs in protected mode** — the desktop, windows, and apps run in 32-bit PM
6. **DOS programs get V86 tasks** — each DOS box is a separate V86 task
7. **File I/O goes through DOS** — even Windows programs call INT 21h (through a PM wrapper) for file access, and it's routed to DOS running in V86 mode

**The key insight:** Windows 98 didn't replace DOS for file I/O, disk access, or boot. It replaced DOS for: display, input, memory management, scheduling, and multitasking. DOS became a "file system and driver layer" running underneath in V86 mode.

### What Windows 98 Took Over vs What DOS Kept

| Component | Windows 98 | DOS |
|-----------|-----------|-----|
| Display/graphics | Windows (GDI, DirectDraw) | No — DOS only sees text mode in V86 |
| Keyboard/mouse | Windows (input manager) | No — Windows intercepts at hardware level |
| Memory management | Windows (VMM, paging) | No — Windows owns all memory |
| Scheduling | Windows (preemptive for Win32, cooperative for Win16) | No |
| File I/O | Routes through to DOS INT 21h | Yes — DOS kernel handles FAT filesystem |
| Disk drivers | Windows VxDs OR DOS drivers in V86 | Shared |
| Boot | DOS boots, Windows takes over | Yes — DOS is the bootloader |
| DOS programs | Windows creates V86 task per DOS box | DOS code runs in V86 mode |

---

## Our Architecture (The FreeDOS Desktop)

We follow the same model:

### Boot Sequence
1. FreeDOS boots normally (real mode)
2. FreeDOS loads our program (a DOS executable that transitions to PM)
3. Our program takes over at Ring 0 — sets up GDT, IDT, paging, TSS
4. FreeDOS continues running in a V86 task (it doesn't know anything changed)
5. Our desktop starts — Allegro-based GUI renders on screen
6. FREECOM (command.com) runs in V86 tasks inside windows

### What We Take Over vs What FreeDOS Keeps

| Component | Our Desktop Shell | FreeDOS |
|-----------|------------------|---------|
| Display | Ported Allegro renderer, VESA framebuffer | No — we intercept V86 video |
| Keyboard | Our PS/2 driver or interrupt handler | No — we feed keys to V86 tasks |
| Mouse | Our PS/2 driver | No |
| Timer | Our PIT driver (scheduling + GUI) | No |
| Memory management | Our paging + physical allocator | No — we control all memory |
| Scheduling | Our preemptive scheduler | No |
| File I/O | Routes through V86 to FreeDOS INT 21h | Yes — FreeDOS handles FAT |
| Disk I/O | Routes through V86 to FreeDOS/BIOS | Yes — FreeDOS disk drivers |
| DOS programs | V86 tasks with captured I/O | FreeDOS kernel provides INT 21h |
| Boot | FreeDOS boots us | Yes |

### The Critical Difference From Earlier Research

- **ch-05 said "replace FreeDOS"** — WRONG. We don't need to replace it.
- FreeDOS gives us a complete DOS environment for free: FAT filesystem, INT 21h API, device drivers, boot
- We just need to take over the "top half" — display, input, scheduling
- FREECOM runs as a real DOS program in V86 mode, using real INT 21h, and we capture its screen output

### Why This Is Better Than Full Standalone

| Factor | Full Standalone (ch-05) | Windows 98 Model |
|--------|------------------------|------------------|
| Filesystem | Must write or port one | FreeDOS FAT for free |
| Disk I/O | Must write ATA/IDE driver | FreeDOS/BIOS handles it |
| Boot | Must integrate with GRUB/multiboot | FreeDOS boots us normally |
| DOS compatibility | Must emulate INT 21h ourselves | Real FreeDOS provides it |
| FREECOM integration | Must emulate all DOS services | FREECOM talks to real FreeDOS via V86 |
| Code to write | More | Less |

---

## How We Transition to Ring 0

This is the one tricky part. We're a DOS program that needs to become Ring 0.

**Option A: Use VCPI (Virtual Control Program Interface)**
- If EMM386 is loaded, we can use VCPI (INT 67h) to get to Ring 0
- VCPI was designed exactly for this — it lets a DOS program set up its own protected mode
- CWSDPMI uses VCPI this way (cwsdpmi: vcpi.asm)
- Windows 3.1 Enhanced Mode used VCPI

**Option B: Direct transition (no EMM386)**
- If no EMM386, we're in real mode and can transition directly
- CLI, set up GDT, LGDT, set PE bit, far JMP (386-bible p.176)
- Simpler but requires no memory manager to be loaded

**Option C: Replace CWSDPMI**
- Write our own DPMI-like host that stays resident
- Loads as a TSR or DOS driver
- When our GUI program starts, it's already in Ring 0

**Windows 98 used Option A** (VCPI through EMM386 or direct if no EMM386).

---

## FREECOM I/O Patterns (from source analysis)

FREECOM at `freecom-master/` uses:

**Output (what we need to capture in V86):**
| Method | INT | Function | What It Does |
|--------|-----|----------|-------------|
| DOS write char | INT 21h | AH=02h | Writes one character to stdout |
| DOS write string | INT 21h | AH=09h | Writes $-terminated string |
| BIOS video | INT 10h | AH=06h | Scroll up (CLS uses this) |
| BIOS video | INT 10h | AH=02h | Set cursor position |
| BIOS video | INT 10h | AH=0Fh | Get video mode/columns |
| Direct memory | 0x0040:xxxx | N/A | Read BIOS data area (screen size, cursor pos) |

**Input (what we need to inject):**
| Method | INT | Function | What It Does |
|--------|-----|----------|-------------|
| DOS buffered input | INT 21h | AH=0Ah | Read line into buffer |
| BIOS key check | INT 16h | AH=01h | Check if key pressed (used by cmdinput.c) |
| BIOS read key | INT 16h | AH=00h | Read keystroke (blocking) |

**Execution (what we need to monitor):**
| Method | INT | Function | What It Does |
|--------|-----|----------|-------------|
| DOS EXEC | INT 21h | AH=4Bh | Execute external program (via lowexec.asm) |

**All of these are interceptable via our V86 monitor's GPF handler.** When FREECOM does INT 21h in V86 mode, it traps to our Ring 0 handler. We decide: pass through to real FreeDOS (for file I/O), or redirect (for screen/keyboard).

### The Interception Strategy

```
FREECOM in V86 → executes INT xxh → GPF trap to our Ring 0 handler
  |
  ├── INT 21h AH=02h/09h (write to screen) → redirect to our text buffer
  ├── INT 21h AH=0Ah (read input) → return from our keyboard queue
  ├── INT 21h AH=4Bh (EXEC program) → create new V86 task for the child
  ├── INT 21h (file I/O) → pass through to real FreeDOS in V86
  ├── INT 10h (video) → redirect to our virtual screen buffer
  ├── INT 16h (keyboard) → return from our keyboard queue
  └── Other INTs → pass through to FreeDOS/BIOS
```

The beauty of this: FREECOM doesn't need any modification. It thinks it's running on a normal DOS system. We just intercept at the interrupt level.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| FREECOM shell core | freecom-master/shell/command.c | Main loop, command parsing, redirection |
| FREECOM keyboard input | freecom-master/lib/cmdinput.c | Enhanced input with INT 16h |
| FREECOM DOS input | freecom-master/lib/inputdos.c | DOS buffered input INT 21h/0Ah |
| FREECOM key check | freecom-master/lib/keyprsd.c | INT 16h AH=01h key pressed check |
| FREECOM exec | freecom-master/lib/exec.c, lib/lowexec.asm | INT 21h/4Bh external command execution |
| FREECOM CLS | freecom-master/cmd/cls.c | INT 10h video usage |
| 386 Bible Ch.15 | i386-bible/pages/page_0217-0223 | V86 mode, GPF trapping |
| CWSDPMI VCPI | cwsdpmi-master/src/vcpi.asm | VCPI usage pattern for PM transition |

---

*Last updated: 2026-04-28*
