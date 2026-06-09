# DOS/4GW Internals — Running DOOM on Pinecore's DPMI Host

> Field guide to the Tenberry/Watcom DOS/4GW DOS extender, the version
> bundled with id Software's DOOM (1993–1995). This document fills the
> implementation gaps that the generic DPMI specification (research 31)
> cannot answer: what specific INT 31h calls DOS/4GW issues, in what
> order, what private vectors it uses (notably `INT 0x8F`), how it
> expects IRQs to be reflected, and what minimum subset DOOM needs.

**Date:** 2026-05-09
**Status:** Active reference — driving the next rounds of DPMI host work.
**Author:** Pinecore kernel research notes.

**Primary sources consulted (cited inline):**

- **OW-DOS4GWQA** — Open Watcom v2 source tree, `docs/doc/rsi/dos4gwqa.gml`, the Tenberry "DOS/4GW Q&A" document distributed with Open Watcom: <https://github.com/open-watcom/open-watcom-v2/blob/master/docs/doc/rsi/dos4gwqa.gml>
- **OW-RSIERR** — Open Watcom v2, `docs/doc/rsi/errors.gml`: <https://github.com/open-watcom/open-watcom-v2/blob/master/docs/doc/rsi/errors.gml>
- **OW-PGDOS32Q** — Open Watcom v2, `docs/doc/pg/pgdos32q.gml`: <https://github.com/open-watcom/open-watcom-v2/blob/master/docs/doc/pg/pgdos32q.gml>
- **OW-RSIACC** — Open Watcom v2, `bld/trap/lcl/dos/dosx/rsi/c/rsiacc.c` (RSI debug trap, runs against DOS/4GW protocol): <https://raw.githubusercontent.com/open-watcom/open-watcom-v2/master/bld/trap/lcl/dos/dosx/rsi/c/rsiacc.c>
- **OS2MUS-FPE** — Michal Necasek, "Floating-Point Exceptions and DOS Extenders," OS/2 Museum: <http://www.os2museum.com/wp/floating-point-exceptions-and-dos-extenders/>
- **DELORIE-DPMI** — DJ Delorie's HTML mirror of the DPMI 1.0 specification: <https://www.delorie.com/djgpp/doc/dpmi/>
- **DPMI09** — DPMI 0.9 specification (Phatcode mirror): <https://www.phatcode.net/res/262/files/dpmi09.html>
- **D4G-UNOFF** — "DOS/4G(W) Unofficial Programmer's Guide": <https://rgmroman.narod.ru/Dos4g.htm>
- **TENBERRY-FAQ** — Tenberry Software FAQ archive: <http://tenberry.com/dos4g/faq/index.html>
- **WIKI-DOS4G** — Wikipedia, DOS/4G: <https://en.wikipedia.org/wiki/DOS/4G>
- **PIKUMA-D4G** — Pikuma, "DOS/4GW and Protected Mode": <https://pikuma.com/blog/what-is-dos4gw-protected-mode>
- **DOOMWIKI-D4G** — DoomWiki: <https://doomwiki.org/wiki/DOS/4GW>
- **CWSDPMI** — CW Sandmann's CWSDPMI r7 source (paths: `cwsdpmi-master/src/...`).
- **STANISLAVS-INTS** — HelpPC interrupt table: <https://stanislavs.org/helppc/int_table.html>
- **CTYME-INT8F** — RBIL HTML index for INT 8F: <https://www.ctyme.com/intr/int-8f.htm>

When a claim is sourced from the project's own dpmi.c symbol or trace
output, it's cited as `(pinecore: dpmi.c:<line>)` or `(pinecore-trace)`.
Where I am extrapolating from extender behavior with no primary source,
I label it **(speculation)** or **(inferred)**. Do not assume otherwise.

---

## 1. DOS/4GW Overview

### 1.1 Heritage

DOS/4GW is a 32-bit DOS extender. It is the youngest sibling in
Tenberry Software's DOS extender family (originally Rational Systems
until the 1995 rename), which evolved as follows (WIKI-DOS4G):

| Year | Product | Notes |
|------|---------|-------|
| 1985 | DOS/16M | 16-bit (286) protected-mode extender for Rational. |
| 1990 | DOS/4G | Full-featured 32-bit extender. Initial release July 1991 at US$5000+ per developer seat (WIKI-DOS4G). |
| 1991 | DOS/4GW | Watcom-bundled, redistribution-licensed limited edition of DOS/4G. Free with Watcom C/C++. The "W" suffix marks it as the Watcom-bundled SKU (PIKUMA-D4G; WIKI-DOS4G). |
| 1992 | DOS/4G PRO | Bound (single-EXE) version with VMM, demand loading, more INT 31h functions. (OW-DOS4GWQA §0a, "Differences in Features.") |
| 1995 | DOS/4GW v1.97 | Last 4GW release Watcom shipped widely. Bug-fix release, primarily for DMA on 16-bit ISA peripherals (e.g. Gravis Ultrasound) (WIKI-DOS4G). |
| 1996 | DOS/4G(W) v2.01 / 2.01a | Final Tenberry releases (April 3, 1996) (WIKI-DOS4G). Also distributed as "DOS/4GW Professional 2.01a." |

