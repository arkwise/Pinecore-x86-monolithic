# 46 — USBDDOS internals (the base we're forking + porting)

Status: research only (no code). Companion to `45-dos-usb-stack-landscape.md`, `47-xhci-from-spec.md`, `48-usb-port-plan.md`.

Source-of-truth: local clone at **`/Users/chelsonaitcheson/Projects/USBDDOS-master/USBDDOS/`**. Tag-checked against v1.0fix2 (CHANGELOG: 12/06/2023). License: GPL v2 (`/Users/chelsonaitcheson/Projects/USBDDOS-master/COPYING`).

Total LOC: ~22,000 across 44 files (`USBDDOS/*.c|*.h` + subdirs `HCD/`, `CLASS/`, `DPMI/`). The chip-touching subset that matters for our port is roughly **8,000-9,000 LOC** — close to the e1000e port-sizing.

---

## 1. Top-level layout

```
USBDDOS-master/
├── main.c                  ← TSR entry (commandline parsing, init, hooks DOS INTs)
├── emm.c / emm.h           ← EMM386/VCPI fallback (real-mode build only)
├── hdpmipt.c / hdpmipt.h   ← HDPMI passthrough — when HDPMI is the host
└── USBDDOS/
    ├── usb.c               ← USB-layer core (enumeration, requests, ISR fan-out)  [1116 LOC]
    ├── usb.h               ← USB layer API + structs
    ├── usbtable.c          ← Static dispatch tables wiring HCD types ↔ class drivers
    ├── usballoc.c          ← Slab-style buffer allocator for endpoint buffers
    ├── usbcfg.h            ← Compile-time tunables (max devices, debug level)
    ├── pci.c               ← PCI BIOS/config-space wrapper                        [419 LOC]
    ├── pic.c               ← 8259 PIC mask/unmask/EOI helpers
    ├── platform.h          ← Three-compiler portability (BC++3.1 / Watcom / DJGPP)
    ├── dbgutil.c           ← _LOG / _ASSERT / serial-port debug output
    ├── sample.c            ← Standalone "enumerate and dump" sanity check
    ├── HCD/
    │   ├── hcd.c           ← Generic root-hub + request dispatch
    │   ├── hcd.h           ← The HCD_Interface / HCD_Method abstraction
    │   ├── uhci.c / .h     ← Intel UHCI (USB 1.1, PIO)                            [850 LOC]
    │   ├── ohci.c / .h     ← OHCI (USB 1.1, MMIO)                                 [844 LOC]
    │   └── ehci.c / .h     ← EHCI (USB 2.0, MMIO + companion controllers)         [853 LOC]
    ├── CLASS/
    │   ├── hid.c / .h      ← Keyboard + mouse + boot protocol                    [702 LOC]
    │   ├── msc.c / .h      ← Mass Storage Class — Bulk-Only Transport            [1131 LOC]
    │   ├── hub.c / .h      ← Hub class — external hub enumeration               [...]
    │   └── cdc.c / .h      ← Communications Device Class (DJGPP build only)
    └── DPMI/
        ├── dpmi.c / .h     ← DPMI host abstraction — DMA, memory map, IRQ hook   [664 LOC]
        ├── dpmi_dj2.c      ← DJGPP-specific DPMI calls
        ├── dpmi_tsr.c      ← TSR-on-DPMI logic
        ├── dpmi_ldr.c      ← Boot-time loader for 16-bit Borland/Watcom build
        ├── dpmi_bc.h       ← Borland-specific DPMI shim (huge — 1996 LOC, inline asm)
        ├── dpmi_wc.c       ← Watcom-specific DPMI shim
        ├── dpmi_i21.h      ← INT 21h calls used by the DPMI layer
        ├── xms.c / .h      ← XMS allocator (fallback when no DPMI host)
        ├── dlmalloc.h      ← Doug Lea malloc — slab allocator backend (6281 LOC, vendored)
        └── djgpp/
            ├── coff.h      ← COFF reloc structs for the binary patcher
            └── gormcb.c    ← go32 real-mode callback shim
```

---

## 2. Architectural model — three layers

