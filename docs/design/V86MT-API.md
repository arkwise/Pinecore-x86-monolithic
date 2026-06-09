# V86MT — V86 Multitasking DPMI Vendor Extension

**Status:** Draft v0.1 (2026-06-04) — pre-implementation. The API surface is the portability anchor; lock it before code lands.

**Purpose:** A vendor extension to DPMI 0.9/1.0 that adds (a) headless V86 task spawning and (b) a tty bridge so a PM client (a graphical DOS desktop) can host one or more windowed text consoles, each running a real DOS program — typically `COMMAND.COM` but any `.COM` / `.EXE` works the same.

**Reference hosts:**
- **pinecore** — kernel-native implementation. First reference impl. Pinecore is the DPMI host already (it has the V86 monitor, LDT/page-table management, INT 21h emulation), so V86MT slots in as a vendor extension to the existing `dpmi.c` INT 31h dispatch.
- **CWSDPMIX** — FreeDOS-side TSR extender. Fork of CW Sandmann's CWSDPMI that adds the V86 monitor, scheduler, and tty bridge. See [`CWSDPMIX.md`](CWSDPMIX.md). Not built yet; the design exists so the API is shaped to be implementable in both worlds.

**Client targets** (any DJGPP / Watcom-built DOS desktop):
- Pinecone Desktop (this repo)
- Seal (Allegro-based, GPL)
- Cube, Ozone, Pineapple 2 — same pattern
- Any future DPMI client that wants windowed shells

---

## Design principles

1. **One API, two hosts.** A client library (`libv86mt`) wraps the vendor INT 31h calls. Identical calling code works against pinecore or CWSDPMIX. Clients never see the host difference.

2. **Register-passed args where possible.** Struct-on-the-wire layouts are the #1 portability hazard (alignment, padding, packing, 16-vs-32-bit fields). Pass scalar args in registers; reserve structs for buffers the host writes (e.g. `vt_poll` state).

3. **DPMI-spec idioms.** Use standard DPMI conventions: selectors for linear ranges, CF=1 + AX=error for failure, byte-counted handles. Anyone who's written a DPMI client should read the API spec and feel at home.

4. **Versioned at the signature.** Discovery returns a 16-bit version. v1 has the eight calls below; v2 can add (graphics mode V86, mouse, serial pass-through) without breaking v1 clients. Clients pin the version they were compiled against and skip calls the host doesn't advertise.

5. **No required dependencies on DPMI 1.0.** v1 of the API works on a DPMI 0.9 host. (DPMI 1.0's nested-client semantics may simplify implementation but aren't on the client-side critical path.)

6. **No host-internal types in the API.** The wire format never exposes "pinecore client slot index" or "CWSDPMIX task ID" — only the opaque 16-bit handle defined here.

---

## Discovery

Vendor extensions in DPMI are discovered through INT 31h **AX = 0x0A00** ("Get Vendor Specific API Entry Point"). The convention:

```
INT 31h  AX=0x0A00  DS:[ESI] = ASCII vendor signature (zero-terminated)
On success:        CF=0, ES:EDI = vendor-API entry point (a far-call gate
                   or wrapper procedure inside the host)
On no-match:       CF=1, AX=0x8001 (Unsupported function)
```

**V86MT vendor signature:** `"V86MT v1"` (8 chars + NUL, exactly 9 bytes).

The returned entry point is a 32-bit PM far-call procedure: clients invoke it with the V86MT function number in AX and arguments in other registers, matching the per-call conventions below. Using a vendor-API entry point (rather than reusing INT 31h directly) means the V86MT calls don't collide with future DPMI 1.x function-code allocations and don't burden hosts that lack V86MT — they just return `0x8001` on discovery and the client falls back gracefully.

**Capability bits.** After successful discovery, the client calls **function 0x0000** (`v86mt_get_caps`) through the entry point. Returns:

```
AX = caps_lo, BX = caps_hi   (a 32-bit feature bitfield)
ECX = max_vts                (concurrent VT handles supported)
DX  = api_minor              (e.g. 1 = "v1.1", 0 = "v1.0 baseline")
```

Capability bits (v1):
- bit 0  — text VTs supported (always 1 in v1)
- bit 1  — `vt_kbd_inject` supports extended scancodes
- bit 2  — `vt_resize` supported beyond 80×25
- bit 3  — multiple concurrent VTs (else `max_vts = 1`)
- bits 4–31 — reserved, must be zero

A v1.0 baseline host advertises `caps = 0x0001`, `max_vts = 1`. That's the minimum a client can rely on — one text VT in 80×25 — and any other capabilities are negotiated.

---

## The API (v1)

