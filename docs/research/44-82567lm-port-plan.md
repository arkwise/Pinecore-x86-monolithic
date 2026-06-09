# 44 — 82567LM-3 packet driver port plan (synthesis + roadmap integration)

Status: research only (no code). Synthesises `41-intel-82567lm-nic.md` (chip), `42-e1000e-linux-driver-map.md` (Linux structure), and `43-packet-driver-spec.md` (DOS-side API) into a phased plan for landing a working packet driver on a Dell OptiPlex 780 — the bring-up vehicle for pinecore-x86's "modern hardware" networking story.

This is **planning**. No code is included or specified. Every directional choice is annotated with risk + verification step.

---

## 0. Executive summary

**Goal:** mTCP `ping`, `dhcp`, `htget` working from a V86 DOS shell on a stock OptiPlex 780, with packets flowing through the 82567LM-3 NIC via a kernel-resident `INT 60h` packet driver.

**Why this matters:** the OptiPlex 780 is one of the cheapest, most available, most reliable hardware platforms with a "modern enough" Intel chipset to be representative of the ICH9/ICH10/PCH-era hardware that defines pinecore's target. No public DOS packet driver exists for the 82567 family. Landing this means pinecore is the **only** way to talk TCP/IP from DOS on this class of hardware. That's the publishable result.

**Approach:** kernel-resident packet driver (not a TSR), because we already own the NIC, the IRQ, the V86 monitor, and the DPMI host. We expose the INT 60h API exactly as a TSR would; applications cannot tell the difference. Internally we skip the RM/PM thunking cost that TSR-based DPMI clients pay.

**Effort:** estimated **~2,000 LOC of original C**, 3-4 weeks calendar at the project's recent velocity (calibrated against FDC bring-up and DPMI host work). The two highest-risk technical items are the **SWFLAG semaphore** (must be perfect or we hang the ME) and **DMA descriptor ring physical-memory management** (affects multiple future drivers, so we get it right once).

**Prerequisites in the project:** Phase 9 (kernel module loader, optional — could ship in-tree first) and the **DPMI 0303h real-mode-callback service**, which the kernel currently lacks. Implementing 0303h is also a prerequisite for any future DPMI-client driver work, so it pays off broadly.

---

## 1. The integration design

### 1.1 Three call paths into our packet driver

Every byte of code we write has to handle all three of these on day one. Two are spec-required; one is a Pinecone-specific simplification.

```
                            kernel-resident INT 60h packet driver
                                            ^
            ┌───────────────────────────────┼──────────────────────────┐
            │                               │                          │
        V86 DOS app                     PM DPMI client            kernel-internal
   (e.g. mTCP in V86)              (e.g. Watt-32-built app)        callers
            │                               │                          │
       INT 60h                       INT 31h AX=0300h               direct call
            │                               │                          │
   V86 monitor traps              DPMI host reflects              (e.g. our own
   → kernel handler               → V86 ↔ kernel short-circuit       diagnostic
                                                                     ping tool)
```

**Path A — V86 INT 60h direct.** A real-mode DOS application (or a 16-bit TSR) calls `INT 60h`. Our V86 monitor catches the `INT` instruction (via the IVT entry pointing into our V86 stub region, see `02-v86-mode.md`), and routes the call to the kernel handler. The handler reads/writes V86 task memory directly using the IVT-known segment registers.

**Path B — DPMI INT 31h AX=0300h reflection.** A protected-mode DPMI client calls `INT 31h` with AX=0300h, BL=0x60. Our existing reflector (per `29-dpmi-host.md` + `39-dpmi-session-35-deepdive.md`) copies the client's RM register block, switches into V86 mode, and executes `INT 60h` there. With our kernel handler intercepting, Path B fans into Path A — the V86 register state is the same, the kernel handler doesn't care which path led there. The DPMI register block round-trips correctly on return.

**Path C — internal kernel call.** Our own kernel code (e.g. an in-kernel ping test, or a TLS handshake module) needs to send/receive packets without going through the V86 trap path. Direct C function calls into the packet driver's API surface; no INT 60h overhead.

**Implication:** the kernel-side packet driver has two interfaces: the **byte-level INT 60h handler** (for paths A + B) and a **C function-pointer API** (for path C). The INT 60h handler is essentially a thin shim that unmarshals registers and calls into the C API.

### 1.2 The upcall (receive callback) flips around

