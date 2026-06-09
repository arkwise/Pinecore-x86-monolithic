# DPMI 0.9 Specification — Complete Function Reference for Host Implementation

> Full INT 31h API reference, mode switch protocol, and implementation details
> for building a DPMI 0.9 host inside the Pinecore bare-metal i386 kernel.

**Date:** 2026-05-02
**Status:** Active reference — implementation guide
**Source:** DPMI Specification Version 0.9, DPMI Committee (Intel, 1990);
CWSDPMI r7 source (cwsdpmi: exphdlr.c); Ralf Brown's Interrupt List

---

## 1. Overview

DPMI 0.9 defines the interface between a protected-mode client (e.g., a
DOS-extended application) and a DPMI host (the software that manages PM
resources). The host provides:

- Protected-mode entry/exit
- LDT management for client selectors
- Memory allocation with linear addresses
- Interrupt reflection between real mode and protected mode
- Real-mode simulation (calling DOS/BIOS from PM)
- Real-mode callbacks (PM code called from RM)

**Our situation:** We ARE the kernel. We run at Ring 0. We will implement the
DPMI host as a set of INT 31h handlers that manage per-client state. The
"client" is a DOS-extended program (like DOOM via DOS/4GW) running at Ring 3.

---

## 2. Mode Switch Protocol (INT 2Fh / AX=1687h)

### 2.1 DPMI Detection

A real-mode program detects DPMI by calling:

```
INT 2Fh
  AX = 1687h

Returns:
  AX = 0000h if DPMI host present (nonzero = not present)
  BX = flags
       bit 0 = 1: 32-bit programs supported
       bit 1-15: reserved
  CL = processor type (03h = 386, 04h = 486, 05h = 586)
  DH = DPMI major version (00h for 0.9)
  DL = DPMI minor version (5Ah for 0.9 = 90 decimal)
  SI = number of paragraphs of real-mode memory needed for
       host private data (client must allocate this before mode switch)
  ES:DI = real-mode FAR entry point for mode switch
```

### 2.2 Mode Switch Entry

The client allocates SI paragraphs of real-mode memory, then calls the mode
switch entry point:

```
Call FAR [ES:DI from above]

Input:
  AX = flags
       bit 0 = 0: 16-bit client
       bit 0 = 1: 32-bit client
  ES = real-mode segment of allocated private data (SI paragraphs)

Returns (in protected mode):
  CS = selector for client's code segment (base = real-mode CS << 4,
       limit = 64KB for 16-bit, 4GB for 32-bit)
  DS = selector for client's data segment (base = real-mode DS << 4)
  SS = selector for client's stack (base = real-mode SS << 4)
  ES = selector for client's PSP (base = PSP segment << 4, limit = 100h)
  FS = 0 (null selector)
  GS = 0 (null selector)
  Carry flag clear on success
  Carry flag set on failure
```

### 2.3 What the Host Must Set Up During Mode Switch

1. **Allocate an LDT** (or per-client LDT entries). DPMI 0.9 requires at
   least 8192 LDT entries available to the client (selectors 0004h-FFFFh
   at 8-byte granularity, so up to 8192 descriptors).

2. **Create initial selectors:**
   - CS: code segment descriptor, base = client's real-mode CS << 4
     - For 32-bit: D bit set, limit = 4GB (page-granular 0FFFFFh)
     - For 16-bit: D bit clear, limit = 64KB
   - DS: data segment descriptor, base = client's RM DS << 4
   - SS: stack segment descriptor, base = client's RM SS << 4
     - ESP preserved from real-mode SP (zero-extended for 32-bit)
   - ES: PSP descriptor, base = PSP segment << 4, limit = 0FFh

3. **Set up the IDT** to trap INT 31h (DPMI services), INT 21h (DOS
   reflection), and hardware interrupts for reflection.

4. **Switch to protected mode** — load GDT, IDT, LDT, set PE bit (already
   set in our kernel), load task selectors.

5. **Return to client** with the selectors above loaded and carry clear.

---

## 3. Complete INT 31h Function Reference

All functions are called via `INT 31h` with the function number in AX.
On success, carry flag is clear. On error, carry flag is set and AX contains
the error code.

### Standard DPMI Error Codes

| Code | Meaning |
|------|---------|
| 8001h | Unsupported function |
| 8002h | Invalid state (wrong object state for operation) |
| 8003h | System integrity (would crash if allowed) |
| 8004h | Deadlock detected |
| 8005h | Request cancelled |
| 8010h | Resource unavailable |
| 8011h | Descriptor unavailable |
| 8012h | Linear memory unavailable |
| 8013h | Physical memory unavailable |
| 8014h | Backing store unavailable |
| 8015h | Callback unavailable |
| 8016h | Handle unavailable |
| 8017h | Lock count exceeded |
| 8018h | Resource owned exclusively |
| 8019h | Resource owned shared |
| 8021h | Invalid value (parameter out of range) |
| 8022h | Invalid selector |
| 8023h | Invalid handle |
| 8024h | Invalid callback |
| 8025h | Invalid linear address |

---

### 3.1 LDT Descriptor Management (0000h-000Ch)

#### INT 31h / AX=0000h — Allocate LDT Descriptors

```
Input:
  AX = 0000h
  CX = number of descriptors to allocate

Output (success):
  CF clear
  AX = base selector (first of CX contiguous descriptors)

Output (error):
  CF set
  AX = 8011h (descriptor unavailable)

Notes:
  - Allocated descriptors are initialized to: present, data, DPL=3,
    base=0, limit=0, byte-granular, expand-up, writable
  - Selectors are spaced 8 apart (AX, AX+8, AX+16, ...)
  - (cwsdpmi: exphdlr.c — ldt_alloc())
```

#### INT 31h / AX=0001h — Free LDT Descriptor

```
Input:
  AX = 0001h
  BX = selector to free

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8022h (invalid selector)

Notes:
  - Must not free CS, DS, SS, or ES of the current context
  - The descriptor is zeroed (marked not present)
  - (cwsdpmi: exphdlr.c — ldt_free())
```

#### INT 31h / AX=0002h — Segment to Descriptor

```
Input:
  AX = 0002h
  BX = real-mode segment value

Output (success):
  CF clear
  AX = selector mapped to the real-mode segment

Output (error):
  CF set
  AX = 8011h (descriptor unavailable)

Notes:
  - Creates a descriptor with base = BX << 4, limit = 64KB-1
  - Multiple calls with the same segment may return the same selector
  - Used to access real-mode data from protected mode (e.g., BIOS data area)
  - The returned selector must NOT be modified or freed by the client
```

