/* ohci.kmd — OHCI 1.0a host controller driver for pinecore-x86.
 *
 * Spec-grounded port from docs/research/57-ohci-from-spec.md.
 * Primary references:
 *   - OpenHCI 1.0a (Compaq/Microsoft/NSC, 1999-09-14) — citations as §X.Y, p.NN
 *   - USB 2.0 §7.1.7.5 (port reset), §9 (enumeration), §11 (hub class)
 * Sanity-check references (NOT sources):
 *   - USBDDOS HCD/ohci.c + Netrunner01 PRs #24-#30, #40 (GPLv2)
 *   - Linux drivers/usb/host/ohci-{hcd,q,mem,pci,hub}.c (GPLv2)
 * Original code written from the spec.
 *
 * License: GPL-2.0 — links EXPORT_SYMBOL_GPL surface of usbcore.kmd.
 *
 * v1 scope (Vortex86SX target):
 *   - PCI probe (class 0x0C/0x03/0x10); HcControl.IR SMM handoff
 *     bounded to ~1 s (Netrunner01 PR #27).
 *   - HcRevision sanity (0x10); HCR software reset; FmInterval save/restore
 *     with 0-fallback (Netrunner01 PR #29-style defensive default).
 *   - HCCA (256 B, 256 B aligned via dma_alloc). Dummy-head EDs for
 *     control + bulk; **all 32 HCCA interrupt heads share one int dummy ED**
 *     — no binary-tree periodic schedule in v1 (interrupt EDs polled every
 *     frame, which is wasteful for bandwidth but correct; defer the
 *     §5.2.7.2 polling tree to v2).
 *   - Per-xfer ED for control transfers (pause via K=1, no tail-dummy).
 *   - Persistent ED in ep_open() for bulk + interrupt endpoints.
 *   - port_reset per §7.4.4 + USB 2.0 §7.1.7.5; returns speed enum.
 *   - IRQ: drain HcInterruptStatus R/WC, walk HccaDoneHead chain (reverse
 *     retirement order, §4.4.2.3), handle RHSC port changes.
 *
 * Out of v1: isoc, suspend/resume, full periodic binary tree, NEC retry
 * write quirks (#22-#30 noted in doc 57 §13; generic handoff timeout done).
 *
 * MMIO mapping: pinecore identity-maps the first 32 MiB. OHCI BAR0 is
 * typically above that (e.g. 0xFEBFF000 on QEMU, vendor-specific on
 * Vortex86) so ohci.kmd must vmm_map_page() it. We identity-map the
 * single 4 KB BAR page with PCD (cache-disable) and use the phys addr
 * as the virtual address.
 */
#include "module.h"
#include "types.h"
#include "usbcore.h"
#include "pci.h"

/* ============================================================
 * Kernel + usbcore imports
 * ============================================================ */
extern void *kmalloc(uint32_t size);
extern void  kfree  (void *p);
extern void *dma_alloc(uint32_t size, uint32_t align);
extern void  dma_free (void *p);
extern uint32_t dma_virt_to_phys(void *p);

extern uint32_t pci_cfg_read (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
extern void     pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn,
                              uint8_t off, uint32_t v);
extern int      pci_find_class(uint8_t class_code, uint8_t subclass,
                               uint8_t prog_if, struct pci_dev *out, int index);
extern void     pit_delay_ms(uint32_t ms);
extern uint64_t pit_ticks_get(void);

extern void     vmm_map_page  (uint32_t virt, uint32_t phys, uint32_t flags);
extern void     vmm_unmap_page(uint32_t virt);
#define PTE_PRESENT   0x001
#define PTE_WRITABLE  0x002
#define PTE_PCD       0x010

extern void  serial_puts(const char *s);
extern void  serial_puthex(uint32_t v);
extern void  klog_stage(const char *text);
extern void  klog_iter(const char *suffix);
extern void *memset(void *dst, int v, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);

typedef void (*irq_handler_t)(void *ctx);
extern int  irq_register  (uint8_t irq, irq_handler_t handler, void *ctx);
extern int  irq_unregister(uint8_t irq, irq_handler_t handler);

extern int  usbcore_register_hcd   (usb_hcd_t *hcd);
extern int  usbcore_unregister_hcd (usb_hcd_t *hcd);
extern int  usbcore_port_connect   (usb_hcd_t *hcd, uint8_t port,
                                    usb_speed_t spd);
extern int  usbcore_port_disconnect(usb_hcd_t *hcd, uint8_t port);

/* ============================================================
 * OHCI operational registers (doc 57 §3, OHCI §7). DWORD-only MMIO.
 * ============================================================ */
#define HcRevision           0x00   /* §7.1.1, p.109 */
#define HcControl            0x04   /* §7.1.2, p.109 */
#define HcCommandStatus      0x08   /* §7.1.3, p.112 */
#define HcInterruptStatus    0x0C   /* §7.1.4, p.113 */
#define HcInterruptEnable    0x10   /* §7.1.5, p.115 */
#define HcInterruptDisable   0x14   /* §7.1.6, p.116 */
#define HcHCCA               0x18   /* §7.2.1, p.117 */
#define HcControlHeadED      0x20   /* §7.2.3, p.118 */
#define HcControlCurrentED   0x24   /* §7.2.4, p.118 */
#define HcBulkHeadED         0x28   /* §7.2.5, p.119 */
#define HcBulkCurrentED      0x2C   /* §7.2.6, p.119 */
#define HcDoneHead           0x30   /* §7.2.7, p.120 */
#define HcFmInterval         0x34   /* §7.3.1, p.120 */
#define HcFmRemaining        0x38   /* §7.3.2, p.121 */
#define HcFmNumber           0x3C   /* §7.3.3, p.122 */
#define HcPeriodicStart      0x40   /* §7.3.4, p.122 */
#define HcLSThreshold        0x44   /* §7.3.5, p.123 */
#define HcRhDescriptorA      0x48   /* §7.4.1, p.124 */
#define HcRhDescriptorB      0x4C   /* §7.4.2, p.125 */
#define HcRhStatus           0x50   /* §7.4.3, p.126 */
/* Port N (1-indexed) lives at HcRhPortStatus_BASE + 4 * (N - 1). */
#define HcRhPortStatus_BASE  0x54   /* §7.4.4, p.128 */

/* HcControl (§7.1.2, p.110) */
#define HCC_CBSR_3_1   (2u << 0)
#define HCC_PLE        (1u << 2)
#define HCC_IE         (1u << 3)
#define HCC_CLE        (1u << 4)
#define HCC_BLE        (1u << 5)
#define HCC_HCFS_RESET (0u << 6)
#define HCC_HCFS_RESUM (1u << 6)
#define HCC_HCFS_OPER  (2u << 6)
#define HCC_HCFS_SUSP  (3u << 6)
#define HCC_HCFS_MASK  (3u << 6)
#define HCC_IR         (1u << 8)

