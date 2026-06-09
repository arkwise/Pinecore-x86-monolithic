# 43 — Crynwr / FTP Software Packet Driver Specification + Real-World Driver Survey

Status: research only (no code). Target consumer: the 82567LM-3 bring-up effort and the kernel/V86 packet-driver integration story for pinecore-x86.

Related docs already in the tree:
- `22-rtl8139-nic.md` (RTL8139 chip-level)
- `23-ne2000-nic.md` (NE2000 / 8390 chip-level)
- `20-pci-bus.md` (enumeration / BAR mapping)
- `29-dpmi-host.md`, `31-dpmi-specification.md`, `39-dpmi-session-35-deepdive.md` (host architecture, INT 31h reflection)

---

## 0. TL;DR for the integration question

The Packet Driver spec is small (one INT vector, ~15-20 functions, one upcall callback). The hard part is **not** the API surface — it is the **upcall**:

- Every received packet causes the driver to call back into the application twice from inside the NIC's hardware ISR: once to request a buffer, once to deliver the packet.
- That callback target was supplied by the application as a real-mode `CS:IP`.
- DOS-extender clients (DJGPP / DOS4G / DOS/32A) running in protected mode have to publish a real-mode-callable shim that re-enters PM to deliver the packet. The two practical mechanisms are:
  - DPMI INT 31h AX=0303h ("Allocate Real Mode Callback Address") — universal, slow.
  - DOS4G/W's `asmpkt4.asm` upcall — proprietary to the DOS4G family, faster, used by Watt-32.

For pinecore-x86, where we are the DPMI host *and* the kernel, we have a third option: install our kernel-resident packet driver at INT 60h, have V86 DOS apps call it via INT 60h directly, have PM DPMI clients reach it via INT 31h AX=0300h reflection (which we already implement), and have our kernel synthesise the upcall as either (a) a normal V86 far-call into the DOS app's real-mode handler, or (b) a 0303h-style real-mode-callback back through DPMI. The kernel owns the NIC and the IRQ, so we never have to ride a real-mode TSR's ISR.

This document captures everything needed to commit to that design.

---

## 1. Spec history and where the canonical text lives

