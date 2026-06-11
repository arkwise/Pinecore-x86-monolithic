/* dpmi.c — DPMI 0.9 Host
 *
 * Provides DOS Protected Mode Interface services so DOS extenders
 * (DOS/4GW, PMODE/W) can run 32-bit applications at Ring 3.
 *
 * The DPMI host manages per-client LDTs, extended memory, interrupt
 * vectors, and real-mode simulation. Clients enter PM via INT 2Fh/1687h
 * detection followed by a mode switch call.
 *
 * (ch-29, ch-30, ch-31)
 */

#include "types.h"
#include "dpmi.h"
#include "serial.h"
#include "pmm.h"
#include "vmm.h"
#include "dos.h"
#include "v86.h"
#include "idt.h"
#include "sched.h"
#include "vbe.h"
#include "mouse.h"
#include "vt.h"
#include "heap.h"
#include "sched.h"

extern void *memset(void *s, int c, uint32_t n);
extern void *memcpy(void *dst, const void *src, uint32_t n);

static struct dpmi_client clients[DPMI_MAX_CLIENTS];

/* Set while DPMI host is actively processing a client request.
 * Prevents scheduler preemption during #GP fixups, INT 31h, etc. */
volatile int dpmi_busy = 0;

/* Set once DPMI client has completed basic setup (RM callback allocated).
 * PIT timer delivery to the PM client is deferred until this is set,
 * because early timer delivery during init causes re-entrancy crashes. */
volatile int dpmi_timer_ready = 0;

/* Ses31 diagnostic: circular log of last N PM INT 21h deliveries.
 * Records the (eip, cs, eflags) tuple pushed onto the client's user
 * stack at delivery time, so when a subsequent #GP fires on a null-CS
 * IRETD we can identify whether the garbage IRETD frame was authored
 * by one of our deliveries (Hypothesis A) or by something between our
 * delivery and the IRETD (Hypothesis B). Power-of-two size for cheap
 * masking. */
#define PM_INT21_LOG_N 8
struct pm_int21_push_rec {
    uint8_t  ah;        /* function code (AH) at delivery time */
    uint8_t  width;     /* 32 or 16: push width in bits */
    uint16_t h_sel;     /* destination handler selector */
    uint32_t eip;       /* pushed return EIP (frame->eip at delivery) */
    uint32_t cs;        /* pushed return CS  (frame->cs  at delivery) */
    uint32_t eflags;    /* pushed EFLAGS    (frame->eflags at delivery) */
    uint32_t ss_esp;    /* (ss<<16)|(new_esp&0xFFFF) — locates the push */
};
static struct pm_int21_push_rec pm_int21_log[PM_INT21_LOG_N];
static uint32_t pm_int21_log_count = 0;  /* total deliveries; slot = count & (N-1) */

/* Ses32 H9 diagnostic: same shape as the INT 21h log above, but for
 * the two other PM-stack 3-dword same-DPL push paths in dpmi.c —
 * exception delivery (~dpmi.c:2056-2080, fEIP/fCS/fEFL triple inside
 * the 8-dword exception frame) and generic PM INT/IRQ delivery
 * (~dpmi.c:2784-2794, 3-dword IRET frame). Session 32 falsified
 * H6-narrow; the (0,0,0x13046) garbage at 0x11F:0x6528 still matches
 * a same-DPL push authored by one of these paths with a zeroed
 * source frame. If either log shows (pushEIP=0, pushCS=0) H9 is
 * confirmed. ss_esp stores the BASE of the push as
 * (frame->ss << 16) | (new_esp & 0xFFFF); for exceptions the
 * (fEIP, fCS, fEFL) triple lives at +0x0C (32-bit) / +0x06 (16-bit)
 * inside that push, for IRQ it lives at +0x00. */
#define PM_EXC_LOG_N 8
struct pm_exc_push_rec {
    uint8_t  exc_num;   /* exception vector (0..31) */
    uint8_t  width;     /* 32 or 16: push width in bits */
    uint16_t h_sel;     /* destination handler selector (pm_vectors[exc].selector) */
    uint32_t eip;       /* frame->eip at delivery (faulting EIP, the fEIP push) */
    uint32_t cs;        /* frame->cs at delivery (faulting CS) */
    uint32_t eflags;    /* frame->eflags at delivery (faulting EFLAGS) */
    uint32_t ss_esp;    /* (ss<<16) | (new_esp & 0xFFFF) — locates the push base */
};
static struct pm_exc_push_rec pm_exc_log[PM_EXC_LOG_N];
static uint32_t pm_exc_log_count = 0;

#define PM_IRQ_LOG_N 8
struct pm_irq_push_rec {
    uint8_t  vector;    /* PM INT vector (non-INT-21h: IRQs reflected to PM + INT 2F etc.) */
    uint8_t  width;     /* 32 or 16 */
    uint16_t h_sel;     /* destination handler selector (pm_vectors[vec].selector) */
    uint32_t eip;       /* frame->eip at delivery (interrupted EIP, the pushEIP) */
    uint32_t cs;        /* frame->cs at delivery (interrupted CS) */
    uint32_t eflags;    /* frame->eflags at delivery (interrupted EFLAGS) */
    uint32_t ss_esp;    /* (ss<<16) | (new_esp & 0xFFFF) */
};
static struct pm_irq_push_rec pm_irq_log[PM_IRQ_LOG_N];
static uint32_t pm_irq_log_count = 0;

/* GDT entry for the active DPMI client's LDT (selector 0x30, index 6) */
#define GDT_LDT_INDEX  6
#define GDT_LDT_SEL    (GDT_LDT_INDEX * 8)

extern uint8_t gdt_start[];  /* from boot.asm */

/* Write a DPMI client's LDT into the GDT and load LDTR */
static void load_client_ldt(struct dpmi_client *c) {
    uint32_t base = (uint32_t)c->ldt;
    uint32_t limit = sizeof(c->ldt) - 1;
    uint8_t *entry = gdt_start + GDT_LDT_INDEX * 8;

    /* Write the LDT descriptor into the GDT slot */
    entry[0] = limit & 0xFF;
    entry[1] = (limit >> 8) & 0xFF;
    entry[2] = base & 0xFF;
    entry[3] = (base >> 8) & 0xFF;
    entry[4] = (base >> 16) & 0xFF;
    entry[5] = 0x82;  /* P=1, DPL=0, type=0010 (LDT) */
    entry[6] = (limit >> 16) & 0x0F;
    entry[7] = (base >> 24) & 0xFF;

    /* Expand GDT limit to include the LDT entry (index 6).
     * gdt_ptr is right after the GDT data in boot.asm. */
    {
        extern uint8_t gdt_ptr[];
        uint16_t new_limit = (GDT_LDT_INDEX + 1) * 8 - 1;  /* 55 */
        *(uint16_t *)gdt_ptr = new_limit;
        __asm__ volatile("lgdt (%0)" : : "r"(gdt_ptr));
    }

    __asm__ volatile("lldt %0" : : "r"((uint16_t)GDT_LDT_SEL));

    serial_puts("DPMI: LDT loaded (GDT limit=");
    serial_puthex((GDT_LDT_INDEX + 1) * 8 - 1);
    serial_puts(", LDT base=");
    serial_puthex(base);
    serial_puts(")\n");
}

/* Linear address space for DPMI client memory blocks.
 *
 * Sits ABOVE the 32 MB kernel identity-mapped zone so client allocations
 * never alias kernel pages, and so first-touch into the zone always #PFs
 * (giving the demand-pager a chance to commit). The end address is a soft
 * cap on linear reservation, NOT a physical-memory ceiling — DJGPP's go32
 * stub computes a brk-extend size in `brk_common` (DESKTOP.EXE @ 1dab-1dd7)
 * that lands around 2.47 GB on a freshly-loaded COFF, then sbrk treats any
 * `0501` rejection as fatal and falls through to `no_memory → exit`. So the
 * cap must be > 2.5 GB for go32 stubs to work. We use 3 GB; that leaves the
 * 0xBF data selector's 3.5 GB byte-limit window (base 0x02000000, page-G,
 * limit 0xCFFFF) able to address every reservation.
 * (cwsdpmi: exphdlr.c:80 — VADDR_START 0x400000; exphdlr.c:337 page_in_user
 *  demand-pages on #PF. We mirror the discipline, not the addresses.) */
#define DPMI_VADDR_START  0x02000000   /* 32 MB */
#define DPMI_VADDR_END    0xF0000000   /*  3.75 GB — DJGPP CRT sometimes
                                         * asks for >3 GB linear (s38 bogus
                                         * size); commit-on-PF means this is
                                         * safe — only touched pages back. */

/* Physical-commit budget across all clients. Total RAM is 32 MB; the kernel
 * + page tables + V86 image + PMM bookkeeping consume the first ~8 MB, so
 * cap client commits at 24 MB. When exhausted, the #PF demand-pager returns
 * 0 (no resume) and the client's exception handler (or kill path) fires. */
#define DPMI_COMMIT_CAP_PAGES   (24 * 1024 * 1024 / 0x1000)
static uint32_t dpmi_committed_pages = 0;

/* Common commit-on-touch: zero-fill, P|W|U-map, bump counter. Returns 1 on
 * success, 0 on OOM or commit-cap exhaustion. Caller already verified CR2
 * is in the DPMI client zone and the fault is P=0 (not-present). */
static int dpmi_commit_page(uint32_t cr2) {
    uint32_t page = cr2 & ~0xFFFu;
    uint32_t phys;
    if (dpmi_committed_pages >= DPMI_COMMIT_CAP_PAGES) return 0;
    phys = pmm_alloc_page();
    if (!phys) return 0;
    vmm_map_page(page, phys, 0x07);  /* P | W | U */
    dpmi_committed_pages++;
    {
        uint32_t *zp = (uint32_t *)page;
        int z;
        for (z = 0; z < 1024; z++) zp[z] = 0;
    }
    return 1;
}

int dpmi_kernel_pf_commit(uint32_t cr2) {
    if (cr2 < DPMI_VADDR_START || cr2 >= DPMI_VADDR_END) return 0;
    if (!dpmi_commit_page(cr2)) {
        serial_puts("DPMI: kernel-PF commit failed at CR2=");
        serial_puthex(cr2);
        serial_puts("\n");
        return 0;
    }
    static int k_map_count = 0;
    if (k_map_count < 20) {
        serial_puts("DPMI: kernel auto-map CR2=");
        serial_puthex(cr2);
        serial_puts("\n");
        k_map_count++;
    }
    return 1;
}

void dpmi_init(void) {
    int i;
    for (i = 0; i < DPMI_MAX_CLIENTS; i++) {
        clients[i].active = 0;
        clients[i].vt_paused = 0;
    }

    /* Set IDT gates to DPL=3 for software interrupt vectors that DPMI
     * clients need to call from Ring 3 PM.
     *
     * CRITICAL: vectors 0-31 MUST stay DPL=0! In V86 mode, INT instructions
     * with DPL=3 gates bypass the GPF handler entirely — the CPU calls the
     * IDT gate directly. We need V86 INT instructions to GPF so our V86
     * monitor can emulate them. Only set DPL=3 on IRQ vectors (32+). */
    {
        extern void idt_set_gate_dpl3(uint8_t num);
        for (i = 32; i < 48; i++)
            idt_set_gate_dpl3(i);
    }

    /* Install the synchronous-RM-call sentinel at linear 0x50C.
     * Bytes `CD F4 CF` — when a V86 RM proc IRETs/RETFs back to this
     * address (the trampoline frame we pushed), INT 0xF4 fires, the
     * V86 monitor calls dpmi_rm_call_unwind() and we restore PM. */
    dpmi_install_sentinel();

    serial_puts("DPMI: host ready (0.9, 32-bit, IDT DPL=3)\n");
}

/* Release a DPMI client's owned resources: walks every active memblock
 * and returns its committed physical pages to the PMM, releases RM
 * callback slots, then marks the client inactive. Used by every exit
 * path — init-failure (line ~548), PM exception terminator (~3378),
 * PM INT 21h AH=4Ch (~3628 — the DOOM exit path), and
 * dpmi_release_client_for_v86 (called from sched_v86_exit).
 *
 * Pre-s40 only the last path was wired, so DPMI clients that exited
 * via PM AH=4Ch (everything DJGPP/DOS-4GW) leaked their extended
 * memory until QEMU was restarted. */
/* Free memblocks + RMCBs (the "resource" half of a client). Doesn't
 * touch LDT or pm_vectors. Used by both release paths. */
static void ldt_free(struct dpmi_client *c, int idx);     /* fwd decl */

static void dpmi_free_client_resources(struct dpmi_client *c) {
    int j;
    int blocks_freed = 0;
    uint32_t pages_freed = 0;

    for (j = 0; j < DPMI_MAX_MEMBLOCKS; j++) {
        if (c->memblocks[j].active) {
            uint32_t off;
            for (off = 0; off < c->memblocks[j].size; off += 0x1000) {
                uint32_t va = c->memblocks[j].base + off;
                uint32_t phys = vmm_get_physical(va);
                if (phys) {
                    vmm_unmap_page(va);
                    pmm_free_page(phys & ~0xFFFu);
                    if (dpmi_committed_pages > 0) dpmi_committed_pages--;
                    pages_freed++;
                }
            }
            c->memblocks[j].active = 0;
            blocks_freed++;
        }
    }
    for (j = 0; j < DPMI_MAX_RMCB; j++)
        c->rmcb[j].active = 0;

    /* V86MT shadow buffers (M3) + LDT selectors (M5) + kbd ring (M6). */
    for (j = 0; j < DPMI_V86MT_MAX_VTS; j++) {
        if (c->v86mt_vts[j].char_sel) {
            ldt_free(c, SEL_TO_IDX(c->v86mt_vts[j].char_sel));
            c->v86mt_vts[j].char_sel = 0;
        }
        if (c->v86mt_vts[j].attr_sel) {
            ldt_free(c, SEL_TO_IDX(c->v86mt_vts[j].attr_sel));
            c->v86mt_vts[j].attr_sel = 0;
        }
        if (c->v86mt_vts[j].kbd_sel) {
            ldt_free(c, SEL_TO_IDX(c->v86mt_vts[j].kbd_sel));
            c->v86mt_vts[j].kbd_sel = 0;
        }
        if (c->v86mt_vts[j].char_buf) {
            kfree(c->v86mt_vts[j].char_buf);
            c->v86mt_vts[j].char_buf = 0;
        }
        if (c->v86mt_vts[j].attr_buf) {
            kfree(c->v86mt_vts[j].attr_buf);
            c->v86mt_vts[j].attr_buf = 0;
        }
        if (c->v86mt_vts[j].kbd_buf) {
            kfree(c->v86mt_vts[j].kbd_buf);
            c->v86mt_vts[j].kbd_buf = 0;
        }
        c->v86mt_vts[j].used = 0;
    }

    if (blocks_freed) {
        serial_puts("DPMI: freed ");
        serial_puthex(blocks_freed);
        serial_puts(" memblocks, ");
        serial_puthex(pages_freed);
        serial_puts(" pages (committed now=");
        serial_puthex(dpmi_committed_pages);
        serial_puts(")\n");
    }
}

/* PM exit (INT 21h AH=4Ch) — partial release. Frees resources but
 * KEEPS the client struct queryable: LDT entries + pm_vectors stay
 * intact so DOS/4GW's V86 unwind (0x000B Get Descriptor, 0x0204 Get
 * PM Vector) can read back what was installed. The full release
 * happens later when the V86 task itself terminates. */
static void dpmi_release_client_pm_exit(struct dpmi_client *c) {
    if (!c->active) return;
    serial_puts("DPMI: PM exit — partial release (LDT preserved for V86 unwind)\n");
    dpmi_free_client_resources(c);
    c->rm_call_save.active = 0;
    c->vt_paused = 0;
    c->exc_save.active = 0;
    c->pm_exited = 1;
    /* c->active stays 1 — V86 INT 31h queries find this client. */
}

/* Full release — called from sched_v86_exit when the hosting V86
 * task terminates. After this, the slot is reusable. */
static void dpmi_release_client(struct dpmi_client *c) {
    if (!c->active) return;
    dpmi_free_client_resources(c);
    c->active = 0;
    c->rm_call_save.active = 0;
    c->vt_paused = 0;
    c->exc_save.active = 0;
    c->pm_exited = 0;
}

void dpmi_release_client_for_v86(int v86_task_id) {
    int i;
    for (i = 0; i < DPMI_MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].v86_task_id == v86_task_id) {
            serial_puts("DPMI: releasing client ");
            serial_puthex(i);
            serial_puts(" for exited v86 task ");
            serial_puthex(v86_task_id);
            serial_puts("\n");
            dpmi_release_client(&clients[i]);
        }
    }
}

struct dpmi_client *dpmi_find_client_for_v86(int v86_task_id) {
    int i;
    if (v86_task_id < 0) return 0;
    for (i = 0; i < DPMI_MAX_CLIENTS; i++) {
        if (clients[i].active &&
            clients[i].v86_task_id == v86_task_id)
            return &clients[i];
    }
    return 0;
}

static int ldt_alloc(struct dpmi_client *c, int count);   /* fwd decl */

int dpmi_alloc_ldt_v86(int v86_task_id, int count) {
    struct dpmi_client *c = dpmi_find_client_for_v86(v86_task_id);
    /* s44 debug — one-shot per-failure type */
    {
        static int no_client_n = 0;
        static int neg_count_n = 0;
        if (!c && no_client_n++ < 2) {
            int i;
            serial_puts("V86_alloc: NO CLIENT for v86=");
            serial_puthex(v86_task_id);
            serial_puts(" — clients:");
            for (i = 0; i < DPMI_MAX_CLIENTS; i++) {
                serial_puts(" [");
                serial_puthex(i);
                serial_puts("]act=");
                serial_puthex(clients[i].active);
                serial_puts(" v86=");
                serial_puthex(clients[i].v86_task_id);
                serial_puts(" pme=");
                serial_puthex(clients[i].pm_exited);
            }
            serial_puts("\n");
        }
        if (count < 0 && neg_count_n++ < 2) {
            serial_puts("V86_alloc: neg count=");
            serial_puthex(count);
            serial_puts("\n");
        }
    }
    if (!c) return 0;
    if (count < 0) return 0;
    /* count == 0: DPMI spec is ambiguous but DOSEMU2 (dpmi.c:845
     * AllocateDescriptors → allocate_descriptors_from) returns the
     * first free slot's selector without marking anything used. Our
     * ldt_alloc already implements this correctly when count=0 (inner
     * j-loop is skipped, returns i=DPMI_LDT_FIRST). DOS/16M probes
     * with CX=0 during init — rejecting it returns sentinel 0x40
     * (TI=0, GDT) which DOS/16M reads as invalid → [32] host error. */
    return ldt_alloc(c, count);
}

struct dpmi_client *dpmi_get_client(int id) {
    if (id < 0 || id >= DPMI_MAX_CLIENTS) return 0;
    if (!clients[id].active) return 0;
    return &clients[id];
}

/* ================================================================
 * LDT Management
 * ================================================================ */

/* Allocate n consecutive free LDT descriptors. Returns first index or 0 on failure. */
static int ldt_alloc(struct dpmi_client *c, int count) {
    int i, j;
    for (i = DPMI_LDT_FIRST; (i + count) <= DPMI_LDT_ENTRIES; i++) {
        for (j = 0; j < count && LDT_FREE(c, i + j); j++)
            ;
        if (j >= count) {
            /* Found consecutive free entries — initialize as data r/w */
            for (j = 0; j < count; j++) {
                c->ldt[i + j].limit_lo = 0;
                c->ldt[i + j].base_lo = 0;
                c->ldt[i + j].base_mid = 0;
                c->ldt[i + j].base_hi = 0;
                c->ldt[i + j].access = 0xF2;   /* P=1, DPL=3, S=1, data r/w */
                c->ldt[i + j].limit_hi = 0x40;  /* D=1 (32-bit), G=0 */
            }
            return i;
        }
    }
    return 0;  /* no space */
}

/* Free an LDT descriptor.
 *
 * We do NOT clear the access byte (which would set P=0). DJGPP's exit()
 * calls 0x0001 to free its own DS *while still running with DS loaded*;
 * the kernel's ISR epilogue then `pop %ds` of that selector — if P=0,
 * #GP in kernel mode. Real DPMI hosts (CWSDPMI, HDPMI) tolerate this by
 * keeping the descriptor physically valid for as long as the client
 * holds a copy in a segment register.
 *
 * Instead, we mark the slot logically-free via the AVL bit in limit_hi
 * (bit 4). ldt_alloc treats AVL-flagged entries as reusable and overwrites
 * limit_hi when reallocating, clearing the bit. The descriptor's
 * P/DPL/S/Type stay intact so reloads succeed. */
static void ldt_free(struct dpmi_client *c, int idx) {
    if (idx < DPMI_LDT_FIRST || idx >= DPMI_LDT_ENTRIES) return;
    c->ldt[idx].limit_hi |= 0x10;  /* AVL=1 → free for reuse, still loadable */
}

/* Set up a descriptor with base, limit, and type */
static void ldt_setup(struct dpmi_client *c, int idx, uint32_t base,
                      uint32_t limit, uint8_t access, int is_32bit) {
    desc_set_base(&c->ldt[idx], base);
    desc_set_limit(&c->ldt[idx], limit);
    c->ldt[idx].access = access;
    if (is_32bit)
        c->ldt[idx].limit_hi |= 0x40;  /* D/B bit = 32-bit */
    else
        c->ldt[idx].limit_hi &= ~0x40; /* D/B bit = 16-bit */
}

/* Public version for V86 handler to set up initial client descriptors */
void dpmi_ldt_setup(int client_id, int idx, uint32_t base,
                    uint32_t limit, uint8_t access, int is_32bit) {
    struct dpmi_client *c = dpmi_get_client(client_id);
    if (c && idx >= 0 && idx < DPMI_LDT_ENTRIES)
        ldt_setup(c, idx, base, limit, access, is_32bit);
}

/* ================================================================
 * Memory Management
 * ================================================================ */

/* Reserve a linear range for the client.
 *
 * DPMI 1.0 reserve-vs-commit: this only carves out linear address space.
 * Physical pages are allocated lazily by `dpmi_handle_pm_exception` on
 * first touch. Zeroing happens at commit time, not here. Matches CWSDPMI
 * (exphdlr.c memory_allocate + paging on-fault) — which is the host
 * behaviour DJGPP/Allegro/DOOM were all written against.
 *
 * Refuses requests that would exhaust the linear reservation window. The
 * physical-commit cap is enforced separately at #PF time. */
static int memblock_alloc(struct dpmi_client *c, uint32_t size,
                          uint32_t *out_base) {
    int i;
    uint32_t base;

    /* Find free slot */
    for (i = 0; i < DPMI_MAX_MEMBLOCKS; i++) {
        if (!c->memblocks[i].active) break;
    }
    if (i == DPMI_MAX_MEMBLOCKS) return -1;

    /* Round up to page boundary */
    size = (size + 0xFFF) & ~0xFFF;
    if (size == 0) size = 0x1000;

    base = c->next_linear;

    /* Linear address space exhaustion → spec error 8012h territory. */
    if (base < DPMI_VADDR_START ||
        base >= DPMI_VADDR_END) {
        return -1;
    }

    /* s57: silently cap requests that exceed the remaining linear window.
     *
     * Surfaced in s56: DESKTOP build s55→s56 grew 37 bytes → s38-family
     * layout shift moves the V2 stub's grow_memory accumulator past
     * 0xF000_0000 → INT 31h 0x0501 fires with BX:CX = 0xFF42:0000 (4.2 GB).
     * The s47 + s49 fixes addressed the same family in earlier builds by
     * pinning stubinfo deterministically and bumping DPMI_VADDR_END to
     * 0xF000_0000, both still in effect — but a 4.2 GB single-call ask
     * doesn't fit in any 32-bit linear window at any base in [VADDR_START,
     * 4 GB). Capping silently is safe because the reserve-vs-commit memory
     * model means the client only physically uses what it touches; the
     * 24 MB commit cap is the real budget. The V2 stub uses the returned
     * base to stamp _stubinfo and rep-movsds the stamped struct into the
     * head of the allocation; it never reads back the size. */
    if (size > (DPMI_VADDR_END - base)) {
        uint32_t capped = DPMI_VADDR_END - base;
        serial_puts("DPMI: 0501 cap ");
        serial_puthex(size);
        serial_puts(" → ");
        serial_puthex(capped);
        serial_puts("\n");
        size = capped;
    }

    c->memblocks[i].active = 1;
    c->memblocks[i].base = base;
    c->memblocks[i].size = size;
    c->next_linear = base + size;
    *out_base = base;
    return 0;
}

static int memblock_free(struct dpmi_client *c, uint32_t base) {
    int i;
    for (i = 0; i < DPMI_MAX_MEMBLOCKS; i++) {
        if (c->memblocks[i].active && c->memblocks[i].base == base) {
            /* Walk every page in the reservation. Pages that were committed
             * (P=1 in PTE) get unmapped + their physical frame returned to
             * PMM + commit counter decremented; pages that were never
             * touched have nothing to do. */
            uint32_t off;
            for (off = 0; off < c->memblocks[i].size; off += 0x1000) {
                uint32_t va = c->memblocks[i].base + off;
                uint32_t phys = vmm_get_physical(va);
                if (phys) {
                    vmm_unmap_page(va);
                    pmm_free_page(phys & ~0xFFFu);
                    if (dpmi_committed_pages > 0) dpmi_committed_pages--;
                }
            }
            c->memblocks[i].active = 0;
            return 0;
        }
    }
    return -1;
}

/* ================================================================
 * DPMI Enter PM — mode switch from V86 to Ring 3 PM
 * ================================================================ */

