# 54 — usbcore env, DMA region, IRQ routing, kexport synthesis

Status: research only (no code). **Pass 1** of the s53 spec-first discipline — the final pre-code doc. Synthesises the per-module requirements from docs 50-53 into a concrete env spec for `s53.a` (kernel-side USB prereqs) and the boot-time module-load order that lets s53.b-d come up cleanly.

Companion docs:
- `50-usb-enumeration-walkthrough.md` — usbcore.kmd (USB device framework)
- `51-uhci-driver-derivation.md` — uhci.kmd (HCD)
- `52-hid-boot-protocol-mapping.md` — hid.kmd (keyboard + mouse)
- `53-msc-bbb-int13h-shim.md` — msc.kmd (storage)
- `48-usb-port-plan.md` — strategy
- `project_kmd_module_loader_landed.md` (memory) — the .kmd loader from s51

This doc cites the prior research docs, not external specs, because it's a synthesis.

---

## 1. The four-module dependency graph

```
                ┌────────────────────────────────────┐
                │            kernel                  │
                │  s53.a: dma_*, irq_register,       │
                │         port-I/O, pit_*, pci_*,    │
                │         int13h_register_disk,      │
                │         keyboard_inject_key,       │
                │         mouse_inject               │
                └─────┬───────────────────────┬──────┘
                      │                       │
                      ▼ EXPORT_SYMBOL         ▼
       ┌──────────────────────────┐    ┌──────────────────┐
       │   usbcore.kmd  (s53.b)   │    │  uhci.kmd (s53.c)│
       │   USB device framework    │    │  HCD plug-in     │
       │   - enumeration          │ ←──┤  - one per       │
       │   - request submission   │    │    controller    │
       │   - class-driver match    │    │  - registers via │
       └──────────┬───────────────┘    │    usbcore_      │
                  │                    │    register_hcd  │
                  │ EXPORT_SYMBOL_GPL  └──────────────────┘
                  │
        ┌─────────┴─────────┐
        ▼                   ▼
   ┌──────────┐        ┌──────────┐
   │ hid.kmd  │        │ msc.kmd  │
   │ (s53.d)  │        │ (s53.d') │
   │ Boot kbd │        │ BBB →    │
   │ + mouse  │        │ INT 13h  │
   └──────────┘        └──────────┘
```

**Load order at boot**:
1. Kernel (s53.a prerequisites already exported).
2. usbcore.kmd — must load before any HCD or class driver.
3. uhci.kmd (and later ehci.kmd, xhci.kmd) — registers itself with usbcore.
4. hid.kmd, msc.kmd, hub.kmd (and others) — register their `usb_class_driver_t` with usbcore.
5. Each HCD probes its PCI devices; when a port reports a connected device, usbcore enumerates it and dispatches to matching class drivers.

If a class driver loads before the HCD, no devices match yet — they'll match later when the HCD probes. **Order tolerance is built in**: usbcore stores registered class drivers and re-runs `match()` for every newly-enumerated device.

**Alphabetical sort** of `\DRIVERS\*.KMD` satisfies the required order (`hid`, `msc`, `uhci`, `usbcore`) **except**: `usbcore` would load last alphabetically. Two options:
- (a) Add `MODULE_DEPENDS("usbcore")` and let the loader resolve order via topological sort.
- (b) Use a manifest: `\DRIVERS\LOAD.LST` lines the .kmds in load order.

**Pick (a)** — already in s52's open questions, simpler than another file format, and the dep info is in the module already. `MODULE_DEPENDS` annotation in each .kmd; loader resolves topologically and loads usbcore first.

---

## 2. DMA region — sizing budget

Cumulative allocation across modules (from docs 50-53):

| Source | Item | Size | Lifetime |
|---|---|---:|---|
| uhci.kmd | Frame List (1024 × 4 B) | 4 KB | persistent, 4 KB-aligned |
| uhci.kmd | Interrupt + Control + Bulk QHs | 24 B | persistent |
| uhci.kmd | Per-endpoint QH (~16 EPs) | 128 B | persistent |
| uhci.kmd | TD pool (256 entries × 32 B) | 8 KB | persistent |
| usbcore.kmd | Setup-packet scratch (per-xfer) | 8 B × ~4 | transient |
| usbcore.kmd | Per-device descriptor buffers | ~512 B × N devices | per-enumeration |
| hid.kmd | Keyboard report buffer (8 B) | 8 B × N kbds | persistent per-kbd |
| hid.kmd | Mouse report buffer (3-5 B) | 8 B × N mice | persistent per-mouse |
| hid.kmd | LED output report scratch | 1 B | transient |
| msc.kmd | CBW + CSW scratch | 44 B × N LUNs | persistent per-LUN |
| msc.kmd | INQUIRY response (36 B) | 36 B × N LUNs | transient at probe |
| msc.kmd | REQUEST_SENSE (18 B) | 18 B × N LUNs | transient on error |
| msc.kmd | I/O bounce buffer | **64 KB × N drives** | persistent per-drive |
| (future) ehci.kmd | Periodic + async schedules | ~16 KB | persistent |
| (future) xhci.kmd | Device contexts + scratchpad | ~32 KB | persistent |