#### INT 31h / AX=0003h — Get Selector Increment Value

```
Input:
  AX = 0003h

Output:
  CF clear (always succeeds)
  AX = selector increment value (always 8 on i386)

Notes:
  - Used by clients to compute selector values when they allocated
    multiple descriptors with function 0000h
```

#### INT 31h / AX=0006h — Get Segment Base Address

```
Input:
  AX = 0006h
  BX = selector

Output (success):
  CF clear
  CX:DX = 32-bit linear base address (CX = high 16, DX = low 16)

Output (error):
  CF set
  AX = 8022h (invalid selector)
```

#### INT 31h / AX=0007h — Set Segment Base Address

```
Input:
  AX = 0007h
  BX = selector
  CX:DX = 32-bit linear base address (CX = high 16, DX = low 16)

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8022h (invalid selector)
  AX = 8025h (invalid linear address)

Notes:
  - Critical function — DOS/4GW uses this to set up flat model
  - Only modifies descriptors the client owns (LDT, DPL=3)
  - (cwsdpmi: exphdlr.c — ldt_set_base())
```

#### INT 31h / AX=0008h — Set Segment Limit

```
Input:
  AX = 0008h
  BX = selector
  CX:DX = 32-bit segment limit (CX = high 16, DX = low 16)

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8022h (invalid selector)
  AX = 8021h (invalid value — limit > 1MB requires page granularity,
              i.e., low 12 bits must be FFFh)

Notes:
  - If limit > 0FFFFFh (1MB), the host must use page granularity
    (G bit set, limit stored as limit >> 12)
  - DOS/4GW sets limit = 0FFFFFFFFh for flat model segments
  - (cwsdpmi: exphdlr.c — ldt_set_limit())
```

#### INT 31h / AX=0009h — Set Descriptor Access Rights

```
Input:
  AX = 0009h
  BX = selector
  CL = access rights byte (byte 5 of descriptor)
       bit 7: present
       bit 6-5: DPL (must be 3 for DPMI client)
       bit 4: 1=code/data, 0=system
       bit 3: 1=code, 0=data
       bit 2: code: conforming; data: expand-down
       bit 1: code: readable; data: writable
       bit 0: accessed
  CH = extended type (high nibble of byte 6)
       bit 7: granularity (1=page, 0=byte)
       bit 6: default size (1=32-bit, 0=16-bit)
       bit 5: reserved (0)
       bit 4: available for OS

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8021h (invalid value)
  AX = 8022h (invalid selector)

Notes:
  - DPL in CL must be 3; host must reject DPL=0/1/2
  - Present bit must be 1 (host may reject not-present)
  - (cwsdpmi: exphdlr.c — ldt_set_access())
```

#### INT 31h / AX=000Ah — Create Alias Descriptor

```
Input:
  AX = 000Ah
  BX = selector (typically a code segment selector)

Output (success):
  CF clear
  AX = new data segment selector (alias of BX)

Output (error):
  CF set
  AX = 8011h (descriptor unavailable)
  AX = 8022h (invalid selector)

Notes:
  - Creates a new LDT data descriptor with same base and limit as BX
  - Used to write to code segments (code descriptors are not writable)
  - The alias is independent — changing one does NOT change the other
```

#### INT 31h / AX=000Bh — Get Descriptor

```
Input:
  AX = 000Bh
  BX = selector
  ES:EDI = pointer to 8-byte buffer

Output (success):
  CF clear
  Buffer at ES:EDI filled with the raw 8-byte descriptor

Output (error):
  CF set
  AX = 8022h (invalid selector)
```

#### INT 31h / AX=000Ch — Set Descriptor

```
Input:
  AX = 000Ch
  BX = selector
  ES:EDI = pointer to 8-byte descriptor to copy in

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8021h (invalid value)
  AX = 8022h (invalid selector)

Notes:
  - Host must validate: DPL must be 3, must be LDT selector
  - Must not be system descriptor (TSS, gate, etc.)
```

---

### 3.2 DOS Memory Management (0100h-0102h)

These functions manage conventional memory (below 1MB) via the DOS INT 21h
memory allocator.

#### INT 31h / AX=0100h — Allocate DOS Memory Block

```
Input:
  AX = 0100h
  BX = number of paragraphs (16-byte blocks) to allocate

Output (success):
  CF clear
  AX = real-mode segment of allocated block
  DX = selector for accessing the block from PM

Output (error):
  CF set
  AX = DOS error code (07h = MCB destroyed, 08h = insufficient memory)
  BX = largest available block in paragraphs

Notes:
  - Host calls INT 21h/48h in real mode on behalf of the client
  - The selector in DX has base = AX << 4, limit = BX * 16 - 1
  - If BX > 64KB/16 = 4096 paragraphs, multiple contiguous selectors
    are allocated (each covering 64KB)
  - Essential for allocating DMA buffers, real-mode data structures
```

#### INT 31h / AX=0101h — Free DOS Memory Block

```
Input:
  AX = 0101h
  DX = selector of block to free (returned by 0100h)

Output (success):
  CF clear

Output (error):
  CF set
  AX = DOS error code (07h = MCB destroyed, 09h = invalid block)
```

#### INT 31h / AX=0102h — Resize DOS Memory Block

```
Input:
  AX = 0102h
  BX = new size in paragraphs
  DX = selector of block

Output (success):
  CF clear

Output (error):
  CF set
  AX = DOS error code
  BX = maximum available paragraphs
```

---

### 3.3 Interrupt Management (0200h-0205h)

#### INT 31h / AX=0200h — Get Real Mode Interrupt Vector

```
Input:
  AX = 0200h
  BL = interrupt number

Output:
  CF clear (always succeeds)
  CX:DX = segment:offset of real-mode interrupt handler
```

#### INT 31h / AX=0201h — Set Real Mode Interrupt Vector

```
Input:
  AX = 0201h
  BL = interrupt number
  CX:DX = segment:offset of new real-mode handler

Output:
  CF clear (always succeeds)

Notes:
  - Modifies the real-mode IVT (first 1KB of memory)
  - The handler runs in real mode
```

#### INT 31h / AX=0202h — Get Processor Exception Handler Vector