int dpmi_enter_pm(int v86_task_id, int is_32bit) {
    int i, ci;
    struct dpmi_client *c;
    int code_idx, data_idx, psp_idx, ss_idx;
    int nested = 0;

    /* If this V86 task already has an active DPMI client, REUSE it.
     * DOS/4GW (and DOS/32A) spawn a 16-bit child PM session for some
     * RM-callback paths after the 32-bit parent is set up. The child
     * needs the parent's LDT entries (the handler selectors that 4GW
     * allocated for INT vectors, DS aliasing, etc.) — allocating a
     * fresh client with a zeroed LDT means the child's IRET to one of
     * the parent's selectors finds "not present" and kernel-#GPs.
     *
     * Reuse strategy: keep all LDT slots and pm_vectors; only re-seed
     * CS/DS/SS/PSP (slots 16/17/18/19) below with the new caller's
     * segment bases and the new bit-width. */
    for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
        if (clients[ci].active && !clients[ci].pm_exited &&
            clients[ci].v86_task_id == v86_task_id) {
            serial_puts("DPMI: nested PM entry for v86 task ");
            serial_puthex(v86_task_id);
            serial_puts(" (reusing client ");
            serial_puthex(ci);
            serial_puts(", was is_32=");
            serial_puthex(clients[ci].is_32bit);
            serial_puts(", now is_32=");
            serial_puthex(is_32bit);
            serial_puts(")\n");
            c = &clients[ci];
            c->is_32bit = is_32bit;
            c->exc_save.active = 0;
            c->rm_call_save.active = 0;
            /* CS/DS/SS/PSP slots get re-seeded by dpmi_transition_to_pm,
             * so the caller's actual RM segment bases land in 16..19.
             * Everything else (handlers at 0x87/0x8F/0xCF, auto-LDT'd
             * selectors, memblocks, RMCBs, exception/PM vectors) persists. */
            return ci;
        }
    }

    /* Before claiming a fresh slot, check if THIS v86 task already
     * owns a pm_exited client (the previous program exited but the
     * shell hasn't terminated). Full-release that slot now so it
     * becomes reusable and gets a clean LDT below. */
    for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
        if (clients[ci].active && clients[ci].pm_exited &&
            clients[ci].v86_task_id == v86_task_id) {
            serial_puts("DPMI: reclaiming pm_exited client for same v86 task ");
            serial_puthex(v86_task_id);
            serial_puts("\n");
            dpmi_release_client(&clients[ci]);
            break;   /* ci now points to the freshly-released slot */
        }
    }
    /* If no pm_exited slot was reclaimed, scan for any !active slot. */
    if (ci == DPMI_MAX_CLIENTS || clients[ci].active) {
        for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
            if (!clients[ci].active) break;
        }
    }
    if (ci == DPMI_MAX_CLIENTS) return -1;

    c = &clients[ci];

    /* Initialize client. Defensive memblock teardown first: if a
     * previous client in this slot crashed without dpmi_release being
     * called (or release ran but missed something), it could leave
     * active memblocks whose physical pages we'd otherwise leak. */
    for (i = 0; i < DPMI_LDT_ENTRIES * 8; i++)
        ((uint8_t *)c->ldt)[i] = 0;
    for (i = 0; i < DPMI_MAX_MEMBLOCKS; i++) {
        if (c->memblocks[i].active) {
            uint32_t off;
            serial_puts("DPMI: new-client init found leaked memblock, freeing\n");
            for (off = 0; off < c->memblocks[i].size; off += 0x1000) {
                uint32_t va = c->memblocks[i].base + off;
                uint32_t phys = vmm_get_physical(va);
                if (phys) {
                    vmm_unmap_page(va);
                    pmm_free_page(phys & ~0xFFFu);
                    if (dpmi_committed_pages > 0) dpmi_committed_pages--;
                }
            }
        }
        c->memblocks[i].active = 0;
    }
    for (i = 0; i < 256; i++) {
        c->pm_vectors[i].selector = 0;
        c->pm_vectors[i].offset = 0;
    }
    for (i = 0; i < 32; i++) {
        c->pm_exc_vectors[i].selector = 0;
        c->pm_exc_vectors[i].offset = 0;
    }

    c->active = 1;
    c->pm_exited = 0;
    c->is_32bit = is_32bit;
    c->v86_task_id = v86_task_id;
    c->next_linear = DPMI_VADDR_START;
    c->virtual_if = 1;
    c->next_rmcb = 0;
    c->exc_save.active = 0;
    c->rm_call_save.active = 0;
    c->pm_int_chain_sel = 0;
    c->exc_return_sel = 0;
    c->vendor_api_sel = 0;
    c->v86mt_api_sel = 0;
    for (i = 0; i < DPMI_V86MT_MAX_VTS; i++) {
        c->v86mt_vts[i].used           = 0;
        c->v86mt_vts[i].cols           = 0;
        c->v86mt_vts[i].rows           = 0;
        c->v86mt_vts[i].cursor_visible = 0;
        c->v86mt_vts[i].cursor_x       = 0;
        c->v86mt_vts[i].cursor_y       = 0;
        c->v86mt_vts[i].screen_dirty   = 0;
        c->v86mt_vts[i].kbd_drops      = 0;
        c->v86mt_vts[i].ticks_consumed = 0;
        c->v86mt_vts[i].char_buf       = 0;
        c->v86mt_vts[i].attr_buf       = 0;
        c->v86mt_vts[i].kbd_buf        = 0;
        c->v86mt_vts[i].char_sel       = 0;
        c->v86mt_vts[i].attr_sel       = 0;
        c->v86mt_vts[i].kbd_sel        = 0;
        c->v86mt_vts[i].task_running   = 0;
        c->v86mt_vts[i].exited         = 0;
        c->v86mt_vts[i].exit_code      = 0;
    }
    for (i = 0; i < DPMI_MAX_RMCB; i++)
        c->rmcb[i].active = 0;
    (void)nested;

    /* Preallocate LDT[5] = CWSDPMI's low-memory selector before any
     * client allocations. Base=0, limit=1 MB-1, 16-bit data, DPL=3.
     * Used by host handlers that return seg:off pointers into low memory
     * (INDOS flag, DOS LoL, real-mode IVT). Sitting at the CWSDPMI-
     * canonical slot stops DOS/4GW's `lar` probe from going off-script. */
    ldt_setup(c, 5, 0, 0x000FFFFF, 0xF2, 0);

    /* Allocate initial LDT descriptors for client segments. ldt_alloc
     * starts scanning at DPMI_LDT_FIRST (16), so the first four calls
     * deterministically land at 16, 17, 18, 19 — matching CWSDPMI's
     * l_acode/l_adata/l_apsp/l_aenv layout that DOS/4GW probes for.
     * SS gets its own slot (LDT[19]) instead of aliasing DS like before;
     * DOS/4GW notices the alias and falls into a non-CWSDPMI code path. */
    code_idx = ldt_alloc(c, 1);   /* 16 — CS  (l_acode) */
    data_idx = ldt_alloc(c, 1);   /* 17 — DS  (l_adata) */
    psp_idx  = ldt_alloc(c, 1);   /* 18 — PSP (l_apsp)  */
    ss_idx   = ldt_alloc(c, 1);   /* 19 — SS  (l_aenv repurposed) */

    if (!code_idx || !data_idx || !psp_idx || !ss_idx) {
        dpmi_release_client(c);
        return -1;
    }

    /* Create exception return thunk.
     * Write INT 0xF3 (CD F3) at linear address 0x504 (after DPMI entry
     * stub at 0x500). Create a DPL=3 code segment pointing there.
     * Exception handlers RETF to this thunk, which triggers INT 0xF3
     * (#GP from ring 3 on DPL=0 gate) — the kernel catches this and
     * restores the (modified) exception frame. */
    {
        uint8_t *thunk = (uint8_t *)0x504;
        thunk[0] = 0xCD;  /* INT */
        thunk[1] = 0xF3;  /* exception return vector */
        int thunk_idx = ldt_alloc(c, 1);
        if (thunk_idx) {
            /* DPL=3, present, code, non-conforming, readable */
            ldt_setup(c, thunk_idx, 0x504, 1, 0xFA, 0);
            c->exc_return_sel = LDT_SEL(thunk_idx);
        } else {
            c->exc_return_sel = c->client_cs;  /* fallback */
        }
    }

    /* DOS/4G "RATIONAL DOS/4G" vendor API entry point stub.
     * INT 31h AX=0x0A00 with DS:EDX="RATIONAL DOS/4G" returns
     * ES:EDI pointing here. Caller does CALL FAR ES:EDI to dispatch
     * by EAX:
     *   0x01 → EAX=0xABCD1234 (per rgmroman.narod.ru/Dos4g.htm)
     *   else → RETF (no-op)
     * 32-bit code segment at linear 0x520 (16 bytes after exc thunk
     * pair at 0x504/0x507; well below the trampoline at 0x550). */
    {
        uint8_t *v = (uint8_t *)0x520;
        int p = 0;
        v[p++] = 0x66; v[p++] = 0x3D; v[p++] = 0x01; v[p++] = 0x00; /* cmp ax,1 */
        v[p++] = 0x75; v[p++] = 0x05;                               /* jne +5 */
        v[p++] = 0xB8; v[p++] = 0x34; v[p++] = 0x12;
        v[p++] = 0xCD; v[p++] = 0xAB;                               /* mov eax,0xABCD1234 */
        v[p++] = 0xCB;                                              /* retf */
        int v_idx = ldt_alloc(c, 1);
        if (v_idx) {
            /* DPL=3 32-bit code, present, readable; D-bit set in is_32 arg. */
            ldt_setup(c, v_idx, 0x520, (uint32_t)(p - 1), 0xFA, 1);
            c->vendor_api_sel = LDT_SEL(v_idx);
        }
    }

    /* V86MT v1 vendor API entry-point stub (Phase 4.7 M1+M2).
     * 32-bit PM far-callable procedure at linear 0x530. Per
     * docs/design/V86MT-API.md, AX selects sub-function. Two paths:
     *   AX=0x0000 (get_caps) → fast inline response:
     *       EAX=0x0009 (text VTs + multi-VT, V86MT_CAP_TEXT_VT|MULTI_VT)
     *       EBX=0 (caps_hi), ECX=4 (max_vts), EDX=0 (api_minor=v1.0)
     *       CLC, RETF
     *   AX≠0  → forward to kernel via INT 31h with AX += 0x0A00, so
     *           sub-functions 0x0001..0x0008 dispatch to INT 31h cases
     *           0x0A01..0x0A08 in dpmi_int31. After INT 31h's exit path
     *           writes EAX/EBX/ECX/EDX/ES/EDI/EFLAGS, RETF back to caller
     *           with the kernel-set values intact.
     * Unknown AX values land on the generic INT 31h "unhandled" tail
     * which sets AX=0x8001 + CF=1 — matches V86MT spec's
     * DPMI_E_UNSUPPORTED contract automatically. */
    {
        uint8_t *m = (uint8_t *)0x530;
        int p = 0;
        m[p++] = 0x66; m[p++] = 0x85; m[p++] = 0xC0;          /* test ax, ax       */
        m[p++] = 0x75; m[p++] = 0x10;                         /* jne +16 (forward) */
        m[p++] = 0xB8; m[p++] = 0x09; m[p++] = 0x00;
        m[p++] = 0x00; m[p++] = 0x00;                         /* mov eax, 0x00000009 */
        m[p++] = 0x31; m[p++] = 0xDB;                         /* xor ebx, ebx      */
        m[p++] = 0xB9; m[p++] = 0x04; m[p++] = 0x00;
        m[p++] = 0x00; m[p++] = 0x00;                         /* mov ecx, 0x00000004 */
        m[p++] = 0x31; m[p++] = 0xD2;                         /* xor edx, edx      */
        m[p++] = 0xF8;                                        /* clc               */
        m[p++] = 0xCB;                                        /* retf              */
        /* forward: */
        m[p++] = 0x66; m[p++] = 0x05;
        m[p++] = 0x00; m[p++] = 0x0A;                         /* add ax, 0x0A00    */
        m[p++] = 0xCD; m[p++] = 0x31;                         /* int 0x31          */
        m[p++] = 0xCB;                                        /* retf              */
        int m_idx = ldt_alloc(c, 1);
        if (m_idx) {
            ldt_setup(c, m_idx, 0x530, (uint32_t)(p - 1), 0xFA, 1);
            c->v86mt_api_sel = LDT_SEL(m_idx);
        }
    }

    /* Pre-seed PM IRQ chain stub at linear 0x507.
     * One byte 0xCF (IRET) reached via a DPL=3 LDT code selector. Seeded
     * into pm_vectors[0x08..0x0F] and pm_vectors[0x70..0x77] so that when
     * a PM client queries "old vector" via 0x0204 before installing its
     * own IRQ handler, it gets a valid sel:0 pointing at IRET. Allegro's
     * djirqs wrapper chains to this old vector with `ljmp *cs:old_vector`
     * after the C handler returns non-zero (e.g. fixed_timer_handler's
     * bios=TRUE branch every ~18 ticks at 200Hz). Without this seed, the
     * chain ljmps to selector 0:0 → null-selector #GP.
     *
     * The IRET pops the wrapper's leftover 3-dword frame (which still
     * holds our pushed inner-IRET frame pointing back to the interrupted
     * PM code) and resumes — equivalent to the no-chain path. We lose
     * BIOS-INT-1Ch chaining and the 0x40:6C tick-count update, both
     * acceptable for v0; CWSDPMI uses an RM-reflection stub here instead. */
    {
        uint8_t *iret_stub = (uint8_t *)0x507;
        iret_stub[0] = 0xCF;  /* IRET */
        int iret_idx = ldt_alloc(c, 1);
        if (iret_idx) {
            /* DPL=3, present, code, non-conforming, readable; limit=0 (1 byte).
             * Match the client's bit-ness so iret pops the correct frame size
             * (32-bit clients pushed 12 bytes via dpmi_deliver_pm_irq; a 16-bit
             * iret here would pop only 6 bytes and load a garbled CS). */
            ldt_setup(c, iret_idx, 0x507, 0, 0xFA, is_32bit);
            uint16_t iret_sel = LDT_SEL(iret_idx);
            for (int v = 0x08; v <= 0x0F; v++) {
                c->pm_vectors[v].selector = iret_sel;
                c->pm_vectors[v].offset   = 0;
            }
            for (int v = 0x70; v <= 0x77; v++) {
                c->pm_vectors[v].selector = iret_sel;
                c->pm_vectors[v].offset   = 0;
            }
        }
    }

    /* Create PM INT 21h chain endpoint stub.
     * Bytes: CD 21 CB  (INT 21h ; RETF) at linear address 0x508.
     * Pointed to by an LDT code descriptor (DPL=3) so DOS extenders can
     * FAR CALL it from Ring 3 after their PM INT 21h handler translates
     * logical handles. Re-entry through INT 21h lands in dpmi_handle_pm_int
     * with CS == pm_int_chain_sel; we route directly to dos_int21 instead
     * of re-delivering to the client handler.
     *
     * Seeded into pm_vectors[0x21] so INT 31h/0204 returns sel:0 to clients
     * that query "previous handler" before installing their own. */
    {
        uint8_t *stub = (uint8_t *)0x508;
        stub[0] = 0xCD;  /* INT */
        stub[1] = 0x21;
        stub[2] = 0xCB;  /* RETF */
        int stub_idx = ldt_alloc(c, 1);
        if (stub_idx) {
            /* DPL=3, present, code, non-conforming, readable; limit=2 (3 bytes) */
            ldt_setup(c, stub_idx, 0x508, 2, 0xFA, 0);
            c->pm_int_chain_sel = LDT_SEL(stub_idx);
            c->pm_vectors[0x21].selector = c->pm_int_chain_sel;
            c->pm_vectors[0x21].offset   = 0;
        } else {
            c->pm_int_chain_sel = 0;  /* delivery will skip if zero */
        }
    }

    /* These will be filled with actual segment bases during the mode switch
     * by the V86 handler which knows the real-mode register state */
    c->client_cs = LDT_SEL(code_idx);
    c->client_ds = LDT_SEL(data_idx);
    c->client_ss = LDT_SEL(ss_idx);    /* separate from DS — CWSDPMI shape */
    c->client_es = LDT_SEL(psp_idx);

    serial_puts("DPMI: client ");
    serial_puthex(ci);
    serial_puts(" entering PM (");
    serial_puts(is_32bit ? "32-bit" : "16-bit");
    serial_puts(") CS=");
    serial_puthex(c->client_cs);
    serial_puts(" DS=");
    serial_puthex(c->client_ds);
    serial_puts("\n");

    return ci;
}

/* ================================================================
 * V86 → Ring 3 PM Transition
 *
 * Called from the V86 GPF handler when a DOS extender calls our
 * DPMI entry point. Modifies the V86 frame so that IRET lands
 * in Ring 3 PM instead of V86 mode.
 *
 * Key insight (386-bible p.220): IRET checks VM bit in EFLAGS
 * on the stack. With VM=1 it pops extra segment registers.
 * With VM=0 it does a normal PM IRET. The extra V86 segments
 * left on the Ring 0 stack are harmless — the stack pointer
 * resets from TSS.ESP0 on the next Ring 3 → Ring 0 transition.
 * ================================================================ */

#include "v86.h"

int dpmi_transition_to_pm(int client_id, struct v86_frame *frame) {
    struct dpmi_client *c = dpmi_get_client(client_id);
    if (!c) return -1;

    /* 1. Load the client's LDT into the GDT and set LDTR */
    load_client_ldt(c);

    /* 2. Set up LDT descriptors from the V86 task's current segments.
     *
     * DPMI spec: initial segments are ALWAYS 16-bit with 64KB limits,
     * regardless of the 32-bit flag. The real-mode stub code is 16-bit.
     * The extender will set up 32-bit flat segments itself via INT 31h. */
    {
        int cs_idx = SEL_TO_IDX(c->client_cs);
        int ds_idx = SEL_TO_IDX(c->client_ds);
        int ss_idx = SEL_TO_IDX(c->client_ss);
        int es_idx = SEL_TO_IDX(c->client_es);

        /* Code segment: base = V86 CS * 16, exec/read, DPL=3, 16-bit */
        ldt_setup(c, cs_idx,
                  (uint32_t)(frame->cs & 0xFFFF) << 4,
                  0xFFFF, 0xFA, 0);

        /* Data segment: base = V86 DS * 16, data r/w, DPL=3, 16-bit */
        ldt_setup(c, ds_idx,
                  (uint32_t)(frame->v86_ds & 0xFFFF) << 4,
                  0xFFFF, 0xF2, 0);

        /* Stack segment: base = V86 SS * 16. SS is now a separate LDT
         * slot from DS (CWSDPMI-shape l_aenv at LDT[19]); we must seed
         * its base here, otherwise IRET loads SS with base=0 and every
         * push/pop in PM faults. */
        ldt_setup(c, ss_idx,
                  (uint32_t)(frame->ss & 0xFFFF) << 4,
                  0xFFFF, 0xF2, 0);

        /* ES/PSP segment: per DPMI 0.9 spec, ES at PM entry holds a 16-bit
         * selector aliasing the program's PSP — NOT whatever the V86 client's
         * ES register happened to be. DJGPP's 16-bit stub may have set ES to
         * the env block or some other temporary before calling DPMI entry;
         * if we mirror that here, the stub's later `mov [stubinfo+0x26], es`
         * captures the wrong selector and 32-bit `_setup_environment` reads
         * a stale base, crashing in `___movedata`. Use the task's actual PSP
         * segment from DOS. */
        struct task *self = sched_get_task(sched_current());
        uint16_t psp_seg = (self && self->dos_task_id >= 0)
                            ? dos_get_psp(self->dos_task_id) : 0;
        if (!psp_seg) psp_seg = frame->v86_es & 0xFFFF;  /* fallback */
        ldt_setup(c, es_idx,
                  (uint32_t)psp_seg << 4,
                  0xFFFF, 0xF2, 0);
        serial_puts("DPMI: PSP descriptor base = ");
        serial_puthex((uint32_t)psp_seg << 4);
        serial_puts(" (psp_seg=");
        serial_puthex(psp_seg);
        serial_puts(", v86_es=");
        serial_puthex(frame->v86_es);
        serial_puts(")\n");

        /* Ses36 — env block descriptor.
         *
         * DJGPP's _setup_environment reads PSP[+0x2C] expecting a PM
         * selector aliasing the env block. Under DOS, PSP[+0x2C] holds
         * the env block's *real-mode segment number*. The PM extender
         * (CWSDPMI) patches PSP[+0x2C] in place with a freshly-allocated
         * LDT selector pointing at the env block (l_aenv in CWSDPMI's
         * descriptor table). We must do the same — otherwise the 32-bit
         * code at _movedata+9 (`mov ds, [ebp+8]`) loads the raw V86
         * segment value and #GPs immediately.
         *
         * Allocate the LDT slot only once per client; the V86 PSP write
         * lasts until the program exits, after which the V86 task is
         * torn down. The descriptor leak is one LDT slot per program. */
        if (psp_seg) {
            uint32_t psp_lin = (uint32_t)psp_seg << 4;
            uint16_t env_seg = *(volatile uint16_t *)(psp_lin + 0x2C);
            if (env_seg) {
                int env_idx = ldt_alloc(c, 1);
                if (env_idx) {
                    ldt_setup(c, env_idx,
                              (uint32_t)env_seg << 4,
                              0xFFFF, 0xF2, 0);
                    uint16_t env_sel = LDT_SEL(env_idx);
                    *(volatile uint16_t *)(psp_lin + 0x2C) = env_sel;
                    serial_puts("DPMI: env_seg ");
                    serial_puthex(env_seg);
                    serial_puts(" → PM sel ");
                    serial_puthex(env_sel);
                    serial_puts(" (base=");
                    serial_puthex((uint32_t)env_seg << 4);
                    serial_puts(")\n");
                }
            }
        }
    }

    /* 3. Save V86 state for potential return */
    c->v86_cs  = frame->cs;
    c->v86_ds  = frame->v86_ds;
    c->v86_ss  = frame->ss;
    c->v86_es  = frame->v86_es;
    c->v86_eip = frame->eip;
    c->v86_esp = frame->esp;

    /* 4. Modify the IRET frame for Ring 3 PM return.
     *
     * Clear VM flag → IRET does normal PM return (no extra segment pops).
     * Set CS/SS to LDT selectors. DS/ES/FS/GS are set via the isr_stubs
     * save area (ds_stub/es_stub/fs/gs fields in the frame). */
    frame->eflags &= ~(1 << 17);     /* clear VM */
    frame->eflags |= (3 << 12);      /* IOPL=3 (allow port I/O from Ring 3) */
    frame->eflags |= (1 << 9);       /* set IF */
    frame->eflags |= (1 << 1);       /* reserved bit 1 */
    frame->eflags &= ~1;             /* clear CF (success) */

    frame->cs  = c->client_cs;        /* LDT code selector */
    frame->ss  = c->client_ss;        /* LDT data selector */

    /* EIP and ESP stay the same — client continues at same address */

    /* 5. Set segment registers in the isr_stubs save area.
     * isr_common does: pop gs, pop fs, pop es, pop ds before IRET.
     * These map to frame->gs, frame->fs, frame->es_stub, frame->ds_stub. */
    frame->ds_stub = c->client_ds;
    frame->es_stub = c->client_es;
    frame->fs      = 0;  /* null selector */
    frame->gs      = 0;  /* null selector */

    serial_puts("DPMI: V86 → PM transition complete\n");
    serial_puts("  CS=");  serial_puthex(c->client_cs);
    serial_puts("  DS=");  serial_puthex(c->client_ds);
    serial_puts("  ES=");  serial_puthex(c->client_es);
    serial_puts("  SS=");  serial_puthex(c->client_ss);
    serial_puts("  frame->es_stub="); serial_puthex(frame->es_stub);
    serial_puts("\n  v86_es="); serial_puthex(c->v86_es);
    serial_puts(" PSP base="); serial_puthex(desc_get_base(&c->ldt[SEL_TO_IDX(c->client_es)]));
    serial_puts("\n  EIP="); serial_puthex(frame->eip);
    serial_puts("  ESP="); serial_puthex(frame->esp);
    serial_puts("  EFLAGS="); serial_puthex(frame->eflags);
    serial_puts(c->is_32bit ? " 32bit" : " 16bit");
    serial_puts("  LDT.CS.limhi=");
    serial_puthex(c->ldt[SEL_TO_IDX(c->client_cs)].limit_hi);
    serial_puts("\n");

    /* s49: full _stubinfo stamp (replaces s47 3-field stamp).
     *
     * Per djgpp/include/stubinfo.h, _GO32_StubInfo is 84 (0x54) bytes
     * with 14 documented fields. The 16-bit go32 DOS stub normally
     * fills them all before invoking the 32-bit CRT. We bypass the
     * 16-bit stub entirely (V86 task → PM transition), so every byte
     * we don't stamp here falls back to whatever the linker happened
     * to place at that offset in the binary's data section — usually
     * fragments of other globals that the CRT misreads as plausible
     * sizes/selectors and trips on (the "s38 family"). Stamping the
     * full struct removes the binary-layout dependency entirely.
     *
     * Layout (offset / size / field):
     *   +0x00 / 16 / magic[]          identifier string
     *   +0x10 /  4 / size             sizeof _GO32_StubInfo (0x54)
     *   +0x14 /  4 / minstack         min stack bytes CRT wants
     *   +0x18 /  4 / memory_handle    DPMI handle of initial alloc
     *   +0x1C /  4 / initial_size     bytes of initial DPMI alloc
     *   +0x20 /  2 / minkeep          DOS memory to keep (paragraphs)
     *   +0x22 /  2 / ds_selector      DS for the 32-bit program
     *   +0x24 /  2 / ds_segment       RM segment alias of DS (0 here)
     *   +0x26 /  2 / psp_selector     PSP selector
     *   +0x28 /  2 / cs_selector      CS for the 32-bit program
     *   +0x2A /  2 / env_size         env block size in paragraphs
     *   +0x2C /  8 / basename[]       exe basename
     *   +0x34 / 16 / argv0[]          full exe path
     *   +0x44 / 16 / dpmi_server[]    DPMI server name
     *
     * Assumption: _stubinfo lives at [ds_base+0]. This matches what
     * the s47 partial stamp already assumed and verified empirically
     * (writing +0x22/26/28 with selectors produced the expected
     * 0x8F/0x97/0x87 readback in dumps). The DJGPP build chain places
     * _stubinfo at the start of the program's DS, because the go32
     * stub allocates this region first.
     */
    {
        uint32_t ds_base = desc_get_base(&c->ldt[SEL_TO_IDX(c->client_ds)]);
        int page0_ok = vmm_get_physical(ds_base & ~0xFFFu) != 0;
        int page1_ok = vmm_get_physical((ds_base + 0x54) & ~0xFFFu) != 0;

        /* Pre-stamp dump: full 84 bytes so we can see which offsets were
         * poisoned in this binary's layout vs. our clean stamp. */
        serial_puts("  stubinfo-pre [ds_base+0x00..0x53]:\n   ");
        for (int i = 0; i < 0x54; i++) {
            uint32_t a = ds_base + i;
            if (vmm_get_physical(a & ~0xFFFu)) {
                uint8_t b = *(volatile uint8_t *)a;
                if (b < 0x10) serial_puts("0");
                serial_puthex(b);
                serial_puts(" ");
            } else {
                serial_puts("-- ");
            }
            if ((i & 0xF) == 0xF) serial_puts("\n   ");
        }
        serial_puts("\n");

        if (page0_ok && page1_ok) {
            volatile uint8_t  *si  = (volatile uint8_t  *)ds_base;
            volatile uint16_t *si16;
            volatile uint32_t *si32;
            int i;

            /* Zero whole struct first — any field we don't explicitly
             * populate is then deterministic (0) rather than poisoned. */
            for (i = 0; i < 0x54; i++) si[i] = 0;

            /* +0x00 magic[16]: matches DJGPP go32 v2.05 stub format. The
             * CRT doesn't strictly verify but tools like `stubedit` do. */
            {
                static const char magic[16] = "go32stub, v 2.05";
                for (i = 0; i < 16; i++) si[i] = (uint8_t)magic[i];
            }

            /* +0x10 size = sizeof _GO32_StubInfo */
            si32 = (volatile uint32_t *)(ds_base + 0x10);  *si32 = 0x54;
            /* +0x14 minstack = 1 MB (DJGPP default crt0 minstack) */
            si32 = (volatile uint32_t *)(ds_base + 0x14);  *si32 = 0x100000;
            /* +0x18 memory_handle = 0 (we don't expose a handle for the
             * initial alloc; CRT stores it for free-on-exit and that path
             * is a no-op for handle 0). */
            si32 = (volatile uint32_t *)(ds_base + 0x18);  *si32 = 0;
            /* +0x1C initial_size = bytes of the initial DPMI alloc.
             * Used by CRT sbrk bookkeeping to know how much of region 0
             * is preallocated. 64 KB is safe — any further growth goes
             * through INT 31h 0x0501 and gets new regions. */
            si32 = (volatile uint32_t *)(ds_base + 0x1C);  *si32 = 0x10000;
            /* +0x20 minkeep = paragraphs of DOS memory to keep (trampolines).
             * 0x200 paragraphs = 8 KB, the DJGPP go32 default. */
            si16 = (volatile uint16_t *)(ds_base + 0x20);  *si16 = 0x200;
            /* +0x22 ds_selector (was s47) */
            si16 = (volatile uint16_t *)(ds_base + 0x22);  *si16 = c->client_ds;
            /* +0x24 ds_segment = real-mode segment alias (we don't expose
             * one for the 32-bit DS — kept 0). */
            si16 = (volatile uint16_t *)(ds_base + 0x24);  *si16 = 0;
            /* +0x26 psp_selector (was s47) */
            si16 = (volatile uint16_t *)(ds_base + 0x26);  *si16 = c->client_es;
            /* +0x28 cs_selector (was s47) */
            si16 = (volatile uint16_t *)(ds_base + 0x28);  *si16 = c->client_cs;
            /* +0x2A env_size = env block size in paragraphs.
             * 0x100 paragraphs = 4 KB, larger than any env we build. */
            si16 = (volatile uint16_t *)(ds_base + 0x2A);  *si16 = 0x100;
            /* +0x2C basename[8] = "PROGRAM" — generic. The exe name isn't
             * plumbed through to PM transition yet; CRT only uses this
             * to set argv[0] when stripping the path. */
            {
                static const char bn[8] = "PROGRAM";
                for (i = 0; i < 8; i++) si[0x2C + i] = (uint8_t)bn[i];
            }
            /* +0x34 argv0[16] = empty (already zeroed). */
            /* +0x44 dpmi_server[16] = "CWSDPMI" — the most-common DPMI
             * host name. CRT may apply server-specific quirks based on
             * this string; claiming CWSDPMI is the safest default. */
            {
                static const char dpsrv[8] = "CWSDPMI";
                for (i = 0; i < 8; i++) si[0x44 + i] = (uint8_t)dpsrv[i];
            }

            serial_puts("  stubinfo-stamp v2: 84 bytes seeded"
                        " (ds=");
            serial_puthex(c->client_ds);
            serial_puts(" cs=");
            serial_puthex(c->client_cs);
            serial_puts(" psp=");
            serial_puthex(c->client_es);
            serial_puts(")\n");

            /* Post-stamp dump: verify the bytes landed. */
            serial_puts("  stubinfo-post [ds_base+0x00..0x53]:\n   ");
            for (i = 0; i < 0x54; i++) {
                uint8_t b = *(volatile uint8_t *)(ds_base + i);
                if (b < 0x10) serial_puts("0");
                serial_puthex(b);
                serial_puts(" ");
                if ((i & 0xF) == 0xF) serial_puts("\n   ");
            }
            serial_puts("\n");
        } else {
            serial_puts("  stubinfo-stamp SKIPPED — ds_base page not mapped\n");
        }
    }

    return 0;
}

/* ================================================================
 * V86MT VT helpers (Phase 4.7 M3 — foundation for M4)
 * ================================================================ */

struct dpmi_v86mt_vt *v86mt_vt_get(int client_id, uint16_t handle) {
    if (client_id < 0 || client_id >= DPMI_MAX_CLIENTS) return 0;
    if (handle < 1 || handle > DPMI_V86MT_MAX_VTS) return 0;
    struct dpmi_v86mt_vt *v = &clients[client_id].v86mt_vts[handle - 1];
    if (!v->used) return 0;
    return v;
}

__attribute__((unused))
/* Mirror a single VGA-text cell into the v86 task's sandbox page so the
 * INT 21h write survives the next sandbox→buf sync inside v86mt_poll. We
 * run in PM Ring 0 with the v86 task's CR3 active, so virtual 0xB8000
 * resolves to the sandbox automatically. */
static inline void v86mt_mirror_cell(uint32_t cell, uint8_t ch, uint8_t attr) {
    volatile uint8_t *vga = (volatile uint8_t *)0xB8000;
    vga[cell * 2 + 0] = ch;
    vga[cell * 2 + 1] = attr;
}

static void v86mt_vt_scroll_up(struct dpmi_v86mt_vt *v) {
    uint32_t row = v->cols;
    uint32_t cells, i;
    for (uint8_t r = 0; r < v->rows - 1; r++) {
        memcpy(v->char_buf + r * row, v->char_buf + (r + 1) * row, row);
        memcpy(v->attr_buf + r * row, v->attr_buf + (r + 1) * row, row);
    }
    memset(v->char_buf + (v->rows - 1) * row, 0x20, row);
    memset(v->attr_buf + (v->rows - 1) * row, 0x07, row);
    /* Mirror the entire surface into the sandbox so direct-VGA writers
     * (EDIT) see the same scrolled contents on their next read. */
    cells = (uint32_t)v->cols * v->rows;
    for (i = 0; i < cells; i++)
        v86mt_mirror_cell(i, v->char_buf[i], v->attr_buf[i]);
}

static void v86mt_vt_clear(struct dpmi_v86mt_vt *v) {
    uint32_t n, i;
    if (!v || !v->char_buf || !v->attr_buf) return;
    n = (uint32_t)v->cols * v->rows;
    memset(v->char_buf, 0x20, n);
    memset(v->attr_buf, 0x07, n);
    for (i = 0; i < n; i++) v86mt_mirror_cell(i, 0x20, 0x07);
    v->cursor_x = 0;
    v->cursor_y = 0;
    v->screen_dirty++;
}

/* Teletype write — called from the DOS shim's INT 21h AH=02/09
 * handlers when the current V86 task is V86MT-owned. Handles
 * \r \n \b \t plus CP437 codepoints. */
void v86mt_vt_putc(struct dpmi_v86mt_vt *v, uint8_t ch, uint8_t attr) {
    if (!v || !v->char_buf) return;
    if (ch == '\r') { v->cursor_x = 0; v->screen_dirty++; return; }
    if (ch == '\n') { v->cursor_x = 0; v->cursor_y++; goto wrap; }
    if (ch == '\b') {
        if (v->cursor_x > 0)      v->cursor_x--;
        else if (v->cursor_y > 0) { v->cursor_y--; v->cursor_x = v->cols - 1; }
        uint32_t i = v->cursor_y * v->cols + v->cursor_x;
        v->char_buf[i] = 0x20;
        v->attr_buf[i] = attr;
        v86mt_mirror_cell(i, 0x20, attr);
        v->screen_dirty++;
        return;
    }
    if (ch == '\t') {
        uint16_t nx = (v->cursor_x + 8) & ~7;
        if (nx > v->cols) nx = v->cols;
        while (v->cursor_x < nx) {
            uint32_t i = v->cursor_y * v->cols + v->cursor_x;
            v->char_buf[i] = 0x20;
            v->attr_buf[i] = attr;
            v86mt_mirror_cell(i, 0x20, attr);
            v->cursor_x++;
        }
        goto wrap;
    }
    {
        uint32_t i = v->cursor_y * v->cols + v->cursor_x;
        v->char_buf[i] = ch;
        v->attr_buf[i] = attr;
        v86mt_mirror_cell(i, ch, attr);
        v->cursor_x++;
    }
wrap:
    if (v->cursor_x >= v->cols) { v->cursor_x = 0; v->cursor_y++; }
    if (v->cursor_y >= v->rows) {
        v86mt_vt_scroll_up(v);
        v->cursor_y = v->rows - 1;
    }
    v->screen_dirty++;
}

/* V86MT M6 — keyboard ring drain. The ring is a 16-byte header plus
 * size × 2-byte entries. Header layout (matches V86MT-API.md):
 *   +0  head     producer cursor (client writes)
 *   +2  tail     consumer cursor (host writes)
 *   +4  size     entries
 *   +6  flags    bit0=shift, bit1=ctrl, bit2=alt (modifier mirror)
 *   +8..+15      reserved
 *   +16          entries[0..size-1], each (scancode<<8 | ascii)
 * Empty ⇔ head == tail. peek leaves tail alone; pop advances it. */
int v86mt_kbd_peek(struct dpmi_v86mt_vt *v, uint16_t *out) {
    if (!v || !v->kbd_buf) return 0;
    uint16_t head = *(volatile uint16_t *)(v->kbd_buf + 0);
    uint16_t tail = *(volatile uint16_t *)(v->kbd_buf + 2);
    uint16_t size = *(volatile uint16_t *)(v->kbd_buf + 4);
    if (head == tail || !size) return 0;
    if (out)
        *out = *(uint16_t *)(v->kbd_buf + 16 + tail * 2);
    return 1;
}

int v86mt_kbd_pop(struct dpmi_v86mt_vt *v, uint16_t *out) {
    if (!v86mt_kbd_peek(v, out)) return 0;
    uint16_t tail = *(volatile uint16_t *)(v->kbd_buf + 2);
    uint16_t size = *(volatile uint16_t *)(v->kbd_buf + 4);
    tail = (tail + 1) % size;
    *(volatile uint16_t *)(v->kbd_buf + 2) = tail;
    return 1;
}

/* ================================================================
 * INT 31h Dispatcher
 * ================================================================ */