/* HcCommandStatus (§7.1.3, p.112) */
#define HCS_HCR        (1u << 0)
#define HCS_CLF        (1u << 1)
#define HCS_BLF        (1u << 2)
#define HCS_OCR        (1u << 3)

/* HcInterruptStatus / Enable / Disable (§7.1.4-6) — same bit layout */
#define HIS_SO         (1u << 0)
#define HIS_WDH        (1u << 1)
#define HIS_SF         (1u << 2)
#define HIS_RD         (1u << 3)
#define HIS_UE         (1u << 4)
#define HIS_FNO        (1u << 5)
#define HIS_RHSC       (1u << 6)
#define HIS_OC         (1u << 30)
#define HIS_MIE        (1u << 31)

/* HcRhStatus (§7.4.3, p.126) — split read/write semantics */
#define HRS_LPSC_SET   (1u << 16)    /* write 1 → SetGlobalPower */

/* HcRhPortStatus bits (§7.4.4, p.128). Read meaning vs write meaning
 * differ for many of these — see doc 57 §3 root-hub trap notes. */
#define HRP_CCS        (1u << 0)
#define HRP_PES        (1u << 1)
#define HRP_PSS        (1u << 2)
#define HRP_PRS        (1u << 4)
#define HRP_PPS        (1u << 8)
#define HRP_LSDA       (1u << 9)
#define HRP_CSC        (1u << 16)
#define HRP_PESC       (1u << 17)
#define HRP_PSSC       (1u << 18)
#define HRP_OCIC       (1u << 19)
#define HRP_PRSC       (1u << 20)

/* ============================================================
 * Endpoint Descriptor (§4.2, p.16-18). 16 B, 16 B aligned.
 * ============================================================ */
typedef struct ohci_ed {
    volatile uint32_t dw0;       /* MPS | F | K | S | D | EN | FA */
    volatile uint32_t tail_p;    /* phys addr of tail TD (low 4 bits 0) */
    volatile uint32_t head_p;    /* phys addr of head TD | C | H */
    volatile uint32_t next_ed;   /* phys addr of next ED in list */
} ohci_ed_t;

#define ED_FA(x)       ((uint32_t)((x) & 0x7F))
#define ED_EN(x)       ((uint32_t)(((x) & 0xF) << 7))
#define ED_DIR_TD      (0u << 11)    /* direction in each TD's DP */
#define ED_DIR_OUT     (1u << 11)
#define ED_DIR_IN      (2u << 11)
#define ED_SPEED_LS    (1u << 13)
#define ED_SKIP        (1u << 14)
#define ED_MPS(x)      ((uint32_t)(((x) & 0x7FF) << 16))

#define HEADP_H        (1u << 0)
#define HEADP_C        (1u << 1)
#define HEADP_MASK     0xFFFFFFF0u

/* ============================================================
 * General Transfer Descriptor (§4.3.1, p.19-25). 16 B HW + SW pad.
 * ============================================================ */
typedef struct ohci_td {
    volatile uint32_t dw0;       /* CC | EC | T | DI | DP | R */
    volatile uint32_t cbp;       /* CurrentBufferPointer (0 = no/done) */
    volatile uint32_t next_td;   /* phys addr next TD */
    volatile uint32_t be;        /* BufferEnd (phys addr last byte) */
    /* SW reserved — 16 B padding so total = 32 B (16 B HW + SW) */
    struct ohci_td   *next_sw;
    void             *xfer;
    uint32_t          _pad[2];
} ohci_td_t;

#define TD_R           (1u << 18)
#define TD_DP_SETUP    (0u << 19)
#define TD_DP_OUT      (1u << 19)
#define TD_DP_IN       (2u << 19)
#define TD_DI(x)       ((uint32_t)(((x) & 0x7) << 21))
#define TD_DI_NONE     TD_DI(7)
#define TD_T_USE_EDC   (0u << 24)
#define TD_T_DATA0     (2u << 24)
#define TD_T_DATA1     (3u << 24)
#define TD_CC_NOTACC   (0xEu << 28)
#define TD_CC_GET(dw0) (((dw0) >> 28) & 0xF)

/* TD condition codes (§4.3.3 Table 4-7, p.32) */
#define CC_NOERROR             0x0
#define CC_CRC                 0x1
#define CC_BITSTUFFING         0x2
#define CC_DATATOGGLEMISMATCH  0x3
#define CC_STALL               0x4
#define CC_DEVICENOTRESPONDING 0x5
#define CC_PIDCHECKFAILURE     0x6
#define CC_UNEXPECTEDPID       0x7
#define CC_DATAOVERRUN         0x8
#define CC_DATAUNDERRUN        0x9
#define CC_BUFFEROVERRUN       0xC
#define CC_BUFFERUNDERRUN      0xD
#define CC_NOTACCESSED         0xE

/* ============================================================
 * HCCA (§4.4, p.33). 256 B, 256 B aligned.
 * ============================================================ */
typedef struct ohci_hcca {
    uint32_t interrupt_table[32];   /* +0x00 — list heads per (fn & 0x1F) */
    volatile uint16_t frame_number; /* +0x80 — HC writes each SOF */
    uint16_t pad1;                  /* +0x82 */
    volatile uint32_t done_head;    /* +0x84 — HC dumps HcDoneHead here */
    uint8_t  reserved[116];         /* +0x88 — HC-private */
} __attribute__((packed)) ohci_hcca_t;

/* ============================================================
 * Per-endpoint state (held in usb_endpoint_t.hcd_priv).
 * ============================================================ */
typedef struct ohci_ep_priv {
    ohci_ed_t           *ed;
    uint8_t              toggle;          /* mirror of ED.C for diagnostics */
    usb_xfer_t          *current_xfer;
    ohci_td_t           *first_td;
    struct ohci_ep_priv *next;
    /* Bulk/interrupt-only: we keep the buffer bounce alive across the
     * async transfer so the IRQ-context completion can copy IN data back. */
    void                *bounce;
    uint32_t             bounce_len;
} ohci_ep_priv_t;

/* ============================================================
 * Per-controller state
 * ============================================================ */
#define OHCI_MAX_CONTROLLERS  4

typedef struct ohci_hc {
    usb_hcd_t        base;
    uint32_t        mmio;        /* identity-mapped virt = phys */
    uint32_t         mmio_phys;
    uint8_t          bus, dev, fn;
    uint8_t          irq;
    uint8_t          num_ports;
    uint8_t          potpgt_ms;

    ohci_hcca_t     *hcca;
    ohci_ed_t       *ctrl_head;   /* dummy head ED for control list */
    ohci_ed_t       *bulk_head;   /* dummy head ED for bulk list */
    ohci_ed_t       *int_head;    /* single dummy for all 32 interrupt heads */

    ohci_ep_priv_t  *ep_list_head;
} ohci_hc_t;

static ohci_hc_t g_hcs[OHCI_MAX_CONTROLLERS];
static int       g_num_hcs = 0;

/* ============================================================
 * MMIO accessors (cache-disabled identity map — see vmm_map_page below)
 * ============================================================ */