```
Input:
  AX = 0202h
  BL = exception number (00h-1Fh)

Output (success):
  CF clear
  CX:EDX = selector:offset of exception handler

Output (error):
  CF set
  AX = 8021h (invalid value — BL > 1Fh)
```

#### INT 31h / AX=0203h — Set Processor Exception Handler Vector

```
Input:
  AX = 0203h
  BL = exception number (00h-1Fh)
  CX:EDX = selector:offset of new exception handler

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8021h (invalid value)
  AX = 8022h (invalid selector)

Notes:
  - Exception handlers run at the client's privilege level (Ring 3)
  - Handler must return via RETF (far return), NOT IRET
  - Host restores state and performs IRET internally
  - Frame format (32-bit vs 16-bit) depends on the CLIENT's bit-ness
    declared at mode switch, NOT the handler's code segment D/B bit
  - (cwsdpmi: exphdlr.c:313-320 — builds the frame)
  - (cwsdpmi: dpmisim.asm:404 — user_exception_return cleanup)

Exception Stack Frame (32-bit client):
  (DPMI 0.9 spec section 4.5; cwsdpmi: exphdlr.c:313-320)

  ESP+00h:  Return EIP    ← host return address (DO NOT modify)
  ESP+04h:  Return CS     ← host code selector  (DO NOT modify)
  ESP+08h:  Error Code    ← valid for exc 08h, 0Ah-0Eh only
  ESP+0Ch:  Faulting EIP  ← handler may modify to redirect execution
  ESP+10h:  Faulting CS   ← handler may modify
  ESP+14h:  EFLAGS        ← handler may modify
  ESP+18h:  Faulting ESP  ← original stack pointer
  ESP+1Ch:  Faulting SS   ← original stack segment

  Total: 32 bytes (8 dwords). Handler does RETF to pop Return CS:EIP.
  Host reads modified frame, restores state, performs IRETD internally.

  CWSDPMI detail: Return CS = GDT_SEL(g_pcode), a DPL=3 GDT code
  segment containing user_exception_return (dpmisim.asm:404), which
  does "add esp,4" (skip error code) then builds IRET frame on the
  user's original SS:ESP and does IRETD.
```

#### INT 31h / AX=0204h — Get Protected Mode Interrupt Vector

```
Input:
  AX = 0204h
  BL = interrupt number (00h-FFh)

Output:
  CF clear (always succeeds)
  CX:EDX = selector:offset of PM interrupt handler

Notes:
  - Returns the current handler that will be called when this
    interrupt fires in protected mode
```

#### INT 31h / AX=0205h — Set Protected Mode Interrupt Vector

```
Input:
  AX = 0205h
  BL = interrupt number (00h-FFh)
  CX:EDX = selector:offset of new PM interrupt handler

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8022h (invalid selector)

Notes:
  - This is how Allegro/DOS/4GW hooks hardware interrupts
  - Handler is called via an interrupt gate (IF cleared on entry)
  - Handler must issue IRET to return
  - For hardware IRQs: handler should call original handler (chain)
    or send EOI to PIC
  - (cwsdpmi: exphdlr.c — sets user_interrupt_handler[])
```

---

### 3.4 Translation Services (0300h-0304h)

#### INT 31h / AX=0300h — Simulate Real Mode Interrupt

```
Input:
  AX = 0300h
  BL = interrupt number
  BH = flags (bit 0: reset IF on real-mode IRET frame; bits 1-7 must be 0)
  CX = number of words to copy from PM stack to RM stack (0 = none)
  ES:EDI = pointer to DPMI_REGS structure (see below)

Output (success):
  CF clear
  ES:EDI = DPMI_REGS structure updated with values after RM interrupt returned

Output (error):
  CF set
  AX = 8012h (linear memory unavailable)
  AX = 8013h (physical memory unavailable)
  AX = 8014h (backing store unavailable)

DPMI Real Mode Register Structure (DPMI_REGS) — 50 bytes:
  Offset  Size  Register
  00h     4     EDI
  04h     4     ESI
  08h     4     EBP
  0Ch     4     reserved (0)
  10h     4     EBX
  14h     4     EDX
  18h     4     ECX
  1Ch     4     EAX
  20h     2     FLAGS
  22h     2     ES
  24h     2     DS
  26h     2     FS
  28h     2     GS
  2Ah     2     IP (ignored for 0300h, host uses IVT)
  2Ch     2     CS (ignored for 0300h)
  2Eh     2     SP (0 = host provides stack)
  30h     2     SS (0 = host provides stack)

Notes:
  - If SS:SP = 0, the host provides a real-mode stack (at least 200h bytes)
  - CX words are pushed onto the RM stack before the INT is executed
  - This is THE critical function for DOS/BIOS access from PM
  - DOOM uses this for INT 21h (file I/O), INT 10h (video), INT 33h (mouse)
  - Host implementation:
    1. Save current PM state
    2. Load registers from DPMI_REGS structure
    3. Switch to real mode (or V86 mode)
    4. Execute INT BL
    5. Copy resulting registers back to DPMI_REGS
    6. Switch back to PM
    7. Return to client
  - (cwsdpmi: exphdlr.c:700+ — go_real_mode, execute, go32)
```

#### INT 31h / AX=0301h — Call Real Mode Far Procedure with IRET Frame

```
Input:
  AX = 0301h
  BH = flags (must be 0)
  CX = words to push on RM stack
  ES:EDI = pointer to DPMI_REGS

Output (success):
  CF clear
  DPMI_REGS updated

Notes:
  - CS:IP from DPMI_REGS specifies the far procedure to call
  - Host pushes FLAGS/CS/IP on RM stack (IRET frame)
  - Procedure must return via IRET
```

#### INT 31h / AX=0302h — Call Real Mode Far Procedure

```
Input:
  AX = 0302h
  BH = flags (must be 0)
  CX = words to push on RM stack
  ES:EDI = pointer to DPMI_REGS

Output (success):
  CF clear
  DPMI_REGS updated

Notes:
  - Like 0301h but pushes only CS:IP (RETF frame)
  - Procedure must return via RETF
```

#### INT 31h / AX=0303h — Allocate Real Mode Callback Address