int dpmi_int31(int client_id, struct dpmi_regs *regs) {
    struct dpmi_client *c = dpmi_get_client(client_id);
    uint16_t func;

    if (!c) return 1;  /* error */

    func = regs->eax & 0xFFFF;

    /* Verbose tracing — log non-repetitive calls, suppress bulk reads */
    {
        static uint16_t last_func = 0xFFFF;
        static int suppress = 0;
        if (func != last_func || (func != 0x0204 && func != 0x0202)) {
            if (suppress > 0) {
                serial_puts("  (... ");
                serial_puthex(suppress);
                serial_puts(" calls suppressed)\n");
                suppress = 0;
            }
            serial_puts("DPMI: INT 31h AX=");
            serial_puthex(func);
            serial_puts(" BX=");
            serial_puthex(regs->ebx & 0xFFFF);
            serial_puts(" CX=");
            serial_puthex(regs->ecx & 0xFFFF);
            serial_puts(" DX=");
            serial_puthex(regs->edx & 0xFFFF);
            serial_puts("\n");
        } else {
            suppress++;
        }
        last_func = func;
    }

    switch (func) {

    /* ---- LDT Descriptor Management ---- */

    case 0x0000: {
        /* Allocate LDT descriptors: CX=count → AX=first selector */
        int idx = ldt_alloc(c, regs->ecx & 0xFFFF);
        if (idx) {
            regs->eax = (regs->eax & 0xFFFF0000) | LDT_SEL(idx);
            return 0;
        }
        return 1;
    }

    case 0x0001: {
        /* Free LDT descriptor: BX=selector */
        int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
        if (idx >= DPMI_LDT_FIRST && idx < DPMI_LDT_ENTRIES && LDT_USED(c, idx)) {
            ldt_free(c, idx);
            return 0;
        }
        return 1;
    }

    case 0x0002: {
        /* Segment-to-descriptor: BX=RM segment → AX=PM selector.
         * Aliases a 64K data descriptor over the RM segment. (DPMI 0.9
         * §3.1, DOS/32A int31h_tab.) DOS/4GW uses this for RM-pointer
         * aliasing into PM. */
        int idx = ldt_alloc(c, 1);
        if (!idx) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x8011;
            return 1;
        }
        uint32_t base = (uint32_t)(regs->ebx & 0xFFFF) << 4;
        ldt_setup(c, idx, base, 0xFFFF, 0xF2, 0);
        regs->eax = (regs->eax & 0xFFFF0000) | LDT_SEL(idx);
        return 0;
    }

    case 0x000D: {
        /* Allocate Specific LDT Descriptor: BX=requested selector.
         * (Research doc 37 §7.1, DPMI 0.9.) Reserve a specific LDT slot
         * — DOS/4GW uses this for fixed-slot conventions. Slots 0..15
         * are reserved for client use; succeeds if free. */
        int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
        if (idx <= 0 || idx >= DPMI_LDT_FIRST || LDT_USED(c, idx)) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x8011;  /* unavailable */
            return 1;
        }
        c->ldt[idx].limit_lo = 0;
        c->ldt[idx].base_lo  = 0;
        c->ldt[idx].base_mid = 0;
        c->ldt[idx].base_hi  = 0;
        c->ldt[idx].access   = 0xF2;   /* P=1 DPL=3 data r/w — marks used */
        c->ldt[idx].limit_hi = 0x40;   /* D=1 */
        serial_puts("DPMI: 000D alloc-specific sel ");
        serial_puthex(regs->ebx & 0xFFFF);
        serial_puts("\n");
        return 0;
    }

    case 0x0003:
        /* Get selector increment value → AX=8 */
        regs->eax = (regs->eax & 0xFFFF0000) | 8;
        return 0;

    case 0x0006: {
        /* Get segment base: BX=selector → CX:DX=base */
        int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
        if (idx < DPMI_LDT_ENTRIES && LDT_USED(c, idx)) {
            uint32_t base = desc_get_base(&c->ldt[idx]);
            regs->ecx = (regs->ecx & 0xFFFF0000) | (base >> 16);
            regs->edx = (regs->edx & 0xFFFF0000) | (base & 0xFFFF);
            return 0;
        }
        regs->eax = (regs->eax & 0xFFFF0000) | 0x8021;  /* invalid selector */
        return 1;
    }

    case 0x0007: {
        /* Set segment base: BX=selector, CX:DX=base */
        int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
        if (idx < DPMI_LDT_ENTRIES && LDT_USED(c, idx)) {
            uint32_t base = ((regs->ecx & 0xFFFF) << 16) | (regs->edx & 0xFFFF);
            desc_set_base(&c->ldt[idx], base);
            return 0;
        }
        regs->eax = (regs->eax & 0xFFFF0000) | 0x8021;
        return 1;
    }

    case 0x0008: {
        /* Set segment limit: BX=selector, CX:DX=limit */
        int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
        if (idx < DPMI_LDT_ENTRIES && LDT_USED(c, idx)) {
            uint32_t limit = ((regs->ecx & 0xFFFF) << 16) | (regs->edx & 0xFFFF);
            /* DPMI 0.9 §3.4 / DOSEMU2 dpmi.c:1101: page-granular
             * limits must have low 12 bits = 0xFFF. Mirror of the
             * V86-side check in v86.c. */
            if (limit > 0xFFFFF && (~limit & 0xFFF)) {
                regs->eax = (regs->eax & 0xFFFF0000) | 0x8025;
                return 1;
            }
            desc_set_limit(&c->ldt[idx], limit);
            return 0;
        }
        regs->eax = (regs->eax & 0xFFFF0000) | 0x8021;
        return 1;
    }

    case 0x0009: {
        /* Set descriptor access rights: BX=selector, CX=access word */
        int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
        if (idx < DPMI_LDT_ENTRIES && LDT_USED(c, idx)) {
            c->ldt[idx].access = 0x10 | (regs->ecx & 0xFF);  /* force S=1 */
            c->ldt[idx].limit_hi = (c->ldt[idx].limit_hi & 0x0F) |
                                   (((regs->ecx >> 8) & 0xD0));
            return 0;
        }
        regs->eax = (regs->eax & 0xFFFF0000) | 0x8021;
        return 1;
    }

    case 0x000A: {
        /* Create code segment alias (data selector): BX=code selector */
        int src = SEL_TO_IDX(regs->ebx & 0xFFFF);
        if (src < DPMI_LDT_ENTRIES && LDT_USED(c, src)) {
            int dst = ldt_alloc(c, 1);
            if (dst) {
                c->ldt[dst] = c->ldt[src];
                c->ldt[dst].access = (c->ldt[dst].access & 0xF0) | 0x02; /* data r/w */
                regs->eax = (regs->eax & 0xFFFF0000) | LDT_SEL(dst);
                return 0;
            }
            regs->eax = (regs->eax & 0xFFFF0000) | 0x8011;  /* no free LDT */
            return 1;
        }
        regs->eax = (regs->eax & 0xFFFF0000) | 0x8021;  /* invalid selector */
        return 1;
    }

    case 0x000B: {
        /* Get Descriptor: BX=selector, ES:EDI→8-byte buffer */
        int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
        int es_idx = SEL_TO_IDX(regs->es & 0xFFFF);
        if (idx < DPMI_LDT_ENTRIES && LDT_USED(c, idx) &&
            es_idx < DPMI_LDT_ENTRIES && LDT_USED(c, es_idx)) {
            uint32_t buf_addr = desc_get_base(&c->ldt[es_idx]) +
                                (regs->edi & 0xFFFF);
            /* Valid range: identity-mapped low memory OR allocated DPMI heap
             * up to c->next_linear. Anything else is unmapped → fault. */
            if (buf_addr + 8 <= c->next_linear) {
                uint8_t *buf = (uint8_t *)buf_addr;
                uint8_t *src = (uint8_t *)&c->ldt[idx];
                int i;
                for (i = 0; i < 8; i++) buf[i] = src[i];
                return 0;
            }
            serial_puts("DPMI: 000B bad buf addr ");
            serial_puthex(buf_addr);
            serial_puts(" ES=");
            serial_puthex(regs->es & 0xFFFF);
            serial_puts(" EDI=");
            serial_puthex(regs->edi);
            serial_puts("\n");
        }
        return 1;
    }

    case 0x000C: {
        /* Set Descriptor: BX=selector, ES:EDI→8-byte buffer.
         * Allow any index >= 1 (index 0 stays null). DOS extenders like
         * DOS/4GW write descriptors to low LDT indices for internal use. */
        int idx = SEL_TO_IDX(regs->ebx & 0xFFFF);
        int es_idx = SEL_TO_IDX(regs->es & 0xFFFF);
        if (idx >= 1 && idx < DPMI_LDT_ENTRIES &&
            es_idx < DPMI_LDT_ENTRIES && LDT_USED(c, es_idx)) {
            uint32_t buf_addr = desc_get_base(&c->ldt[es_idx]) +
                                (regs->edi & 0xFFFF);
            if (buf_addr + 8 <= c->next_linear) {
                uint8_t *buf = (uint8_t *)buf_addr;
                uint8_t *dst = (uint8_t *)&c->ldt[idx];
                int i;
                for (i = 0; i < 8; i++) dst[i] = buf[i];
                /* Force P=1 + DPL=3 (clients trust descriptor; if they
                 * happen to write access without P set we'd silently mark
                 * the segment not-present and the next load #GPs). */
                c->ldt[idx].access |= 0xE0;
                static int set_desc_log = 0;
                if (set_desc_log < 16) {
                    serial_puts("DPMI: 000C sel ");
                    serial_puthex(regs->ebx & 0xFFFF);
                    serial_puts(" desc:");
                    for (i = 0; i < 8; i++) {
                        serial_puts(" ");
                        serial_puthex(dst[i]);
                    }
                    serial_puts("\n");
                    set_desc_log++;
                }
                return 0;
            }
            serial_puts("DPMI: 000C bad buf addr ");
            serial_puthex(buf_addr);
            serial_puts("\n");
        }
        return 1;
    }

    /* ---- DOS Memory Management ---- */

    case 0x0100: {
        /* Allocate DOS memory: BX=paragraphs → AX=RM segment, DX=selector.
         *
         * Ses35: previously handed out fake segments at `0x3000 + idx*0x100`
         * without reserving the linear range, which collided with V86 task
         * memory and let the DJGPP stub's `mov [stubinfo+0x26], es` write
         * land in a region later clobbered. Now route through the V86 task's
         * real `next_alloc_seg` bump-allocator so the returned segment is
         * exclusive conventional memory the client can use safely. */
        uint16_t paras = regs->ebx & 0xFFFF;
        struct task *self = sched_get_task(sched_current());
        uint16_t seg = (self && self->dos_task_id >= 0)
                       ? dos_alloc_paragraphs(self->dos_task_id, paras) : 0;
        if (!seg) {
            /* Out of conventional memory — DPMI error 8 + BX=max-available. */
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0008;
            regs->ebx = (regs->ebx & 0xFFFF0000) | 0;
            return 1;
        }
        int idx = ldt_alloc(c, 1);
        if (!idx) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x8011;
            return 1;
        }
        ldt_setup(c, idx, (uint32_t)seg << 4, (uint32_t)paras << 4,
                  0xF2, 0);  /* data, DPL=3, 16-bit */
        regs->eax = (regs->eax & 0xFFFF0000) | seg;
        regs->edx = (regs->edx & 0xFFFF0000) | LDT_SEL(idx);
        return 0;
    }

    case 0x0101: {
        /* Free DOS memory: DX=selector */
        int idx = SEL_TO_IDX(regs->edx & 0xFFFF);
        if (idx >= DPMI_LDT_FIRST && idx < DPMI_LDT_ENTRIES) {
            ldt_free(c, idx);
            return 0;
        }
        return 1;
    }

    case 0x0102: {
        /* Resize DOS memory block: BX=new paragraphs, DX=selector.
         * Our 0x0100 allocator hands out 0x100-paragraph slots, so any
         * size ≤ 0x100 fits; larger requests fail with AX=0x8014 and
         * BX = max-available. (DPMI 0.9, DOS/32A int31h_tab.) */
        uint16_t new_para = regs->ebx & 0xFFFF;
        int idx = SEL_TO_IDX(regs->edx & 0xFFFF);
        if (idx < DPMI_LDT_FIRST || idx >= DPMI_LDT_ENTRIES) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x8022;
            return 1;
        }
        if (new_para > 0x100) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x8014;
            regs->ebx = (regs->ebx & 0xFFFF0000) | 0x100;
            return 1;
        }
        desc_set_limit(&c->ldt[idx], new_para ? ((uint32_t)new_para << 4) - 1 : 0);
        return 0;
    }

    /* ---- Interrupt Management ---- */

    case 0x0200: {
        /* Get RM interrupt vector: BL=int# → CX:DX=seg:off */
        uint8_t int_num = regs->ebx & 0xFF;
        uint16_t *ivt = (uint16_t *)(int_num * 4);
        regs->ecx = (regs->ecx & 0xFFFF0000) | ivt[1];
        regs->edx = (regs->edx & 0xFFFF0000) | ivt[0];
        return 0;
    }

    case 0x0201: {
        /* Set RM interrupt vector: BL=int#, CX:DX=seg:off */
        uint8_t int_num = regs->ebx & 0xFF;
        uint16_t *ivt = (uint16_t *)(int_num * 4);
        ivt[0] = regs->edx & 0xFFFF;
        ivt[1] = regs->ecx & 0xFFFF;
        return 0;
    }

    case 0x0202: {
        /* Get Processor Exception Handler Vector: BL=exception# → CX:EDX.
         * Backed by pm_exc_vectors[] — separate from PM INT vectors so that
         * installing an exception 0x08 (double-fault) handler doesn't make
         * us route PIT IRQs to it. */
        uint8_t exc = regs->ebx & 0xFF;
        if (exc < 32) {
            regs->ecx = (regs->ecx & 0xFFFF0000) | c->pm_exc_vectors[exc].selector;
            regs->edx = c->pm_exc_vectors[exc].offset;
            return 0;
        }
        return 1;
    }

    case 0x0203: {
        /* Set Processor Exception Handler Vector: BL=exception#, CX:EDX */
        uint8_t exc = regs->ebx & 0xFF;
        if (exc < 32) {
            c->pm_exc_vectors[exc].selector = regs->ecx & 0xFFFF;
            c->pm_exc_vectors[exc].offset = regs->edx;
            serial_puts("DPMI: set exception ");
            serial_puthex(exc);
            serial_puts(" = ");
            serial_puthex(regs->ecx & 0xFFFF);
            serial_puts(":");
            serial_puthex(regs->edx);
            serial_puts("\n");
            return 0;
        }
        return 1;
    }

    case 0x0204: {
        /* Get PM interrupt vector: BL=int# → CX:EDX=selector:offset */
        uint8_t int_num = regs->ebx & 0xFF;
        regs->ecx = (regs->ecx & 0xFFFF0000) | c->pm_vectors[int_num].selector;
        regs->edx = c->pm_vectors[int_num].offset;
        return 0;
    }

    case 0x0205: {
        /* Set PM interrupt vector: BL=int#, CX:EDX=selector:offset */
        uint8_t int_num = regs->ebx & 0xFF;
        c->pm_vectors[int_num].selector = regs->ecx & 0xFFFF;
        c->pm_vectors[int_num].offset = regs->edx;
        serial_puts("DPMI: set PM vector ");
        serial_puthex(int_num);
        serial_puts(" = ");
        serial_puthex(regs->ecx & 0xFFFF);
        serial_puts(":");
        serial_puthex(regs->edx);
        serial_puts("\n");
        return 0;
    }

    /* ---- Translation Services ---- */

    case 0x0300: {
        /* Simulate real-mode interrupt: BL=int#, ES:EDI→DPMI regs */
        /* Route to our existing DOS/BIOS emulation instead of actually
         * entering V86 mode — much simpler and works because we already
         * handle all the INT 21h/10h/16h etc. services */
        uint8_t int_num = regs->ebx & 0xFF;
        struct dpmi_regs *rm_regs = (struct dpmi_regs *)
            (desc_get_base(&c->ldt[SEL_TO_IDX(regs->es & 0xFFFF)]) +
             (regs->edi & 0xFFFFFFFF));
        int dos_tid = v86_get_dos_task(c->v86_task_id);

        serial_puts("DPMI: 0300 INT ");
        serial_puthex(int_num);
        serial_puts(" AX=");
        serial_puthex(rm_regs->eax & 0xFFFF);
        serial_puts("\n");

        if (int_num == 0x21) {
            /* DOS services — route to our dos_int21 */
            struct dos_regs dregs;
            dregs.eax = rm_regs->eax;
            dregs.ebx = rm_regs->ebx;
            dregs.ecx = rm_regs->ecx;
            dregs.edx = rm_regs->edx;
            dregs.esi = rm_regs->esi;
            dregs.edi = rm_regs->edi;
            dregs.ds  = rm_regs->ds;
            dregs.es  = rm_regs->es;
            dregs.eflags = rm_regs->flags;
            dos_int21(dos_tid, &dregs);
            rm_regs->eax = dregs.eax;
            rm_regs->ebx = dregs.ebx;
            rm_regs->ecx = dregs.ecx;
            rm_regs->edx = dregs.edx;
            rm_regs->esi = dregs.esi;
            rm_regs->edi = dregs.edi;
            rm_regs->flags = dregs.eflags & 0xFFFF;
            /* s45 trace: log what we wrote back into the RmCallStruct so we
             * can verify return values reach the caller's PUSHAD frame. */
            serial_puts("DPMI: 0300 ret EAX=");
            serial_puthex(rm_regs->eax);
            serial_puts(" EBX=");
            serial_puthex(rm_regs->ebx);
            serial_puts(" CF=");
            serial_puthex(rm_regs->flags & 1);
            serial_puts("\n");
        } else if (int_num == 0x10) {
            /* Video BIOS — mode setting through our VGA driver */
            uint8_t ah = (rm_regs->eax >> 8) & 0xFF;
            if (ah == 0x00) {
                uint8_t mode = rm_regs->eax & 0x7F;
                extern void vga_set_mode_13h(void);
                extern void vga_set_mode_03h(void);
                if (mode == 0x13) vga_set_mode_13h();
                else vga_set_mode_03h();
                /* Record per-VT video state so vt_switch knows how to
                 * save/restore this VT's framebuffer on background. */
                {
                    int vtn = sched_vt_for_v86(c->v86_task_id);
                    struct vt *v = vt_get(vtn);
                    if (v) {
                        v->video = (mode == 0x13) ? VT_VID_GFX_13H : VT_VID_TEXT_03H;
                        serial_puts("VT: video=");
                        serial_puts((mode == 0x13) ? "GFX_13H" : "TEXT_03H");
                        serial_puts(" vt=");
                        serial_puthex(vtn);
                        serial_puts(" (INT10/AH=00)\n");
                    }
                }
            } else if (ah == 0x4F) {
                /* VBE/VESA functions. Allegro's vesa_init calls 4F00 first
                 * (Get Controller Info), iterates 4F01 (Get Mode Info) over
                 * the returned mode list, then 4F02 (Set Mode). Backed by
                 * vbe.c's Bochs VBE driver for the actual mode-set. */
                uint8_t al = rm_regs->eax & 0xFF;
                uint16_t es = rm_regs->es & 0xFFFF;
                uint16_t di = rm_regs->edi & 0xFFFF;
                uint8_t *buf = (uint8_t *)(((uint32_t)es << 4) + di);
                static const uint16_t vesa_modes[] = {
                    0x0101,  /* 640x480x8   */
                    0x0111,  /* 640x480x16  */
                    0x0114,  /* 800x600x16  */
                    0x0117,  /* 1024x768x16 */
                    0xFFFF,
                };
                static uint16_t current_vesa_mode = 0x0003;  /* text mode at boot */

                if (al == 0x00) {
                    /* Get Controller Info: 256-byte VBE 2.0 info block. */
                    for (int i = 0; i < 256; i++) buf[i] = 0;
                    buf[0]='V'; buf[1]='E'; buf[2]='S'; buf[3]='A';
                    *(uint16_t *)(buf + 0x04) = 0x0200;
                    *(uint32_t *)(buf + 0x06) =
                        ((uint32_t)es << 16) | ((uint16_t)(di + 0xC0));
                    *(uint32_t *)(buf + 0x0A) = 0;
                    *(uint32_t *)(buf + 0x0E) =
                        ((uint32_t)es << 16) | ((uint16_t)(di + 0xE0));
                    *(uint16_t *)(buf + 0x12) = 0x40;  /* 4 MB / 64K */
                    {
                        static const char oem[] = "PineCore VBE";
                        for (unsigned i = 0; i < sizeof(oem); i++)
                            buf[0xC0 + i] = oem[i];
                    }
                    for (unsigned i = 0;
                         i < sizeof(vesa_modes)/sizeof(vesa_modes[0]); i++)
                        ((uint16_t *)(buf + 0xE0))[i] = vesa_modes[i];
                    rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0x004F;
                } else if (al == 0x01) {
                    /* Get Mode Info: CX = mode, fill 256-byte block. */
                    uint16_t mode = rm_regs->ecx & 0xFFFF;
                    uint16_t w = 0, h = 0;
                    uint8_t bpp = 0;
                    switch (mode) {
                        case 0x0101: w=640;  h=480;  bpp=8;  break;
                        case 0x0111: w=640;  h=480;  bpp=16; break;
                        case 0x0114: w=800;  h=600;  bpp=16; break;
                        case 0x0117: w=1024; h=768;  bpp=16; break;
                        default:
                            rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0x014F;
                            break;
                    }
                    if (w) {
                        for (int i = 0; i < 256; i++) buf[i] = 0;
                        /* ModeAttributes: bit 0=supported, 3=color,
                         * 4=graphics, 7=LFB available. */
                        *(uint16_t *)(buf + 0x00) = 0x0099;
                        *(uint16_t *)(buf + 0x10) = w * (bpp / 8);
                        *(uint16_t *)(buf + 0x12) = w;
                        *(uint16_t *)(buf + 0x14) = h;
                        buf[0x16] = 8;   /* XCharSize */
                        buf[0x17] = 16;  /* YCharSize */
                        buf[0x18] = 1;   /* NumberOfPlanes */
                        buf[0x19] = bpp;
                        buf[0x1A] = 1;   /* NumberOfBanks */
                        buf[0x1B] = (bpp == 8) ? 4 : 6;  /* 4=paletted, 6=RGB */
                        if (bpp == 16) {
                            buf[0x1F] = 5;  buf[0x20] = 11; /* Red 5:11 */
                            buf[0x21] = 6;  buf[0x22] = 5;  /* Green 6:5 */
                            buf[0x23] = 5;  buf[0x24] = 0;  /* Blue 5:0 */
                        }
                        /* VBE 2.0: PhysBasePtr at +0x28 → LFB discovered
                         * from PCI BAR0 at vbe_init time. */
                        *(uint32_t *)(buf + 0x28) = vbe_lfb_phys();
                        rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0x004F;
                    }
                } else if (al == 0x0A) {
                    /* Return Protected Mode Interface (VBE 2.0+).
                     * Allegro's vesa.c get_pmode_functions REQUIRES this
                     * call to succeed before it'll take the LFB path —
                     * if it fails, Allegro falls back to gfx_vesa_1
                     * (VBE 1.x banked) which writes to 0xA000:0000 and
                     * never reaches the LFB on our host. We use LFB only,
                     * so banking is no-op: emit a 16-byte PMI table with
                     * three RET-near stubs (setWindow/setDisplayStart/
                     * setPalette) at fixed linear 0x900 (V86-accessible,
                     * past BDA, no overlap with our V86 stubs at 0x550). */
                    static const uint8_t pmi_template[16] = {
                        0x08, 0x00, /* setWindow      offset = 8  */
                        0x09, 0x00, /* setDisplayStart offset = 9 */
                        0x0A, 0x00, /* setPalette     offset = 10 */
                        0x00, 0x00, /* IOPrivInfo = 0 (no MMIO)   */
                        0xC3,       /* setWindow:       RET near  */
                        0xC3,       /* setDisplayStart: RET near  */
                        0xC3,       /* setPalette:      RET near  */
                        0, 0, 0, 0, 0
                    };
                    uint8_t *pmi_buf = (uint8_t *)0x900;
                    for (unsigned i = 0; i < 16; i++)
                        pmi_buf[i] = pmi_template[i];
                    rm_regs->es  = 0x0090;
                    rm_regs->edi = 0;
                    rm_regs->ecx = (rm_regs->ecx & 0xFFFF0000) | 16;
                    rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0x004F;
                } else if (al == 0x02) {
                    /* Set Mode: BX = mode (bit 14=LFB, bit 15=no clear). */
                    uint16_t mode = rm_regs->ebx & 0x3FFF;
                    uint16_t w = 0, h = 0;
                    uint8_t bpp = 0;
                    switch (mode) {
                        case 0x0101: w=640;  h=480;  bpp=8;  break;
                        case 0x0111: w=640;  h=480;  bpp=16; break;
                        case 0x0114: w=800;  h=600;  bpp=16; break;
                        case 0x0117: w=1024; h=768;  bpp=16; break;
                    }
                    if (w && vbe_set_mode(w, h, bpp, 0) == 0) {
                        current_vesa_mode = mode;
                        /* Rescale kernel mouse driver to the new screen
                         * size — without this, the bounds set at
                         * mouse_init time (default 640x480) won't track
                         * if the client picks 800x600 or 1024x768. The
                         * client may follow up with INT 33h AX=7/8 to
                         * further narrow the range; this just establishes
                         * a sensible default before that lands. */
                        mouse_set_bounds(w, h);
                        /* Record per-VT video state so vt_switch knows
                         * the LFB dimensions to save on background. */
                        {
                            int vtn = sched_vt_for_v86(c->v86_task_id);
                            struct vt *v = vt_get(vtn);
                            if (v) {
                                v->video = VT_VID_GFX_LFB;
                                v->gfx_w = w;
                                v->gfx_h = h;
                                v->gfx_bpp = bpp;
                                serial_puts("VT: video=GFX_LFB w=");
                                serial_puthex(w);
                                serial_puts(" h=");
                                serial_puthex(h);
                                serial_puts(" bpp=");
                                serial_puthex(bpp);
                                serial_puts(" vt=");
                                serial_puthex(vtn);
                                serial_puts("\n");
                            }
                        }
                        rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0x004F;
                    } else {
                        rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0x014F;
                    }
                } else if (al == 0x03) {
                    /* Get Current Mode → BX = current mode number. */
                    rm_regs->ebx = (rm_regs->ebx & 0xFFFF0000) | current_vesa_mode;
                    rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0x004F;
                } else {
                    /* Other 4Fxx: fail. */
                    rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0x014F;
                }
            }
            /* Other INT 10h subfunctions — return success */
        } else if (int_num == 0x33) {
            /* Microsoft Mouse driver INT 33h. Backed by kernel PS/2
             * driver (src/kernel/mouse.c) — IRQ 12 updates pos_x/pos_y/
             * buttons, we read them out for AX=3 polls.
             *
             * Allegro's auto-selected driver under our (WinNT-fingerprinted)
             * host is mousedrv_winnt → init_mouse(NULL) → no AX=0C callback
             * install, just AX=0 (reset) then AX=3 polled from PIT chain.
             * AX=4/7/8/B are set-position/range/mickeys reset, called from
             * set_mouse_range / position_mouse. Mickey units are 1/8 pixel
             * by Allegro convention (int33_position multiplies by 8). */
            uint16_t ax = rm_regs->eax & 0xFFFF;
            uint8_t  ah = (ax >> 8) & 0xFF;
            uint8_t  al = ax & 0xFF;

            static int int33_log_count = 0;
            if (int33_log_count < 16) {
                serial_puts("INT33: AX=");
                serial_puthex(ax);
                serial_puts(" CX=");
                serial_puthex(rm_regs->ecx & 0xFFFF);
                serial_puts(" DX=");
                serial_puthex(rm_regs->edx & 0xFFFF);
                serial_puts("\n");
                int33_log_count++;
            }

            if (ah == 0) {
                if (al == 0x00) {
                    /* Reset / driver-presence. AX=FFFF = present, BX=button
                     * count. PS/2 mice are 3-button standard. */
                    rm_regs->eax = (rm_regs->eax & 0xFFFF0000) | 0xFFFF;
                    rm_regs->ebx = (rm_regs->ebx & 0xFFFF0000) | 3;
                } else if (al == 0x03) {
                    /* Get position & buttons.
                     * Return: BX=button mask, CX=x*8, DX=y*8. */
                    int x = mouse_get_x();
                    int y = mouse_get_y();
                    int b = mouse_get_buttons();
                    rm_regs->ebx = (rm_regs->ebx & 0xFFFF0000) | (b & 0xFFFF);
                    rm_regs->ecx = (rm_regs->ecx & 0xFFFF0000) | ((x * 8) & 0xFFFF);
                    rm_regs->edx = (rm_regs->edx & 0xFFFF0000) | ((y * 8) & 0xFFFF);
                } else if (al == 0x04) {
                    /* Set position. CX=x*8, DX=y*8. */
                    int x = (int)(rm_regs->ecx & 0xFFFF) / 8;
                    int y = (int)(rm_regs->edx & 0xFFFF) / 8;
                    mouse_set_position(x, y);
                } else if (al == 0x07 || al == 0x08) {
                    /* Set horizontal (07) / vertical (08) range. CX=min*8,
                     * DX=max*8. Allegro sends one then the other; combine
                     * them into a single mouse_set_bounds when we've seen
                     * both. Min is almost always 0; trust max as the
                     * exclusive upper bound. */
                    static int range_max_x = 639, range_max_y = 479;
                    int max = (int)(rm_regs->edx & 0xFFFF) / 8;
                    if (al == 0x07) {
                        range_max_x = max;
                    } else {
                        range_max_y = max;
                    }
                    mouse_set_bounds(range_max_x + 1, range_max_y + 1);
                } else if (al == 0x0B) {
                    /* Read motion counters (mickeys since last call). We
                     * track absolute position, not deltas — return 0,0.
                     * Allegro's polling path uses AX=3 not AX=B, so this
                     * is rarely hit; mick driver uses it heavily but mick
                     * isn't selected for us. */
                    rm_regs->ecx = (rm_regs->ecx & 0xFFFF0000);
                    rm_regs->edx = (rm_regs->edx & 0xFFFF0000);
                } else if (al == 0x0C) {
                    /* Install event handler. Win NT driver path passes
                     * NULL, so we shouldn't see this — but if we do,
                     * accept silently. Async RM-callback dispatch is a
                     * future-session item. */
                } else if (al == 0x0F) {
                    /* Set mickey/pixel ratio (sensitivity). No-op success. */
                } else if (al == 0x01 || al == 0x02) {
                    /* Show / hide cursor. Allegro draws its own cursor;
                     * no hardware cursor to flip. */
                }
                /* Other AH=0 subfunctions: silently leave regs alone. */
            }
        } else if (int_num == 0x15) {
            /* Extended services — return CF=1 for unsupported */
            rm_regs->flags |= 1;
        } else if (int_num == 0x16) {
            /* BIOS keyboard. Allegro's pcdos_key_init (dkeybd.c:261)
             * drains the BIOS type-ahead buffer with
             *     while (kbhit()) simulate_keypress(getch());
             * before installing its own keyboard ISR. DJGPP's kbhit()
             * uses INT 16h AH=11h and tests !(flags & 0x40) (ZF) to
             * decide whether a key is pending. If we leave flags
             * untouched the caller-supplied ZF=0 sticks → kbhit
             * lies "key available" → getch() → INT 21h AH=07h which
             * blocks forever. Report "no key" by setting ZF=1, and
             * clear AX so any caller that reads it sees 0. */
            uint8_t ah = (rm_regs->eax >> 8) & 0xFF;
            if (ah == 0x01 || ah == 0x11) {
                rm_regs->flags |= 0x40;
                rm_regs->eax = rm_regs->eax & 0xFFFF0000;
            } else if (ah == 0x02 || ah == 0x12) {
                /* Get shift status — no modifiers currently held. */
                rm_regs->eax = (rm_regs->eax & 0xFFFFFF00);
            } else if (ah == 0x00 || ah == 0x10) {
                /* Blocking get keystroke. Allegro gates this behind
                 * kbhit() so it should not fire once status returns
                 * "no key". Defensive: return scancode/char = 0. */
                rm_regs->eax = rm_regs->eax & 0xFFFF0000;
            } else {
                rm_regs->flags |= 1;  /* unknown sub-fn → CF=1 */
            }
        } else {
            /* Unknown RM INT — log for debugging */
            static int unk_rm_count = 0;
            if (unk_rm_count < 20) {
                serial_puts("DPMI: 0300 unhandled INT ");
                serial_puthex(int_num);
                serial_puts(" AX=");
                serial_puthex(rm_regs->eax & 0xFFFF);
                serial_puts(" DS=");
                serial_puthex(rm_regs->ds);
                serial_puts(" DX=");
                serial_puthex(rm_regs->edx & 0xFFFF);
                serial_puts("\n");
                unk_rm_count++;
            }
        }
        return 0;
    }

    /* ---- Memory Management ---- */

    case 0x0500: {
        /* Get free memory info: ES:EDI → 48-byte buffer.
         *
         * DPMI 0.9 §"INT 31h/0500h" layout (all DWORDs):
         *   00h largest available free block (bytes)
         *   04h max unlocked memory in pages
         *   08h max locked memory in pages
         *   0Ch total linear address space (pages)
         *   10h total unlocked pages
         *   14h total free pages
         *   18h total physical pages
         *   1Ch free linear address space (pages)
         *   20h paging-file size (pages)        <-- 4GW reads this
         *   24h-2Bh reserved zero
         *
         * Match CWSDPMI's shape so DOS/4GW stays on the CWSDPMI-compat
         * codepath: pretend a 64 MB swap file (0x4000 pages * 4KB) even
         * though we don't page. */
        uint32_t *buf = (uint32_t *)
            (desc_get_base(&c->ldt[SEL_TO_IDX(regs->es & 0xFFFF)]) +
             (regs->edi & 0xFFFFFFFF));
        /* Linear addresses are CHEAP under reserve-vs-commit — what's
         * actually scarce is physical commit budget. DJGPP/Allegro/DOOM
         * size their heap from buf[0] "largest available block", so
         * report the linear window remaining from c->next_linear up to
         * DPMI_VADDR_END. Backing-store/physical fields report the real
         * commit headroom so a host-aware client can be polite. */
        uint32_t linear_remaining_bytes = (c->next_linear < DPMI_VADDR_END)
            ? (DPMI_VADDR_END - c->next_linear) : 0;
        uint32_t phys_free_pages = pmm_get_free_count();
        uint32_t commit_headroom_pages = (dpmi_committed_pages < DPMI_COMMIT_CAP_PAGES)
            ? (DPMI_COMMIT_CAP_PAGES - dpmi_committed_pages) : 0;
        /* s43 — DOS/16M [6] fix. Previously we reported the full linear
         * window (2 GB - 32 MB = ~0xBE000000 bytes) for buf[0] and the
         * derived page count for buf[3]/buf[7]. DOS/16M treats buf[0] as
         * SIGNED 32-bit when checking "is this allocation feasible?":
         * 0xBE000000 has the sign bit set → reads as a negative integer
         * → ANY positive required size > "available" → error [6] "not
         * enough memory to load program". Cap reported sizes to the real
         * commit-headroom budget (~24 MB at boot), which is the physical
         * memory we can actually back, keeps the sign bit clear, and
         * matches what 286-class extenders expect from a DPMI host.
         * (DOS/4GW unaffected — it's 32-bit and was happy with either
         * value; this just reports a more honest number to both.) */
        uint32_t headroom_bytes = commit_headroom_pages << 12;
        uint32_t reported_largest_bytes = (linear_remaining_bytes < headroom_bytes)
            ? linear_remaining_bytes : headroom_bytes;
        uint32_t reported_linear_pages = reported_largest_bytes >> 12;
        int i;
        for (i = 0; i < 12; i++) buf[i] = 0;
        buf[0] = reported_largest_bytes;       /* largest available block (bytes) */
        buf[1] = commit_headroom_pages;        /* max unlocked pages */
        buf[2] = commit_headroom_pages;        /* max locked pages */
        buf[3] = reported_linear_pages;        /* total linear pages */
        buf[4] = commit_headroom_pages;        /* total unlocked pages */
        buf[5] = commit_headroom_pages;        /* total free pages */
        buf[6] = phys_free_pages;              /* total physical pages */
        buf[7] = reported_linear_pages;        /* free linear pages */
        buf[8] = 0;                            /* paging-file size (we don't swap yet) */
        serial_puts("DPMI: 0500 reply largest=");
        serial_puthex(reported_largest_bytes);
        serial_puts(" linear_pages=");
        serial_puthex(reported_linear_pages);
        serial_puts(" commit_hr=");
        serial_puthex(commit_headroom_pages);
        serial_puts(" pmm_free=");
        serial_puthex(phys_free_pages);
        serial_puts(" next_linear=");
        serial_puthex(c->next_linear);
        serial_puts(" committed=");
        serial_puthex(dpmi_committed_pages);
        serial_puts("\n");
        return 0;
    }

    case 0x0501: {
        /* Allocate memory block: BX:CX=size → BX:CX=linear addr, SI:DI=handle */
        uint32_t size = ((regs->ebx & 0xFFFF) << 16) | (regs->ecx & 0xFFFF);
        uint32_t base;
        if (memblock_alloc(c, size, &base) < 0)
            return 1;
        regs->ebx = (regs->ebx & 0xFFFF0000) | (base >> 16);
        regs->ecx = (regs->ecx & 0xFFFF0000) | (base & 0xFFFF);
        regs->esi = (regs->esi & 0xFFFF0000) | (base >> 16);
        regs->edi = (regs->edi & 0xFFFF0000) | (base & 0xFFFF);
        serial_puts("DPMI: alloc ");
        serial_puthex(size);
        serial_puts(" bytes at ");
        serial_puthex(base);
        serial_puts("\n");
        return 0;
    }

    case 0x0502: {
        /* Free memory block: SI:DI=handle (=base address) */
        uint32_t base = ((regs->esi & 0xFFFF) << 16) | (regs->edi & 0xFFFF);
        return memblock_free(c, base) < 0 ? 1 : 0;
    }

    case 0x0503: {
        /* Resize memory block — simplified: just report success if growing into free space */
        /* TODO: proper resize */
        return 0;
    }

    case 0x0506:
    case 0x0507: {
        /* Get/Set Page Attributes (DPMI 1.0). DJGPP/Allegro startup calls
         * 0507 right after 0501 to commit pages and mark them r/w. Our
         * memblock_alloc already maps every page eagerly with P|W|U flags
         * (line ~263), so the attributes the client wants are already in
         * effect. Treat both calls as success no-ops.
         *
         * Note: we intentionally don't touch the ES:EDI buffer on 0506.
         * If a client actually reads back attributes (rare — most just
         * call 0507 and assume success) we'd need to fill the array with
         * 0x0001 (committed) per page. Add that lazily if a real client
         * needs it. (Ses34: discovered by Pinecone DESKTOP.EXE faulting
         * with CF=1/0x8001 from default case and writing to "uncommitted"
         * pages.) */
        return 0;
    }

    /* ---- Translation Services (continued) ---- */

    case 0x0301:
    case 0x0302: {
        /* Call real-mode procedure with FAR/IRET return.
         * PM↔RM trampoline (docs/research/29-dpmi-host.md): stash the
         * RmCallStruct address + which return kind it is on the client
         * save, then return code 2 ("switch to V86"). The caller
         * (dpmi_handle_pm_int) does the actual frame manipulation because
         * it has the live isr_frame. The RM proc then runs in V86 until
         * it returns to our sentinel (linear DPMI_SENTINEL_LIN);
         * v86_emulate_int(0xF4) calls dpmi_rm_call_unwind() which copies
         * V86 regs back to the RmCallStruct and restores the PM frame. */
        if (c->rm_call_save.active) {
            /* Nested call — not supported. Return CF=1 / AX=0x8001. */
            regs->eax = (regs->eax & 0xFFFF0000) | 0x8001;
            return 1;
        }
        uint32_t rm_struct_linear =
            desc_get_base(&c->ldt[SEL_TO_IDX(regs->es & 0xFFFF)]) +
            (regs->edi & 0xFFFFFFFF);
        c->rm_call_save.rm_struct_linear = rm_struct_linear;
        c->rm_call_save.is_iret = (func == 0x0302) ? 1 : 0;
        /* active=0 until dpmi_rm_call_setup completes the switch */

        struct dpmi_regs *rm = (struct dpmi_regs *)rm_struct_linear;
        serial_puts("DPMI: 030");
        serial_puthex(func & 0xF);
        serial_puts(" trampoline target=");
        serial_puthex(rm->cs);
        serial_puts(":");
        serial_puthex(rm->ip);
        serial_puts(" AX=");
        serial_puthex(rm->eax & 0xFFFF);
        /* s43 — full RmCallStruct dump to decode AX=0x40DE etc. */
        serial_puts(" BX=");
        serial_puthex(rm->ebx & 0xFFFF);
        serial_puts(" CX=");
        serial_puthex(rm->ecx & 0xFFFF);
        serial_puts(" DX=");
        serial_puthex(rm->edx & 0xFFFF);
        serial_puts(" SI=");
        serial_puthex(rm->esi & 0xFFFF);
        serial_puts(" DI=");
        serial_puthex(rm->edi & 0xFFFF);
        serial_puts(" DS=");
        serial_puthex(rm->ds);
        serial_puts(" ES=");
        serial_puthex(rm->es);
        serial_puts("\n");
        return 2;  /* signal dispatcher to switch to V86 */
    }

    case 0x0303: {
        /* Allocate real-mode callback: DS:ESI=PM proc, ES:EDI=RM regs → CX:DX
         *
         * We write stubs in low memory (0x0500-0x05FF area, avoiding the DPMI
         * entry at 0x500-0x502). Each stub is: CD F2 xx CF (INT 0xF2, id, IRET)
         * = 4 bytes. When V86 code calls the callback, the V86 monitor traps
         * INT 0xF2, reads the callback ID from the next byte, and dispatches
         * to the PM handler stored in the client's rmcb[] array. */
        if (c->next_rmcb < DPMI_MAX_RMCB) {
            int id = c->next_rmcb;
            /* Place stubs at 0x0060:0x0100 (linear 0x0700) to avoid
             * conflicts with DPMI entry (0x500) and IVT IRET pad (0x600). */
            uint16_t seg = 0x0070;  /* 0x0700 linear */
            uint16_t off = id * 4;
            uint8_t *stub = (uint8_t *)(seg * 16 + off);
            stub[0] = 0xCD;  /* INT */
            stub[1] = 0xF2;  /* private "RM callback" vector */
            stub[2] = (uint8_t)id;  /* callback ID */
            stub[3] = 0xCF;  /* IRET (fallback) */

            /* Store PM handler and register buffer addresses */
            c->rmcb[id].pm_sel  = regs->ds & 0xFFFF;
            c->rmcb[id].pm_off  = regs->esi;
            c->rmcb[id].regs_sel = regs->es & 0xFFFF;
            c->rmcb[id].regs_off = regs->edi;
            c->rmcb[id].active  = 1;

            regs->ecx = (regs->ecx & 0xFFFF0000) | seg;
            regs->edx = (regs->edx & 0xFFFF0000) | off;
            serial_puts("DPMI: rmcb ");
            serial_puthex(id);
            serial_puts(" = ");
            serial_puthex(seg);
            serial_puts(":");
            serial_puthex(off);
            serial_puts(" → PM ");
            serial_puthex(regs->ds & 0xFFFF);
            serial_puts(":");
            serial_puthex(regs->esi);
            serial_puts("\n");
            c->next_rmcb++;
            dpmi_timer_ready = 1;  /* init far enough — allow timer delivery */
            return 0;
        }
        return 1;
    }

    case 0x0304: {
        /* Free real-mode callback: CX:DX */
        return 0;
    }

    case 0x0305: {
        /* Get State Save/Restore Addresses.
         * Returns: AX=buffer size, BX:CX=RM callback, SI:EDI=PM callback.
         * Buffer size 0 means no state to save. */
        regs->eax = (regs->eax & 0xFFFF0000) | 0;  /* size = 0 */
        regs->ebx = (regs->ebx & 0xFFFF0000) | 0;   /* RM seg = 0 */
        regs->ecx = (regs->ecx & 0xFFFF0000) | 0;   /* RM off = 0 */
        regs->esi = (regs->esi & 0xFFFF0000) | 0;    /* PM sel = 0 */
        regs->edi = 0;                                /* PM off = 0 */
        return 0;
    }

    case 0x0306: {
        /* Get Raw Mode Switch Addresses.
         * BX:CX = save/call from PM to enter RM (far call from PM)
         * SI:EDI = save/call from RM to enter PM (far call from RM)
         *
         * DOS/4GW uses these for fast mode switching.
         * We provide the IVT IRET stub addresses so the calls are harmless
         * (they just IRET back). Real implementation would need actual
         * PM↔RM switch routines in low memory. */
        regs->ebx = (regs->ebx & 0xFFFF0000) | 0;   /* RM switch seg */
        regs->ecx = (regs->ecx & 0xFFFF0000) | 0x500; /* RM switch off (our DPMI entry) */
        regs->esi = (regs->esi & 0xFFFF0000) | c->client_cs;  /* PM switch sel */
        regs->edi = 0;                                /* PM switch off */
        return 0;
    }

    /* ---- Vendor Extensions ---- */

    case 0x0A00: {
        /* Get Vendor-Specific API Entry Point.
         *
         * Per rgmroman.narod.ru/Dos4g.htm DOS/4G documentation:
         *   Input:  EAX = 0x0A00
         *           DS:(E)DX = ASCIIZ vendor name (e.g. "RATIONAL DOS/4G\0")
         *   Output: ES:(E)DI = far ptr to vendor entry point (32-bit far func)
         *           CF=0 on success; CF=1, AX=0x8001 if unsupported
         *
         * NOTE: DOS/4G docs say DS:EDX (not DS:ESI as some sources state).
         *
         * The entry point dispatches subfunctions by AX:
         *   0x00: Get package function entry
         *   0x01: Sanity stub — returns EAX = 0xABCD1234
         *   0x02: Call 16-bit function
         *   0x03: Returns "mov ax, 666h / retf" code pattern
         *   0x04: Interrupt-control API (AutoPassup, v1.8+)
         *
         * s45: log the string queried and return our pre-allocated vendor
         * stub (planted at boot — see dpmi_init). The stub is a tiny PM
         * code segment that implements subfn 0x01 (sanity) and IRETs for
         * everything else, so probes pass and unknown subfns are no-ops. */
        /* Three calling conventions in the wild:
         *   - DPMI 1.0 spec:        ES:(E)DI = ASCIIZ vendor name
         *   - DOS/4G doc (rgmroman):DS:EDX  = ASCIIZ vendor name
         *   - V86MT-API.md:         DS:ESI  = ASCIIZ vendor name
         * Read all three, pick the first that holds a printable ASCII string
         * and matches a vendor we know. */
        const char *vname = NULL;
        uint16_t es_sel = regs->es & 0xFFFF;
        uint16_t ds_sel = regs->ds & 0xFFFF;
        int es_idx = SEL_TO_IDX(es_sel);
        int ds_idx = SEL_TO_IDX(ds_sel);
        const char *via_esdi  = NULL;
        const char *via_dsedx = NULL;
        const char *via_dsesi = NULL;
        if ((es_sel & 4) && es_idx < DPMI_LDT_ENTRIES && LDT_USED(c, es_idx)) {
            uint32_t es_base = desc_get_base(&c->ldt[es_idx]);
            via_esdi = (const char *)(es_base + (regs->edi & 0xFFFFFFFFu));
        }
        if ((ds_sel & 4) && ds_idx < DPMI_LDT_ENTRIES && LDT_USED(c, ds_idx)) {
            uint32_t ds_base = desc_get_base(&c->ldt[ds_idx]);
            via_dsedx = (const char *)(ds_base + (regs->edx & 0xFFFFFFFFu));
            via_dsesi = (const char *)(ds_base + (regs->esi & 0xFFFFFFFFu));
        }
        serial_puts("DPMI: INT 31h AX=0x0A00 ES:EDI=");
        serial_puthex(es_sel); serial_puts(":"); serial_puthex(regs->edi);
        serial_puts(" DS:EDX=");
        serial_puthex(ds_sel); serial_puts(":"); serial_puthex(regs->edx);
        serial_puts(" DS:ESI=");
        serial_puthex(ds_sel); serial_puts(":"); serial_puthex(regs->esi);
        serial_puts("\n  via ES:EDI=\"");
        if (via_esdi) {
            int i;
            for (i = 0; i < 32 && (uint8_t)via_esdi[i] >= 0x20 && via_esdi[i] != 0; i++)
                serial_putc(via_esdi[i]);
        }
        serial_puts("\"  via DS:EDX=\"");
        if (via_dsedx) {
            int i;
            for (i = 0; i < 32 && (uint8_t)via_dsedx[i] >= 0x20 && via_dsedx[i] != 0; i++)
                serial_putc(via_dsedx[i]);
        }
        serial_puts("\"  via DS:ESI=\"");
        if (via_dsesi) {
            int i;
            for (i = 0; i < 32 && (uint8_t)via_dsesi[i] >= 0x20 && via_dsesi[i] != 0; i++)
                serial_putc(via_dsesi[i]);
        }
        serial_puts("\"");
        /* Try each pointer for a known vendor prefix.
         * 'R' → "RATIONAL" (DOS/4G); 'V' → "V86MT v1" (Phase 4.7). */
        const char *cands[3] = { via_esdi, via_dsedx, via_dsesi };
        for (int ci = 0; ci < 3 && !vname; ci++) {
            const char *s = cands[ci];
            if (!s) continue;
            if (s[0] == 'R' || s[0] == 'V') vname = s;
        }
        /* V86MT v1 — return the entry-point stub planted at boot
         * (see dpmi_init_client; covers AX=0x0000 get_caps in v1.0). */
        if (vname && c->v86mt_api_sel &&
            vname[0] == 'V' && vname[1] == '8' && vname[2] == '6' &&
            vname[3] == 'M' && vname[4] == 'T' && vname[5] == ' ' &&
            vname[6] == 'v' && vname[7] == '1') {
            regs->es  = (regs->es  & 0xFFFF0000) | c->v86mt_api_sel;
            regs->edi = 0;
            serial_puts(" → V86MT v1 ES:EDI=");
            serial_puthex(c->v86mt_api_sel);
            serial_puts(":0\n");
            return 0;
        }
        /* "RATIONAL DOS/4G" — pretend we ARE DOS/4G. The stub
         * was planted at boot in c->vendor_api_sel (see dpmi_init_client). */
        if (vname && c->vendor_api_sel &&
            vname[0] == 'R' && vname[1] == 'A' && vname[2] == 'T' &&
            vname[3] == 'I' && vname[4] == 'O' && vname[5] == 'N' &&
            vname[6] == 'A' && vname[7] == 'L') {
            regs->es  = (regs->es  & 0xFFFF0000) | c->vendor_api_sel;
            regs->edi = 0;
            serial_puts(" → ES:EDI=");
            serial_puthex(c->vendor_api_sel);
            serial_puts(":0\n");
            return 0;
        }
        serial_puts(" → CF=1\n");
        regs->eax = (regs->eax & 0xFFFF0000) | 0x8001;
        return 1;
    }

    /* ---- V86MT v1 dispatch (Phase 4.7 M2+) ----
     *
     * Reached via the stub at linear 0x530, which translates client
     * sub-function N (1..8) into INT 31h AX=0x0A00+N. The stub does
     * `add ax, 0x0A00; int 0x31; retf` so all standard DPMI exit
     * plumbing (regs writeback, CF, ES) applies. See V86MT-API.md. */

    case 0x0A01: {
        /* v86mt_vt_alloc — BL = cols, BH = rows.
         * v1.0 baseline supports 80×25 only; M3 kmalloc's char + attr
         * shadow buffers and inits to spaces / light-gray-on-black.
         * Returned BX/CX/DX = 0 until M5 maps the buffers via LDT
         * selectors. */
        uint8_t cols = regs->ebx & 0xFF;
        uint8_t rows = (regs->ebx >> 8) & 0xFF;
        serial_puts("DPMI: V86MT vt_alloc cols=");
        serial_puthex(cols);
        serial_puts(" rows=");
        serial_puthex(rows);
        if (cols != DPMI_V86MT_COLS || rows != DPMI_V86MT_ROWS) {
            serial_puts(" → CF=1 BAD_DIMENSIONS\n");
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0002;
            return 1;
        }
        for (int h = 0; h < DPMI_V86MT_MAX_VTS; h++) {
            struct dpmi_v86mt_vt *v = &c->v86mt_vts[h];
            if (v->used) continue;
            uint32_t n = (uint32_t)cols * rows;
            uint8_t *cb = (uint8_t *)kmalloc(n);
            uint8_t *ab = (uint8_t *)kmalloc(n);
            if (!cb || !ab) {
                if (cb) kfree(cb);
                if (ab) kfree(ab);
                serial_puts(" → CF=1 OOM\n");
                regs->eax = (regs->eax & 0xFFFF0000) | 0x0030;
                return 1;
            }
            v->used           = 1;
            v->cols           = cols;
            v->rows           = rows;
            v->cursor_visible = 1;
            v->cursor_x       = 0;
            v->cursor_y       = 0;
            v->screen_dirty   = 0;
            v->kbd_drops      = 0;
            v->ticks_consumed = 0;
            v->char_buf       = cb;
            v->attr_buf       = ab;
            v->kbd_buf        = 0;
            v->kbd_sel        = 0;
            v->task_running   = 0;
            v->exited         = 0;
            v->exit_code      = 0;
            v86mt_vt_clear(v);
            /* M6 — keyboard ring. 16-byte header + 32 × 2-byte entries =
             * 80 bytes, kmalloc-allocated so it lives in the identity-mapped
             * low-RAM heap (PTE_USER), readable + writable through a DPL=3
             * data selector. Header init: head=tail=0, size=N, flags=0. */
            uint8_t *kb = (uint8_t *)kmalloc(DPMI_V86MT_KBD_BYTES);
            if (!kb) {
                kfree(cb); kfree(ab);
                v->used = 0;
                v->char_buf = 0;
                v->attr_buf = 0;
                serial_puts(" → CF=1 OOM(kbd)\n");
                regs->eax = (regs->eax & 0xFFFF0000) | 0x0030;
                return 1;
            }
            memset(kb, 0, DPMI_V86MT_KBD_BYTES);
            *(uint16_t *)(kb + 4) = DPMI_V86MT_KBD_ENTRIES;  /* size */
            v->kbd_buf = kb;
            /* M5 — allocate LDT descriptors mapping char_buf + attr_buf
             * into client-readable PM space. Identity-mapped low-RAM heap
             * pages are PTE_USER, so ring 3 reads through the selector
             * just work. Access byte 0xF0 = present, DPL=3, data, RO.
             * M6 adds the kbd selector — RW (access 0xF2) so client can
             * batch-write directly when it wants to bypass 0x0A03. */
            int char_idx = ldt_alloc(c, 1);
            int attr_idx = ldt_alloc(c, 1);
            int kbd_idx  = ldt_alloc(c, 1);
            if (!char_idx || !attr_idx || !kbd_idx) {
                if (char_idx) ldt_free(c, char_idx);
                if (attr_idx) ldt_free(c, attr_idx);
                if (kbd_idx)  ldt_free(c, kbd_idx);
                kfree(cb); kfree(ab); kfree(kb);
                v->used = 0;
                v->char_buf = 0;
                v->attr_buf = 0;
                v->kbd_buf  = 0;
                serial_puts(" → CF=1 NO_LDT\n");
                regs->eax = (regs->eax & 0xFFFF0000) | 0x0008;
                return 1;
            }
            ldt_setup(c, char_idx, (uint32_t)cb, n - 1, 0xF0, 1);
            ldt_setup(c, attr_idx, (uint32_t)ab, n - 1, 0xF0, 1);
            ldt_setup(c, kbd_idx,  (uint32_t)kb, DPMI_V86MT_KBD_BYTES - 1,
                      0xF2, 1);
            v->char_sel = LDT_SEL(char_idx);
            v->attr_sel = LDT_SEL(attr_idx);
            v->kbd_sel  = LDT_SEL(kbd_idx);
            regs->eax = (regs->eax & 0xFFFF0000) | (uint16_t)(h + 1);
            regs->ebx = (regs->ebx & 0xFFFF0000) | v->char_sel;
            regs->ecx = (regs->ecx & 0xFFFF0000) | v->attr_sel;
            regs->edx = (regs->edx & 0xFFFF0000) | v->kbd_sel;
            serial_puts(" → handle=");
            serial_puthex(h + 1);
            serial_puts(" char_buf=");
            serial_puthex((uint32_t)cb);
            serial_puts(" attr_buf=");
            serial_puthex((uint32_t)ab);
            serial_puts(" char_sel=");
            serial_puthex(v->char_sel);
            serial_puts(" attr_sel=");
            serial_puthex(v->attr_sel);
            serial_puts(" kbd_sel=");
            serial_puthex(v->kbd_sel);
            serial_puts(" CF=0\n");
            return 0;
        }
        serial_puts(" → CF=1 NO_FREE_VT\n");
        regs->eax = (regs->eax & 0xFFFF0000) | 0x0001;
        return 1;
    }

    case 0x0A02: {
        /* v86mt_vt_spawn — BX = handle, DS:ESI = argv packed (filename\0arg…\0\0),
         * ES:EDI = env packed (or NULL = inherit).
         *
         * Phase 4.7 M7 — real binary load. Parses filename + a single
         * space-joined arg string from the DS:ESI argv block, then hands
         * off to sched_create_v86_exec (same path the kernel uses to
         * launch COMMAND.COM at boot under PURE mode). v86_task_entry
         * runs in the new scheduler task's context, calls exe_load to
         * read the MZ binary from FAT, then v86_start_exe to enter V86
         * mode. We tag the V86 task with the V86MT owner immediately on
         * return so the DOS shim routes its INT 21h output into the VT
         * shadow buffer (and so v86_destroy_task mirrors lifecycle back
         * into vt_state for the PM client's vt_poll). */
        uint16_t handle = regs->ebx & 0xFFFF;
        serial_puts("DPMI: V86MT vt_spawn handle=");
        serial_puthex(handle);
        if (handle < 1 || handle > DPMI_V86MT_MAX_VTS ||
            !c->v86mt_vts[handle - 1].used) {
            serial_puts(" → CF=1 BAD_HANDLE\n");
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0010;
            return 1;
        }
        char filename[64];
        char args[128];
        filename[0] = 0;
        args[0]     = 0;
        {
            uint16_t ds_sel = regs->ds & 0xFFFF;
            int ds_idx = SEL_TO_IDX(ds_sel);
            if ((ds_sel & 4) && ds_idx < DPMI_LDT_ENTRIES && LDT_USED(c, ds_idx)) {
                uint32_t base = desc_get_base(&c->ldt[ds_idx]);
                const char *p = (const char *)(base + (regs->esi & 0xFFFFFFFFu));
                int i = 0, j = 0;
                while (i < (int)sizeof(filename) - 1 && p[i]) {
                    filename[i] = p[i];
                    i++;
                }
                filename[i] = 0;
                /* Skip first NUL, gather space-joined args until 0x00 0x00. */
                if (p[i] == 0) i++;
                while (i < 1024 && j < (int)sizeof(args) - 1) {
                    if (p[i] == 0 && p[i + 1] == 0) break;
                    if (p[i] == 0) { args[j++] = ' '; i++; continue; }
                    args[j++] = p[i++];
                }
                args[j] = 0;
            }
            serial_puts(" filename=\"");
            serial_puts(filename);
            serial_puts("\" args=\"");
            serial_puts(args);
            serial_puts("\"");
        }
        if (!filename[0]) {
            serial_puts(" → CF=1 BAD_ARGV\n");
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0013;
            return 1;
        }
        int task_id = sched_create_v86_exec("v86mt-vt", -1, filename, args);
        if (task_id < 0) {
            serial_puts(" → CF=1 EXEC_FAILED (sched alloc)\n");
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0012;
            return 1;
        }
        int v86_id = sched_get_task(task_id)->v86_task_id;
        v86_set_v86mt_owner(v86_id, client_id, handle);
        /* Arm the per-task 0xB8000 sandbox so TUI apps that write directly
         * to VGA text memory (EDIT.EXE, the entire DFlat+ family) land in
         * private RAM that v86mt_poll syncs into the client's char/attr
         * buffers — instead of stomping the kernel VGA at physical 0xB8000. */
        if (v86_arm_v86mt_sandbox(v86_id) < 0) {
            serial_puts("DPMI: V86MT sandbox arm failed (no page)\n");
        }
        c->v86mt_vts[handle - 1].task_running = 1;
        c->v86mt_vts[handle - 1].exited       = 0;
        c->v86mt_vts[handle - 1].exit_code    = 0;
        serial_puts(" → CF=0 sched_task=");
        serial_puthex(task_id);
        serial_puts(" v86=");
        serial_puthex(v86_id);
        serial_puts("\n");
        regs->eax &= 0xFFFF0000;
        return 0;
    }

    case 0x0A03: {
        /* v86mt_kbd_inject — BX = handle, CX = (scancode<<8 | ascii).
         * Convenience wrapper around the shared kbd ring: enqueues one
         * entry as if the client had written it through kbd_sel directly.
         * Ring layout matches v86mt_kbd_peek/pop (16-byte header at
         * kbd_buf, then size × 2-byte entries). Full-ring policy = drop
         * + bump kbd_drops + return V86MT_E_RING_FULL (0x0020). */
        uint16_t h = regs->ebx & 0xFFFF;
        if (h < 1 || h > DPMI_V86MT_MAX_VTS || !c->v86mt_vts[h - 1].used) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0010;
            return 1;
        }
        struct dpmi_v86mt_vt *v = &c->v86mt_vts[h - 1];
        if (!v->kbd_buf) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0010;
            return 1;
        }
        uint16_t head = *(volatile uint16_t *)(v->kbd_buf + 0);
        uint16_t tail = *(volatile uint16_t *)(v->kbd_buf + 2);
        uint16_t size = *(volatile uint16_t *)(v->kbd_buf + 4);
        uint16_t next = (uint16_t)((head + 1) % size);
        if (next == tail) {
            v->kbd_drops++;
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0020;
            return 1;
        }
        {
            /* Synthesize a non-zero scancode when the caller only had
             * ASCII (Allegro strips scancode in cmdbox_feed_char). Real
             * BIOS keyboard entries are (scancode<<8 | ascii); apps treat
             * scancode=0 as "no key" or "extended key" and ignore the
             * ASCII byte. Map ASCII → PS/2 make-code for the printable
             * range so EDIT et al. see "valid" entries. Falls back to 0x39
             * (space) for anything not in the table — any non-zero value
             * is enough for apps that just want the ASCII. */
            uint16_t entry = (uint16_t)(regs->ecx & 0xFFFF);
            uint8_t  sc    = (entry >> 8) & 0xFF;
            uint8_t  ch    = entry & 0xFF;
            if (sc == 0 && ch != 0) {
                static const uint8_t ascii_to_sc[128] = {
                    [0x08]=0x0E, [0x09]=0x0F, [0x0D]=0x1C, [0x1B]=0x01,
                    [' ']=0x39, ['!']=0x02, ['"']=0x28, ['#']=0x04,
                    ['$']=0x05, ['%']=0x06, ['&']=0x08, ['\'']=0x28,
                    ['(']=0x0A, [')']=0x0B, ['*']=0x09, ['+']=0x0D,
                    [',']=0x33, ['-']=0x0C, ['.']=0x34, ['/']=0x35,
                    ['0']=0x0B, ['1']=0x02, ['2']=0x03, ['3']=0x04,
                    ['4']=0x05, ['5']=0x06, ['6']=0x07, ['7']=0x08,
                    ['8']=0x09, ['9']=0x0A,
                    [':']=0x27, [';']=0x27, ['<']=0x33, ['=']=0x0D,
                    ['>']=0x34, ['?']=0x35, ['@']=0x03, ['[']=0x1A,
                    ['\\']=0x2B,[']']=0x1B, ['^']=0x07, ['_']=0x0C,
                    ['`']=0x29, ['{']=0x1A, ['|']=0x2B, ['}']=0x1B,
                    ['~']=0x29,
                    ['A']=0x1E, ['B']=0x30, ['C']=0x2E, ['D']=0x20,
                    ['E']=0x12, ['F']=0x21, ['G']=0x22, ['H']=0x23,
                    ['I']=0x17, ['J']=0x24, ['K']=0x25, ['L']=0x26,
                    ['M']=0x32, ['N']=0x31, ['O']=0x18, ['P']=0x19,
                    ['Q']=0x10, ['R']=0x13, ['S']=0x1F, ['T']=0x14,
                    ['U']=0x16, ['V']=0x2F, ['W']=0x11, ['X']=0x2D,
                    ['Y']=0x15, ['Z']=0x2C,
                    ['a']=0x1E, ['b']=0x30, ['c']=0x2E, ['d']=0x20,
                    ['e']=0x12, ['f']=0x21, ['g']=0x22, ['h']=0x23,
                    ['i']=0x17, ['j']=0x24, ['k']=0x25, ['l']=0x26,
                    ['m']=0x32, ['n']=0x31, ['o']=0x18, ['p']=0x19,
                    ['q']=0x10, ['r']=0x13, ['s']=0x1F, ['t']=0x14,
                    ['u']=0x16, ['v']=0x2F, ['w']=0x11, ['x']=0x2D,
                    ['y']=0x15, ['z']=0x2C,
                };
                sc = ascii_to_sc[ch & 0x7F];
                if (!sc) sc = 0x39;   /* fallback: SPACE scancode */
                entry = ((uint16_t)sc << 8) | ch;
            }
            *(uint16_t *)(v->kbd_buf + 16 + head * 2) = entry;
        }
        /* Publish the entry before advancing head (single producer). */
        *(volatile uint16_t *)(v->kbd_buf + 0) = next;
        /* Wake the v86mt-vt INT 16h blocker if it's sleeping on this VT.
         * Pairs with sched_block(BLOCK_V86MT_KBD, h) in v86.c's INT 16h
         * handler — see "Real blocking" comment there. */
        sched_unblock(BLOCK_V86MT_KBD, h);
        /* Also push the key into the BIOS Data Area keyboard buffer at
         * 0x40:0x1E so direct-poll TUI apps (FreeDOS EDIT, every DFlat+
         * application, TASM, MASM, etc.) see it. INT 16h apps use the
         * ring above; BDA-pollers use this. The BDA is shared identity-
         * mapped across all V86 tasks, so the layout matches what the
         * hw kbd ISR writes for non-v86mt VTs. */
        {
            volatile uint16_t *bda_head = (volatile uint16_t *)0x41A;
            volatile uint16_t *bda_tail = (volatile uint16_t *)0x41C;
            uint16_t bhead = *bda_head;
            uint16_t btail = *bda_tail;
            uint16_t bnext = btail + 2;
            if (bnext >= 0x3E) bnext = 0x1E;   /* wrap */
            if (bnext != bhead) {              /* drop on full */
                volatile uint16_t *slot =
                    (volatile uint16_t *)(0x400 + btail);
                /* Re-fetch the (now scancode-synthesized) entry from the
                 * v86mt ring head-1 — that's the freshest published entry. */
                uint16_t ring_idx = head;     /* index we just wrote */
                uint16_t injected =
                    *(uint16_t *)(v->kbd_buf + 16 + ring_idx * 2);
                *slot = injected;
                *bda_tail = bnext;
            }
        }
        regs->eax &= 0xFFFF0000;
        return 0;
    }

    case 0x0A04: {
        /* v86mt_poll — BX = handle, ES:EDI = ptr to 32-byte vt_state.
         * Kernel writes the current state into client memory and returns
         * CF=0. Caller polls every frame to see screen_dirty / cursor /
         * exited state. See docs/design/V86MT-API.md "vt_state struct". */
        uint16_t h = regs->ebx & 0xFFFF;
        if (h < 1 || h > DPMI_V86MT_MAX_VTS || !c->v86mt_vts[h - 1].used) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0010;
            return 1;
        }
        uint16_t es_sel = regs->es & 0xFFFF;
        int es_idx = SEL_TO_IDX(es_sel);
        if (!(es_sel & 4) || es_idx >= DPMI_LDT_ENTRIES || !LDT_USED(c, es_idx)) {
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0030;
            return 1;
        }
        uint32_t es_base = desc_get_base(&c->ldt[es_idx]);
        struct dpmi_v86mt_vt *v = &c->v86mt_vts[h - 1];
        uint8_t *out = (uint8_t *)(es_base + (regs->edi & 0xFFFFFFFFu));
        /* Sync the v86 task's VGA-text sandbox into the client-visible
         * char_buf / attr_buf BEFORE writing state. EDIT and other TUI
         * apps write directly to virtual 0xB8000; the sandbox captures
         * those writes, and v86_sync_v86mt_sandbox deinterleaves them
         * into the API-shaped char/attr buffers the client renders. */
        {
            int v86_id = v86_task_for_v86mt(client_id, h);
            if (v86_id >= 0 && v->char_buf && v->attr_buf) {
                uint32_t cells = (uint32_t)v->cols * v->rows;
                uint8_t old_dirty = v->char_buf[0] ^ v->char_buf[cells - 1];
                v86_sync_v86mt_sandbox(v86_id, v->char_buf, v->attr_buf, cells);
                /* Bump screen_dirty if the sync actually changed anything
                 * (cheap signature check). Lets DESKTOP know to re-blit. */
                uint8_t new_dirty = v->char_buf[0] ^ v->char_buf[cells - 1];
                if (new_dirty != old_dirty) v->screen_dirty++;
            }
        }
        /* Layout per V86MT-API.md (exactly 32 bytes, packed). */
        *(uint16_t *)(out + 0)  = v->task_running;
        *(uint16_t *)(out + 2)  = v->exit_code;
        *(uint16_t *)(out + 4)  = v->exited;
        *(uint16_t *)(out + 6)  = v->screen_dirty;
        *(uint16_t *)(out + 8)  = v->cursor_x;
        *(uint16_t *)(out + 10) = v->cursor_y;
        *(uint16_t *)(out + 12) = v->cursor_visible;
        *(uint16_t *)(out + 14) = v->cols;
        *(uint16_t *)(out + 16) = v->rows;
        *(uint16_t *)(out + 18) = v->kbd_drops;
        *(uint16_t *)(out + 20) = 0;
        *(uint32_t *)(out + 22) = v->ticks_consumed;
        *(uint16_t *)(out + 26) = 0;
        *(uint16_t *)(out + 28) = 0;
        *(uint16_t *)(out + 30) = 0x0100;  /* api_version v1.0 */
        regs->eax &= 0xFFFF0000;
        return 0;
    }

    case 0x0A08: {
        /* v86mt_vt_free — BX = handle (1..MAX_VTS). */
        uint16_t h = regs->ebx & 0xFFFF;
        serial_puts("DPMI: V86MT vt_free handle=");
        serial_puthex(h);
        if (h < 1 || h > DPMI_V86MT_MAX_VTS ||
            !c->v86mt_vts[h - 1].used) {
            serial_puts(" → CF=1 BAD_HANDLE\n");
            regs->eax = (regs->eax & 0xFFFF0000) | 0x0010;
            return 1;
        }
        struct dpmi_v86mt_vt *v = &c->v86mt_vts[h - 1];
        if (v->char_sel) { ldt_free(c, SEL_TO_IDX(v->char_sel)); v->char_sel = 0; }
        if (v->attr_sel) { ldt_free(c, SEL_TO_IDX(v->attr_sel)); v->attr_sel = 0; }
        if (v->kbd_sel)  { ldt_free(c, SEL_TO_IDX(v->kbd_sel));  v->kbd_sel  = 0; }
        if (v->char_buf) { kfree(v->char_buf); v->char_buf = 0; }
        if (v->attr_buf) { kfree(v->attr_buf); v->attr_buf = 0; }
        if (v->kbd_buf)  { kfree(v->kbd_buf);  v->kbd_buf  = 0; }
        v->used = 0;
        v->task_running = 0;
        v->exited = 0;
        regs->eax &= 0xFFFF0000;
        serial_puts(" → CF=0 freed\n");
        return 0;
    }

    /* ---- Misc ---- */

    case 0x0400:
        /* Get DPMI version: AX=version, BX=flags, CL=CPU, DH:DL=PIC
         *
         * s42 exploratory — Fix B: bumped AX 0x005A (DPMI 0.90)
         * → 0x0100 (DPMI 1.0). See v86.c INT 2Fh/1687h note for
         * full rationale. Must be consistent across both the 2Fh
         * detect and 31h/0400 query so clients see one host. */
        regs->eax = (regs->eax & 0xFFFF0000) | 0x0100; /* v1.00 [s42-B] */
        regs->ebx = (regs->ebx & 0xFFFF0000) | 0x0005; /* 32-bit, V86 mode */
        regs->ecx = (regs->ecx & 0xFFFF0000) | 0x0003; /* i386 */
        regs->edx = (regs->edx & 0xFFFF0000) | 0x2070; /* PIC: master=0x20, slave=0x70 */
        return 0;

    case 0x0600: /* Lock linear region — no-op for non-paging kernel */
    case 0x0601: /* Unlock linear region */
    case 0x0602: /* Mark RM region pageable */
    case 0x0603: /* Relock RM region */
        return 0;

    case 0x0604:
        /* Get page size → BX:CX = 4096 */
        regs->ebx = (regs->ebx & 0xFFFF0000) | 0;
        regs->ecx = (regs->ecx & 0xFFFF0000) | 4096;
        return 0;

    case 0x0800: {
        /* Physical address mapping: BX:CX=phys, SI:DI=size → BX:CX=linear */
        uint32_t phys = ((regs->ebx & 0xFFFF) << 16) | (regs->ecx & 0xFFFF);
        uint32_t size = ((regs->esi & 0xFFFF) << 16) | (regs->edi & 0xFFFF);
        uint32_t pages = (size + 0xFFF) / 0x1000;
        uint32_t p;
        /* Identity-map the physical region (works for VGA at 0xA0000, etc.) */
        for (p = 0; p < pages; p++)
            vmm_map_page(phys + p * 0x1000, phys + p * 0x1000, 0x07);
        /* Return same address (identity mapped) */
        return 0;
    }

    case 0x0900:
        /* Get and disable virtual interrupt state */
        regs->eax = (regs->eax & 0xFFFFFF00) | c->virtual_if;
        c->virtual_if = 0;
        return 0;

    case 0x0901:
        /* Get and enable virtual interrupt state */
        regs->eax = (regs->eax & 0xFFFFFF00) | c->virtual_if;
        c->virtual_if = 1;
        return 0;

    case 0x0902:
        /* Get virtual interrupt state */
        regs->eax = (regs->eax & 0xFFFFFF00) | c->virtual_if;
        return 0;

    case 0x0702:    /* Mark page as paging candidate (DPMI 1.0) */
    case 0x0703:    /* Discard page contents (DPMI 1.0) */
        /* No-op: we don't page. Return success so paging-aware clients
         * (DOS/32A int31h_tab lists these) get the "honoured" signal. */
        return 0;

    case 0x0801: {
        /* Free physical address mapping (DPMI 0.9 + 1.0): BX:CX=linear
         * base previously returned by 0x0800. We use identity-mapped
         * low memory plus permanent 0xA0000 VGA — nothing to undo.
         * (DOS/32A int31h_tab.) */
        return 0;
    }

    case 0x0E00: {
        /* Get coprocessor status (DPMI 1.0 + DOS/32A int31h_tab):
         *   AX = 0x4787 + bits:
         *     bit 0  MPv (math chip present V86) — 1
         *     bit 1  MPp (math chip present PM)  — 1
         *     bit 2  CPv (coprocessor emulator V86)  — 0
         *     bit 3  CPp (coprocessor emulator PM)   — 0
         * DOOM checks for FPU here. Pinecore requires a real FPU
         * (our kernel uses it), so always report present. */
        regs->eax = (regs->eax & 0xFFFF0000) | 0x4787;
        return 0;
    }

    case 0x0E01:
        /* Set coprocessor emulation — ignore */
        return 0;

    default: {
        /* Per-func hit counter so we can rank which post-init queries
         * dominate. Small fixed table, LRU-evict if full. */
        static struct { uint16_t func; uint32_t count; } unh[16];
        static int unh_n = 0;
        int i, slot = -1;
        for (i = 0; i < unh_n; i++) if (unh[i].func == func) { slot = i; break; }
        if (slot < 0 && unh_n < 16) { slot = unh_n++; unh[slot].func = func; unh[slot].count = 0; }
        if (slot >= 0) unh[slot].count++;
        serial_puts("DPMI: unhandled INT 31h func=");
        serial_puthex(func);
        serial_puts(" BX=");
        serial_puthex(regs->ebx & 0xFFFF);
        serial_puts(" CX=");
        serial_puthex(regs->ecx & 0xFFFF);
        serial_puts(" DX=");
        serial_puthex(regs->edx & 0xFFFF);
        serial_puts(" hit=");
        serial_puthex(slot >= 0 ? unh[slot].count : 0);
        serial_puts("\n");
        /* DPMI 0.9: on CF=1, AX is the error code. Returning AX
         * unmodified gives the caller garbage; set 0x8001 (unsupported
         * function) so DOS/4GW sees a coherent error. (Research doc 37
         * §8.3.) */
        regs->eax = (regs->eax & 0xFFFF0000) | 0x8001;
        return 1;  /* unsupported */
    }
    }
}