Spec says: when a packet arrives, the driver calls back into the application from the NIC's hardware ISR. Two-call protocol (AX=0 "give buffer", AX=1 "here is data") — see `43-packet-driver-spec.md` §3.

Real TSR drivers: ride the chip's IRQ vector, run on whatever stack happens to be current, fire the upcall from there. Tiny driver, every packet costs RM→PM→RM thunking when the app is in PM.

**Our model:** the NIC's hardware IRQ is routed through our kernel IDT to a kernel-mode ISR (Ring 0, our stack — no size constraint). The ISR drains the RX ring, decodes the Ethernet type field, looks up the matching `access_type` handle, and fires the upcall by **switching into the appropriate execution context**:

- **V86 callback target** → schedule a "synthesise this far call" event on the V86 monitor; effectively a re-entry into the V86 task with CS:IP forced to the callback address, registers set per the two-call protocol. Mechanically the same primitive as IRQ-to-V86 delivery (which we already do for the timer tick).
- **PM callback target** (via the `0303h`-style RMCB the DPMI client allocated) → the kernel synthesises the PM-side frame directly. No real-mode round-trip — we have the PM `CS:EIP` from the 0303h allocation; we save current state, build the PM stack frame, jump there. On the callback's IRETD, restore + return.

This is structurally a major win. A TSR-resident driver under CWSDPMI takes ~50-200µs per upcall round-trip (DPMI 0303h thunk plus reg marshalling). Our kernel-direct path is ~5-10µs because there's no mode switch and no reg marshalling beyond what the spec already demands.

### 1.3 Why this works only because we are the DPMI host

The kernel-direct upcall path **requires** that we know about every PM callback the application has registered. The 0303h "allocate real-mode callback" call is the only spec-blessed way for a PM app to advertise a callback target. If we are the DPMI host (we are — per `29-dpmi-host.md`), we control 0303h: when the app calls it, we record the PM target and hand back our own thunk address. When the packet driver later fires the upcall, we recognise our thunk address and short-circuit.

A TSR loaded **alongside** an unrelated DPMI host has no such visibility — it sees only the thunk address, has to actually call it in real mode, and pays the full thunk cost. We don't have that constraint.

**Architectural reuse:** the same kernel-as-DPMI-host argument that justifies the existing DPMI implementation makes the packet driver dramatically simpler. This is a recurring pattern in pinecore's design — owning the host side of multiple legacy interfaces (DPMI, packet driver, VESA, etc.) is what lets us run modern hardware behind DOS-shaped APIs efficiently.

---

## 2. Memory model — the DMA descriptor ring problem

Bus-master DMA needs physical addresses. Our kernel runs with paging on; the descriptors must contain RAM physical addresses, not our linear/virtual ones. This affects every PCI bus-master device we'll write a driver for (NIC, NVMe, AHCI, USB-via-xHCI, Voodoo command FIFO, HDA buffer descriptors).

### 2.1 Decision: "DMA region" — a reserved low-physical pool

At kernel boot we reserve a contiguous physical region (e.g. 2 MB at physical addresses `0x02000000-0x021FFFFF`, immediately above the kernel identity map per `39-dpmi-session-35-deepdive.md`) and identity-map it. Every device driver allocates DMA-bound buffers from this region. For these buffers, `linear == physical`, so writing physical addresses into descriptor rings is a direct conversion.

Pros:
- Trivial linear→physical conversion (identity).
- Always satisfies bus-master alignment requirements (paragraph-aligned, page-aligned, cache-line-aligned all work).
- Doesn't interact with the DPMI client zone's reserve-vs-commit model (which sits at linear 32 MB-2 GB — see `dpmi.c:DPMI_VADDR_START`).
- Survives reset of the DPMI host or client; lives entirely in kernel control.

Cons:
- Static cap. 2 MB feels generous for one NIC (a 256-descriptor ring × 2 KB buffer = 512 KB; plus TX = 1 MB total) but multiple devices share. May need to grow before we add more drivers.
- Wastes physical RAM that could be DPMI commit-pool. Mitigation: keep this small (start at 2 MB; raise if needed).

Verified-clean: our existing PMM bitmap (`pmm.c`) can carve out this region at boot by pre-marking the pages as allocated; the DPMI commit-cap is computed from the rest. Trivial.

### 2.2 Alternative we rejected: on-demand pinning