```
Input:
  AX = 0303h
  DS:ESI = selector:offset of PM procedure to call
  ES:EDI = selector:offset of DPMI_REGS structure (locked, always accessible)

Output (success):
  CF clear
  CX:DX = real-mode segment:offset of callback entry point

Output (error):
  CF set
  AX = 8015h (callback unavailable)

Notes:
  - Returns a real-mode FAR address that, when called from real mode,
    triggers a switch to PM and calls the procedure at DS:ESI
  - On entry to the PM callback procedure:
    DS:ESI = selector:offset of real-mode SS:SP at time of call
    ES:EDI = selector:offset of the DPMI_REGS structure
    SS:ESP = host-provided locked PM stack
  - The PM procedure must fill DPMI_REGS with return values
  - The PM procedure returns via IRET (far return with flags)
  - The host then switches back to RM with the DPMI_REGS values loaded
  - Maximum: typically 16 callbacks available (CWSDPMI has 16)
  - Used for: mouse callbacks, DMA completion, any RM code that needs
    to notify a PM program
  - (cwsdpmi: exphdlr.c — allocate_callback(), callback_stubs[])

Callback Stub Structure (what the host places in conventional memory):
  The CX:DX address points to a small real-mode stub:
    PUSHF           ; save flags
    CLI             ; disable interrupts
    PUSH CS         ; push return address for host to capture
    CALL FAR host   ; switch to host/PM
    ... host does the mode switch, calls PM procedure,
    ... loads DPMI_REGS into real-mode registers
    IRET            ; return to real-mode caller

Implementation detail:
  The host needs ~16 of these stubs in conventional memory (below 1MB).
  Each stub is ~20 bytes. Each is associated with a (PM_proc, DPMI_REGS) pair.
```

#### INT 31h / AX=0304h — Free Real Mode Callback

```
Input:
  AX = 0304h
  CX:DX = real-mode callback address (from 0303h)

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8024h (invalid callback)
```

---

### 3.5 Memory Management (0500h-0503h)

#### INT 31h / AX=0500h — Get Free Memory Information

```
Input:
  AX = 0500h
  ES:EDI = pointer to 48-byte (30h) buffer

Output:
  CF clear (always succeeds)
  Buffer filled with DPMI Memory Information Structure:

  Offset  Size  Field
  00h     4     Largest available free block (bytes)
  04h     4     Maximum unlocked page allocation (pages)
  08h     4     Maximum locked page allocation (pages)
  0Ch     4     Linear address space size (pages)
  10h     4     Total unlocked pages
  14h     4     Total free pages
  18h     4     Total physical pages
  1Ch     4     Free linear address space (pages)
  20h     4     Swap file size (pages)
  24h-2Fh 12    Reserved (all FFFFFFFFh)

Notes:
  - Any field may be FFFFFFFFh if the host cannot determine the value
  - One page = 4096 bytes
  - DOS/4GW checks field at offset 00h to determine available memory
```

#### INT 31h / AX=0501h — Allocate Memory Block

```
Input:
  AX = 0501h
  BX:CX = size of block in bytes (BX = high 16, CX = low 16)

Output (success):
  CF clear
  BX:CX = linear address of allocated block (BX = high, CX = low)
  SI:DI = memory block handle (SI = high, DI = low)

Output (error):
  CF set
  AX = 8012h (linear memory unavailable)
  AX = 8013h (physical memory unavailable)
  AX = 8014h (backing store unavailable)

Notes:
  - THIS IS THE PRIMARY MEMORY ALLOCATION FUNCTION
  - Returns a linear address, NOT a selector — client must create
    a descriptor (0000h) and set its base (0007h) to use the memory
  - The handle is used for free (0502h) and resize (0503h)
  - DOS/4GW typically allocates one large block and manages it internally
  - DOOM's memory manager (z_zone.c) sits on top of this
  - For our kernel: we allocate physical pages, map them to a contiguous
    linear address range, and return that range
  - (cwsdpmi: exphdlr.c — memory_allocate())
```

#### INT 31h / AX=0502h — Free Memory Block

```
Input:
  AX = 0502h
  SI:DI = memory block handle (from 0501h)

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8023h (invalid handle)

Notes:
  - Does NOT free associated descriptors — client must do that separately
```

#### INT 31h / AX=0503h — Resize Memory Block

```
Input:
  AX = 0503h
  BX:CX = new size in bytes
  SI:DI = memory block handle

Output (success):
  CF clear
  BX:CX = new linear address (may have moved!)
  SI:DI = new handle (may have changed!)

Output (error):
  CF set
  AX = 8012h (linear memory unavailable)
  AX = 8013h (physical memory unavailable)
  AX = 8023h (invalid handle)

Notes:
  - Linear address and handle may both change on resize
  - Client must update any descriptors pointing to the old address
```

---

### 3.6 Page Locking (0600h-0604h)

#### INT 31h / AX=0600h — Lock Linear Region

```
Input:
  AX = 0600h
  BX:CX = starting linear address (BX = high, CX = low)
  SI:DI = size in bytes (SI = high, DI = low)

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8013h (physical memory unavailable)
  AX = 8017h (lock count exceeded)
  AX = 8025h (invalid linear address)

Notes:
  - Prevents the region from being paged to disk
  - Required for interrupt handlers and DMA buffers
  - Lock count is maintained — must unlock same number of times
  - For our kernel (no swap): this is a no-op, always succeed
```

#### INT 31h / AX=0601h — Unlock Linear Region

```
Input:
  AX = 0601h
  BX:CX = starting linear address
  SI:DI = size in bytes

Output (success):
  CF clear

Output (error):
  CF set
  AX = 8002h (invalid state — not locked)
  AX = 8025h (invalid linear address)
```

#### INT 31h / AX=0602h — Mark Real Mode Region as Pageable

```
Input:
  AX = 0602h
  BX:CX = starting linear address
  SI:DI = size in bytes

Output (success):
  CF clear

Output (error):
  CF set

Notes:
  - Tells host that conventional memory in this range can be paged
  - For our kernel: no-op
```

#### INT 31h / AX=0603h — Relock Real Mode Region

```
Input:
  AX = 0603h
  BX:CX = starting linear address
  SI:DI = size in bytes

Output (success):
  CF clear

Notes:
  - Reverses 0602h
  - For our kernel: no-op
```

#### INT 31h / AX=0604h — Get Page Size

```
Input:
  AX = 0604h

Output:
  CF clear
  BX:CX = page size in bytes (BX = high, CX = low)
          Always 00000000h:00001000h = 4096

Notes:
  - Always returns 4096 on i386
```

---

### 3.7 Physical Address Mapping (0800h)

