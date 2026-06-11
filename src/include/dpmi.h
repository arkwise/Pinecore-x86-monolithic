#ifndef DPMI_H
#define DPMI_H

#include "types.h"

/* DPMI 0.9 Host — Pinecore implements DPMI services so DOS extenders
 * (DOS/4GW, PMODE/W) can run 32-bit protected mode applications.
 *
 * Architecture:
 *   V86 task runs DOS extender stub → detects DPMI via INT 2Fh/1687h →
 *   calls mode switch entry → kernel creates LDT, switches to Ring 3 PM →
 *   client calls INT 31h for services → kernel handles at Ring 0
 *
 * (ch-29, ch-30, ch-31)
 */

/* LDT configuration */
#define DPMI_LDT_ENTRIES   2048
#define DPMI_LDT_FIRST     16    /* entries 0-15 reserved for client's internal use */

/* Memory block tracking */
#define DPMI_MAX_MEMBLOCKS 64

/* Real-mode callback slots */
#define DPMI_MAX_RMCB      16

/* x86 segment descriptor (8 bytes) */
struct seg_descriptor {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;      /* P, DPL, S, Type */
    uint8_t  limit_hi;    /* flags: G, D/B, L, AVL + limit 19:16 */
    uint8_t  base_hi;
} __attribute__((packed));

/* DPMI register structure for INT 31h/0300h (simulate RM interrupt)
 * 50 bytes, same layout as DPMI spec */
struct dpmi_regs {
    uint32_t edi, esi, ebp, reserved_esp;
    uint32_t ebx, edx, ecx, eax;
    uint16_t flags;
    uint16_t es, ds, fs, gs;
    uint16_t ip, cs;
    uint16_t sp, ss;
} __attribute__((packed));

/* Memory block descriptor */
struct dpmi_memblock {
    uint32_t base;       /* linear address */
    uint32_t size;       /* bytes */
    uint8_t  active;
};

/* Per-client DPMI state */
struct dpmi_client {
    uint8_t  active;
    uint8_t  is_32bit;   /* 1 = 32-bit client */
    int      v86_task_id; /* originating V86 task */

    /* Phase 4.6 multi-mode VT switching: when the bound VT is in the
     * background, vt_switch sets this flag. Acts as a safety belt against
     * any future PM-IRQ delivery path that iterates clients without keying
     * on frame->cs — the lookup loops in dpmi_pm_has_handler and
     * dpmi_deliver_pm_irq skip flagged clients. Cleared on switch-toward
     * and on client release. */
    int      vt_paused;

    /* s40 — set when client exited via PM INT 21h AH=4Ch but the V86
     * task that hosted it is still running (i.e. COMMAND.COM is about
     * to print the return code and prompt again). DOS/4GW's V86 unwind
     * still queries our DPMI services (0x000B Get Descriptor,
     * 0x0204 Get PM Interrupt Vector, …) and expects REAL data — so we
     * keep this client's LDT + pm_vectors alive until sched_v86_exit
     * does the real teardown. The dpmi_enter_pm reuse path skips
     * pm_exited clients so a subsequent EXEC (e.g. DOOM after DESKTOP)
     * gets a fresh slot instead of inheriting the previous program's
     * state. */
    int      pm_exited;

    /* LDT — 128 entries * 8 bytes = 1024 bytes */
    struct seg_descriptor ldt[DPMI_LDT_ENTRIES];

    /* Initial selectors assigned at mode switch */
    uint16_t client_cs;
    uint16_t client_ds;
    uint16_t client_ss;
    uint16_t client_es;  /* PSP selector */

    /* Saved V86 state (for returning from PM to V86/DOS) */
    uint16_t v86_cs, v86_ds, v86_ss, v86_es;
    uint32_t v86_eip, v86_esp;

    /* Extended memory blocks */
    struct dpmi_memblock memblocks[DPMI_MAX_MEMBLOCKS];
    uint32_t next_linear;  /* next available linear address for alloc */

    /* PM interrupt handlers (client-installed via INT 31h/0205).
     * Indexed by BIOS-style INT number; consulted on PM software INT delivery
     * and on hardware-IRQ-to-PM reflection. */
    struct {
        uint16_t selector;
        uint32_t offset;
    } pm_vectors[256];