Each call documented as: **function number, register-in, register-out, errors**. All operate through the vendor-API entry point returned by INT 31h AX=0x0A00.

### `v86mt_get_caps` — function 0x0000

Already covered under Discovery.

### `v86mt_vt_alloc` — function 0x0001

Allocate a headless VT. Returns a handle and three LDT selectors mapping the VT's char buffer, attribute buffer, and keyboard-injection ring.

```
In:   AX = 0x0001
      BL = cols (typically 80; capped at host's max)
      BH = rows (typically 25)
Out:  CF=0 on success:
        AX = handle (1..max_vts; 0 is invalid)
        BX = char_sel    (16-bit LDT selector, RO, base=shadow chars, limit=cols*rows-1)
        CX = attr_sel    (16-bit LDT selector, RO, base=shadow attrs, limit=cols*rows-1)
        DX = kbd_sel     (16-bit LDT selector, RW, points at host-mapped keyboard ring buffer
                          described below)
        EDI = host-managed; reserved, must not be touched by client
      CF=1 on failure:
        AX = error code
```

The client renders the char buffer by reading from `char_sel:0` (codepoint stream, IBM CP437) plus `attr_sel:0` (Color/blink attribute bytes — same encoding as text-mode video memory).

The keyboard ring is a fixed-layout buffer described under `v86mt_kbd_inject`.

Errors:
- `0x0001` — no free VTs (already allocated `max_vts`)
- `0x0002` — cols/rows out of range
- `0x0008` — out of LDT descriptors

### `v86mt_vt_spawn` — function 0x0002

Launch a real-mode binary into a VT.

```
In:   AX = 0x0002
      BX = handle (from vt_alloc)
      DS:ESI = far ptr to "argv0\0arg1\0arg2\0\0" packed string (zero-terminated args,
               double-zero terminates the list; argv[0] = path to .COM/.EXE)
      ES:EDI = far ptr to "env block" formatted exactly like a DOS environment
               (NUL-terminated VAR=VALUE strings, double-NUL terminates; trailing
               "\1\0" + program-name string per DOS convention).  May be NUL=NUL
               (empty env), in which case the host inherits the parent's env.
Out:  CF=0 on success: AX = 0  (task is launched; use vt_poll to observe state)
      CF=1 on failure: AX = error code
```

Errors:
- `0x0010` — invalid handle
- `0x0011` — VT already has a task
- `0x0012` — exec failed (file not found, bad MZ, insufficient memory)
- `0x0013` — argv string malformed

The host is responsible for: allocating real-mode memory for the task, building a PSP, copying the argv into PSP+0x80 (DOS command tail format), creating a fresh DOS state (own DTA, own JFT, own CWD per drive), and starting execution. The task runs preempted from the host's timer ISR.

### `v86mt_kbd_inject` — function 0x0003

Push a keystroke into a VT's keyboard ring. Used when the desktop has focus and wants to forward keys to a windowed shell.

```
In:   AX = 0x0003
      BX = handle
      CX = scancode_ascii pair:
           CH = BIOS scancode (set bit 7 for extended keys when caps bit 1 is set)
           CL = ASCII / IBM character (0 = function key with no ASCII)
Out:  CF=0 on success: AX = 0
      CF=1 on failure: AX = error code
```

Errors:
- `0x0010` — invalid handle
- `0x0020` — ring full (client should retry or drop)

