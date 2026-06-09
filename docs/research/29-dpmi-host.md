# DPMI Host Implementation — Running DOOM on Pinecore

> DPMI (DOS Protected Mode Interface) 0.9 host implementation for running 32-bit DOS extender applications (DOS/4GW, CWSDPMI-hosted DJGPP programs) in protected mode on our Ring 0 kernel.

**Date:** 2026-05-02
**Status:** Complete — register-level implementation reference
**Sources:** DPMI 0.9 Specification (DPMI Committee); CWSDPMI r7 source code (cwsdpmi: exphdlr.c, control.c, gdt.h, tss.h, dpmisim.h)

---

## Architecture Overview

### What DPMI Is

DPMI provides a standardized interface for real-mode DOS programs to enter 32-bit protected mode. A **DPMI host** (like CWSDPMI, or our kernel) provides:
- Mode switching (real → protected → real)
- LDT descriptor management for the PM client
- Extended memory allocation
- Interrupt vector management (PM + RM)
- Real-mode simulation from PM (calling DOS/BIOS)

### How DOS Extender Games Work

```
DOOM.EXE (MZ header)
  └─ DOS/4GW stub (real-mode code)
       ├─ Detects DPMI host via INT 2Fh/1687h
       ├─ Calls DPMI entry point to enter PM
       ├─ Loads LE (Linear Executable) game code
       ├─ Sets up flat 32-bit segments via LDT
       └─ Jumps to 32-bit game code
            ├─ INT 31h/0501h → allocate memory
            ├─ INT 31h/0300h → call BIOS for video/keyboard
            ├─ INT 31h/0205h → hook timer/keyboard interrupts
            └─ INT 21h → file I/O (transparently translated)
```

### Pinecore Integration

Since we already have V86 mode, the DOS extender's real-mode stub runs in V86. When it detects DPMI (via INT 2Fh) and calls the entry point, our kernel:
1. Creates an LDT for the client
2. Sets up initial PM selectors (CS, DS, SS, ES = PSP)
3. Switches the V86 task to Ring 3 PM mode
4. Routes subsequent INT 31h calls to our DPMI handler
5. Translates INT 21h calls from PM to our DOS emulation

---

## DPMI Detection Protocol

### INT 2Fh/AX=1687h — Detect DPMI Host

The DOS extender calls this to check if DPMI is available:

**Input:** AX = 1687h
**Output:**
| Register | Value | Meaning |
|----------|-------|---------|
| AX | 0 = DPMI available | Non-zero = not available |
| BX | Flags | Bit 0: 32-bit programs supported |
| CL | CPU type | 3=386, 4=486, 5=Pentium |
| DH | DPMI major version | 0 for 0.9 |
| DL | DPMI minor version | 90 (0x5A) for 0.9 |
| SI | Paragraphs needed | Private data area size (in 16-byte paragraphs) |
| ES:DI | Entry point | Far pointer to DPMI mode switch entry |

(cwsdpmi: control.c → dpmiint2f, intercepted at INT 2Fh)

### Mode Switch Entry Point

The DOS extender allocates SI paragraphs of DOS memory, then calls ES:DI as a FAR procedure:

**Input to entry point:**
| Register | Value |
|----------|-------|
| AX | Flags: bit 0 = 1 for 32-bit client |
| ES | Real-mode segment of DPMI private data |

