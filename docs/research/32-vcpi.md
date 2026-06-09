# VCPI — Virtual Control Program Interface

> VCPI allows V86-mode programs to enter protected mode when running under a V86 monitor (like EMM386 or our kernel). Required by DOS/16M (used by DOOM shareware).

**Date:** 2026-05-03
**Status:** Complete — register-level reference for implementation
**Sources:** VCPI Specification v1.0 (Phar Lap/Qualitas/Quarterdeck, 1989); DOS/16M documentation

---

## Overview

VCPI is an extension to the EMS (Expanded Memory) interface. Programs detect VCPI by:
1. Checking that INT 67h points to a valid device driver with name `EMMXXXX0`
2. Calling INT 67h/DE00h to check VCPI version

VCPI provides 13 functions (DE00h-DE0Ch) for memory management and PM switching.

## EMS Device Detection

Programs find EMS by checking the device driver name at the INT 67h handler's segment:

```
1. Read INT 67h vector from IVT (0000:019C)
2. Handler segment = high word of vector
3. Check bytes at segment:000A — must be "EMMXXXX0" (8 bytes)
```

If the signature matches, EMS is present and VCPI may be available.

## VCPI Functions (INT 67h, AH=DEh)

### DE00h — VCPI Presence Detection
- **Input:** AX=DE00h
- **Output:** AH=00h (success), BH=major version, BL=minor version
- Returns version 1.0 (BX=0100h)

### DE01h — Get Protected Mode Interface
- **Input:** AX=DE01h, ES:DI=buffer for 3 GDT entries (24 bytes), DS:SI=buffer for page table entries
- **Output:** AH=00h, EBX=PM entry point offset (in first code segment)
- The 3 GDT entries are: VCPI code (Ring 0), VCPI data, client code
- The client can call the PM entry point for VCPI services in PM

### DE02h — Get Maximum Physical Memory Address
- **Input:** AX=DE02h
- **Output:** AH=00h, EDX=highest physical address + 1

### DE03h — Get Number of Free 4KB Pages
- **Input:** AX=DE03h
- **Output:** AH=00h, EDX=number of free 4KB pages

### DE04h — Allocate a 4KB Page
- **Input:** AX=DE04h
- **Output:** AH=00h, EDX=physical address of allocated page
- Error: AH=88h (no pages available)

### DE05h — Free a 4KB Page
- **Input:** AX=DE05h, EDX=physical address of page
- **Output:** AH=00h

### DE06h — Get Physical Address of Page in First MB
- **Input:** AX=DE06h, CX=page number (0-255, covering first 1MB)
- **Output:** AH=00h, EDX=physical address (usually = CX * 4096, identity mapped)

### DE07h — Read CR0
- **Input:** AX=DE07h
- **Output:** AH=00h, EBX=CR0 value

### DE08h — Read Debug Registers
- **Input:** AX=DE08h, ES:DI=buffer (8 dwords for DR0-DR7)
- **Output:** AH=00h, buffer filled

### DE09h — Load Debug Registers
- **Input:** AX=DE09h, ES:DI=buffer (8 dwords)
- **Output:** AH=00h

### DE0Ah — Get 8259 Interrupt Vector Mappings
- **Input:** AX=DE0Ah
- **Output:** AH=00h, BX=master PIC base, CX=slave PIC base

### DE0Bh — Set 8259 Interrupt Vector Mappings
- **Input:** AX=DE0Bh, BX=master base, CX=slave base
- **Output:** AH=00h
- Note: VCPI host may ignore this

### DE0Ch — Switch from V86 to Protected Mode (THE BIG ONE)
- **Input:** AX=DE0Ch
- **On stack:** linear address of structure:
  ```
  Offset 0:  CR3 (page directory physical address)
  Offset 4:  GDTR linear address
  Offset 8:  IDTR linear address
  Offset 12: LDTR selector
  Offset 16: TR selector
  Offset 20: EIP (entry point)
  Offset 24: CS selector
  ```
- **Output:** CPU is in protected mode at specified CS:EIP with specified GDT/IDT/LDT/TSS

## Implementation Strategy for Pinecore

Since we are already a PM kernel running V86 tasks, implementing VCPI DE0Ch is different from a traditional VCPI server (like EMM386):

**Traditional (EMM386):** EMM386 IS the V86 monitor. When VCPI client calls DE0Ch, EMM386 actually loads the client's CR3, GDT, IDT, etc. and drops to PM. The client then runs as the OS.

**Pinecore approach:** We STAY in control. When a V86 task calls DE0Ch:
1. We read the client's desired state (GDT, IDT, CR3, CS:EIP)
2. We DON'T load the client's CR3/GDT/IDT (that would destroy our kernel)
3. Instead, we parse the client's GDT to find its code/data segments
4. We create equivalent LDT entries for the client
5. We clear VM bit and IRET into the client's PM code
6. All client interrupts come to OUR IDT, and we forward to the client's handlers

This is a **virtualized VCPI** — the client thinks it controls PM, but we maintain the real PM environment and forward everything.

### Key difference from DPMI:
- DPMI: client knows it's sharing PM with a host, uses INT 31h for services
- VCPI: client thinks it IS the PM environment, has its own GDT/IDT

For DOS/16M, after DE0Ch it will:
1. Set up its own GDT with flat 32-bit segments
2. Set up its own IDT for interrupt handling
3. Load the game's 32-bit code
4. Run the game

We need to intercept the interrupts (especially INT 21h for file I/O) and forward them through our DOS emulation.

---

*Primary source: VCPI Specification Version 1.0 (Phar Lap Software / Qualitas / Quarterdeck, 1989)*