/* ================================================================
 * PM Exception Handler Redirection
 *
 * When a CPU exception (0-31) occurs in a PM DPMI client, redirect
 * to the client's registered exception handler (set via INT 31h/0203h).
 * This is critical for DOS extenders: DOS/4GW traps #GP (13) to
 * translate real-mode segment values to PM selectors.
 *
 * DPMI exception frame pushed on client stack (16-bit client):
 *   [SP+0]  return IP
 *   [SP+2]  return CS
 *   [SP+4]  error code (dword for 16-bit DPMI!)
 *   [SP+8]  exception IP (faulting instruction)
 *   [SP+10] exception CS
 *   [SP+12] exception flags
 * Then IRET jumps to the handler. Handler does RETF to return.
 * ================================================================ */

uint32_t dpmi_handle_pm_exception(uint8_t exc_num, uint32_t esp) {
    struct isr_frame *frame = (struct isr_frame *)esp;
    uint16_t cs_sel = frame->cs & 0xFFFF;
    int cs_idx = SEL_TO_IDX(cs_sel);
    int ci;
    struct dpmi_client *c = 0;

    for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
        if (clients[ci].active) {
            if (clients[ci].client_cs == cs_sel ||
                (cs_idx < DPMI_LDT_ENTRIES && LDT_USED(&clients[ci], cs_idx))) {
                c = &clients[ci];
                break;
            }
        }
    }

    if (!c) return 0;

    /* Demand-paging for #PF — the commit half of DPMI's reserve-vs-commit
     * memory model. 0501 only reserves linear address space; the physical
     * page is allocated here on first touch. Matches CWSDPMI's discipline
     * (exphdlr.c:337 `page_in_user`, called from the host's #PF dispatch).
     *
     * Why this is needed: DJGPP's go32-v2 stub reserves ~2 GB via 0501 to
     * cover text+data+bss+heap+stack, then sets ESP to the top of the
     * reservation and starts running. The first PUSH would fault on an
     * uncommitted page — which is exactly the signal the stub expects the
     * host to act on.
     *
     * Policy: P=0 fault AND CR2 inside the DPMI client zone → allocate +
     * map + zero (DPMI spec guarantees zeroed allocations) + retry the
     * instruction. Anything else falls through to the client's exception
     * handler (or the "no handler" kill path). Capped at DPMI_COMMIT_CAP
     * to leave the kernel headroom; over-cap → CF-set-equivalent via no
     * resume → client sees its own #PF handler. */
    if (exc_num == 0x0E) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if ((frame->err_code & 1) == 0 &&
            cr2 >= DPMI_VADDR_START &&
            cr2 <  DPMI_VADDR_END) {
            if (dpmi_commit_page(cr2)) {
                static int auto_map_count = 0;
                if (auto_map_count < 20) {
                    serial_puts("DPMI: auto-map CR2=");
                    serial_puthex(cr2);
                    serial_puts(" (PF at CS:EIP=");
                    serial_puthex(frame->cs);
                    serial_puts(":");
                    serial_puthex(frame->eip);
                    serial_puts(")\n");
                    auto_map_count++;
                }
                return esp;  /* retry faulting instruction */
            }
            /* commit failed (cap or OOM) — fall through to client handler */
        }
    }

    /* For #GP (13): if the faulting instruction is a host-only privileged
     * insn the client has no business issuing (LLDT/LGDT/LIDT/LTR/SLDT/SIDT
     * /SGDT/STR/CLTS/HLT and friends), skip it as a no-op rather than
     * delivering to the client's #GP handler.
     *
     * Why: DOS/4GW's binary contains code that assumes 4GW itself is the
     * DPMI host. In particular its PM #GP handler at 0x6A9B does
     * `MOV BX,0x68 ; LLDT BX` (file offset 0xB849) — it tries to load
     * its own LDT. As a ring-3 PM client that #GPs immediately. If we
     * deliver that #GP to 4GW's own handler, the LLDT in the handler
     * #GPs again → infinite recursion → kernel eats the recursion until
     * the PM stack overflows.
     *
     * Skipping these as no-ops lets 4GW's host-mode code path "run"
     * without effect; the host-state operations would have been our
     * responsibility anyway.  */
    if (exc_num == 13) {
        uint32_t cs_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->cs & 0xFFFF)]);
        uint8_t *code = (uint8_t *)(cs_base + (frame->eip & 0xFFFF));
        int p = 0;
        /* skip operand-size / address-size / segment override prefixes */
        while (code[p] == 0x66 || code[p] == 0x67 ||
               code[p] == 0x26 || code[p] == 0x2E || code[p] == 0x36 ||
               code[p] == 0x3E || code[p] == 0x64 || code[p] == 0x65) p++;

        if (code[p] == 0x0F && (code[p+1] == 0x00 || code[p+1] == 0x01)) {
            /* 0F 00 /r: SLDT/STR/LLDT/LTR/VERR/VERW
             * 0F 01 /r: SGDT/SIDT/LGDT/LIDT/SMSW/LMSW (mod==3: extended)
             * Skip the whole instruction. */
            uint8_t modrm = code[p+2];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm  = modrm & 7;
            int len = 3;  /* prefix + 0F xx + modrm */
            if (mod != 3) {
                /* memory operand — variable length encoding */
                if (rm == 4) len++;                         /* SIB */
                if (mod == 0 && rm == 5) len += 4;          /* 32-bit disp32 */
                else if (mod == 0 && rm == 6) len += 2;     /* 16-bit disp16 */
                else if (mod == 1) len += 1;                /* disp8 */
                else if (mod == 2) len += 4;                /* disp32 (or 16-bit) */
            }
            static int pm_priv_count = 0;
            if (pm_priv_count < 16) {
                serial_puts("DPMI: skip priv PM insn 0F ");
                serial_puthex(code[p+1]);
                serial_puts(" reg=");
                serial_puthex((modrm >> 3) & 7);
                serial_puts(" at CS:EIP=");
                serial_puthex(frame->cs);
                serial_puts(":");
                serial_puthex(frame->eip);
                serial_puts(" len=");
                serial_puthex(p + len);
                serial_puts("\n");
                pm_priv_count++;
            }
            frame->eip += p + len;
            return esp;
        }

        /* Other privileged instructions */
        if (code[p] == 0xFA || code[p] == 0xFB) {  /* CLI / STI */
            frame->eip += p + 1;
            return esp;
        }
        if (code[p] == 0xF4) {  /* HLT */
            frame->eip += p + 1;
            return esp;
        }
    }

    /* Exception 5 (#BR — BOUND range exceeded): skip the BOUND instruction.
     * DOS/4GW uses BOUND for internal array checks. The BOUND instruction
     * is 2 bytes (opcode + modrm) + addressing mode bytes. */
    if (exc_num == 5) {
        uint32_t cs_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->cs & 0xFFFF)]);
        uint8_t *code = (uint8_t *)(cs_base + (frame->eip & 0xFFFF));
        /* BOUND is 62 /r — skip opcode + modrm + addressing mode */
        if (code[0] == 0x62) {
            uint8_t modrm = code[1];
            uint8_t mod = modrm >> 6;
            uint8_t rm = modrm & 7;
            int len = 2;
            if (mod == 0 && rm == 6) len += 2;  /* [disp16] */
            else if (mod == 1) len += 1;         /* [reg+disp8] */
            else if (mod == 2) len += 2;         /* [reg+disp16] */
            frame->eip += len;
            return esp;
        }
    }

    /* For #GP (exception 13): auto-fix segment loads.
     *
     * Strategy 1: If the error code references an LDT selector whose index
     * is within our LDT but the entry is empty, create a placeholder.
     * This handles DOS/4GW's internally-computed selectors.
     *
     * Strategy 2: If the error code is a GDT selector (real-mode segment),
     * decode the instruction and patch the operand with a new LDT selector. */
    if (exc_num == 13 && frame->err_code != 0 && !(frame->err_code & 2)) {
        uint16_t bad_val = frame->err_code & 0xFFFF;

        /* Strategy 1: LDT selector reference */
        if (bad_val & 4) {  /* TI bit → LDT reference */
            int target_idx = bad_val >> 3;
            /* Auto-create: only for indices >= DPMI_LDT_FIRST.
             *
             * CRITICAL (docs/research/37-dos4gw-internals.md): DOS/4GW probes
             * LDT[0..15] via `LAR` expecting them to be NOT PRESENT — that's
             * the CWSDPMI signature it's looking for. If we auto-fill the
             * low slots, 4GW sees a non-CWSDPMI host and falls into a path
             * that references selectors we never allocate → the auto-LDT
             * cascade smashes its descriptor view. Leave slots 0..15 alone
             * and let the #GP propagate. The slot at LDT[5] is preallocated
             * once at PM-entry as the CWSDPMI low-mem selector. */
            if (target_idx >= DPMI_LDT_FIRST && target_idx < DPMI_LDT_ENTRIES) {
                if (LDT_FREE(c, target_idx) && target_idx < 256) {
                    /* Empty entry — create flat data segment (read/write).
                     * Must be DATA (0xF2), not code (0xFA), because these
                     * selectors are typically loaded into DS/ES by MOV sreg.
                     * Loading a code segment into DS causes #GP.
                     * DOS/4GW will overwrite with correct values later. */
                    ldt_setup(c, target_idx, 0, 0xFFFF, 0xF2, 0);
                    static int ldt_fix_count = 0;
                    if (ldt_fix_count < 20) {
                        serial_puts("DPMI: auto-LDT[");
                        serial_puthex(target_idx);
                        serial_puts("] for sel ");
                        serial_puthex(bad_val);
                        serial_puts(" EIP=");
                        serial_puthex(frame->eip);
                        serial_puts("\n");
                        ldt_fix_count++;
                    }
                    return esp;  /* restart instruction */
                }
            }
            /* RPL fix: for ANY valid LDT entry (including low indices
             * written by INT 31h/000C for DOS/4GW internal selectors) */
            if (target_idx < DPMI_LDT_ENTRIES &&
                LDT_USED(c, target_idx) && (bad_val & 3) != 3) {
                /* Entry exists but selector has wrong RPL (< 3).
                 * Strategy: try targeted instruction decode FIRST so we patch
                 * only the specific operand the failing instruction uses.
                 * Broad register/stack scan runs only as a last-resort fallback
                 * — it can corrupt unrelated registers (e.g. EBX holding a
                 * file handle) when low-16 bits coincide with bad_val. */
                uint16_t good = LDT_SEL(target_idx);
                uint32_t ss_base2 = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ss & 0xFFFF)]);
                /* Targeted instruction decode */
                {
                    uint32_t cs_base2 = desc_get_base(&c->ldt[SEL_TO_IDX(frame->cs & 0xFFFF)]);
                    uint8_t *code2 = (uint8_t *)(cs_base2 + frame->eip);
                    int pos2 = 0;
                    while (code2[pos2] == 0x66 || code2[pos2] == 0x67 ||
                           code2[pos2] == 0x26 || code2[pos2] == 0x2E ||
                           code2[pos2] == 0x36 || code2[pos2] == 0x3E ||
                           code2[pos2] == 0x64 || code2[pos2] == 0x65)
                        pos2++;
                    if (code2[pos2] == 0x8E) {
                        uint8_t modrm = code2[pos2 + 1];
                        uint8_t mod = modrm >> 6;
                        uint8_t rm = modrm & 7;
                        if (mod == 3) {
                            /* MOV sreg, reg — patch source register */
                            uint32_t *reg_tbl[] = {
                                &frame->eax, &frame->ecx, &frame->edx, &frame->ebx,
                                (uint32_t *)&frame->esp, &frame->ebp, &frame->esi, &frame->edi
                            };
                            *reg_tbl[rm] = (*reg_tbl[rm] & 0xFFFF0000) | good;
                            return esp;
                        } else {
                            /* MOV sreg, [mem] — decode and patch memory */
                            uint32_t ds_base2 = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ds & 0xFFFF)]);
                            uint16_t ea = 0;
                            switch (rm) {
                                case 0: ea = (frame->ebx + frame->esi) & 0xFFFF; break;
                                case 1: ea = (frame->ebx + frame->edi) & 0xFFFF; break;
                                case 2: ea = (frame->ebp + frame->esi) & 0xFFFF; break;
                                case 3: ea = (frame->ebp + frame->edi) & 0xFFFF; break;
                                case 4: ea = frame->esi & 0xFFFF; break;
                                case 5: ea = frame->edi & 0xFFFF; break;
                                case 6: if (mod == 0) ea = *(uint16_t *)&code2[pos2+2];
                                        else ea = frame->ebp & 0xFFFF; break;
                                case 7: ea = frame->ebx & 0xFFFF; break;
                            }
                            if (mod == 1) ea += (int8_t)code2[pos2 + 2];
                            else if (mod == 2) ea += *(uint16_t *)&code2[pos2 + 2];
                            uint32_t seg_base = (rm == 2 || rm == 3 || (rm == 6 && mod != 0))
                                                ? ss_base2 : ds_base2;
                            uint16_t *mem = (uint16_t *)(seg_base + ea);
                            if ((*mem & 0xFFFF) == bad_val || ((*mem & 0xFFF8) == (bad_val & 0xFFF8))) {
                                *mem = good;
                                return esp;
                            }
                        }
                    }
                    /* POP sreg — patch stack */
                    if (code2[pos2] == 0x07 || code2[pos2] == 0x1F || code2[pos2] == 0x17) {
                        uint16_t *stk2 = (uint16_t *)(ss_base2 + (frame->esp & 0xFFFF));
                        *stk2 = good;
                        return esp;
                    }
                    /* LES (C4), LDS (C5) — seg word at mem+2 (or +4 with 32-bit operand) */
                    if (code2[pos2] == 0xC4 || code2[pos2] == 0xC5) {
                        uint8_t modrm = code2[pos2 + 1];
                        uint8_t mod = modrm >> 6;
                        uint8_t rm = modrm & 7;
                        int cs_db = c->ldt[SEL_TO_IDX(frame->cs & 0xFFFF)].limit_hi & 0x40;
                        int has_66_2 = 0;
                        { int p; for (p = 0; p < pos2; p++) if (code2[p] == 0x66) has_66_2 = 1; }
                        int op32_2 = cs_db ? !has_66_2 : has_66_2;
                        uint32_t ds_base2 = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ds & 0xFFFF)]);
                        uint16_t ea = 0;
                        switch (rm) {
                            case 0: ea = (frame->ebx + frame->esi) & 0xFFFF; break;
                            case 1: ea = (frame->ebx + frame->edi) & 0xFFFF; break;
                            case 2: ea = (frame->ebp + frame->esi) & 0xFFFF; break;
                            case 3: ea = (frame->ebp + frame->edi) & 0xFFFF; break;
                            case 4: ea = frame->esi & 0xFFFF; break;
                            case 5: ea = frame->edi & 0xFFFF; break;
                            case 6: if (mod == 0) ea = *(uint16_t *)&code2[pos2+2];
                                    else ea = frame->ebp & 0xFFFF; break;
                            case 7: ea = frame->ebx & 0xFFFF; break;
                        }
                        if (mod == 1) ea += (int8_t)code2[pos2 + 2];
                        else if (mod == 2) ea += *(uint16_t *)&code2[pos2 + 2];
                        uint32_t seg_base = (rm == 2 || rm == 3 || (rm == 6 && mod != 0))
                                            ? ss_base2 : ds_base2;
                        int off = op32_2 ? 4 : 2;
                        uint16_t *seg_word = (uint16_t *)(seg_base + ((ea + off) & 0xFFFF));
                        if ((*seg_word & 0xFFF8) == (bad_val & 0xFFF8)) {
                            *seg_word = good;
                            return esp;
                        }
                    }
                }
                /* Targeted decode found nothing. Fall back to broad scan,
                 * but only match selectors with the same index as bad_val
                 * (low 16 bits exactly equal). This still has false positives
                 * but is much rarer than a generic value match. */
                if ((frame->eax & 0xFFFF) == bad_val) { frame->eax = (frame->eax & 0xFFFF0000) | good; return esp; }
                if ((frame->ecx & 0xFFFF) == bad_val) { frame->ecx = (frame->ecx & 0xFFFF0000) | good; return esp; }
                if ((frame->edx & 0xFFFF) == bad_val) { frame->edx = (frame->edx & 0xFFFF0000) | good; return esp; }
                if ((frame->ebx & 0xFFFF) == bad_val) { frame->ebx = (frame->ebx & 0xFFFF0000) | good; return esp; }
                if ((frame->ebp & 0xFFFF) == bad_val) { frame->ebp = (frame->ebp & 0xFFFF0000) | good; return esp; }
                if ((frame->esi & 0xFFFF) == bad_val) { frame->esi = (frame->esi & 0xFFFF0000) | good; return esp; }
                if ((frame->edi & 0xFFFF) == bad_val) { frame->edi = (frame->edi & 0xFFFF0000) | good; return esp; }
                {
                    uint16_t *stk = (uint16_t *)(ss_base2 + frame->esp);
                    int i;
                    for (i = 0; i < 16; i++) {
                        if (stk[i] == bad_val) { stk[i] = good; return esp; }
                    }
                }
                goto redirect_to_handler;
            }
            /* Out-of-range LDT reference */
            if (target_idx >= 256 || target_idx >= DPMI_LDT_ENTRIES)
                goto redirect_to_handler;
        }

        /* GDT references (TI=0): the client is trying to load a GDT selector
         * from Ring 3. If there's a matching LDT entry at the same index,
         * patch the instruction to use the LDT version (with TI=1, RPL=3).
         * This handles DOS/4GW computing index*8 without TI+RPL bits. */
        if (!(bad_val & 4)) {
            int gdt_idx = bad_val >> 3;
            if (gdt_idx > 0 && gdt_idx < DPMI_LDT_ENTRIES && LDT_USED(c, gdt_idx)) {
                /* The LDT has an entry at this index — replace the GDT
                 * selector with the LDT equivalent in the instruction operand */
                uint32_t cs_base2 = desc_get_base(&c->ldt[SEL_TO_IDX(frame->cs & 0xFFFF)]);
                uint32_t ss_base2 = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ss & 0xFFFF)]);
                uint8_t *code2 = (uint8_t *)(cs_base2 + frame->eip);
                uint16_t good = LDT_SEL(gdt_idx);
                int pos2 = 0;
                while (code2[pos2] == 0x66 || code2[pos2] == 0x67 ||
                       code2[pos2] == 0x26 || code2[pos2] == 0x2E ||
                       code2[pos2] == 0x36 || code2[pos2] == 0x3E ||
                       code2[pos2] == 0x64 || code2[pos2] == 0x65)
                    pos2++;
                /* MOV sreg, reg (8E /r with mod=3): patch source register */
                if (code2[pos2] == 0x8E) {
                    uint8_t modrm = code2[pos2 + 1];
                    uint8_t mod = modrm >> 6;
                    uint8_t rm = modrm & 7;
                    if (mod == 3) {
                        uint32_t *reg_tbl[] = {
                            &frame->eax, &frame->ecx, &frame->edx, &frame->ebx,
                            (uint32_t *)&frame->esp, &frame->ebp, &frame->esi, &frame->edi
                        };
                        *reg_tbl[rm] = (*reg_tbl[rm] & 0xFFFF0000) | good;
                        return esp;
                    } else {
                        /* MOV sreg, [mem]: patch memory */
                        uint32_t ds_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ds & 0xFFFF)]);
                        uint16_t ea = 0;
                        switch (rm) {
                            case 0: ea = (frame->ebx + frame->esi) & 0xFFFF; break;
                            case 1: ea = (frame->ebx + frame->edi) & 0xFFFF; break;
                            case 2: ea = (frame->ebp + frame->esi) & 0xFFFF; break;
                            case 3: ea = (frame->ebp + frame->edi) & 0xFFFF; break;
                            case 4: ea = frame->esi & 0xFFFF; break;
                            case 5: ea = frame->edi & 0xFFFF; break;
                            case 6: if (mod == 0) ea = *(uint16_t *)&code2[pos2+2];
                                    else ea = frame->ebp & 0xFFFF; break;
                            case 7: ea = frame->ebx & 0xFFFF; break;
                        }
                        if (mod == 1) ea += (int8_t)code2[pos2 + 2];
                        else if (mod == 2) ea += *(uint16_t *)&code2[pos2 + 2];
                        uint32_t seg_base = (rm == 2 || rm == 3 || (rm == 6 && mod != 0))
                                            ? ss_base2 : ds_base;
                        uint16_t *mem = (uint16_t *)(seg_base + ea);
                        *mem = good;
                        return esp;
                    }
                }
                /* POP sreg */
                if (code2[pos2] == 0x07 || code2[pos2] == 0x1F || code2[pos2] == 0x17) {
                    uint16_t *stk = (uint16_t *)(ss_base2 + (frame->esp & 0xFFFF));
                    *stk = good;
                    return esp;
                }
                /* LSS/LFS/LGS */
                if (code2[pos2] == 0x0F &&
                    (code2[pos2+1] == 0xB2 || code2[pos2+1] == 0xB4 || code2[pos2+1] == 0xB5)) {
                    /* For now just redirect */
                }
                goto redirect_to_handler;
            }
            goto redirect_to_handler;
        }

        uint32_t cs_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->cs & 0xFFFF)]);
        uint32_t ss_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ss & 0xFFFF)]);
        uint8_t *code = (uint8_t *)(cs_base + (frame->eip & 0xFFFF));

        /* Find or create LDT selector for this real-mode segment.
         * Check if we already have a descriptor with this base. */
        uint32_t want_base = (uint32_t)bad_val << 4;
        int idx = 0;
        uint16_t good_sel = 0;
        {
            int i;
            for (i = DPMI_LDT_FIRST; i < DPMI_LDT_ENTRIES; i++) {
                if (LDT_USED(c, i) && desc_get_base(&c->ldt[i]) == want_base) {
                    idx = i;
                    good_sel = LDT_SEL(i);
                    break;
                }
            }
            if (!good_sel) {
                idx = ldt_alloc(c, 1);
                if (idx) {
                    good_sel = LDT_SEL(idx);
                    ldt_setup(c, idx, want_base, 0xFFFF, 0xF2, 0);
                }
            }
        }
        if (good_sel) {
            uint32_t base = want_base;

            /* Determine default operand size from CS D/B bit.
             * D/B=1 → 32-bit default; 66h prefix toggles to 16-bit.
             * D/B=0 → 16-bit default; 66h prefix toggles to 32-bit. */
            int cs_db = c->ldt[SEL_TO_IDX(frame->cs & 0xFFFF)].limit_hi & 0x40;
            int pos = 0;
            int has_66 = 0;
            while (code[pos] == 0x66 || code[pos] == 0x67 ||
                   code[pos] == 0x26 || code[pos] == 0x2E ||
                   code[pos] == 0x36 || code[pos] == 0x3E ||
                   code[pos] == 0x64 || code[pos] == 0x65) {
                if (code[pos] == 0x66) has_66 = 1;
                pos++;
            }
            /* op32: true if operating in 32-bit mode for this instruction */
            int op32 = cs_db ? !has_66 : has_66;
            int fixed = 0;

            /* POP ES (07), POP DS (1F), POP SS (17) — patch stack value */
            if (code[pos] == 0x07 || code[pos] == 0x1F || code[pos] == 0x17) {
                uint16_t *stk_val = (uint16_t *)(ss_base + (frame->esp & 0xFFFF));
                *stk_val = good_sel;
                fixed = 1;
            }
            /* POP FS (0F A1), POP GS (0F A9) */
            else if (code[pos] == 0x0F && (code[pos+1] == 0xA1 || code[pos+1] == 0xA9)) {
                uint16_t *stk_val = (uint16_t *)(ss_base + (frame->esp & 0xFFFF));
                *stk_val = good_sel;
                fixed = 1;
            }
            /* MOV sreg, r/m16 (8E) */
            else if (code[pos] == 0x8E) {
                uint8_t modrm = code[pos + 1];
                uint8_t mod = modrm >> 6;
                uint8_t rm = modrm & 7;
                if (mod == 3) {
                    /* MOV sreg, reg — patch the source register */
                    uint32_t *reg_tbl[] = {
                        &frame->eax, &frame->ecx, &frame->edx, &frame->ebx,
                        (uint32_t *)&frame->esp, &frame->ebp, &frame->esi, &frame->edi
                    };
                    *reg_tbl[rm] = (*reg_tbl[rm] & 0xFFFF0000) | good_sel;
                    fixed = 1;
                } else {
                    /* MOV sreg, [mem] — decode memory operand, patch the word */
                    /* Compute effective address using 16-bit addressing */
                    uint32_t ds_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ds & 0xFFFF)]);
                    uint16_t ea = 0;
                    int inst_len = 2;  /* opcode + modrm */
                    switch (rm) {
                        case 0: ea = (frame->ebx + frame->esi) & 0xFFFF; break;
                        case 1: ea = (frame->ebx + frame->edi) & 0xFFFF; break;
                        case 2: ea = (frame->ebp + frame->esi) & 0xFFFF; break;
                        case 3: ea = (frame->ebp + frame->edi) & 0xFFFF; break;
                        case 4: ea = frame->esi & 0xFFFF; break;
                        case 5: ea = frame->edi & 0xFFFF; break;
                        case 6: if (mod == 0) { ea = *(uint16_t *)&code[pos+2]; inst_len += 2; }
                                else ea = frame->ebp & 0xFFFF; break;
                        case 7: ea = frame->ebx & 0xFFFF; break;
                    }
                    if (mod == 1) { ea += (int8_t)code[pos + 2]; inst_len++; }
                    else if (mod == 2) { ea += *(uint16_t *)&code[pos + 2]; inst_len += 2; }

                    /* Use SS base for BP-relative, DS base otherwise */
                    uint32_t seg_base = (rm == 2 || rm == 3 || (rm == 6 && mod != 0))
                                        ? ss_base : ds_base;
                    uint16_t *mem = (uint16_t *)(seg_base + ea);
                    if (*mem == bad_val) {
                        *mem = good_sel;
                        fixed = 1;
                    }
                }
            }
            /* LES (C4), LDS (C5) — patch segment word in memory */
            else if (code[pos] == 0xC4 || code[pos] == 0xC5) {
                /* These load reg:sreg from memory (offset, then segment).
                 * Decode modrm to find memory operand, then patch segment word.
                 * Segment word is at mem+2 (or mem+4 with 66h prefix). */
                uint8_t modrm = code[pos + 1];
                uint8_t mod = modrm >> 6;
                uint8_t rm = modrm & 7;
                uint32_t ds_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ds & 0xFFFF)]);
                uint16_t ea = 0;
                switch (rm) {
                    case 0: ea = (frame->ebx + frame->esi) & 0xFFFF; break;
                    case 1: ea = (frame->ebx + frame->edi) & 0xFFFF; break;
                    case 2: ea = (frame->ebp + frame->esi) & 0xFFFF; break;
                    case 3: ea = (frame->ebp + frame->edi) & 0xFFFF; break;
                    case 4: ea = frame->esi & 0xFFFF; break;
                    case 5: ea = frame->edi & 0xFFFF; break;
                    case 6: if (mod == 0) ea = *(uint16_t *)&code[pos+2];
                            else ea = frame->ebp & 0xFFFF; break;
                    case 7: ea = frame->ebx & 0xFFFF; break;
                }
                if (mod == 1) ea += (int8_t)code[pos + 2];
                else if (mod == 2) ea += *(uint16_t *)&code[pos + 2];
                uint32_t seg_base = (rm == 2 || rm == 3 || (rm == 6 && mod != 0))
                                    ? ss_base : ds_base;
                int off = op32 ? 4 : 2;
                uint16_t *seg_word = (uint16_t *)(seg_base + ((ea + off) & 0xFFFF));
                if (*seg_word == bad_val) {
                    *seg_word = good_sel;
                    fixed = 1;
                }
            }
            /* LSS (0F B2), LFS (0F B4), LGS (0F B5) — segment at mem+2/+4 */
            else if (code[pos] == 0x0F &&
                     (code[pos+1] == 0xB2 || code[pos+1] == 0xB4 || code[pos+1] == 0xB5)) {
                uint8_t modrm = code[pos + 2];
                uint8_t mod = modrm >> 6;
                uint8_t rm = modrm & 7;
                uint32_t ds_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ds & 0xFFFF)]);
                uint16_t ea = 0;
                switch (rm) {
                    case 0: ea = (frame->ebx + frame->esi) & 0xFFFF; break;
                    case 1: ea = (frame->ebx + frame->edi) & 0xFFFF; break;
                    case 2: ea = (frame->ebp + frame->esi) & 0xFFFF; break;
                    case 3: ea = (frame->ebp + frame->edi) & 0xFFFF; break;
                    case 4: ea = frame->esi & 0xFFFF; break;
                    case 5: ea = frame->edi & 0xFFFF; break;
                    case 6: if (mod == 0) ea = *(uint16_t *)&code[pos+3];
                            else ea = frame->ebp & 0xFFFF; break;
                    case 7: ea = frame->ebx & 0xFFFF; break;
                }
                if (mod == 1) ea += (int8_t)code[pos + 3];
                else if (mod == 2) ea += *(uint16_t *)&code[pos + 3];
                uint32_t seg_base = (rm == 2 || rm == 3 || (rm == 6 && mod != 0))
                                    ? ss_base : ds_base;
                int seg_off = op32 ? 4 : 2;
                uint16_t *seg_word = (uint16_t *)(seg_base + ((ea + seg_off) & 0xFFFF));
                if (*seg_word == bad_val) {
                    /* SS needs data r/w, FS/GS too */
                    *seg_word = good_sel;
                    fixed = 1;
                }
            }
            /* IRET (CF) — CS on stack is bad, stack: IP, CS, FLAGS */
            else if (code[pos] == 0xCF) {
                /* Stack layout for IRET: [SP+0]=IP, [SP+2]=CS, [SP+4]=FLAGS */
                int off = op32 ? 4 : 2;
                uint16_t *cs_on_stack = (uint16_t *)(ss_base +
                    ((frame->esp + off) & 0xFFFF));
                if ((*cs_on_stack & 0xFFF8) == (bad_val & 0xFFF8)) {
                    /* Code selector — exec/read access */
                    ldt_setup(c, idx, want_base, 0xFFFF, 0xFA, 0);
                    *cs_on_stack = good_sel;
                    fixed = 1;
                }
            }
            /* RETF (CB) or RETF imm16 (CA xx xx) — CS on stack is bad */
            else if (code[pos] == 0xCA || code[pos] == 0xCB) {
                /* Stack layout for RETF: [SP+0]=IP, [SP+2]=CS (16-bit)
                 * or [SP+0]=EIP, [SP+4]=CS (32-bit) */
                int off = op32 ? 4 : 2;
                uint16_t *cs_on_stack = (uint16_t *)(ss_base +
                    ((frame->esp + off) & 0xFFFF));
                uint16_t stk_val = *cs_on_stack;
                /* Match: error code (with EXT/IDT bits) vs stack value
                 * (may have wrong RPL). Compare with lower 3 bits masked. */
                if ((stk_val & 0xFFF8) == (bad_val & 0xFFF8)) {
                    /* If it's an existing LDT entry, just force RPL=3 */
                    int cs_idx2 = stk_val >> 3;
                    if ((stk_val & 4) && cs_idx2 < DPMI_LDT_ENTRIES && LDT_USED(c, cs_idx2)) {
                        *cs_on_stack = (stk_val & 0xFFFC) | 3;  /* force RPL=3 */
                    } else {
                        /* Real-mode segment — create code selector */
                        ldt_setup(c, idx, want_base, 0xFFFF, 0xFA, 0);
                        *cs_on_stack = good_sel;
                    }
                    fixed = 1;
                }
            }
            /* JMP FAR / CALL FAR via memory (FF /5, FF /3) */
            else if (code[pos] == 0xFF) {
                uint8_t modrm = code[pos + 1];
                uint8_t reg = (modrm >> 3) & 7;
                if (reg == 3 || reg == 5) {
                    /* CALL FAR or JMP FAR — CS from memory operand.
                     * Decode memory address and patch segment word. */
                    /* Similar to LES/LDS decode */
                    uint8_t mod = modrm >> 6;
                    uint8_t rm = modrm & 7;
                    uint32_t ds_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ds & 0xFFFF)]);
                    uint16_t ea = 0;
                    switch (rm) {
                        case 0: ea = (frame->ebx + frame->esi) & 0xFFFF; break;
                        case 1: ea = (frame->ebx + frame->edi) & 0xFFFF; break;
                        case 2: ea = (frame->ebp + frame->esi) & 0xFFFF; break;
                        case 3: ea = (frame->ebp + frame->edi) & 0xFFFF; break;
                        case 4: ea = frame->esi & 0xFFFF; break;
                        case 5: ea = frame->edi & 0xFFFF; break;
                        case 6: if (mod == 0) ea = *(uint16_t *)&code[pos+2];
                                else ea = frame->ebp & 0xFFFF; break;
                        case 7: ea = frame->ebx & 0xFFFF; break;
                    }
                    if (mod == 1) ea += (int8_t)code[pos + 2];
                    else if (mod == 2) ea += *(uint16_t *)&code[pos + 2];
                    uint32_t seg_base2 = (rm == 2 || rm == 3 || (rm == 6 && mod != 0))
                                        ? ss_base : ds_base;
                    int seg_off = op32 ? 4 : 2;
                    uint16_t *seg_word = (uint16_t *)(seg_base2 + ((ea + seg_off) & 0xFFFF));
                    if (*seg_word == bad_val) {
                        /* Code segment — set exec/read access */
                        ldt_setup(c, idx, want_base, 0xFFFF, 0xFA, 0);
                        *seg_word = good_sel;
                        fixed = 1;
                    }
                }
            }

            if (fixed) {
                static int fix_count = 0;
                static uint16_t last_bad[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
                if (bad_val != last_bad[0] && bad_val != last_bad[1] &&
                    bad_val != last_bad[2] && bad_val != last_bad[3] && fix_count < 40) {
                    serial_puts("DPMI: seg fix ");
                    serial_puthex(bad_val);
                    serial_puts("→");
                    serial_puthex(good_sel);
                    serial_puts(" base=");
                    serial_puthex(base);
                    serial_puts("\n");
                    fix_count++;
                }
                last_bad[3] = last_bad[2];
                last_bad[2] = last_bad[1];
                last_bad[1] = last_bad[0];
                last_bad[0] = bad_val;
                return esp;  /* restart instruction */
            }

            /* Couldn't patch — keep selector for future use */
            static int unpatched_count = 0;
            if (unpatched_count < 10) {
                serial_puts("DPMI: #GP unpatched op=");
                serial_puthex(code[pos]);
                serial_puts(" err=");
                serial_puthex(bad_val);
                serial_puts(" EIP=");
                serial_puthex(frame->eip);
                serial_puts("\n");
                unpatched_count++;
            }
        }
    }

