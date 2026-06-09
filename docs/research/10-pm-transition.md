# Protected Mode Transition — How to Take Over from FreeDOS

> The exact sequence to go from a FreeDOS program to Ring 0 with paging, IDT, and TSS.

**Date:** 2026-04-28
**Status:** Complete — based on CWSDPMI source and 386 bible

---

## Findings

### Two Paths: Real Mode vs V86 Mode

When our program starts under FreeDOS, the CPU is in one of two states:

**Real Mode (no EMM386 loaded):**
- We're in actual real mode, can transition directly to PM
- Simpler path — CLI, GDT, PE bit, JMP

**V86 Mode (EMM386 loaded):**
- CPU is already in PM, we're running in a V86 task under EMM386
- Must use VCPI (INT 67h) to get to "real" protected mode
- EMM386 mediates the transition

### Detection

CWSDPMI detects this with a simple SMSW instruction (cwsdpmi: mswitch.asm:450-454):
```asm
SMSW AX        ; Store Machine Status Word
AND AX, 1      ; Test PE bit
; Returns 0 = real mode, 1 = V86 mode
```

Then checks for VCPI availability (cwsdpmi: vcpi.asm:73-83):
```asm
INT 67h AX=0xDE00  ; VCPI_PRESENT
; AH=0 means VCPI is available
```

---

## Path 1: Direct Transition (Real Mode, No EMM386)

Based on cwsdpmi: mswitch.asm:125-201 and 386-bible p.174-186.

### Step 1: Prepare (still in real mode)

Before calling the switch routine:
- Allocate memory for GDT, IDT, page directory, page tables in conventional memory
- Fill GDT entries (see GDT Layout below)
- Fill IDT entries (256 interrupt gates)
- Set up at least one TSS
- Set up page directory and page tables (identity-map first 4MB minimum)

### Step 2: Enable A20

The A20 address line must be enabled for addresses above 1MB:
```asm
call _set_a20    ; (cwsdpmi: mswitch.asm:126)
```
Methods: keyboard controller (port 0x64/0x60), fast A20 (port 0x92), or BIOS INT 15h/AX=2401h.

### Step 3: Load GDT and IDT

```asm
LGDT fword ptr gdt_phys    ; 6-byte pointer: [2-byte limit][4-byte base]
LIDT fword ptr idt_phys    ; Same format
```
(cwsdpmi: mswitch.asm:130-131)

### Step 4: Set PE Bit

```asm
CLI                    ; Interrupts MUST be disabled
MOV EAX, CR0
OR AL, 1               ; Set PE (Protection Enable) bit
MOV CR0, EAX           ; NOW IN PROTECTED MODE
```
(cwsdpmi: mswitch.asm:133-136, 386-bible p.176)

### Step 5: Far JMP to Flush Prefetch

```asm
DB 0EAh                          ; Far JMP opcode
DW offset go_protect_far_jump    ; EIP
DW g_rcode                       ; CS selector (Ring 0 code)
```
(cwsdpmi: mswitch.asm:137-139, 386-bible p.176)

This JMP is critical — the CPU has prefetched instructions in real-mode format, they must be discarded.

### Step 6: Load Segment Registers

```asm
go_protect_far_jump:
    CLI
    MOV AX, g_rdata      ; Flat 4GB data selector
    MOV DS, AX
    MOV ES, AX
    MOV FS, AX
    MOV GS, AX
    MOV SS, AX
```
(cwsdpmi: mswitch.asm:143-150)

### Step 7: Enable Paging (if desired)

```asm
MOV EAX, page_directory_physical_address
MOV CR3, EAX            ; Set Page Directory Base Register

MOV EAX, CR0
OR EAX, 0x80000000      ; Set PG bit (bit 31)
MOV CR0, EAX

; Far JMP to flush TLB
DB 0EAh
DW offset paging_enabled
DW g_rcode
```
(cwsdpmi: mswitch.asm:186-196, 386-bible p.177)

### Step 8: Load Task Register

```asm
MOV AX, g_ctss           ; TSS selector
LTR AX                   ; Load Task Register
```
(cwsdpmi: mswitch.asm:199)

### Step 9: Jump to Kernel Entry

```asm
JMPT g_atss              ; Task gate jump — loads full context from TSS
```
(cwsdpmi: mswitch.asm:204)

Or simpler: just `JMP` to our 32-bit kernel entry point.

---

## Path 2: VCPI Transition (V86 Mode, EMM386 Loaded)

Based on cwsdpmi: mswitch.asm:117-121 and vcpi.h.

### The CLIENT Structure