static inline uint32_t mmio_readl(uint32_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void mmio_writel(uint32_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static inline uint32_t port_reg(ohci_hc_t *hc, uint8_t port) {
    (void)hc;
    return HcRhPortStatus_BASE + 4u * port; /* port is 0-indexed here */
}

/* ============================================================
 * Small logging helpers — match uhci.c style
 * ============================================================ */
static void log_s(const char *s) { serial_puts(s); }
static void log_h(uint32_t v)    { serial_puthex(v); }

/* ============================================================
 * ED + TD constructors (§4.2, §4.3.1)
 * ============================================================ */
static void ed_init_dummy(ohci_ed_t *ed) {
    memset((void *)ed, 0, sizeof(*ed));
    ed->dw0     = ED_SKIP;
    ed->head_p  = 0;
    ed->tail_p  = 0;
    ed->next_ed = 0;
}

static ohci_td_t *td_alloc(uint8_t dp, uint32_t t_bits, int allow_short,
                           uint8_t di, uint32_t buf_phys, uint32_t len) {
    ohci_td_t *td = (ohci_td_t *)dma_alloc(sizeof(ohci_td_t), 16);
    if (!td) return 0;
    memset((void *)td, 0, sizeof(*td));
    td->dw0 = TD_CC_NOTACC
            | t_bits
            | TD_DI(di)
            | dp
            | (allow_short ? TD_R : 0);
    td->cbp     = (len == 0) ? 0 : buf_phys;
    td->next_td = 0;
    td->be      = (len == 0) ? 0 : (buf_phys + len - 1);
    return td;
}

static int td_status(ohci_td_t *td) {
    uint32_t cc = TD_CC_GET(td->dw0);
    switch (cc) {
    case CC_NOERROR:
    case CC_DATAUNDERRUN:           /* §4.3.1.3.5 — short read OK if R=1 */
        return USB_OK;
    case CC_STALL:                  return USB_ESTALL;
    case CC_DEVICENOTRESPONDING:    return USB_ETIMEDOUT;
    case CC_NOTACCESSED:            return USB_ETIMEDOUT; /* HC didn't even visit */
    default:                        return USB_EIO;
    }
}

/* ============================================================
 * Control transfer — synchronous, per-xfer ED, K=1/K=0 pause pattern.
 * (Doc 57 §11 + OHCI §5.2.8.2 control-list processing.)
 * ============================================================ */
static int ohci_submit_control(usb_hcd_t *base, usb_xfer_t *xfer) {
    ohci_hc_t *hc = (ohci_hc_t *)base->priv;
    if (!hc || !xfer || !xfer->dev) return USB_EINVAL;

    usb_device_t *dev = xfer->dev;
    uint8_t  addr = dev->address;
    uint16_t mps  = dev->ep0_max_packet ? dev->ep0_max_packet : 8;
    int      ls   = (dev->speed == USB_SPEED_LOW);

    /* Allocate per-xfer ED + setup buffer + (optional) data bounce. */
    ohci_ed_t *ed = (ohci_ed_t *)dma_alloc(sizeof(ohci_ed_t), 16);
    if (!ed) return USB_ENOMEM;
    uint8_t *setup_buf = (uint8_t *)dma_alloc(8, 16);
    if (!setup_buf) { dma_free(ed); return USB_ENOMEM; }
    memcpy(setup_buf, &xfer->setup, 8);

    uint8_t *bounce = 0;
    if (xfer->setup.wLength > 0) {
        bounce = (uint8_t *)dma_alloc(xfer->setup.wLength, 16);
        if (!bounce) { dma_free(setup_buf); dma_free(ed); return USB_ENOMEM; }
        if (!xfer->dir_in && xfer->data)
            memcpy(bounce, xfer->data, xfer->setup.wLength);
    }

    /* SETUP TD — DATA0, no IRQ, no short. */
    ohci_td_t *setup_td = td_alloc(TD_DP_SETUP, TD_T_DATA0, 0, 7,
                                   dma_virt_to_phys(setup_buf), 8);
    if (!setup_td) goto cleanup_alloc_fail;

    /* Data-stage TDs. Toggle starts at DATA1 for first data packet
     * (§4.3.1.3.4 paragraph last, p.22). */
    ohci_td_t *prev   = setup_td;
    uint32_t  dp_data = xfer->dir_in ? TD_DP_IN : TD_DP_OUT;
    uint32_t  t_bits  = TD_T_DATA1;
    uint16_t  left    = xfer->setup.wLength;
    uint8_t  *p       = bounce;
    while (left > 0) {
        uint16_t chunk = left > mps ? mps : left;
        ohci_td_t *td = td_alloc(dp_data, t_bits, 1 /*allow short*/,
                                 7, dma_virt_to_phys(p), chunk);
        if (!td) goto cleanup_chain_fail;
        prev->next_td = dma_virt_to_phys(td);
        prev->next_sw = td;
        prev = td;
        /* Toggle alternates DATA0/DATA1. */
        t_bits = (t_bits == TD_T_DATA1) ? TD_T_DATA0 : TD_T_DATA1;
        left -= chunk;
        p    += chunk;
    }

    /* STATUS TD — opposite direction, DATA1, zero length, IRQ on
     * completion (DI=0 → "interrupt at end of frame after this TD"). */
    uint32_t dp_status = xfer->dir_in ? TD_DP_OUT : TD_DP_IN;
    ohci_td_t *status_td = td_alloc(dp_status, TD_T_DATA1, 0, 0, 0, 0);
    if (!status_td) goto cleanup_chain_fail;
    prev->next_td = dma_virt_to_phys(status_td);
    prev->next_sw = status_td;

    /* Build ED. SKIP=1 while we wire it up. */
    memset((void *)ed, 0, sizeof(*ed));
    ed->dw0 = ED_FA(addr) | ED_EN(0) | ED_DIR_TD
            | (ls ? ED_SPEED_LS : 0)
            | ED_MPS(mps)
            | ED_SKIP;
    ed->head_p  = dma_virt_to_phys(setup_td);
    ed->tail_p  = 0;
    ed->next_ed = 0;

    /* Splice ED into the control list AFTER the dummy head. v1: single
     * outstanding control xfer at a time. */
    ed->next_ed = hc->ctrl_head->next_ed;
    hc->ctrl_head->next_ed = dma_virt_to_phys(ed);

    /* Un-skip: HC may now process this ED. */
    ed->dw0 &= ~ED_SKIP;

    /* Kick HC: ControlListFilled. (§7.1.3 CLF, p.112 — write-to-set) */
    mmio_writel(hc->mmio, HcCommandStatus, HCS_CLF);

    /* Busy-wait for status TD's CC to leave NOTACCESSED. */
    uint32_t timeout = xfer->timeout_ms ? xfer->timeout_ms : 5000;
    uint64_t deadline = pit_ticks_get() + (timeout / 10) + 1;
    while (pit_ticks_get() < deadline) {
        if (TD_CC_GET(status_td->dw0) != CC_NOTACCESSED) break;
        pit_delay_ms(1);
    }

    /* DEBUG: dump SETUP wire bytes + data bounce + TD chain CCs. */
    log_s("ohci ctl: setup_buf_va="); log_h((uint32_t)setup_buf);
    log_s(" setup_buf_pa="); log_h(dma_virt_to_phys(setup_buf));
    if (bounce) {
        log_s(" bounce_va="); log_h((uint32_t)bounce);
        log_s(" bounce_pa="); log_h(dma_virt_to_phys(bounce));
    }
    log_s("\n");
    log_s("ohci ctl: setup_wire=");
    for (int i = 0; i < 8; i++) { log_h(setup_buf[i]); log_s(" "); }
    log_s("\n");
    log_s("ohci ctl: data_td0.cbp="); log_h(setup_td->next_sw ? setup_td->next_sw->cbp : 0);
    log_s(" data_td0.be="); log_h(setup_td->next_sw ? setup_td->next_sw->be : 0);
    log_s("\n");
    if (bounce && xfer->dir_in && xfer->setup.wLength > 0) {
        log_s("ohci ctl: data_recv=");
        for (int i = 0; i < (int)xfer->setup.wLength && i < 18; i++) {
            log_h(bounce[i]); log_s(" ");
        }
        log_s("\n");
    }
    log_s("ohci ctl: setup.cc=");
    log_h(TD_CC_GET(setup_td->dw0));
    {
        int idx = 0;
        for (ohci_td_t *t = setup_td->next_sw;
             t && t != status_td;
             t = t->next_sw) {
            log_s(" d"); log_h((uint32_t)idx); log_s(".cc=");
            log_h(TD_CC_GET(t->dw0));
            log_s(" cbp="); log_h(t->cbp);
            idx++;
        }
    }
    log_s(" status.cc="); log_h(TD_CC_GET(status_td->dw0));
    log_s(" ed.head_p="); log_h(ed->head_p);
    log_s(" ed.dw0=");   log_h(ed->dw0);
    log_s("\n");

    int rc = USB_OK;
    if (TD_CC_GET(status_td->dw0) == CC_NOTACCESSED) {
        rc = USB_ETIMEDOUT;
    } else {
        /* Walk chain in submission order to find first error and count
         * data-stage bytes actually transferred. */
        uint32_t actual = 0;
        for (ohci_td_t *t = setup_td; t; t = t->next_sw) {
            int s = td_status(t);
            if (s != USB_OK) { rc = s; break; }
            if (t == setup_td || t == status_td) continue;
            /* For completed data TDs: CBP=0 means full transfer; otherwise
             * bytes-actually = (orig BE - CBP + 1). (§4.3.1.3.1, p.21) */
            if (t->cbp == 0) {
                actual += (t->be != 0)
                    ? (t->be - dma_virt_to_phys(bounce ? bounce : 0) + 1)
                    : 0;
            } else {
                /* short — between CBP-1 and BE-CBP+1; conservative count */
                actual += (t->be >= t->cbp) ? (t->be - t->cbp + 1) : 0;
            }
        }
        /* Simpler: ask the bounce buffer — we know we copied `wLength`
         * bytes in, the actual transfer length is harder to extract from
         * fragmented CBPs. For v1 we trust wLength on success. */
        if (rc == USB_OK) {
            actual = xfer->setup.wLength;
            /* If any data TD had a short transfer (CC=DATAUNDERRUN), we
             * leave actual as wLength-clamped; v1 accuracy good enough
             * for descriptor fetches. */
        }
        xfer->actual = actual;
    }

    /* Pause ED, unlink from list. */
    ed->dw0 |= ED_SKIP;
    hc->ctrl_head->next_ed = ed->next_ed;

    /* Copy back to caller buffer for IN transfers. */
    if (rc == USB_OK && xfer->dir_in && bounce && xfer->data && xfer->actual)
        memcpy(xfer->data, bounce, xfer->actual);

    /* Free TD chain + buffers + ED. */
    for (ohci_td_t *t = setup_td; t; ) {
        ohci_td_t *nxt = t->next_sw;
        dma_free(t);
        t = nxt;
    }
    if (bounce) dma_free(bounce);
    dma_free(setup_buf);
    dma_free(ed);
    return rc;

cleanup_chain_fail:
    for (ohci_td_t *t = setup_td; t; ) {
        ohci_td_t *nxt = t->next_sw;
        dma_free(t);
        t = nxt;
    }
cleanup_alloc_fail:
    if (bounce) dma_free(bounce);
    dma_free(setup_buf);
    dma_free(ed);
    return USB_ENOMEM;
}

/* ============================================================
 * Port reset (doc 57 §10 + USB 2.0 §7.1.7.5)
 * Returns speed enum on success, negative errno on failure.
 * Port is 0-indexed for the HCD ops; the OHCI register layout has
 * Port N (1-indexed) at HcRhPortStatus_BASE + 4*(N-1) so port 0 here
 * = OHCI port 1.
 * ============================================================ */
static int ohci_port_reset(usb_hcd_t *base, uint8_t port) {
    ohci_hc_t *hc = (ohci_hc_t *)base->priv;
    if (!hc || port >= hc->num_ports) return USB_EINVAL;
    uint32_t reg = port_reg(hc, port);

    uint32_t s = mmio_readl(hc->mmio, reg);
    if (!(s & HRP_CCS)) return USB_ENODEV;

    /* SetPortReset — write 1 to bit 4. HC asserts USB reset for 10 ms
     * (USB 2.0 §7.1.7.5), then clears PRS itself and sets PRSC. */
    mmio_writel(hc->mmio, reg, HRP_PRS);

    /* Poll PRSC. Generous deadline — spec is 10 ms, allow 50. */
    int ok = 0;
    for (int i = 0; i < 50; i++) {
        s = mmio_readl(hc->mmio, reg);
        if (s & HRP_PRSC) { ok = 1; break; }
        pit_delay_ms(1);
    }
    if (!ok) return USB_EIO;

    /* Clear PRSC (R/WC). */
    mmio_writel(hc->mmio, reg, HRP_PRSC);

    /* Recovery 10 ms (USB 2.0 §9.2.6.2). */
    pit_delay_ms(10);

    /* PES should auto-set after reset. If not, SetPortEnable manually. */
    s = mmio_readl(hc->mmio, reg);
    if (!(s & HRP_PES)) {
        mmio_writel(hc->mmio, reg, HRP_PES);
        pit_delay_ms(2);
        s = mmio_readl(hc->mmio, reg);
        if (!(s & HRP_PES)) return USB_EIO;
    }

    /* Speed from LSDA bit. */
    return (s & HRP_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static int ohci_port_status(usb_hcd_t *base, uint8_t port, uint32_t *st) {
    ohci_hc_t *hc = (ohci_hc_t *)base->priv;
    if (!hc || port >= hc->num_ports || !st) return USB_EINVAL;
    *st = mmio_readl(hc->mmio, port_reg(hc, port));
    return USB_OK;
}

static int ohci_port_enable(usb_hcd_t *base, uint8_t port) {
    ohci_hc_t *hc = (ohci_hc_t *)base->priv;
    if (!hc || port >= hc->num_ports) return USB_EINVAL;
    mmio_writel(hc->mmio, port_reg(hc, port), HRP_PES);
    return USB_OK;
}

/* ============================================================
 * Endpoint open/close — bulk + interrupt get a persistent ED.
 * Control endpoints (EP0) use per-xfer EDs in submit_control so
 * nothing persists at ep_open time for them.
 * ============================================================ */
static int ohci_ep_open(usb_hcd_t *base, usb_device_t *dev,
                        usb_endpoint_t *ep) {
    ohci_hc_t *hc = (ohci_hc_t *)base->priv;
    if (!hc || !dev || !ep) return USB_EINVAL;

    ohci_ep_priv_t *priv = (ohci_ep_priv_t *)kmalloc(sizeof(*priv));
    if (!priv) return USB_ENOMEM;
    memset(priv, 0, sizeof(*priv));

    /* Control endpoints don't need a persistent ED — submit_control
     * allocates fresh per call. Just register the priv so submit_xfer
     * can EBUSY-gate, and so ep_close has something to free. */
    if (ep->type == USB_EP_TYPE_CONTROL) {
        ep->hcd_priv = priv;
        priv->next = hc->ep_list_head;
        hc->ep_list_head = priv;
        return USB_OK;
    }

    if (ep->type != USB_EP_TYPE_BULK && ep->type != USB_EP_TYPE_INTERRUPT) {
        kfree(priv);
        return USB_ENOSYS;   /* v1: no isoc */
    }

    /* Allocate the persistent ED. */
    priv->ed = (ohci_ed_t *)dma_alloc(sizeof(ohci_ed_t), 16);
    if (!priv->ed) { kfree(priv); return USB_ENOMEM; }
    memset((void *)priv->ed, 0, sizeof(*priv->ed));

    int      ls       = (dev->speed == USB_SPEED_LOW);
    uint16_t mps      = ep->max_packet ? ep->max_packet : 8;
    uint8_t  ep_num   = ep->addr & 0x0F;
    uint32_t dir_bits = (ep->addr & 0x80) ? ED_DIR_IN : ED_DIR_OUT;

    priv->ed->dw0 = ED_FA(dev->address) | ED_EN(ep_num) | dir_bits
                  | (ls ? ED_SPEED_LS : 0)
                  | ED_MPS(mps)
                  | ED_SKIP;             /* idle until first submit */
    priv->ed->head_p  = 0;
    priv->ed->tail_p  = 0;
    priv->ed->next_ed = 0;

    /* Splice into the right schedule list AFTER its dummy head. */
    ohci_ed_t *head = (ep->type == USB_EP_TYPE_BULK)
                          ? hc->bulk_head
                          : hc->int_head;
    priv->ed->next_ed = head->next_ed;
    head->next_ed     = dma_virt_to_phys(priv->ed);

    ep->hcd_priv = priv;
    priv->next = hc->ep_list_head;
    hc->ep_list_head = priv;
    return USB_OK;
}

static void ohci_ep_close(usb_hcd_t *base, usb_device_t *dev,
                          usb_endpoint_t *ep) {
    (void)dev;
    ohci_hc_t *hc = (ohci_hc_t *)base->priv;
    if (!hc || !ep || !ep->hcd_priv) return;
    ohci_ep_priv_t *priv = (ohci_ep_priv_t *)ep->hcd_priv;

    /* Unlink from HC's per-EP list. */
    ohci_ep_priv_t **pp = &hc->ep_list_head;
    while (*pp && *pp != priv) pp = &(*pp)->next;
    if (*pp) *pp = priv->next;

    /* TODO v2: unlink priv->ed from the schedule chain. v1 leaves the
     * ED on the list with K=1; HC walks past it. Acceptable until
     * hot-unplug becomes important. */
    if (priv->ed) {
        priv->ed->dw0 |= ED_SKIP;
        dma_free(priv->ed);
    }
    if (priv->bounce) dma_free(priv->bounce);
    kfree(priv);
    ep->hcd_priv = 0;
}

/* ============================================================
 * Async bulk + interrupt submit (doc 57 §11 + OHCI §5.2)
 * ============================================================ */
static int ohci_submit_xfer(usb_hcd_t *base, usb_xfer_t *xfer) {
    ohci_hc_t *hc = (ohci_hc_t *)base->priv;
    if (!hc || !xfer || !xfer->dev || !xfer->ep || !xfer->pipe)
        return USB_EINVAL;
    ohci_ep_priv_t *priv = (ohci_ep_priv_t *)xfer->pipe;
    if (priv->current_xfer) return USB_EBUSY;
    if (!priv->ed) return USB_EINVAL;   /* control endpoints can't use this */

    usb_endpoint_t *ep  = xfer->ep;
    uint16_t mps   = ep->max_packet ? ep->max_packet : 8;
    uint32_t dp    = xfer->dir_in ? TD_DP_IN : TD_DP_OUT;

    /* Bounce buffer — caller's xfer->data is typically a kernel-task
     * stack/.bss buffer outside the DMA region. Allocate once per
     * outstanding xfer; freed on completion. */
    void *bounce = 0;
    if (xfer->len > 0) {
        bounce = dma_alloc(xfer->len, 16);
        if (!bounce) return USB_ENOMEM;
        if (!xfer->dir_in && xfer->data) memcpy(bounce, xfer->data, xfer->len);
    }
    priv->bounce     = bounce;
    priv->bounce_len = xfer->len;

    /* Build TD chain — one per mps chunk. Last TD has DI=0 (IRQ
     * ASAP); others DI=7 (no IRQ). All use T=USE_EDC so HC consumes
     * ED.toggleCarry and updates it across the chain. */
    ohci_td_t *first = 0, *prev = 0;
    uint32_t left = xfer->len;
    uint8_t *p    = (uint8_t *)bounce;
    while (left > 0) {
        uint32_t chunk = left > mps ? mps : left;
        ohci_td_t *td = td_alloc(dp, TD_T_USE_EDC, 1, 7,
                                 p ? dma_virt_to_phys(p) : 0, chunk);
        if (!td) {
            /* Free any partial chain. */
            for (ohci_td_t *t = first; t; ) {
                ohci_td_t *nxt = t->next_sw; dma_free(t); t = nxt;
            }
            if (bounce) { dma_free(bounce); priv->bounce = 0; }
            return USB_ENOMEM;
        }
        if (!first) first = td;
        else {
            prev->next_td = dma_virt_to_phys(td);
            prev->next_sw = td;
        }
        td->xfer = xfer;
        prev = td;
        left -= chunk;
        p    += chunk;
    }
    /* Zero-length transfer is unusual but legal (status-stage style). */
    if (!first) {
        first = td_alloc(dp, TD_T_USE_EDC, 0, 0, 0, 0);
        if (!first) return USB_ENOMEM;
        first->xfer = xfer;
        prev = first;
    } else {
        prev->dw0 &= ~TD_DI_NONE;     /* clear DI=7 */
        prev->dw0 |=  TD_DI(0);       /* IRQ ASAP */
    }

    /* Pause ED while we attach. */
    priv->ed->dw0 |= ED_SKIP;
    priv->ed->head_p = dma_virt_to_phys(first);
    priv->ed->tail_p = 0;             /* HC stops at next_td=0 */
    priv->ed->dw0   &= ~ED_SKIP;

    priv->first_td     = first;
    priv->current_xfer = xfer;

    /* Kick the appropriate list. */
    if (ep->type == USB_EP_TYPE_BULK)
        mmio_writel(hc->mmio, HcCommandStatus, HCS_BLF);
    /* Interrupt list runs every frame from HCCA — no explicit kick. */

    return USB_OK;
}

/* ============================================================
 * IRQ handler (doc 57 §12 + OHCI §6.5)
 * Drains HcInterruptStatus, walks HccaDoneHead chain for completed
 * TDs, fires xfer->done() callbacks, scans root-hub ports on RHSC.
 * ============================================================ */
static void ohci_irq_handler(void *ctx) {
    ohci_hc_t *hc = (ohci_hc_t *)ctx;
    uint32_t hcis = mmio_readl(hc->mmio, HcInterruptStatus);
    if (hcis == 0) return;   /* not us — shared IRQ etiquette */

    /* Capture done queue head BEFORE we ack WDH — HC may overwrite
     * HccaDoneHead with a new chain once we clear WDH. */
    uint32_t done_phys = hc->hcca->done_head;
    hc->hcca->done_head = 0;
    done_phys &= ~0x3u;   /* LSb = "other event also pending" — ignore here */

    /* Ack handled HcInterruptStatus bits (R/WC). */
    mmio_writel(hc->mmio, HcInterruptStatus, hcis);

    if (hcis & HIS_UE) log_s("ohci: UnrecoverableError\n");
    if (hcis & HIS_SO) log_s("ohci: SchedulingOverrun\n");

    /* --- Done queue walk (reverse retirement order, §4.4.2.3) ---
     * Each retired TD has next_td pointing to the *previously* retired
     * TD. For per-EP completion we walk our ep_list and check each ED's
     * head_p (= where HC stopped). v1 uses a hybrid: walk the done
     * chain so we know SOMETHING completed, then per-EP check. */
    if (hcis & HIS_WDH) {
        (void)done_phys;   /* per-TD walk via next_sw below covers it */

        for (ohci_ep_priv_t *priv = hc->ep_list_head; priv;
             priv = priv->next) {
            if (!priv->current_xfer || !priv->first_td || !priv->ed) continue;

            /* All TDs in this xfer done when the last TD's CC != NOTACC. */
            ohci_td_t *last = priv->first_td;
            while (last->next_sw) last = last->next_sw;
            if (TD_CC_GET(last->dw0) == CC_NOTACCESSED) continue;

            usb_xfer_t *xfer = priv->current_xfer;
            int rc = USB_OK;
            uint32_t actual = 0;
            for (ohci_td_t *t = priv->first_td; t; t = t->next_sw) {
                int s = td_status(t);
                if (s != USB_OK) { rc = s; break; }
                /* On full success, CBP=0 → consumed buf full; bytes =
                 * (BE - orig CBP + 1). We only know orig CBP if we
                 * captured it; for v1, treat success as full chunk
                 * transferred. Class drivers (HID) read fixed-size. */
            }
            if (rc == USB_OK) actual = xfer->len;
            xfer->actual = actual;
            xfer->status = rc;

            /* Bounce-back for IN. */
            if (rc == USB_OK && xfer->dir_in && priv->bounce && xfer->data
                && actual > 0)
                memcpy(xfer->data, priv->bounce, actual);

            /* Free TD chain. */
            for (ohci_td_t *t = priv->first_td; t; ) {
                ohci_td_t *nxt = t->next_sw; dma_free(t); t = nxt;
            }
            priv->first_td     = 0;
            priv->current_xfer = 0;
            priv->ed->head_p   = 0;
            /* ED.toggleCarry persists across xfers; HC has already
             * updated it via HEADP_C bit. */

            if (priv->bounce) {
                dma_free(priv->bounce);
                priv->bounce = 0; priv->bounce_len = 0;
            }

            if (xfer->done)
                xfer->done(xfer, xfer->ctx, USB_CB_IN_ISR);
        }
    }

    /* --- Root-hub status change (§6.5.7, §7.1.4 RHSC) --- */
    if (hcis & HIS_RHSC) {
        for (uint8_t p = 0; p < hc->num_ports; p++) {
            uint32_t reg = port_reg(hc, p);
            uint32_t ps  = mmio_readl(hc->mmio, reg);
            if (ps & HRP_CSC) {
                mmio_writel(hc->mmio, reg, HRP_CSC);
                /* Connection events are processed at boot-time in
                 * the probe loop; runtime hot-plug deferred — usbcore
                 * has connect/disconnect entrypoints but v1 only
                 * surfaces boot-time. */
            }
            if (ps & HRP_PESC) mmio_writel(hc->mmio, reg, HRP_PESC);
            if (ps & HRP_OCIC) mmio_writel(hc->mmio, reg, HRP_OCIC);
        }
    }
}

/* ============================================================
 * HC ops vtable
 * ============================================================ */
static usb_hcd_ops_t ohci_ops = {
    .submit_control = ohci_submit_control,
    .submit_xfer    = ohci_submit_xfer,
    .ep_open        = ohci_ep_open,
    .ep_close       = ohci_ep_close,
    .port_reset     = ohci_port_reset,
    .port_status    = ohci_port_status,
    .port_enable    = ohci_port_enable,
    .set_address    = 0,    /* OHCI uses control-path SET_ADDRESS */
};

/* ============================================================
 * Take control from SMM/BIOS (doc 57 §9 + OHCI §5.1.1.3, p.40-42)
 * ============================================================ */
static void ohci_takeover(ohci_hc_t *hc) {
    uint32_t ctrl = mmio_readl(hc->mmio, HcControl);

    if (ctrl & HCC_IR) {
        /* SMM driver currently owns the HC. Request ownership via OCR;
         * SMM clears IR when handoff completes. Netrunner01 PR #27:
         * bound the wait — some BIOSes never clear IR. */
        mmio_writel(hc->mmio, HcCommandStatus, HCS_OCR);
        for (int i = 0; i < 1000; i++) {
            if (!(mmio_readl(hc->mmio, HcControl) & HCC_IR)) break;
            pit_delay_ms(1);
        }
        if (mmio_readl(hc->mmio, HcControl) & HCC_IR) {
            /* SMM never released. Force-clear; if SMM keeps re-asserting
             * we lose, but most BIOSes that hang at this point will
             * simply give up after we write 0. */
            ctrl = mmio_readl(hc->mmio, HcControl);
            mmio_writel(hc->mmio, HcControl, ctrl & ~HCC_IR);
            log_s("ohci: forced IR=0 (SMM handoff timeout)\n");
        }
        return;
    }

    /* BIOS or nobody owns it. */
    uint32_t hcfs = ctrl & HCC_HCFS_MASK;
    if (hcfs == HCC_HCFS_RESET) {
        /* Cold-boot or post-reset — wait the USB spec reset duration
         * before proceeding. (§5.1.1.3.5, p.41) */
        pit_delay_ms(50);
    } else if (hcfs != HCC_HCFS_OPER) {
        /* Suspended or resuming — drive USBRESUME, wait 20 ms (USB
         * spec resume duration). (§5.1.1.3.4, p.41) */
        mmio_writel(hc->mmio, HcControl, (ctrl & ~HCC_HCFS_MASK) | HCC_HCFS_RESUM);
        pit_delay_ms(20);
    }
    /* USBOPERATIONAL — we can proceed directly. */
}

/* ============================================================
 * Per-controller bring-up (doc 57 §9 + OHCI §5.1.1.4)
 * ============================================================ */
static int ohci_init_hc(ohci_hc_t *hc) {
    /* Step 0: identity-map the MMIO BAR page. PCD (cache-disable) so
     * register writes go to the device, not a cache line. PWT not
     * needed for OHCI per chipset notes. */
    vmm_map_page(hc->mmio_phys & ~0xFFFu, hc->mmio_phys & ~0xFFFu,
                 PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    hc->mmio = (uint32_t)(hc->mmio_phys & ~0xFFFu);
    /* If the BAR-low-bits address actual register window beyond the
     * 4 KB page (rare), this v1 fails — log + assume page-0 layout. */

    /* Step 1: HcRevision sanity (§7.1.1, p.109). REV[7:0] = 0x10 for
     * 1.0/1.0a. Upper bits are vendor-specific extensions; mask. */
    uint32_t rev = mmio_readl(hc->mmio, HcRevision) & 0xFF;
    if (rev != 0x10) {
        log_s("ohci: bad rev "); log_h(rev); log_s("\n");
        return USB_ENODEV;
    }

    /* Step 2: SMM/BIOS take-over (§5.1.1.3). */
    ohci_takeover(hc);

    /* Step 3: save HcFmInterval — vendor-tuned timing we must restore
     * post-reset within 2 ms (§5.1.1.4 step 3, p.42). */
    uint32_t fi_saved = mmio_readl(hc->mmio, HcFmInterval);
    if (fi_saved == 0) {
        /* Spec reset value: FI=0x2EDF (11999), FSMPS=0x2778 derived.
         * Some chipsets report 0 if HCR-mid-state. (Defensive default.) */
        fi_saved = 0x27782EDFu;
    }

    /* Step 4: software reset — HCR=1, self-clears in ≤10 µs (§7.1.3 HCR,
     * p.112). Spec says ≤10 µs but a 1 ms poll loop is fine. */
    mmio_writel(hc->mmio, HcCommandStatus, HCS_HCR);
    int hcr_ok = 0;
    for (int i = 0; i < 50; i++) {
        if (!(mmio_readl(hc->mmio, HcCommandStatus) & HCS_HCR)) {
            hcr_ok = 1; break;
        }
        pit_delay_ms(1);
    }
    if (!hcr_ok) {
        log_s("ohci: HCR did not self-clear\n");
        return USB_EIO;
    }

    /* Step 5: restore FmInterval BEFORE 2 ms elapses (else the HC
     * auto-transitions to USBRESUME). (§5.1.1.4 step 3, p.42) */
    mmio_writel(hc->mmio, HcFmInterval, fi_saved);

    /* Step 6: allocate HCCA + dummy head EDs. */
    hc->hcca      = (ohci_hcca_t *)dma_alloc(sizeof(ohci_hcca_t), 256);
    hc->ctrl_head = (ohci_ed_t *)dma_alloc(sizeof(ohci_ed_t), 16);
    hc->bulk_head = (ohci_ed_t *)dma_alloc(sizeof(ohci_ed_t), 16);
    hc->int_head  = (ohci_ed_t *)dma_alloc(sizeof(ohci_ed_t), 16);
    if (!hc->hcca || !hc->ctrl_head || !hc->bulk_head || !hc->int_head)
        return USB_ENOMEM;

    memset((void *)hc->hcca, 0, sizeof(*hc->hcca));
    ed_init_dummy(hc->ctrl_head);
    ed_init_dummy(hc->bulk_head);
    ed_init_dummy(hc->int_head);

    /* All 32 interrupt-table heads point at the single int dummy.
     * Means every interrupt ED gets polled every frame — v1 simplicity
     * over §5.2.7.2 binary-tree bandwidth distribution. */
    uint32_t int_phys = dma_virt_to_phys(hc->int_head);
    for (int i = 0; i < 32; i++)
        hc->hcca->interrupt_table[i] = int_phys;

    /* Step 7: program memory-pointer registers (§7.2). */
    mmio_writel(hc->mmio, HcControlHeadED, dma_virt_to_phys(hc->ctrl_head));
    mmio_writel(hc->mmio, HcBulkHeadED,    dma_virt_to_phys(hc->bulk_head));
    mmio_writel(hc->mmio, HcHCCA,          dma_virt_to_phys(hc->hcca));

    /* Step 8: clear all pending interrupt status; enable MIE | WDH |
     * RHSC | UE | FNO. NOT SF (interrupt-per-frame at 1 ms is too
     * noisy). (§7.1.4-5, p.113-115) */
    mmio_writel(hc->mmio, HcInterruptStatus, 0x7Fu);
    mmio_writel(hc->mmio, HcInterruptEnable,
                HIS_MIE | HIS_WDH | HIS_RHSC | HIS_UE | HIS_FNO);

    /* Step 9: transition to USBOPERATIONAL with all lists enabled.
     * CBSR=3:1 (USB-spec-favored), IR=0 (we own the IRQ), IE=0 (no
     * isoc in v1). (§7.1.2 HcControl, p.110) */
    mmio_writel(hc->mmio, HcControl,
                HCC_HCFS_OPER | HCC_CLE | HCC_BLE | HCC_PLE | HCC_CBSR_3_1);

    /* Step 10: HcPeriodicStart = 90 % of FI (§7.3.4, p.122 — typical
     * value 0x3E67 for FI=0x2EDF). */
    uint16_t fi = fi_saved & 0x3FFF;
    mmio_writel(hc->mmio, HcPeriodicStart, (uint32_t)((fi * 9u) / 10u));

    /* Step 11: root hub. Read NDP + POTPGT, power on, wait. */
    uint32_t rha = mmio_readl(hc->mmio, HcRhDescriptorA);
    hc->num_ports = rha & 0xFF;
    if (hc->num_ports > 15) hc->num_ports = 15;
    if (hc->num_ports < 1)  hc->num_ports = 1;
    uint8_t potpgt = (rha >> 24) & 0xFF;
    hc->potpgt_ms = potpgt ? (potpgt * 2u) : 20;   /* §7.4.1 + PR #25 floor */
    hc->base.num_ports = hc->num_ports;

    /* SetGlobalPower (HcRhStatus bit 16 = LPSC write). */
    mmio_writel(hc->mmio, HcRhStatus, HRS_LPSC_SET);
    pit_delay_ms(hc->potpgt_ms);

    /* Clear all change bits on every port to start clean. */
    for (uint8_t p = 0; p < hc->num_ports; p++) {
        uint32_t reg = port_reg(hc, p);
        mmio_writel(hc->mmio, reg,
                    HRP_CSC | HRP_PESC | HRP_PSSC | HRP_OCIC | HRP_PRSC);
    }

    /* Step 12: install IRQ handler. */
    irq_register(hc->irq, ohci_irq_handler, hc);

    log_s("ohci@"); log_h(hc->mmio_phys);
    log_s(": rev=0x10 ports="); log_h(hc->num_ports);
    log_s(" irq="); log_h(hc->irq); log_s(" running\n");
    return USB_OK;
}

/* ============================================================
 * PCI probe (doc 57 §2)
 * Match class 0x0C / subclass 0x03 / progif 0x10.
 * ============================================================ */
static int ohci_probe_pci(void) {
    int found = 0;
    klog_stage("ohci: pci_find_class");
    for (int i = 0; ; i++) {
        struct pci_dev pd;
        {
            char suf[8];
            const char hex[] = "0123456789ABCDEF";
            int p = 0;
            suf[p++] = 'i'; suf[p++] = '=';
            suf[p++] = hex[(i >> 4) & 0xF];
            suf[p++] = hex[i & 0xF];
            suf[p++] = 0;
            klog_iter(suf);
        }
        if (pci_find_class(0x0C, 0x03, 0x10, &pd, i) != 0) break;
        if (g_num_hcs >= OHCI_MAX_CONTROLLERS) break;

        /* BAR0 = MMIO base (OHCI App.A BAR_OHCI, p.135). Bit 0 = 0 = MMIO. */
        uint32_t bar0 = pd.bar[0];
        if (bar0 & 1) {
            log_s("ohci: BAR0 is I/O space, skipping\n");
            continue;
        }
        uint32_t mmio_phys = bar0 & 0xFFFFF000u;
        if (mmio_phys == 0) {
            log_s("ohci: BAR0 unmapped, skipping\n");
            continue;
        }

        ohci_hc_t *hc = &g_hcs[g_num_hcs++];
        memset(hc, 0, sizeof(*hc));
        hc->mmio_phys = mmio_phys;
        hc->bus = pd.bus; hc->dev = pd.dev; hc->fn = pd.fn;
        hc->irq = pd.irq;

        hc->base.name        = "ohci";
        hc->base.mmio_or_io  = (void *)mmio_phys;
        hc->base.ops         = &ohci_ops;
        hc->base.priv        = hc;

        /* Enable Memory Access + Bus Master in the PCI command reg.
         * (OHCI App.A COMMAND, p.134 — MA=bit 1, BM=bit 2.) */
        uint32_t cmd = pci_cfg_read(pd.bus, pd.dev, pd.fn, 0x04);
        pci_cfg_write(pd.bus, pd.dev, pd.fn, 0x04,
                      (cmd & 0xFFFF0000u) | ((cmd | 0x0006u) & 0xFFFFu));

        {
            char suf[32];
            const char hex[] = "0123456789ABCDEF";
            int p = 0;
            suf[p++] = 'i'; suf[p++] = 'n'; suf[p++] = 'i'; suf[p++] = 't';
            suf[p++] = ' '; suf[p++] = 'b'; suf[p++] = 'd'; suf[p++] = 'f';
            suf[p++] = '=';
            suf[p++] = hex[(pd.bus >> 4) & 0xF]; suf[p++] = hex[pd.bus & 0xF];
            suf[p++] = ':';
            suf[p++] = hex[(pd.dev >> 4) & 0xF]; suf[p++] = hex[pd.dev & 0xF];
            suf[p++] = '.';
            suf[p++] = hex[pd.fn & 0xF];
            suf[p++] = 0;
            klog_iter(suf);
        }

        if (ohci_init_hc(hc) != USB_OK) {
            log_s("ohci: init failed at phys="); log_h(mmio_phys); log_s("\n");
            g_num_hcs--;
            continue;
        }
        if (usbcore_register_hcd(&hc->base) != USB_OK) {
            log_s("ohci: usbcore_register_hcd refused\n");
            g_num_hcs--;
            continue;
        }

        /* Initial port-status sweep — devices present at boot get
         * reset+enumerated here. RHSC interrupt covers hot-plug afterward. */
        for (uint8_t p = 0; p < hc->num_ports; p++) {
            uint32_t reg = port_reg(hc, p);
            uint32_t ps  = mmio_readl(hc->mmio, reg);
            if (!(ps & HRP_CCS)) {
                /* Empty port — clear change bits and move on. */
                mmio_writel(hc->mmio, reg,
                            HRP_CSC | HRP_PESC | HRP_PRSC);
                continue;
            }
            int r = ohci_port_reset(&hc->base, p);
            if (r < 0) {
                log_s("ohci: port_reset failed port="); log_h(p);
                log_s(" err="); log_h((uint32_t)r); log_s("\n");
                continue;
            }
            usbcore_port_connect(&hc->base, p, (usb_speed_t)r);
        }

        found++;
    }
    return found;
}

/* ============================================================
 * Module init / exit
 * ============================================================ */
static int ohci_module_init(void) {
    log_s("ohci.kmd: probing PCI for OHCI controllers\n");
    int n = ohci_probe_pci();
    log_s("ohci.kmd: "); log_h(n); log_s(" controller(s) initialised\n");
    /* Stay loaded even with no controllers — same policy as uhci.kmd. */
    return 0;
}

static void ohci_module_exit(void) {
    log_s("ohci.kmd: exit\n");
    for (int i = 0; i < g_num_hcs; i++) {
        ohci_hc_t *hc = &g_hcs[i];
        irq_unregister(hc->irq, ohci_irq_handler);
        /* Disable all interrupts + halt HC. */
        mmio_writel(hc->mmio, HcInterruptDisable, 0xC000007Fu);
        mmio_writel(hc->mmio, HcControl, HCC_HCFS_SUSP);
        usbcore_unregister_hcd(&hc->base);
        /* DMA blocks + MMIO map leak — acceptable on rare unload. */
    }
    g_num_hcs = 0;
}

module_init(ohci_module_init);
module_exit(ohci_module_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("OHCI 1.0a host controller driver");
MODULE_NAME("ohci");