**Realistic boot scenario**: 1 UHCI controller + 1 USB keyboard + 1 USB mouse + 1 USB stick = ~12 KB (uhci) + 16 B (hid) + 44 B + 64 KB (msc) ≈ 80 KB.

**Headroom scenario**: 2 EHCI + 4 USB hubs + 8 attached devices (3 sticks, 1 keyboard, 1 mouse, 1 audio, 1 webcam, 1 spare) = ~200 KB.

**xHCI + USB 3 expansion later**: +64 KB for the chip's device contexts + scratchpad pages.

**Decision: reserve 256 KB.** Comfortable for v1; doesn't crowd low memory. Doc 48 §4 said "~2 MB" as a budget — that was forward-thinking with an over-allocation for audio isochronous later. v1 ships smaller.

---

## 3. DMA region — physical placement

UHCI and EHCI both require physical addresses for their pointers. Our identity-mapped DMA region must satisfy:

- **32-bit physical address** — fits in TD/QH pointer fields.
- **Below 4 GB** trivially since pinecore runs on 32-bit hardware; never above.
- **4 KB-aligned** at start (for Frame List + page-table convenience).
- **Contiguous physically** so the entire region is identity-mapped via one fixed PTE range. Doc 51 §4 says virt == phys throughout this region — saves `dma_virt_to_phys` from doing real walks.
- **Not crossing 64 KB page boundaries inside a single TD's buffer** (UHCI footnote constraint per doc 51 §18). msc.kmd's 64 KB pool would *exactly* hit a page boundary; we must allocate the pool aligned and split TDs at the boundary.

### Placement candidates

| Region | Start | Size | Notes |
|---|---|---|---|
| Low conventional (0x00500–0x9FFFF) | — | 638 KB | crowded with BDA / IVT / V86 RM stubs |
| Below kernel base (0x10000–0xA0000) | — | 576 KB | overlaps Pinecone V86 segments (0x1100 = M4TEST per memory s52) |
| Just above kernel `.bss` | dynamic | 256 KB | clean, predictable |
| HMA (0x100000–0x10FFEF) | — | 64 KB | too small |
| Extended (≥1 MB) | 0x00200000+ | 256 KB | cleanest — well above kernel + V86 layout |

**Pick: 0x00200000 (2 MB physical) for 256 KB**, identity-mapped at vmm init.