VCPI requires a CLIENT structure that tells EMM386 where our PM environment is (cwsdpmi: vcpi.h:5-13):

```c
typedef struct {
    word32 page_table;      // Page Directory physical address (for CR3)
    word32 gdt_address;     // GDT physical address
    word32 idt_address;     // IDT physical address
    word16 ldt_selector;    // LDT selector in GDT
    word16 tss_selector;    // TSS selector in GDT
    word32 entry_eip;       // Protected mode entry point
    word16 entry_cs;        // Entry code selector
} CLIENT;
```

### The Switch

```asm
MOV ESI, abs_client          ; ESI = physical address of CLIENT structure
MOV AX, 0xDE0C               ; VCPI_MODE_CHANGE
INT 67h                       ; Call VCPI host (EMM386)
; NEVER RETURNS — execution continues at entry_cs:entry_eip
```
(cwsdpmi: mswitch.asm:119-121)

EMM386 internally:
1. Validates CLIENT structure
2. Loads CR3 from CLIENT.page_table
3. Switches from V86 to real protected mode using our GDT/IDT
4. Jumps to CLIENT.entry_cs:CLIENT.entry_eip
5. We're now Ring 0 with our own GDT, IDT, paging, and TSS

### After VCPI Transition

Same post-transition code as Path 1 from Step 6 onwards (load segment registers, etc.)

---

## GDT Layout

Minimum GDT entries for our kernel (based on cwsdpmi: gdt.h and 386-bible p.177):

| Index | Selector | Name | Base | Limit | Type | DPL | Notes |
|-------|----------|------|------|-------|------|-----|-------|
| 0 | 0x00 | Null | — | — | — | — | Required null descriptor |
| 1 | 0x08 | Kernel Code | 0x0 | 4GB | Code, Execute/Read | 0 | 32-bit, Ring 0 |
| 2 | 0x10 | Kernel Data | 0x0 | 4GB | Data, Read/Write | 0 | 32-bit, Ring 0 |
| 3 | 0x18 | User Code | 0x0 | 4GB | Code, Execute/Read | 3 | 32-bit, Ring 3 (for future) |
| 4 | 0x20 | User Data | 0x0 | 4GB | Data, Read/Write | 3 | 32-bit, Ring 3 (for future) |
| 5 | 0x28 | Kernel TSS | tss_addr | 104+ | TSS, Available | 0 | 32-bit TSS |
| 6 | 0x30 | Real Mode Code | CS*16 | 64KB | Code, Execute/Read | 0 | 16-bit, for transition |
| 7 | 0x38 | Real Mode Data | DS*16 | 64KB | Data, Read/Write | 0 | 16-bit, for transition |

Descriptor format (386-bible p.94, cwsdpmi: gdt.inc:13-20):
```
Bytes 0-1: Limit[15:0]
Bytes 2-3: Base[15:0]
Byte 4:    Base[23:16]
Byte 5:    Access (P=1, DPL, S, Type)
Byte 6:    Limit[19:16] | Flags (G=1 for 4KB granularity, D/B=1 for 32-bit)
Byte 7:    Base[31:24]
```

---

## What Happens to FreeDOS

After we switch to PM:
- FreeDOS kernel is still in conventional memory (first 640KB)
- Its interrupt vectors are still in the IVT (address 0x0000-0x03FF)
- But we've loaded our own IDT — FreeDOS interrupt handlers are no longer called
- FreeDOS is effectively dormant — its code is still there but never executes
- We own the CPU completely

If we need BIOS services (e.g., VESA mode setting during init), we can:
1. Create a temporary V86 task
2. Call the BIOS interrupt via our V86 monitor
3. Return to PM
4. This is how Windows 98 called BIOS during startup

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| CWSDPMI mswitch.asm | cwsdpmi-master/src/mswitch.asm:85-204 | Complete PM transition code |
| CWSDPMI vcpi.asm | cwsdpmi-master/src/vcpi.asm:73-109 | VCPI detection and interface |
| CWSDPMI vcpi.h | cwsdpmi-master/src/vcpi.h:5-13 | CLIENT structure |
| CWSDPMI control.c | cwsdpmi-master/src/control.c:325-496 | Startup, GDT/IDT/TSS setup |
| CWSDPMI gdt.inc | cwsdpmi-master/src/gdt.inc:13-20 | GDT entry layout |
| 386 Bible Ch.10 | i386-bible/pages/page_0174-0186 | Init sequence |
| 386 Bible Ch.5 | i386-bible/pages/page_0091-0105 | Paging setup |

---

*Last updated: 2026-04-28*