DOOM (all retail v1.0–v1.9, including Ultimate Doom) shipped with
DOS/4GW. id Software bundled v1.95 in early DOOM releases and 1.97
in later releases — confirmed by every retail extraction
(DOOMWIKI-D4G, but the wiki page is light; this is widely reproduced
fact, see WIKI-DOS4G's note that v1.97 was the bug-fix release that
shipped to most game customers). Open Watcom redistributes v1.97 as
the canonical DOS/4GW (OW-PGDOS32Q states "Open Watcom uses DOS/4GW
Protected Mode Run-time Version 1.97") .

### 1.2 What v1.95 / v1.97 / v2.01 differ in

For Pinecore's DPMI host the version differences only matter in three
ways:

1. **DMA channel handling.** v1.95 had bugs reading from secondary
   DMA channels (16-bit) on the ISA bus. v1.97 fixed this
   (WIKI-DOS4G). DOOM's sound code uses DMA via INT 21h-routed BIOS
   calls; we don't need to fix anything 4GW-side, but we do need
   working INT 31h/0100h DOS-memory allocation so the extender can
   build a DMA buffer below 1 MB.
2. **DPMI 1.0 functions present in PRO/v2 but not in v1.95/v1.97.**
   See §7. Notably, DOOM never relies on DPMI 1.0 — the v1.97 surface
   is a strict subset of what we need to provide.
3. **Coprocessor emulator integration.** Watcom can compile floating-
   point math to call an emulator; if no x87 is present, the emulator
   uses INT 7 (device-not-available) and IRQ 13 / INT 75h handlers
   that DOS/4GW redirects (OS2MUS-FPE). For a 386+VGA DOOM target we
   always have an FPU, so this is moot — but it explains some of the
   vectors DOS/4GW hooks even when the application doesn't ask.

### 1.3 Relationship to DOS/4G PRO, CauseWay, Pharlap, PMODE/W

Important so we don't overfit to 4GW idioms vs. generic DOS-extender
idioms:

- **DOS/4G PRO** is binary-bound: the .EXE *is* the extender, plus
  the LE image. Init sequence is the same; PRO supports more INT 31h
  calls (301h–306h, 504h–50Ah) (OW-DOS4GWQA §0a).
- **CauseWay** is binary-compatible at the API level, but not at the
  vendor-extension level. CauseWay famously omits the protected-mode
  IRQ 13 handler that DOS/4GW installs (OS2MUS-FPE), so FPU exception
  delivery to RTL signal handlers is broken there.
- **Pharlap 386|DOS-Extender** is older and uses a different (Pharlap
  proprietary) API — but also implements DPMI for compatibility.
- **PMODE/W** by Tran is a small Watcom-compatible bound extender;
  open-source (MIT). It mimics 4GW's mode-switch protocol exactly
  enough that Watcom-built EXEs run unchanged. It's a useful
  read-along for "what does the extender do at startup," though its
  source is not 4GW source.

Pinecore aims to host any of these clients but optimizes the call set
for **DOS/4GW v1.97**, the version DOOM uses.

---

## 2. Init Sequence — From `DOOM.EXE` Real-Mode Stub to Game Entry

This section is the chronological skeleton. Each step cites the
source-of-record where I have one; otherwise it's **(inferred from
trace)** referencing Pinecore's own pinecore-trace observations.

### 2.1 Real-mode phase (V86 in our kernel)

