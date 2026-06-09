# CWSDPMI IRQ Delivery Mechanism — Session-36 Reference Reading

**Status:** RESEARCH, not yet implemented in pinecore-x86. Read in s36 dynamic-loop iteration 1.

**Source files (local at `/Users/chelsonaitcheson/Downloads/cwsdpmi-master/src/`):**
- `exphdlr.c:40-76` — author-written architecture comment, IS the spec.
- `exphdlr.c:108-115` — vector-translation table.
- `exphdlr.c:166-218` — PIC remap (`init_controllers`).
- `tables.asm:155-294` — per-IRQ ring-0-to-ring-3 stubs (`_irq0`/`_irq1`/.../`irq_common`).
- `mswitch.asm` — `user_interrupt_return` trampoline (not yet read in detail).

---

## Architecture in one paragraph

CWSDPMI **remaps the master PIC** from BIOS-default 0x08 to an empty IDT region (typically 0x88) so BIOS `int 08h` IVT entries stay intact for V86 fallback. When a hardware IRQ fires while a PM client is on the CPU, the CPU dispatches to CWSDPMI's IDT entry, which is a small per-IRQ stub (`_irqN`) that pushes the IRQ number then jumps to `irq_common`. `irq_common` saves a few registers, **switches to a 4 KB locked stack** (`_locked_stack` — pages are pinned, won't take page faults during the handler), copies the CPU-pushed ring-change IRET frame onto the locked stack, and pushes a second frame above it pointing at the user's registered handler. It then modifies the original IRET-frame slots on the ring-0 stack so that an `IRETD` from ring-0 transitions to the user handler at the new SS:ESP. When the user's handler does its own `IRETD`, the locked-stack IRET frame pops it into `user_interrupt_return` — a small trampoline that emulates a final IRET back to the original interrupted ring-3 code, restoring SS:ESP.

## Vector translation

From `exphdlr.c:110-111`:

```c
word8 hard_master_lo=0x08, hard_master_hi=0x0f;
word8 hard_slave_lo=0x70,  hard_slave_hi=0x77;
```

After PIC remap, `hard_master_lo` may be 0x88 or similar — but the **PM-side mapping** stays canonical:

| IRQ line | DPMI PM INT vector | `user_interrupt_handler[]` index |
|----------|--------------------|-----------------------------------|
| 0 (PIT)  | 0x08               | 0                                 |
| 1 (KBD)  | 0x09               | 1                                 |
| 2 (cascade) | 0x0A             | 2                                 |
| 3-7      | 0x0B-0x0F          | 3-7                               |
| 8 (RTC)  | 0x70               | 8                                 |
| 9-15     | 0x71-0x77          | 9-15                              |
| INT 1Ch (BIOS timer tick) | 0x1C | 16                       |
| INT 23h (Ctrl-Break) | 0x23     | 17                                |

`n_user_hwint = 18` (per `exphdlr.c:79`).

Clients register handlers via INT 31h `0x0205` using the **DPMI PM INT vector number** (the middle column). We already do this — `pm_vectors[0x08]` etc. The remap base only affects where the IDT entries live in CWSDPMI's address space.

## The author's policy comment (`exphdlr.c:44-76` paraphrased)

