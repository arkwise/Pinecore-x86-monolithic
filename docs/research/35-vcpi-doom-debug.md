# Chapter 35 — VCPI/DOOM Debug Session: DOS/4GW Extender Analysis

## Overview

This chapter documents the debugging session investigating why DOOM.EXE hangs
after VCPI initialization under Pinecore. DOOM uses the DOS/4GW extender
(originally DOS/16M by Tenberry/Rational Systems), which has a built-in DPMI
server that uses VCPI internally to switch from V86 to protected mode.

## Key Finding: Why DOOM Uses VCPI, Not DPMI

DOS/4GW has its own built-in DPMI server. Detection logic:
1. If an external DPMI host responds (INT 2Fh/1687h), DOS/4GW uses it
2. If no external DPMI BUT VCPI is present, DOS/4GW uses VCPI to build
   its own DPMI environment

Since Pinecore runs in V86 mode, DOS/4GW detects this (SMSW shows PE=1)
and knows VCPI is the only way to escape V86. Our DPMI advertisement via
INT 2Fh/1687h is ignored.

## VCPI 1.0 Specification Summary

All functions called via `INT 67h` with AH=DEh from V86 mode.

| Func  | Name                    | Key I/O                                    |
|-------|-------------------------|--------------------------------------------|
| DE00h | Detect VCPI             | AH=0 present, BX=version                   |
| DE01h | Get PM Interface        | ES:DI=PTE buf, DS:SI=3 GDT entries, ret EBX=entry offset |
| DE02h | Max Phys Address        | EDX=highest 4K page                         |
| DE03h | Free 4K Pages           | EDX=count                                   |
| DE04h | Allocate 4K Page        | EDX=phys addr                               |
| DE05h | Free 4K Page            | EDX=phys addr                               |
| DE06h | Get Phys Addr of 1st MB | CX=page#, EDX=phys                          |
| DE07h | Read CR0                | EBX=CR0                                     |
| DE0Ah | Get PIC Vectors         | BX=master, CX=slave                         |
| DE0Bh | Set PIC Vectors         | BX=master, CX=slave (informational)         |
| DE0Ch | Switch to PM            | **ESI**=linear addr of structure (NOT stack) |

### DE0Ch Data Structure (at ESI, in first MB)

```
+00h  DWORD  CR3 (page directory physical address)
+04h  DWORD  Linear address of 6-byte GDTR pseudo-descriptor
+08h  DWORD  Linear address of 6-byte IDTR pseudo-descriptor
+0Ch  WORD   LDTR selector
+0Eh  WORD   TR selector
+10h  DWORD  EIP of PM entry point
+14h  WORD   CS of PM entry point
```

**CRITICAL**: The structure address is in **ESI**, not on the stack.
Our original implementation read from the stack — this was a bug.

### DE01h Details