redirect_to_handler:
    /* Redirect to client's exception handler if registered.
     * DPMI exception frame depends on handler's D/B bit:
     * 32-bit: push dword err_code, dword EIP, dword CS, dword EFLAGS (16 bytes)
     * 16-bit: push word err_code, word IP, word CS, word FLAGS (8 bytes)
     *
     * Handler returns via RETF (pops IP/CS, then adjusts SP to skip err+flags).
     * For 32-bit handlers, the DPMI spec says all fields are dwords. */
    {
        static int redir_count = 0;
        if (redir_count < 50) {
            serial_puts("DPMI: exc ");
            serial_puthex(exc_num);
            serial_puts(" err=");
            serial_puthex(frame->err_code);
            serial_puts(" CS=");
            serial_puthex(frame->cs);
            serial_puts(" EIP=");
            serial_puthex(frame->eip);
            if (exc_num == 0x0E) {
                uint32_t cr2;
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                serial_puts(" CR2=");
                serial_puthex(cr2);
                /* Also dump the CS descriptor */
                int ci2 = SEL_TO_IDX(frame->cs & 0xFFFF);
                if ((frame->cs & 4) && ci2 < DPMI_LDT_ENTRIES) {
                    uint32_t csbase = desc_get_base(&c->ldt[ci2]);
                    uint32_t cslim = c->ldt[ci2].limit_lo
                        | ((uint32_t)(c->ldt[ci2].limit_hi & 0x0F) << 16);
                    if (c->ldt[ci2].limit_hi & 0x80) cslim = (cslim << 12) | 0xFFF;
                    serial_puts(" CS.base=");
                    serial_puthex(csbase);
                    serial_puts(" CS.lim=");
                    serial_puthex(cslim);
                }
            }
            serial_puts(" → handler\n");
            /* First-hit byte dump for #GP and #UD so we can decode the
             * faulting instruction. Track up to 4 distinct CS:EIP sites. */
            if (exc_num == 0x0D || exc_num == 0x06) {
                static uint32_t dumped_eip[4] = {0,0,0,0};
                static uint16_t dumped_cs[4]  = {0,0,0,0};
                static int dumped_n = 0;
                int i2, already = 0;
                for (i2 = 0; i2 < dumped_n; i2++)
                    if (dumped_eip[i2] == frame->eip &&
                        dumped_cs[i2]  == (frame->cs & 0xFFFF)) { already = 1; break; }
                if (!already && dumped_n < 4) {
                    int ci3 = SEL_TO_IDX(frame->cs & 0xFFFF);
                    if ((frame->cs & 4) && ci3 < DPMI_LDT_ENTRIES &&
                        LDT_USED(c, ci3)) {
                        uint32_t base = desc_get_base(&c->ldt[ci3]);
                        uint8_t *code2 = (uint8_t *)(base + frame->eip);
                        serial_puts("  bytes@");
                        serial_puthex(frame->eip);
                        serial_puts(":");
                        for (i2 = 0; i2 < 16; i2++) {
                            serial_puts(" ");
                            serial_puthex(code2[i2]);
                        }
                        serial_puts("\n");
                        /* s45: also dump bytes BEFORE the faulter (EIP-32..EIP-1)
                         * so we can see how the trampoline / preceding code
                         * was set up. Critical for understanding the IRETD-
                         * trampoline protocol. */
                        if (frame->eip >= 32) {
                            serial_puts("  bytes@");
                            serial_puthex(frame->eip - 32);
                            serial_puts(" (pre): ");
                            for (i2 = -32; i2 < 0; i2++) {
                                serial_puts(" ");
                                serial_puthex(code2[i2]);
                            }
                            serial_puts("\n");
                        }
                        /* s45: dump stack window — 16 dwords starting at
                         * SS:ESP (so we see saved registers, outer frames,
                         * and any trampoline parameters past the IRET frame). */
                        {
                            int ss_idx2 = SEL_TO_IDX(frame->ss & 0xFFFF);
                            if ((frame->ss & 4) && ss_idx2 < DPMI_LDT_ENTRIES &&
                                LDT_USED(c, ss_idx2)) {
                                uint32_t ss_b2 = desc_get_base(&c->ldt[ss_idx2]);
                                int ss_b32 = (c->ldt[ss_idx2].limit_hi & 0x40) ? 1 : 0;
                                uint32_t s_off = ss_b32 ? frame->esp : (frame->esp & 0xFFFF);
                                uint32_t *stk = (uint32_t *)(ss_b2 + s_off);
                                serial_puts("  stk@SS:ESP=");
                                serial_puthex(frame->ss & 0xFFFF);
                                serial_puts(":");
                                serial_puthex(frame->esp);
                                serial_puts(" lin=");
                                serial_puthex(ss_b2 + s_off);
                                serial_puts(" dwords:");
                                int sd2;
                                for (sd2 = 0; sd2 < 16; sd2++) {
                                    serial_puts(" ");
                                    serial_puthex(stk[sd2]);
                                }
                                serial_puts("\n");
                            }
                        }
                        /* s45: decode CALL/JMP FAR with disp16 memory operand
                         * (FF /3 mod=00 r/m=6, FF /5 mod=00 r/m=6) and dump
                         * the seg:off the app is calling/jumping through.
                         * DOOM caches Get-RM-Vector results into a table and
                         * CALL FAR's through them; bad cached value → #GP. */
                        uint8_t op0 = code2[0], modrm = code2[1];
                        int op_size_64 = (op0 == 0x66);
                        if (op_size_64) { op0 = code2[1]; modrm = code2[2]; }
                        if (op0 == 0xFF) {
                            int reg = (modrm >> 3) & 7;
                            int mod = (modrm >> 6) & 3;
                            int rm  = modrm & 7;
                            if ((reg == 3 || reg == 5) && mod == 0 && rm == 6) {
                                uint8_t *d = op_size_64 ? &code2[3] : &code2[2];
                                uint16_t disp16 = d[0] | (d[1] << 8);
                                int ds_idx = SEL_TO_IDX(frame->ds & 0xFFFF);
                                if ((frame->ds & 4) && ds_idx < DPMI_LDT_ENTRIES &&
                                    LDT_USED(c, ds_idx)) {
                                    uint32_t ds_base = desc_get_base(&c->ldt[ds_idx]);
                                    uint32_t lin = ds_base + disp16;
                                    serial_puts("  far-target@DS:");
                                    serial_puthex(disp16);
                                    serial_puts(" (lin=");
                                    serial_puthex(lin);
                                    serial_puts(") bytes:");
                                    int dn = op_size_64 ? 6 : 4;
                                    for (i2 = 0; i2 < dn; i2++) {
                                        serial_puts(" ");
                                        if (vmm_get_physical(lin & ~0xFFFu)) {
                                            uint8_t b = *(volatile uint8_t *)(lin + i2);
                                            if (b < 0x10) serial_puts("0");
                                            serial_puthex(b);
                                        } else {
                                            serial_puts("--");
                                        }
                                    }
                                    /* Also dump a 32-byte window around the
                                     * target to spot the cached vector table
                                     * shape. */
                                    serial_puts(" win[-8..+24]:");
                                    for (i2 = -8; i2 < 24; i2++) {
                                        uint32_t a = lin + i2;
                                        serial_puts(" ");
                                        if (vmm_get_physical(a & ~0xFFFu)) {
                                            uint8_t b = *(volatile uint8_t *)a;
                                            if (b < 0x10) serial_puts("0");
                                            serial_puthex(b);
                                        } else {
                                            serial_puts("--");
                                        }
                                    }
                                    serial_puts("\n");
                                }
                            }
                        }
                    }
                    dumped_eip[dumped_n] = frame->eip;
                    dumped_cs[dumped_n]  = frame->cs & 0xFFFF;
                    dumped_n++;
                }
            }
            /* Ses36 ES-leak hunt: at the first 2 #GP delivery events, dump
             * stubinfo bytes at offset 0x20..0x2F from BOTH the original
             * 16-bit stub DS (where the stub wrote psp_selector) AND from
             * FS (where the 32-bit setup_environment reads it). Compares
             * the write-side and the read-side, so we can tell:
             *   - bytes match and contain 0x97 at +0x26 → bug is elsewhere
             *   - bytes match but contain V86-segment value → stub wrote bad ES
             *   - bytes diverge → FS points at a different copy that got stomped */
            if (exc_num == 0x0D) {
                static int stub_dumped = 0;
                if (stub_dumped < 2) {
                    /* (a) original stub DS — client_ds */
                    int sds_idx = SEL_TO_IDX(c->client_ds);
                    uint32_t sds_base = (sds_idx < DPMI_LDT_ENTRIES && LDT_USED(c, sds_idx))
                                        ? desc_get_base(&c->ldt[sds_idx]) : 0;
                    serial_puts("  stubinfo-DS sel=");
                    serial_puthex(c->client_ds);
                    serial_puts(" base=");
                    serial_puthex(sds_base);
                    serial_puts(" [+0x20..0x2F]: ");
                    if (sds_base) {
                        for (int i = 0x20; i < 0x30; i++) {
                            uint32_t a = sds_base + i;
                            if (vmm_get_physical(a & ~0xFFFu)) {
                                uint8_t b = *(volatile uint8_t *)a;
                                if (b < 0x10) serial_puts("0");
                                serial_puthex(b);
                                serial_puts(" ");
                            } else { serial_puts("-- "); }
                        }
                    }
                    serial_puts("\n");
                    /* (b) FS at fault — what the 32-bit code reads from */
                    uint16_t fs_sel = frame->fs & 0xFFFF;
                    int fs_idx = SEL_TO_IDX(fs_sel);
                    uint32_t fs_base = 0;
                    if ((fs_sel & 4) && fs_idx > 0 && fs_idx < DPMI_LDT_ENTRIES &&
                        LDT_USED(c, fs_idx)) {
                        fs_base = desc_get_base(&c->ldt[fs_idx]);
                    }
                    serial_puts("  stubinfo-FS sel=");
                    serial_puthex(fs_sel);
                    serial_puts(" base=");
                    serial_puthex(fs_base);
                    serial_puts(" [+0x20..0x2F]: ");
                    if (fs_base) {
                        for (int i = 0x20; i < 0x30; i++) {
                            uint32_t a = fs_base + i;
                            if (vmm_get_physical(a & ~0xFFFu)) {
                                uint8_t b = *(volatile uint8_t *)a;
                                if (b < 0x10) serial_puts("0");
                                serial_puthex(b);
                                serial_puts(" ");
                            } else { serial_puts("-- "); }
                        }
                    }
                    serial_puts("\n");
                    stub_dumped++;
                }
            }

            /* On first 3 #GP-at-PM events, dump 32 bytes of faulting stack
             * so we can see what RETF / IRETD would pop. Mask ESP when SS.B=0. */
            if (exc_num == 0x0D) {
                static int stk_dumped = 0;
                if (stk_dumped < 3) {
                    int ss_ci = SEL_TO_IDX(frame->ss & 0xFFFF);
                    if ((frame->ss & 4) && ss_ci < DPMI_LDT_ENTRIES &&
                        LDT_USED(c, ss_ci)) {
                        uint32_t ss_b = desc_get_base(&c->ldt[ss_ci]);
                        int ss_b32 = (c->ldt[ss_ci].limit_hi & 0x40) ? 1 : 0;
                        uint32_t s_off = ss_b32 ? frame->esp : (frame->esp & 0xFFFF);
                        uint32_t *stk = (uint32_t *)(ss_b + s_off);
                        serial_puts("  stk SS=");
                        serial_puthex(frame->ss & 0xFFFF);
                        serial_puts(" ESP=");
                        serial_puthex(frame->esp);
                        serial_puts(" base=");
                        serial_puthex(ss_b);
                        serial_puts(" dwords:");
                        int sd;
                        for (sd = 0; sd < 8; sd++) {
                            serial_puts(" ");
                            serial_puthex(stk[sd]);
                        }
                        serial_puts("\n");
                    }
                    stk_dumped++;
                }
            }
            redir_count++;
        }
    }
    if (exc_num < 32 && c->pm_exc_vectors[exc_num].selector) {
        uint32_t ss_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ss & 0xFFFF)]);
        /* Frame width follows the CLIENT's bit-ness, per DPMI 0.9 §4.5 and
         * matching CWSDPMI (exphdlr.c:313-320). DOS/4GW is a
         * 32-bit client whose PM handlers live in a 16-bit CS=0x87 but
         * access frame fields with 16-bit instructions reading the dword
         * layout (e.g. MOV DS,[BP+0x16] reads the low word of the
         * "faulting CS" dword in a 32-bit frame). The handler also uses
         * 66 CB / 66 CF prefixes in 16-bit CS, which encode 32-bit RETF /
         * IRETD and pop a 32-bit return slot. */
        int is_32 = c->is_32bit;
        /* DPMI 0.9 exception frame layout (32-bit client):
         *   [SS:SP+00]: return EIP  (thunk: INT 0xF3)
         *   [SS:SP+04]: return CS   (exc_return_sel)
         *   [SS:SP+08]: error code
         *   [SS:SP+0C]: faulting EIP
         *   [SS:SP+10]: faulting CS
         *   [SS:SP+14]: faulting EFLAGS
         *   [SS:SP+18]: faulting ESP
         *   [SS:SP+1C]: faulting SS
         * Handler does (66) RETF → pops return CS:EIP → executes thunk →
         * INT 0xF3 traps back to kernel → kernel restores frame. */
        int ss_idx = SEL_TO_IDX(frame->ss & 0xFFFF);
        int ss32 = (ss_idx < DPMI_LDT_ENTRIES &&
                    (c->ldt[ss_idx].limit_hi & 0x40)) ? 1 : 0;
        uint32_t orig_esp = frame->esp;
        uint32_t frame_sz = is_32 ? 32 : 16;
        uint32_t new_esp;
        if (ss32) {
            new_esp = orig_esp - frame_sz;
        } else {
            /* B=0 stack: SP wraps in low 16; high 16 of ESP stays put. */
            uint32_t sp = (orig_esp - frame_sz) & 0xFFFF;
            new_esp = (orig_esp & 0xFFFF0000) | sp;
        }
        uint32_t stk_off = ss32 ? new_esp : (new_esp & 0xFFFF);
        c->exc_save.active      = 1;
        c->exc_save.is_32       = is_32;
        c->exc_save.orig_esp    = orig_esp;
        c->exc_save.orig_ss     = frame->ss;
        c->exc_save.frame_base  = ss_base + stk_off;
        c->exc_save.orig_eip    = frame->eip;
        c->exc_save.orig_cs     = frame->cs;
        c->exc_save.orig_eflags = frame->eflags;
        {
            /* s45 falsification: stamp this deliver with a monotonic seq so
             * the INT 0xF3 return path can correlate which deliver it pairs
             * with. The s44 close-out claimed deliver/return read the SAME
             * memory at frame_base and got different bytes — but the deliver
             * and return throttled dumps tick on independent counters, so
             * they may be from DIFFERENT exception cycles entirely. */
            static uint32_t exc_seq_next = 0;
            c->exc_save.seq = ++exc_seq_next;
        }
        frame->esp = new_esp;
        if (is_32) {
            uint32_t *stk32 = (uint32_t *)(ss_base + stk_off);
            stk32[0] = 0;                    /* return EIP (offset 0 in thunk) */
            stk32[1] = c->exc_return_sel;    /* return CS */
            stk32[2] = frame->err_code;      /* error code */
            stk32[3] = frame->eip;           /* faulting EIP */
            stk32[4] = frame->cs;            /* faulting CS */
            stk32[5] = frame->eflags;        /* faulting EFLAGS */
            stk32[6] = orig_esp;             /* faulting ESP (before our push) */
            stk32[7] = frame->ss;            /* faulting SS */
        } else {
            uint16_t *stk = (uint16_t *)(ss_base + stk_off);
            stk[0] = 0;                              /* return IP */
            stk[1] = c->exc_return_sel;               /* return CS */
            stk[2] = frame->err_code & 0xFFFF;        /* error code */
            stk[3] = frame->eip & 0xFFFF;             /* faulting IP */
            stk[4] = frame->cs & 0xFFFF;              /* faulting CS */
            stk[5] = frame->eflags & 0xFFFF;          /* faulting FLAGS */
            stk[6] = orig_esp & 0xFFFF;               /* faulting SP (low 16) */
            stk[7] = frame->ss & 0xFFFF;              /* faulting SS */
        }
        /* Ses32 H9 diagnostic: record this exception delivery's
         * (fEIP, fCS, fEFL) triple into the circular log. Dumped at next
         * #GP delivery, alongside session-31's INT 21h log. */
        {
            uint32_t i = pm_exc_log_count & (PM_EXC_LOG_N - 1);
            pm_exc_log[i].exc_num = exc_num;
            pm_exc_log[i].width   = is_32 ? 32 : 16;
            pm_exc_log[i].h_sel   = c->pm_exc_vectors[exc_num].selector;
            pm_exc_log[i].eip     = frame->eip;
            pm_exc_log[i].cs      = frame->cs;
            pm_exc_log[i].eflags  = frame->eflags;
            pm_exc_log[i].ss_esp  =
                ((frame->ss & 0xFFFF) << 16) | (new_esp & 0xFFFF);
            pm_exc_log_count++;
        }
        if (is_32) {
            static int deliv_dumps = 0;
            if (deliv_dumps < 32) {
                uint32_t *frm = (uint32_t *)(ss_base + stk_off);
                serial_puts("DPMI: exc deliver seq=");
                serial_puthex(c->exc_save.seq);
                serial_puts(" fb=");
                serial_puthex(ss_base + stk_off);
                serial_puts(" [retEIP=");
                serial_puthex(frm[0]);
                serial_puts(" retCS=");
                serial_puthex(frm[1]);
                serial_puts(" EC=");
                serial_puthex(frm[2]);
                serial_puts(" fEIP=");
                serial_puthex(frm[3]);
                serial_puts(" fCS=");
                serial_puthex(frm[4]);
                serial_puts(" fEFL=");
                serial_puthex(frm[5]);
                serial_puts(" fESP=");
                serial_puthex(frm[6]);
                serial_puts(" fSS=");
                serial_puthex(frm[7]);
                serial_puts("]\n");
                deliv_dumps++;
            }
            /* Ses31 diagnostic: on #GP, dump the PM INT 21h push log
             * (oldest → newest). If any entry shows (eip=0,cs=0) it
             * confirms Hypothesis A — one of our deliveries authored
             * the garbage IRETD frame. Otherwise Hypothesis B. */
            if (exc_num == 0x0D) {
                static int gp_log_dumps = 0;
                if (gp_log_dumps < 2) {
                    uint32_t total = pm_int21_log_count;
                    uint32_t n = total < PM_INT21_LOG_N
                                 ? total : PM_INT21_LOG_N;
                    serial_puts("DPMI: PM INT 21h push log total=");
                    serial_puthex(total);
                    serial_puts(" showing=");
                    serial_puthex(n);
                    serial_puts("\n");
                    uint32_t start = total < PM_INT21_LOG_N
                                     ? 0
                                     : (total & (PM_INT21_LOG_N - 1));
                    uint32_t k;
                    for (k = 0; k < n; k++) {
                        uint32_t idx = (start + k) & (PM_INT21_LOG_N - 1);
                        struct pm_int21_push_rec *r = &pm_int21_log[idx];
                        serial_puts("  [");
                        serial_puthex(k);
                        serial_puts("] AH=");
                        serial_puthex(r->ah);
                        serial_puts(" w=");
                        serial_puthex(r->width);
                        serial_puts(" sel=");
                        serial_puthex(r->h_sel);
                        serial_puts(" pushEIP=");
                        serial_puthex(r->eip);
                        serial_puts(" pushCS=");
                        serial_puthex(r->cs);
                        serial_puts(" pushEFL=");
                        serial_puthex(r->eflags);
                        serial_puts(" ss:esp=");
                        serial_puthex(r->ss_esp);
                        serial_puts("\n");
                    }
                    /* Ses32 H9: dump the PM exception delivery push log
                     * (oldest → newest). pushEIP/pushCS = the faulting
                     * EIP/CS pushed at +0x0C inside the 32-bit exc frame
                     * (or +0x06 for 16-bit). A (pushEIP=0, pushCS=0) entry
                     * confirms H9 for the exception-delivery path. */
                    {
                        uint32_t etot = pm_exc_log_count;
                        uint32_t en = etot < PM_EXC_LOG_N
                                      ? etot : PM_EXC_LOG_N;
                        serial_puts("DPMI: PM exc push log total=");
                        serial_puthex(etot);
                        serial_puts(" showing=");
                        serial_puthex(en);
                        serial_puts("\n");
                        uint32_t estart = etot < PM_EXC_LOG_N
                                          ? 0
                                          : (etot & (PM_EXC_LOG_N - 1));
                        uint32_t ek;
                        for (ek = 0; ek < en; ek++) {
                            uint32_t idx = (estart + ek) & (PM_EXC_LOG_N - 1);
                            struct pm_exc_push_rec *r = &pm_exc_log[idx];
                            serial_puts("  [");
                            serial_puthex(ek);
                            serial_puts("] exc=");
                            serial_puthex(r->exc_num);
                            serial_puts(" w=");
                            serial_puthex(r->width);
                            serial_puts(" sel=");
                            serial_puthex(r->h_sel);
                            serial_puts(" pushEIP=");
                            serial_puthex(r->eip);
                            serial_puts(" pushCS=");
                            serial_puthex(r->cs);
                            serial_puts(" pushEFL=");
                            serial_puthex(r->eflags);
                            serial_puts(" ss:esp=");
                            serial_puthex(r->ss_esp);
                            serial_puts("\n");
                        }
                    }
                    /* Ses32 H9: dump the PM IRQ/INT delivery push log.
                     * pushEIP/pushCS = the interrupted EIP/CS pushed at
                     * +0x00 of the 3-dword (or 3-word) IRET frame. A
                     * (pushEIP=0, pushCS=0) entry confirms H9 for the
                     * IRQ/INT-delivery path. */
                    {
                        uint32_t itot = pm_irq_log_count;
                        uint32_t in = itot < PM_IRQ_LOG_N
                                      ? itot : PM_IRQ_LOG_N;
                        serial_puts("DPMI: PM IRQ push log total=");
                        serial_puthex(itot);
                        serial_puts(" showing=");
                        serial_puthex(in);
                        serial_puts("\n");
                        uint32_t istart = itot < PM_IRQ_LOG_N
                                          ? 0
                                          : (itot & (PM_IRQ_LOG_N - 1));
                        uint32_t ik;
                        for (ik = 0; ik < in; ik++) {
                            uint32_t idx = (istart + ik) & (PM_IRQ_LOG_N - 1);
                            struct pm_irq_push_rec *r = &pm_irq_log[idx];
                            serial_puts("  [");
                            serial_puthex(ik);
                            serial_puts("] vec=");
                            serial_puthex(r->vector);
                            serial_puts(" w=");
                            serial_puthex(r->width);
                            serial_puts(" sel=");
                            serial_puthex(r->h_sel);
                            serial_puts(" pushEIP=");
                            serial_puthex(r->eip);
                            serial_puts(" pushCS=");
                            serial_puthex(r->cs);
                            serial_puts(" pushEFL=");
                            serial_puthex(r->eflags);
                            serial_puts(" ss:esp=");
                            serial_puthex(r->ss_esp);
                            serial_puts("\n");
                        }
                    }
                    gp_log_dumps++;
                }
            }
        }
        frame->eip = c->pm_exc_vectors[exc_num].offset;
        frame->cs  = c->pm_exc_vectors[exc_num].selector;
        /* Ensure DS and ES are valid PM selectors for the handler */
        frame->ds  = c->client_ds;
        frame->es  = c->client_ds;
        /* DPMI 0.9 §4.5 / CWSDPMI exphdlr.c:330: PM exception handler runs
         * with IF=0 (and TF=0). Without this, the timer IRQ fires inside the
         * handler — IRQ→PM delivery lands on the SAME PM stack as our exc
         * frame, and the timer ISR scribbles the frame. s45 falsification:
         * seq=1 deliver/return at fb=0x0201FCB8 showed deliver[0x08..0x17]
         * had migrated to return[0x10..0x1F] — consistent with an
         * interrupting ISR pushing/popping on the frame's stack. */
        frame->eflags &= ~0x300u;
        return esp;
    }

    return 0;  /* no handler */
}