Rationale:
- 2 MB is well past pinecore's kernel end (currently ~600 KB load + .bss).
- 2 MB is well past V86 task segments (PINE.COM / M4TEST.COM at 0x11000 in s52).
- 4 KB-aligned trivially (it's a 2 MB boundary).
- Doesn't conflict with any future kernel-image growth below 1 MB.
- Single contiguous range — one PTE block.

PMM must mark `[0x200000, 0x240000)` (= 64 pages) as **reserved-for-DMA** at boot, so `pmm_alloc_page()` never hands them out. vmm sets these pages `PRESENT | WRITABLE` (no USER bit — DMA region is kernel-only).

---

## 4. `dma_alloc` / `dma_free` API contract (s53.a)

```c
/* Allocate `size` bytes, aligned to `align` (power of 2).
 * Returns identity-mapped virtual pointer (= physical address).
 * NULL on failure. */
void *dma_alloc(size_t size, size_t align);

/* Free a previously allocated block. Pointer must come from dma_alloc. */
void  dma_free(void *p);

/* Convert dma_alloc'd virtual pointer to physical address.
 * Trivial in our identity-mapped region: returns (uint32_t)p.
 * Provided as an API so the DMA-region implementation can move later
 * without changing every caller. */
uint32_t dma_virt_to_phys(void *p);
```

### Implementation sketch (pmm + slab)

```c
/* 256 KB identity-mapped region at 0x200000. */
#define DMA_REGION_BASE  0x00200000U
#define DMA_REGION_SIZE  (256 * 1024U)

/* Simple bitmap allocator with a 16-byte granularity. */
static uint8_t  dma_bitmap[DMA_REGION_SIZE / 16 / 8];

void dma_init(void) {
    /* PMM: mark all 64 pages reserved. */
    pmm_reserve_range(DMA_REGION_BASE, DMA_REGION_SIZE);
    /* VMM: identity-map RW kernel. */
    vmm_map_range(DMA_REGION_BASE, DMA_REGION_BASE, DMA_REGION_SIZE,
                  PTE_PRESENT | PTE_WRITABLE);
    memset(dma_bitmap, 0, sizeof dma_bitmap);
}

void *dma_alloc(size_t size, size_t align) {
    if (align < 16) align = 16;
    size_t units = (size + 15) / 16;
    size_t step  = align / 16;
    /* Find `units` consecutive free units, with start aligned to `step`. */
    for (size_t i = 0; i < sizeof(dma_bitmap) * 8; i += step) {
        if (bitmap_range_free(i, units)) {
            bitmap_mark_used(i, units);
            return (void*)(DMA_REGION_BASE + i * 16);
        }
    }
    return NULL;
}
```

(Real impl needs free-block-size tracking so `dma_free` knows how much to clear. For v1 a simple sentinel-prefix or a small allocation-table on the side suffices. ~150 LOC.)

---

## 5. IRQ routing

### PCI INTx legacy interrupt routing

UHCI / EHCI / xHCI all assert INTx (legacy interrupts) by default. The PCI Interrupt Line register at offset 0x3C gives the IRQ number — set by BIOS during POST. For pinecore the chain is:

```
PCI device asserts INTA#
  → south bridge → PIC IRQ N (BIOS-programmed, 0x3C reads N)
  → idt.c IRQ N handler
  → registered irq_handler_t with .context = HCD instance
```

s53.a needs:

```c
typedef void (*irq_handler_t)(void *ctx);

int  irq_register(uint8_t irq, irq_handler_t handler, void *ctx);
int  irq_unregister(uint8_t irq, irq_handler_t handler);
void irq_eoi(uint8_t irq);                          /* manual EOI */
```

### Shared IRQs

Two USB controllers on the same PCI segment frequently share an IRQ line. uhci.kmd's IRQ handler **must check USBSTS first** and return early if not its IRQ:

```c
void uhci_irq_handler(void *ctx) {
    uhci_hc_t *hc = ctx;
    uint16_t s = inw(hc->io + 0x02);
    if (s == 0) return;                              /* not us */
    /* ... */
}
```

`irq_register` must chain — multiple handlers per IRQ line, all called in registration order. This is already in the kernel per memory (s50 Path B uses `keyboard_inject_key` from inside an IRQ handler chain).

### Polling fallback for connect/disconnect

Per doc 51 §13: UHCI raises **no** interrupt on port connect/disconnect. uhci.kmd's IRQ handler polls PORTSC on every IRQ (cheap), **plus** a slow PIT-driven poll (~100 ms) catches idle-system hot-plug.

This means s53.a should expose a way to register a periodic callback:

```c
int pit_register_periodic(uint32_t period_ms, void (*cb)(void *), void *ctx);
int pit_unregister_periodic(void (*cb)(void *));
```

uhci.kmd registers a 100 ms callback that walks all known controllers' ports.

---

## 6. Boot-time bring-up walkthrough

When the kernel hands off to user-mode shell prompt, USB is already alive. Sequence:

```
[kernel main.c]
  ├─ pmm_init                                         (s53.a: reserve DMA region)
  ├─ vmm_init                                         (s53.a: identity-map DMA)
  ├─ dma_init                                         (s53.a)
  ├─ pic_init / idt_init
  ├─ pci_init                                         (already in s51)
  ├─ vbe_init / vt_init / serial_init / kbd_init
  ├─ module_loader_init                               (s51, already done)
  │    └─ iterate \DRIVERS\*.KMD, topo-sort by MODULE_DEPENDS, load each
  │       1. usbcore.kmd     → registers nothing globally; exports symbols
  │       2. uhci.kmd        → probes PCI, finds UHCI controllers, calls
  │                            usbcore_register_hcd for each
  │                            registers irq_register(N, uhci_irq, hc)
  │                            registers pit_register_periodic(100, ...)
  │       3. hid.kmd         → calls usbcore_register_class_driver(&hid_drv)
  │       4. msc.kmd         → calls usbcore_register_class_driver(&msc_drv)
  ├─ vt_switch_to(0)
  └─ shell_start                                       prompt appears

[asynchronously, ~50 ms later]
  uhci_irq fires (or PIT poll) on first PORTSC change
  ├─ uhci notes connect, port_reset, speed-detect
  ├─ usbcore_port_connect(hcd, port, speed)
  │    ├─ usbcore enumerates: GET_DESCRIPTOR(Device, 8), SET_ADDRESS,
  │    │                       GET_DESCRIPTOR(Device, 18), GET_DESCRIPTOR(Config),
  │    │                       SET_CONFIGURATION
  │    └─ for each interface: walk class_drivers, call match() then probe()
  │       └─ hid.kmd or msc.kmd takes ownership; opens interrupt/bulk pipes
  └─ keyboard or storage starts working
```

The user types at the prompt — keyboard_inject_key flows from `hid_kbd_complete` → existing keyboard.c queue → DOS INT 16h shim. The user types `dir e:` — INT 13h dispatch lands in msc.kmd's CHS/LBA reader.

---

## 7. The complete kexport list

Synthesized from docs 50-53 §9/§16/§12/§14 (kernel → modules).

### Memory + DMA

```c
void    *kmalloc(size_t size);                                 EXPORT_SYMBOL
void     kfree(void *p);                                       EXPORT_SYMBOL
void    *dma_alloc(size_t size, size_t align);                 EXPORT_SYMBOL
void     dma_free(void *p);                                    EXPORT_SYMBOL
uint32_t dma_virt_to_phys(void *p);                            EXPORT_SYMBOL
```

### Port I/O

```c
uint8_t  inb(uint16_t port);      void outb(uint16_t, uint8_t);  EXPORT_SYMBOL
uint16_t inw(uint16_t port);      void outw(uint16_t, uint16_t); EXPORT_SYMBOL
uint32_t inl(uint16_t port);      void outl(uint16_t, uint32_t); EXPORT_SYMBOL
```

### PCI

```c
uint8_t  pci_cfg_read_byte (pci_bdf_t bdf, uint8_t off);            EXPORT_SYMBOL
uint16_t pci_cfg_read_word (pci_bdf_t bdf, uint8_t off);            EXPORT_SYMBOL
uint32_t pci_cfg_read_dword(pci_bdf_t bdf, uint8_t off);            EXPORT_SYMBOL
void     pci_cfg_write_byte (pci_bdf_t bdf, uint8_t off, uint8_t);  EXPORT_SYMBOL
void     pci_cfg_write_word (pci_bdf_t bdf, uint8_t off, uint16_t); EXPORT_SYMBOL
void     pci_cfg_write_dword(pci_bdf_t bdf, uint8_t off, uint32_t); EXPORT_SYMBOL
int      pci_find_class(uint8_t base, uint8_t sub, uint8_t pi,
                        pci_device_t *out, int index);              EXPORT_SYMBOL
```

### IRQ + timing

```c
int  irq_register(uint8_t irq, irq_handler_t handler, void *ctx);    EXPORT_SYMBOL
int  irq_unregister(uint8_t irq, irq_handler_t handler);             EXPORT_SYMBOL
void irq_eoi(uint8_t irq);                                           EXPORT_SYMBOL
void irq_disable(void);                                              EXPORT_SYMBOL
void irq_enable(void);                                               EXPORT_SYMBOL

void pit_delay_ms(uint32_t ms);                                      EXPORT_SYMBOL
uint64_t pit_ticks_get(void);                                        EXPORT_SYMBOL
int  pit_register_periodic(uint32_t period_ms,
                           void (*cb)(void *), void *ctx);            EXPORT_SYMBOL
int  pit_unregister_periodic(void (*cb)(void *));                    EXPORT_SYMBOL
```

### Logging

```c
void serial_printf(const char *fmt, ...);                           EXPORT_SYMBOL
void vga_printf(const char *fmt, ...);                              EXPORT_SYMBOL
```

### DOS hand-off sinks

```c
/* Keyboard: hid.kmd → kernel INT 16h queue (s50 Path B work) */
void keyboard_inject_key(uint8_t scancode, uint8_t make);                EXPORT_SYMBOL
void keyboard_inject_scancode_sequence(const uint8_t *seq, int n);       EXPORT_SYMBOL

/* Mouse: hid.kmd → kernel INT 33h driver */
void mouse_inject(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);  EXPORT_SYMBOL

/* Storage: msc.kmd → kernel INT 13h dispatcher (doc 48 §4) */
int  int13h_register_disk(int13h_disk_ops_t *ops, void *ctx, uint8_t lun);  EXPORT_SYMBOL
void int13h_unregister_disk(int13h_disk_ops_t *ops);                        EXPORT_SYMBOL
```

### usbcore.kmd → others (re-stated from doc 50 §9)

```c
int  usbcore_register_hcd(usb_hcd_t *hcd);                      EXPORT_SYMBOL_GPL
int  usbcore_unregister_hcd(usb_hcd_t *hcd);                    EXPORT_SYMBOL_GPL
int  usbcore_port_connect(usb_hcd_t *, uint8_t, usb_speed_t);   EXPORT_SYMBOL_GPL
int  usbcore_port_disconnect(usb_hcd_t *, uint8_t);             EXPORT_SYMBOL_GPL

int  usbcore_register_class_driver(usb_class_driver_t *);       EXPORT_SYMBOL_GPL
int  usbcore_unregister_class_driver(usb_class_driver_t *);     EXPORT_SYMBOL_GPL

int  usbcore_control_transfer(usb_device_t *dev,
                              uint8_t bmRequestType, uint8_t bRequest,
                              uint16_t wValue, uint16_t wIndex,
                              void *data, uint16_t wLength,
                              uint32_t timeout_ms);             EXPORT_SYMBOL_GPL
int  usbcore_submit_xfer(usb_device_t *dev, usb_endpoint_t *ep,
                         void *data, uint32_t len, uint32_t timeout_ms,
                         usb_xfer_done_cb_t done, void *ctx);   EXPORT_SYMBOL_GPL
int  usbcore_submit_xfer_sync(usb_device_t *dev, usb_endpoint_t *ep,
                              void *data, uint32_t len,
                              uint32_t timeout_ms);             EXPORT_SYMBOL_GPL

usb_endpoint_t *usbcore_find_endpoint(usb_interface_t *iface,
                                      uint8_t addr);            EXPORT_SYMBOL_GPL
int  usbcore_open_endpoint(usb_device_t *, usb_endpoint_t *);   EXPORT_SYMBOL_GPL
int  usbcore_close_endpoint(usb_device_t *, usb_endpoint_t *);  EXPORT_SYMBOL_GPL

int  usbcore_clear_halt(usb_device_t *, usb_endpoint_t *);      EXPORT_SYMBOL_GPL
int  usbcore_set_interface(usb_device_t *, uint8_t iface, uint8_t alt); EXPORT_SYMBOL_GPL
int  usbcore_get_string(usb_device_t *, uint8_t idx, uint16_t langid,
                        char *buf, size_t buflen);              EXPORT_SYMBOL_GPL
```

**Total: ~50 kernel-side exports + 16 usbcore exports**. The kernel-side count is in the same ballpark as `48-usb-port-plan.md` §4 estimated (30-35); the slight over-count reflects per-byte-size port I/O variants and the keyboard-sequence helper added for HID multi-byte scancodes.

---

## 8. PCORE.CFG additions for USB

The existing `config.c` (s50 Path B + s51 native boot) accepts simple key/value lines. Proposed additions:

```ini
# USB stack policy

# 0 = disable all USB modules; 1 = load on boot (default)
usb_enable = 1

# Reserved DMA region — overrideable for memory-constrained builds.
# Address in hex, size in KB.
usb_dma_base = 0x200000
usb_dma_size = 256

# Per-class enables. Disable msc to save 64KB if no USB storage planned.
usb_hid = 1
usb_msc = 1

# HID keyboard typematic
usb_kbd_initial_delay_ms = 500
usb_kbd_repeat_rate_ms = 33

# Verbose diagnostics over COM1 (slows boot, useful for bring-up only).
usb_trace = 0
```

s53.a `config_*` getters: `config_usb_enabled()`, `config_usb_dma_base()`, `config_usb_dma_size_kb()`, `config_usb_trace_enabled()`, `config_usb_kbd_initial_delay()`, `config_usb_kbd_repeat_rate()`.

---

## 9. Diagnostic surfaces

Per existing kernel discipline (memory `project_panic_infrastructure.md` + `project_build_stamp.md`), USB modules emit:

### Serial COM1 trace

Each module prints at init:
```
[usbcore] v0.1 init: 0 HCDs, 0 class drivers
[uhci] v0.1 init: 2 controllers
[uhci@0x3000] BIOS legacy disarmed (LEGSUP was 0x201F)
[uhci@0x3000] reset OK, 2 ports
[uhci@0x3000] running, IRQ 11
[uhci@0x3020] reset OK, 2 ports
[uhci@0x3020] running, IRQ 11 (shared)
[hid] v0.1 registered class driver: hid_boot
[msc] v0.1 registered class driver: msc_bbb
[uhci@0x3000:p0] connect, FULL speed
[usbcore] enum on uhci@0x3000:p0 → addr 1, VID 046d PID c31c
[usbcore] match: interface 0 (class=3 sub=1 proto=1) → hid_boot
[hid] kbd attached on usbcore.dev1.iface0: protocol=Boot, polling EP 0x81
[uhci@0x3020:p1] connect, FULL speed
[usbcore] enum on uhci@0x3020:p1 → addr 2, VID 058f PID 6387
[usbcore] match: interface 0 (class=8 sub=6 proto=0x50) → msc_bbb
[msc] LUN 0 INQUIRY: Generic  USB Flash Drive  1.00
[msc] LUN 0 CAPACITY: 15728640 LBAs × 512 B = 7.5 GB, removable
[msc] LUN 0 registered as drive E:
```

When `usb_trace = 1`: per-IRQ USBSTS dump, per-IO CBW/CSW headers, sense data on every error.

### Shell commands (future)

Out of scope for s53.a but worth listing now:
- `lsusb` — list all enumerated devices
- `usbtree` — print HCD → port → device tree
- `usbstat` — per-controller stats (IRQs, transfers, errors)
- `umount E:` — eject a USB stick (calls `int13h_unregister_disk` + maybe START_STOP_UNIT)

---

## 10. Verification milestones (ordered)

Per s53 work-stream order:

### s53.a — kernel-side prereqs (1 session)

- [ ] PMM reserves `[0x200000, 0x240000)` as DMA region
- [ ] VMM identity-maps the DMA region kernel-only
- [ ] `dma_alloc` / `dma_free` / `dma_virt_to_phys` implemented + exported
- [ ] `irq_register` chains multiple handlers per IRQ
- [ ] `pit_register_periodic` added (or piggybacked on existing PIT tick)
- [ ] All port-I/O wrappers exported (already exist as inlines; add to `kexports.c`)
- [ ] `int13h_register_disk` + `int13h_unregister_disk` (per doc 48 §4)
- [ ] `keyboard_inject_key` + `keyboard_inject_scancode_sequence` exported (former already done s50)
- [ ] `mouse_inject` exported
- [ ] PCORE.CFG `usb_*` keys parsed
- [ ] Build + boot verify: `lsmod` (if implemented) shows kernel exports

### s53.b — usbcore.kmd (2 sessions)

- [ ] Module skeleton + `MODULE_DEPENDS` nothing
- [ ] `usb_device_t` / `usb_interface_t` / `usb_endpoint_t` allocated
- [ ] `usb_hcd_t` registry — `usbcore_register_hcd` adds to list
- [ ] `usb_class_driver_t` registry — same
- [ ] `usbcore_enumerate_new_device` from doc 50 §3 — the 8-step recipe
- [ ] Standard request helpers (GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION) from doc 50 §4
- [ ] Configuration descriptor parser from doc 50 §6
- [ ] Per-newly-enumerated-device: walk class drivers, dispatch `match()` → `probe()`
- [ ] `usbcore_control_transfer` — sync wrapper around hcd->ops->submit_control
- [ ] `usbcore_clear_halt`, `usbcore_set_interface` convenience wrappers
- [ ] Build + smoke-test with a stub HCD that always returns -ENODEV — verify no crash

### s53.c — uhci.kmd (3 sessions)

- [ ] Module skeleton + `MODULE_DEPENDS("usbcore")`
- [ ] PCI scan via `pci_find_class(0x0C, 0x03, 0x00, ...)` from doc 51 §2
- [ ] `uhci_disarm_legacy` (PCI LEGSUP at 0xC0)
- [ ] `uhci_init` — full bring-up sequence (doc 51 §9)
- [ ] Frame List + 3 QHs (interrupt, control, bulk) allocated + linked
- [ ] `uhci_port_reset` + `uhci_port_status` (doc 51 §10)
- [ ] `uhci_submit_control` — CBW + 3-TD chain (doc 51 §11)
- [ ] `uhci_submit_xfer` — bulk/interrupt (doc 51 §12)
- [ ] `uhci_irq_handler` — USBSTS poll + port-status walk + completion drain (doc 51 §13)
- [ ] Endpoint open/close — per-EP QH (doc 51 §14)
- [ ] `usbcore_register_hcd` called per controller
- [ ] **Smoke test**: boot QEMU with a USB tablet attached, verify enumeration reaches usbcore (even without a class driver matching)

### s53.d — hid.kmd (2 sessions)

- [ ] Module skeleton + `MODULE_DEPENDS("usbcore")`
- [ ] `hid_match` (interfaceClass=3, sub=1, proto∈{1,2}) from doc 52 §2
- [ ] `hid_probe` — SET_PROTOCOL(Boot), SET_IDLE, open EP_IN, submit first xfer (doc 52 §7)
- [ ] `hid_kbd_complete` — modifier diff + array diff + phantom-state guard + scancode emit (doc 52 §8)
- [ ] HID Usage → AT Set 1 lookup table (doc 52 §10)
- [ ] `hid_mouse_complete` — buttons/dx/dy/wheel → `mouse_inject` (doc 52 §9)
- [ ] LED sync via SET_REPORT(Output)
- [ ] Typematic timer (host-side, 500 ms / 33 ms defaults)
- [ ] `hid_disconnect` — cancel in-flight xfer, free priv
- [ ] **Smoke test**: real Vortex86 hardware with USB keyboard, expect keypresses to reach Pinecore Commando prompt

### s53.d' — msc.kmd (2 sessions)

- [ ] Module skeleton + `MODULE_DEPENDS("usbcore")`
- [ ] `msc_match` (class=8, sub=6, proto=0x50)
- [ ] `msc_get_max_lun` (Get Max LUN class request)
- [ ] `msc_probe` — INQUIRY, TEST_UNIT_READY poll, READ_CAPACITY(10), `int13h_register_disk` (doc 53 §10)
- [ ] `msc_xfer` — CBW/data/CSW round trip (doc 53 §10)
- [ ] `msc_reset_recovery` — three control transfers in order (doc 53 §8)
- [ ] SCSI helpers — INQUIRY, READ_CAPACITY, READ(10), WRITE(10), TUR, REQUEST_SENSE (doc 53 §9)
- [ ] INT 13h CHS + LBA paths (doc 53 §11)
- [ ] **Smoke test**: USB stick formatted FAT16 (we already mount FAT in s50/s51); verify `dir E:` returns file list

### s53.e — boot-time iteration + close (1 session)

- [ ] Module loader replaces hardcoded HELLO.KMD with `\DRIVERS\*.KMD` iteration
- [ ] Topological sort by `MODULE_DEPENDS`
- [ ] Update PCBOOT image-builder to stage all four modules (`usbcore.kmd`, `uhci.kmd`, `hid.kmd`, `msc.kmd`)
- [ ] End-to-end: boot Vortex86 from USB stick, USB keyboard and USB stick both visible

**Total estimate: ~10 sessions** to land all four modules + s53.a kernel-side. Doc 48 §3 estimated 12 — reasonable margin for first-real-hardware quirks.

---

## 11. Open architectural decisions

Surfaces issues across all four modules that don't fit in any individual doc.

1. **USB device address space — per-HCD or global?** (doc 50 §12 q1) — **Pick: per-HCD-managed by usbcore** — usbcore owns the address allocator, queries each HCD for capacity. Resolves naturally.

2. **Class probe ordering with multiple interfaces.** A keyboard-with-trackpoint has 2 HID interfaces. usbcore walks interfaces in order, calling match()→probe() on each. Each interface gets its own `iface->driver_priv`. **Resolved.**

3. **What if a device matches both HID and a vendor class?** Composite device with multiple interfaces — first match wins per interface (USB 2.0 §9.7 + Spec convention). usbcore walks class drivers in registration order. Predictable.

4. **Hub class deferral.** The user-facing impact: no intermediate USB hubs work in v1. Root-hub-only. Most Vortex86 boards have ≥4 root ports; OptiPlex 780 has more. Acceptable. `hub.kmd` is a separate later module.

5. **Concurrent HCD presence.** UHCI + EHCI on same board (OptiPlex 780): both probe their PCI devices, both call `usbcore_register_hcd`. usbcore tracks them independently. Port routing between them (the "companion controller" pattern from EHCI spec §4.2) is **EHCI's problem** — EHCI's port-control register includes a "route to companion" bit. v1 ehci.kmd doesn't ship; v1 will see low/full-speed devices on UHCI ports only.

6. **Resource cleanup on disconnect** (docs 50/52/53 all q'd this) — **standard policy**: when usbcore observes disconnect:
   1. Mark device dead immediately (sets `dev->address = 0`).
   2. Iterate device's interfaces; for each with a driver, call `driver->disconnect`.
   3. Class driver cancels in-flight transfers (returns -ENODEV to pending callbacks).
   4. usbcore frees device's endpoints, recycles the address.
   5. HCD frees QHs/TDs (UHCI: unlink QH from chain; xHCI: Disable Slot).

7. **Memory leak on early-failure during enumeration.** If `usbcore_enumerate_new_device` fails at step 6 (after `SET_ADDRESS` succeeded), the device's address is allocated but the device struct may be partially-initialised. usbcore must back out: `SET_ADDRESS(0)` to release the device's address (or just abandon — next port reset reassigns), free the half-built struct.

8. **Class driver auto-unload on no-devices?** Not in v1 — class drivers stay resident even with 0 matching devices. Saves complexity at the cost of ~10 KB per loaded module.

---

## 12. The four research docs as one coherent package

Docs 50-53 each cover one layer; doc 54 binds them. The contracts at each layer boundary:

```
                              ┌──────────────────────┐
                              │   DOS / application   │
                              └──────────┬───────────┘
                                         │ INT 16h, INT 33h, INT 13h
                                         ▼
       ┌─────────────────┐    ┌────────────────────┐    ┌─────────────────┐
       │ kernel-side     │    │   class drivers    │    │ kernel-side     │
       │ inject helpers  │ ◄──┤  hid.kmd (doc 52)  │    │ INT 13h disp.   │
       │ + INT 13h disp. │    │  msc.kmd (doc 53)  ├──► │ + drive table   │
       └─────────────────┘    └─────────┬──────────┘    └─────────────────┘
                                        │ class_driver_t.probe (doc 50)
                                        │ usbcore_control_transfer (doc 50)
                                        │ usbcore_submit_xfer (doc 50)
                                        ▼
                              ┌────────────────────┐
                              │   usbcore.kmd       │
                              │      (doc 50)       │
                              └─────────┬───────────┘
                                        │ usb_hcd_ops_t.submit_* (doc 50)
                                        │ usbcore_port_connect (doc 50)
                                        ▼
                              ┌────────────────────┐
                              │      HCD            │
                              │   uhci.kmd (doc 51) │
                              │      (...ehci/xhci) │
                              └─────────┬───────────┘
                                        │ port-I/O (doc 51)
                                        │ DMA-backed TDs/QHs (doc 51)
                                        ▼
                              ┌────────────────────┐
                              │     hardware        │
                              │  USB controller     │
                              │  + connected devices│
                              └────────────────────┘
```

Every up-arrow on this diagram is covered by a documented contract; every layer can be replaced with a stub for testing without breaking the layers above or below.

---

## 13. Acceptance criteria — doc 54 done

- [x] Module dependency graph documented
- [x] Boot-time load order specified
- [x] DMA region budget per-module + cumulative
- [x] DMA region placement (0x200000, 256 KB) justified
- [x] `dma_alloc` / `dma_free` / `dma_virt_to_phys` API contract
- [x] IRQ routing (PCI INTx + shared-IRQ chain + PIT polling fallback)
- [x] Boot-time bring-up walkthrough end-to-end
- [x] Complete kexport list synthesized (kernel + usbcore)
- [x] PCORE.CFG additions specified
- [x] Diagnostic / trace surfaces described
- [x] s53.a through s53.e implementation milestones with checklists
- [x] Open architectural decisions resolved or noted
- [x] Layer-contract diagram showing the full stack

**End of the doc 50-54 research arc.** Code work can begin at s53.a with no further spec digestion required.

---

## 14. Provenance

- **Sources:** docs 50-53 synthesis + s51 module loader memory + s50 Path B keyboard inject design + doc 48 strategy.
- **No new external specs read** for this doc — it synthesises material from the prior four.
- **Discipline:** spec-first per CONTRIBUTING.md rule #3 + s53 spec-grounding contract.
- **Implementation can begin:** s53.a kernel-side exports + DMA region carving in next session.