**Ring buffer format** (read by the host's virtual INT 16h emulator):
- The ring lives at `kbd_sel:0` (16 bytes header + N × 2 bytes for entries).
- Header: `head` (word, 0..N-1) + `tail` (word, 0..N-1) + `size` (word, N) + `flags` (word — bit 0 = "has-shift", bit 1 = "has-ctrl", bit 2 = "has-alt" — modifier mirror).
- Entries: each is `(scancode << 8) | ascii`, exactly the BIOS INT 16h AH=0x10 return format.
- The host updates `tail` (consumer); the client updates `head` (producer). Standard SPSC ring. **Both sides MUST use atomic word writes** for head/tail; the entry payload is written before bumping `head`, and the host reads `tail` before reading the entry.

Why a shared ring instead of "host call to enqueue one key"? Because high-key-rate forwarding (paste of a 200-char string) should not require 200 mode switches. The ring lets the client batch.

### `v86mt_poll` — function 0x0004

Non-blocking inspection of VT state. The desktop calls this once per frame to know whether to repaint, whether the task exited, etc.

```
In:   AX = 0x0004
      BX = handle
      ES:EDI = far ptr to `vt_state` struct (32 bytes, see below)
Out:  CF=0 on success: AX = 0  (state struct populated)
      CF=1 on failure: AX = error code
```

**`vt_state` struct** (exactly 32 bytes, packed, little-endian; no compiler-specific padding):

```c
struct vt_state {
    word16  task_running;     /* +0:  1 = task is alive, 0 = exited (or not spawned) */
    word16  exit_code;        /* +2:  DOS exit code (valid when task_running=0 && exited=1) */
    word16  exited;           /* +4:  0 = never spawned or still running; 1 = task ended */
    word16  screen_dirty;     /* +6:  1 if char/attr buffers changed since last poll; host clears
                                       this counter when client reads (counter monotonic — clients
                                       can compare two reads, no race) — actually a 16-bit counter
                                       that increments per dirty event, wrapping at 0xFFFF */
    word16  cursor_x;         /* +8:  0..cols-1 */
    word16  cursor_y;         /* +10: 0..rows-1 */
    word16  cursor_visible;   /* +12: 1 = drawn, 0 = hidden */
    word16  cols;             /* +14: current text-mode columns */
    word16  rows;             /* +16: current text-mode rows */
    word16  kbd_drops;        /* +18: count of keys dropped due to ring-full (informational) */
    word16  reserved_a;       /* +20: must be 0 */
    word32  ticks_consumed;   /* +22: cumulative timer ticks this task got (debug/stats) */
    word16  reserved_b;       /* +26: must be 0 */
    word16  reserved_c;       /* +28: must be 0 */
    word16  api_version;      /* +30: echoes the host's v86mt version (e.g. 0x0100 for v1.0) */
};
```

Errors:
- `0x0010` — invalid handle
- `0x0030` — bad buffer pointer (segment limit too small, page fault on write)

### `v86mt_focus` — function 0x0005

Tell the host which VT is foreground. Foreground VT receives real keyboard input directly (the host's hardware INT 9h ISR routes scancodes to its kbd ring). Background VTs only see keys the client `kbd_inject`s. There can be at most one foreground VT at a time; passing handle=0 clears focus (the PM client owns input — e.g. when its desktop title bar has focus).

```
In:   AX = 0x0005
      BX = handle (0 = "no VT, client owns keyboard")
Out:  CF=0 on success: AX = 0
      CF=1 on failure: AX = error code
```

Errors:
- `0x0010` — invalid handle (non-zero handle that doesn't exist)

**Pause policy.** Background VTs continue executing (per pinecore's existing `vt_switch_policy` for graphics-mode DPMI clients — see `project_vt_switch_policy` memory). A future v2 capability bit may add explicit pause/resume; for v1 it's run-always.

### `v86mt_vt_resize` — function 0x0006 (capability bit 2)

Change the text-mode dimensions of a VT. Allocates new buffers, reassigns the LDT selectors. The existing char/attr/kbd_sel values returned by `vt_alloc` are invalidated on success — clients must re-fetch via a follow-up call (TBD: probably a `vt_query_selectors`, function 0x0008).

```
In:   AX = 0x0006
      BX = handle
      CL = new cols
      CH = new rows
Out:  CF=0 on success: AX = 0
      CF=1 on failure: AX = error code
```

Errors as for alloc, plus:
- `0x0010` — invalid handle
- `0x0014` — task is running (some hosts may require killing the task first to resize)

### `v86mt_vt_kill` — function 0x0007

Force-terminate the V86 task in a VT without releasing the VT. The task gets the equivalent of `INT 21h AH=0x4C AL=0xFF`. State is preserved (char/attr buffers stay readable for "show last output"); a follow-up `vt_spawn` reuses the VT.

```
In:   AX = 0x0007
      BX = handle
Out:  CF=0 on success: AX = 0
      CF=1 on failure: AX = error code
```

Errors:
- `0x0010` — invalid handle
- `0x0015` — no task to kill (returns success in baseline impls; failure is for hosts that distinguish)

### `v86mt_vt_free` — function 0x0008

Release the VT entirely. Kills any running task, frees the char/attr/kbd buffers, releases the LDT selectors. The handle is invalid after this call.

```
In:   AX = 0x0008
      BX = handle
Out:  CF=0 on success: AX = 0
      CF=1 on failure: AX = error code
```

Errors:
- `0x0010` — invalid handle

---

## Error code summary

| Code     | Name                       | Meaning                                          |
|----------|----------------------------|--------------------------------------------------|
| `0x0001` | `V86MT_E_NO_FREE_VT`       | All VT slots are allocated                       |
| `0x0002` | `V86MT_E_BAD_DIMENSIONS`   | cols/rows out of range                           |
| `0x0008` | `V86MT_E_NO_LDT`           | LDT descriptor pool exhausted                    |
| `0x0010` | `V86MT_E_BAD_HANDLE`       | Handle is not a live allocation                  |
| `0x0011` | `V86MT_E_TASK_LIVE`        | VT already has a running task                    |
| `0x0012` | `V86MT_E_EXEC_FAILED`      | spawn failed (file, MZ, memory)                  |
| `0x0013` | `V86MT_E_BAD_ARGV`         | argv string malformed                            |
| `0x0014` | `V86MT_E_BUSY`             | Operation requires no running task               |
| `0x0015` | `V86MT_E_NO_TASK`          | Operation requires a running task                |
| `0x0020` | `V86MT_E_RING_FULL`        | kbd ring buffer full                             |
| `0x0030` | `V86MT_E_BAD_BUFFER`       | Client-passed buffer is unreadable/unwritable    |
| `0x8001` | `DPMI_E_UNSUPPORTED`       | (standard) Function not implemented              |

Codes follow DPMI convention: low 8 bits are a sub-error, high 8 bits group the family. Hosts may add `0x80xx`-range proprietary codes; clients should treat any unknown code as a hard error.

---

## Threading and reentrancy

- All vendor-API calls are **synchronous and non-blocking** from the client's perspective.
- The host MUST be reentrant against its own INT 21h / INT 31h reflection paths. (A v86 task's INT 21h reflection happens on the host's timer or trap path; the PM client may call `vt_poll` while a V86 task is mid-INT-21h — implementations have to handle that.)
- The PM client MUST NOT call vendor-API functions from a real-mode callback (RMCB) — those run on a tightly locked stack and have no reentrancy budget. v2 may relax this.

---

## Versioning rules

- The signature string `"V86MT v1"` covers ALL of v1.x. Minor bumps (v1.1, v1.2…) add capability bits and new function numbers; existing functions never change their register conventions or struct layouts.
- A breaking change requires a new signature: `"V86MT v2"`. v2 hosts SHOULD also respond to `"V86MT v1"` for compatibility — they advertise a v1-flavored entry point that wraps v2 semantics.
- The `api_version` field in `vt_state` is the host's actual version. Clients can use it to enable opt-in fast paths.

---

## Test contract

A V86MT host conforms iff it passes:

1. **Discovery** — INT 31h AX=0x0A00 with `"V86MT v1"` returns a valid entry point. The entry point's function 0x0000 returns plausible caps.
2. **Lifecycle** — `vt_alloc → vt_spawn(echo via COMMAND.COM) → poll until task_running=0 → vt_free` completes without leaking LDT or memory.
3. **TTY round-trip** — after spawning `COMMAND.COM`, the char buffer shows the prompt `C:\>` (or equivalent) within 500 ms. Injecting `"DIR\r"` produces a directory listing in the buffer within 2 s.
4. **Kbd ring** — injecting 256 keys back-to-back, no drops if the consumer (the V86 task's INT 16h) drains fast enough. If drained at <half the inject rate, the host MUST report drops via `kbd_drops`.
5. **Concurrency** (capability bit 3) — two VTs running `COMMAND.COM` simultaneously, each receives its own keystrokes via `kbd_inject`, each shows independent output.
6. **Resource hygiene** — alloc/spawn/kill/free 100 times in a loop; no LDT, memory, or handle leaks.

A reference test program (`tools/v86mt-conformance.c` — TBD) runs these against any host.

---

## Open questions (decide before v1 freezes)

1. **Path / arg encoding** — is argv ASCII or OEM CP437? DOS itself is CP437, but DJGPP toolchains pass ASCII through `__argv`. Probably mandate CP437 for the vendor call and convert on the client side.
2. **`vt_query_selectors`** — needed for `vt_resize` re-fetch. Add as function 0x0009?
3. **Mouse forwarding** — currently out of scope. v2 capability?
4. **Foreground-VT keyboard takeover semantics** — when a VT is foreground, does the desktop still get ESC / Alt-Tab to switch back? Recommendation: host reserves a "magic key" (configurable, default Ctrl-Alt-Q) that always returns input to the PM client. Spec it before v1 freeze.
5. **Shared filesystem state** — each VT gets independent CWD per drive, independent DTA, independent JFT, but what about the global FCB chain, share table, etc.? Probably "the host best-efforts isolates; clients that share won't notice." Document the limits.
6. **Standard-DOS-version probe** — when a VT runs, what `INT 21h AH=0x30` says. Should match parent DOS. Don't lie.

---

## Relation to the existing roadmap

The pinecore-side implementation of V86MT corresponds to Phase 4.7 ("Tier 3 prompt" — see `prompt.h` for the original tier description and `roadmap.md`). This document supersedes the loose "headless-VT DPMI services" placeholder there with a concrete API.