Allocate buffers from anywhere in linear space, lock pages, walk PTEs for physical addresses. Works but is more code, more surface for bugs, and we have no real need for it. The "DMA region" approach is the right starting point; we can add on-demand pinning later if a driver legitimately can't fit in the reserved pool.

### 2.3 64-bit DMA — relevant?

The 82567 supports 64-bit DMA addresses (`netdev.c:7386` requests `DMA_BIT_MASK(64)`). Our DMA region lives at physical 32 MB, well under 4 GB; we write the high dword of `RDBAH`/`TDBAH` as zero. No issue.

---

## 3. Phased delivery plan

### Phase A — Prerequisites (1 session, ~2-3 days)

A.1 **DPMI 0303h (Allocate Real Mode Callback Address)** implementation in `dpmi.c`. This is independent of the NIC work and unblocks any future packet-driver receive flow from PM DPMI clients. (~150 LOC)

A.2 **DMA region** carved at boot in `pmm.c` (2 MB at physical 0x02000000). API: `dma_alloc(size, align) → linear=physical`, `dma_free(ptr)`. Identity-mapped in `vmm.c` so any kernel code can read/write it directly. (~80 LOC)

A.3 **Hardware verification on the OptiPlex 780**:
  - Confirm device ID `0x10DE` (or sibling) via PCI scan output.
  - Read PCI config 0x3C — capture the assigned IRQ line for the LAN device.
  - Time a single SWFLAG acquire (cheap test → no AMT contention; slow → full dance required).
  - Read PHY ID via direct MDIC; verify `0x01410CB0`.
  - Read MAC address via EERD; spot-check against the sticker.

These five reads should be a 50-line diagnostic tool, not a driver. Run once, log results, decide whether the SWFLAG dance is overhead-only or load-bearing.

**Gate:** A.1 + A.2 land in kernel; A.3 produces a known-good hardware-fact sheet for the rest of the work.

---

### Phase B — Driver skeleton & init (1-2 sessions, ~4-6 days)

B.1 **PCI bind**. Add 82567LM-3 device IDs to our PCI driver registry (`src/kernel/pci.c`, see `20-pci-bus.md`). On match: capture BAR0 (MMIO MAC CSRs), BAR1 (flash window — informational), IRQ line.

B.2 **MMIO mapping**. Identity-map BAR0 into kernel address space via `vmm_map_page`. Confirm we can read `STATUS` register and get a sane value (link state, speed).

B.3 **Reset sequence**. Implement the chip reset recipe from `41-intel-82567lm-nic.md` §5. Mirror Linux's `e1000_reset_hw_ich8lan` algorithmically:
   - Disable IRQs via IMC.
   - Disable RX/TX rings.
   - SWFLAG acquire (the algorithm from `ich8lan.c:1757-1810`, transliterated to C without copy).
   - `CTRL.RST`, wait, verify clear.
   - `CTRL_EXT.DRV_LOAD`.
   - PHY reset.
   - SWFLAG release.

B.4 **MAC address read**. NVM via EERD (from `41` §7). Write into `RAL(0)`/`RAH(0)`. Skip checksum validation initially.

B.5 **SWFLAG primitive** as its own helper. Bracket every PHY operation. Strict pairing — make leaks impossible by audit.

B.6 **PHY ID verification**. Read MII regs 2-3 via MDIC, confirm `0x01410CB0`.

B.7 **Diagnostic command in our shell**: `e1000 status` — prints chip state, link, speed, MAC, ring head/tail. Useful throughout the rest of bring-up.

**Gate:** kernel boots → e1000 status shows healthy chip, MAC address read OK, link state visible.

**Risk:** SWFLAG implementation correctness. Mitigation: dedicated test session reading and writing PHY ID under SWFLAG repeatedly; verify board doesn't hang and FWSM reads sane values. If anything is unstable, debug here before moving on.

---

### Phase C — RX path (1-2 sessions, ~4-6 days)

C.1 **RX descriptor ring** allocation in DMA region. 256 descriptors × 16 B = 4 KB. RX buffer pool = 256 × 2048 B = 512 KB. All in the DMA region; addresses are both linear and physical.

C.2 **Configure RX**: write `RDBAL(0)`/`RDBAH(0)`/`RDLEN(0)`. Pre-fill descriptors with buffer physical addresses. Write `RDH=0`, `RDT=N-1`. Enable RX ring via `RXDCTL(0)`. Set `RCTL = EN | BAM | (BSIZE 2048)`.

C.3 **Bus master enable** in PCI command register.