/* ================================================================
 * Real-Mode Callback Dispatch
 *
 * Called from the V86 monitor when it traps INT 0xF2 at a callback
 * stub in low memory (0x0700+). Saves the current V86 registers into
 * the client's register buffer, then redirects execution to the PM
 * handler registered via INT 31h/0303.
 *
 * The PM handler receives:
 *   DS:ESI → saved RM register structure (in client's buffer)
 *   ES:EDI → RM SS:SP area (so handler can modify RM stack)
 * The handler modifies the register structure, then executes RETF/IRET
 * which returns here. We restore RM state from the (modified) buffer.
 *
 * For now, we use a simplified approach: the PM handler is called via
 * the same frame-redirect mechanism used for PM interrupt vectors.
 * The callback IRET returns to V86 mode.
 * ================================================================ */

uint32_t dpmi_rmcb_dispatch(int rmcb_id, uint32_t esp) {
    struct v86_frame {
        /* isr_stubs save area */
        uint32_t gs, fs, es, ds;
        uint32_t edi, esi, ebp, _esp, ebx, edx, ecx, eax;
        uint32_t int_num, err_code;
        /* CPU IRET frame (ring transition + V86) */
        uint32_t eip, cs, eflags, v86_esp, ss;
        /* V86 extra segments (popped by IRET with VM=1) */
        uint32_t v86_es, v86_ds, v86_fs, v86_gs;
    } *frame = (struct v86_frame *)esp;

    /* Find the DPMI client that owns this callback.
     * Search all clients for the active one with this rmcb slot. */
    int ci;
    struct dpmi_client *c = 0;
    for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
        if (clients[ci].active && rmcb_id < clients[ci].next_rmcb &&
            clients[ci].rmcb[rmcb_id].active) {
            c = &clients[ci];
            break;
        }
    }

    if (!c) {
        serial_puts("DPMI: rmcb dispatch ");
        serial_puthex(rmcb_id);
        serial_puts(" — no client\n");
        return 0;
    }

    /* s42 — V86-mode callback (rm_mode=1). Set up by v86.c 0x0303.
     * Handler lives in V86 memory; no PM transition. The caller's
     * INT F2 trap already left frame->eip at the stub's IRET byte,
     * so the V86 monitor's normal +2 EIP advancement will resume at
     * IRET and return to where the V86 caller CALL FAR'd in from.
     * This is effectively a no-op callback that satisfies DOS/16M's
     * handshake. Real V86-handler dispatch (save regs, far call to
     * pm_sel:pm_off) can be added later if needed. */
    if (c->rmcb[rmcb_id].rm_mode) {
        serial_puts("DPMI: rmcb ");
        serial_puthex(rmcb_id);
        serial_puts(" fire → RM (no-op) ");
        serial_puthex(c->rmcb[rmcb_id].pm_sel);
        serial_puts(":");
        serial_puthex(c->rmcb[rmcb_id].pm_off);
        serial_puts("\n");
        return 0;
    }

    serial_puts("DPMI: rmcb ");
    serial_puthex(rmcb_id);
    serial_puts(" fire → PM ");
    serial_puthex(c->rmcb[rmcb_id].pm_sel);
    serial_puts(":");
    serial_puthex(c->rmcb[rmcb_id].pm_off);
    serial_puts("\n");

    /* Copy current V86 registers into the client's register buffer */
    uint32_t buf_base = desc_get_base(
        &c->ldt[SEL_TO_IDX(c->rmcb[rmcb_id].regs_sel)]);
    struct dpmi_regs *rm_regs = (struct dpmi_regs *)
        (buf_base + c->rmcb[rmcb_id].regs_off);

    rm_regs->edi = frame->edi;
    rm_regs->esi = frame->esi;
    rm_regs->ebp = frame->ebp;
    rm_regs->reserved_esp = 0;
    rm_regs->ebx = frame->ebx;
    rm_regs->edx = frame->edx;
    rm_regs->ecx = frame->ecx;
    rm_regs->eax = frame->eax;
    rm_regs->flags = frame->eflags & 0xFFFF;
    rm_regs->es = frame->v86_es & 0xFFFF;
    rm_regs->ds = frame->v86_ds & 0xFFFF;
    rm_regs->fs = frame->v86_fs & 0xFFFF;
    rm_regs->gs = frame->v86_gs & 0xFFFF;
    rm_regs->cs = frame->cs & 0xFFFF;
    rm_regs->ip = frame->eip & 0xFFFF;
    rm_regs->ss = frame->ss & 0xFFFF;
    rm_regs->sp = frame->v86_esp & 0xFFFF;

    /* Rebuild the frame as a PM Ring 3 IRET frame.
     * The PM handler will return with RETF or IRET; we set up a return
     * address that lands back in V86 mode. For simplicity, we set the
     * PM handler's stack to have a return frame pointing to the client's
     * original PM return point. Since the callback handler typically just
     * modifies the register buffer and IRETs, we redirect EIP/CS and
     * let the IRET from the handler return to wherever the client was.
     *
     * Simplified approach: convert the V86 frame to a PM frame that
     * calls the handler. The handler IRETs back to this code, and we
     * restore V86 from the register buffer. But since we can't easily
     * resume from here, we use the same technique as PM vector redirect:
     * push a return frame on the client's PM stack, then set CS:EIP to
     * the handler.
     *
     * Actually, even simpler for now: just save the V86 state and return
     * to V86 with the register buffer filled. The PM client's timer
     * handler is already being called via the PIT delivery in idt.c.
     * The RM callback is mainly needed for the PM handler to chain to
     * the old RM timer handler — which is an IRET stub (no-op). */

    /* For now, just return to V86 with the register buffer populated.
     * The PIT timer delivery to PM (idt.c line 218) handles the actual
     * PM timer handler invocation. */
    return 0;
}