    /* CPU exception handlers (client-installed via INT 31h/0203).
     * Indexed by exception number 0..31; consulted when a CPU fault/trap
     * occurs in PM. Keep separate from pm_vectors[]: DJGPP installs CPU
     * exception 0x08 (double fault) at the same array index that hardware
     * IRQ 0 → INT 0x08 would use for PM delivery; conflating them routed
     * the PIT IRQ to DJGPP's exception-8 stub. */
    struct {
        uint16_t selector;
        uint32_t offset;
    } pm_exc_vectors[32];

    /* Real-mode callback slots.
     *
     * rm_mode = 0: PM handler. pm_sel:pm_off is a PM code sel:off.
     *              When the RMCB is invoked from V86, dpmi_rmcb_dispatch
     *              switches to PM and calls pm_sel:pm_off.
     * rm_mode = 1: V86 handler (s42). pm_sel is an RM segment; pm_off is
     *              the low-16 offset within that segment. regs_sel/off
     *              point at a register save buffer also in V86 memory.
     *              When invoked, no PM transition — dispatch is a V86
     *              far call to (pm_sel << 4) + pm_off, with regs saved
     *              into the buffer first (per DPMI 0.9 §3.4 RMCB spec).
     *              DOS/16M issues 0x0303 from V86 during cleanup with
     *              this shape; we implement it to satisfy its handshake
     *              and avoid the s42 host-error cascade. */
    struct {
        uint16_t pm_sel;      /* PM code selector OR RM segment */
        uint32_t pm_off;      /* PM offset OR RM offset (low 16 used) */
        uint16_t regs_sel;    /* PM data sel OR RM segment of regs buf */
        uint32_t regs_off;    /* PM offset OR RM offset of regs buf */
        uint8_t  active;
        uint8_t  rm_mode;     /* 0 = PM handler (default), 1 = V86 handler */
    } rmcb[DPMI_MAX_RMCB];
    int next_rmcb;

    /* Exception return thunk — LDT code segment containing INT 0xF3 */
    uint16_t exc_return_sel;

    /* INT 31h AX=0x0A00 "RATIONAL DOS/4G" vendor API entry point.
     * 32-bit DPL=3 code segment whose body dispatches by EAX subfunction:
     *   0x01 → returns EAX=0xABCD1234 (sanity check)
     *   else → RETF (no-op for unimplemented subfns) */
    uint16_t vendor_api_sel;

    /* INT 31h AX=0x0A00 "V86MT v1" vendor API entry point (Phase 4.7 M1+).
     * 32-bit DPL=3 code segment. Inline fast-path for AX=0x0000
     * (get_caps); all other sub-functions forwarded to INT 31h cases
     * 0x0A01..0x0A08 in dpmi_int31. See docs/design/V86MT-API.md. */
    uint16_t v86mt_api_sel;

    /* V86MT VT slot table (Phase 4.7 M2+).
     * v1.0 baseline advertises caps=0x0009, max_vts=4. M3 attaches a
     * headless shadow buffer (char + attr) per allocated VT. M4 routes
     * V86 INT 10h/21h writes into these buffers; M5 LDT-maps them into
     * the PM client. Fields mirror what vt_state poll (M5) reports. */
#define DPMI_V86MT_MAX_VTS  4
#define DPMI_V86MT_COLS    80
#define DPMI_V86MT_ROWS    25
    struct dpmi_v86mt_vt {
        uint8_t   used;             /* 1 = handle (i+1) is live */
        uint8_t   cols;
        uint8_t   rows;
        uint8_t   cursor_visible;
        uint16_t  cursor_x;
        uint16_t  cursor_y;
        uint16_t  screen_dirty;     /* monotonic 16-bit counter */
        uint16_t  kbd_drops;
        uint32_t  ticks_consumed;
        uint8_t  *char_buf;         /* cols*rows CP437 codepoints */
        uint8_t  *attr_buf;         /* cols*rows colour/blink attrs */
        /* Phase 4.7 M5 — LDT selectors mapping char_buf / attr_buf so the
         * PM client can read seg:0 directly. char_sel = attr_sel = 0
         * until they are populated by vt_alloc. */
        uint16_t  char_sel;
        uint16_t  attr_sel;
        /* Phase 4.7 M6 — keyboard ring + RW selector mapping it into the
         * client. Layout per docs/design/V86MT-API.md "Ring buffer format":
         * 16-byte header (head, tail, size, flags + 8 reserved) + N entries
         * of 2 bytes each (scancode<<8 | ascii). Client = producer (bumps
         * head), host's INT 16h emulator = consumer (bumps tail). */
        uint8_t  *kbd_buf;
        uint16_t  kbd_sel;
        /* Task lifecycle mirror — populated by vt_spawn / v86_destroy_task
         * for vt_poll reporting. */
        uint16_t  task_running;
        uint16_t  exited;
        uint16_t  exit_code;
    } v86mt_vts[DPMI_V86MT_MAX_VTS];
#define DPMI_V86MT_KBD_ENTRIES  32
#define DPMI_V86MT_KBD_BYTES    (16 + DPMI_V86MT_KBD_ENTRIES * 2)