1. **MZ stub runs.** Tenberry licensed a tiny MZ-format launcher into
   `DOOM.EXE`. It prints the famous "DOS/4GW Protected Mode Run-time
   Version 1.97 Copyright (c) Rational Systems, Inc. 1990-1994" banner
   (PIKUMA-D4G; PIKUMA's article reproduces the banner verbatim).
2. **DPMI host detection — `INT 2Fh AX=1687h`.** Returns the entry
   point and version. Pinecore answers this in V86 by populating
   AX=0, BX=1, DH:DL=0:5Ah, ES:DI=mode-switch entry, SI=paragraphs
   needed for private data (we currently say 16) (research 31 §10).
3. **DOS memory allocation for DPMI host private data.** The stub
   issues `INT 21h AH=48h` for `SI` paragraphs and passes `ES =
   allocated_seg` to the entry point. (We've implemented this in the
   V86 monitor.)
4. **Mode switch.** Stub `CALL FAR ES:DI` with AX bit-0=1 (32-bit).
   Pinecore's V86 monitor traps the GPF on the call and switches the
   task to Ring-3 PM with the seed selectors loaded (research 29
   §"Mode Switch Entry"). ES contains private-data seg; CS, DS, SS,
   ES become 64KB-limit LDT selectors aliased to RM CS, DS, SS, PSP.

### 2.2 Protected-mode phase, pre-LE-load

The DOS/4GW kernel itself is now executing in PM. It does not yet
know the LE image's layout — the stub on disk past the MZ contains
the LE blob the extender will parse.

5. **Capability probe.** `INT 31h AX=0400h` (Get DPMI Version).
   DOS/4GW reads our reply (DPMI09 §"INT 31h/0400h"). Our reply (per
   pinecore: dpmi.c:973) is BX=5 (32-bit + V86), CL=3, DH:DL=PIC
   bases, AX=0x005A.
6. **(Optional) `INT 31h AX=0401h`** Get DPMI Capabilities — DPMI 1.0
   only. v1.97 likely skips this; if called we should return CF=1
   "unsupported."
7. **Vendor query.** `INT 31h AX=0A00h DS:ESI = "RATIONAL DOS/4G"`.
   This is a self-query: DOS/4GW is checking whether a *higher-level*
   DOS/4G family extender is hosting it (OW-DOS4GWQA §7a quotes
   exactly this idiom). We must return CF=1 (unsupported vendor) so
   it proceeds as the host. (pinecore: dpmi.c:967-969 returns
   error — confirmed correct.)
8. **Get free-memory info.** `INT 31h AX=0500h` writes a 48-byte
   buffer at ES:EDI describing how much memory is available. DOS/4GW
   uses this to pick the size of its primary allocation (D4G-UNOFF
   discusses this; see also DELORIE-DPMI api/310500.html).
9. **Big memory allocation.** `INT 31h AX=0501h` for the bulk of free
   memory — typically several MB. Returned `BX:CX` is the linear
   address of the block; `SI:DI` is the host-defined handle.
10. **LDT bring-up for flat model.**
    - `INT 31h AX=0000h CX=N` for some small N (typically 4–8) to
      reserve descriptors.
    - For each descriptor: `INT 31h AX=0007h` set base=0;
      `INT 31h AX=0008h` set limit=0xFFFFFFFF; `INT 31h AX=0009h` set
      access (flat code, flat data, etc.).
    - Note: DPMI 0.9 lets the host use page granularity itself.
      We set G=1 when limit > 0xFFFFF (research 31 §3.1).
11. **(In trace) Auto-LDT cascade.** This is *not* a clean DPMI call
    sequence — it's a side-effect of DOS/4GW pushing values that
    happen to look like selectors and getting #GP. CWSDPMI handles
    this by allocating an LDT entry on demand. Pinecore handles
    it via a custom "auto-LDT for unknown selectors" path that has
    fired ~46 times during init (per problem statement). (pinecore-
    trace.) **(Speculation)** This cascade is mostly DOS/4GW saving
    aliased data segments to prepare for switching the C runtime to
    a flat addressing model — most of the new selectors point into
    the big 0501h block and have base != 0.

### 2.3 Protected-mode phase, LE load and fix-up

12. **Read LE header from the EXE.** DOS/4GW issues `INT 21h AH=42h`
    to seek past the MZ to the LE header, then `INT 21h AH=3Fh` to
    read it. From PM these reflect to RM via INT 31h/0300h
    (DPMI09 §"INT 31h/0300h"). For us, INT 21h from PM bypasses 0300h
    and lands directly in our DOS emulator (research 29 §"INT 21h
    Translation from PM").
13. **Allocate LE-image memory** out of the big 0501h block (no
    further DPMI calls — internal sub-allocator).
14. **Read LE pages.** Multiple `INT 21h AH=3Fh` calls to load object
    pages. Each generates a PM file-IO call.
15. **Apply LE fixups.** Internal — uses the LDT base/limit calls from
    step 10 to point each LE object's selector at the loaded-page
    address. **(Speculation)** This is when the auto-LDT cascade fires
    most heavily.

### 2.4 Protected-mode phase, hand-off to client

16. **Install protected-mode exception handlers.** `INT 31h AX=0203h`
    BL=0..1Fh — DOS/4GW always installs all 32 because its run-time
    library catches `#DE`, `#UD`, `#GP`, `#PF` and converts them into
    RTL signal terminators (DPMI09 §"INT 31h/0203h"; OS2MUS-FPE
    confirms it specifically claims the FPE chain).
17. **Hook PM interrupts the runtime needs.** `INT 31h AX=0205h`
    BL=2 (FPE), and during DOOM-specific init, BL=8 (timer), BL=9
    (keyboard), BL=33h (mouse) (OS2MUS-FPE on FPE; DPMI09
    §"INT 31h/0205h" for syntax).
18. **Allocate real-mode callbacks** for any RM→PM bridge needed.
    Pre-DOOM, DOS/4GW typically allocates one RMCB for its own use
    (e.g. as a destination for IRQ 13 reflection or for the
    INT 21h reflector). DOOM later allocates one for the mouse INT
    33h callback. (DPMI09 §"INT 31h/0303h"; pinecore: dpmi.c:887
    "INT 0xF2" trampoline mechanism.)
19. **Switch CS/DS/SS to flat selectors and JMP to LE entry.** The
    LE image's `STARTOBJ:STARTEIP` field is the address of the C
    run-time library's `cstart_` entry point. From there it calls
    `_cmain`, which calls `main`, which is `D_DoomMain`.

### 2.5 What we observe in pinecore-trace post-init

After step 19, two distinct things can happen:

- **Healthy runtime.** Periodic INT 21h (file I/O for save games),
  INT 33h calls (mouse), INT 31h/0300h reflections (BIOS video
  calls), and IRQ 0 timer ticks delivered through the PM handler.
- **Unhealthy livelock — what we hit.** Two `INT 0x8F` sites
  (EIP=0x6DCD and EIP=0x6E4D, immediately after 2-byte `CD 8F`
  instructions). See §3 for the diagnosis.

---

## 3. `INT 0x8F` — The Vendor / Private Vector (load-bearing finding)

This is the part of the document with the biggest implementation
consequence. **Read this section twice before changing dpmi.c.**

### 3.1 What `INT 0x8F` is *not*

- It is **not** a standard PC interrupt. STANISLAVS-INTS lists vector
  range `80h–F0h` as "DOS reserved for BASIC interpreter use," and
  CTYME-INT8F's only entry for INT 8F itself is "IBM ROM BASIC —
  used while in interpreter." That meaning is dead-letter on a
  modern DOS extender — BASIC isn't running.
- It is **not** part of DPMI. The DPMI specification (DPMI09;
  DELORIE-DPMI ch5.n.html) does not assign any meaning to vectors
  outside the DPMI service interrupts (INT 31h, INT 2Fh AX=16xxh,
  INT 21h passthrough, INT 0–1Fh exceptions, IRQs).
- It is **not** documented in the Tenberry/Watcom DOS4GW
  documentation included in Open Watcom v2 (we grepped the master
  copy of `dos4gwqa.gml`, `errors.gml`, `pgdos32q.gml`, and the
  redist `dos4gw.doc` — no occurrence of `8f`, `0x8f`, or `8Fh`).

### 3.2 What `INT 0x8F` *almost certainly is* (high-confidence inference)

The Watcom run-time library and DOS/4GW between them reserve a
handful of vectors in the BASIC-reserved 80h–F0h range as private
mode-switch / call-back trampolines. The technique is widely used
by DOS extenders:

- A 32-bit code page in the PM client wants to call into the
  extender (or have the extender dispatch a callback).
- An `INT n` instruction is a single 2-byte trap that reaches the
  extender's interrupt handler with full register state preserved.
  No call-gate setup; no LDT alias; no special selectors.
- The extender installs its own handler (via 0205h or directly into
  the IDT it controls) for a few "private" vectors and the client
  emits `INT n` at the call sites it cares about.

Patterns this matches:
- Watcom RTL's IRQ 13 reflector forwards to **INT 02h** (NMI
  vector), per OS2MUS-FPE.
- Watcom RTL's `signal()` machinery dispatches via a pseudo-vector
  set in the runtime support segment (the RTL defines a switch table
  internal to the C library).
- DJGPP/CWSDPMI uses `INT 0x21` at `0xCA:0` and several other
  fixed PM addresses as interrupt-bounce stubs.
- Pinecore itself uses `INT 0xF2` and `INT 0xF3` as private
  trampolines for RMCB dispatch and exception-return (pinecore:
  dpmi.c:887, dpmi.c:1893). DOS/4GW does the same trick — but with
  **its** chosen private vector, `0x8F`.

The strongest evidence that `INT 0x8F` is a *call-back / poll
trampoline*, not a "real" interrupt (i.e. not something to deliver
IRQs to), is its observed use pattern (pinecore-trace):

- It fires from DOS/4GW's own code segment (CS is the extender's CS,
  not the client's flat CS).
- It fires with the same EIP every time (0x6DCD and 0x6E4D are exact
  return addresses for `CD 8F`). That's a polling loop, not an
  irq-driven callback.
- It is preceded by a successful PM `INT 21h/0x48` chain delivery
  (pinecore: dpmi.c:1986–2024 chain logic). I.e. DOS/4GW just made a
  DOS allocation and is now trying to do something with the result.

**Most likely interpretation (speculation, but well-grounded):** at
EIP 0x6DCD/0x6E4D, DOS/4GW is **inside its own extender kernel,
*not* in the LE image yet,** and `INT 0x8F` is a "complete the
real-mode service that needs a context switch back to the caller"
trampoline. After a successful real-mode call (INT 21h/0x48), the
extender post-processes: it builds a selector for the new DOS block,
copies bytes around, then issues `INT 0x8F` to "return through the
extender's user-mode trampoline back to the caller." When `INT 0x8F`
unconditionally returns CF=0 with no state change (which is what we
do today), the extender has no way to know whether it succeeded, and
the next instruction loops back.

### 3.3 What we cannot confirm from public sources

I exhaustively searched:
- Open Watcom v2 (`bld/`, `docs/`, `contrib/`) — no `INT 8F` source.
  DOS/4GW *binaries* are redistributed in `bld/redist/dos4gw/` but
  the source is not in this repository (Open Watcom is open; DOS/4GW
  itself remains Tenberry-proprietary).
- Tenberry FAQ pages — 403/timed out (TENBERRY-FAQ; site is largely
  archived but blocks WebFetch).
- DJGPP/CWSDPMI mailing list archives — no INT 0x8F mentions in
  search.
- PMODE/W open source — does not use INT 0x8F (it's an open Watcom-
  compatible alternative; uses different internal mechanisms).
- Ralf Brown's Interrupt List — listing for INT 8F is the IBM ROM
  BASIC entry; nothing for DOS/4GW.

So we have **no primary source** confirming what `INT 0x8F` does in
DOS/4GW. The above is best-effort inference from extender-design
patterns + observed behavior.

### 3.4 What to do about it (recommendation, see also §8 for code)

Three options ranked by risk:

**Option A — return success and a "no operation completed" signal
in the registers.** Currently we return CF=0 + don't touch
registers. **(Speculation:** the loop is checking AX or some other
return value to see if a request completed. If we set AX=0
explicitly, the extender may interpret that as "done.") This is the
cheapest experiment. **Cost:** 5 lines in dpmi.c.

**Option B — treat INT 0x8F as a no-op that *does* advance the
caller past a wait-state.** Many extenders use a private vector as a
"yield" — the extender wants to be re-entered later when more state
is available. **Pinecore-specific approach:** instead of returning
to the same EIP, advance the caller's EIP past the next instruction
or two (e.g. past a comparison branch). This is dangerous because
we're patching code we don't understand.

**Option C — install an actual 4GW-internal handler for INT 8F via
0205h and see what happens.** DOS/4GW may itself install a PM handler
on INT 8F at startup. If we look at our `pm_vectors[0x8F]` after
init, we'll know. If the field is non-zero, we should be **delivering
to the client's handler**, not consuming the INT in the host. This
is the most likely real fix. **Action item:** add a serial trace at
the moment of dispatch: `if (vector == 0x8F) { dump pm_vectors[0x8F]
}`. (See §8.)

---

## 4. IRQ Delivery to PM Clients in DOS/4GW

DOS/4GW's interrupt model has three layers, all of which have to
agree with our kernel.

### 4.1 The three categories DOS/4GW distinguishes

(OW-DOS4GWQA §4a–4e is the primary source for everything in this
subsection.)

1. **Auto-passup IRQs (vectors 08h–2Eh except 21h).** "If the
   interrupt is in the auto-passup range, 8 to 2Eh; and you install
   a handler with INT 21h/25h or `_dos_setvect()`; and you do not
   install a handler for the same interrupt using any other
   mechanism. DOS/4GW will route both protected-mode interrupts and
   real-mode interrupts to your protected-mode handler" (OW-DOS4GWQA
   §4a). This is the *recommended* path for DOOM-style games.