C.4 **Polling RX test**. Before plumbing interrupts, poll `RDH` from a kernel diagnostic command. Send broadcast traffic to the OptiPlex from another machine. Verify descriptors get marked `DD=1` and packet bytes appear in our buffers. **This is the first end-to-end DMA confirmation.**

C.5 **IRQ ISR**. Install handler at IDT vector matching the BIOS-assigned PCI IRQ line. Skeleton: read `ICR`, bail if zero, process bits. For now: log the ICR value, ack via 8259 EOI, return.

C.6 **RX drain on `ICR.RXT0`**. Walk descriptors with `DD=1`, log Ethernet header + length, recycle the descriptor by clearing status and bumping `RDT`. Still no upcall — just kernel logs.

**Gate:** kernel boots, runs `ifconfig eth0 up`-equivalent, accepts broadcast traffic, logs valid Ethernet frames.

**Risk:** descriptor format alignment / endianness / physical address truncation. Mitigation: ring layout has been the wear point of every e1000 port ever — verify field by field against `41` §4, then verify with a hexdump of received bytes vs ethertool output from another machine sending known packets.

---

### Phase D — TX path (1 session, ~2-3 days)

D.1 **TX descriptor ring** allocation similarly. 64 descriptors × 16 B is plenty.

D.2 **Configure TX**: `TDBAL`/`TDBAH`/`TDLEN`, `TDH=TDT=0`, enable ring via `TXDCTL`, `TCTL = EN | PSP | …`.

D.3 **Send-one-packet kernel test command**: `e1000 send <bytes>` builds a 64-byte frame, places it in a DMA region buffer, fills a TX descriptor with `CMD = EOP | IFCS | RS`, writes `TDT=1`. Verify with Wireshark on the receiving end.

D.4 **TX completion** harvest. When `ICR.TXDW` fires, walk descriptors with `STA.DD=1`, recycle buffers.

**Gate:** `e1000 send` produces packets visible on the wire.

---

### Phase E — Packet driver API surface (1-2 sessions, ~4-6 days)

E.1 **Install handler at INT 60h**. The kernel writes a real-mode entry point into the IVT slot for INT 60h that, when invoked from V86, traps to our kernel dispatcher. The handler region also contains the 8-byte signature `'PKT DRVR' '\0'` at the documented offset 3 (per `43` §2.2).

E.2 **Function dispatcher**. AH-indexed. Implement:
  - `driver_info` (1)
  - `access_type` (2)
  - `release_type` (3)
  - `send_pkt` (4)
  - `terminate` (5) — return CANT_TERMINATE; we're the kernel
  - `get_address` (6)
  - `reset_interface` (7) — return success (no-op)
  - `get_parameters` (10)
  - `as_send_pkt` (11)
  - `set_rcv_mode` (20)
  - `get_rcv_mode` (21)
  - `set_multicast_list` (22) — initially CANT_SET
  - `get_statistics` (24)
  - `set_address` (25) — initially CANT_SET

E.3 **Handle table**: small static array (16 entries) of `{ if_type, callback_v86_cs_ip, callback_pm_target_or_null }`. `access_type` allocates, `release_type` frees, all paths look up by handle.

E.4 **RX type demultiplex**. When a packet arrives, scan the handle table for matching Ethernet type (or wildcard handle). For matching handle, queue the buffer in a per-handle ring for upcall delivery.

E.5 **Upcall dispatch — V86 path**. Synthesise the two-call protocol by injecting CS:IP into the V86 task and resuming. First call with AX=0, app returns buffer in ES:DI. Second call with AX=1, copy bytes, return.

E.6 **Upcall dispatch — PM path**. Detect that the registered callback is one of our 0303h thunks. Bypass the thunk; jump directly to the PM target with synthesised register state.

**Gate:** mTCP's `dhcp.exe` (running in V86) runs successfully — gets an IP lease from the LAN's DHCP server. This is the smallest test that exercises function 1, 2 (×3 — IP+ARP+broadcast), 4, the upcall, and the type demux.

**Risk:** the two-call upcall protocol is fiddly and apps assume specific register preservation. Mitigation: implement the V86 path first against an mTCP V86 binary; once it works, port the algorithm to the PM path.

---

### Phase F — DPMI integration polish (1 session, ~2-3 days)

F.1 **0300h vendor short-circuit**. When a PM client calls INT 31h AX=0300h, BL=0x60, the host detects our kernel handler and routes the call without a V86 trip. (Minor optimisation but cleans up the trace.)