1. **Default**: hardware IRQs that occur in PM are handled at ring 0 (CWSDPMI's IDT entries) and reflected to real mode via the IVT.
2. **If the user has not hooked a HW int, we do not reflect RM ints into PM at all.** — load-bearing rule.
3. **Once hooked** (via INT 31h `0x0205`), HW interrupts that occur in PM jump to the irq-routines in `tables.asm` (ring-0), which move to a locked stack and change to ring-3 for the user routine.
4. The user's IRET goes to `user_interrupt_return` — emulates an IRET at the same ring but with stack change.
5. HW interrupts that occur in **RM** (V86) jump to RMCBs specially set up to call the user's routine (type != 0).
6. **Chaining is ugly.** CWSDPMI returns the original PM HW interrupt routine on `0x0204` calls (modifying CS to ring 3). A chained user-handler IRETD returns to that original PM handler. Reflection to RM would cause an infinite loop (RMCB → PM → RM → ...), so the saved RM handler is invoked via a specially-saved IVT pointer **only when a RMCB is installed**.
7. INT 1Ch only needs RMCB + chaining (no PM ring-0 handler).
8. INT 23h/24h are non-standard (carry-flag returns / AL=3 returns) — RMCB type 2, deferred.

## `irq_common` flow (the `run_ring=3` branch, `tables.asm:240-294`)

When called: CPU has just dispatched IRQ via interrupt gate, pushing the 5-dword ring-change IRET frame (`SS, ESP, EFLAGS, CS, EIP`). Our `_irqN` stub then pushed the IRQ number (1 word, after the `db push n; jmp short irq_common` macro expansion).

```asm
push bp; mov bp,sp                ; bp now indexes through the ring-change frame
push ds; push es                   ; save ds/es so we can use DPMI data segment
push esi; push edi; push ecx       ; save scratch regs

push g_pdata; pop ds                ; ds = DPMI data segment

; If locked_count == 0, switch to the locked stack at _locked_stack+4076
;   (4076 = 4096 - 20, leaving 20 bytes for the 5-dword ring-change frame)
; Otherwise reuse the existing locked stack ESP - 20.

inc _locked_count

mov ecx,5; rep movsd                ; Copy the ring-change IRET frame to locked stack
sub edi,32                          ; Make room for inner frame (12B) + 20B above

; Push the "return-to-trampoline" IRET frame at edi:
mov dword ptr es:[edi+8], 3002h      ; inner EFLAGS (IF=0, IOPL=3, reserved)
mov word  ptr es:[edi+4], g_pcode    ; inner CS
mov dword ptr es:[edi],   offset _user_interrupt_return   ; inner EIP

; Look up the user's handler far pointer:
xchg bx,[bp+2]                      ; bx = IRQ # times 6 (sizeof far32)
mov ecx, dword ptr _user_interrupt_handler[bx]            ; offset
mov dword ptr [bp+4], ecx           ; OVERWRITE outer IRET frame's EIP
mov cx, word ptr _user_interrupt_handler[bx+4]            ; selector
mov word ptr [bp+8], cx             ; OVERWRITE outer IRET frame's CS
mov word ptr [bp+12], 3002h         ; OVERWRITE outer EFLAGS
mov dword ptr [bp+16], edi          ; OVERWRITE outer ESP -> locked stack
mov word ptr [bp+20], es            ; OVERWRITE outer SS -> g_pdata

; Restore scratch and IRETD
pop ecx; pop edi; pop esi; pop es; pop ds; pop bp; pop bx
iretd
```

After this `iretd`, the CPU pops the **modified outer ring-change frame** and lands at:
- `CS:EIP = user_handler_sel:user_handler_off`
- `SS:ESP = g_pdata:edi (locked stack)`
- `EFLAGS = 0x3002` (IF clear, IOPL=3 — the user handler runs with interrupts disabled by default)

The locked stack at the new ESP has a single IRET frame:
- `[ESP+0..3]` = `offset _user_interrupt_return`
- `[ESP+4..5]` = `g_pcode`
- `[ESP+6..7]` = padding (16-bit selector slot pads to 32-bit)
- `[ESP+8..11]` = `0x00003002`

When the user's handler does `iretd`, this frame is popped and the CPU jumps to `_user_interrupt_return` (still in ring 3, on the locked stack — DPL of `g_pcode` is 3).

## `user_interrupt_return` (TBR — `mswitch.asm`)

Not yet read in detail. From the comment at `tables.asm:175` and `exphdlr.c:54-55`: it emulates an IRET at the same ring but changing stacks back to the original interrupted SS:ESP. The original 5-dword frame is still saved on the locked stack 20 bytes above where the user's handler started; the trampoline reads from there and constructs/executes an IRET-style jump to the real interrupted code.

## What we need to adapt for pinecore

Pinecore differences vs CWSDPMI:
1. **We don't remap the PIC away from the base** — our PIC is at base 0x20 (vectors 32-47). That's fine; we just translate vector→INT number at delivery time (already designed: `IRQ N → INT 0x08+N` for N<8, `INT 0x70+(N-8)` for N>=8).
2. **We have a preemptive scheduler** driven by RTC IRQ 8. CWSDPMI doesn't preempt. This is the meta-issue the user flagged in s36.
3. **No locked stack yet.** Our kernel-PF demand-pager catches uncommitted PM pages, so pushing onto the client's stack from the kernel's IRQ handler will recover from a fault. Acceptable for now — defer locked-stack to a later cleanup.
4. **No `user_interrupt_return` trampoline yet.** The simplest path: push a faux IRET frame on the client's stack pointing at a small thunk that issues `INT 0xF2` (an unused vector); the kernel catches `INT 0xF2` and IRETs to the original interrupted state which we stash in `c->irq_save` at delivery time. Pattern is analogous to the existing INT 0xF3 / `exc_save` exception-return path (`dpmi.c:2783` `if (vector == 0xF3 && c)`).

## Proposed implementation outline (for s37 iteration 1)

1. **Add `irq_save` substruct to `dpmi_client`** (parallel to existing `exc_save` and `rm_call_save`): fields `active`, `orig_eip`, `orig_cs`, `orig_eflags`, `orig_esp`, `orig_ss`.
2. **Add an IRQ-return thunk selector** (parallel to `pm_int_chain_sel` and `exc_return_sel`): allocate an LDT code selector pointing at 3 bytes in low memory containing `CD F2 CF` (INT 0xF2; IRET — defensive).
3. **Implement `dpmi_handle_pm_hw_irq(vector, esp)` in dpmi.c.** Don't reuse `dpmi_handle_pm_int` — its "unhandled vector ≥0x60" path clobbers EAX (s36 attempt 1's failure). New function:
   - Look up client via CS, return 0 if none.
   - Check `pm_vectors[vector].selector != 0` (real handler); return 0 if none.
   - Snapshot the IRQ-time frame into `c->irq_save`.
   - Push a 3-dword (32-bit client) IRET frame on PM stack at `frame->esp-12`:
     - `[+0]` = `irq_return_thunk_off` (typically 0)
     - `[+4]` = `irq_return_thunk_sel`
     - `[+8]` = `frame->eflags & ~IF`  (the saved IF is restored by the trampoline)
   - Modify `frame->cs/eip` to point at the user handler.
   - Modify `frame->eflags` to clear IF (handler runs with interrupts disabled).
   - Modify `frame->esp -= 12`.
   - Return new_esp (= original esp, since we modified the frame in place).