2. **Second IRQ range (70h–77h).** *Not* auto-passup. "DOS/4G does
   allow you to specify additional passup interrupts" via
   environment variables / API (OW-DOS4GWQA §4b). DOOM uses IRQ 0
   (timer), IRQ 1 (keyboard), IRQ 8 (RTC unused), and possibly
   IRQ 5 (Sound Blaster) — all in the auto-passup range.
3. **Software interrupts.** Reflected to RM by default; PM handlers
   can be installed via INT 31h/0205h.

### 4.2 What "default" means when the client hasn't hooked

(OW-DOS4GWQA §4a, §4c.)

- If a hardware IRQ fires while the client is in PM and the client
  has *not* installed a PM handler, DOS/4GW reflects the IRQ to the
  real-mode handler (typically the BIOS/DOS default IRQ handler).
- The reflection is "pass-down": switch to RM, run the RM handler,
  return to PM (OS2MUS-FPE).
- The host (us) **must** acknowledge the PIC (EOI) on the way out
  if reflection failed or was a no-op. Otherwise IRQs stop
  re-firing.

### 4.3 What the FPE/IRQ 13 path does (OS2MUS-FPE primary source)

This is worth knowing because it's a specific case where DOS/4GW
installs a handler the application didn't explicitly ask for, and
where CauseWay (a competitor) gets it wrong:

1. DOS/4GW, at startup, hooks **IRQ 13 (INT 75h)** in PM.
2. When IRQ 13 fires (FPU error), DOS/4GW's handler:
   - Clears the FPU error (FCLEX or equivalent).
   - Sends EOI to both PICs.
   - Issues **INT 02h** in PM. (NMI vector — repurposed by the
     Watcom RTL as the FPE entry.)
3. The Watcom run-time library has previously hooked PM INT 02h
   via INT 31h/0205h (or via INT 31h/0203h for exception #16 in
   newer versions). The handler raises `SIGFPE`.
4. If the application has installed a SIGFPE handler via
   `signal()`, it runs. Otherwise the RTL terminates the program.

(OS2MUS-FPE is unambiguous about all of this; see also OW-DOS4GWQA
§4a discussion of "exceptions vs interrupts.")

**Implication for Pinecore:** if we don't ourselves emulate INT 75h
(IRQ 13) or trigger FPU errors, none of this matters for DOOM.
DOOM masks FPU exceptions in `I_Init()`. We can ignore the path.
But it's worth noting that **DOS/4GW will install a PM handler for
INT 75h** during init, and we'll see a 0205h call for BL=0x75 in
the trace.

### 4.4 What we currently do vs. what we should do

(pinecore: dpmi.c:1944 onward.)

