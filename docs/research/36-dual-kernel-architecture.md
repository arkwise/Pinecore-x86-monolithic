# Chapter 36 — Dual Kernel Architecture: DOS vs Pure Pinecore

## The Problem

Pinecore currently boots from FreeDOS, which leaves stale BIOS/FreeDOS
handlers in low memory. DOS extenders (DOS/4GW used by DOOM) save these
handlers and chain to them, hitting dead code that loops forever.

This is fundamentally unsolvable while booting from FreeDOS — the stale
handlers are part of the FreeDOS kernel that FREECOM depends on.

## Solution: Two Kernel Modes

### DOS Kernel (`kernel-dos`)

- **Boot:** From FreeDOS (floppy/HDD with FreeDOS boot sector)
- **Shell:** FREECOM (COMMAND.COM) running in V86 mode
- **App support:** Real-mode DOS apps only (.COM, .EXE 16-bit)
- **No PM apps:** Does NOT provide VCPI/DPMI for protected-mode DOS apps
- **Use case:** Running classic real-mode DOS programs, file management,
  batch files, FreeDOS utilities
- **Existing code:** Everything we have today (v86.c, dos.c, fat.c, etc.)

### Pure Pinecore Kernel (`kernel-pure`)

- **Boot:** Standalone bootloader (PINE.COM or direct multiboot)
- **Shell:** Built-in Pinecore shell (no FREECOM dependency)
- **32-bit native:** Runs entirely in protected mode from boot
- **V86 for DOS:** Creates V86 tasks for real-mode DOS compatibility
- **DPMI/VCPI:** Full PM app support — DOOM, Quake, etc.
- **Clean IVT:** We control ALL interrupt vectors from the start.
  No stale handlers. No FreeDOS residue.
- **Use case:** Running protected-mode DOS games and applications

## What Changes Between Modes

| Component         | DOS Kernel              | Pure Kernel             |
|-------------------|------------------------|------------------------|
| Bootloader        | FreeDOS boot sector     | Own boot sector / multiboot |
| Shell             | FREECOM (V86)           | Built-in shell (PM)    |
| INT 21h           | Emulated for V86 FREECOM| Emulated for V86 + PM apps |
| IVT               | Inherited from FreeDOS  | Fully controlled by us |
| VCPI              | Disabled (stale handlers)| Full DE0Ch support     |
| DPMI              | Basic (for simple apps) | Full (for DOS extenders)|
| Memory model      | V86 + FreeDOS MCBs     | Flat 32-bit + V86 tasks|

## Shared Code (both kernels use)

- `pmm.c` — Physical memory manager
- `vmm.c` — Virtual memory / page tables
- `idt.c`, `isr_stubs.asm` — Interrupt handling
- `sched.c` — Preemptive scheduler
- `vga.c` — VGA text/graphics driver
- `fat.c` — FAT filesystem
- `ata.c` — HDD driver
- `serial.c` — Debug serial output
- `keyboard.c` — Keyboard driver
- `vt.c` — Virtual terminals
- `dpmi.c` — DPMI host
- `v86.c` — V86 monitor
- `dos.c` — DOS INT 21h emulation

## Pure Kernel Additions Needed

1. **Own bootloader** — Boot sector that loads kernel directly to PM
   (we already have PINE.COM which does this)
2. **IVT initialization** — Set ALL 256 IVT entries to safe IRET stubs
   at kernel startup. No inherited FreeDOS state.
3. **Built-in shell** — PM-native command interpreter (we have `shell.c`)
4. **VCPI server** — Full DE0Ch implementation with clean page tables
5. **Clean V86 environment** — V86 tasks start with a known-good IVT,
   zero-filled low memory, and our DOS emulation for INT 21h

## Build System

```
src/
  kernel/          # Shared kernel code
  kernel-dos/      # DOS-specific (FreeDOS boot glue)
  kernel-pure/     # Pure-specific (standalone boot, full VCPI)
  boot/
    pine.com       # DOS bootloader (loads kernel from FreeDOS)
    boot.asm       # Standalone boot sector (pure mode)
```

Makefile targets:
- `make dos`  — Build DOS kernel (current default)
- `make pure` — Build pure kernel
- `make flat` — Build both + disk images