4. **Add `INT 0xF2` catch in `dpmi_handle_pm_int`** (parallel to existing `INT 0xF3` block at `dpmi.c:2783`): if vector is 0xF2 and `c->irq_save.active`, restore the saved frame from `c->irq_save`, clear `active`, return new_esp. The trampoline's `IRET` will land in the original interrupted code.
5. **Wire up in `idt.c`** PM-routing block: replace the s36 reverted attempt with `dpmi_handle_pm_hw_irq(pm_vec, esp)`. EOI before returning new_esp. Fall through to kernel handling if 0 returned.

## Risk register for s37 implementation

- **R1**: Allegro's PM INT 0x08 handler may itself try to chain to the "previous" handler via INT 31h `0x0204`. Our `0x0204` returns whatever was at `pm_vectors[0x08]` before Allegro set it — which is 0 (no previous handler). If Allegro tries to call 0:0, it #GPs. **Mitigation**: seed `pm_vectors[0x08..0x0F]` and `pm_vectors[0x70..0x77]` at PM-entry with a default chain stub that does `IRET` only (no-op handler), similar to how `pm_vectors[0x21]` is seeded with `pm_int_chain_sel`.
- **R2**: Allegro may EOI in its handler. We also EOI in the kernel before mode-switching. Double-EOI on master PIC is harmless (clears the highest-priority ISR; second EOI does nothing if no ISR is in service). Verify in trace.
- **R3**: User handler may modify state we didn't expect (e.g., re-enable interrupts via `STI` and then take another IRQ, recursing). DPMI 0x0901/0x0902 (Get/Enable virtual interrupt state) is supposed to virtualize this. Our host doesn't implement those services yet. **Mitigation**: ignore for now; revisit only if Allegro misbehaves.
- **R4**: Scheduler preemption while in the PM handler. `dpmi_busy` should be set across the IRQ-delivery path; while the user handler runs, `dpmi_busy=0`, so RTC IRQ can fire and preempt. That's fine for correctness, but means the handler may run slowly under load. **Mitigation**: acceptable for s37; revisit if measurements show preemption is starving the PM handler.

## Open questions for s38+

1. Do we ever need to virtualize hardware IRQs that fire while V86 is running, into the PM handler? CWSDPMI uses RMCBs for this (`exphdlr.c:57-58`). Our task model pauses V86 while PM runs, so the question only arises if a multi-task setup has PM and V86 alive simultaneously (e.g., a PM TSR + an active V86 task). Defer.
2. Locked-stack discipline. Currently rely on kernel-PF demand-pager. If we ever hit a PM-stack-#PF inside the IRQ-delivery push (the kernel itself is doing the push), it would reenter the kernel PF handler. Should work via `dpmi_kernel_pf_commit`, but worth verifying.

---

This memo lives at `docs/research/49-cwsdpmi-irq-delivery.md` so the implementation work can cite specific lines instead of re-reading CWSDPMI from scratch each iteration.