F.2 **0303h target tagging**. When a PM client allocates an RMCB and the thunk is later registered as a packet driver callback, mark the thunk in our table so the upcall dispatcher knows to use the PM short-circuit path.

F.3 **Watt-32 compatibility test**. DJGPP-built Watt-32 app — `ping.exe` or similar — running in PM, talking to the packet driver via 0300h+0303h. Verify full round-trip.

**Gate:** mTCP (V86), Watt-32 (PM via 0303h short-circuit) both work for ping + DHCP + a small HTTP fetch.

---

### Phase G — Errata + edge cases (1 session, ~2 days)

G.1 **No jumbo on 82567 -3**. Per `41` §10.1: never set `RCTL.LPE`. Cap `get_parameters.MTU` at 1500.

G.2 **MDIC retry for FW contention**. Per `41` §10.2: retry MDIC reads 3× on garbled data.

G.3 **ITR throttling**. Set to ~65 µs minimum interval to cap interrupt rate under load.

G.4 **Shared-IRQ chain check**. ISR exits early if `ICR==0` so we play nicely with USB or other devices on the same legacy IRQ line.

G.5 **Link change handling**. On `ICR.LSC`, re-read STATUS, update internal state, log "Link 1000Mbps Full Duplex" or similar.

**Gate:** all known errata covered; mTCP + Watt-32 stability tests run for 1 hour with no driver-side hang.

---

### Phase H — Acceptance + documentation (1 session, ~1-2 days)

H.1 **Acceptance tests**:
  - `mTCP dhcp` lease succeeds in V86.
  - `mTCP ping` to LAN router succeeds for 1000 packets, 0% loss.
  - `mTCP htget` of a 10 MB file completes intact (md5sum match).
  - `mTCP ftp` of same.
  - Watt-32 `pingw.exe` from PM completes 100 packets.
  - `PKTSTAT.COM` reads function-24 stats without crashing.

H.2 **`docs/research/45-82567lm-implementation-journal.md`** — empirical findings, bug log, what worked, what surprised us. Standard pinecore session-style writeup.

H.3 **Update CHANGELOG, SESSION-STATE, FILE-STATUS** per CONTRIBUTING.md.

**Gate:** acceptance tests pass on real OptiPlex 780 hardware (not just emulated).

---

## 4. Roadmap integration

The work fits cleanly between existing Phase 10 ("Networking + Packet Driver Shim", which defines the generic Crynwr-API kernel layer) and Phase 11 ("Modern Hardware Drivers", which lists the specific chips we want native drivers for). I propose adding:

- A new bullet under Phase 10 calling out the **DPMI 0303h prerequisite** explicitly (currently implicit in "DPMI integration").
- A new bullet under Phase 11 → Networking listing **82567LM-3 (OptiPlex 780)** as a bring-up vehicle ahead of the I219/I225/I226 line.
- A new **Phase 10.3 — 82567LM bring-up** sub-phase (or Phase 11.1, depending on dependency framing) that breaks the work into the seven phases above.

A patch for `roadmap.md` accompanies this doc.

---

## 5. Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|-----------|--------|-----------|
| 1 | SWFLAG dance wrong → ME hang → board needs power cycle | Low (well-specced) | High | Phase B.5 dedicated test; never exit a function holding SWFLAG. |
| 2 | DMA descriptor format mismatch (endianness, alignment) | Medium | High (silent corruption) | Phase C.4 polled-RX test before plumbing IRQs; hexdump verify. |
| 3 | Upcall protocol mismatch — app gets registers wrong | Medium | High (silent app crash) | Phase E.5 use mTCP DHCP first (most-stressed code path); compare register state vs. trace from real NE2000 driver. |
| 4 | IRQ sharing with USB/other → spurious | Medium | Low (drop the IRQ) | Phase C.5 ICR=0 fast bail; verify. |
| 5 | NVM checksum mismatch on stock board | Low | Low | Skip validation initially; read MAC + RAR-program anyway. |
| 6 | OptiPlex 780 ships with 82567V-3 not LM-3 on some sub-SKUs | Medium | Low | Bind both device IDs (`0x10DE` and `0x10CB`); algorithm is identical. |
| 7 | DPMI 0303h adds latent bugs (RMCB stack issues) | Medium | Medium | Implement A.1 with isolated test client before any packet-driver work. |
| 8 | Interrupt routing wrong (wrong IRQ from PCI cfg 0x3C) | Low | Medium | Phase A.3 captures live value; ISR is wired to that, not a constant. |
| 9 | TX bus-master conflict if PCI cmd reg not set | Low | High (TX silent fail) | Phase D explicitly enables bus master before first `TDT` write. |
| 10 | Multi-V86-task contention (DOOM trying to use NIC while shell holds it) | Low | Medium | Single-handle policy initially: first-come, others get NO_SPACE. Address later. |

