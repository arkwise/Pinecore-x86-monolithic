# 55 — TinyUSB host architecture (cross-read against pinecore usbcore)

Status: research only (no code). Cross-reference pass before EHCI/OHCI/xHCI .kmd work commits us to current ABI shape.

Companion docs:
- `50-usb-enumeration-walkthrough.md` — pinecore usbcore design we're comparing against
- `54-usbcore-env-synthesis.md` — current kexport surface
- `48-usb-port-plan.md` — strategy
- `56-hub-class-walkthrough.md` — full hub class drill-down (companion, just landed)
- `57-ohci-spec-derivation.md` — OHCI 1.0a function-by-function (companion, just landed)

Citation format: `(tinyusb: src/host/usbh.c:NNN)` parallel to existing `(USBDDOS: HCD/uhci.c:NNN)` style in docs 45-54. Pinecore citations are `(pinecore: <path>:<line>)`.

---

## 1. Why this cross-read

We have three independent USB host stacks of interest:

| Stack | License | Lineage | Role here |
|---|---|---|---|
| TinyUSB | MIT (tinyusb: src/host/usbh.h:1) | clean-room embedded stack | **free to study and selectively adopt patterns** |
| USBDDOS | GPLv2 | FreeDOS USB stack | spec sanity-check (doc 46), citation-only since `feedback_spec_first_when_reference_source_is_present` |
| Linux drivers/usb/ | GPLv2 | reference HCD canon | spec sanity-check only; same constraints as USBDDOS |

TinyUSB is the only one of the three whose API shape is **safe to copy ideas from** under the licensing rules already governing the project. Even so, we still write spec-first per CONTRIBUTING.md rule #3 — this doc surfaces shape questions, not copy-paste candidates.

The deeper question the cross-read answers: **before we write `ehci.kmd` (~3 sessions per doc 48), `ohci.kmd` (~3 sessions per doc 57), and eventually `xhci.kmd` (~5+ sessions), is our `usb_hcd_ops_t` vtable (pinecore: src/include/usbcore.h:218-230) the right shape?** Re-shaping that ABI later means re-touching uhci.kmd. Better to find the gaps now.

TinyUSB was chosen specifically because it has the same scope as us — a single-host-image USB stack that runs natively on small systems without an underlying OS — and ships **production HCDs for four chips** (DWC2, MAX3421, FSDev, RP2040). The vtable that survived four port targets has revealed corner cases ours has not yet hit.

We are **not** considering forking TinyUSB. We compare API shapes, not code.

### Scope of this comparison

In scope:
- Top-level host API and enumeration state machine
- HCD vtable surface (functions HCDs implement, events HCDs raise)
- Class driver registration and dispatch
- Transfer submission semantics (sync vs async, callbacks)
- DMA / bounce-buffer model