The Packet Driver Specification was authored at FTP Software, Inc., starting in 1986 (Crynwr PD Spec rev 1.09, §1; corroborated [PC/TCP Packet Driver — Wikipedia](https://en.wikipedia.org/wiki/PC/TCP_Packet_Driver)). The version that became "the spec" is **rev 1.09, dated 14 Sep 1989**; rev 1.10 added the look-ahead variant of the upcall; rev 1.11 is the last revision that appears in the wild. The PC/TCP doc was placed in the public domain and reproduced on Crynwr's site as `packet_driver.html` (Crynwr Software, http://crynwr.com/packet_driver.html). Mirrors exist on ibiblio (http://www.ibiblio.org/towfiq/packet-d.html) and in `network_calls.txt` (http://sininenankka.dy.fi/leetos/network_calls.txt).

Primary-source caveat: the canonical Crynwr HTML wasn't directly fetched from this environment during research. All quotations below are from search-tool excerpts of that page and its mirrors; the **register-level details should be re-verified against `packet_driver.html` or a local copy of `PACKET_D.109` before any code is written**. This is flagged again in "Open Questions" at the end.

The original Russ Nelson driver collection (skeleton + per-NIC sources) lives at `crynwr.com/drivers/` and is mirrored on GitHub at https://github.com/fragglet/crynwr_mirror — that mirror is the practical source-of-truth for the implementation patterns surveyed in §5.

---

## 2. The contract — function-by-function reference (Class 1, Ethernet)

### 2.1 Calling conventions

- The driver installs a handler at one of the software interrupt vectors **0x60–0x80** (inclusive of 0x80 in some texts, 0x7F in others). Drivers do not pick a fixed vector; the user gives the driver a vector number on the command line (e.g. `NE2000 0x60 IRQ=10 IOADDR=0x300`).
- An application *discovers* the driver by scanning vectors 0x60..0x80 looking for the signature string.
- Function selection is by `AH` register. Most functions take a handle in `BX`.
- Errors are signalled by setting **CF=1**, with an error code in **DH** (Crynwr PD Spec rev 1.09, §"Error returns"; see also §4 of `network_calls.txt`).
- Success returns CF=0. Function-specific data comes back in registers detailed per function.
- The driver must preserve all segment registers other than those it explicitly returns. Applications generally save/restore everything around the INT call anyway.

### 2.2 The signature — "PKT DRVR" at offset 3

From the Crynwr PD Spec (§"Driver detection", quoted in the Wikipedia page and Crynwr text):

> "The handler for the interrupt starts with 3 bytes of executable code (either a 3-byte jump instruction, or a 2-byte jump followed by a NOP), followed by the null-terminated ASCII text string `PKT DRVR`."

So the byte layout at `[interrupt_vector_seg : interrupt_vector_off + 3]` is the 8 bytes `'P','K','T',' ','D','R','V','R'` followed by `0x00`. An application detector reads the vector via INT 21h AX=3500h..3580h, follows the far pointer, skips 3 bytes, and `memcmp`s 9 bytes. (Crynwr PD Spec rev 1.09, §"PKT DRVR signature"; reaffirmed in http://crynwr.com/packet_driver.html excerpt.)

### 2.3 Class numbers (Crynwr PD Spec rev 1.09, §"driver_info")

Triplet `<class, type, number>`. Classes (relevant subset):

| Class | Medium |
|-------|--------|
| 1     | DIX Ethernet / IEEE 802.3 Ethernet |
| 2     | ProNET-10 |
| 3     | IEEE 802.5 Token Ring |
| 4     | Omninet |
| 5     | Appletalk |
| 6     | Serial Line (SLIP) |
| ...   | (a dozen more, irrelevant here) |

For our use only Class 1 matters. The spec text emphasises that applications **must** check class+type from `driver_info()` because addressing semantics differ across classes (Crynwr PD Spec rev 1.09, §"Class field semantics"; cited at http://crynwr.com/packet_driver.html).

### 2.4 Function-by-function

Numbers below are `AH` values. Where there's direct register-level confirmation, the source is the Crynwr spec text excerpt; where the spec is fuzzy in the excerpts available, items are labelled "this is conjecture" or "needs verification against `PACKET_D.109`".

#### Function 1 — `driver_info`

- Returns the driver's class, type, number, name, function level, and basic version.
- Input: `AH=1`, `BX=handle` (or `BX=0xFFFF` for "any handle" per some implementations).
- Output (CF=0):
  - `BX` = version (e.g. `0x010C` for v1.12 of the driver, not the spec)
  - `CH` = class (1 for Ethernet)
  - `DX` = type (NE2000=1, 3C501=4, etc — Crynwr maintains the list)
  - `CL` = number (per-instance, 0 for first instance)
  - `DS:SI` = pointer to driver's null-terminated name string
  - `AL` = basic/extended functionality level (1 = basic, 2 = basic+extended, 5 = high-perf with `as_send_pkt`, etc).
- Errors: `BAD_HANDLE` if the handle does not refer to this driver. (Crynwr PD Spec rev 1.09, §"driver_info"; http://crynwr.com/packet_driver.html.)

#### Function 2 — `access_type`

The most important function. Registers an upcall for a particular Ethernet type-field value.

- Input:
  - `AH=2`
  - `AL` = if_class (1 for Ethernet)
  - `BX` = if_type (0xFFFF = any type for this class)
  - `DL` = if_number (0xFF = any)
  - `DS:SI` = pointer to type bytes (e.g. the 2 bytes `0x08 0x00` for IPv4, `0x08 0x06` for ARP)
  - `CX` = type length (typically 2 for Ethernet)
  - `ES:DI` = far pointer to receiver callback (the "upcall handler")
- Output (CF=0): `AX` = handle (16-bit, opaque to application).
- Errors: `NO_CLASS`, `NO_TYPE`, `NO_NUMBER`, `BAD_TYPE`, `NO_SPACE`, `TYPE_INUSE`.
- The handle is what the driver stores in its internal type-demux table. When a packet of that type arrives the driver invokes `ES:DI` (the application's handler) with this handle in `BX`. (Crynwr PD Spec rev 1.09, §"access_type"; "PC/TCP will normally make 5 access_type() calls for IP, ARP and 3 kinds of Berkeley Trailer encapsulation packets" — https://en.wikipedia.org/wiki/PC/TCP_Packet_Driver.)
- A "type" of length 0 means "match anything" — used by `tcpdump`-style sniffers. The driver then has to be in promiscuous mode (see §8).

#### Function 3 — `release_type`

- Input: `AH=3`, `BX=handle`.
- Output: CF=0 on success.
- Errors: `BAD_HANDLE`.
- Removes the demux entry and frees the handle.

#### Function 4 — `send_pkt`

The synchronous transmit primitive.

- Input:
  - `AH=4`
  - `DS:SI` = far pointer to packet buffer (must contain full MAC header — dest MAC, src MAC, type — followed by payload)
  - `CX` = packet length in bytes (must be >= 60 typically — driver may pad if smaller, depends on chip)
- Output: CF=0 on success. The buffer may be reused after return.
- Errors: `CANT_SEND`, `BAD_COMMAND`.
- Semantics: the driver may copy the buffer into its own transmit FIFO/ring synchronously, or wait until the chip says "TX done", before returning. Either way, after CF-clear return, the application owns the buffer again. ("The application must supply the entire packet, including local network headers" — Crynwr spec excerpt.)

#### Function 5 — `terminate`

- Input: `AH=5`, `BX=handle`.
- Output: CF=0 on success.
- Errors: `CANT_TERMINATE`, `BAD_HANDLE`.
- Asks the driver to unhook itself from INT 60h, restore the previous handler, free its IRQ, and exit (un-TSR). Most drivers refuse if any handles other than the caller's are still active.

#### Function 6 — `get_address`

- Input: `AH=6`, `BX=handle`, `ES:DI` = output buffer, `CX` = buffer length (>=6 for Ethernet).
- Output (CF=0): `CX` = length actually written (6); buffer filled with the 6-byte MAC.
- Errors: `BAD_HANDLE`, `NO_SPACE`.

#### Function 7 — `reset_interface`

- Input: `AH=7`, `BX=handle`.
- Output: CF=0 on success.
- Semantics: ask the driver to reset the NIC. Few drivers implement this fully; some do a no-op and return success.

#### Function 10 (0x0A) — `get_parameters`

- Extended-class function (level >= 2).
- Input: `AH=10`.
- Output: `ES:DI` = pointer to `struct PktParameters` containing: major rev, minor rev, length of buffer (i.e. address length, 6), addr_len, MTU, multicast aval, rcv_bufs, xmt_bufs, int_num.

#### Function 11 (0x0B) — `as_send_pkt`

The asynchronous transmit; added in spec rev 1.09 itself.

- Input:
  - `AH=11`
  - `DS:SI` = buffer, `CX` = length, `ES:DI` = upcall function ("call when buffer is safe to reuse")
- Output: CF=0 on success; the call returns *before* TX completes.
- Behavior: "the upcall routine is called when the application's data has been copied out of the buffer, allowing the application to safely modify or re-use the buffer. The driver may pass a non-zero error code to the upcall if the copy failed... otherwise it should indicate success, even if the packet hasn't actually been transmitted yet." (Crynwr PD Spec rev 1.09, §"as_send_pkt", excerpt from search results.)
- Important: TX-completion upcalls happen at IRQ time (TX-done interrupt) and have the same re-entrancy constraints as the RX upcall (§3).

#### Function 14 (0x0E) — `set_handle` / "high-performance" group

The 1.10/1.11 high-performance API extends `access_type` with batched-handle semantics. Register-level coverage of function 14 is incomplete in the excerpts obtained; this group is rarely used by anything except FTP Software's own TCP. **Verification against `PACKET_D.109` is needed before claiming numeric values here.** This is conjecture; treat as "exists, details unclear" for now.

#### Function 20 (0x14) — `set_rcv_mode`

- Input: `AH=20`, `BX=handle`, `CX` = mode value (1..6).
- Mode values (Crynwr PD Spec rev 1.09 §"set_rcv_mode" — verbatim from search excerpt of http://crynwr.com/packet_driver.html):
  - 1 = receiver off
  - 2 = receive only packets sent to this interface (unicast-to-self only)
  - 3 = mode 2 + broadcast
  - 4 = mode 3 + a limited set of multicast (filter list set via function 22)
  - 5 = mode 3 + all multicast (multicast-promiscuous)
  - 6 = all packets (full promiscuous)
- Errors: `BAD_HANDLE`, `BAD_MODE`, `CANT_SET`.
- Note: this is a per-interface setting, not per-handle. Two applications cannot demand conflicting modes; whoever asked last wins, which is a known wart of the spec.

#### Function 21 (0x15) — `get_rcv_mode`

- Input: `AH=21`, `BX=handle`.
- Output: `AX` = current mode (1..6).

#### Function 22 (0x16) — `set_multicast_list`

- Input: `AH=22`, `ES:DI` = pointer to address list, `CX` = total length (= num_addrs * addr_len).
- Used together with mode 4 to install a filter.
- Errors: `NO_MULTICAST`, `BAD_ADDRESS`, `NO_SPACE`.

#### Function 23 (0x17) — `get_multicast_list`

- Input: `AH=23`.
- Output: `ES:DI` = current list, `CX` = list length.

#### Function 24 (0x18) — `get_statistics`

- Input: `AH=24`, `BX=handle`.
- Output: `DS:SI` = pointer to stats structure: `packets_in`, `packets_out`, `bytes_in`, `bytes_out`, `errors_in`, `errors_out`, `packets_dropped`.

#### Function 25 (0x19) — `set_address`

- Input: `AH=25`, `ES:DI` = new address, `CX` = length (6).
- Output: CF=0 if accepted.
- Errors: `BAD_ADDRESS`, `CANT_SET`.
- Most drivers refuse to do this on real hardware; the EEPROM MAC is sacred.

### 2.5 Error codes

The spec defines symbolic names and numeric values. The numbers below come from `network_calls.txt` (http://sininenankka.dy.fi/leetos/network_calls.txt) and corroborate the Crynwr text excerpt; verify against `PACKET_D.109`.

| Value | Symbol | Meaning |
|-------|--------|---------|
| 1     | BAD_HANDLE       | handle does not refer to a current access_type |
| 2     | NO_CLASS         | this driver does not implement that class |
| 3     | NO_TYPE          | this driver does not implement that type |
| 4     | NO_NUMBER        | this driver does not implement that number |
| 5     | BAD_TYPE         | type field garbage |
| 6     | NO_MULTICAST     | multicasts not supported |
| 7     | CANT_TERMINATE   | refusing to unhook |
| 8     | BAD_MODE         | bad receive mode (see set_rcv_mode) |
| 9     | NO_SPACE         | buffer too small or out of memory |
| 10    | TYPE_INUSE       | type is already registered by another handle |
| 11    | BAD_COMMAND      | unknown AH function |
| 12    | CANT_SEND        | transmit failed at the hardware |
| 13    | CANT_SET         | hardware refused the request |
| 14    | BAD_ADDRESS      | address malformed (e.g. wrong length) |
| 15    | CANT_RESET       | reset_interface failed |

This list is conjecture in its exact numeric assignment — only the names are guaranteed from the search excerpts. **Verify before relying.**

---

## 3. The receive upcall — deep dive (the integration crux)

### 3.1 The two-call convention

When a packet arrives that matches a registered `access_type`, the driver calls back into the application **twice** from inside the hardware ISR (Crynwr PD Spec rev 1.09 §"Receive callback"; verbatim from spec excerpt and from https://en.wikipedia.org/wiki/PC/TCP_Packet_Driver):

**First call — buffer request:**
- `AX = 0` (the "give me a buffer" call)
- `BX = handle` (the one returned by `access_type`)
- `CX = packet length in bytes` (the full MAC frame length minus FCS)
- `DS:SI = 0:0` (per 1.09; in rev 1.10 may point to a "look-ahead" buffer — see 3.2)
- Application returns: `ES:DI = far pointer to a buffer of at least CX bytes`, or `ES:DI = 0:0` to refuse the packet (driver will drop it).

**Second call — packet delivery:**
- `AX = 1` (the "here is the data" call)
- `BX = handle`
- `CX = length` (same as before)
- `DS:SI = pointer to the buffer the application supplied` (i.e. the same `ES:DI` returned from the AX=0 call, swapped to DS:SI)
- Application: data is now in the buffer; do whatever with it; return.

Spec text on length semantics: "It is important that the packet length (CX) be valid on the AX == 0 call, so that the receiver can allocate a buffer of the proper size, and this length must include the MAC header and all received data, but not the trailing Frame Check Sequence (if any)." (Crynwr PD Spec rev 1.09 §"Receive callback".)

### 3.2 Look-ahead variant (spec rev 1.10)

Rev 1.10 added an optional optimisation: on the AX=0 call the driver may pass `DS:SI != 0` pointing to a *look-ahead buffer* holding the first `DX` bytes of the frame (typically the MAC + IP header). The app can inspect this and decide whether to accept the packet at all — useful for filtering ICMP from a flood, or for sniffer apps that just need the headers. If the app accepts, it returns a buffer in `ES:DI` of size `CX` (its real buffer); if it refuses, returns `ES:DI = 0:0`. (Crynwr PD Spec rev 1.10 §"Receive callback look-ahead"; excerpt from http://crynwr.com/packet_driver.html.)

### 3.3 Context constraints inside the upcall

The application's callback runs **inside the NIC's hardware interrupt service routine**. That means:
- The PIC has not yet been EOI'd for this IRQ (typically).
- Interrupts may be off (the driver typically `cli`s around the upcall; behaviour varies).
- The DOS state is undefined (we may have interrupted DOS in the middle of an INT 21h).
- Therefore the upcall **cannot call DOS** (no INT 21h) and should do absolutely minimal work — copy bytes out of the supplied buffer into a ring/queue and return.
- The upcall's stack is the driver's stack (small — a few hundred bytes typically). Applications that need significant stack space install their own.

This single fact drives every design choice that follows.

### 3.4 The protected-mode application problem

Apps built with DJGPP (Watt-32) or DOS/32A or DOS4G live in protected mode (DPMI client). They cannot register a real-mode `ES:DI` directly — their code lives at a 32-bit protected-mode address.

Two mechanisms exist:

**(a) DPMI real-mode callback (INT 31h AX=0303h)**
The application calls `INT 31h AX=0303h` passing the PM selector:offset of its callback. The DPMI host returns a real-mode segment:offset pointing at a small thunk inside the host's DOS-memory area. When the packet driver invokes that real-mode address from its ISR, the DPMI host:
1. Saves the real-mode registers into a `DpmiRegRec` structure provided at 0303h-time.
2. Switches CPU to PM (on a locked stack — DPMI 0.9 §4.3, "Stacks and Mode Switching", https://www.delorie.com/djgpp/doc/dpmi/ch4.3.html).
3. Enters the PM callback.
4. On IRET, the host copies PM regs back into `DpmiRegRec` and resumes the real-mode caller.

Cost: every received packet pays a full RM→PM→RM switch round-trip plus reg-marshalling. On a 386SX/40 that's ~50-200µs depending on host. Survivable for slow links, brutal for 100M or 1G traffic.

**(b) DOS4G / DOS/32A specialised upcall (Watt-32's `asmpkt4.asm` path)**
DOS4G publishes a faster mechanism that bypasses the generic 0303h thunk. Watt-32 detects DOS4G/DOS/32A and switches to this path when available: "PKTDRVR upcalls are not via RMCBs, but use the DOS4GW method, which is much faster and reduces the number of buffer copies through a single transfer from PKTDRVR to ring-buffer" (Watt-32 src/pcpkt.c — http://www.watt-32.net/watt-doc/pcpkt_8c.html; see also `Watt-32/asmpkt4.asm`). This is extender-specific — non-portable.

### 3.5 How the kernel-resident packet driver flips this around

Because pinecore-x86 owns the kernel, the NIC, the IRQ, and the DPMI host, we have a fourth option that none of the historical drivers had:

- We install our `PKT DRVR` handler at INT 60h. The handler dispatches in Ring 0.
- A V86 DOS app (e.g. an old mTCP binary) calls INT 60h. Our V86 monitor traps this; the kernel services functions 1-25 directly; for `access_type` the kernel records the V86 `ES:DI` as the callback.
- A PM DPMI client (e.g. DJGPP-built mTCP, or DOOM if it ever uses networking) calls INT 31h AX=0300h with BL=0x60 — our DPMI host's existing 0300h reflector either reflects to V86 (which trips back into us — wasteful) **or** detects the PD signature and short-circuits the call into the kernel directly, marshalling regs without a V86 trip.
- When a packet arrives, the kernel's NIC ISR (Ring 0, on the kernel stack — *not* a borrowed DOS stack — so size is not a constraint) needs to deliver to:
  - A V86 callback: schedule a V86 monitor entry that simulates the two-call ISR-context upcall by injecting the right CS:IP into the V86 task. This is essentially the same mechanism used to deliver hardware IRQs to V86 DOS today.
  - A PM DPMI client: the kernel synthesises a 0303h-equivalent — it has the client's PM CS:EIP, it can build a PM stack frame and jump there.

Net effect: the latency baseline becomes "kernel ISR → callback" (a few µs) instead of "TSR ISR → DPMI thunk → PM callback" (50-200µs). This is the same architectural argument that justified making the kernel the DPMI host (see `29-dpmi-host.md`).

The catch: we have to faithfully reproduce the spec's two-call protocol, including the constraint that the upcall happens "in interrupt context" so applications written to assume DOS-is-unsafe will continue to behave correctly. We must also serialise so that two packets arriving back-to-back don't tail-call each other and overrun the application's buffer logic.

---

## 4. IRQ handling and 8259 EOI

A real-mode packet driver TSR hooks both:
1. The software INT vector (60h-7Fh) — for application calls.
2. The hardware IRQ vector (e.g. INT 0x0B for IRQ 3) — for NIC ISR.

### 4.1 ISR responsibilities

On NIC interrupt:
1. Save all registers (including segments).
2. Switch to driver's data segment.
3. Read NIC's ISR/status register to identify cause (RX-done, TX-done, error).
4. Process RX: copy frame out of NIC's receive ring; locate the matching `access_type` handle by demuxing the 2-byte Ethernet type field; invoke the application callback with the two-call protocol.
5. Process TX-complete: invoke `as_send_pkt` upcall if any.
6. Acknowledge the NIC's interrupt (chip-specific — write to ISR or ICR).
7. **EOI the 8259** — write `0x20` (non-specific EOI) to port 0x20 (master) and 0xA0 (slave) if IRQ >= 8. The spec is that DOS drivers always use non-specific EOI; "DOS device drivers are expected to send a non-specific EOI to the 8259s when they finish servicing their device" (OSDev wiki — 8259 PIC, https://wiki.osdev.org/8259_PIC). This excludes specific-EOI and AEOI modes.
8. `iret`.

### 4.2 IRQ chaining

If the NIC's IRQ line is shared (rare for ISA, common for PCI), the driver chains: at install time, it reads the previous INT 0Bh handler via INT 21h AX=3500h, stores it, then at ISR-entry it inspects the NIC's status register, and if "this is not my interrupt" it `jmp`s to the previous handler instead of EOI'ing. Most Crynwr drivers do this defensively even on ISA. (Pattern visible in `crynwr_mirror/8390.asm` and `ne2000.asm`; e.g. https://github.com/skiselev/isa8_eth/blob/main/software/driver/NE2000.ASM.)

### 4.3 In our kernel

We never run on the DOS IRQ vector. The PIC is owned by the kernel; the NIC IRQ is delivered to the kernel's ISR via the IDT. EOI is the kernel's responsibility (already implemented for timer/keyboard/floppy). Chaining is unnecessary — we own the line. The "is this my interrupt?" check is still needed if the IRQ is shared with another PCI device on the same line (very plausible on real hardware).

---

## 5. Real driver survey

Each driver below was selected to cover a different design point. Source-code references are to the fragglet mirror unless noted: https://github.com/fragglet/crynwr_mirror.

### 5.1 The Crynwr skeleton

Russ Nelson built a skeleton: all drivers share a "head" (the resident TSR boilerplate, INT 60h dispatch table, signature, common error paths) and a "tail" (init-time code that prints stats and ETHs the EEPROM MAC). Each NIC adds a per-chip middle file (`ne2000.asm`, `3c509.asm`, etc.) that fills in:
- `init`: identify the card, read MAC from EEPROM, configure interrupt + I/O base, hook IRQ.
- `as_send_pkt` / `send_pkt`: chip-specific transmit.
- `recv`: chip-specific receive ring drain.
- `xmit_done_isr`: TX-complete branch of the ISR.
- `recv_isr`: RX branch of the ISR.

This skeleton is described in Russ Nelson's release notes (http://www.crynwr.com/changes) and visible in the mirror layout: `head.asm`, `tail.asm`, plus per-chip files. The version numbering convention: "if the skeleton changes, the major version is incremented and all minor versions are reset to zero" — so `NE2000 v11.x` means skeleton v11. (Crynwr changes file, http://www.crynwr.com/changes.)

### 5.2 Comparison table

Sizes are rough — the mirror's files vary slightly across versions. Reported "LOC" is for the chip-specific assembly only (not including the shared skeleton `head.asm` / `tail.asm`, which is another ~1500 lines).

| Driver | Bus | Chip | Chip LOC | Notes |
|--------|-----|------|----------|-------|
| NE2000 (`ne2000.asm` + `8390.asm`/`.inc`) | ISA | NS DP8390 (NE2000-clone) | ~400 chip + ~800 8390 | Programmed I/O; uses the 8390 NIC ring; `8390.asm` is shared with 3c503/wd80x3/etc. (fragglet/crynwr_mirror/8390.asm) |
| 3C509 (`3c509.asm`) | ISA + PnP | 3Com 905-series predecessor (ID seq) | ~700 | Uses 3Com's command/status register pair; ID-sequence to find the card; PIO; window-register architecture. Multiple revisions of the driver exist; v11.5 is the "standard". (fragglet/crynwr_mirror/3c509.asm) |
| RTL8139 / RTSPKT (3.40/3.44) | PCI | Realtek RTL8139 | ~1500-2000 (estimate; binary-only mostly) | PCI enumeration (BIOS INT 1Ah); BARs for I/O + MMIO; DMA descriptor ring; bus-master. Source not available on GitHub — only `RTSPKT.COM` binary + `PACKET.TXT` README (http://www.georgpotthast.de/sioux/packet.htm). |
| E1000PKT / GIGPKTDRVR | PCI | Intel 82540/82541/82544/82545/82547 | unknown (Intel-authored, GPLv2) | Source at https://github.com/ulrich-hansen/E1000PKT (FreeDOS repackaging) and originally https://downloadcenter.intel.com/download/12586. Full e1000 DMA ring; requires PCI BIOS 2.1+. |
| PCNTPK | ISA / PCI | AMD PCnet (Am79C97x) | ~1200 | Based on Clarkson University packet driver; supports Am2100/Am1500T legacy + PCnet-Fast III. Source via https://archive.org/details/pcntnd.dos. |

### 5.3 NE2000 (`ne2000.asm` + `8390.asm`) — the archetype

Structure visible in the mirror:
- `head.asm` provides the resident handler and INT 60h dispatch.
- `8390.asm` provides chip-generic operations: ring management, page select, remote-DMA transfer setup. The 8390 has a "remote DMA" engine that the CPU uses to read/write the NIC's on-board 8/16KB SRAM via two I/O ports — every byte costs an `outb`.
- `ne2000.asm` provides board-level glue: I/O base detection (probe 0x300/0x320/0x340), reset sequence (write to NE_RESET=0x1F).
- ISR path:
  - Read EN0_ISR (ISR register).
  - If RX bit set: switch to page 0, read CURR (current page), copy receive buffer out via remote DMA, advance BOUND, fire upcall.
  - If TX-done bit set: clear bit; check for `as_send_pkt` completion upcall.
  - Issue EOI.
- Transmit path: set up remote DMA write, push packet bytes via `out dx, al` loop, write TBCR0/1 with length, write CR with TXP|STA. (Detailed walkthroughs at OSDev — https://wiki.osdev.org/Ne2000 — and OS/2 Museum "Was the NE2000 Really That Bad?" https://www.os2museum.com/wp/was-the-ne2000-really-that-bad/.)

Key takeaway: NE2000 is small because the chip is small and the bus is dumb. There is no DMA on the host side — every byte goes through `IN`/`OUT`. CPU cost is huge per packet but driver complexity is tiny.

### 5.4 3C509 (`3c509.asm`) — the ISA-but-clever case

Different from NE2000:
- The 3C509 has an "ID sequence" for finding the card on a contention-shared 0x110 I/O port. (`hackerb9/3C509B-nestor/defs.asm` and `jfabienke/3com-packet-driver` are good references — https://github.com/jfabienke/3com-packet-driver).
- It exposes a windowed register file: write the "window select" register, then access registers via the window's port.
- It still uses PIO for data, but with FIFO threshold support (TX can be started when only part of the packet has been written).
- The driver is ~700 LOC and supports promiscuous mode natively. (See Crynwr changes file's list of "drivers with promiscuous": 3c501, 3c523, ni9210, ni5210, at&t, ni6510, wd8003e — 3c509 was added later. http://www.crynwr.com/changes.)

### 5.5 RTL8139 / RTSPKT — closest analog by bus class

`RTSPKT.COM` is published binary-only by Realtek (3.44) and Georg Potthast (3.40). Source is hard to come by, but the algorithmic shape is well-documented elsewhere (OSDev — https://wiki.osdev.org/RTL8139, Linux `8139cp.c`, sanos `rtl8139.c`, minirighi). The DOS driver must:
- Enumerate PCI via INT 1Ah AX=B100h..B109h to find vendor 0x10EC / device 0x8139.
- Read BAR0 for the I/O base, BAR1 for MMIO (rarely used in DOS — uses BAR0).
- Allocate a single physical-contiguous receive ring buffer (8K/16K/32K + 16 bytes for the wraparound + 1500 for the last frame). In real mode this is just a DOS conventional-memory allocation. In DPMI this is dramatically harder — see §6.2.
- Write the buffer's *physical* address (not its real-mode segment, not its PM linear address) to `RBSTART`. Bus-master DMA reads physical addresses straight off the bus.
- Enable Bus Master in PCI command register.
- On RX interrupt, the NIC has already written packets into the ring; the driver walks the ring, finds frames matching `access_type`, fires upcalls.

PCI NIC packet drivers are 3-5× the source-code volume of ISA drivers, mostly because of (a) PCI enumeration, (b) DMA-descriptor management, (c) the need to handle bus-master quirks per chip.

### 5.6 E1000PKT / GIGPKTDRVR — the only real Intel-gigabit DOS packet driver

https://github.com/ulrich-hansen/E1000PKT is a FreeDOS repackaging of Intel's GIGPKTDRVR.EXE (originally https://downloadcenter.intel.com/download/12586). Licence: GPLv2. Supported chips per the README and `eeprom.inc`:

- 82540 (PRO/1000 MT desktop)
- 82541 (PRO/1000 MT mobile)
- 82544 (PRO/1000 XT server)
- 82545 (PRO/1000 MF/MT server)
- 82547 (PRO/1000 CT)

Notably **not** supported: 82571, 82572, 82573, 82574, 82577, 82578, 82579, 82567 (any -LF/-V/-LM variant), 82583, I217, I218, I219. The 82567LM-3 is **not in E1000PKT's chip list**. It would require porting.

**Why 82567 is different — critical architecture note:**

The 82567LM-3 is **only a PHY** (specifically a PCI-Express GbE PHY), not a complete NIC. The MAC is **integrated into the ICH9 chipset itself**, accessed via two proprietary host-side serial buses: **GLCI** (Gigabit LAN Connect Interface, for 1Gbps traffic) and **LCI** (low-speed LAN Connect Interface, for 10/100). The PHY register interface is reached through `MDIC` writes via the MAC-side registers, with semaphore protection between the MAC and the chipset's BIOS firmware. (Intel 82567 GbE PHY datasheet — https://www.intel.com/content/dam/doc/datasheet/82567-gbe-phy-datasheet.pdf; https://ark.intel.com/content/www/us/en/ark/products/35646/intel-82567lm-gigabit-ethernet-phy.html; confirmed in Linux `e1000e/ich8lan.c` — https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/intel/e1000e/ich8lan.c.)

Consequence for us:
- E1000PKT and the original Intel GIGPKTDRVR target the "discrete e1000" generation (82540-82547), which has a self-contained MAC+PHY accessible through a single PCI BAR. Their code structure does not map onto the 82567.
- The e1000e Linux driver is the canonical reference for the 82567LM-3 — specifically `ich8lan.c`, which contains all the ICH9-specific PHY-via-MDIC quirks. Even the e1000e community thread "[034/116] e1000e: enable new 82567V-3 device" (https://lkml.kernel.org/lkml/20100330225619.674623653@linux.site/) shows how late device-id-only patches were still needed.
- There is **no known DOS packet driver for the 82567LM-3.** Confirmed by VOGONS thread "MS-DOS packet drivers for these Intel cards" (https://www.vogons.org/viewtopic.php?t=101377; multiple users report no working driver), and by the absence of any 82567 device ID in any FreeDOS / mTCP-community / packetdriversdos.net listing (https://packetdriversdos.net/).

This means our bring-up work is also a green-field driver. The good news is `ich8lan.c` is liberally commented and the GPL is permissive enough to "study principles, write original" per CONTRIBUTING.md rule #3.

### 5.7 PCNTPK — the closest extant Intel-branded driver

AMD PCnet (Am79C97x) is the lineage of the QEMU `pcnet` default NIC, not the 82567 lineage. PCNTPK is structurally similar to RTL8139 packet drivers: PCI enumeration, BARs, single DMA ring, bus-master. Source on https://archive.org/details/pcntnd.dos and discussed at http://www.oldcomputers.it/parts/cubix/ers/docs/sitocubix/amd-c13.htm. Useful as a stylistic reference but irrelevant to the 82567 MAC layout.

---

## 6. DOS extender / DPMI interaction

### 6.1 The two reflection paths (DPMI host's responsibility)

When a DPMI client wants to talk to the packet driver, it executes `INT 31h` with AX=0300h, BL=60 (the packet driver's INT vector), and a real-mode register block at ES:EDI. (INT 31h AX=0300h — https://www.delorie.com/djgpp/doc/dpmi/api/310300.html; Tech Help Manual — http://www.techhelpmanual.com/602-int_31h_0300h__simulate_real_mode_interrupt.html; RBIL — https://fd.lod.bz/rbil/interrup/dos_extenders/310300.html.)

The DPMI host:
1. Copies the register block to real-mode CPU state.
2. Switches CPU to real mode (V86 mode for 32-bit hosts).
3. Executes `INT 60h`.
4. Switches CPU back to PM.
5. Copies returned register state back to the client's structure.

For data buffers (e.g. `send_pkt`'s DS:SI, `access_type`'s ES:DI), the PM client must pre-allocate a buffer in **DOS conventional memory** (INT 31h AX=0100h, "Allocate DOS Memory Block") and pass its real-mode segment. The packet driver knows nothing about protected mode.

### 6.2 Real-mode callbacks (INT 31h AX=0303h)

When the PM client calls `access_type` (function 2), the `ES:DI` it passes for the upcall **must be a real-mode address that the driver can `call far` to from its ISR**. The PM client cannot give its own PM address directly.

The mechanism (DPMI 0.9 §"Allocate Real Mode Callback Address"; https://www.delorie.com/djgpp/doc/dpmi/api/310303.html; http://www.techhelpmanual.com/605-int_31h_0303h__allocate_real_mode_callback_address.html):

1. PM client calls `INT 31h AX=0303h`, passing DS:SI = PM selector:offset of the PM upcall handler, ES:DI = pointer to a `DpmiRegRec` structure (the host will fill this with the RM register state on each callback invocation).
2. Host returns CX:DX = real-mode segment:offset of a small thunk it owns in DOS conventional memory.
3. Client calls `INT 31h AX=0300h, BL=60, AH=2 (access_type)` passing CX:DX as the `ES:DI` upcall pointer.
4. On packet receipt, the driver's ISR does a `call far [CX:DX]` from real mode. Control lands in the host's thunk, which:
   - Saves real-mode registers into `DpmiRegRec`.
   - Switches to a host-private PM stack ("the host automatically switches to a locked protected mode stack during servicing of hardware interrupts, software interrupts 1Ch/23h/24h, all exceptions, and during the execution of real mode callbacks" — DPMI 0.9 §4.3, https://www.delorie.com/djgpp/doc/dpmi/ch4.3.html).
   - Calls the PM handler.
   - On IRET, restores real-mode state from `DpmiRegRec` and returns to the driver.

DPMI 0.9 guarantees at least 16 callbacks per client (techhelpmanual 605). Hosts vary; HDPMI provides 32, CWSDPMI 16, DOS4G 8.

### 6.3 Watt-32's path

Watt-32 (the DJGPP/Watcom DOS TCP stack) implements both paths in `src/pcpkt.c` (http://www.watt-32.net/watt-doc/pcpkt_8c.html). For most DJGPP/CWSDPMI/HDPMI clients it uses 0303h RMCBs. For DOS4G/DOS/32A targets it uses the faster `asmpkt4.asm` direct-thunk mechanism, which avoids a `DpmiRegRec` marshalling step.

Documented quote: "Watcom/DOS4GW targets don't use real-to-protected mode upcall (RMCB), but instead use an alternative method implemented in asmpkt4.asm." (Watt-32 changes file at https://github.com/gvanem/Watt-32/blob/master/changes; also mirror at https://github.com/sezero/watt32/blob/2.2.10-sezero/changes.)

Watt-32 also implements an internal ring-buffer between the upcall (which just memcpys into the ring) and the user-mode TCP state machine (which polls the ring). This decouples the IRQ-time upcall from any DOS-calling code path. (`pcpkt.c` — "the routine enqueues received packets into the packet queue, is called from pkt_receiver_rm/_pm(), with packets copied to rx_buffer in DOS memory by the packet-driver".)

### 6.4 What pinecore-x86 already implements

Per `29-dpmi-host.md` and `39-dpmi-session-35-deepdive.md`, the kernel implements INT 31h AX=0300h reflection (with caveats around segment leaks — see the s35 ES-leak hypothesis in MEMORY.md). 0303h ("Allocate Real Mode Callback") is **not yet implemented**. Until it is, PM packet-driver clients can call into the driver synchronously (send_pkt etc) but cannot register upcalls — which is half the point. Implementing 0303h is a prerequisite for any PM application doing receive.

The kernel-as-driver design described in §3.5 partly removes this prerequisite: if the kernel owns the upcall delivery, it can synthesise the PM-side jump directly from the NIC ISR without needing a generic RMCB thunk. But applications use 0303h regardless of whether we're hosting the packet driver or some TSR is — so we should implement 0303h anyway.

---

## 7. PCI-NIC specifics for our case

### 7.1 Enumeration

- INT 1Ah AX=B101h (PCI installation check) — returns version.
- INT 1Ah AX=B102h (find device by vendor+device ID) — for 82567LM-3, vendor 0x8086, device IDs include 0x10DE, 0x10F5 depending on revision (Linux `ich8lan.c` `e1000_ich9lan_info` table). In our kernel we are already past INT-1Ah-based enumeration — `20-pci-bus.md` covers our direct config-space access.
- Read BAR0 (memory BAR for 82567), BAR1 (flash, irrelevant), and the interrupt line.
- Enable Bus Master and Memory Space bits in PCI command register (offset 0x04).

### 7.2 DMA descriptor rings

The 82567/e1000e family uses descriptor rings:
- TX ring: array of `e1000_tx_desc`, each pointing to a physical buffer + length + flags. Head pointer (TDH) and tail pointer (TDT) registers; the driver advances TDT to indicate "new descriptors ready", the NIC advances TDH as it transmits.
- RX ring: array of `e1000_rx_desc`, each pre-filled with a physical buffer address. NIC writes incoming packets, sets DD (Descriptor Done) bit. Driver polls or takes interrupt, reads filled descriptors, recycles them by advancing RDT.

For DOS this means we need:
- Physical-address allocation that is bus-master-safe.
- Knowledge of where in linear memory each physical page lives, so we can write a physical address into a descriptor.

In a flat real-mode driver this is trivial: every address is identity-mapped (lin = phys = seg*16+off). In our kernel with paging on, we have to either:
- Pin a chunk of low memory (below 16MB? — depends on the 82567's DMA addressing capabilities; the 82567 supports 64-bit DMA addresses, so no 16MB ISA limit applies. e1000e `enum.h` `E1000_FLAG_PCI_EXPRESS`.)
- Or pin arbitrary pages and read PTEs to convert virt→phys at descriptor-fill time.

The cleanest path: reserve a contiguous physical region at boot, allocate buffers from it linearly, use the trivial linear→phys mapping. This is the same pattern the kernel will need for VESA framebuffer, Voodoo command FIFO, and AC97 audio (none of which are in scope right now).

### 7.3 The 82567's twist

Unlike a "normal" e1000 (82540 etc), the 82567 has the PHY behind the GLCI/LCI buses with the MAC inside the ICH9. PHY register reads/writes go through MDIC in the MAC, but there is a *semaphore* protocol: BIOS may also be touching the PHY (for Manageability / AMT). Linux's `ich8lan.c` `e1000_acquire_swflag_ich8lan` / `e1000_release_swflag_ich8lan` shows the protocol — read SWSM, set FWH_SW bit, wait for FW to release, etc. **Skipping the semaphore on certain Lenovo ThinkPads with AMT firmware will hang the machine.** This is the single highest-risk bring-up item.

This is conjecture for our environment (we're running on Pineapple3 hardware, not Lenovo AMT firmware, so the semaphore *may* be a no-op for us — verify with a brief test before committing to a complex synchronization implementation).

---

## 8. Receive modes — chip-level mapping

| Spec mode (function 20) | What the chip register has to look like (82567-class) |
|-------------------------|---------------------------------------------------------|
| 1 — off                  | RCTL.RXEN = 0 |
| 2 — unicast-to-self      | RCTL.UPE = 0, RCTL.MPE = 0, RCTL.BAM = 0 (or just rely on RAL/RAH MAC filter) |
| 3 — mode 2 + broadcast   | RCTL.BAM = 1 |
| 4 — mode 3 + mc filter   | RCTL.MPE = 0, MTA (multicast table array) populated from function 22 list |
| 5 — mode 3 + all mc      | RCTL.MPE = 1 |
| 6 — promiscuous          | RCTL.UPE = 1 (unicast promisc), RCTL.MPE = 1 (multicast promisc) |

(RCTL = Receive Control register, bits per Intel e1000 datasheet 10.2.x; Linux `e1000_regs.h` `E1000_RCTL_UPE` etc. — https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/intel/e1000e/regs.h.)

For NE2000 the analogous mapping is the RCR register; for RTL8139 it's `RxConfig.AAP`/`AB`/`AM`. Each chip family has its own set of bits but the spec's six modes map onto every modern Ethernet controller cleanly. Mode 4 (filtered multicast) is the only one that requires `set_multicast_list` (function 22) to populate a list.

---

## 9. Prior art for Intel gigabit DOS — summary

| Chip family | DOS packet driver | Source available |
|-------------|-------------------|------------------|
| 82540 / 82541 / 82544 / 82545 / 82547 | E1000PKT / GIGPKTDRVR | Yes — GPLv2 at https://github.com/ulrich-hansen/E1000PKT |
| 82550 / 82551 / 82559 (PRO/100) | PROAPPS / Intel "PRO" DOS pkg | Binary only; Intel MS-DOS Drivers v24.3 https://archive.org/details/PRODOS |
| 82567 / 82577 / 82578 / 82579 (ICH9/ICH10/PCH) | **None known** | — |
| I217/I218/I219 (PCH later) | **None known** | — |
| AMD PCnet (Am79C97x) | PCNTPK | https://archive.org/details/pcntnd.dos |

The unambiguous conclusion: **for the 82567LM-3 there is no DOS prior art**. The closest reference is the Linux `e1000e` driver, file `ich8lan.c`, MIT-incompatible-but-GPL — so we read it and write original code per CONTRIBUTING.md rule #3.

A secondary reference is the OSDev wiki Intel 8254x page (https://wiki.osdev.org/Intel_8254x), which is at the 82540 level — only useful for the high-level descriptor-ring shape, not for the ICH9 PHY semantics.

The VOGONS thread documenting this gap is at https://www.vogons.org/viewtopic.php?t=101377 — users repeatedly attempt and fail to find a DOS driver for any 82567-family card. Several propose porting e1000e; none have completed it publicly.

---

## 10. Testing tools

| Tool | Purpose | Source |
|------|---------|--------|
| `PKTSTAT.COM` | Reads function-24 stats from a driver. (Crynwr changes file, "Russell Nelson wrote pktmulti, pktsend, pktstat") | Crynwr collection |
| `PKTSEND.COM` | Sends synthetic frames via `send_pkt`. | Crynwr |
| `PKTMULTI.COM` | Demonstrates multiple `access_type` handles in one process. | Crynwr |
| `PKTWATCH` / `WATCH.EXE` | Promiscuous sniffer (mode 6); displays decoded frames. Source: https://github.com/fragglet/crynwr_mirror/blob/main/pktwatch.asm. | Crynwr |
| `mTCP ping.exe`, `dhcp.exe`, `htget.exe`, `ftp.exe` | End-to-end correctness — DHCP gets us an address, ping proves bidirectional flow, htget proves payload integrity. (https://www.brutman.com/mTCP/) | mTCP |
| `WATTCP` apps (sample utilities) | DJGPP-built; will exercise the DPMI 0300h+0303h paths. | Watt-32 |
| `DIS_PKT9` | NDIS-to-packet-driver shim — confirms our driver looks like NE2000-class to a generic NDIS consumer; secondary, optional. (https://help.fdos.org/en/hhstndrd/network/crynwr.htm) | FreeDOS |

For pinecore-x86 specifically the smallest viable acceptance test is: load packet driver in kernel, run mTCP `dhcp` from a V86 DOS shell, observe an IP lease, ping the gateway. That exercises: function 1 (info), function 2 (access_type for IP=0x0800 and ARP=0x0806), function 4 (send_pkt), and the receive upcall.

---

## 11. References

### Spec primary sources
- Crynwr — **PC/TCP Packet Driver Specification** (rev 1.09, 14 Sep 1989; updates through 1.11). http://crynwr.com/packet_driver.html — primary; not directly fetched during this research session.
- Crynwr — **Packet Driver Changes** (CHANGES file describing skeleton revisions). http://www.crynwr.com/changes.
- Mirror — `network_calls.txt` (http://sininenankka.dy.fi/leetos/network_calls.txt).
- Mirror — http://www.ibiblio.org/towfiq/packet-d.html.
- Wikipedia — https://en.wikipedia.org/wiki/PC/TCP_Packet_Driver.

### Driver source code
- https://github.com/fragglet/crynwr_mirror — archive of Russ Nelson's packet drivers (head.asm, tail.asm, 8390.asm, ne2000.asm, 3c509.asm, 3c503.asm, lance.asm, 82586.asm, depca.asm, at1700.asm, arcnet.asm, ...; pktwatch.asm; binaries/).
- https://github.com/skiselev/isa8_eth/blob/main/software/driver/NE2000.ASM — modern NE2000 packet driver derivation, well-commented.
- https://github.com/hackerb9/3C509B-nestor and https://github.com/davidebreso/3c509b — 3C509B XT-slot driver.
- https://github.com/jfabienke/3com-packet-driver — modern 3C515-TX + 3C509B with PnP.
- https://github.com/ulrich-hansen/E1000PKT — Intel GIGPKTDRVR (GPLv2) repackaged for FreeDOS; supports 82540/41/44/45/47 — **not** 82567.
- Intel original — https://downloadcenter.intel.com/download/12586 (Gigabit Packet Driver for DOS, GPLv2).
- Intel — https://www.intel.com/content/www/us/en/download/2595/29138/intel-ethernet-adapter-drivers-for-ms-dos-final-release.html (NDIS, not packet driver).
- AMD PCNET — https://archive.org/details/pcntnd.dos.
- RTL8139 — binary `RTSPKT.COM` at http://www.georgpotthast.de/sioux/packet.htm; README text at https://contents.driverguide.com/content.php?id=931807&path=Realtek/RTL8139/LAN/RTSPKT/PACKET.TXT.
- Aggregate collection — https://packetdriversdos.net/ (Omar Yabar's archive).

### DPMI / DOS extender
- DPMI 0.9 spec — https://www.phatcode.net/res/262/files/dpmi09.html.
- DJGPP DPMI docs — INT 31h 0300h https://www.delorie.com/djgpp/doc/dpmi/api/310300.html; 0303h https://www.delorie.com/djgpp/doc/dpmi/api/310303.html; §4.3 Stacks https://www.delorie.com/djgpp/doc/dpmi/ch4.3.html; §4.6 Real-Mode Callbacks https://www.delorie.com/djgpp/doc/dpmi/ch4.6.html.
- Tech Help Manual — http://www.techhelpmanual.com/602-int_31h_0300h__simulate_real_mode_interrupt.html and http://www.techhelpmanual.com/605-int_31h_0303h__allocate_real_mode_callback_address.html.
- Watt-32 — http://www.watt-32.net/watt-doc/pcpkt_8c.html, http://www.watt-32.net/change.log, https://github.com/gvanem/Watt-32, `asmpkt4.asm` for DOS4G upcall path.

### Hardware references
- Intel — https://www.intel.com/content/dam/doc/datasheet/82567-gbe-phy-datasheet.pdf (82567 GbE PHY datasheet PDF).
- Intel ARK — https://ark.intel.com/content/www/us/en/ark/products/35646/intel-82567lm-gigabit-ethernet-phy.html.
- Linux e1000e — https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/intel/e1000e/ich8lan.c (the ICH9-specific glue, where the 82567LM-3 lives).
- Linux e1000e — https://lkml.kernel.org/lkml/20100330225619.674623653@linux.site/ (82567V-3 enable patch).
- OSDev — https://wiki.osdev.org/Intel_8254x (high-level e1000 descriptor architecture).
- OSDev — https://wiki.osdev.org/Ne2000, https://wiki.osdev.org/RTL8139, https://wiki.osdev.org/8259_PIC.
- OS/2 Museum — https://www.os2museum.com/wp/was-the-ne2000-really-that-bad/ — NE2000 architecture commentary.

### Practical / community
- mTCP — https://www.brutman.com/mTCP/, programming sample https://www.brutman.com/mTCP/mTCP_Programming_Sample.html, TCPacket https://www.brutman.com/mTCP/mTCP_tcpacket.html.
- Brutman — https://www.brutman.com/Dos_Networking/.
- FreeDOS — https://help.fdos.org/en/hhstndrd/network/crynwr.htm; NDIS shim https://help.fdos.org/en/hhstndrd/network/ndis_ins.htm.
- VOGONS — https://www.vogons.org/viewtopic.php?t=101377 — community survey confirming no 82567 driver exists.
- Georg Potthast — http://www.georgpotthast.de/sioux/packet.htm — practical NIC/driver matrix.
- Kermit project archive — https://www.columbia.edu/kermit/ftp/packetdrivers/README.TXT.

### Cross-references in this repo
- `docs/research/20-pci-bus.md` — PCI BAR mapping, our config-space access.
- `docs/research/22-rtl8139-nic.md` — RTL8139 chip programming.
- `docs/research/23-ne2000-nic.md` — NE2000 / 8390 chip programming.
- `docs/research/29-dpmi-host.md` — pinecore's DPMI host.
- `docs/research/31-dpmi-specification.md` — DPMI 0.9 service map.
- `docs/research/39-dpmi-session-35-deepdive.md` — current INT 31h 0300h state.

---

## 12. Open questions

1. **Exact numeric values of all error codes.** The names in §2.5 are firm from multiple search excerpts. The numeric assignments (1=BAD_HANDLE, etc.) are conjecture from `network_calls.txt`. Get a clean copy of `PACKET_D.109` or the Crynwr `packet_driver.html` and pin these down before writing any code that returns them.
2. **`set_handle` (function 14) and the rest of the high-perf group.** No register-level data is available on functions 12-19; some are marked "implementation level >= high-perf" in the spec. Apps in scope (mTCP, Watt-32) don't appear to use them; nice to confirm.
3. **Look-ahead buffer ownership and timing.** Rev 1.10 introduces the look-ahead path; does the driver have to leave the look-ahead buffer valid across the AX=0 return? How long? Need to read rev 1.10 text directly.
4. **DPMI host 0303h implementation status.** Confirmed not yet implemented in pinecore-x86 per session-35 notes; queue this as a prerequisite milestone.
5. **82567LM-3 GLCI semaphore semantics on Pineapple3.** Whether the BIOS/AMT path is active determines whether we must implement the `e1000_acquire_swflag_ich8lan` dance or can shortcut it. Cheap to test: try reading PHY ID without semaphore, see if it hangs.
6. **Physical-memory allocation strategy for DMA descriptor rings.** Need to decide between (a) reserve-at-boot contiguous low region with identity mapping, (b) on-demand allocation with VtoP from PTEs. Choice affects every PCI-bus-master driver, not just this one. (Recommend (a) for tractability.)
7. **Multi-V86-task delivery.** If two V86 DOS tasks both want to use the network (e.g. mTCP+DOOM-multiplayer), do we virtualise INT 60h per task, or do all tasks share one driver instance? Spec lets multiple apps in one DOS share via `access_type` handles, so the spec-correct answer is "share". But cross-V86-task delivery (one task's frame arrives while another task is current) needs design — possibly a per-task ring with kernel-side multiplexing.
8. **Whether the kernel-resident model conflicts with mTCP's expectations.** mTCP scans INT 60h-7Fh for the signature; our kernel handler must appear identical to a real TSR. Verify the signature byte layout will satisfy mTCP's scan code (`UTILS.CPP` in mTCP src — needs to be looked at to confirm the exact byte-comparison routine).
9. **rev 1.09 vs 1.11 — should we target 1.09 or the latest?** Most real apps only require 1.09 functionality. 1.10 look-ahead and 1.11 high-perf extensions are optional. Recommend "implement 1.09 fully, leave 1.10/1.11 stubs returning `BAD_COMMAND`".