The server provides:
- 3 GDT descriptors (24 bytes at DS:SI): VCPI code, VCPI data, client code
- Page table entries in the buffer at ES:DI (identity map of server's pages)
- EBX = offset of PM entry point within the first GDT code segment

The client must include the VCPI code segment in its own GDT so it can
FAR CALL back to the server for DE0Ch mode switching from PM.

### PM→V86 Switch (DE0Ch from PM)

Client pushes V86 return frame on stack before FAR CALL to VCPI entry:
```
ESP→ [GS] [FS] [DS] [ES] [SS] [ESP] [EFLAGS] [CS] [EIP]
```

(cwsdpmi: mswitch.asm:220-239)

## INT 15h/BFxxh — DOS/16M Proprietary API

DOS/4GW uses these to detect a pre-installed extender instance:

- **BF00h, BF01h**: Unknown, used by DOS/4GW
- **BF02h**: Installation check — DX=0 means not installed. If DX!=0,
  DX:SI → XBRK structure with GDT/IDT/memory info
- **BFDEh**: DESQview/X integration
- **BF03h**: Uninstall
- **BF05h**: Initialize PM interface

**CRITICAL BUG FOUND**: Our INT 15h handler returned CF=1 for unknown
subfunctions but did NOT clear DX. If DX had garbage from a previous
operation, the extender could think a host was installed and try to
use a stale pointer from DX:SI. Fixed by explicitly zeroing DX and SI.

## DOOM's Observed VCPI Initialization Sequence

```
1. INT 15h/AX=BFDEh     — DESQview/X check (we return CF=1, DX=0)
2. INT 67h AX=DE00h      — VCPI detect → present v1.0
3. INT 67h AX=DE0Ah      — Get PIC vectors → master=0x20, slave=0x28
4. INT 15h/AX=BF02h      — DOS/16M host check (we return CF=1, DX=0)
5. INT 67h AH=42h AL=02h — EMS: get page count
6. INT 67h AX=DE03h       — VCPI: free 4K pages
7. INT 67h AX=DE01h       — VCPI: get PM interface
8. INT 15h/AX=8800h       — Extended memory size → 31744 KB
9. *** HANGS ***          — extender loops at CS=0x0006:IP=0xB03E
```

After step 8, NO more GPFs occur. The extender runs pure compute code
(no INTs, CLI/STI, or I/O) until it hits the stale code.

## The Stale Code Bug

### Symptom
V86 task loops at CS=0x0006:IP=0xB03E (linear 0xB09E).
Instructions at that address: `ES: CMP WORD [SI], 0` — polling loop.

### Register State at Breakpoint
```
EAX=0x00040000  EBX=0x0000000A  ECX=0x80000050  EDX=0x00000006
ESI=0x00000010  EDI=0x00000000  EBP=0x00191520
SS:ESP=0x22B5:0xFFF0  DS=0x22B5  ES=0x1500
```

### How the Extender Gets There

The extender's code at CS=0x2A01:IP=0x0EF6 (decoded from byte dump):
```asm
RET                     ; return from INT 15h subroutine
NOP
MOV [BX+1Eh], CS        ; save CS
XCHG AX, BX
MOV BP, SP
MOV AL, [BX+10BEh]      ; read dispatch table
ADD BX, BX              ; BX *= 4
ADD BX, BX
XOR CX, CX
LEA DX, [BX-20h]
CMP DX, 0098h           ; range check
JA +0Bh
TEST AL, AL
JZ +07h
MOV CX, 22B5h           ; load data segment
ADD BX, 0F6Ah           ; compute table offset
MOV ES, CX              ; ES = data segment
MOV CX, ES:[BX+2]       ; read saved handler SEGMENT
JCXZ +0Dh               ; if segment=0, skip
MOV AX, ES:[BX]          ; read saved handler OFFSET
MOV [BP+16h], CX        ; overwrite CS in IRET frame
MOV [BP+14h], AX        ; overwrite IP in IRET frame
POP DS; POP ES; POPA; IRET  ; return through modified frame
```

This is a V86 INT chain handler. The extender:
1. Hooks an interrupt vector
2. Saves the original handler in a table at DS:0F6A+
3. Dispatches: reads the saved handler from the table
4. Modifies the IRET frame on the stack to chain to it
5. IRETs → CPU loads CS:IP from the modified stack → CS=0x0006

### Why the Pointer is Not Found in Memory Scans

The far pointer `0x0006:0xB03E` was NOT found anywhere in conventional
memory via byte pattern scan. This suggests either:
1. The pointer is constructed at runtime from separate values
2. The table was populated by the extender's real-mode stub before
   our kernel took over (i.e., from the FreeDOS boot environment)
3. The extender's segment/PSP changes between runs (confirmed: DS was
   0x22B5 in one run and 0x49FB in another)

### Remaining Questions

1. **Which interrupt is being chained?** BX=0x000A at breakpoint time,
   but the dispatch code multiplies BX by 4 and adds 0x0F6A, so the
   index may not directly correspond to an INT number.

2. **Where does 0x0006:0xB03E come from?** This was the IVT value for
   some interrupt in the FreeDOS environment that existed before Pinecore
   loaded. The extender's stub code (which runs from the MZ header before
   our V86 monitor intercepts) saved it.