- ✅ INT 31h goes to dpmi_int31. Correct.
- ✅ INT 21h with installed PM handler (DOS/4GW's own) gets
  delivered to the client handler chain. Correct.
- ✅ INT 21h without PM handler reflects through `dos_int21()`.
  Correct.
- ⚠️ Hardware IRQs: our IDT installs Ring-0 handlers (idt.c). For
  IRQ 0 we *do* call into dpmi_handle_pm_int when a client is
  active. For other IRQs, we currently EOI and drop. **(Inferred
  from dpmi.c trace logic — verify.)**
- ❌ Unhandled PM software INTs (no installed handler): we clear CF
  and return (pinecore: dpmi.c:2231). DPMI 0.9 (DPMI09 §B.1) says
  "unhandled PM software INTs are reflected to the RM IVT." We do
  NOT reflect; we silently succeed. This is fine for INT 0x8F if
  it's an extender-private call (§3), but it's wrong for any other
  software INT a future client might issue.

### 4.5 Concrete IRQ-delivery contract DOS/4GW expects from us

For each of IRQ 0 (timer / INT 08h), IRQ 1 (keyboard / INT 09h),
IRQ 5 (SB / INT 0Dh), IRQ 8 (RTC / INT 70h):

| Step | What we do at the host | When |
|------|------------------------|------|
| 1 | Save Ring-3 state in our IDT entry | At every IRQ |
| 2 | Look up client's PM handler via pm_vectors[vec] | If client active |
| 3 | If installed: build IRET frame on client's PM stack, redirect to handler@selector:offset, clear TF, mask IF in EFLAGS pushed (interrupt-gate semantics, DPMI09 §"INT 31h/0205h Notes") | Step 2 found a handler |
| 4 | If not installed: reflect to RM IVT[vec] — switch to V86, execute, return | Step 2 didn't find a handler |
| 5 | Always: EOI both PICs once on the actual return path | At end |

(pinecore: dpmi.c:2200 onward implements step 3. Step 4 is currently
the silent-succeed path — see §4.4.)

---

## 5. Mode-Switch Protocol — Confirming the Contract

This is what we already implement; this section just verifies that
the contract is right. The primary sources are DPMI09 §3 and
research 29 §"Mode Switch Entry."

### 5.1 Entry register state (V86 → PM)

| Register | Required value (32-bit client) |
|----------|--------------------------------|
| AX bit 0 | 1 (set by client to request 32-bit) |
| ES | RM segment of host private data, allocated via DOS INT 21h/48h for SI paragraphs |
| All other registers | Client-defined; we copy them into the PM frame |

### 5.2 Selector setup the host must do

(DPMI09 §3.2; research 31 §2.2.)

- CS: code32 selector, base = RM CS << 4, limit = 4GB (G=1, D=1),
  DPL=3, RPL=3, present, code, readable.
- DS: data32 selector, base = RM DS << 4, limit = 4GB or 64KB
  (depends; DOS/4GW immediately replaces this with a flat selector
  via 0007h/0008h, so 64KB is fine), DPL=3, present, data, R/W.
- SS: data32 selector, base = RM SS << 4, **default ESP = zero-
  extended SP**, limit = 4GB/64KB, B=1 (32-bit stack ops), DPL=3.
- ES: PSP descriptor, base = PSP << 4, limit = 0x100, data16, R/W,
  DPL=3.
- FS, GS = 0 (null).

### 5.3 EFLAGS on entry

DPMI09 §3.2 says "interrupt flag and other flags" should be
preserved from RM. In practice DOS/4GW sets up its own EFLAGS:
IF=1 (host enables interrupts after handing control to extender's
init code), IOPL=3 (so the client can execute CLI/STI/IN/OUT — see
research 29 §"Initial PM Environment"), TF=0, NT=0, RF=0, VM=0
(of course).

(pinecore: dpmi.c:309-313 sets IOPL=3 and IF=1; we are correct.)

### 5.4 The CS D/B bit — confirmed correct

(OW-DOS4GWQA does not discuss this directly; CWSDPMI's
control.c:469-493 sets D=1 for 32-bit clients.) For a DOOM-style
client with bit 0 of AX set, we MUST set CS.D=1, otherwise the
4GW kernel is decoded as 16-bit and immediately #UDs.

(pinecore: dpmi.c — `is_32bit` flag drives this; we are correct.)

### 5.5 Return contract

(DPMI09 §3.2.)

- CF=0 on success.
- Selectors loaded; client is at the LE image entry point in PM.
- Real-mode SS:SP is preserved on the host's stack, accessible
  via INT 31h/0306h (Get Raw Mode Switch Addresses) if the client
  needs to swap back.

---

## 6. DOOM-Specific Requirements

### 6.1 What we know with high confidence

The id Software DOOM source release (1997, GitHub
`id-Software/DOOM`) is the **Linux** port (`linuxdoom-1.10/`). The
DOS source was withheld because the DMX (Digital Music Express)
sound library was licensed (id Software's release notes are explicit:
"The bad news: this code only compiles and runs on linux. We
couldn't release the dos code because of a copyrighted sound
library we used"). The DOS source did leak in late 2023 and a
private archive is circulating, but we can't quote it directly.

Even from the Linux port, we can deduce DOOM's DOS calling pattern
by mapping each Linux `i_*.c` function to its DOS equivalent
(based on community ports like Chocolate DOOM and Crispy DOOM, and
the function comments in the linuxdoom source).

### 6.2 DOOM's system-services map

Source: `linuxdoom-1.10/i_*.c` files on `id-Software/DOOM`, mapped
to DOS calls.

| File | Linux function | DOS equivalent | DPMI calls touched |
|------|----------------|----------------|--------------------|
| i_main.c | `main()` calls `D_DoomMain()` | Same | None directly |
| i_system.c | `I_GetTime` uses `gettimeofday` | Reads BIOS tick counter at 0040:006C | INT 31h/0800h or RM read via 0300h |
| i_system.c | `I_Init` calls `I_InitSound`; graphics init commented out | DOS calls `I_InitGraphics` first, then sound | 0205h hooks for IRQs |
| i_system.c | `I_AllocLow` uses malloc | DOS uses INT 31h/0100h for DMA/transfer buffers below 1 MB | 0100h, 0101h |
| i_system.c | `I_Quit` shuts down sound, music, graphics | INT 10h/AH=00h to mode 03h text | 0300h |
| i_video.c | X11/SDL framebuffer | Direct writes to mapped 0xA0000 | 0800h (phys map of A000:0000 to PM linear) |
| i_video.c | X11 keyboard | INT 9 (IRQ 1) PM hook + IN 60h | 0205h on INT 9 |
| i_sound.c | OSS calls | Sound Blaster DSP via I/O ports + DMA | 0100h (DMA buffer), IRQ 5 hook via 0205h |
| i_net.c | UDP sockets | IPX driver, INT 7Ah | 0200h, 0300h |
| d_main.c | Main loop | Same | None |
| g_game.c | Game logic | Same | None |
| z_zone.c | Big-block allocator | Same — calls Z_Init once with the 0501h block | 0501h once at startup |

(All paths above derived from `id-Software/DOOM`, files
`linuxdoom-1.10/i_main.c`, `i_system.c`, `i_video.c`, `i_sound.c`,
`i_net.c`, `d_main.c`, `g_game.c`, `z_zone.c`. Citations are by
file basename, since the only authoritative copy is the GitHub
repo above.)

### 6.3 What DOOM does NOT use

- VESA/VBE — DOOM 1.x is mode-13h only (320×200×256 linear at
  A000:0000). Mode 13h means we only ever need a 64KB physical
  mapping at 0xA0000.
- Mouse — only used in the menu in early DOOM; INT 33h.
- DPMI 1.0 functions — never.
- Memory locking (0600h) is called once for the audio buffer (DMA
  needs to be physically contiguous & locked), but our non-paging
  kernel makes 0600h a no-op.

### 6.4 Critical INT 31h call set for DOOM (filtered from research 31 §8)

| Function | DOOM uses for | Already in pinecore? |
|----------|--------------|----------------------|
| 0000h | LDT alloc (flat model setup by 4GW) | ✅ |
| 0001h | LDT free | ✅ |
| 0007h | Set base | ✅ |
| 0008h | Set limit | ✅ |
| 0009h | Set access rights | ✅ |
| 0100h | Alloc DOS mem (DMA buffer) | (verify; line 800?) |
| 0200h | Get RM IVT vector | (verify) |
| 0201h | Set RM IVT vector | (verify) |
| 0203h | Set exception handler (RTL hooks all 32) | ✅ |
| 0204h | Get PM handler | ✅ |
| 0205h | Set PM handler (timer/keyboard/FPE) | ✅ |
| 0300h | Simulate RM int (BIOS, file I/O) | ✅ |
| 0303h | Alloc RM callback (mouse) | ✅ |
| 0500h | Mem info | ✅ |
| 0501h | Alloc memory | ✅ |
| 0502h | Free memory | ✅ |
| 0600h | Lock region (no-op) | ✅ |
| 0800h | Phys map (VGA 0xA0000) | ✅ |
| 0900h-0902h | Virtual IF | ✅ |

All marked ✅ are present; the (verify) entries should be confirmed
present by `grep -n 'case 0x010\|case 0x020' src/kernel/dpmi.c`.

---

## 7. The Unhandled INT 31h Functions Observed in Trace

The pinecore-trace post-init showed `0x000F`, `0x000D`, `0x001A`,
`0x001E`, `0x0022`, `0x0222` as unhandled INT 31h numbers. Triage:

### 7.1 `AX=000Dh` — Allocate Specific LDT Descriptor (DPMI 0.9)

(DELORIE-DPMI api/31000d.html.) Allocates a *specific* LDT slot
(input BX = selector). The first 16 LDT entries (selectors 0x04–
0x7C) are reserved for this function and clients use it for
interop with TSRs / other DPMI clients. **DOS/4GW uses this** to
reserve specific selectors that match Pharlap-style conventions
(for DOS/4G PRO compatibility; the 4GW kernel itself uses some
fixed slots).

**Minimum implementation:** if the requested selector is a free LDT
entry, allocate it; otherwise return CF=1 AX=0x8011 (descriptor
unavailable). For Pinecore, since we own the LDT and our first-
client setup uses indices 16+, slots 0..15 are typically free.

```c
case 0x000D: {
    int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
    if (idx <= 0 || idx >= DPMI_LDT_FIRST) {
        regs->eax = (regs->eax & ~0xFFFF) | 0x8011;
        return 1;
    }
    if (LDT_USED(c, idx)) {
        regs->eax = (regs->eax & ~0xFFFF) | 0x8011;
        return 1;
    }
    /* mark used, init as data r/w */
    c->ldt[idx].access = 0xF2;
    c->ldt[idx].limit_hi = 0x40;
    /* mark in bitmap */
    return 0;
}
```

### 7.2 `AX=000Eh` and `AX=000Fh` — Get/Set Multiple Descriptors (DPMI 1.0)

(DELORIE-DPMI api/31000e.html, api/31000f.html.) Bulk-read or
bulk-write LDT entries via a buffer at ES:(E)DI. Each entry in the
buffer is `{selector(2), descriptor(8)}` for 10 bytes per entry.

**These are DPMI 1.0 functions.** v1.97 doesn't normally use them
(OW-DOS4GWQA §0a confirms PRO supports 504h–50Ah but not 50Bh; for
000Eh/000Fh the answer is the same — DPMI 1.0 only). If we observe
them in the trace, *something is calling DPMI 1.0 functions* — most
likely the LE image's RTL is detecting a 1.0-capable host.

**Recommended action:** return CF=1 AX=0x8001 (unsupported).
Returning success here is dangerous — the caller will assume
descriptors were set but they weren't.

### 7.3 `AX=001Ah`, `AX=001Eh`, `AX=0022h`, `AX=0222h` — Not in DPMI 0.9 or 1.0

I cross-checked DELORIE-DPMI's complete function-by-number index
(DELORIE-DPMI ch5.n.html). The defined ranges are 0000h–000Fh,
0100h–0102h, 0200h–0205h + 0210h–0213h (1.0), 0300h–0306h, 0400h–
0401h, 0500h–050Bh, 0600h–0604h, 0700h–0703h, 0800h–0801h, 0900h–
0902h, 0A00h, 0B00h–0B03h, 0C00h–0C01h, 0D00h–0D03h, 0E00h–0E01h.

`001Ah`, `001Eh`, `0022h`, `0222h` are **not in the DPMI spec**.

The most likely explanations, in order of probability:

1. **Watcom/Tenberry vendor extension.** Some non-standard sub-
   functions Tenberry added; these can only be reached after a
   prior INT 31h/0A00h with `DS:ESI = "RATIONAL DOS/4G"` returns
   an entry point. Since our 0A00h returns CF=1 (we say
   "unsupported"), DOS/4GW shouldn't be calling these. **Unless**
   the LE image itself was built against a Tenberry SDK and is
   issuing these directly without 0A00h gating. **(Speculation.)**
2. **Trace artifacts.** Our trace logs `regs->eax & 0xFFFF`, but
   if we capture state mid-fault (e.g. during the auto-LDT
   cascade), the AX value may not be a real INT 31h call number —
   could be left over from a previous instruction. The cluster of
   bizarre numbers (0x222 in particular) suggests this. **(High-
   confidence speculation.)**
3. **DPMI 1.0 extensions to existing functions.** Some hosts add
   minor sub-functions that aren't in either spec.

**Recommended action:** for now, return CF=1 AX=0x8001 (unsupported)
for all four. If the trace re-captures these *after* we install
proper 000D/000E/000F handlers, we'll know it's #1 and need to
investigate the LE image directly. If they disappear, it was #2.

### 7.4 Summary table — the 6 unhandled funcs

| Func | Origin | Recommended pinecore action |
|------|--------|------------------------------|
| 000Dh | DPMI 0.9 (Allocate Specific LDT Descriptor) | **Implement.** See §7.1 code. |
| 000Fh | DPMI 1.0 (Set Multiple Descriptors) | Return CF=1 AX=0x8001. |
| 001Ah | Not in DPMI spec | Return CF=1 AX=0x8001. |
| 001Eh | Not in DPMI spec | Return CF=1 AX=0x8001. |
| 0022h | Not in DPMI spec | Return CF=1 AX=0x8001. |
| 0222h | Not in DPMI spec | Return CF=1 AX=0x8001. |

We already do "return CF=1 AX=0x8001" via the `default` arm of the
switch (pinecore: dpmi.c:1031–1051 the unhandled-trace case sets
`return 1`, and the caller sets CF=1; we should also set AX=0x8001
explicitly). Verify dpmi_int31's caller sets AX on error.

---

## 8. Next Implementation Steps for Pinecore

In priority order. Each step references dpmi.c line numbers from
the current tree (pinecore-trace and grep at time of writing).

### 8.1 Diagnose `INT 0x8F` before changing host behavior (HIGH)

Before adding any handler, get more telemetry. In
`dpmi_handle_pm_int` near pinecore: dpmi.c:2246, when vector ==
0x8F:

1. Dump `c->pm_vectors[0x8F].selector:offset` — is the client (or
   DOS/4GW itself, since both are "the client" from our POV)
   trying to install a PM handler for INT 8F via 0205h?
2. Dump `c->rm_vectors[0x8F].seg:off` if we track RM vectors.
3. Dump the 16 bytes *at* `pm_vectors[0x8F].selector:offset`
   (if non-zero) — the actual handler bytes. **(Speculation:** if
   DOS/4GW installs a handler that jumps to `c->client_cs:0`, that
   tells us this is an internal extender call.)
4. Dump 32 bytes *before* and *after* EIP=0x6DCD and 0x6E4D in the
   extender's CS to disassemble the loop. The bytes around 0x6DCB
   (`CD 8F`) and 0x6E49 (`CD 8F`) would tell us what the loop
   condition is — if it's `cmp ax, 0x...` then we know what AX
   value to return.

**File:** `/Users/chelsonaitcheson/Projects/dos-desktop/src/kernel/dpmi.c`
**Approximate line:** 2246 (existing dump code) — extend it.

### 8.2 Implement INT 31h/000Dh (Allocate Specific LDT Descriptor) (HIGH)

DOS/4GW probably needs this for some of its private LDT slots.
Without it, the auto-LDT cascade may be a side-effect of failed
000Dh calls returning CF=1, which DOS/4GW then converts to
"allocate any free slot" via 0000h.

**File:** `/Users/chelsonaitcheson/Projects/dos-desktop/src/kernel/dpmi.c`
**Insert after:** the `case 0x0001:` block, around dpmi.c:480
**Code:** see §7.1.

### 8.3 Make `default` arm of dpmi_int31 set AX=0x8001 (MEDIUM)

(pinecore: dpmi.c:1031–1051.) Currently we `return 1` and the
caller (`dpmi_handle_pm_int` at dpmi.c:1966–1969) sets CF=1 but
does NOT set AX. DPMI says callers may inspect AX for the error
code on CF=1. DOS/4GW v1.97's RTL definitely does this; an
unset AX will be interpreted as "extender error 0" or whatever
random value AX held coming in.

**Action:** in the `default` arm, before `return 1`, do:

```c
regs->eax = (regs->eax & 0xFFFF0000) | 0x8001;
```

### 8.4 Reflect unhandled PM software INTs to RM IVT (MEDIUM)

(pinecore: dpmi.c:2228–2232.) Currently we silently CF=0 return.
DPMI09 §B.1 says unhandled PM software INTs reflect to the RM IVT
handler. This is wrong specifically for INT 0x8F (it's an extender-
private vector, not a real IVT entry), so we should keep the
current "CF=0 return" behavior **only** for vectors known to be
extender-private. Add a switch:

```c
switch (vector) {
    case 0x8F:
    case 0xF0: case 0xF1: case 0xF2: case 0xF3:  /* our private */
        /* do not reflect; just CF=0 return */
        frame->eflags &= ~1;
        return 0;
    default:
        /* reflect to RM IVT (set up DPMI_REGS, call simulate-RM) */
        ...
}
```

**File:** `/Users/chelsonaitcheson/Projects/dos-desktop/src/kernel/dpmi.c:2228`

### 8.5 Trace 0205h calls and confirm DOS/4GW hooks INT 0x8F (MEDIUM)

In `dpmi_int31` case 0x0205, log:

```c
serial_puts("DPMI: 0205h hook: vec=");
serial_puthex(regs->ebx & 0xFF);
serial_puts(" → ");
serial_puthex(regs->ecx & 0xFFFF);
serial_puts(":");
serial_puthex(regs->edx);
serial_puts("\n");
```

If we see `vec=8F → <something>`, we now know the loop is *waiting
for that handler to be invoked*. The fix is to deliver to it. If we
*don't* see `vec=8F` ever, then INT 0x8F is not a handler-driven
mechanism and §3.2's interpretation is correct — DOS/4GW expects
the host (us) to **own** INT 0x8F and provide its semantics. In
that case the experiment in §8.1 will tell us what those semantics
are.

### 8.6 Add IRQ-reflection fallback for Step 4 of §4.5 (LOW; only matters post-DOOM-bringup)

When IRQs other than IRQ 0 fire and the client hasn't hooked them
via 0205h, reflect to the RM IVT. Today we EOI-and-drop, which is
fine for DOOM (it hooks everything it cares about) but breaks
generic DOS-extended apps.

**File:** `/Users/chelsonaitcheson/Projects/dos-desktop/src/kernel/idt.c`
(or wherever IRQ dispatch lives). **Out of scope for the current
DOOM bringup.**

### 8.7 Defer dpmi_timer_ready until after first 0205h hook for IRQ 0 (LOW)

(pinecore: dpmi.c:31, dpmi.c:920.) Currently `dpmi_timer_ready` is
set on the first RMCB allocation. The DPMI-correct gate is "client
has installed a PM handler for INT 8 via 0205h." Switch the gate.
This avoids spurious early timer interrupts during 4GW init.

---

## 9. Outstanding Questions / Caveats

1. **No primary source for `INT 0x8F`.** Section 3 is reasoned-out
   inference. The only way to confirm is the diagnostic in §8.1
   (dump 0205h hooks and disassemble the loop bytes) — or to obtain
   a leaked DOS/4GW source. **Action:** run the diagnostic, capture
   serial output, attach to next session.
2. **Auto-LDT cascade still poorly understood.** The 46
   #GP-driven LDT auto-allocations during init are a big code-
   smell. If DOS/4GW is failing to alloc descriptors via 0000h /
   000Dh, we may be silently corrupting its LDT view. **Action:**
   instrument case 0x0000 to print the count it requested vs. the
   first selector returned. We may discover we're satisfying calls
   for `CX > 1` with non-contiguous slots (which violates DPMI09
   §3.1).
3. **DOS/4GW v1.97 banner suggests we are running 1.97.** Confirm
   by reading the first 256 bytes of the EXE we load and looking
   for "DOS/4GW Protected Mode Run-time Version 1.97." This isn't
   load-bearing but ensures our research targets the right binary.
4. **DOOM 1.9 vs. Ultimate Doom vs. Final Doom.** All ship 4GW
   v1.97 *or earlier*. Some final-doom builds (TNT, Plutonia)
   ship a slightly different stub. If we find DOOM working but
   TNT/Plutonia not, the difference is the stub, not the LE.
5. **The DOS-source leak.** A 2023 leak contains the original DOS
   DOOM (`d_main.c`, `i_*.c` for DOS) — confirms the call set in
   §6.2. We cannot quote it directly but its existence corroborates
   the inference chain.

---

## 10. Summary Cheat-Sheet

| Ask | Short answer | Detailed in |
|-----|--------------|-------------|
| What is DOS/4GW? | Tenberry's 32-bit DOS extender, Watcom-bundled limited edition. v1.97 ships with DOOM. | §1 |
| What does DOS/4GW do at startup? | Detect DPMI, switch to PM, allocate big block, build flat selectors, load LE image, hook exceptions, jump to client. | §2 |
| What is INT 0x8F? | An extender-private vector with no public documentation. **(Speculation:** internal trampoline / poll site.) Three diagnostic paths in §8.1. | §3 |
| How do IRQs get to the client? | DOS/4GW's "auto-passup" model: if client hooked via 0205h or INT 21h/25h, deliver to PM; else reflect to RM. | §4 |
| What's the mode-switch contract? | DPMI09 §3.2; we already implement it. CS.D=1 for 32-bit, IOPL=3, IF=1. | §5 |
| What does DOOM use? | INT 21h (file I/O), INT 10h (mode 13h once), INT 33h (mouse), IRQs 0/1/5, 0x800h for VGA. Mode 13h only — no VESA. | §6 |
| Are 000F, 000D, 001A, 001E, 0022, 0222 real DPMI calls? | 000Dh yes (DPMI 0.9), 000Fh yes (DPMI 1.0), the rest no. | §7 |
| Code changes? | Implement 000Dh; trace 0205h to see if 0x8F is hooked; dump `INT 0x8F` site bytes; set AX=0x8001 on default error. | §8 |

---

## 11. References (consolidated)

| Tag | Source | URL / path |
|-----|--------|------------|
| OW-DOS4GWQA | Open Watcom v2 — "DOS/4GW Q&A" | <https://github.com/open-watcom/open-watcom-v2/blob/master/docs/doc/rsi/dos4gwqa.gml> |
| OW-RSIERR | Open Watcom v2 — DOS/4G error codes | <https://github.com/open-watcom/open-watcom-v2/blob/master/docs/doc/rsi/errors.gml> |
| OW-PGDOS32Q | Open Watcom v2 — Programmer's Guide DOS32 | <https://github.com/open-watcom/open-watcom-v2/blob/master/docs/doc/pg/pgdos32q.gml> |
| OW-RSIACC | Open Watcom v2 — RSI debug trap (DOS/4GW protocol) | <https://github.com/open-watcom/open-watcom-v2/blob/master/bld/trap/lcl/dos/dosx/rsi/c/rsiacc.c> |
| OS2MUS-FPE | Necasek, "Floating-Point Exceptions and DOS Extenders" | <http://www.os2museum.com/wp/floating-point-exceptions-and-dos-extenders/> |
| DELORIE-DPMI | DJ Delorie's DPMI 1.0 mirror | <https://www.delorie.com/djgpp/doc/dpmi/> |
| DPMI09 | DPMI 0.9 spec (Phatcode mirror) | <https://www.phatcode.net/res/262/files/dpmi09.html> |
| D4G-UNOFF | DOS/4G(W) Unofficial Programmer's Guide | <https://rgmroman.narod.ru/Dos4g.htm> |
| TENBERRY-FAQ | Tenberry FAQ index (often unavailable) | <http://tenberry.com/dos4g/faq/index.html> |
| WIKI-DOS4G | Wikipedia, DOS/4G | <https://en.wikipedia.org/wiki/DOS/4G> |
| PIKUMA-D4G | Pikuma, "DOS/4GW and Protected Mode" | <https://pikuma.com/blog/what-is-dos4gw-protected-mode> |
| DOOMWIKI-D4G | DoomWiki, DOS/4GW | <https://doomwiki.org/wiki/DOS/4GW> |
| CWSDPMI | CWSDPMI r7 source (CW Sandmann) | local: cwsdpmi-master/src/ |
| STANISLAVS-INTS | HelpPC interrupt table | <https://stanislavs.org/helppc/int_table.html> |
| CTYME-INT8F | RBIL HTML, INT 8F page | <https://www.ctyme.com/intr/int-8f.htm> |
| DOOM-LINUX | id Software Linux DOOM source | <https://github.com/id-Software/DOOM/tree/master/linuxdoom-1.10> |
| pinecore: dpmi.c | Local DPMI host implementation | /Users/chelsonaitcheson/Projects/dos-desktop/src/kernel/dpmi.c |
| pinecore-trace | Live serial output captured during DOS/4GW init | session 18 logs |

---

*Last updated: 2026-05-09. Status: actionable. Next session should
run §8.1's diagnostic before touching the host.*