/* ================================================================
 * PM Software Interrupt Handler
 *
 * Called from isr_dispatch when a Ring 3 PM client executes a
 * software INT. Routes DOS/BIOS calls through our emulation
 * and DPMI calls through dpmi_int31.
 * ================================================================ */

int dpmi_pm_has_handler(uint16_t cs_sel, uint8_t vector) {
    int cs_idx = SEL_TO_IDX(cs_sel);
    for (int ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
        if (!clients[ci].active) continue;
        if (clients[ci].client_cs == cs_sel ||
            (cs_idx < DPMI_LDT_ENTRIES && LDT_USED(&clients[ci], cs_idx))) {
            struct dpmi_client *c = &clients[ci];
            return c->pm_vectors[vector].selector &&
                   (c->pm_vectors[vector].offset != 0 ||
                    c->pm_vectors[vector].selector == c->pm_int_chain_sel);
        }
    }
    return 0;
}

/* Hardware-IRQ-to-PM delivery.
 *
 * Builds an inner IRET frame on the PM stack and rewrites the kernel's
 * isr_frame so the imminent IRETD lands at the PM client's registered handler
 * for `vector` (a BIOS-style INT number, e.g. 0x08 for IRQ 0). The PM handler,
 * on its own IRETD, pops the 3 dwords we pushed and resumes the interrupted PM
 * code at the original CS:EIP/EFL.
 *
 * The caller (idt.c) is responsible for two things:
 *   1. Translating the IDT vector → BIOS-style vector (IRQ 0..7 → INT 0x08..0x0F,
 *      IRQ 8..15 → INT 0x70..0x77) per CWSDPMI exphdlr.c:110-111.
 *   2. Gating on dpmi_pm_has_handler() — call here only when a real handler
 *      exists. We do NOT touch frame->eax / eflags.CF on failure (unlike
 *      dpmi_handle_pm_int's unhandled-vector path), so the caller can fall
 *      through to kernel-side IRQ handling cleanly.
 *
 * Do NOT EOI before calling — the PM handler runs at IOPL=3 and does its own
 * EOI (Allegro's fixed_timer_handler does outportb(0x20, 0x20)). CWSDPMI's
 * irq_common likewise doesn't EOI. Double-EOI would prematurely ACK the next
 * pending IRQ.
 *
 * Returns 0 on successful delivery (frame modified in place; caller returns
 * the same esp), -1 if no client matched the frame's CS (caller falls back). */
int dpmi_deliver_pm_irq(uint8_t vector, uint32_t esp) {
    struct isr_frame *frame = (struct isr_frame *)esp;
    uint16_t cs_sel = frame->cs & 0xFFFF;
    int cs_idx = SEL_TO_IDX(cs_sel);
    struct dpmi_client *c = 0;
    for (int ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
        if (!clients[ci].active) continue;
        if (clients[ci].client_cs == cs_sel ||
            (cs_idx < DPMI_LDT_ENTRIES && LDT_USED(&clients[ci], cs_idx))) {
            c = &clients[ci];
            break;
        }
    }
    if (!c) return -1;

    uint16_t h_sel = c->pm_vectors[vector].selector;
    uint32_t h_off = c->pm_vectors[vector].offset;

    int ss_idx = SEL_TO_IDX(frame->ss & 0xFFFF);
    uint32_t ss_base = desc_get_base(&c->ldt[ss_idx]);
    int ss32 = (ss_idx < DPMI_LDT_ENTRIES &&
                (c->ldt[ss_idx].limit_hi & 0x40)) ? 1 : 0;
    int is_32 = c->is_32bit;
    uint32_t bytes = is_32 ? 12 : 6;

    uint32_t new_esp = ss32
        ? frame->esp - bytes
        : ((frame->esp & 0xFFFF0000) | ((frame->esp - bytes) & 0xFFFF));
    uint32_t stk_off = ss32 ? new_esp : (new_esp & 0xFFFF);

    /* Push inner IRET frame. Width follows client bit-ness, same rule as
     * dpmi_handle_pm_int's generic delivery (DPMI 0.9 §4.5). */
    if (is_32) {
        uint32_t *stk32 = (uint32_t *)(ss_base + stk_off);
        stk32[0] = frame->eip;
        stk32[1] = frame->cs;
        stk32[2] = frame->eflags;
    } else {
        uint16_t *stk = (uint16_t *)(ss_base + stk_off);
        stk[0] = frame->eip & 0xFFFF;
        stk[1] = frame->cs & 0xFFFF;
        stk[2] = frame->eflags & 0xFFFF;
    }

    /* Delivery log: first 6 entries per vector logged in detail; bulk counter
     * tracks steady-state throughput. */
    {
        static int irq_deliv_log = 0;
        static uint32_t irq_deliv_counts[256] = {0};
        irq_deliv_counts[vector]++;
        if (irq_deliv_log < 6) {
            serial_puts("DPMI: IRQ→PM v=");
            serial_puthex(vector);
            serial_puts(" h=");
            serial_puthex(h_sel);
            serial_puts(":");
            serial_puthex(h_off);
            serial_puts(" from ");
            serial_puthex(frame->cs);
            serial_puts(":");
            serial_puthex(frame->eip);
            serial_puts(" SS:ESP=");
            serial_puthex(frame->ss);
            serial_puts(":");
            serial_puthex(frame->esp);
            serial_puts(" →");
            serial_puthex(new_esp);
            serial_puts(" efl=");
            serial_puthex(frame->eflags);
            serial_puts(" count=");
            serial_puthex(irq_deliv_counts[vector]);
            serial_puts("\n");
            irq_deliv_log++;
        }
        /* Periodic counter dump so the steady-state rate is observable. */
        if ((irq_deliv_counts[vector] & 0xFF) == 0) {
            serial_puts("DPMI: IRQ→PM v=");
            serial_puthex(vector);
            serial_puts(" count=");
            serial_puthex(irq_deliv_counts[vector]);
            serial_puts("\n");
        }
    }

    frame->esp = new_esp;
    frame->eip = h_off;
    frame->cs  = h_sel;
    /* Clear TF (single-step) and IF — CPU clears IF when entering via IDT,
     * and a hooked handler that re-enables interrupts (Allegro does so after
     * EOI) needs to do so explicitly. Matches CWSDPMI's 0x3002 EFL. */
    frame->eflags &= ~0x300;  /* TF=bit8, IF=bit9 */
    return 0;
}