#### INT 31h / AX=0800h — Physical Address Mapping

```
Input:
  AX = 0800h
  BX:CX = physical address (BX = high, CX = low)
  SI:DI = size in bytes (SI = high, DI = low)

Output (success):
  CF clear
  BX:CX = linear address that maps to the physical address

Output (error):
  CF set
  AX = 8003h (system integrity)
  AX = 8021h (invalid value)

Notes:
  - Maps device memory (video framebuffer, MMIO) into linear address space
  - Allegro uses this to map the VGA/SVGA framebuffer
  - DOOM uses this for VGA memory at physical A0000h
  - Must NOT be used for memory below 1MB (already mapped 1:1 typically)
  - Implementation: create page table entries mapping the physical range
    to an unused linear address range with PCD/PWT bits set (uncacheable
    for MMIO)
  - (cwsdpmi: exphdlr.c — physical_map())
```

---

### 3.8 Virtual Interrupt State (0900h-0902h)

These manage the virtual interrupt flag (VIF) for the client. The client
cannot directly modify IF (it's Ring 3), so DPMI virtualizes it.

#### INT 31h / AX=0900h — Get and Disable Virtual Interrupt State

```
Input:
  AX = 0900h

Output:
  CF clear
  AL = previous virtual interrupt state (0 = disabled, 1 = enabled)
  (Virtual interrupts are now DISABLED)

Notes:
  - Equivalent of CLI for the DPMI client
  - Does NOT actually disable hardware interrupts
  - Host tracks per-client VIF; when VIF=0, host queues interrupts
    for the client instead of delivering them immediately
```

#### INT 31h / AX=0901h — Get and Enable Virtual Interrupt State

```
Input:
  AX = 0901h

Output:
  CF clear
  AL = previous virtual interrupt state
  (Virtual interrupts are now ENABLED)

Notes:
  - Equivalent of STI for the DPMI client
  - If interrupts were queued while VIF=0, they are delivered now
```

#### INT 31h / AX=0902h — Get Virtual Interrupt State

```
Input:
  AX = 0902h

Output:
  CF clear
  AL = current virtual interrupt state (0 = disabled, 1 = enabled)
```

---

### 3.9 Vendor Extensions (0A00h)

#### INT 31h / AX=0A00h — Get DPMI Vendor-Specific API Entry Point

```
Input:
  AX = 0A00h
  DS:ESI = pointer to ASCIIZ vendor string

Output (success):
  CF clear
  ES:EDI = vendor API entry point (FAR call address in PM)

Output (error):
  CF set
  AX = 8001h (unsupported function — vendor not recognized)

Notes:
  - CWSDPMI recognizes "CWSDPMI" → returns entry for extended functions
  - DOS/4GW recognizes "RATIONAL DOS/4G" for its extensions
  - For our kernel: can implement our own vendor extensions for
    kernel-specific services (process creation, etc.)
```

---

## 4. Initial Protected Mode Environment — Detailed

When a 32-bit DPMI client enters protected mode, it receives:

### 4.1 Selector Layout

```
Selector  Points To               Base                Limit       Type
CS        Client code             RM_CS << 4          4GB         Code32, DPL=3, readable
DS        Client data             RM_DS << 4          4GB         Data32, DPL=3, writable
SS        Client stack            RM_SS << 4          4GB         Data32, DPL=3, writable
ES        Client PSP              PSP_seg << 4        0FFh        Data16, DPL=3, writable
FS        (null)                  -                   -           0
GS        (null)                  -                   -           0
```

For 32-bit clients, the D bit is set on CS (default 32-bit operands/addresses)
and the B bit is set on SS/DS (32-bit stack operations).

### 4.2 How DOS/4GW Transforms This

DOS/4GW immediately restructures the environment after mode switch:

1. Allocates a large memory block (0501h) — e.g., 16MB
2. Allocates new LDT descriptors (0000h)
3. Sets descriptor base = 0, limit = 4GB (flat model) via 0007h/0008h
4. Reloads DS, ES, SS with the flat selectors
5. The LE image is loaded into the allocated memory block
6. CS is set up pointing to the code (or a flat code selector is created)
7. Jumps to the LE entry point

After this, the client runs in a flat memory model: all segments base=0,
limit=4GB. Linear addresses = physical addresses (in the client's view).

### 4.3 LDT Structure

The host manages the LDT. Key constraints:

- At least 8192 entries available per client
- Selectors are in the range 0004h to FFFFh (bit 2 = TI = 1 for LDT)
- Each selector = (index * 8) | 0x04 | 0x03 (TI=1, RPL=3)
- Host must track which descriptors are allocated vs free
- Initial selectors (CS, DS, SS, ES) consume 4 entries

Implementation approach:
```c
// LDT bitmap — 1 bit per descriptor, 8192 bits = 1024 bytes
static uint8_t ldt_bitmap[1024];
static struct descriptor ldt[8192];

int dpmi_alloc_descriptors(int count) {
    // Find 'count' contiguous free entries in ldt_bitmap
    // Mark them allocated
    // Initialize as present, data, DPL=3, base=0, limit=0
    // Return first selector = (index * 8) | 0x07
}
```

---

## 5. Interrupt Reflection — Detailed

### 5.1 Hardware Interrupt Flow (PM Client Active)

When a hardware interrupt fires while a DPMI client is running in PM:

```
1. CPU: interrupt via IDT → Ring 0 handler (our kernel)
2. Kernel: save client state (all GPRs, segment regs, EFLAGS)
3. Kernel: check if client has a PM handler installed (0205h)
4. IF client handler exists:
   a. Build an IRET frame on the client's stack:
      - Push EFLAGS (with VIF state)
      - Push CS (client's code selector)
      - Push EIP (client's return address)
   b. Set CS:EIP = client's handler (from 0205h)
   c. Clear IF in EFLAGS (interrupt gate semantics)
   d. Return to Ring 3 → client's handler runs
   e. Client handler does IRET → returns to interrupted code
5. IF no client handler:
   a. Reflect to real mode:
      - Switch to V86 or real mode
      - Execute the original real-mode INT handler
      - Switch back to PM
      - Resume client
```

### 5.2 Software Interrupt Reflection (INT 21h, etc.)

When a PM client executes `INT 21h` (or any software interrupt without a
PM handler installed):

```
1. CPU: INT 21h → IDT entry 21h → Ring 0
2. Kernel: no PM handler for INT 21h → reflect to real mode
3. Build DPMI_REGS from client's register state
4. Switch to V86/real mode
5. Execute INT 21h with those registers
6. Copy results back to client's registers
7. Switch back to PM, resume client
```

This is how DOS file I/O works from protected mode — the host transparently
reflects INT 21h to the real-mode DOS kernel.

### 5.3 Register Structure for Translation

The DPMI_REGS structure (50 bytes, detailed in function 0300h above) is the
canonical format for register exchange between PM and RM. It is used by:

- 0300h: Simulate real-mode interrupt
- 0301h: Call RM far procedure (IRET)
- 0302h: Call RM far procedure (RETF)
- 0303h: Real-mode callbacks (passed to PM handler at ES:EDI)
- Internal interrupt reflection

```c
typedef struct {
    uint32_t edi;       // 00h
    uint32_t esi;       // 04h
    uint32_t ebp;       // 08h
    uint32_t reserved;  // 0Ch (must be 0)
    uint32_t ebx;       // 10h
    uint32_t edx;       // 14h
    uint32_t ecx;       // 18h
    uint32_t eax;       // 1Ch
    uint16_t flags;     // 20h
    uint16_t es;        // 22h
    uint16_t ds;        // 24h
    uint16_t fs;        // 26h
    uint16_t gs;        // 28h
    uint16_t ip;        // 2Ah
    uint16_t cs;        // 2Ch
    uint16_t sp;        // 2Eh
    uint16_t ss;        // 30h
} __attribute__((packed)) dpmi_regs_t;
```

---

## 6. Real-Mode Callback Mechanism — Detailed

### 6.1 Purpose

Real-mode callbacks allow PM code to be invoked from real mode. The most
common use case is the mouse callback: INT 33h allows registering a
real-mode callback that fires on mouse events. With DPMI, the PM program
installs a real-mode callback, gives the RM address to INT 33h, and when
the mouse moves, the chain is:

```
Mouse IRQ → INT 33h driver (RM) → callback stub (RM) →
  mode switch to PM → PM handler → mode switch back → INT 33h returns
```

### 6.2 Implementation

The host maintains an array of callback slots (typically 16):

```c
#define MAX_CALLBACKS 16

typedef struct {
    int active;
    uint32_t pm_proc_sel;    // CS for PM procedure
    uint32_t pm_proc_off;    // EIP for PM procedure
    uint32_t regs_sel;       // ES for DPMI_REGS
    uint32_t regs_off;       // EDI for DPMI_REGS
    uint16_t rm_seg;         // Real-mode callback segment
    uint16_t rm_off;         // Real-mode callback offset
} callback_slot_t;

callback_slot_t callbacks[MAX_CALLBACKS];
```

Each callback needs a real-mode entry stub in conventional memory. The stub
is very small:

```asm
; Real-mode callback stub (one per slot)
; Located in conventional memory (below 640KB)
callback_stub_N:
    ; CPU is in real mode (or V86) when this executes
    ; The host detects this call via a software interrupt or
    ; trap mechanism and performs the mode switch
    int 0xxh        ; trap to host (host-specific mechanism)
    db N            ; callback slot number
    iret            ; return to real-mode caller after host processes
```

In practice, CWSDPMI uses a different mechanism — the callback stubs perform
a `CALL FAR` to the DPMI host entry point, which triggers a mode switch via
the V86 monitor (GPF on far call to Ring 0 segment).

### 6.3 Callback PM Procedure Protocol

When the PM procedure runs:

```
Entry state:
  DS:ESI = selector:offset pointing to real-mode stack at time of callback
           (host creates a descriptor mapping RM SS:SP)
  ES:EDI = selector:offset of the DPMI_REGS structure
           (same one passed to 0303h — host fills it with RM register state)
  SS:ESP = host-provided locked PM stack
  All other registers undefined

The PM procedure must:
  1. Read/process the information in DPMI_REGS (ES:EDI)
  2. Optionally modify DPMI_REGS to set return values for real-mode caller
  3. Optionally read data from the RM stack via DS:ESI
  4. Set ES:EDI to point to the DPMI_REGS to load on return to RM
  5. Execute IRETD to return control to the host

On IRETD:
  Host switches back to real mode, loads registers from DPMI_REGS,
  continues execution of the real-mode caller
```

---

## 7. DOS Extender Integration (DOS/4GW + CWSDPMI Model)

### 7.1 The Boot Sequence

A DOS/4GW program (like DOOM.EXE) is structured as:

```
DOOM.EXE layout:
  ┌─────────────────────────┐
  │ MZ Header (DOS stub)    │  Real-mode 16-bit code
  │ - Contains DOS/4GW stub │  This IS the DOS extender
  │ - OR: "requires DOS4GW" │
  ├─────────────────────────┤
  │ LE Header               │  Linear Executable format
  │ - Object table          │  (32-bit protected mode)
  │ - Fixup table           │
  ├─────────────────────────┤
  │ Object pages (code)     │  The actual 32-bit code
  │ Object pages (data)     │  
  └─────────────────────────┘
```

### 7.2 What the MZ Stub Does

1. **Check for DPMI:** Call INT 2Fh/AX=1687h
2. **If no DPMI:** Load CWSDPMI.EXE (or DOS4GW.EXE) as a TSR, retry
3. **Allocate private data:** INT 21h/48h for SI paragraphs
4. **Enter protected mode:** Call the mode switch entry point
5. **Now in PM:** Begin DOS/4GW initialization

### 7.3 What DOS/4GW Does in Protected Mode

1. **Allocate memory:** INT 31h/0501h — get a large block (all available)
2. **Set up flat model:**
   - Allocate descriptors (0000h)
   - Set base = 0 (0007h)
   - Set limit = 4GB (0008h, with CX:DX = FFFFFFFFh)
   - Set access rights: 32-bit, DPL=3 (0009h)
3. **Parse the LE header** from the EXE file
4. **Load object pages** into the allocated memory
5. **Apply fixups** (relocations) from the fixup table
6. **Set up the stack** from the LE header's stack object
7. **Jump to the LE entry point** — the 32-bit application starts

### 7.4 Runtime Services

During execution, DOS/4GW intercepts:

- **INT 21h:** Translates PM file I/O calls to real mode via 0300h
  - File handles are passed through directly
  - Buffer addresses must be translated (PM linear → RM segment:offset)
  - For reads/writes, DOS/4GW allocates a DOS transfer buffer (0100h)
    below 1MB and copies data in/out
- **INT 31h:** Passed through to DPMI host
- **INT 10h, 33h, etc.:** Reflected to real mode via 0300h

---

## 8. What DOOM Specifically Calls — The Critical Subset

Based on analysis of DOOM's source (id Software) and DOS/4GW behavior,
here is the minimum DPMI function set required:

### 8.1 Must-Implement (DOOM will not run without these)

| Function | Used For | Priority |
|----------|----------|----------|
| **0501h** | Allocate memory block — DOOM's Z_Init allocates zone memory | CRITICAL |
| **0502h** | Free memory block — cleanup | CRITICAL |
| **0000h** | Allocate LDT descriptors — flat model setup | CRITICAL |
| **0001h** | Free LDT descriptor | CRITICAL |
| **0007h** | Set segment base — flat model (base=0) | CRITICAL |
| **0008h** | Set segment limit — flat model (limit=4GB) | CRITICAL |
| **0009h** | Set access rights — 32-bit, DPL=3 | CRITICAL |
| **0300h** | Simulate RM interrupt — ALL DOS/BIOS calls | CRITICAL |
| **0200h** | Get RM interrupt vector | HIGH |
| **0201h** | Set RM interrupt vector | HIGH |
| **0204h** | Get PM interrupt vector | HIGH |
| **0205h** | Set PM interrupt vector — keyboard, timer hooks | HIGH |
| **0100h** | Allocate DOS memory — DMA buffers, transfer buffers | HIGH |
| **0101h** | Free DOS memory | HIGH |
| **0500h** | Get free memory info — DOS/4GW checks available memory | HIGH |

### 8.2 Should-Implement (needed for full compatibility)

| Function | Used For |
|----------|----------|
| **0800h** | Physical address mapping — VGA framebuffer |
| **0006h** | Get segment base — introspection |
| **0003h** | Get selector increment — multi-descriptor allocation |
| **000Bh** | Get descriptor — introspection |
| **000Ch** | Set descriptor — bulk descriptor setup |
| **0303h** | Allocate RM callback — mouse handler |
| **0304h** | Free RM callback |
| **0600h** | Lock linear region — IRQ handlers, DMA |
| **0601h** | Unlock linear region |
| **0604h** | Get page size |
| **0900h** | Get/disable virtual interrupts |
| **0901h** | Get/enable virtual interrupts |
| **0902h** | Get virtual interrupt state |

### 8.3 Can-Stub (return success, minimal implementation)

| Function | Why |
|----------|-----|
| **0002h** | Segment to descriptor — rarely used by DOS/4GW |
| **000Ah** | Create alias — not needed for flat model |
| **0102h** | Resize DOS memory — rare |
| **0202h** | Get exception handler — DOS/4GW may query |
| **0203h** | Set exception handler — DOS/4GW installs handlers |
| **0301h** | Call RM far proc (IRET) — rare |
| **0302h** | Call RM far proc (RETF) — rare |
| **0503h** | Resize memory block — rare for DOOM |
| **0602h** | Mark pageable — no-op |
| **0603h** | Relock — no-op |
| **0A00h** | Vendor extensions — return 8001h (unsupported) |

### 8.4 DOOM-Specific Call Flow

```
DOOM.EXE startup:
  DOS/4GW stub detects DPMI (INT 2Fh/1687h)
  Mode switch to PM
  DOS/4GW: 0501h — allocate ~4MB memory block
  DOS/4GW: 0000h, 0007h, 0008h, 0009h — create flat selectors
  DOS/4GW: loads LE image into memory
  DOOM main():
    Z_Init: already have memory from 0501h
    I_InitGraphics: 0300h → INT 10h (set video mode 13h)
    I_InitSound: 0100h (DMA buffer), 0300h → DMA controller setup
    D_DoomMain: game loop
      Rendering: writes directly to mapped VGA memory
      Input: 0205h hooks keyboard IRQ (IRQ 1 / INT 9)
      Sound: DMA transfers, uses locked memory (0600h)
      File I/O: 0300h → INT 21h (open/read/write/close WAD files)
    I_Quit: 0300h → INT 10h (restore text mode), 0502h (free memory)
```

---

## 9. Implementation Plan for Pinecore Kernel

### 9.1 Data Structures Needed

```c
// Per-DPMI-client state
typedef struct dpmi_client {
    // LDT management
    struct descriptor ldt[8192];
    uint8_t ldt_alloc[1024];      // bitmap: 8192 bits
    
    // Memory blocks
    struct {
        uint32_t linear_addr;
        uint32_t size;
        uint32_t handle;
        int active;
    } mem_blocks[256];
    uint32_t next_handle;
    
    // Interrupt vectors
    struct { uint16_t sel; uint32_t off; } pm_vectors[256];
    struct { uint16_t seg; uint16_t off; } rm_vectors[256];
    struct { uint16_t sel; uint32_t off; } exception_handlers[32];
    
    // Real-mode callbacks
    callback_slot_t callbacks[MAX_CALLBACKS];
    
    // Virtual interrupt state
    int vif;                       // virtual IF
    
    // DOS memory allocations (track for cleanup)
    struct { uint16_t seg; uint16_t sel; uint16_t paras; } dos_blocks[64];
    
    // Mode switch state
    uint32_t rm_sp, rm_ss;        // saved real-mode stack
    tss_t *task;                  // this client's TSS
} dpmi_client_t;
```

### 9.2 INT 31h Dispatch Table

```c
// Handler function type
typedef int (*dpmi_handler_t)(dpmi_client_t *client, regs_t *regs);

// Dispatch — called from INT 31h IDT handler
int dpmi_dispatch(dpmi_client_t *client, regs_t *regs) {
    uint16_t func = regs->eax & 0xFFFF;
    
    switch (func) {
        case 0x0000: return dpmi_alloc_descriptors(client, regs);
        case 0x0001: return dpmi_free_descriptor(client, regs);
        case 0x0002: return dpmi_seg_to_descriptor(client, regs);
        case 0x0003: return dpmi_get_selector_inc(client, regs);
        case 0x0006: return dpmi_get_base(client, regs);
        case 0x0007: return dpmi_set_base(client, regs);
        case 0x0008: return dpmi_set_limit(client, regs);
        case 0x0009: return dpmi_set_access(client, regs);
        case 0x000A: return dpmi_create_alias(client, regs);
        case 0x000B: return dpmi_get_descriptor(client, regs);
        case 0x000C: return dpmi_set_descriptor(client, regs);
        case 0x0100: return dpmi_alloc_dos_mem(client, regs);
        case 0x0101: return dpmi_free_dos_mem(client, regs);
        case 0x0102: return dpmi_resize_dos_mem(client, regs);
        case 0x0200: return dpmi_get_rm_vector(client, regs);
        case 0x0201: return dpmi_set_rm_vector(client, regs);
        case 0x0202: return dpmi_get_exception(client, regs);
        case 0x0203: return dpmi_set_exception(client, regs);
        case 0x0204: return dpmi_get_pm_vector(client, regs);
        case 0x0205: return dpmi_set_pm_vector(client, regs);
        case 0x0300: return dpmi_simulate_rm_int(client, regs);
        case 0x0301: return dpmi_call_rm_iret(client, regs);
        case 0x0302: return dpmi_call_rm_retf(client, regs);
        case 0x0303: return dpmi_alloc_callback(client, regs);
        case 0x0304: return dpmi_free_callback(client, regs);
        case 0x0500: return dpmi_get_mem_info(client, regs);
        case 0x0501: return dpmi_alloc_memory(client, regs);
        case 0x0502: return dpmi_free_memory(client, regs);
        case 0x0503: return dpmi_resize_memory(client, regs);
        case 0x0600: return dpmi_lock_region(client, regs);
        case 0x0601: return dpmi_unlock_region(client, regs);
        case 0x0602: return dpmi_mark_pageable(client, regs);
        case 0x0603: return dpmi_relock_region(client, regs);
        case 0x0604: return dpmi_get_page_size(client, regs);
        case 0x0800: return dpmi_phys_addr_map(client, regs);
        case 0x0900: return dpmi_get_disable_vif(client, regs);
        case 0x0901: return dpmi_get_enable_vif(client, regs);
        case 0x0902: return dpmi_get_vif(client, regs);
        case 0x0A00: return dpmi_vendor_entry(client, regs);
        default:
            regs->eax = (regs->eax & 0xFFFF0000) | 0x8001;
            return -1; // unsupported, set CF
    }
}
```

### 9.3 Key Implementation Notes

**INT 2Fh/1687h handler:** Must be installed in the V86 monitor's INT 2Fh
chain. When a V86 task calls INT 2Fh with AX=1687h, intercept it and return
our DPMI entry point.

**Mode switch entry:** This is a real-mode FAR call entry point. When called
from V86 mode, it will GPF (calling a Ring 0 address from Ring 3 in V86).
Our V86 monitor catches this GPF and performs the actual mode switch:
1. Allocate per-client dpmi_client_t
2. Build the LDT with initial selectors
3. Load LDTR for this client
4. Create a Ring 3 TSS or IRET frame with the PM selectors loaded
5. Return to Ring 3 in protected mode

**0300h (simulate RM INT):** This is the most complex function. Implementation:
1. Save PM client state
2. Enter V86 mode with registers from DPMI_REGS
3. If SS:SP = 0, provide a host stack in conventional memory
4. Push CX words from PM stack to V86 stack
5. Simulate the interrupt (push FLAGS/CS/IP, jump to IVT[BL])
6. V86 task runs until IRET
7. Capture V86 registers into DPMI_REGS
8. Return to PM client

**Physical address mapping (0800h):** Since we control page tables:
1. Find free linear address range of the requested size
2. Map physical pages to those linear addresses (PTE = phys | PT_P | PT_W | PT_U | PT_PCD)
3. Return the linear address
4. Set PT_PCD (page cache disable) for MMIO regions

**Flat model support:** DOS/4GW will call 0008h with limit = 0xFFFFFFFF.
The descriptor needs G=1 (page granular), limit field = 0xFFFFF, which
with G=1 gives limit = 0xFFFFF * 4096 + 4095 = 0xFFFFFFFF. This is the
standard 4GB flat segment.

---

## 10. INT 2Fh/AX=1687h Response Values for Our Kernel

```c
void dpmi_detect_handler(v86_regs_t *regs) {
    regs->eax = 0x0000;          // DPMI present
    regs->ebx = 0x0001;          // 32-bit programs supported
    regs->ecx = 0x0003;          // processor = 386
    regs->edx = 0x005A;          // version 0.90 (DH=0, DL=5Ah=90)
    regs->esi = 0x0010;          // 16 paragraphs (256 bytes) private data
    regs->es  = DPMI_ENTRY_SEG;  // real-mode segment of entry point
    regs->edi = DPMI_ENTRY_OFF;  // offset of entry point
}
```

---

## 11. Summary: Minimum Viable DPMI Host for DOOM

To run DOOM via DOS/4GW, implement in this order:

1. **INT 2Fh/1687h** — DPMI detection (V86 intercept)
2. **Mode switch entry** — V86 → PM transition, LDT setup
3. **0000h, 0001h** — descriptor allocation/free
4. **0007h, 0008h, 0009h** — set base/limit/access (flat model)
5. **0501h, 0502h** — memory allocation (the big one)
6. **0300h** — simulate real-mode interrupt (DOS/BIOS calls)
7. **0200h-0205h** — interrupt vector get/set
8. **0100h, 0101h** — DOS memory (transfer buffers)
9. **0500h** — free memory info
10. **0600h** — lock region (no-op for non-paging kernel)
11. **0800h** — physical address mapping (VGA framebuffer)
12. **0900h-0902h** — virtual interrupt state

That gives you a DPMI host that can run DOOM and most DOS/4GW applications.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| DPMI Spec 0.9 | Intel, 1990 (web: delorie.com/djgpp/doc/dpmi/) | Official specification |
| CWSDPMI exphdlr.c | cwsdpmi-master/src/exphdlr.c | INT 31h handler implementations |
| CWSDPMI mswitch.asm | cwsdpmi-master/src/mswitch.asm | Mode switching |
| Ralf Brown's Int List | fd.lod.bz/rbil/ | INT 31h, INT 2Fh/1687h details |
| DOS/4GW manual | Tenberry Software | DOS extender behavior |
| DOOM source | github.com/id-Software/DOOM | What functions a real game uses |
| Research 03 | docs/research/03-cwsdpmi-internals.md | Our CWSDPMI analysis |
| Research 14 | docs/research/14-v86-monitor.md | V86 mode for RM simulation |

---

*Last updated: 2026-05-02*