3. **Could the MZ loader stub be the issue?** DOOM.EXE has a real-mode
   stub that runs before the extender. This stub may read and save IVT
   entries before our V86 monitor is active. But our monitor IS active
   when DOOM loads (it's a child process of COMMAND.COM).

## Proposed Fix Strategy

### Option A: Sanitize IVT Before EXEC
Before loading any DOS program, scan the IVT for entries pointing to
low segments (< 0x0060) that contain stale FreeDOS code. Replace with
IRET stubs. Risk: may break programs that legitimately use those handlers.

### Option B: Make VCPI Path Actually Work
Instead of debugging the stale handler chain, implement proper VCPI
DE0Ch so the extender actually switches to PM and never chains the
stale handler. The extender only chains the stale handler because it's
still in V86 mode doing setup — if DE0Ch works, it skips this.

### Option C: Disable VCPI, Force DPMI
Remove VCPI detection. DOS/4GW will then use our DPMI host. This is
simpler but may not work for all extenders.

### Recommended: Option B
The proper fix is making VCPI DE0Ch work. The stale handler chain is
a symptom of the extender running too long in V86 mode. Once DE0Ch
transitions to PM, the extender is in its own protected-mode environment
and never touches the stale handlers again.

## Source References

- CWSDPMI vcpi.asm: (cwsdpmi: vcpi.asm:119-121) — `mov esi, _abs_client; mov ax, DE0Ch; int 67h`
- CWSDPMI mswitch.asm: (cwsdpmi: mswitch.asm:220-239) — PM→V86 switch via FAR CALL
- CWSDPMI paging.c:69-82: (cwsdpmi: paging.c:69) — `link_vcpi()` fills CLIENT structure
- CWSDPMI vcpi.h: (cwsdpmi: vcpi.h) — CLIENT typedef with CR3, GDTR, IDTR, LDT, TR, CS:EIP
- Ralf Brown's Interrupt List: INT 67h/AX=DE0Ch, Table 03665
- Ralf Brown's Interrupt List: INT 15h/AX=BF02h (DOS/16M installation check)

## Session 11b — Pure Kernel Breakthrough

### A20 Gate Bug
After fixing the stale handler issue with the pure kernel's clean IVT,
DOOM's extender was disabling the A20 address line via port 0x92 or the
keyboard controller. With A20 off, bit 20 of all addresses is forced to 0,
causing kernel code at 0x1090D6 to wrap to 0x0090D6. Page fault → double
fault → triple fault → CPU reset.

**Root cause**: V86 POPF emulation allowed IOPL bits (12-13) through to
real EFLAGS. The extender did PUSHF (got IOPL=3 virtual), then POPF
(wrote IOPL=3 to real EFLAGS). With IOPL=3 in real EFLAGS, IN/OUT
instructions in V86 mode execute directly (no GPF), bypassing our I/O
port filtering. The extender's `OUT 0x92, AL` with bit 1 clear disabled A20.

**Fix**: POPF/POPFD mask changed from `0x0FD5` to `0x0ED5` — excludes
IOPL (bits 12-13) and TF (bit 8). Real IOPL stays 0, all I/O traps.
Also added A20 protection filter on port 0x92 output.

### DOOM Enters Protected Mode via DPMI!
With the pure kernel (clean IVT, IOPL=0 enforced), DOOM's DOS/4GW
extender **chose DPMI** instead of VCPI:

```
INT 2Fh/1687h — DPMI detected by client
INT F1h AX=0001 — DPMI mode switch (32-bit)
DPMI: V86 → PM transition complete
  CS=0x0087 DS=0x008F EIP=0x6E6A 32-bit
```

It entered 32-bit protected mode through our DPMI host. Then page faulted
at CR2=0xC391737B — the extender is trying to access memory it hasn't
allocated yet via DPMI INT 31h/0501h.

**Next**: Handle the DPMI calls DOS/4GW makes after entering PM (memory
allocation, segment setup, interrupt vectors, real-mode simulation).

## Changes Made This Session

1. **Fixed DE0Ch**: Read structure from `frame->esi` instead of stack.
   Also properly dereference the GDTR pseudo-descriptor (6-byte pointer
   at the linear address) to get the actual GDT base.

2. **Fixed INT 15h/BFxxh**: Explicitly zero DX and SI for DOS/16M
   "not installed" response.

3. **Added IVT IRET stubs**: INT 10h, 11h, 12h, 15h — safe stubs at
   0x0510+ so FAR CALLs to "original" handlers don't hit stale BIOS.

4. **Added INT 3 (0xCC) handler**: V86 GPF handler now catches INT 3
   for breakpoint debugging.

5. **Added V86 IVT dump**: Full IVT dump function for debugging.

6. **POPF IOPL protection**: Mask changed to 0x0ED5 — IOPL stays 0 so
   all V86 I/O traps through GPF. Prevents extenders from bypassing
   I/O filtering.

7. **A20 gate protection**: Port 0x92 writes force bit 1 (A20) set.
   KBC port 0x60/0x64 A20 commands filtered. Prevents triple fault
   from A20 disable.

8. **MOV CRn/DRn emulation**: Added 0x0F 0x22 (MOV CR,reg) and
   0x0F 0x21/0x23 (MOV DR) handlers. CR writes ignored, DR reads
   return 0, DR writes ignored.

9. **Debug exception (INT 1) handling**: V86 INT 1 clears TF and
   resumes instead of crashing.

10. **Dual kernel build system**: `make all` builds both kernel.dos.bin
    and kernel.pure.bin. `make flat` updates all disk images.
    `make run-pure` and `make run-doom` use QEMU multiboot.