```
┌───────────────────────────────────────────────────────────────┐
│  CLASS DRIVERS — implement DOS-shimmed APIs                  │
│  HID → INT 16h / INT 33h     MSC → INT 13h     HUB → recur   │
│  CDC → COM-port emulation                                     │
└───────────────────────────────────────────────────────────────┘
                              │
┌───────────────────────────────────────────────────────────────┐
│  USB LAYER — core enumeration, request fan-out (usb.c)        │
│  USB_Init / USB_EnumerateDevices / USB_SendRequest            │
│  Per-IRQ ISR dispatch to all HCs sharing the line             │
└───────────────────────────────────────────────────────────────┘
                              │
┌───────────────────────────────────────────────────────────────┐
│  HCD LAYER — per-host-controller implementations (HCD/*)      │
│  UHCI_*, OHCI_*, EHCI_* fill in HCD_Type + HCD_Method tables  │
│  Hardware register I/O, DMA descriptors, IRQ handling         │
└───────────────────────────────────────────────────────────────┘
                              │
┌───────────────────────────────────────────────────────────────┐
│  DPMI / PLATFORM LAYER (DPMI/*)                              │
│  DMA-safe allocation, MMIO mapping, IRQ install, compiler ABI │
└───────────────────────────────────────────────────────────────┘
```

The layering is **clean**. The HCD layer never knows about classes; the class layer never knows about chip-specific quirks; the USB layer enumerates and routes. This is what makes the codebase portable into a kernel module (we replace the bottom DPMI layer with our kernel APIs; everything above is largely untouched).

---

## 3. The HCD abstraction (the key interface)

Defined at **`USBDDOS/HCD/hcd.h:43-153`**. The two function-pointer tables that every HCD implementation fills:

### `HCD_Method` (`hcd.h:76-98`) — per-controller-instance operations
- `ControlTransfer`(device, ep, dir, setup8, data, length, cb, cbdata)
- `IsochronousTransfer`(device, ep, dir, buffer, length, cb, cbdata)
- `BulkTransfer`(device, ep, dir, buffer, length, cb, cbdata)
- `InterruptTransfer`(device, ep, dir, buffer, length, cb, cbdata)
- `GetPortStatus`(pHCI, port) → 16-bit status
- `SetPortStatus`(pHCI, port, status) → BOOL
- `InitDevice`(device) — set up chip-specific per-device structures
- `RemoveDevice`(device)
- `CreateEndpoint`(device, ep_addr, dir, type, max_packet, interval) → opaque endpoint pointer
- `RemoveEndPoint`(device, endpoint)

### `HCD_Type` (`hcd.h:100-107`) — per-controller-class operations
- `dwPI` — PCI programming interface byte (UHCI=0x00, OHCI=0x10, EHCI=0x20, xHCI=0x30)
- `name` — string for logging
- `InitController`(pHCI, pPCIDev)
- `DeinitController`(pHCI)
- `ISR`(pHCI) — called from the shared IRQ handler with the comment **"DON'T do IO, memory allocation in ISR"**

### `HCD_Interface` (`hcd.h:109-121`) — per-instance state
- PCI BDF
- HCD_Type pointer + HCD_Method pointer + opaque per-controller data
- `dwPhysicalAddress` — chip's physical MMIO/IO base
- `dwBaseAddress` — same, but linear/virtual under DPMI mapping
- `DeviceList[HCD_MAX_DEVICE_COUNT]`
- port count

**For xHCI all we do is fill in a fourth implementation** following this same shape. The abstraction was clearly designed to accommodate exactly that.

---

## 4. UHCI bring-up — the readable template

The cleanest of the three implementations because UHCI is the simplest spec. **`UHCI_InitController` at `HCD/uhci.c:59-127`** does (paraphrased — no code quotes):

1. **Legacy + IRQ enable** (`uhci.c:65-69`): read PCI config word at `LEGSUP` (offset 0xC0), clear `USBSMIEN` (stop USB SMI from BIOS), set `USBPIRQDEN` (enable PCI IRQ delivery), set 0x8F00 (clear all legacy trap bits), write back. This is the "BIOS hand-off" — after this, BIOS legacy USB stops, our driver owns the controller.

2. **PCI command-register enable** (`uhci.c:71-77`): set BusMaster + IOSpace + MemorySpace bits, clear InterruptDisable. Standard PCI activation.

3. **BAR read** (`uhci.c:79-82`): read 32-bit BAR at offset USBBASE (`uhci.h`), mask off the lower 5 bits (IO BAR flag bits) → `dwPhysicalAddress` and `dwBaseAddress` (UHCI is I/O-port-based, not MMIO — phys and base are identical).