uint32_t dpmi_handle_pm_int(uint8_t vector, uint32_t esp) {
    struct isr_frame *frame = (struct isr_frame *)esp;

    /* Find the active DPMI client from the CS selector.
     * DOS extenders allocate new code segments, so we can't just match
     * client_cs. Check if the CS selector is an LDT selector (bit 2 set)
     * belonging to any active client's LDT. */
    int ci;
    struct dpmi_client *c = 0;
    uint16_t cs_sel = frame->cs & 0xFFFF;
    int cs_idx = SEL_TO_IDX(cs_sel);
    for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
        if (clients[ci].active) {
            /* Match if CS is the original client_cs OR any valid LDT entry */
            if (clients[ci].client_cs == cs_sel ||
                (cs_idx < DPMI_LDT_ENTRIES && LDT_USED(&clients[ci], cs_idx))) {
                c = &clients[ci];
                break;
            }
        }
    }

    if (!c) {
        serial_puts("DPMI: PM INT ");
        serial_puthex(vector);
        serial_puts(" but no active client for CS=");
        serial_puthex(frame->cs);
        serial_puts("\n");
        return 0;
    }

    /* Post-init trace: log first 50 PM INTs after timer delivery is enabled.
     * This captures what DOS/4GW does after completing basic setup. */
    {
        static int post_init_trace = 0;
        if (dpmi_timer_ready && post_init_trace < 200) {
            serial_puts("PM-TRACE: INT ");
            serial_puthex(vector);
            if (vector == 0x31) {
                serial_puts(" AX=");
                serial_puthex(frame->eax & 0xFFFF);
            } else if (vector == 0x21) {
                serial_puts(" AH=");
                serial_puthex((frame->eax >> 8) & 0xFF);
            }
            serial_puts(" EIP=");
            serial_puthex(frame->eip);
            serial_puts("\n");
            post_init_trace++;
        }
    }

    /* Exception handler return (INT 0xF3).
     * The handler did RETF → popped return CS:EIP → ran our thunk →
     * INT 0xF3 trapped here. The client stack now contains the
     * (possibly modified) exception frame:
     *   ESP+0: error code
     *   ESP+4: faulting EIP (may be modified by handler)
     *   ESP+8: faulting CS
     *   ESP+C: faulting EFLAGS
     *   ESP+10: faulting ESP
     *   ESP+14: faulting SS
     * Read the frame, clean the stack, and restore execution. */
    if (vector == 0xF3 && c) {
        /* Read the (possibly modified) faulting frame from the FIXED location
         * where we originally wrote it — handlers commonly relocate their own
         * ESP (private stack) and the CPU's post-RETF ESP is therefore not a
         * reliable pointer to our frame. exc_save.frame_base captures the
         * absolute linear address we used at delivery time. */
        int f3_is_32 = c->exc_save.active ? c->exc_save.is_32 : 1;
        uint32_t saved_eip, saved_cs, saved_eflags, saved_esp, saved_ss;
        if (c->exc_save.active) {
            if (f3_is_32) {
                uint32_t *frm = (uint32_t *)c->exc_save.frame_base;
                /* frm[2]=err, [3]=eip, [4]=cs, [5]=eflags, [6]=esp, [7]=ss */
                saved_eip    = frm[3];
                saved_cs     = frm[4];
                saved_eflags = frm[5];
                saved_esp    = frm[6];
                saved_ss     = frm[7];
            } else {
                uint16_t *frm = (uint16_t *)c->exc_save.frame_base;
                /* frm[2]=err, [3]=ip, [4]=cs, [5]=flags, [6]=sp, [7]=ss */
                saved_eip    = frm[3];
                saved_cs     = frm[4];
                saved_eflags = frm[5];
                saved_esp    = frm[6];
                saved_ss     = frm[7];
            }
        } else {
            /* Fall back to the legacy "read from current ESP" path if delivery
             * didn't record state — shouldn't happen but keeps us alive. */
            uint32_t ss_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ss & 0xFFFF)]);
            uint32_t *exc = (uint32_t *)(ss_base + frame->esp);
            saved_eip    = exc[1];
            saved_cs     = exc[2];
            saved_eflags = exc[3];
            saved_esp    = exc[4];
            saved_ss     = exc[5];
        }

        {
            static int f3_dumps = 0;
            if (f3_dumps < 32) {
                serial_puts("DPMI: exc return seq=");
                serial_puthex(c->exc_save.active ? c->exc_save.seq : 0xFFFFFFFFu);
                serial_puts(" is_32=");
                serial_puthex(f3_is_32);
                serial_puts(" frame_base=");
                serial_puthex(c->exc_save.frame_base);
                serial_puts(" cur_esp=");
                serial_puthex(frame->esp);
                if (c->exc_save.active && f3_is_32) {
                    uint32_t *frm = (uint32_t *)c->exc_save.frame_base;
                    serial_puts(" [retEIP=");
                    serial_puthex(frm[0]);
                    serial_puts(" retCS=");
                    serial_puthex(frm[1]);
                    serial_puts(" EC=");
                    serial_puthex(frm[2]);
                    serial_puts(" fEIP=");
                    serial_puthex(frm[3]);
                    serial_puts(" fCS=");
                    serial_puthex(frm[4]);
                    serial_puts(" fEFL=");
                    serial_puthex(frm[5]);
                    serial_puts(" fESP=");
                    serial_puthex(frm[6]);
                    serial_puts(" fSS=");
                    serial_puthex(frm[7]);
                    serial_puts("]");
                }
                serial_puts("\n");
                f3_dumps++;
            }
        }

        serial_puts("DPMI: exc return → ");
        serial_puthex(saved_cs);
        serial_puts(":");
        serial_puthex(saved_eip);
        serial_puts(" SS:ESP=");
        serial_puthex(saved_ss);
        serial_puts(":");
        serial_puthex(saved_esp);
        serial_puts("\n");

        /* s45 trampoline recovery: a fault at IRETD with the inner IRET
         * frame's CS=0 is a host-call trampoline. Stack at fSS:fESP
         * carries:
         *   [+00..+0B]: bad sentinel (EIP=0, CS=0, EFL=...)
         *   [+0C..+17]: REAL continuation (EIP=cont, CS=valid, EFL=valid)
         * Observed in s45 DOOM run: IRETD at 0x10F:0x913, sentinel + real
         * frame at 0x10F:0x918 (5 bytes past the IRETD, after a 3-byte
         * 87 DB 90 filler — the classic trampoline shape).
         * Consume one IRET frame (12 bytes for same-PL 32-bit IRETD) and
         * resume the original faulting instruction; the IRETD will now pop
         * the REAL frame and continue. Apply only on BAD CS *and* a
         * snapshot exists *and* the faulter looks like IRETD (66 CF). */
        /* Validate saved_cs against the client's LDT. The handler's frame
         * memmove (s45 empirical) leaves the err_code in the fCS slot for
         * exceptions with a non-zero EC — and an EC value with bit 2 set
         * (e.g. 0xB5C from cycle 2) sneaks past a bit-2 check. Require
         * LDT-used to be sure. */
        int cs_valid = (saved_cs && (saved_cs & 4));
        if (cs_valid) {
            int idx_cs = SEL_TO_IDX(saved_cs);
            cs_valid = (idx_cs < DPMI_LDT_ENTRIES && LDT_USED(c, idx_cs));
        }
        if (!cs_valid) {
            int is_iretd = 0;
            int iretd_len = 0;  /* 1 for CF (16-bit IRET), 2 for 66 CF (IRETD) */
            if (c->exc_save.active) {
                int cs_idx_o = SEL_TO_IDX(c->exc_save.orig_cs & 0xFFFF);
                if ((c->exc_save.orig_cs & 4) && cs_idx_o < DPMI_LDT_ENTRIES) {
                    uint32_t cs_base_o = desc_get_base(&c->ldt[cs_idx_o]);
                    uint8_t *code = (uint8_t *)(cs_base_o + (c->exc_save.orig_eip & 0xFFFFFFFFu));
                    if (code[0] == 0x66 && code[1] == 0xCF) { is_iretd = 1; iretd_len = 2; }
                    else if (code[0] == 0xCF) { is_iretd = 1; iretd_len = 1; }
                }
            }
            if (is_iretd && c->exc_save.active) {
                static int tramp_logs = 0;
                if (tramp_logs < 8) {
                    serial_puts("DPMI: exc return BAD CS=");
                    serial_puthex(saved_cs);
                    serial_puts(" + IRETD at ");
                    serial_puthex(c->exc_save.orig_cs);
                    serial_puts(":");
                    serial_puthex(c->exc_save.orig_eip);
                    serial_puts(" — reverse-tramp: skip ");
                    serial_puthex(iretd_len);
                    serial_puts("B past IRETD, ESP unchanged\n");
                    tramp_logs++;
                }
                /* s45 trampoline recovery — TWO VARIANTS, only one active:
                 *
                 * VARIANT A (active): "+12 ESP" — consume the bad IRET
                 *   frame on the stack and re-execute the IRETD. The
                 *   second IRETD pops the real frame at orig_esp+12,
                 *   advances ESP by 12 more, and jumps to the real target.
                 *   The function being trampolined-to relies on IRETD
                 *   having advanced ESP for it — function epilogue's
                 *   RETF reads its return address at orig_esp+12+18 etc.
                 *
                 *     saved_eip = c->exc_save.orig_eip;
                 *     saved_esp = c->exc_save.orig_esp + 12;
                 *
                 * VARIANT B (alt, kept for reference): "skip IRETD" —
                 *   step past the IRETD instruction, leave ESP alone.
                 *   Tested in s45 run #4: faults earlier with RETF→CS=0
                 *   because the called function expects the +12 ESP
                 *   alignment that real IRETD would have provided.
                 *
                 *     saved_eip = c->exc_save.orig_eip + iretd_len;
                 *     saved_esp = c->exc_save.orig_esp;
                 *
                 * Variant A wins empirically — DOOM runs further before
                 * hitting the next blocker (CALL FAR through uninitialized
                 * pointer at DS:0x3628). */
                (void)iretd_len;  /* used only in variant B */
                saved_eip    = c->exc_save.orig_eip;
                saved_cs     = c->exc_save.orig_cs;
                saved_eflags = c->exc_save.orig_eflags;
                saved_esp    = c->exc_save.orig_esp + 12;
                saved_ss     = c->exc_save.orig_ss;
            } else {
                serial_puts("DPMI: exc return BAD CS=");
                serial_puthex(saved_cs);
                serial_puts(" — aborting to V86\n");
                /* Return to V86 mode instead of crashing */
                frame->eip    = c->v86_eip;
                frame->cs     = c->v86_cs;
                frame->eflags = 0x20202;  /* VM=1, IF=1 */
                frame->esp    = c->v86_esp;
                frame->ss     = c->v86_ss;
                {
                    uint32_t *v86_segs = (uint32_t *)&frame->ss + 1;
                    v86_segs[0] = c->v86_es;
                    v86_segs[1] = c->v86_ds;
                    v86_segs[2] = 0;
                    v86_segs[3] = 0;
                }
                dpmi_release_client(c);
                return 0;
            }
        }

        /* Restore execution at the (modified) faulting location.
         * For 16-bit handlers we only have 16-bit values from the stack;
         * preserve the high halves of ESP/SS that the handler couldn't
         * have written, so we don't truncate a 32-bit faulting context. */
        if (f3_is_32) {
            frame->eip    = saved_eip;
            frame->cs     = saved_cs;
            frame->eflags = saved_eflags;
            frame->esp    = saved_esp;
            frame->ss     = saved_ss;
        } else {
            frame->eip    = (frame->eip    & 0xFFFF0000) | (saved_eip    & 0xFFFF);
            frame->cs     = (frame->cs     & 0xFFFF0000) | (saved_cs     & 0xFFFF);
            frame->eflags = (frame->eflags & 0xFFFF0000) | (saved_eflags & 0xFFFF);
            /* For 16-bit handlers, ESP/SS can't be touched by the handler
             * (it has no 32-bit width to write), so resume from our save
             * rather than from a stack value the handler couldn't author. */
            if (c->exc_save.active) {
                frame->esp = c->exc_save.orig_esp;
                frame->ss  = c->exc_save.orig_ss;
            } else {
                frame->esp = (frame->esp & 0xFFFF0000) | (saved_esp & 0xFFFF);
                frame->ss  = (frame->ss  & 0xFFFF0000) | (saved_ss  & 0xFFFF);
            }
        }
        c->exc_save.active = 0;
        return 0;
    }

    if (vector == 0x31) {
        /* DPMI services */
        struct dpmi_regs regs;
        regs.eax = frame->eax;
        regs.ebx = frame->ebx;
        regs.ecx = frame->ecx;
        regs.edx = frame->edx;
        regs.esi = frame->esi;
        regs.edi = frame->edi;
        regs.ebp = frame->ebp;
        regs.es  = frame->es;
        regs.ds  = frame->ds;
        regs.flags = frame->eflags & 0xFFFF;

        int result = dpmi_int31(ci, &regs);

        if (result == 2) {
            /* 0x0301/0x0302 trampoline: dpmi_int31 stashed the
             * RmCallStruct linear address in c->rm_call_save. Perform
             * the actual PM→V86 switch here where we have the frame. */
            extern int dpmi_rm_call_setup_isr(int client_id, struct isr_frame *frame);
            return dpmi_rm_call_setup_isr(ci, frame);
        }

        frame->eax = regs.eax;
        frame->ebx = regs.ebx;
        frame->ecx = regs.ecx;
        frame->edx = regs.edx;
        frame->esi = regs.esi;
        frame->edi = regs.edi;
        /* Write back segment regs so dispatchers that update ES/DS (vendor
         * AX=0x0A00 returns ES:EDI = vendor entry-point) reach the client.
         * Other cases never touch regs.es/ds, so this is a no-op for them. */
        frame->es = (frame->es & 0xFFFF0000) | (regs.es & 0xFFFF);
        frame->ds = (frame->ds & 0xFFFF0000) | (regs.ds & 0xFFFF);
        if (result == 0)
            frame->eflags &= ~1;  /* CF=0: success */
        else
            frame->eflags |= 1;   /* CF=1: error */
        return 0;  /* no ESP change */
    }

    if (vector == 0x21) {
        /* DOS services — translate LDT selectors to pseudo-segments
         * so dos_int21's v86_ptr(seg, off) = seg*16 + off resolves
         * to the correct linear address (LDT base + offset).
         *
         * Handler delivery: DOS/4GW installs its own PM INT 21h handler
         * (e.g. 0xC7:0xC9E) to translate logical → physical file handles
         * before chaining to host. We deliver to the client handler when
         * one is installed and we're not already executing inside the
         * chain stub. The handler chains by FAR CALL'ing pm_int_chain_sel:0
         * (whose bytes are CD 21 CB) — the resulting INT 21h re-enters
         * here with CS == pm_int_chain_sel and falls through to dispatch
         * the dos_int21 call with the translated handle. */
        {
            uint16_t cs_now = frame->cs & 0xFFFF;
            uint16_t h_sel  = c->pm_vectors[0x21].selector;
            /* Ses31: deliver to the registered PM handler even when the
             * caller's CS matches h_sel. DOS/32A (and any DPMI 0.9
             * client) reflects DOS calls via DPMI 0x0300 — it does not
             * re-issue INT 21h in PM — so there is no recursion risk.
             * The old `cs_now != h_sel` guard skipped delivery for
             * DOS/32A's vendor calls (AH=0xFF AX=0xFF80..0xFF9A) which
             * its own _int21 handles internally, causing the host to
             * return CF=1 unhandled and DOS/32A to accumulate corrupt
             * state. Only bypass when CS==chain-stub (explicit "host
             * take over" signal from the chained handler). */
            if (h_sel && h_sel != c->pm_int_chain_sel
                      && cs_now != c->pm_int_chain_sel) {
                uint32_t ss_base =
                    desc_get_base(&c->ldt[SEL_TO_IDX(frame->ss & 0xFFFF)]);
                int ss_i = SEL_TO_IDX(frame->ss & 0xFFFF);
                int ss32 = (ss_i < DPMI_LDT_ENTRIES &&
                            (c->ldt[ss_i].limit_hi & 0x40)) ? 1 : 0;
                uint32_t bytes = c->is_32bit ? 12 : 6;
                uint32_t new_esp = ss32
                    ? frame->esp - bytes
                    : ((frame->esp & 0xFFFF0000) | ((frame->esp - bytes) & 0xFFFF));
                uint32_t stk_off = ss32 ? new_esp : (new_esp & 0xFFFF);
                frame->esp = new_esp;
                if (c->is_32bit) {
                    uint32_t *stk32 = (uint32_t *)(ss_base + stk_off);
                    stk32[0] = frame->eip;
                    stk32[1] = frame->cs;
                    stk32[2] = frame->eflags;
                } else {
                    uint16_t *stk = (uint16_t *)(ss_base + stk_off);
                    stk[0] = frame->eip & 0xFFFF;
                    stk[1] = frame->cs & 0xFFFF;
                    stk[2] = frame->eflags & 0xFFFF;
                }
                /* Ses31 diagnostic: record this delivery's pushed tuple
                 * into the circular log. Dumped at next #GP delivery. */
                {
                    uint32_t i = pm_int21_log_count & (PM_INT21_LOG_N - 1);
                    pm_int21_log[i].ah     = (frame->eax >> 8) & 0xFF;
                    pm_int21_log[i].width  = c->is_32bit ? 32 : 16;
                    pm_int21_log[i].h_sel  = h_sel;
                    pm_int21_log[i].eip    = frame->eip;
                    pm_int21_log[i].cs     = frame->cs;
                    pm_int21_log[i].eflags = frame->eflags;
                    pm_int21_log[i].ss_esp =
                        ((frame->ss & 0xFFFF) << 16) | (new_esp & 0xFFFF);
                    pm_int21_log_count++;
                }
                frame->eip = c->pm_vectors[0x21].offset;
                frame->cs  = h_sel;
                frame->eflags &= ~0x100;  /* clear TF */
                {
                    static int deliv_trace = 0;
                    if (deliv_trace < 16) {
                        serial_puts("DPMI: deliver PM INT 21h/");
                        serial_puthex((frame->eax >> 8) & 0xFF);
                        serial_puts(" BX=");
                        serial_puthex(frame->ebx & 0xFFFF);
                        serial_puts(" → ");
                        serial_puthex(h_sel);
                        serial_puts(":");
                        serial_puthex(c->pm_vectors[0x21].offset);
                        serial_puts("\n");
                        deliv_trace++;
                    }
                }
                return 0;
            }
        }
        uint8_t ah = (frame->eax >> 8) & 0xFF;
        serial_puts("DPMI: PM INT 21h/");
        serial_puthex(ah);
        serial_puts(" BX=");
        serial_puthex(frame->ebx & 0xFFFF);
        serial_puts(" DS=");
        serial_puthex(frame->ds & 0xFFFF);
        serial_puts(" DX=");
        serial_puthex(frame->edx & 0xFFFF);
        serial_puts(" CS=");
        serial_puthex(frame->cs & 0xFFFF);
        serial_puts("\n");
        uint32_t ds_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ds & 0xFFFF)]);
        uint32_t es_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->es & 0xFFFF)]);

        /* Get the DOS task ID from the V86 task */
        int dos_tid = v86_get_dos_task(c->v86_task_id);

        if (ah == 0x4C) {
            /* Terminate — return to V86/DOS.
             *
             * Overwrite the PM frame in place. The PM isr_frame and
             * V86 v86_frame share the same layout up through SS
             * (offset 72). V86 has 4 extra dwords (ES,DS,FS,GS) at
             * offsets 76-88 which we write above the PM frame.
             * IRET with VM=1 pops those extra segments. */
            uint8_t ret_code = frame->eax & 0xFF;
            serial_puts("DPMI: PM exit (code ");
            serial_puthex(ret_code);
            serial_puts(") → returning to V86\n");

            /* If the client left the card in a VESA mode, the
             * downstream V86 shell (FreeCOM, etc.) is in text mode 03h
             * mentally but the hardware is still scanning out a graphics
             * framebuffer — the user sees a frozen image after exit.
             * Allegro's GFX_TEXT teardown only calls restore_console_state
             * (a system_driver hook the DJGPP/DOS port no-ops), so no INT
             * 10h AX=0003 ever fires from PM. Three steps:
             *   1. Disable Bochs VBE (it overrides VGA regs once enabled).
             *   2. Reprogram VGA for text mode 03h.
             *   3. Tell the VT manager to repaint — the chars in its
             *      screen[] buffers are intact, but 0xB8000 may have been
             *      stomped by the graphics mode, and the VGA cursor
             *      position needs re-syncing. */
            vbe_set_text_mode();
            extern void vga_set_mode_03h(void);
            vga_set_mode_03h();
            extern void vt_repaint(void);
            vt_repaint();
            /* Hardware is back in 80×25 text — sync the VT record. */
            {
                int vtn = sched_vt_for_v86(c->v86_task_id);
                struct vt *v = vt_get(vtn);
                if (v) {
                    v->video = VT_VID_TEXT_03H;
                    serial_puts("VT: video=TEXT_03H vt=");
                    serial_puthex(vtn);
                    serial_puts(" (PM exit)\n");
                }
            }

            /* Use the frame in place — same ESP, just overwrite fields */

            /* isr_stubs save area: use kernel selectors for the
             * pop gs/fs/es/ds at CPL=0. IRET with VM=1 will load
             * the real V86 segments from the V86 area below. */
            frame->gs      = 0;
            frame->fs      = 0;
            frame->es      = 0x10;  /* kernel data seg */
            frame->ds      = 0x10;  /* kernel data seg */

            /* Clear GPRs, set return code */
            frame->edi = 0;
            frame->esi = 0;
            frame->ebp = 0;
            frame->eax = ret_code;

            /* CPU IRET frame — V86 mode return */
            frame->eip    = c->v86_eip;
            frame->cs     = c->v86_cs;
            frame->eflags = 0x20202;  /* VM=1, IF=1, bit1=1 */
            frame->esp    = c->v86_esp;
            frame->ss     = c->v86_ss;

            /* Write V86 extra segments ABOVE the PM frame.
             * These 4 dwords sit right after SS in memory. */
            {
                uint32_t *v86_segs = (uint32_t *)&frame->ss + 1;
                v86_segs[0] = c->v86_es;   /* v86_es */
                v86_segs[1] = c->v86_ds;   /* v86_ds */
                v86_segs[2] = 0;            /* v86_fs */
                v86_segs[3] = 0;            /* v86_gs */
            }

            dpmi_release_client_pm_exit(c);
            return 0;  /* no ESP change — frame modified in place */
        }

        /* INT 21h/48h (Allocate Memory): returns LDT selector, not segment */
        if (ah == 0x48) {
            struct dos_regs dregs;
            dregs.eax = frame->eax;
            dregs.ebx = frame->ebx;
            dregs.eflags = frame->eflags;
            dregs.ds = ds_base >> 4;
            dregs.es = es_base >> 4;
            dos_int21(dos_tid, &dregs);
            if (!(dregs.eflags & 1)) {
                /* Success: AX = real-mode segment. Create LDT descriptor. */
                uint16_t rm_seg = dregs.eax & 0xFFFF;
                uint32_t base = (uint32_t)rm_seg << 4;
                uint32_t size = (uint32_t)(frame->ebx & 0xFFFF) << 4;
                int idx = ldt_alloc(c, 1);
                if (idx) {
                    ldt_setup(c, idx, base, size ? size - 1 : 0xFFFF, 0xF2, 0);
                    frame->eax = (frame->eax & 0xFFFF0000) | LDT_SEL(idx);
                    frame->eflags &= ~1;  /* CF=0 */
                    serial_puts("DPMI: INT 21h/48h → sel ");
                    serial_puthex(LDT_SEL(idx));
                    serial_puts(" base ");
                    serial_puthex(base);
                    serial_puts("\n");
                } else {
                    frame->eflags |= 1;  /* CF=1, no LDT space */
                    frame->eax = (frame->eax & 0xFFFF0000) | 8;
                }
            } else {
                frame->eax = dregs.eax;
                frame->ebx = dregs.ebx;
                frame->eflags |= 1;
            }
            return 0;
        }

        /* INT 21h/49h (Free Memory): ES = LDT selector, translate to segment */
        if (ah == 0x49) {
            int idx = SEL_TO_IDX(frame->es & 0xFFFF);
            if (idx < DPMI_LDT_ENTRIES && LDT_USED(c, idx)) {
                uint32_t base = desc_get_base(&c->ldt[idx]);
                struct dos_regs dregs;
                dregs.eax = frame->eax;
                dregs.es = base >> 4;
                dregs.eflags = frame->eflags;
                dregs.ds = ds_base >> 4;
                dos_int21(dos_tid, &dregs);
                ldt_free(c, idx);
                frame->eflags = (frame->eflags & ~0xFF) | (dregs.eflags & 0xFF);
            }
            return 0;
        }

        /* Route other INT 21h through dos_int21 with pseudo-segments */
        {
            struct dos_regs dregs;
            dregs.eax = frame->eax;
            dregs.ebx = frame->ebx;
            dregs.ecx = frame->ecx;
            dregs.edx = frame->edx;
            dregs.esi = frame->esi;
            dregs.edi = frame->edi;
            dregs.ebp = frame->ebp;
            dregs.ds  = ds_base >> 4;
            dregs.es  = es_base >> 4;
            dregs.eflags = frame->eflags;
            dos_int21(dos_tid, &dregs);
            frame->eax = dregs.eax;
            frame->ebx = dregs.ebx;
            frame->ecx = dregs.ecx;
            frame->edx = dregs.edx;
            frame->esi = dregs.esi;
            frame->edi = dregs.edi;
            frame->eflags = (frame->eflags & ~0xFF) | (dregs.eflags & 0xFF);
            return 0;
        }
    }

    /* If client installed a PM handler for this vector, redirect to it.
     * Push IRET frame (return CS:IP + flags) on client stack.
     * Frame size depends on CLIENT's bit-ness (same rule as exceptions):
     * 32-bit client: push dword EIP, dword CS, dword EFLAGS (12 bytes)
     * 16-bit client: push word IP, word CS, word FLAGS (6 bytes)
     * (DPMI 0.9 spec: handler's CS D/B bit is irrelevant; DOS/4GW uses
     * 16-bit CS segments but expects 32-bit frames as a 32-bit client.) */
    /* Deliver only if a valid PM handler is installed.
     * - offset==0 with selector!=chain_sel is treated as "no handler" — we'd
     *   IRET the client to address 0 and crash. The chain stub for INT 21h
     *   intentionally lives at sel:0 (CD 21 CB at linear 0x508). */
    int has_handler = c && c->pm_vectors[vector].selector &&
                      (c->pm_vectors[vector].offset != 0 ||
                       c->pm_vectors[vector].selector == c->pm_int_chain_sel);
    if (!has_handler && c && c->pm_vectors[vector].selector) {
        static int rej = 0;
        if (rej < 8) {
            serial_puts("DPMI: reject deliver INT ");
            serial_puthex(vector);
            serial_puts(" — sel=");
            serial_puthex(c->pm_vectors[vector].selector);
            serial_puts(" off=0 (no handler)\n");
            rej++;
        }
    }
    if (has_handler) {
        uint32_t ss_base = desc_get_base(&c->ldt[SEL_TO_IDX(frame->ss & 0xFFFF)]);
        /* Frame width follows the CLIENT's bit-ness (DPMI 0.9 §4.5, matches
         * CWSDPMI exphdlr.c:313-320). 4GW's 16-bit handler uses 66 CF / 66 CB to do
         * 32-bit IRETD / RETF for a 32-bit client.
         * Also: mask ESP when SS.B=0 — the high 16 of ESP register is stale
         * garbage for 16-bit stacks; CPU stack access uses only SP. */
        int is_32 = c->is_32bit;
        int ss_idx = SEL_TO_IDX(frame->ss & 0xFFFF);
        int ss32 = (ss_idx < DPMI_LDT_ENTRIES &&
                    (c->ldt[ss_idx].limit_hi & 0x40)) ? 1 : 0;
        uint32_t bytes = is_32 ? 12 : 6;
        uint32_t new_esp;
        if (ss32) {
            new_esp = frame->esp - bytes;
        } else {
            uint32_t sp = (frame->esp - bytes) & 0xFFFF;
            new_esp = (frame->esp & 0xFFFF0000) | sp;
        }
        uint32_t stk_off = ss32 ? new_esp : (new_esp & 0xFFFF);
        frame->esp = new_esp;
        if (is_32) {
            uint32_t *stk32 = (uint32_t *)(ss_base + stk_off);
            stk32[0] = frame->eip;
            stk32[1] = frame->cs;
            stk32[2] = frame->eflags;
        } else {
            uint16_t *stk = (uint16_t *)(ss_base + stk_off);
            stk[0] = frame->eip & 0xFFFF;
            stk[1] = frame->cs & 0xFFFF;
            stk[2] = frame->eflags & 0xFFFF;
        }
        /* Ses32 H9 diagnostic: record this PM-INT delivery's pushed
         * (EIP, CS, EFL) tuple into the circular log. Dumped at next
         * #GP delivery, alongside session-31's INT 21h log and the
         * session-32 PM exception log. Captures everything that hits the
         * generic PM-INT path — hardware IRQs reflected to PM, INT 2F,
         * INT 0x88/etc. — but NOT INT 21h (early-returned above). */
        {
            uint32_t i = pm_irq_log_count & (PM_IRQ_LOG_N - 1);
            pm_irq_log[i].vector = vector;
            pm_irq_log[i].width  = is_32 ? 32 : 16;
            pm_irq_log[i].h_sel  = c->pm_vectors[vector].selector;
            pm_irq_log[i].eip    = frame->eip;
            pm_irq_log[i].cs     = frame->cs;
            pm_irq_log[i].eflags = frame->eflags;
            pm_irq_log[i].ss_esp =
                ((frame->ss & 0xFFFF) << 16) | (new_esp & 0xFFFF);
            pm_irq_log_count++;
        }
        frame->eip = c->pm_vectors[vector].offset;
        frame->cs  = c->pm_vectors[vector].selector;
        /* Clear TF in the handler's context — the CPU clears TF when
         * entering an interrupt via IDT, and since we do software dispatch
         * we must do it manually. Otherwise INT 1 (trace) storms occur. */
        frame->eflags &= ~0x100;  /* TF = bit 8 */
        return 0;
    }

    /* Unhandled PM INT — simulate RM reflection.
     * DPMI spec: unhandled PM software INTs are reflected to RM IVT handlers.
     * Since our IVT points to IRET stubs, reflection is effectively a no-op.
     *
     * For private vectors (0x60+), set CF=1 so a polling caller sees
     * "no service" and breaks out of its retry loop. DOS/32A's DOOM stub
     * calls INT 0x88 in a tight loop expecting eventually CF=1; clearing
     * CF (success) kept it spinning forever. Standard BIOS/DOS vectors
     * (< 0x60) keep the CF=0 contract — they're either handled above or
     * intentionally a no-op. */
    if (vector >= 0x60) {
        frame->eflags |= 1;        /* CF = 1 (no service) */
        frame->eax &= 0xFFFF0000;  /* AX = 0 */
    }

    /* INT 0x2F multiplex. DOS/4GW polls this from PM to discover host
     * features. AX=1687h is "Get DPMI PM entry point" — even though
     * we're already in PM, DOS/4GW checks anyway. Respond with DPMI
     * 0.90 host info; ES:DI=0:0 (no nested entry — we are the host). */
    if (vector == 0x2F) {
        uint16_t ax_in = frame->eax & 0xFFFF;
        if (ax_in == 0x1687) {
            /* s42 exploratory — Fix B (cont.): DX bumped 0x005A → 0x0100
             * to match the V86-side INT 2Fh/1687h and INT 31h/0400
             * version reports. See v86.c comment for full rationale. */
            frame->eax &= 0xFFFF0000;       /* AX = 0 (success) */
            frame->ebx = (frame->ebx & 0xFFFF0000) | 1;  /* BX bit 0 = 32-bit support */
            frame->ecx = (frame->ecx & 0xFFFF0000) | 5;  /* CL = 386 */
            frame->edx = (frame->edx & 0xFFFF0000) | 0x0100; /* DPMI 1.00 [s42-B] */
            frame->es  = 0;                  /* ES:DI = 0:0 (no entry; we are host) */
            frame->edi = 0;
            frame->esi = (frame->esi & 0xFFFF0000) | 0x100; /* SI = stack para */
        } else {
            /* Other 2Fh subfuncs: set AX=0 by default (claim "handled") */
            frame->eax &= 0xFFFF0000;
        }
    }

    static int unhandled_pm_count = 0;
    if (unhandled_pm_count < 20) {
        serial_puts("DPMI: unhandled PM INT ");
        serial_puthex(vector);
        serial_puts(" CS=");
        serial_puthex(frame->cs);
        serial_puts(" EIP=");
        serial_puthex(frame->eip);
        serial_puts(" AX=");
        serial_puthex(frame->eax & 0xFFFF);
        serial_puts(" BX=");
        serial_puthex(frame->ebx & 0xFFFF);
        serial_puts(" CX=");
        serial_puthex(frame->ecx & 0xFFFF);
        serial_puts(" DX=");
        serial_puthex(frame->edx & 0xFFFF);
        serial_puts("\n");
        unhandled_pm_count++;
    }
    /* INT 0x8F is DOS/4GW's vendor-private vector (research doc 37 §3).
     * First time we see it: dump pm_vectors[0x8F] state (did anyone hook
     * it via 0205h?) and 32 bytes around the loop site (16 before + 16
     * after the INT) so we can decode the loop condition. We dump per
     * call site (0x6DCD, 0x6E4D) so we see both. */
    if (vector == 0x8F && c) {
        static uint32_t dumped_eips[4] = {0,0,0,0};
        static int dumped_n = 0;
        int already = 0, i;
        for (i = 0; i < dumped_n; i++)
            if (dumped_eips[i] == frame->eip) { already = 1; break; }
        if (!already && dumped_n < 4) {
            int cidx = SEL_TO_IDX(frame->cs & 0xFFFF);
            serial_puts("DPMI: INT 0x8F at CS:EIP=");
            serial_puthex(frame->cs);
            serial_puts(":");
            serial_puthex(frame->eip);
            serial_puts(" pm_vectors[0x8F]=");
            serial_puthex(c->pm_vectors[0x8F].selector);
            serial_puts(":");
            serial_puthex(c->pm_vectors[0x8F].offset);
            serial_puts("\n");
            if (cidx < DPMI_LDT_ENTRIES && LDT_USED(c, cidx)) {
                uint32_t cs_base = desc_get_base(&c->ldt[cidx]);
                /* dump 32 bytes: EIP-18 .. EIP+13 (covers 16 before INT
                 * since INT is at EIP-2, plus 14 after). */
                uint32_t off = frame->eip;
                uint32_t start = (off >= 18) ? off - 18 : 0;
                uint8_t *code = (uint8_t *)(cs_base + start);
                serial_puts("  bytes [");
                serial_puthex(start);
                serial_puts("]:");
                for (i = 0; i < 32; i++) {
                    serial_puts(" ");
                    serial_puthex(code[i]);
                }
                serial_puts("\n");
            }
            dumped_eips[dumped_n++] = frame->eip;
        }
    }

    return 0;
}

/* ================================================================
 * Synchronous RM call trampoline (INT 31h AX=0x0301/0x0302)
 *
 * Trampoline + sentinel pattern (docs/research/29-dpmi-host.md):
 *   1. PM client calls INT 31h with ES:EDI -> RmCallStruct
 *   2. dpmi_int31 0x0301/0x0302 stashes the buffer addr + return kind
 *      in client->rm_call_save, returns code 2
 *   3. dpmi_handle_pm_int sees code 2, calls dpmi_rm_call_setup_isr
 *      below to perform the actual frame switch
 *   4. We save ALL PM frame state into rm_call_save, then rewrite the
 *      frame so IRET (back from the IDT entry) lands in V86 mode at
 *      the RM target with regs from the buffer
 *   5. Trampoline IRET frame is pushed onto the V86 stack pointing
 *      at our sentinel (CD F4 CF at linear 0x50C)
 *   6. RM proc runs in V86, eventually IRETs/RETFs into the sentinel
 *   7. V86 monitor traps INT 0xF4, calls dpmi_rm_call_unwind below
 *   8. We write V86 regs into the RmCallStruct and restore PM frame
 * ================================================================ */

/* One-time setup of the sentinel stub bytes in low memory. */
void dpmi_install_sentinel(void) {
    uint8_t *s = (uint8_t *)DPMI_SENTINEL_LIN;
    s[0] = 0xCD;                 /* INT */
    s[1] = DPMI_SENTINEL_VEC;    /* 0xF4 */
    s[2] = 0xCF;                 /* IRET (defensive — should never run) */
}

/* PM→V86 switch for 0x0301/0x0302. Called from dpmi_handle_pm_int. */
int dpmi_rm_call_setup_isr(int client_id, struct isr_frame *frame) {
    struct dpmi_client *c = dpmi_get_client(client_id);
    if (!c) return 0;

    struct dpmi_regs *rm = (struct dpmi_regs *)c->rm_call_save.rm_struct_linear;

    /* Save full PM frame */
    c->rm_call_save.pm_eax = frame->eax;
    c->rm_call_save.pm_ebx = frame->ebx;
    c->rm_call_save.pm_ecx = frame->ecx;
    c->rm_call_save.pm_edx = frame->edx;
    c->rm_call_save.pm_esi = frame->esi;
    c->rm_call_save.pm_edi = frame->edi;
    c->rm_call_save.pm_ebp = frame->ebp;
    c->rm_call_save.pm_eip = frame->eip;
    c->rm_call_save.pm_cs  = frame->cs;
    c->rm_call_save.pm_eflags = frame->eflags;
    c->rm_call_save.pm_esp = frame->esp;
    c->rm_call_save.pm_ss  = frame->ss;
    c->rm_call_save.pm_ds  = frame->ds;
    c->rm_call_save.pm_es  = frame->es;
    c->rm_call_save.pm_fs  = frame->fs;
    c->rm_call_save.pm_gs  = frame->gs;

    /* Set up V86 stack from RmCallStruct (or default RM TOS if zero). */
    uint16_t rm_ss = rm->ss ? rm->ss : 0x0070;
    uint16_t rm_sp = rm->ss ? rm->sp : 0x0F00;

    /* Push trampoline frame on V86 stack. Always include FLAGS even for
     * 0x0301 — the FAR-return target ignores the extra word, but our
     * sentinel uses IRET so any padding is harmless because IRET will
     * consume FLAGS anyway. Simpler logic, same effect. */
    uint8_t *stk = (uint8_t *)((uint32_t)rm_ss << 4);
    if (c->rm_call_save.is_iret) {
        /* IRET frame: FLAGS, CS, IP (3 words, 6 bytes total) */
        rm_sp = (rm_sp - 2) & 0xFFFF;
        *(uint16_t *)(stk + rm_sp) = rm->flags ? rm->flags : 0x0202;
        rm_sp = (rm_sp - 2) & 0xFFFF;
        *(uint16_t *)(stk + rm_sp) = DPMI_SENTINEL_SEG;
        rm_sp = (rm_sp - 2) & 0xFFFF;
        *(uint16_t *)(stk + rm_sp) = DPMI_SENTINEL_OFF;
    } else {
        /* FAR return frame: CS, IP (2 words, 4 bytes) */
        rm_sp = (rm_sp - 2) & 0xFFFF;
        *(uint16_t *)(stk + rm_sp) = DPMI_SENTINEL_SEG;
        rm_sp = (rm_sp - 2) & 0xFFFF;
        *(uint16_t *)(stk + rm_sp) = DPMI_SENTINEL_OFF;
    }

    /* Switch frame to V86 mode at RM target. */
    frame->cs    = rm->cs;
    frame->eip   = rm->ip;
    frame->ss    = rm_ss;
    frame->esp   = rm_sp;
    /* Stub slots (offsets 0..12) are popped into PM seg registers at CPL=0
     * by isr_common *before* IRET. Loading a V86 segment value here (e.g.
     * 4GW's DOS buffer segment 0x22B5) faults — the CPU treats it as a PM
     * selector → LDT[0x456] null → #GP. Use safe values; IRET-to-VM=1
     * reloads the real V86 segs from the v86_es/ds/fs/gs slots above SS. */
    frame->ds = 0x10;  /* kernel data */
    frame->es = 0x10;
    frame->fs = 0;
    frame->gs = 0;
    {
        uint32_t *v86_segs = (uint32_t *)&frame->ss + 1;
        v86_segs[0] = rm->es;   /* v86_es */
        v86_segs[1] = rm->ds;   /* v86_ds */
        v86_segs[2] = rm->fs;   /* v86_fs */
        v86_segs[3] = rm->gs;   /* v86_gs */
    }

    /* GP regs from RmCallStruct */
    frame->eax = rm->eax;
    frame->ebx = rm->ebx;
    frame->ecx = rm->ecx;
    frame->edx = rm->edx;
    frame->esi = rm->esi;
    frame->edi = rm->edi;
    frame->ebp = rm->ebp;

    /* EFLAGS: VM=1, IF=1, IOPL=0 (real, but virtualized as 3 to V86) */
    frame->eflags = 0x00020002u                  /* VM=1, reserved bit 1 */
                  | ((uint32_t)rm->flags & 0xCD5) /* CF/PF/AF/ZF/SF/DF/OF from buf */
                  | 0x200;                        /* IF=1 */

    c->rm_call_save.active = 1;

    serial_puts("DPMI: RM-trampoline switch → ");
    serial_puthex(rm->cs);
    serial_puts(":");
    serial_puthex(rm->ip);
    serial_puts(" SS:SP=");
    serial_puthex(rm_ss);
    serial_puts(":");
    serial_puthex(rm_sp);
    serial_puts("\n");

    /* H6 diagnostic: log the PM state we just stashed in c->rm_call_save.
     * Compared against the PM-restore log in dpmi_rm_call_unwind, this
     * pins down whether the 0x0302 round-trip corrupts client state. */
    serial_puts("DPMI: PM-save CS:EIP=");
    serial_puthex(c->rm_call_save.pm_cs);
    serial_puts(":");
    serial_puthex(c->rm_call_save.pm_eip);
    serial_puts(" SS:ESP=");
    serial_puthex(c->rm_call_save.pm_ss);
    serial_puts(":");
    serial_puthex(c->rm_call_save.pm_esp);
    serial_puts(" EFL=");
    serial_puthex(c->rm_call_save.pm_eflags);
    /* Ses36 ES-leak hunt: log the ES/DS we just snapshotted. The pair
     * (PM-save ES, PM-restore ES) must be byte-identical across the V86
     * round-trip — any divergence proves V86 ES is leaking into PM. */
    serial_puts(" ES=");
    serial_puthex(c->rm_call_save.pm_es);
    serial_puts(" DS=");
    serial_puthex(c->rm_call_save.pm_ds);
    serial_puts("\n");

    return 0;  /* dispatcher already wrote regs to frame; we've overridden them */
}

/* V86→PM unwind. Called from v86_emulate_int(0xF4). */
void dpmi_rm_call_unwind(struct v86_frame *frame) {
    int ci;
    struct dpmi_client *c = 0;
    for (ci = 0; ci < DPMI_MAX_CLIENTS; ci++) {
        if (clients[ci].active && clients[ci].rm_call_save.active) {
            c = &clients[ci];
            break;
        }
    }
    if (!c) {
        serial_puts("V86: INT F4 sentinel with no pending RM call\n");
        return;
    }

    struct dpmi_regs *rm = (struct dpmi_regs *)c->rm_call_save.rm_struct_linear;

    /* Write post-call V86 state to caller's RmCallStruct */
    rm->eax = frame->eax;
    rm->ebx = frame->ebx;
    rm->ecx = frame->ecx;
    rm->edx = frame->edx;
    rm->esi = frame->esi;
    rm->edi = frame->edi;
    rm->ebp = frame->ebp;
    rm->ds  = frame->v86_ds;
    rm->es  = frame->v86_es;
    rm->fs  = frame->v86_fs;
    rm->gs  = frame->v86_gs;
    rm->cs  = frame->cs & 0xFFFF;
    rm->ip  = frame->eip & 0xFFFF;
    rm->ss  = frame->ss & 0xFFFF;
    rm->sp  = frame->esp & 0xFFFF;
    rm->flags = frame->eflags & 0xFFFF;

    /* Restore PM frame */
    frame->eax = c->rm_call_save.pm_eax;
    frame->ebx = c->rm_call_save.pm_ebx;
    frame->ecx = c->rm_call_save.pm_ecx;
    frame->edx = c->rm_call_save.pm_edx;
    frame->esi = c->rm_call_save.pm_esi;
    frame->edi = c->rm_call_save.pm_edi;
    frame->ebp = c->rm_call_save.pm_ebp;
    frame->eip = c->rm_call_save.pm_eip;
    frame->cs  = c->rm_call_save.pm_cs;
    frame->eflags = c->rm_call_save.pm_eflags;
    frame->esp = c->rm_call_save.pm_esp;
    frame->ss  = c->rm_call_save.pm_ss;
    frame->ds_stub = c->rm_call_save.pm_ds;
    frame->es_stub = c->rm_call_save.pm_es;
    frame->fs  = c->rm_call_save.pm_fs;
    frame->gs  = c->rm_call_save.pm_gs;

    /* PM CF reflects the RM call outcome (RM flags bit 0). */
    if (rm->flags & 1) frame->eflags |=  1;
    else               frame->eflags &= ~1u;

    c->rm_call_save.active = 0;

    serial_puts("DPMI: RM-trampoline unwind, PM resume at ");
    serial_puthex(frame->cs);
    serial_puts(":");
    serial_puthex(frame->eip);
    serial_puts(" RM-AX=");
    serial_puthex(rm->eax & 0xFFFF);
    serial_puts("\n");

    /* H6 diagnostic: log the PM state we just restored from c->rm_call_save
     * plus 8 dwords from the resumed PM stack. Diff against the PM-save log
     * in dpmi_rm_call_setup_isr — they should be identical. A divergence in
     * the register fields means rm_call_save was scribbled mid-call; a
     * divergence in the stack-contents (vs. expectations) means V86 code
     * stomped the PM stack via aliased linear addressing. */
    serial_puts("DPMI: PM-restore CS:EIP=");
    serial_puthex(frame->cs);
    serial_puts(":");
    serial_puthex(frame->eip);
    serial_puts(" SS:ESP=");
    serial_puthex(frame->ss);
    serial_puts(":");
    serial_puthex(frame->esp);
    serial_puts(" EFL=");
    serial_puthex(frame->eflags);
    serial_puts(" ES=");
    serial_puthex(frame->es_stub);
    serial_puts(" DS=");
    serial_puthex(frame->ds_stub);
    {
        int idx = SEL_TO_IDX(frame->ss & 0xFFFF);
        if (idx > 0 && idx < DPMI_LDT_ENTRIES && LDT_USED(c, idx)) {
            uint32_t ss_base = desc_get_base(&c->ldt[idx]);
            uint32_t lin = ss_base + frame->esp;
            serial_puts("\n  PM-stk:");
            for (int i = 0; i < 8; i++) {
                uint32_t a = lin + i * 4;
                serial_puts(" ");
                /* Guard: skip dwords on pages not currently mapped. The PM
                 * client owns these pages via its LDT segment, but if a
                 * given 4 KB has never been touched it may not have a
                 * kernel PTE yet — a raw deref here #PFs the kernel. */
                if (vmm_get_physical(a & ~0xFFFu) == 0)
                    serial_puts("--------");
                else
                    serial_puthex(*(uint32_t *)a);
            }
        }
    }
    serial_puts("\n");

    /* Ses33 H6-broad diagnostic: fixed-corridor dump at LDT[0x11F].base + 0x6500
     * (16 dwords = 64 bytes), regardless of current SS:ESP. The corridor
     * brackets the eventual fault frame at offset +0x28 (= linear 0x6528),
     * which is dword index 10 in this window. If the dword at +0x28 reads
     * clean at unwind N and (0, ?, EFL) at unwind N+1, the writer ran
     * during the V86 work between those unwinds. Tagged with a static
     * unwind-index so successive snapshots diff cleanly by eye. */
    {
        static uint32_t corridor_idx = 0;
        int idx11F = SEL_TO_IDX(0x11F);
        if (idx11F < DPMI_LDT_ENTRIES && LDT_USED(c, idx11F)) {
            uint32_t base11F = desc_get_base(&c->ldt[idx11F]);
            uint32_t corridor = base11F + 0x6500;
            serial_puts("DPMI: corridor[");
            serial_puthex(corridor_idx);
            serial_puts("] 0x11F:0x6500=");
            serial_puthex(corridor);
            serial_puts("\n  ");
            for (int i = 0; i < 16; i++) {
                uint32_t a = corridor + i * 4;
                /* Mark the fault dword (offset +0x28, i.e. index 10) so the
                 * reader doesn't have to count every time. */
                if (i == 10) serial_puts("[6528]=");
                if (vmm_get_physical(a & ~0xFFFu) == 0)
                    serial_puts("--------");
                else
                    serial_puthex(*(uint32_t *)a);
                serial_puts(" ");
                /* Wrap output every 8 dwords for readability. */
                if (i == 7) serial_puts("\n  ");
            }
            serial_puts("\n");
        } else {
            serial_puts("DPMI: corridor[");
            serial_puthex(corridor_idx);
            serial_puts("] sel=0x11F not in LDT — skipping\n");
        }
        corridor_idx++;
    }
}