    /* PM INT 21h chain endpoint — LDT code segment containing INT 21h; RETF.
     * Seeded into pm_vectors[0x21] before client installs its handler so that
     * INT 31h/0204 returns a valid sel:off. DOS extenders (DOS/4GW) chain to
     * the saved address via FAR CALL after translating logical → physical
     * handles. The kernel detects entry via this selector and dispatches to
     * dos_int21 directly instead of re-delivering to the client handler. */
    uint16_t pm_int_chain_sel;

    /* Virtual interrupt flag */
    uint8_t virtual_if;

    /* Exception-delivery save. Captured at dpmi_handle_pm_exception so we
     * can resume from the original ESP/SS even if the handler relocated
     * its stack. Also remembers the handler's bit-ness so the INT 0xF3
     * return path reads the right frame width. */
    struct {
        uint8_t  active;
        uint8_t  is_32;       /* matched handler D-bit at delivery time */
        uint32_t frame_base;  /* linear address where we wrote the frame */
        uint32_t orig_esp;
        uint32_t orig_ss;
        uint32_t seq;         /* s45 falsification: per-deliver sequence number,
                                 stamped at deliver time, dumped at return time
                                 so we can correlate deliver/return cycles. */
        /* s45 snapshot: original faulting context. Used as fallback when the
         * handler's return frame is bogus (CS=0 etc.). Empirical: DOS/4GW's
         * #GP handler does a STD;REP MOVSD that shifts the delivered frame
         * up by 8 bytes within the same buffer — fEIP/fCS/fEFL end up at
         * +0x14..+0x1F instead of +0x0C..+0x17. The values are unchanged
         * (handler doesn't actually modify them), so falling back to the
         * snapshot resumes the faulting context exactly as the handler
         * intended. */
        uint32_t orig_eip;
        uint32_t orig_cs;
        uint32_t orig_eflags;
    } exc_save;

    /* Synchronous RM-call save (INT 31h AX=0x0301/0x0302).
     * PM↔RM trampoline (see docs/research/29-dpmi-host.md): when a PM
     * client calls 0x0301/0x0302 we save the PM frame here, switch the
     * task to V86 mode running the target RM proc, and push an IRET-frame
     * on the V86 stack pointing at our sentinel (`CD F4 CF` at linear
     * DPMI_SENTINEL_LIN). When the RM proc IRETs/RETFs, control lands at
     * the sentinel; the INT 0xF4 V86-monitor handler reads the post-call
     * V86 regs into the caller's RmCallStruct and restores the PM frame
     * from this save. */
    struct {
        uint8_t  active;
        uint8_t  is_iret;            /* 1 = 0x302 (IRET), 0 = 0x301 (FAR) */
        uint32_t rm_struct_linear;   /* address of RmCallStruct */
        uint32_t pm_eax, pm_ebx, pm_ecx, pm_edx;
        uint32_t pm_esi, pm_edi, pm_ebp;
        uint32_t pm_eip, pm_eflags, pm_esp;
        uint16_t pm_cs, pm_ss;
        uint32_t pm_ds, pm_es, pm_fs, pm_gs;
    } rm_call_save;
};