4. **Controller reset** (`uhci.c:83`, `UHCI_ResetHC`).

5. **Frame List allocation** (`uhci.c:85-106`): UHCI's "frame list" is a 1024-entry array of physical pointers, one per 1 ms USB frame. Existing BIOS-allocated frame list is checked first; if absent, allocate 4 KB via `XMS_Alloc` (preferred over `DPMI_DMAMalloc` here to save the driver's own conventional/low-DOS memory, per the `xms.c` comment). Realloc with bigger pad if alignment misses 4 KB.

6. **Frame list base register write** (`uhci.c:97-103`): `outpd(base + FLBASEADD, DataArea)`. Map physical → linear if above 1 MB via `DPMI_MapMemory`.

7. **DataArea population** (`uhci.c:108-117`): allocate the `UHCI_HCData` struct via `DPMI_DMAMalloc` (this one must stay DMA-coherent), zero it, record frame-list handle.

8. **IRQ enable** (`uhci.c:118-120`): `UHCI_EnableInterrupt(pHCI, TRUE)`.

9. **Queue head + transfer descriptor setup** (`uhci.c:121`, `UHCI_QHTDSchedule`): standard UHCI "tree of QHs" — one per polling rate (1 ms, 2 ms, 8 ms — for interrupt EPs), one for control, one for bulk. Frame list entries point into this tree at the appropriate priority.

10. **Final start** (`uhci.c:122-127`): clear `USBSTS`, set start-of-frame to 0x40, set `USBCMD = MAXP|CF`, start HC.

**Key takeaways for the pinecore port:**
- All the PCI hand-off, MMIO mapping, IRQ enable, DMA allocation are wrapped in `DPMI_*` and `XMS_*` calls — our kernel-module port replaces these with `pci_*`, `dma_alloc`, `irq_install`.
- The chip-touching logic is ~100 LOC of `outp/inp` to fixed UHCI register offsets — straightforward.
- Frame list + QH/TD tree structure has to be rebuilt identically — chip doesn't care which OS built it, only the layout.

---

## 5. The ISR architecture (the part our kernel will rewrite)

USBDDOS multiplexes a single shared PCI IRQ across multiple host controllers via a hand-rolled "ISR wrapper" in **`usb.c`**:

### Setup (`usb.c:65-70`)
- Static `USB_ISR_FinalizerHeader` linked list
- Per-HC `DPMI_ISR_HANDLE USB_ISRHandle[USB_MAX_HC_COUNT]`
- Re-entrancy flag `USB_InISR`
- Saved PIC IRQ mask `USB_IRQMask`

### IRQ dispatch
1. Hardware IRQ fires.
2. DPMI host invokes USBDDOS's saved RM callback (or directly if PM-resident).
3. `USB_ISR_Wraper` (`usb.c:80`) runs — saves regs, disables IRQs, sets `USB_InISR = TRUE`, calls `USB_ISR` inline-function.
4. `USB_ISR` walks `USB_ISRHandle[]` array, for each registered HC calls its `pType->ISR(pHCI)` (`hcd.h:106`).
5. Each HC ISR (`UHCI_ISR` at `uhci.c:151`, etc.) reads its status register, processes completed transfers, invokes user callbacks via `HCD_InvokeCallBack`.
6. After all HCs serviced, `USB_InISR = FALSE`, 8259 EOI, IRET.

### The hard rule (HCD_Type comment, `hcd.h:106`)
> "DON'T do IO, memory allocation in ISR"