**On return (success):**
- Carry flag clear
- Client is now in protected mode
- CS = LDT code selector (base = client's real-mode CS * 16)
- DS = LDT data selector (base = client's real-mode DS * 16)
- SS = LDT stack selector (base = client's real-mode SS * 16)
- ES = LDT selector for PSP (base = PSP segment * 16)
- All selectors have 64KB limit, DPL = 3

(cwsdpmi: control.c:377-496, DPMIstartup → fills LDT entries l_acode, l_adata, l_apsp, l_aenv)

---

## LDT Management

### CWSDPMI's LDT Structure

- 128 LDT entries (l_num = 128 in gdt.h)
- Entries 0-15 reserved for system use
- Entries 16+ (l_free = 16) available for client allocation
- Initial client selectors: l_acode=16, l_adata=17, l_apsp=18, l_aenv=19
- Each descriptor is 8 bytes (standard x86 segment descriptor)
- LDT itself is a GDT entry (g_ldt = 15) with type = LDT (0x82)

(cwsdpmi: gdt.h:50-70)

### Descriptor Format (8 bytes)

```
Bytes 0-1: Limit 15:0
Byte 2:    Base 15:0 (low word)
Byte 3:    Base 23:16
Byte 4:    Access rights (stype): P, DPL, S, Type
Byte 5:    Limit 19:16 + flags (G, D/B, L, AVL)
Byte 6:    Base 31:24
Byte 7:    (part of base)
```

### Key DPMI Descriptor Functions

**0000h — Allocate LDT Descriptors**
- Input: CX = number of descriptors
- Output: AX = first selector (LDT index * 8 | 4 | DPL)
- CWSDPMI scans from l_free for CX consecutive free entries
- Initializes as data, read/write, present, base=0, limit=0, 32-bit
(cwsdpmi: exphdlr.c:464-483, alloc_ldt)

**0001h — Free LDT Descriptor**
- Input: BX = selector
- Clears stype to 0 (marks free)
- Also clears any segment registers containing this selector
(cwsdpmi: exphdlr.c:506-516, free_desc)

**0006h — Get Segment Base**
- Input: BX = selector
- Output: CX:DX = 32-bit base address
(cwsdpmi: exphdlr.c:627-634)

**0007h — Set Segment Base**
- Input: BX = selector, CX:DX = base address
(cwsdpmi: exphdlr.c:636-644)

**0008h — Set Segment Limit**
- Input: BX = selector, CX:DX = 32-bit limit
- If limit > 0xFFFFF, uses page granularity (G bit)
(cwsdpmi: exphdlr.c:646-661)

**0009h — Set Access Rights**
- Input: BX = selector, CX = access rights word
(cwsdpmi: exphdlr.c:663-670)

**000Ah — Create Code Segment Alias**
- Input: BX = code selector
- Output: AX = new data selector with same base/limit
(cwsdpmi: exphdlr.c:672-683)

---

## Memory Management

### 0501h — Allocate Memory Block

The most critical function for DOS extender games.

**Input:** BX:CX = size in bytes
**Output:** BX:CX = linear address, SI:DI = handle

CWSDPMI implementation (exphdlr.c:1177-1210):
- Maintains a linked list of AREAS (first_addr, last_addr)
- Virtual addresses start at 0x400000 (VADDR_START)
- Size is rounded up to page boundary (4KB)
- Pages are allocated on demand (page fault handler)
- For Ring 0 operation, pages are pre-allocated and locked

**For Pinecore:** We can simplify — allocate physical pages from PMM, identity-map them above 4MB (or in a per-task address space). Return the linear address directly.

### 0502h — Free Memory Block
- Input: SI:DI = handle (= linear address of block)
- Walks AREAS list, frees pages

### 0500h — Get Free Memory Information
- Output: ES:EDI → 48-byte buffer with free memory stats
- Key field at offset 0: largest available block in bytes

---

## Interrupt Management

### 0200h/0201h — Get/Set Real-Mode Interrupt Vector
- Read/write the IVT at 0000:(int_num * 4)
- For hardware interrupts, maps through PIC remapping
(cwsdpmi: exphdlr.c:898-914)

### 0204h/0205h — Get/Set Protected-Mode Interrupt Vector

**0205h is critical** — this is how games hook the timer and keyboard.

**Input:** BL = interrupt number, CX:EDX = selector:offset of PM handler

CWSDPMI's implementation for hardware interrupts (exphdlr.c:954-1018):
1. Maps interrupt number to hardware IRQ (0x08-0x0F → master, slave PIC)
2. Saves the original real-mode vector
3. Allocates a RMCB (Real-Mode CallBack) so real-mode interrupts get forwarded to PM
4. Updates the IDT entry for PM handling
5. When interrupt fires in PM: IDT → Ring 0 handler (tables.asm irq routines) → switch to locked stack → call user handler at Ring 3
6. When interrupt fires in RM: RMCB trampoline → switch to PM → call user handler

**For Pinecore:** Our IDT already handles hardware interrupts. We need to add per-DPMI-client interrupt handler registration. When the client hooks INT 8 (timer), we save its selector:offset and call it from our IRQ handler at Ring 3 DPL.

---

## Real-Mode Simulation (0300h-0304h)

### 0300h — Simulate Real-Mode Interrupt

**The most complex DPMI function.** PM code needs to call DOS/BIOS services.

**Input:**
- BL = interrupt number
- BH = flags (bit 0: reset IF)
- CX = number of words to copy from PM stack to RM stack
- ES:EDI → DPMI register structure (50 bytes)

**DPMI Register Structure (50 bytes at ES:EDI):**
```
Offset  Size  Register
0x00    4     EDI
0x04    4     ESI
0x08    4     EBP
0x0C    4     Reserved (ESP — ignored on input)
0x10    4     EBX
0x14    4     EDX
0x18    4     ECX
0x1C    4     EAX
0x20    2     Flags
0x22    2     ES
0x24    2     DS
0x26    2     FS
0x28    2     GS
0x2A    2     IP (ignored for 0300h — set from IVT)
0x2C    2     CS (ignored for 0300h — set from IVT)
0x2E    2     SP (0 = use DPMI host's stack)
0x30    2     SS (0 = use DPMI host's stack)
```

**CWSDPMI implementation (exphdlr.c:1020-1097):**
1. Copy register structure from client memory (memget via ES:EDI)
2. If SP:SS = 0, provide a temporary real-mode stack (128 words)
3. Copy CX words from PM stack to RM stack (for parameter passing)
4. Clear TF/IF in flags, set required bits
5. For 0300h: read CS:IP from IVT for the requested interrupt
6. Call `dpmisim()` — switches to real mode, sets all registers, executes the interrupt, returns
7. Copy modified register structure back to client memory

**For Pinecore:** Since we're already Ring 0 with V86 mode, we can:
- Read the register structure from client memory
- Set up a V86 frame with those registers  
- Execute the interrupt in V86 mode
- Copy the results back

OR simpler: since our DOS/BIOS emulation already handles INT 21h/10h/16h in the V86 GPF handler, we can just route 0300h directly to our existing handlers. No need to actually enter V86 — just call dos_int21() or the BIOS emulation directly with the provided registers.

### 0303h — Allocate Real-Mode Callback

Provides a real-mode FAR address that, when called, switches to PM and calls a handler.

**Input:** DS:ESI = PM handler address, ES:EDI → register structure buffer
**Output:** CX:DX = real-mode callback address

Used by games for:
- Mouse callbacks (INT 33h handler calls PM code)
- Timer callbacks

CWSDPMI uses a table of RMCB structures (dpmisim.h, 24 slots):
```c
typedef struct {
    uint32_t cb_address;   // PM handler offset
    uint16_t cb_sel;       // PM handler selector (0 = free)
    uint8_t  cb_type;      // 0 = DPMI callback, 1 = HW interrupt
    uint32_t reg_ptr;      // register structure offset
    uint16_t reg_sel;      // register structure selector
} dpmisim_rmcb_struct;
```

---

## Initial PM Environment Setup

When the DOS extender calls the DPMI entry point, Pinecore must:

### Step 1: Create LDT
- Allocate an LDT (128 entries * 8 bytes = 1024 bytes)
- Add LDT descriptor to GDT (type = 0x82, LDT system segment)
- Load LDTR with the GDT selector

### Step 2: Create Initial Selectors
```
LDT[16] = code:  base = CS_real * 16, limit = 0xFFFF, DPL=3, exec/read
LDT[17] = data:  base = DS_real * 16, limit = 0xFFFF, DPL=3, read/write
LDT[18] = PSP:   base = PSP_seg * 16, limit = 0xFFFF, DPL=3, read/write
LDT[19] = env:   base = ENV_seg * 16, limit = 0xFFFF, DPL=3, read/write
```
(cwsdpmi: control.c:469-493)

### Step 3: Set Client Register State
```
CS  = LDT_SEL(16)    // (16*8) | 4 | 3 = 0x87
DS  = LDT_SEL(17)    // 0x8F
SS  = LDT_SEL(stack) // may be same as DS or separate
ES  = LDT_SEL(18)    // PSP selector
EFLAGS = 0x3202       // IF=1, IOPL=3, bit1=1
EIP = from mode switch entry (client's original IP)
ESP = from mode switch entry (client's original SP)
```

### Step 4: Switch from V86 to Ring 3 PM
- Modify the V86 frame: clear VM bit, set segment registers to LDT selectors
- On IRET, CPU enters Ring 3 PM instead of V86

---

## INT 21h Translation from PM

When a PM client executes INT 21h, the CPU traps via the IDT (interrupt gate, DPL=3). Our Ring 0 handler receives:
- All registers from the PM client
- DS, ES are LDT selectors — need to resolve to linear addresses

**Key issue:** PM client's DS:DX might point to a string, but DS is an LDT selector, not a segment. We must:
1. Look up the LDT entry for DS
2. Compute linear address = LDT_base + DX
3. Pass this to our dos_int21() which expects linear (V86) addresses

For most INT 21h calls, this is straightforward because the first 1MB is identity-mapped.

DOS/4GW typically sets up selectors with base=0 for a flat model, so DS:offset = linear address directly.

---

## What DOOM Actually Calls

Based on DOS/4GW behavior, the critical DPMI functions are:

| Function | Purpose | Priority |
|----------|---------|----------|
| INT 2Fh/1687h | Detect DPMI | Must have |
| Mode switch | Enter PM | Must have |
| 0000h | Allocate LDT descriptors | Must have |
| 0001h | Free LDT descriptor | Must have |
| 0003h | Get selector increment (=8) | Trivial |
| 0006h | Get segment base | Must have |
| 0007h | Set segment base | Must have |
| 0008h | Set segment limit | Must have |
| 0009h | Set access rights | Must have |
| 000Ah | Create code alias | Should have |
| 0100h | Allocate DOS memory | Must have |
| 0101h | Free DOS memory | Must have |
| 0200h | Get RM interrupt vector | Must have |
| 0201h | Set RM interrupt vector | Should have |
| 0204h | Get PM interrupt vector | Must have |
| 0205h | Set PM interrupt vector | Must have |
| 0300h | Simulate RM interrupt | Must have |
| 0303h | Allocate RM callback | Should have |
| 0400h | Get DPMI version | Trivial |
| 0500h | Get free memory info | Should have |
| 0501h | Allocate memory block | Must have |
| 0502h | Free memory block | Must have |
| 0600h | Lock linear region | Trivial (no-op) |
| 0604h | Get page size (=4096) | Trivial |
| 0800h | Physical address mapping | Must have (for VGA/VESA LFB) |
| 0900h-0902h | Virtual interrupt state | Should have |

---

## Implementation Plan for Pinecore

### New files needed:
- `src/kernel/dpmi.c` — INT 31h handler, LDT management, memory blocks
- `src/include/dpmi.h` — DPMI structures and API
- `src/kernel/leload.c` — LE (Linear Executable) loader (optional — DOS/4GW loads it itself)

### Architecture:
1. **V86 task runs DOS extender stub** (existing infrastructure)
2. **INT 2Fh/1687h in V86** → our v86.c recognizes it, returns DPMI info
3. **Mode switch call** → kernel creates LDT, switches V86 task to Ring 3 PM
4. **INT 31h in PM** → IDT gate traps to Ring 0 → our dpmi.c handles it
5. **INT 21h in PM** → IDT gate → translate selectors → existing dos_int21()
6. **Hardware IRQs** → IDT → if client hooked it, call at Ring 3; else reflect

### Estimated size: ~2000 lines of C + ~200 lines of ASM
### Estimated effort: 2-3 sessions

---

## Key Insights from CWSDPMI Source

1. **LDT is simple** — just an array of 128 descriptors. alloc_ldt() scans linearly for free entries. (cwsdpmi: exphdlr.c:464)

2. **Memory blocks are virtual** — CWSDPMI uses page-fault-driven allocation. We can simplify: pre-allocate physical pages since we have 32MB. (cwsdpmi: exphdlr.c:1177)

3. **Real-mode simulation (0300h) is the hardest part** — requires saving PM state, switching to RM, executing interrupt, returning. For us: we can call our existing DOS/BIOS emulation directly without actually switching modes. (cwsdpmi: exphdlr.c:1020)

4. **Hardware interrupt forwarding** — CWSDPMI uses RMCBs (real-mode callbacks) to forward RM interrupts to PM handlers. We don't need this complexity because our kernel handles all interrupts at Ring 0 already. We just need to call the client's PM handler. (cwsdpmi: exphdlr.c:954-1018)

5. **The locked stack** — CWSDPMI maintains a separate 4KB stack for interrupt handling because the user stack may be invalid. We have per-task kernel stacks already. (cwsdpmi: exphdlr.c:106)

6. **Nesting** — CWSDPMI supports nested DPMI clients (saving/restoring TSS and LDT state). For v1.0, we can skip this. (cwsdpmi: control.c:387-396)

---

*Primary sources: DPMI Specification Version 0.9 (DPMI Committee, Intel); CWSDPMI r7 source code (CW Sandmann); ATA/ATAPI-6 (T13); DOS/4GW documentation*