/* LDT selector macros */
#define LDT_SEL(idx)     (((idx) * 8) | 4 | 3)  /* LDT, RPL=3 */
#define SEL_TO_IDX(sel)  ((sel) / 8)
/* Slot is "free" when it has never been allocated (access==0) OR has been
 * logically freed via 0x0001 (AVL bit in limit_hi set). The AVL-flagged
 * state keeps the descriptor physically loadable so the kernel's ISR
 * epilogue `pop %ds` of a just-freed selector — typical of DJGPP exit() —
 * doesn't #GP. The slot becomes truly reusable: ldt_alloc reinitializes
 * limit_hi on allocation, clearing AVL. */
#define LDT_FREE(c, i)  (!(c)->ldt[i].access || ((c)->ldt[i].limit_hi & 0x10))
#define LDT_USED(c, i)  ((c)->ldt[i].access && !((c)->ldt[i].limit_hi & 0x10))

/* Descriptor helpers */
static inline void desc_set_base(struct seg_descriptor *d, uint32_t base) {
    d->base_lo  = base & 0xFFFF;
    d->base_mid = (base >> 16) & 0xFF;
    d->base_hi  = (base >> 24) & 0xFF;
}

static inline uint32_t desc_get_base(struct seg_descriptor *d) {
    return d->base_lo | ((uint32_t)d->base_mid << 16) | ((uint32_t)d->base_hi << 24);
}

static inline void desc_set_limit(struct seg_descriptor *d, uint32_t limit) {
    if (limit > 0xFFFFF) {
        /* Page granularity */
        limit >>= 12;
        d->limit_hi = (d->limit_hi & 0x70) | 0x80 | (limit >> 16);
    } else {
        d->limit_hi = (d->limit_hi & 0x70) | (limit >> 16);
    }
    d->limit_lo = limit & 0xFFFF;
}

/* Init DPMI subsystem */
void dpmi_init(void);

/* Handle INT 31h from a PM client.
 * Called from the IDT interrupt gate handler.
 * Returns 0 on success (carry clear), 1 on error (carry set). */
int dpmi_int31(int client_id, struct dpmi_regs *regs);

/* Handle INT 2Fh/1687h — DPMI detection from V86 */
int dpmi_detect(struct dpmi_regs *regs);

/* Mode switch: transition a V86 task to Ring 3 PM.
 * Called when DOS extender calls the DPMI entry point. */
int dpmi_enter_pm(int v86_task_id, int is_32bit);

/* Perform V86 → Ring 3 PM transition (modifies the V86 frame for PM IRET) */
struct v86_frame;
int dpmi_transition_to_pm(int client_id, struct v86_frame *frame);

/* Sentinel for synchronous RM-call unwind (docs/research/29-dpmi-host.md).
 * Bytes `CD F4 CF` are written at SENTINEL_LIN at PM-entry. */
#define DPMI_SENTINEL_SEG   0x0000
#define DPMI_SENTINEL_OFF   0x050C
#define DPMI_SENTINEL_LIN   (((uint32_t)DPMI_SENTINEL_SEG << 4) + DPMI_SENTINEL_OFF)
#define DPMI_SENTINEL_VEC   0xF4

struct isr_frame;

/* Installs the sentinel stub at DPMI_SENTINEL_LIN; called from dpmi_init. */
void dpmi_install_sentinel(void);

/* Performs the PM→V86 trampoline switch for 0x0301/0x0302. Called from
 * the PM INT 31h dispatcher after dpmi_int31 has set rm_call_save and
 * returned the special "switch to V86" code (2). */
int dpmi_rm_call_setup_isr(int client_id, struct isr_frame *frame);

/* Performs the V86→PM unwind when V86 hits INT 0xF4 at the sentinel.
 * Called from v86_emulate_int. */
void dpmi_rm_call_unwind(struct v86_frame *frame);

/* Get DPMI client state */
struct dpmi_client *dpmi_get_client(int id);

/* Find an active (or pm_exited) client owned by the given V86 task,
 * or NULL. Used by the V86 INT 31h handler to service post-PM-exit
 * queries (0x000B Get Descriptor, 0x0204 Get PM Vector) with real
 * data from the still-allocated LDT + pm_vectors. */
struct dpmi_client *dpmi_find_client_for_v86(int v86_task_id);

