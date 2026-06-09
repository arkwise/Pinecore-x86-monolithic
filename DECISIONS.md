# Decisions Register

---

## Architecture Decisions

```
DECIDED: Standalone Ring 0 system — FreeDOS boots us, then dormant — WHY: DOS is not reentrant, sharing INT 21h between preemptive tasks causes lockups — SEE: ch-09
DECIDED: Own FAT driver + ATA/IDE driver — WHY: Can't share FreeDOS file I/O between tasks, InDOS flag only detects problem not solves it — SEE: ch-09
DECIDED: Own DOS emulation layer (~40-50 INT 21h functions) — WHY: FREECOM calls INT 21h, we handle it ourselves, fully reentrant — SEE: ch-09
DECIDED: FREECOM runs unmodified in V86 tasks — WHY: Intercept its INTs at GPF level, no source changes needed — SEE: ch-08
DECIDED: Build custom window manager — WHY: Allegro 4.x only has modal dialogs, no overlapping windows — SEE: ch-04
DECIDED: Port Allegro software renderer (src/c/) — WHY: ~18,500 lines of tested portable C rendering, ~200 lines platform layer — SEE: ch-07
DECIDED: V86 monitor for DOS program windows — WHY: Intercept INT 21h (screen), INT 10h, INT 16h per-task — SEE: ch-08
DECIDED: Own ALL drivers (display, input, timer, disk, filesystem) — WHY: Can't share any DOS services between preemptive tasks — SEE: ch-09
DECIDED: Preemptive multitasking via PIT timer — WHY: At Ring 0 we own IRQ 0 directly — SEE: ch-05
```

## Technology Choices

```
DECIDED: i386-elf cross-compiler for Ring 0 code — WHY: DJGPP can't produce bare-metal ELF/flat binaries. i386 target for 386 hardware compatibility (kernel without desktop). Desktop minimum is Pentium MMX. — SEE: ch-06
DECIDED: FreeDOS boots our program as a normal DOS executable — WHY: Like WIN.COM, launched from DOS prompt or AUTOEXEC.BAT — SEE: ch-08
DECIDED: VCPI (INT 67h) for PM transition if EMM386 loaded — WHY: Standard mechanism, same as Windows 3.1/98 used — SEE: ch-08
DECIDED: Direct PM transition if no EMM386 — WHY: Simpler path, CLI → GDT → PE bit → JMP — SEE: ch-08, 386-bible p.176
```

## Naming

```
DECIDED: Kernel name is "Pinecore" — date: 2026-04-30
DECIDED: Boot stub is PINE.COM — launches Pinecore from FreeDOS prompt — date: 2026-04-30
```

## Shell Decisions

```
DECIDED: Built-in Linux-like shell alongside FREECOM — WHY: Custom kernel-mode shell with ls/cat/cp/rm/mkdir commands, doesn't need V86. FREECOM still available as a V86 DOS shell option. — date: 2026-04-30
DECIDED: Built-in shell runs in kernel mode — WHY: Direct access to FAT driver, keyboard, VGA. No V86 overhead, no DOS emulation needed. — date: 2026-04-30
DECIDED: Virtual terminal system with Alt+1..6 switching — WHY: Multiple shells running simultaneously, demonstrates preemptive multitasking. Like Linux VTs. — date: 2026-04-30
DECIDED: VT1 = Pinecore shell (default), VT2 = COMMAND.COM — WHY: Show the native shell first, DOS available on demand — date: 2026-04-30
DECIDED: Each VT has its own 80x25 shadow buffer — WHY: Active VT renders to VGA, inactive VTs buffer silently. Already have text_buf[] in v86_task struct. — date: 2026-04-30
DECIDED: Status bar on VGA row 0 — WHY: Shows VT tabs, active indicator, hotkey hints. Shells render from row 1. — date: 2026-05-01
DECIDED: Software context switch (not hardware TSS) — WHY: Faster, flexible, matches Linux/Windows. ESP swap in timer ISR. — date: 2026-05-01
DECIDED: Distribution = single 1.44MB floppy (pinecore-dist.img) — FDC driver + FAT12 working. — date: 2026-05-01
DECIDED: IRQ8 (RTC) for preemption at 8192 Hz — WHY: Frees IRQ0 (PIT) for audio/timing. RTC can do 8192 Hz via CMOS rate register. Scheduler moves from INT 32 to INT 40. — date: 2026-05-01 (planned)
DECIDED: VESA 3.0 support target — WHY: PM interface avoids V86 for mode setting on newer hardware. Backwards compat: VBE 2.0 LFB via V86 INT 10h, VGA via direct port I/O. — date: 2026-05-01 (planned)
```

## Process Decisions

```
DECIDED: Research all unknowns before any code — WHY: Ring 0 + V86 monitor + Allegro port is complex — SEE: roadmap.md
DECIDED: Build i386-elf cross-compiler as first implementation step — WHY: Nothing else can be built without it — SEE: ch-06
DECIDED: Keep Multiboot path for QEMU development alongside FreeDOS boot — WHY: Fast iteration, don't need real FreeDOS disk for every test — date: 2026-04-30
```

## Rejected Alternatives

```
REJECTED: Sharing FreeDOS for file I/O (Windows 98 model) — WHY: DOS is not reentrant, InDOS critical section blocks all tasks, system locks up — SEE: ch-09
REJECTED: CWSDPMI + DJGPP (Ring 3 app) — WHY: Can't create V86 tasks, can't own timer, can't intercept FREECOM's INTs — SEE: ch-02, ch-03
REJECTED: Modifying CWSDPMI — WHY: ~7500 lines coupled to DOS, fighting its design — SEE: ch-05
REJECTED: Pseudo-terminal (fake shell) — WHY: With Ring 0 we can run real FREECOM unmodified in V86 — SEE: ch-05, ch-08
REJECTED: DJGPP for Ring 0 code — WHY: Locked to COFF-go32, no ELF output — SEE: ch-06
```

---

*Last updated: 2026-04-28*
