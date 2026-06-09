# DOS Boot Stub — Loading Pinecore from FreeDOS

> How to build a 16-bit DOS program that loads the 32-bit kernel and transitions to protected mode. The "WIN.COM" equivalent for Pinecore.

**Date:** 2026-04-30
**Status:** Research needed — architecture documented, implementation details require CWSDPMI source study

---

## Overview

Pinecore follows the Windows 98 model:
1. FreeDOS boots normally (kernel.sys, command.com)
2. User runs `PINE.COM` (or AUTOEXEC.BAT runs it)
3. PINE.COM saves real-mode state, loads kernel, transitions to PM
4. Pinecore kernel runs (our existing code)
5. On exit, tears down PM, restores real-mode state, returns to FreeDOS C:\>

---

## PINE.COM Structure

A DOS .COM file (max 64KB, org 100h) that:

### Phase 1: Preparation (real mode, DOS services available)
1. Print banner: "Starting Pinecore..."
2. Detect CPU: verify 386+ (check EFLAGS bit 18 toggle) (386-bible p.174)
3. Detect mode: SMSW to check PE bit (cwsdpmi: mswitch.asm:450-454)
4. If V86: check VCPI availability (INT 67h AX=DE00h) (cwsdpmi: vcpi.asm:73-83)
5. Open KERNEL.BIN from disk (INT 21h/3Dh)
6. Get file size (INT 21h/42h seek to end)
7. Allocate memory for kernel (INT 21h/48h or use XMS)
8. Read kernel into memory (INT 21h/3Fh)
9. Close file (INT 21h/3Eh)

### Phase 2: Save real-mode state
1. Save all segment registers (CS, DS, ES, FS, GS, SS)
2. Save SP
3. Save IVT (256 vectors, 1KB at 0000:0000)
4. Save PIC mask registers (ports 0x21, 0xA1)
5. Save PIT state (if possible)
6. Store return address for exit-to-DOS

### Phase 3: Set up PM structures (still in real mode)
1. Build GDT in conventional memory (8 entries — see ch-10)
2. Build IDT entries (48 interrupt gates for our ISR stubs)
3. Set up page directory + page tables (identity-map first 4MB + kernel)
4. Set up TSS
5. Enable A20 gate (keyboard controller method or fast A20 port 0x92)

### Phase 4: Transition to PM
- **If real mode:** direct transition (ch-10 Path 1)
  - CLI, LGDT, LIDT, set PE bit, far JMP, load segments, enable paging, LTR
- **If V86 mode:** VCPI transition (ch-10 Path 2)
  - Fill CLIENT structure, INT 67h AX=DE0Ch
  - Execution continues at our PM entry point

### Phase 5: Jump to kernel
- Call our 32-bit kernel entry point (kernel_main or equivalent)
- Kernel initialises its subsystems (PIC, PIT, keyboard, etc.)
- Kernel enters main loop

---

## Return-to-DOS Path

When the user exits Pinecore:

### Step 1: Tear down kernel
1. Disable interrupts (CLI)
2. Stop PIT timer (or restore original rate)
3. Mask all IRQs

### Step 2: Disable paging
```asm
MOV EAX, CR0
AND EAX, 0x7FFFFFFF    ; Clear PG bit
MOV CR0, EAX
XOR EAX, EAX
MOV CR3, EAX            ; Flush TLB
```

### Step 3: Return to real mode
```asm
; Load 16-bit compatible GDT entries (64KB limit, 16-bit)
MOV AX, rm_data_selector
MOV DS, AX
MOV ES, AX
MOV FS, AX
MOV GS, AX
MOV SS, AX

; Clear PE bit
MOV EAX, CR0
AND AL, 0xFE            ; Clear PE
MOV CR0, EAX

; Far JMP to real-mode CS:IP
DB 0xEA
DW offset real_mode_return
DW saved_cs_value
```
(386-bible p.186, cwsdpmi: mswitch.asm reverse path)

### Step 4: Restore real-mode state
1. Restore all saved segment registers
2. Restore SP
3. Restore IVT from saved copy
4. Restore PIC masks
5. Re-enable interrupts (STI)
6. Return to DOS (INT 21h/4Ch or RET)

---

## Where KERNEL.BIN Lives

Options:
1. **Same directory as PINE.COM** — `KERNEL.BIN` next to `PINE.COM` on the FAT disk
2. **Appended to PINE.COM** — single file, stub reads from itself (like COMMAND.COM's string resources)
3. **Fixed disk location** — kernel at known sectors (fragile)

Option 1 is simplest and most maintainable.

---

## Memory Map During Transition

```
0x00000 - 0x003FF  IVT (saved before transition)
0x00400 - 0x004FF  BIOS data area (preserved)
0x00500 - 0x07BFF  Free conventional memory
0x07C00 - 0x07DFF  Boot sector (may be overwritten)
0x08000 - 0x0FFFF  PINE.COM stub + PM setup structures (GDT, IDT, page tables)
0x10000 - 0x9FFFF  Kernel loaded here (up to ~576KB)
0xA0000 - 0xBFFFF  Video memory
0xC0000 - 0xFFFFF  ROM BIOS
0x100000+          Extended memory (kernel can use after PM transition)
```

---

## Build Considerations

The stub is 16-bit real-mode code. Must be built separately:
- **Assembler:** NASM with `bits 16` and `org 100h` (for .COM format)
- **Output:** flat binary .COM file
- **Kernel:** built with i686-elf-gcc as currently (flat binary or ELF)
- **No linking needed:** stub and kernel are separate files

---

## Research Still Needed

1. **XMS vs direct loading:** If kernel is > 64KB, need XMS (INT 15h/AH=87h block move) or unreal mode to copy to extended memory
2. **Return-to-DOS from VCPI:** Does VCPI provide a return path, or must we do it manually?
3. **A20 gate methods:** Which method is most reliable? (cwsdpmi: exp.asm has all three)
4. **Interrupt state on return:** Do we need to reinitialise the PIC to the BIOS mapping (IRQ 0-7 at INT 8-15)?

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| CWSDPMI mswitch.asm | cwsdpmi-master/src/mswitch.asm | PM transition + return |
| CWSDPMI vcpi.asm | cwsdpmi-master/src/vcpi.asm | VCPI detection and switch |
| CWSDPMI exp.asm | cwsdpmi-master/src/exp.asm | A20 gate methods |
| 386 Bible Ch.10 | i386-bible/pages/page_0174-0186 | PM init sequence |
| 386 Bible Ch.15 | i386-bible/pages/page_0217-0223 | V86 mode |
| Research ch-10 | docs/research/10-pm-transition.md | Our PM transition notes |
| Research ch-08 | docs/research/08-windows98-model.md | Windows 98 architecture model |

---

*Last updated: 2026-04-30*