Out of scope:
- Device-side stack (TinyUSB ships dual host+device; we only build host)
- Build-time configuration mechanisms (TinyUSB's `CFG_TUH_*` macros)
- RTOS abstraction layer (`osal_*`)
- Per-chip HCD details beyond the one glue we read

---

## 2. TinyUSB host stack architectural overview

Concrete file inventory (sizes from the GitHub `master` snapshot read for this doc):

| Layer | File | Lines | Role |
|---|---|---:|---|
| Public host API | `src/host/usbh.h` | 563 | application-facing entry points + callbacks |
| Internal host API | `src/host/usbh_pvt.h` | 108 | host↔class-driver contract |
| Host stack core | `src/host/usbh.c` | 2089 | enumeration state machine + transfer plumbing |
| HCD interface | `src/host/hcd.h` | 283 | HCD↔core contract (the vtable analogue) |
| Hub class | `src/host/hub.c` | 756 | hub class driver (companion doc 56) |
| HID class | `src/class/hid/hid_host.c` | 853 | HID class driver |
| MSC class | `src/class/msc/msc_host.c` | 822 | MSC class driver |
| One HCD example | `src/portable/synopsys/dwc2/hcd_dwc2.c` | 1253 | full DWC2 port |
| Types | `src/common/tusb_types.h` | 937 | descriptors, enums, request codes |

```
              ┌─────────────────────────────────────────┐
              │           application                    │
              │   tuh_mount_cb, tuh_hid_report_received  │
              └───────────────┬──────────────────────────┘
                              │ tuh_control_xfer, tuh_edpt_xfer,
                              │ tuh_hid_receive_report, ...
                              ▼
              ┌─────────────────────────────────────────┐
              │     usbh.c — host stack core             │
              │   enum_new_device                        │
              │       (tinyusb: src/host/usbh.c:1824)    │
              │   process_enumeration state machine      │
              │       (tinyusb: src/host/usbh.c:1837)    │
              │   tu_bind_driver_to_ep_itf               │
              │       (tinyusb: src/host/usbh.c:2125)    │
              └────────┬─────────────────────┬───────────┘
                       │                     │
              usbh_class_driver_t            hcd_* ops (vtable
              .open/.set_config/             via direct calls — no
              .xfer_cb/.close                struct-of-fn-ptrs)
                       │                     │
              ┌────────▼─────────┐  ┌────────▼──────────┐
              │   class drivers  │  │   HCD ports        │
              │  hub_*, hidh_*,  │  │  hcd_dwc2,         │
              │  msch_*, cdch_*  │  │  hcd_rp2040,       │
              │  ...             │  │  hcd_max3421,      │
              │                  │  │  hcd_fsdev         │
              └──────────────────┘  └──────────┬─────────┘
                       ▲                       │
                       │ hcd_event_xfer_complete│
                       │ hcd_event_device_attach│
                       │ hcd_event_handler      │
                       └───────────────────────┘
```

Key shape observation: **TinyUSB has two distinct contracts** going down from the core — a struct-of-function-pointers (`usbh_class_driver_t`, tinyusb: src/host/usbh_pvt.h:38-44) for class drivers, but **direct C calls** to a fixed `hcd_*` ABI for HCDs. Only **one HCD per build** (compile-time selected). This is the embedded-stack assumption — TinyUSB does not expect to host multiple HCDs in one image.

Pinecore is the opposite: we expect a board to have several controllers at once (Vortex86 = 1 UHCI; OptiPlex 780 = UHCI + EHCI; future = xHCI), and our `.kmd` loader must compose them at runtime. That is the load-bearing reason our HCD interface (pinecore: src/include/usbcore.h:218-238) is a `usb_hcd_ops_t` vtable while TinyUSB's is a direct call.

We are right on this one and TinyUSB's approach does not generalize for our system. Documented; not changing.

---

## 3. HCD abstraction — vtable surfaces compared

TinyUSB's HCD contract is a fixed set of `hcd_*` C functions every port implements (tinyusb: src/host/hcd.h:100-157). Side-by-side:

| Concern | TinyUSB function | Pinecore equivalent | Notes |
|---|---|---|---|
| Init controller | `hcd_init(rhport, rh_init)` (src/host/hcd.h:103) | per-module `*_module_init()` + `pci_find_class` → `usbcore_register_hcd` | TinyUSB takes a single int port id; ours takes a fully-formed `usb_hcd_t*` |
| Deinit | `hcd_deinit(rhport)` (src/host/hcd.h:106) | module exit + `usbcore_unregister_hcd` | — |
| Interrupts | `hcd_int_handler(rhport, in_isr)` (src/host/hcd.h:109) | `irq_register(irq, handler, ctx)` callback | TinyUSB caller decides ISR context; we always run from ISR |
| Frame counter | `hcd_frame_number(rhport)` (src/host/hcd.h:118) | — **(no equivalent)** | needed for isoc scheduling — gap for future ISOC |
| Root-hub port query | `hcd_port_connect_status(rhport)` (src/host/hcd.h:124) | `ops->port_status(hcd, port, *status)` (pinecore: src/include/usbcore.h:227) | shape match |
| Port reset | `hcd_port_reset(rhport)` + `hcd_port_reset_end(rhport)` (src/host/hcd.h:127-130) | `ops->port_reset(hcd, port)` (single call, blocking) (pinecore: src/include/usbcore.h:226) | TinyUSB splits reset into "start" + "end" so the core can sequence the 10 ms wait; we do it inside the HCD |
| Speed query | `hcd_port_speed_get(rhport)` (src/host/hcd.h:133) | returned by `port_reset` as part of status | functionally equivalent |
| Device teardown | `hcd_device_close(rhport, daddr)` (src/host/hcd.h:136) | — **(no per-device teardown call)** | gap — we rely on `ep_close` for each endpoint individually |
| Open endpoint | `hcd_edpt_open(rhport, daddr, ep_desc)` (src/host/hcd.h:142) | `ops->ep_open(hcd, dev, ep)` (pinecore: src/include/usbcore.h:222) | shape match; ours passes the parsed `usb_endpoint_t*` |
| Close endpoint | `hcd_edpt_close(rhport, daddr, ep_addr)` (src/host/hcd.h:145) | `ops->ep_close(hcd, dev, ep)` (pinecore: src/include/usbcore.h:224) | shape match |
| Generic transfer | `hcd_edpt_xfer(rhport, daddr, ep_addr, buf, buflen)` (src/host/hcd.h:148) | `ops->submit_xfer(hcd, xfer)` (pinecore: src/include/usbcore.h:221) | TinyUSB has no Setup-vs-Data distinction in this op; setup is separate |
| Abort transfer | `hcd_edpt_abort_xfer(rhport, daddr, ep_addr)` (src/host/hcd.h:151) | — **(no abort op)** | gap — relevant for disconnect + class-driver close |
| Setup packet | `hcd_setup_send(rhport, daddr, setup8)` (src/host/hcd.h:154) | `ops->submit_control(hcd, xfer)` covers the full 3-stage (pinecore: src/include/usbcore.h:220) | structural split — see §6 |
| Clear stall | `hcd_edpt_clear_stall(rhport, daddr, ep_addr)` (src/host/hcd.h:157) | host-side via `usbcore_clear_halt` standard-request (pinecore: src/modules/usbcore.c:181) | TinyUSB makes HCD reset data-toggle hardware as part of clear-stall; we currently only send the CLEAR_FEATURE control request and assume HCD honours toggle reset on next ep open. **HCD-state gap.** |
| Set address | — (TinyUSB sends the SET_ADDRESS via the normal control path) | `ops->set_address(hcd, dev, addr)` (pinecore: src/include/usbcore.h:229) | we kept this because xHCI's Address-Device command is a queue-of-commands operation, not a Setup packet — same reason TinyUSB's xHCI port would also need a special path |

### Upward events (HCD → core)

TinyUSB has a strict event-driven upward path:

| TinyUSB event | Purpose |
|---|---|
| `hcd_event_device_attach(rhport, in_isr)` | root-hub port saw connect |
| `hcd_event_device_remove(rhport, in_isr)` | root-hub port saw disconnect |
| `hcd_event_xfer_complete(daddr, ep_addr, xferred_bytes, result, in_isr)` (tinyusb: src/portable/synopsys/dwc2/hcd_dwc2.c:1097) | a queued transfer finished |
| `hcd_event_handler(event, in_isr)` | generic dispatcher used by hubs too (tinyusb: src/host/hub.c:614) |

All four take an `in_isr` boolean so the core knows whether it must defer to task context.

**Pinecore equivalent**:

- For root-hub events, uhci.kmd calls `usbcore_port_connect(hcd, port, speed)` / `usbcore_port_disconnect(hcd, port)` directly (pinecore: src/modules/uhci.c uses these; pinecore: src/modules/usbcore.c:468 + 475).
- For transfer completion, the synchronous control path returns directly from `submit_control` (pinecore: src/modules/uhci.c:258), and the async bulk/interrupt path calls the caller's `xfer->done` callback from IRQ context (pinecore: src/modules/usbcore.c:551, hid uses this at src/modules/hid.c:199 + 235).
- We have **no `in_isr` flag** in our path. Class-driver callbacks for async transfers run unconditionally in IRQ context. This works because pinecore is single-threaded with no scheduler under us. TinyUSB sometimes runs above FreeRTOS — they have to track ISR-vs-task to know whether to take a mutex or grab the scheduler lock.

**Verdict on §3**: our shape matches well, plus we win on multi-HCD support, minus we lack abort + per-device-close + frame-counter. Adopt those three gaps as future-work items in §9.

---

## 4. Enumeration state machine — compared to doc 50 §3

Our enumeration (pinecore: src/modules/usbcore.c:340-424) is a **straight-line synchronous function**: GET_DESCRIPTOR(8) → SET_ADDRESS → 2 ms delay → GET_DESCRIPTOR(18) → GET_DESCRIPTOR(CONFIG, 9) → GET_DESCRIPTOR(CONFIG, total) → SET_CONFIGURATION → probe class drivers. Each control transfer is a `submit_control` call that blocks the caller (which is the IRQ-fired connect handler) until done or timeout.

TinyUSB's enumeration (tinyusb: src/host/usbh.c:1824-2170) is an **explicit asynchronous state machine** with named stages:

```
ENUM_RESET → ENUM_ADDR0_DEVICE_DESC → ENUM_SET_ADDR
  → ENUM_AFTER_SET_ADDRESS_RECOVERY_DELAY
  → ENUM_GET_DEVICE_DESC → ENUM_GET_9_BYTE_CONFIG_DESC
  → ENUM_GET_FULL_CONFIG_DESC → ENUM_SET_CONFIG
  → ENUM_CONFIG_DRIVER (loops per interface)
  → enum_full_complete(true/false)
```

Each transition is triggered by the completion callback of the previous control transfer. The 2 ms post-SET_ADDRESS recovery (USB 2.0 §9.2.6.3, p.246) is enforced by `enum_delay_async()` (tinyusb: src/host/usbh.c:1964) — a delayed transition rather than a blocking `pit_delay_ms`.

### Tradeoffs

| Axis | Pinecore (sync) | TinyUSB (async FSM) |
|---|---|---|
| Implementation complexity | low — straight-line C with goto-fail | high — explicit state enum, dispatch table, recoverable transitions |
| IRQ context occupancy | enumeration ties up the IRQ handler for the entire ~50-100 ms enumeration time | each transition is short; IRQ returns between stages |
| Concurrent enumeration | one device at a time per HCD | multiple devices on different ports can interleave |
| Error recovery | goto-fail rolls back the whole device | per-stage retry up to `USBH_CONTROL_RETRY_MAX = 3` (tinyusb: src/host/usbh.c:1467) |
| Suitability for RTOS | bad — blocks scheduler | designed for cooperative task loop |

For pinecore's current target (single-CPU, no preemption above the IRQ layer, ≤2 USB devices visible at boot), the sync approach is fine. The synchronous design is also what made `pit_delay_ms(2)` (pinecore: src/modules/usbcore.c:370) a one-liner; TinyUSB needs 30 LOC of FSM plumbing to express the same delay.

But: per `project_v86mt_milestones`, we're moving towards a pre-emptive desktop. The day a long-running V86 task tries to do work while a USB stick is being plugged in is the day the sync enumeration becomes a problem (the IRQ that fires `usbcore_port_connect` holds the CPU for ~100 ms with interrupts disabled-ish).

**Recommend**: keep the sync FSM for v1; document the upper bound on per-port-connect IRQ occupancy (≤150 ms with current timeouts); revisit when M7+ (real preemptive scheduling above kernel-tasks) is in flight. Add it to §9 as a deferred adopt.

### Detail: the retry behaviour

TinyUSB retries failed control transfers up to 3 times before giving up the device (tinyusb: src/host/usbh.c:1462-1467, condition: "if device remains connected and failed count below threshold"). Pinecore retries zero times — any short read or timeout fails the entire enumeration and we `device_free` (pinecore: src/modules/usbcore.c:421-424).

The 3-retry policy is **directly motivated by real-world device flakiness** — many cheap USB sticks and keyboards stall their first GET_DESCRIPTOR after a port reset. iPXE does the same, and so does Linux usbcore. We should adopt this. Quick win.

---

## 5. Class driver registration — vtable comparison

Pinecore:

```c
typedef struct usb_class_driver {
    const char *name;
    int  (*match)     (usb_interface_t *iface);
    int  (*probe)     (usb_device_t *dev, usb_interface_t *iface);
    void (*disconnect)(usb_device_t *dev, usb_interface_t *iface);
} usb_class_driver_t;
```
(pinecore: src/include/usbcore.h:240-245)

TinyUSB:

```c
typedef struct {
    char const* name;
    bool     (*const init)      (void);
    bool     (*const deinit)    (void);
    uint16_t (*const open)      (uint8_t rhport, uint8_t dev_addr, ...);
    bool     (*const set_config)(uint8_t dev_addr, uint8_t itf_num);
    bool     (*const xfer_cb)   (uint8_t dev_addr, uint8_t ep_addr, ...);
    void     (*const close)     (uint8_t dev_addr);
} usbh_class_driver_t;
```
(tinyusb: src/host/usbh_pvt.h:38-44)

### Per-field comparison

| Concept | Pinecore | TinyUSB | Comment |
|---|---|---|---|
| Driver name | `name` | `name` | identical |
| Driver-lifecycle init | — (module init does it) | `init()` / `deinit()` | TinyUSB separates "driver registered" from "module loaded". Our model collapses them; module init does both. |
| Match decision | `match(iface)` returns int 0/1 | `open(rhport, dev_addr, desc, max_len)` returns 0 (no claim) or claimed byte count | TinyUSB **combines** match-and-claim into a single op. Returning >0 means "I claimed N bytes of the config descriptor stream, walk past them and try the next interface" |
| Per-device-per-interface setup | `probe(dev, iface)` | `set_config(dev_addr, itf_num)` | shape match; ours splits match from setup, TinyUSB combines |
| Transfer completion | — (we use per-submission `done` callback) | `xfer_cb(dev_addr, ep_addr, result, xferred_bytes)` | TinyUSB has a single per-driver entry point for **all** transfers on **all** endpoints owned by this driver. We attach a `done` cb to each `usbcore_submit_xfer` call (pinecore: src/modules/hid.c:255). |
| Disconnect | `disconnect(dev, iface)` | `close(dev_addr)` | TinyUSB closes per-device, we close per-interface — composite-device-with-mixed-classes (HID + audio) would split better in our model |

### The interesting differences

**(a) Combined match+claim.** TinyUSB's `open()` walking the descriptor stream byte-by-byte is a direct adaptation for composite devices whose interface association descriptors (IADs) bundle several interfaces under one driver claim (e.g. CDC-ACM = control interface + data interface, audio + UAC = multiple interfaces). The driver returns "I claimed 3 interfaces worth of descriptor bytes" and the enumerator skips past them.

Pinecore's per-interface `match()` cannot express IAD bundling. v1 doesn't ship anything that needs it (HID, MSC, hub are all single-interface). For future audio/networking classes that **do** use IADs (e.g. CDC-NCM, UVC), our model would need extension. Not urgent.

**(b) Single per-driver `xfer_cb`.** TinyUSB tracks `(dev_addr, ep_addr) → driver`, and when a transfer completes, the core looks up the owning driver and calls its `xfer_cb`. The driver disambiguates by `ep_addr`.

Pinecore tracks `(xfer → done_cb)` per-submission. The class driver pre-binds different completion functions per endpoint at submission time (pinecore: src/modules/hid.c:254-256, where kbd vs mouse picks a different `done` cb).

Tradeoff: TinyUSB's model is simpler (one cb per driver), but the driver must demux. Ours is more flexible (per-xfer cb) but allocates a cb pointer per submission. For our v1 with a small number of endpoints per driver, ours is fine.

**(c) Class-driver init/deinit hooks.** TinyUSB lets a class driver allocate its global state at registration time and free it at deregistration. Pinecore conflates this with module init/exit because **a class driver = a .kmd module = a single bundle of (driver, state, init, exit)** in our world. This matches Linux's `module_init`/`module_exit`. We're idiomatic.

**Verdict**: our vtable is fine for v1 + v2. The IAD gap is a future-class problem; flag it in §9.

---

## 6. Transfer submission + completion

### Setup packet handling

TinyUSB **splits the control transfer** at the HCD boundary:
1. Core calls `hcd_setup_send(rhport, daddr, setup8)` (tinyusb: src/host/hcd.h:154 + src/host/usbh.c:1318) — HCD queues the 8-byte setup
2. Core later calls `hcd_edpt_xfer(rhport, daddr, ep_addr=0x00 or 0x80, buf, len)` for the data stage
3. Core calls `hcd_edpt_xfer` again with len=0 in opposite direction for the status stage

This is because **xHCI's Transfer Ring requires explicit Setup, Data, Status TRBs in sequence** — the HCD cannot bundle them into one host-side call without giving up control over each TRB's IOC bit. EHCI's QTD chain has the same property. UHCI doesn't care (we can build the 3-TD chain ourselves and submit at once).

Pinecore bundles the whole control transfer into one `submit_control(hcd, xfer)` call (pinecore: src/include/usbcore.h:220, pinecore: src/modules/uhci.c:258). The UHCI implementation builds Setup-TD + Data-TD chain + Status-TD chain internally (pinecore: src/modules/uhci.c:287-321).

This works for UHCI. For EHCI / xHCI, **we will have to expose lower-level control over the per-stage submission, or accept that the HCD internally splits one `submit_control` into three xHCI commands and waits on the third's interrupt**.

The lazy option (HCD splits internally) is workable but ties us to synchronous-on-the-stack waiting throughout, even for xHCI which is otherwise fully async. The TinyUSB-style split keeps the door open for async control later.

**Recommendation**: keep the bundled `submit_control` for v1 (UHCI works fine); when writing ehci.kmd, **add an optional `submit_setup` / `submit_xfer` pair to the vtable** that the HCD can implement in addition to `submit_control`. usbcore.kmd's `usbcore_control_transfer` keeps its bundled signature; internally it uses split ops if present, falls back to `submit_control` otherwise. This is the smallest forward-compatible change.

### Sync vs async

| Operation | Pinecore | TinyUSB |
|---|---|---|
| `usbcore_control_transfer` | synchronous (blocks until xfer completes or timeout) (pinecore: src/modules/usbcore.c:112-137) | `tuh_control_xfer()` is **async by default**; a sync wrapper (`TU_API_SYNC`, tinyusb: src/host/usbh.h:353-475) blocks via a state-poll loop |
| `usbcore_submit_xfer` | async — caller passes `done(xfer, ctx)` (pinecore: src/include/usbcore.h:282-285) | `tuh_edpt_xfer()` async (tinyusb: src/host/usbh.h:229) |
| Bulk read | async + done cb | async + cb |
| INT polling | async, re-submitted from cb | async, re-submitted from cb (`tuh_hid_receive_report`, tinyusb: src/class/hid/hid_host.c:498) |

We diverge on control: ours is fundamentally sync. This is fine on our side because we have no scheduler that needs the CPU back during the ~5 ms a control transfer takes. **Don't change.**

### Error reporting

| Path | Pinecore | TinyUSB |
|---|---|---|
| Errno encoding | `USB_E*` negative ints (pinecore: src/include/usbcore.h:69-76) | `xfer_result_t` enum (XFER_RESULT_SUCCESS/FAILED/STALLED) |
| Timeout | per-xfer `timeout_ms` field, HCD enforces (pinecore: src/include/usbcore.h:212) | per-call timeout; if 0, no timeout (tinyusb: src/host/usbh.h:159) |
| STALL vs error | currently collapsed to USB_EIO | distinct `XFER_RESULT_STALLED` |

**Adopt**: distinguish STALL from generic I/O error. STALL is recoverable (CLEAR_FEATURE(ENDPOINT_HALT) plus toggle reset); generic error usually isn't. Worth a USB_ESTALL = -42 added to the errno table. Small, no ABI break.

---

## 7. Memory and DMA model

Pinecore's contract is fixed by `project_hcd_bounce_buffer_contract`:

> Every HCD must bounce-buffer caller data through `dma_alloc`; caller buffers in kernel stacks/heap/.bss are outside the DMA region and `dma_virt_to_phys` returns 0 → HC DMAs over IVT at phys 0.

This is enforced by uhci.kmd at the entry to `submit_control` (pinecore: src/modules/uhci.c:267-281): allocate a bounce buffer in the DMA region, `memcpy` the caller's payload in, hand the phys addr to the HC, copy back out after. The same happens for `submit_xfer` (pinecore: src/modules/uhci.c:501-545).

TinyUSB has no such contract. From DWC2 (tinyusb: src/portable/synopsys/dwc2/hcd_dwc2.c:728-730):

```
channel->hcdma = (uint32_t) edpt->buffer;
hcd_dcache_clean(edpt->buffer, edpt->buflen);
```

The HCD takes the application's buffer pointer **directly** as the DMA address, then cleans the data cache before the transfer and invalidates after (tinyusb: src/portable/synopsys/dwc2/hcd_dwc2.c:1087-1089). This works because TinyUSB runs on systems where:
- the physical address space is a 1:1 mapping of virtual (typical MCU)
- the only DMA-coherence concern is the data cache
- the application is expected to provide aligned, cache-line-clean buffers (documented contract)

Pinecore runs on:
- i386 with paging — virt != phys for most regions; only our explicit `[0x200000, 0x240000)` region is identity-mapped (doc 54 §3)
- no data cache flush primitive exposed to drivers (we have `wbinvd` available but don't use it — i386 host snoop is implicit in our `inb/outb` model)
- a callsite (class driver) that allocates buffers on the kernel stack, in module .bss, or in `kmalloc`'d heap — **all of which are outside the DMA region**

The bounce buffer is therefore a **system-architecture necessity for pinecore**, not a TinyUSB-style nice-to-have. Documented in `project_hcd_bounce_buffer_contract`; right answer.

### One thing TinyUSB does that we should consider

TinyUSB pre-allocates **static per-channel data structures** at compile time (DWC2: `_hcd_data` global). No runtime `dma_alloc` happens during transfer submission — only application buffer addresses change. This is the embedded reflex: avoid dynamic allocation in the hot path.

Pinecore currently allocates a fresh per-xfer QH + setup buffer + bounce in `uhci_submit_control` (pinecore: src/modules/uhci.c:272-281) and frees them on completion (pinecore: src/modules/uhci.c:363-381). For ~10 transfers/sec during enumeration this is fine; for 1000 bulk transfers/sec on a USB-stick read it is a lot of `dma_alloc` churn. ehci.kmd / xhci.kmd should consider pre-allocating per-endpoint QH/QTD pools at `ep_open` time and reusing them — TinyUSB's model. This is an **internal HCD optimization** and does not touch the ABI.

---

## 8. Hub handling — defer to doc 56

Briefly:
- TinyUSB ships hub as a regular class driver (tinyusb: src/host/hub.c, 756 lines, vtable at `hidh_*` analog — `hub_init` :201, `hub_open` :207, `hub_set_config` :305, `hub_xfer_cb` :476, `hub_close` :294)
- The interrupt-endpoint status pipe is polled in `hub_edpt_status_xfer()` (tinyusb: src/host/hub.c:287); completion at `hub_xfer_cb()` (tinyusb: src/host/hub.c:476) walks the status bitmap and triggers per-port handlers
- Hub-driven child enumeration: the hub class driver calls `hcd_event_handler()` with `HCD_EVENT_DEVICE_ATTACH` (tinyusb: src/host/hub.c:614) — **the same event the root-hub HCD raises for a root-port connect**, so the enumeration core does not care whether the connect came from a root port or behind a hub. Architecturally elegant.

Pinecore implication: when we add `hub.kmd`, we should preserve this invariant — `usbcore_port_connect(hcd, port, speed)` is the single entry point for "a device just attached", whether `hcd` is the root or a synthetic "hub HCD" wrapping the hub's port. Companion doc 56 covers the hub class spec walk in depth.

---

## 9. What we should adopt / reject / defer

Concrete recommendations, each with "Why:" and "Where touched:".

### Adopt (small, low-risk, do soon)

**A1. Per-control-transfer retry up to N (≈3).**
- Why: cheap USB devices STALL or short-read their first descriptor read post-reset; TinyUSB and Linux both do this, USBDDOS doesn't and is famous for "didn't enumerate" flake. (tinyusb: src/host/usbh.c:1462-1467)
- Where touched: pinecore: src/modules/usbcore.c `enumerate_new_device` — wrap each control transfer in a retry loop.
- Cost: ~20 LOC. No ABI change.

**A2. Distinguish STALL from generic I/O error.**
- Why: STALL is recoverable (CLEAR_FEATURE), generic isn't. Class drivers need to know which. (tinyusb returns `XFER_RESULT_STALLED`)
- Where touched: pinecore: src/include/usbcore.h add `#define USB_ESTALL -42`; pinecore: src/modules/uhci.c return USB_ESTALL when TD STATUS shows Stalled (bit 22 of status word).
- Cost: ~10 LOC. ABI add only; no breakage.

**A3. Add an in-IRQ-context flag to async completion callbacks.**
- Why: even today our completion cb runs from IRQ ctx, but neither the call signature nor docs say so explicitly. When we add a real scheduler (M6+ kernel preemption), HID's `keyboard_inject_scancode_sequence` from cb context will need to differentiate. TinyUSB's `in_isr` boolean (tinyusb: src/host/hcd.h:109 + hcd_event_xfer_complete) is the right shape.
- Where touched: pinecore: src/include/usbcore.h `usb_xfer_done_cb_t` add a flag arg (or a `flags` field on `usb_xfer_t`).
- Cost: ABI break of one typedef. Touches hid.c x 2 callbacks. **Do now, before USB stack stabilizes for downstream consumers.**

### Adopt (medium, defer to next-driver session)

**A4. Add `hcd_event_xfer_complete`-style **single completion entry** as an alternative to per-xfer `done` cb.**
- Why: when xhci.kmd lands, the Event Ring dequeue happens in a single IRQ handler with no per-TRB context; binding back to per-xfer cb requires a lookup table the HCD must maintain anyway. A core-side `usbcore_xfer_complete(dev, ep, status, actual)` exported function gives xHCI a place to land completions without per-xfer state on the HCD side.
- Where touched: pinecore: src/include/usbcore.h add the export; pinecore: src/modules/usbcore.c implement (looks up pending xfer by `(dev_addr, ep_addr)`, calls its done cb).
- Cost: ~40 LOC. Keeps the per-xfer `done` cb model; adds a second path the HCD can choose.

**A5. Split control transfer into Setup + Data + Status for HCDs that need it.**
- Why: ehci.kmd will need it for QTD-chain control; xhci.kmd will need it for Transfer-Ring TRB-by-TRB control. UHCI doesn't need it.
- Where touched: pinecore: src/include/usbcore.h add optional `submit_setup` + `submit_data` ops to `usb_hcd_ops_t`; pinecore: src/modules/usbcore.c `usbcore_control_transfer` prefers the split path if available, falls back to bundled `submit_control`.
- Cost: ~80 LOC. **Schedule alongside ehci.kmd in s53.usb.ehci.**

**A6. Add `port_reset_end` distinction.**
- Why: TinyUSB splits port reset into "start" and "end" so the core can sequence the mandatory 10 ms wait (USB 2.0 §7.1.7.5). Our current `port_reset` blocks for the wait inside the HCD, holding the IRQ context for 10 ms. With the splitter, the core can yield in between.
- Where touched: pinecore: src/include/usbcore.h split `port_reset` into two; pinecore: src/modules/uhci.c follow.
- Cost: ~20 LOC + one extra round-trip per port reset (probably negligible).

### Reject (don't adopt)

**R1. TinyUSB's single-HCD-per-build assumption.** Our `usb_hcd_ops_t` vtable is the right shape for multi-HCD-at-runtime systems. Stay.

**R2. Direct C-call HCD ABI.** Same reason as R1 — runtime composition is a hard requirement for us.

**R3. TinyUSB's `xfer_cb` single-callback-per-driver model.** Our per-xfer `done` callback gives class drivers a cleaner per-endpoint demux story. Stay.

**R4. Compile-time `CFG_TUH_*` configuration macros.** Not idiomatic for a runtime-loadable-module system; PCORE.CFG keys (doc 54 §8) are the right shape.

**R5. TinyUSB's app-callback-driven enum-descriptor hooks (`tuh_enum_descriptor_device_cb`).** They let the app override descriptors mid-enumeration; we have no need (no end-user descriptor mangling use-case in scope).

### Defer (genuinely later, but write down so we don't forget)

**D1. Async enumeration FSM.** Worth re-evaluating once kernel preemption (M6+) is real. Until then, sync enum keeps the code small and the only cost is ~150 ms of IRQ occupancy per port connect, which is invisible to end users.

**D2. Per-device close op.** TinyUSB's `hcd_device_close(rhport, daddr)` gives the HCD a single place to free per-device resources. We currently `ep_close` each endpoint individually, which works but scatters cleanup. Add when xhci.kmd lands (xHCI has per-Device-Slot teardown that benefits from this).

**D3. Abort transfer op.** TinyUSB's `hcd_edpt_abort_xfer` is necessary for clean class-driver-close paths under high IRQ load. Add when we have real users disconnecting USB sticks mid-write.

**D4. IAD (Interface Association Descriptor) support.** Required for CDC-NCM, UVC, UAC composite devices. Not on the v1 or v2 roadmap; flag for whenever audio enters the picture.

**D5. Per-HCD pre-allocated transfer pools.** TinyUSB's static `_hcd_data` style. For ehci.kmd / xhci.kmd write-from-scratch, design transfers around pre-allocated QH/QTD pools at `ep_open`. Not an ABI change — pure HCD-internal optimization.

**D6. Frame number export.** TinyUSB has `hcd_frame_number(rhport)`. Required for isochronous scheduling. Add when ISOC lands (UAC).

---

## 10. Open questions for follow-up

1. **A3's ABI break — what's the migration plan for the in-IRQ flag?** Easiest is to change `usb_xfer_done_cb_t` from `int (*)(usb_xfer_t*, void*)` to `int (*)(usb_xfer_t*, void*, uint32_t flags)`. That touches hid.c twice. Acceptable but should land in one commit. **Confirm: do we want to do A3 in s53.usb.cleanup before ohci.kmd, or hold it until ehci.kmd's session anyway?**

2. **For A5 (split control), should `submit_setup` take just 8 bytes (TinyUSB style) or a full `usb_xfer_t*` with `setup` populated and `len=0`?** The latter keeps the HCD signature smaller (one ops slot per primitive type, not two), and lets us reuse the existing xfer-completion plumbing. **Lean towards reuse; needs design confirmation when ehci.kmd starts.**

3. **For D2 (per-device close), the contract for "any in-flight transfer when device closes" — does the HCD silently complete them with `USB_ENODEV`, or does the core walk the pending xfer table first?** TinyUSB walks first then calls `hcd_device_close`. Either works for us; pick at write time.

4. **Is `usbcore_register_class_driver`'s probe-against-already-enumerated-devices behaviour (pinecore: src/modules/usbcore.c:506-516) something TinyUSB does too?** Not obviously — TinyUSB drivers are compile-time-registered, so "register driver while devices already present" isn't a flow. We took the right call here for hot-load via .kmd. No action.

5. **Should we expose a `usbcore_event` enum (CONNECT, DISCONNECT, XFER_COMPLETE, PORT_OVERCURRENT) for class drivers to subscribe to?** TinyUSB's `tuh_event_hook_cb_t` (tinyusb: src/host/usbh.h:125) gives the app a hook. We don't have a use case yet (no `lsusb` shell command, no per-event GUI). **Defer; revisit when shell + GUI land.**

6. **Cache coherence — do we need any flush primitive in pinecore at all?** i386 with snoop-coherent PCI bus mastering should make this a no-op. But Vortex86 + future ARM/RISC-V ports won't necessarily be snoop-coherent. **Document the current assumption: pinecore-x86 relies on bus-snoop; any non-x86 port must add `dma_sync_for_device` / `dma_sync_for_cpu` to the kexport surface.**

---

## 11. Acceptance criteria — doc 55 done

- [x] License-fit framing in §1 — MIT TinyUSB OK to study, GPLv2 USBDDOS spec-citation-only
- [x] Architectural overview diagram in §2 with file:line anchors for every layer
- [x] HCD vtable side-by-side in §3, gap analysis identified (3 missing ops)
- [x] Enumeration state machine compared in §4, retry behaviour called out
- [x] Class driver vtable compared in §5, IAD gap noted
- [x] Sync vs async transfer model in §6, error encoding gap noted
- [x] DMA model contrasted in §7, bounce-buffer-as-necessity reaffirmed
- [x] Hub callout to doc 56 in §8
- [x] Three-category recommendations in §9: 3 adopt-now, 3 adopt-medium, 5 reject, 6 defer
- [x] 6 open questions surfaced for human review in §10
- [x] No code changes proposed — every recommendation is "should we change X" framed for human approval

---

## 12. Provenance

- **Primary TinyUSB sources read** (GitHub `master` snapshot at fetch time):
  - `src/host/usbh.h` (563 lines)
  - `src/host/usbh_pvt.h` (108 lines)
  - `src/host/usbh.c` (2089 lines — enumeration core read for the FSM)
  - `src/host/hcd.h` (283 lines — full HCD vtable)
  - `src/host/hub.c` (756 lines — for §8 callout)
  - `src/class/hid/hid_host.c` (853 lines)
  - `src/class/msc/msc_host.c` (822 lines)
  - `src/portable/synopsys/dwc2/hcd_dwc2.c` (1253 lines — one HCD port end-to-end)
  - `src/common/tusb_types.h` (937 lines)
- **Pinecore sources read**: `src/include/usbcore.h`, `src/modules/usbcore.c`, `src/modules/uhci.c` (skim), `src/modules/hid.c` (skim).
- **Discipline:** comparison-only; per CONTRIBUTING.md rule #3 no code adopted. Every recommendation in §9 is a "should we" framed for human review, not a code patch. License MIT on TinyUSB makes selective adoption legal; we still write from spec per `feedback_spec_first_when_reference_source_is_present`.
- **No new external specs read** for this doc — it cross-references doc 50 (USB 2.0 §9 enumeration), doc 54 (env synthesis), and the TinyUSB public surface.
- **Citation parity:** `(tinyusb: src/host/usbh.c:NNN)` for upstream and `(pinecore: <path>:<line>)` for ours.

---

## 13. What to read next in this pack

- **doc 56** — full hub class spec walkthrough (USB 2.0 §11), the only TinyUSB class driver whose semantics are not adequately covered by doc 50.
- **doc 57** — OHCI 1.0a function-by-function (doc 51-style derivation), the next HCD to ship after uhci.kmd.
- **doc 58** — EHCI 1.0 from spec, queued for the s53.usb.ehci session.

A3 (in-IRQ flag) and A1 (per-control retry) are the cheapest two recommendations from §9 to land; consider a `s53.usb.cleanup` micro-session before starting ohci.kmd so the ABI is settled before the next HCD writes against it.