/* Allocate `count` consecutive LDT descriptors in the client owned by
 * the given v86 task. Returns the base LDT index (multiply by 8 +
 * (4|3) for the selector) or 0 on failure. Used by V86 INT 31h
 * AX=0x0000. */
int dpmi_alloc_ldt_v86(int v86_task_id, int count);

/* Set up an LDT descriptor for a client (used by V86 handler during mode switch) */
void dpmi_ldt_setup(int client_id, int idx, uint32_t base,
                    uint32_t limit, uint8_t access, int is_32bit);

/* Handle CPU exception from a Ring 3 PM client.
 * Redirects to the client's registered exception handler if one exists.
 * Returns non-zero if handled, 0 if no handler (caller should halt). */
uint32_t dpmi_handle_pm_exception(uint8_t exc_num, uint32_t esp);

/* Commit-on-touch entry for #PFs that come from kernel mode (e.g. the host
 * itself dereferencing a client buffer that was 0501-reserved but never
 * touched by the client). CR2 must lie in the DPMI client linear zone.
 * Returns 1 if the page was committed and the faulting instruction can be
 * retried, 0 otherwise (caller should fall through to normal exception
 * handling — typically a kernel halt for unrelated faults). */
int dpmi_kernel_pf_commit(uint32_t cr2);

/* Handle software INT from a Ring 3 PM client.
 * Called by isr_dispatch when it detects a PM client issued a software INT.
 * Returns new ESP if frame was rebuilt (e.g., V86 return), or 0 for no change. */
uint32_t dpmi_handle_pm_int(uint8_t vector, uint32_t esp);

/* Returns 1 if the PM client owning `cs_sel` has a real handler installed for
 * the given DPMI INT `vector` (via INT 31h/0205), 0 otherwise. Used by the IDT
 * dispatcher to gate hardware-IRQ delivery — only attempt PM delivery when a
 * handler exists, so we don't touch frame->eax/eflags in the unhandled path. */
int dpmi_pm_has_handler(uint16_t cs_sel, uint8_t vector);

/* Deliver a hardware IRQ to the active PM client's registered handler.
 * `vector` is the BIOS-style INT number (0x08..0x0F for IRQ 0..7,
 * 0x70..0x77 for IRQ 8..15). Caller must have verified via
 * dpmi_pm_has_handler that a real handler exists and must NOT EOI before
 * calling — the client handler does its own EOI (matches CWSDPMI). Modifies
 * the isr_frame at `esp` in place to redirect IRETD to the PM handler.
 * Returns 0 on successful delivery, -1 if no client matched (caller should
 * fall through to kernel-side handling). */
int dpmi_deliver_pm_irq(uint8_t vector, uint32_t esp);

/* Dispatch a real-mode callback: save RM regs, switch to PM handler.
 * Called from V86 monitor when it traps INT 0xF2 at a callback stub.
 * rmcb_id = callback slot number (0..DPMI_MAX_RMCB-1).
 * Returns new ESP if frame was rebuilt for PM, 0 otherwise. */
uint32_t dpmi_rmcb_dispatch(int rmcb_id, uint32_t esp);

/* Max clients */
#define DPMI_MAX_CLIENTS  4

/* Set while DPMI host is processing a client request (exception, INT, INT 31h).
 * Prevents scheduler preemption during multi-step operations. */
extern volatile int dpmi_busy;
extern volatile int dpmi_timer_ready;

/* V86MT VT helpers (Phase 4.7 M4) — dos.c calls these on every INT 21h
 * AH=02/09 when the current V86 task is V86MT-owned. */
struct dpmi_v86mt_vt *v86mt_vt_get(int client_id, uint16_t handle);
void v86mt_vt_putc(struct dpmi_v86mt_vt *v, uint8_t ch, uint8_t attr);

/* V86MT kbd-ring helpers (Phase 4.7 M6) — v86.c INT 16h emulator calls these
 * when the current V86 task is V86MT-owned. peek = non-destructive read
 * (returns 1 if a key is available and writes it to *out); pop = same as
 * peek but advances the tail. Returns 0 if ring is empty. */
int v86mt_kbd_peek(struct dpmi_v86mt_vt *v, uint16_t *out);
int v86mt_kbd_pop (struct dpmi_v86mt_vt *v, uint16_t *out);

#endif