Which is observed in practice — the ISR completes transfers and queues callbacks; the *callbacks themselves* run later (either deferred to a "finalizer" linked list run at the next idle point, or — for synchronous transfers — completed and signalled via the request's `pFnCB`).

This is structurally identical to how kernel drivers work — top-half (ISR) does the minimum, bottom-half (deferred) does the heavy lifting. **For pinecore the port is direct**: top-half stays in our IDT entry, the per-HC `ISR` calls plug straight in.

---

## 6. The DPMI / DMA layer — where the port work concentrates

The three primitives that every other layer depends on:

### `DPMI_DMAMalloc(size, alignment)` (`DPMI/dpmi.c:~234`)
Returns a linear pointer to a buffer that's also addressable as a *physical* address by the host controller's bus-master DMA. Implementation:
- Under HDPMI: 0501h alloc, returns linear; then 0506h to get the physical address, store in a "DMA tag" so `DPMI_DMAFree` can free it.
- Under CWSDPMI: similar via 0501h + 0600h (lock pages).
- Under no host (real mode / Borland build): use XMS or DOS conventional malloc; linear = physical.

### `DPMI_MapMemory(physical, size)` (`DPMI/dpmi.c:~154`)
Returns a linear pointer for accessing a physical region that's not part of our allocations (e.g. the BIOS-pre-allocated frame list, or MMIO BARs). 0800h on HDPMI.

### `DPMI_InstallISR(irq, handler)` (`DPMI/dpmi.c` IRQ section)
Hooks the PIC-remapped vector for IRQ N. Variants for "wrap with PM stack switch via 0303h" (PM build) and "direct vector hook" (real-mode build).

### `DPMI_CallRealModeINT(vec, regs)` — repeated in `dpmi.c:613,619,623,630,639,644`
INT 21h reflection — used by the TSR code paths to access DOS file/console services.

**Pinecore port mapping:**

| USBDDOS primitive | Pinecore replacement |
|---|---|
| `DPMI_DMAMalloc` | `dma_alloc` from our reserved DMA region (linear = physical via identity map, per `44-82567lm-port-plan.md` §2) |
| `DPMI_DMAFree` | `dma_free` |
| `DPMI_MapMemory(phys, len)` | `vmm_map_physical(phys, len)` |
| `DPMI_UnmapMemory` | `vmm_unmap_physical` |
| `DPMI_InstallISR(irq, cb)` | `irq_register(irq, cb)` |
| `DPMI_CallRealModeINT(0x21, ...)` | direct call into our `dos.c` INT 21h handler — no V86 trip |
| `XMS_Alloc`, `XMS_Free` | `kmalloc`, `kfree` for kernel heap; `dma_alloc` if buffer needs to be DMA-addressable |
| `outp` / `inp` / `outpd` / `inpd` | our existing `io_outb`/`io_inb` family |
| `pci_*` config-space helpers in `USBDDOS/pci.c` | our existing `src/kernel/pci.c` per `20-pci-bus.md` |

All the layers above (HCD, USB core, class drivers) are nearly unmodified — they call the DPMI layer through a small set of named functions, and we replace the layer wholesale.

---

## 7. Known bugs / improvements — the upstream contribution backlog

From `TODO`, code grep, and CHANGELOG:

### High-impact fixes (good upstream PR material)
1. **UHCI isochronous transfer broken** (TODO #1 — `UHCI_USE_INTERRUPT` macro at `uhci.c:16` left at 1 with comment "debug use, do not change to 0", suggesting polling fallback is incomplete).
2. **EHCI isochronous transfer not implemented** (TODO #2). UHCI has at least skeleton; EHCI is empty.
3. **xHCI absent** (TODO #4). The big one.

### Quality-of-life additions
4. **No USB Audio Class.** No `CLASS/uac.c`. Pairs with our planned SB16 shim.
5. **No USB Video Class.** Less critical — niche.
6. **No printer class.** Bret Johnson's DOSUSB has this; useful for retro-printing workflows.
7. **PCI IRQ routing fallback** (CHANGELOG 12/24/2023: "if IRQ=0xFF, set IRQ from PCI IRQ routing options") — this exists but only triggers on the all-1s case. Robust routing (read BIOS `_PRT` from `$PIR` table) would also help when BIOS programs the wrong IRQ.

### Code-quality / portability
8. **`dlmalloc.h` 6,281 LOC vendored** — this is Doug Lea's malloc, fine, but consider a smaller allocator for memory-constrained TSR builds.
9. **Borland builds drop CDC for code-size** — the explicit `#if defined(__BC__) || defined(__WC__) #define USE_CDC 0` at `usbtable.c:25` could be addressed by splitting CDC into a separately-loaded module.
10. **`platform.h` (537 LOC)** — heroic three-compiler portability (BC, Watcom, DJGPP). Useful but a tax on every change.

### Things the upstream might NOT want (so we keep them pinecore-side)
- Kernel-mode rewrites of the DPMI layer (their world is DPMI client, not Ring 0).
- Removal of the 16-bit Borland support (their userbase still uses it).
- Removal of XMS fallback (some boards have no DPMI host).

---

## 8. Lines of code accounting — port estimate

Mirrors the methodology used in `42-e1000e-linux-driver-map.md`:

| Subsystem | USBDDOS LOC | Our port LOC (est.) | Reason |
|-----------|-------------|---------------------|--------|
| USB core (`usb.c`) | 1,116 | 700 | Drop DPMI-host-shopping, drop dual-build branching |
| HCD common (`HCD/hcd.c`, `hcd.h`) | 161+ | 150 | Mostly direct port |
| UHCI driver | 850 + 266h | 600 | Drop dual-mode polling, simplify XMS path |
| OHCI driver | 844 + 283h | 600 | Same |
| EHCI driver | 853 + 608h | 700 | Same; add isoc properly |
| **xHCI driver (NEW)** | 0 | **3,000-4,000** | From scratch per `47-xhci-from-spec.md` |
| HID class | 702 | 400 | Same algorithms, lighter framework |
| MSC class | 1,131 | 700 | Same |
| HUB class | ~400 | 300 | Same |
| CDC class | ~400 | 300 | Same |
| **UAC class (NEW)** | 0 | **1,500** | From scratch |
| pci.c | 419 | reuse our `pci.c` | Already have PCI infra |
| dpmi.c + xms.c | ~900 | reuse our kernel | DPMI/DMA replaced with kernel APIs |
| platform.h, dbgutil | ~880 | 50 | Most is BC/Watcom shims we don't need |
| dlmalloc.h | 6,281 | reuse kernel heap | We already have `heap.c` |
| **Total chip-touching code we write** | — | **~8,000-9,000 LOC** | |

Plus contribute-back content: the xHCI + UAC + isoc fixes for upstream, written in USBDDOS's idioms (which adds ~30% to the LOC budget for each contribution because of the BC/Watcom dual-build).

---

## 9. The "good first contribution" path

If we want to demonstrate competence to the upstream maintainer before submitting xHCI (which is a huge PR), the order would be:

1. **PR #1 (small, ~50 LOC):** documentation fixes, typo corrections, README clarifications. Probability of merge: very high. Builds the maintainer relationship.
2. **PR #2 (medium, ~300 LOC):** UHCI isochronous transfer fix (TODO #1). Concrete, useful, observable improvement.
3. **PR #3 (large, ~500 LOC):** EHCI isochronous transfer implementation (TODO #2). Now we're committing real chip-level work.
4. **PR #4 (huge, ~4,000 LOC):** xHCI HCD. By this point we have a track record; the maintainer reviews on character, not just diff size.

This is the standard open-source contribution arc. Time investment: ~3-4 sessions per PR if done well.

---

## 10. References

### Local files (all in `/Users/chelsonaitcheson/Projects/USBDDOS-master/`)
- `README.md` — author's project description, supported hardware, build options
- `CHANGELOG` — recent release notes
- `TODO` — open work items, the source of our "improvements" priority list
- `COPYING` — GPL v2
- `USBDDOS/HCD/hcd.h` — the core abstraction
- `USBDDOS/HCD/uhci.c` — UHCI bring-up template (reads cleanly)
- `USBDDOS/HCD/ehci.c` — EHCI bring-up; isoc gap is here
- `USBDDOS/HCD/ohci.c` — OHCI bring-up
- `USBDDOS/usb.c` — USB core + ISR multiplexing
- `USBDDOS/CLASS/{hid,msc,hub,cdc}.{c,h}` — class drivers
- `USBDDOS/DPMI/dpmi.{c,h}` — the DPMI abstraction (replace with kernel APIs)

### Upstream
- USBDDOS on GitHub: <https://github.com/crazii/USBDDOS> — **verified 2026-05-26**. Maintainer handle `crazii`. GPLv2. 101 stars. Active community fork at <https://github.com/Netrunner01/USBDDOS>.
- Original ancestor: <https://code.google.com/archive/p/usb-driver-under-dos/>

### Cross-references in this repo
- `45-dos-usb-stack-landscape.md` — why USBDDOS over alternatives
- `47-xhci-from-spec.md` — the xHCI driver we'll add
- `48-usb-port-plan.md` — kernel-module port plan
- `42-e1000e-linux-driver-map.md` — same methodology applied to the NIC
- `20-pci-bus.md` — pinecore's PCI infra USBDDOS's `pci.c` is replaced by
- `44-82567lm-port-plan.md` §2 — DMA region design that the USB descriptor rings share