---

## 6. What's explicitly out of scope (for this port)

- **Jumbo frames** — erratum + niche use case.
- **TSO / IPv4/TCP checksum offload** — not used by mTCP/Watt-32.
- **Multi-queue RX/TX** — 82567 only has one real queue.
- **MSI / MSI-X** — legacy INTx works.
- **VLAN filtering** — apps build VLANs in software if needed.
- **WoL** — we're running, not sleeping.
- **EEE** — not on ICH10.
- **PTP timestamping** — not needed.
- **iAMT manageability** — we coexist via SWFLAG only.
- **Set MAC address from app** — refuse with CANT_SET.

If a future demand requires any of these, the chip supports them and the driver framework can be extended; this scope is deliberately the minimum that lights up the headline use case.

---

## 7. Tooling & test environment

### 7.1 Bring-up rig
- One OptiPlex 780 (the bring-up machine).
- One Ethernet switch + a second machine with `tcpdump` (the "tester").
- Serial cable from OptiPlex COM1 → tester for logging.
- A FreeDOS bootable USB stick with a reference packet driver for that NIC class (NE2000 in QEMU on the tester for protocol-level comparison), plus mTCP utilities.

### 7.2 In-emulator pre-flight
- QEMU can emulate an **e1000** NIC. The QEMU "e1000" is actually 82540 / 82544 silicon — close enough algorithmically to test descriptor ring layout, packet handling, and upcall flow before touching real hardware. Use to nail Phases C-F before exposing to the OptiPlex.
- DPMI host work (Phase A.1, 0303h) can be entirely tested in QEMU.

### 7.3 Logs to capture
- Per-IRQ: ICR value, RX descriptors drained, TX descriptors harvested.
- Per upcall: handle, type, length, app callback target.
- Per SWFLAG cycle: acquire time (ms), release.

These get added to our serial-log discipline; the existing `grep` patterns in SESSION-STATE.md generalise.

---

## 8. Open questions to resolve before code

1. **Should the 82567LM driver live in-tree or as a kernel module (Phase 9)?** The module loader doesn't exist yet. In-tree is faster to ship; revisit modularisation in Phase 9 once we have it. → **In-tree** for v0.1.
2. **Do we need 0303h, or can the kernel-direct path replace it entirely?** Even with the short-circuit, applications spec-conformantly call 0303h. We implement it so we look like a normal DPMI host. → **Yes**, implement 0303h.
3. **DMA region size?** 2 MB is fine for one NIC. NVMe + xHCI later need more. Plan for growth. → **Start 2 MB, grow as drivers arrive.**
4. **What's the "kernel ping" diagnostic?** Useful to have a kernel-internal `e1000 ping <ip>` that exercises the driver without an external app. Build alongside Phase H.
5. **Do we ship a binary `PKTSCAN.COM` for the apps to detect us, or rely on existing tools?** mTCP scans automatically; nothing extra needed.

---

## 9. References

### In this repo
- `41-intel-82567lm-nic.md` — chip register surface
- `42-e1000e-linux-driver-map.md` — Linux driver structure
- `43-packet-driver-spec.md` — Crynwr API
- `20-pci-bus.md` — PCI enumeration
- `29-dpmi-host.md` — our DPMI host
- `31-dpmi-specification.md` — INT 31h reference
- `39-dpmi-session-35-deepdive.md` — current host state
- `22-rtl8139-nic.md`, `23-ne2000-nic.md` — prior chip-level NIC research

### External
- Linux v6.6 e1000e (local: `/Users/chelsonaitcheson/Projects/linux-e1000e-ref/`)
- mTCP: <https://www.brutman.com/mTCP/>
- Watt-32: <https://github.com/gvanem/Watt-32>
- Crynwr packet drivers: <https://github.com/fragglet/crynwr_mirror>
- E1000PKT (closest existing Intel-NIC DOS driver, doesn't support 82567): <https://github.com/ulrich-hansen/E1000PKT>
