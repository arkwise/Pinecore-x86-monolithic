/* uhci.kmd — UHCI 1.1 host controller driver for pinecore-x86.
 *
 * Spec-grounded port from docs/research/51-uhci-driver-derivation.md.
 * Primary references:
 *   - UHCI Design Guide 1.1 (Intel, March 1996) — every citation as §X.Y, p.NN
 *   - USB 2.0 §7.1.7.5 (port reset timing), §9 (enumeration timing constraints)
 * Sanity-check references (NOT sources):
 *   - USBDDOS HCD/uhci.c (GPLv2)
 *   - iPXE drivers/usb/uhci.c (GPL2_OR_LATER_OR_UBDL)
 * Original code written from the spec.
 *
 * License: GPL-2.0 — links EXPORT_SYMBOL_GPL surface of usbcore.kmd.
 *
 * v1 scope:
 *   - PCI probe (class 0x0C/0x03/0x00); BIOS LEGSUP disarm; HC reset.
 *   - Frame list + 3-QH schedule (interrupt → control → bulk reclaim).
 *   - Synchronous submit_control (per-xfer QH + TD chain, busy-wait on
 *     status TD's Active bit).
 *   - submit_xfer for bulk + interrupt — async by way of an IRQ-driven
 *     completion walk that calls the xfer's `done` callback.
 *   - port_reset returns speed enum on success, negative on failure.
 *   - IRQ handler drains USBSTS R/WC, polls PORTSC for hot-plug, walks
 *     open endpoints for completed TDs.
 *
 * Out of v1: isochronous, suspend/resume, SOFMOD tuning, full controller
 * restart on fatal errors.
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

extern uint8_t  inb(uint16_t port);
extern void     outb(uint16_t port, uint8_t v);
extern uint16_t inw(uint16_t port);
extern void     outw(uint16_t port, uint16_t v);
extern uint32_t inl(uint16_t port);
extern void     outl(uint16_t port, uint32_t v);

extern uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
extern void     pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn,
                              uint8_t off, uint32_t v);
extern int      pci_find_class(uint8_t class_code, uint8_t subclass,
                               uint8_t prog_if, struct pci_dev *out, int index);
extern void     pit_delay_ms(uint32_t ms);
extern uint64_t pit_ticks_get(void);

extern void  serial_puts(const char *s);
extern void  serial_puthex(uint32_t v);
extern void  klog_stage(const char *text);
extern void  klog_iter(const char *suffix);
extern void *memset(void *dst, int v, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);

typedef void (*irq_handler_t)(void *ctx);
extern int  irq_register(uint8_t irq, irq_handler_t handler, void *ctx);
extern int  irq_unregister(uint8_t irq, irq_handler_t handler);

extern int  usbcore_register_hcd      (usb_hcd_t *hcd);
extern int  usbcore_unregister_hcd    (usb_hcd_t *hcd);
extern int  usbcore_port_connect      (usb_hcd_t *hcd, uint8_t port,
                                       usb_speed_t spd);
extern int  usbcore_port_disconnect   (usb_hcd_t *hcd, uint8_t port);

/* ============================================================
 * UHCI I/O register offsets (doc 51 §4)
 * ============================================================ */
#define UHCI_USBCMD      0x00   /* §2.1.1, p.11 */
#define UHCI_USBSTS      0x02   /* §2.1.2, p.13 */
#define UHCI_USBINTR     0x04   /* §2.1.3, p.14 */
#define UHCI_FRNUM       0x06   /* §2.1.4, p.14 */
#define UHCI_FRBASEADD   0x08   /* §2.1.5, p.15 */
#define UHCI_SOFMOD      0x0C   /* §2.1.6, p.15 */
#define UHCI_PORTSC1     0x10   /* §2.1.7, p.16 */

/* USBCMD bits (§2.1.1) */
#define UHCI_CMD_RS      (1 << 0)
#define UHCI_CMD_HCRESET (1 << 1)
#define UHCI_CMD_GRESET  (1 << 2)
#define UHCI_CMD_CF      (1 << 6)
#define UHCI_CMD_MAXP    (1 << 7)

/* USBSTS bits (§2.1.2) */
#define UHCI_STS_USBINT  (1 << 0)
#define UHCI_STS_ERROR   (1 << 1)
#define UHCI_STS_HSE     (1 << 3)
#define UHCI_STS_HCPE    (1 << 4)
#define UHCI_STS_HALT    (1 << 5)

/* USBINTR bits (§2.1.3) */
#define UHCI_INTR_TIMEOUT (1 << 0)
#define UHCI_INTR_IOC     (1 << 2)
#define UHCI_INTR_SPI     (1 << 3)

/* PORTSC bits (§2.1.7) */
#define UHCI_PORT_CCS    (1 << 0)   /* CurrentConnectStatus  (RO)  */
#define UHCI_PORT_CSC    (1 << 1)   /* ConnectStatusChange   (R/WC) */
#define UHCI_PORT_PE     (1 << 2)   /* PortEnable             (R/W) */
#define UHCI_PORT_PEC    (1 << 3)   /* PortEnableChange      (R/WC) */
#define UHCI_PORT_LS_DM  (1 << 4)   /* LineStatus D−          (RO)  */
#define UHCI_PORT_LS_DP  (1 << 5)   /* LineStatus D+          (RO)  */
#define UHCI_PORT_RES    (1 << 6)   /* ResumeDetect           (R/W) */
#define UHCI_PORT_ALW1   (1 << 7)   /* always-1               (RO)  */
#define UHCI_PORT_LSDA   (1 << 8)   /* LowSpeedDeviceAttached (RO)  */
#define UHCI_PORT_PR     (1 << 9)   /* PortReset              (R/W) */
#define UHCI_PORT_SUS    (1 << 12)  /* Suspend                (R/W) */

/* PCI BIOS legacy support register (§5.2.1, p.39) */
#define UHCI_PCI_LEGSUP   0xC0

/* ============================================================
 * Schedule data structures (doc 51 §6, §7)
 * Both are HW-touched — packed, dma_alloc'd, identity-mapped.
 * ============================================================ */

typedef struct uhci_td {
    /* HW words */
    volatile uint32_t link;       /* DWord 0 — §3.2.1, p.21 */
    volatile uint32_t ctrl;       /* DWord 1 — §3.2.2, p.22 */
    volatile uint32_t token;      /* DWord 2 — §3.2.3, p.24 */
    volatile uint32_t buf;        /* DWord 3 — §3.2.4, p.25 */
    /* SW reserved (§3.2.5, p.25) — chained list of TDs in this xfer */
    struct uhci_td *next_sw;
    void           *xfer;         /* usb_xfer_t * — for IRQ completion */
    uint32_t        _pad[2];      /* keep total 32 B */
} uhci_td_t;

typedef struct uhci_qh {
    volatile uint32_t qhlp;       /* horizontal — §3.3, p.25 */
    volatile uint32_t qelp;       /* vertical — §3.3, p.26 */
    /* Two pad words so kmalloc-style 16-byte alignment is preserved. */
    uint32_t _pad[2];
} uhci_qh_t;

/* TD control/status field encodings */
#define TD_LINK_T            0x0001
#define TD_LINK_Q            0x0002
#define TD_LINK_VF           0x0004

#define TD_CTRL_ACTIVE       (1 << 23)
#define TD_CTRL_STALLED      (1 << 22)
#define TD_CTRL_DBERR        (1 << 21)
#define TD_CTRL_BABBLE       (1 << 20)
#define TD_CTRL_NAK          (1 << 19)
#define TD_CTRL_CRC_TIMEOUT  (1 << 18)
#define TD_CTRL_BITSTUFF     (1 << 17)
#define TD_CTRL_IOC          (1 << 24)
#define TD_CTRL_LS           (1 << 26)
#define TD_CTRL_CERR_3       (3 << 27)
#define TD_CTRL_ACTLEN_MASK  0x000007FF

/* PIDs (§3.2.3, p.24) */
#define USB_PID_OUT          0xE1
#define USB_PID_IN           0x69
#define USB_PID_SETUP        0x2D

/* ============================================================
 * Per-endpoint driver state (doc 51 §14)
 * ============================================================ */
typedef struct uhci_ep_priv {
    uhci_qh_t       *qh;
    uint8_t          toggle;
    usb_xfer_t      *current_xfer;
    uhci_td_t       *first_td;
    struct uhci_ep_priv *next;
} uhci_ep_priv_t;

/* ============================================================
 * Per-controller state
 * ============================================================ */
#define UHCI_MAX_CONTROLLERS  4

typedef struct uhci_hc {
    usb_hcd_t       base;        /* exposed to usbcore */
    uint16_t        io;          /* I/O base from BAR4 */
    uint8_t         bus, dev, fn;
    uint8_t         irq;
    uint8_t         num_ports;

    volatile uint32_t *frame_list;   /* 4 KB, 1024 entries */
    uhci_qh_t       *qh_int;
    uhci_qh_t       *qh_ctrl;
    uhci_qh_t       *qh_bulk;

    /* List of all open endpoints with pending xfers — IRQ walks for
     * completions. Singly-linked head insertion. */
    uhci_ep_priv_t *ep_list_head;
} uhci_hc_t;

static uhci_hc_t  g_hcs[UHCI_MAX_CONTROLLERS];
static int        g_num_hcs = 0;

/* ============================================================
 * Small logging helpers
 * ============================================================ */
static void log_s(const char *s) { serial_puts(s); }
static void log_h(uint32_t v)    { serial_puthex(v); }

/* ============================================================
 * PCI legacy disarm (doc 51 §3)
 * ============================================================ */
static void uhci_disarm_legacy(uint8_t bus, uint8_t dev, uint8_t fn) {
    /* Clear all SMI/trap enables; clear R/WC status bits; preserve
     * USBPIRQDEN (bit 13) so the IRQ still routes. (§5.2.1, p.39) */
    uint32_t reg = pci_cfg_read(bus, dev, fn, UHCI_PCI_LEGSUP);
    uint32_t lo  = reg & 0xFFFF;
    uint32_t hi  = reg & 0xFFFF0000;
    lo = 0xAF00;  /* see doc 51 §3 — bit breakdown */
    pci_cfg_write(bus, dev, fn, UHCI_PCI_LEGSUP, hi | lo);
}

/* ============================================================
 * TD constructor (doc 51 §11 helper)
 * ============================================================ */
static uhci_td_t *uhci_make_td(uint8_t addr, uint8_t ep, uint8_t toggle,
                               uint8_t pid, uint16_t len,
                               uint32_t buf_phys, int low_speed) {
    uhci_td_t *td = (uhci_td_t *)dma_alloc(sizeof(uhci_td_t), 16);
    if (!td) return 0;
    memset((void *)td, 0, sizeof(*td));
    td->link  = TD_LINK_T;
    td->ctrl  = TD_CTRL_CERR_3
              | (low_speed ? TD_CTRL_LS : 0)
              | TD_CTRL_ACTIVE;
    uint32_t maxlen = (len == 0) ? 0x7FF : (uint32_t)(len - 1);
    td->token = (maxlen   << 21)
              | (((uint32_t)toggle & 1) << 19)
              | (((uint32_t)ep     & 0xF)  << 15)
              | (((uint32_t)addr   & 0x7F) <<  8)
              |  pid;
    td->buf   = buf_phys;
    return td;
}

/* Decode a finished TD's status into an errno. */
static int uhci_td_status(uhci_td_t *td) {
    uint32_t c = td->ctrl;
    if (c & TD_CTRL_STALLED)      return USB_EIO;
    if (c & TD_CTRL_DBERR)        return USB_EIO;
    if (c & TD_CTRL_BABBLE)       return USB_EIO;
    if (c & TD_CTRL_CRC_TIMEOUT)  return USB_ETIMEDOUT;
    if (c & TD_CTRL_BITSTUFF)     return USB_EIO;
    return USB_OK;
}

/* ============================================================
 * Submit synchronous control transfer (doc 51 §11)
 * ============================================================ */
static int uhci_submit_control(usb_hcd_t *base, usb_xfer_t *xfer) {
    uhci_hc_t *hc = (uhci_hc_t *)base->priv;
    if (!hc || !xfer || !xfer->dev) return USB_EINVAL;

    usb_device_t *dev = xfer->dev;
    uint8_t  addr = dev->address;
    uint16_t mps  = dev->ep0_max_packet ? dev->ep0_max_packet : 8;
    int      ls   = (dev->speed == USB_SPEED_LOW);

    /* Allocate per-xfer QH + setup buffer + data bounce buffer. The
     * caller's data pointer (xfer->data) is typically on a kernel-task
     * stack, NOT in the DMA region — dma_virt_to_phys would return 0
     * and the HC would DMA into low memory (IVT). Bounce through a
     * dma_alloc'd buffer, copy in/out around the transfer. */
    uhci_qh_t *qh = (uhci_qh_t *)dma_alloc(sizeof(uhci_qh_t), 16);
    if (!qh) return USB_ENOMEM;
    uint8_t *setup_buf = (uint8_t *)dma_alloc(8, 16);
    if (!setup_buf) { dma_free(qh); return USB_ENOMEM; }
    memcpy(setup_buf, &xfer->setup, 8);

    uint8_t *bounce = 0;
    if (xfer->setup.wLength > 0) {
        bounce = (uint8_t *)dma_alloc(xfer->setup.wLength, 16);
        if (!bounce) { dma_free(setup_buf); dma_free(qh); return USB_ENOMEM; }
        if (!xfer->dir_in && xfer->data)
            memcpy(bounce, xfer->data, xfer->setup.wLength);
    }

    /* Build SETUP TD. */
    uhci_td_t *setup_td = uhci_make_td(addr, 0, 0, USB_PID_SETUP,
                                       8, dma_virt_to_phys(setup_buf), ls);
    if (!setup_td) { dma_free(setup_buf); dma_free(qh); return USB_ENOMEM; }

    /* Data-stage TD chain — points into the bounce buffer. */
    uhci_td_t *prev = setup_td;
    uint8_t   pid_d = xfer->dir_in ? USB_PID_IN : USB_PID_OUT;
    uint8_t   toggle = 1;
    uint16_t  left = xfer->setup.wLength;
    uint8_t  *p    = bounce;
    while (left > 0) {
        uint16_t chunk = left > mps ? mps : left;
        uhci_td_t *td = uhci_make_td(addr, 0, toggle, pid_d, chunk,
                                     dma_virt_to_phys(p), ls);
        if (!td) goto cleanup_fail;
        prev->link    = dma_virt_to_phys(td) | TD_LINK_VF;
        prev->next_sw = td;
        prev          = td;
        toggle ^= 1;
        left -= chunk;
        p    += chunk;
    }

    /* Status TD — opposite direction, DATA1, 0 bytes, IOC=1. */
    uint8_t pid_s = xfer->dir_in ? USB_PID_OUT : USB_PID_IN;
    uhci_td_t *status_td = uhci_make_td(addr, 0, 1, pid_s, 0, 0, ls);
    if (!status_td) goto cleanup_fail;
    status_td->ctrl |= TD_CTRL_IOC;
    status_td->link  = TD_LINK_T;
    prev->link       = dma_virt_to_phys(status_td) | TD_LINK_VF;
    prev->next_sw    = status_td;

    /* Stand-alone per-xfer QH (T=1 on horizontal so HC doesn't roam). */
    qh->qhlp = TD_LINK_T;
    qh->qelp = dma_virt_to_phys(setup_td);  /* TD, valid */

    /* Splice into control QH chain — point its qelp at our QH. (v1
     * supports one outstanding control xfer at a time. Future work:
     * a real linked queue.) */
    hc->qh_ctrl->qelp = dma_virt_to_phys(qh) | TD_LINK_Q;

    /* Busy-wait for status TD's Active bit to clear or timeout. PIT is
     * 100 Hz so each tick = 10 ms; convert ms → ticks with +1 ceiling. */
    uint32_t timeout = xfer->timeout_ms ? xfer->timeout_ms : 5000;
    uint64_t deadline = pit_ticks_get() + (timeout / 10) + 1;
    while (pit_ticks_get() < deadline) {
        if ((status_td->ctrl & TD_CTRL_ACTIVE) == 0) break;
        pit_delay_ms(1);
    }

    int rc = USB_OK;
    if (status_td->ctrl & TD_CTRL_ACTIVE) {
        rc = USB_ETIMEDOUT;
    } else {
        /* Walk TDs in submission order, accumulate transferred bytes,
         * stop at first non-OK status. */
        uint32_t actual = 0;
        for (uhci_td_t *t = setup_td; t; t = t->next_sw) {
            int s = uhci_td_status(t);
            if (s != USB_OK) { rc = s; break; }
            uint32_t al = (t->ctrl & TD_CTRL_ACTLEN_MASK);
            /* ActLen field is encoded as bytes - 1; 0x7FF means no
             * transfer occurred. (§3.2.2, p.23) */
            if (al != 0x7FF && t != setup_td && t != status_td)
                actual += al + 1;
        }
        xfer->actual = actual;
    }

    /* Unlink from control QH (idle the slot). */
    hc->qh_ctrl->qelp = TD_LINK_T;

    /* Copy back to caller buffer for IN transfers. */
    if (rc == USB_OK && xfer->dir_in && bounce && xfer->data && xfer->actual)
        memcpy(xfer->data, bounce, xfer->actual);

    /* Free the TD chain + bounce + setup buffer + QH. */
    for (uhci_td_t *t = setup_td; t; ) {
        uhci_td_t *nxt = t->next_sw;
        dma_free(t);
        t = nxt;
    }
    if (bounce) dma_free(bounce);
    dma_free(setup_buf);
    dma_free(qh);
    return rc;

cleanup_fail:
    for (uhci_td_t *t = setup_td; t; ) {
        uhci_td_t *nxt = t->next_sw;
        dma_free(t);
        t = nxt;
    }
    if (bounce) dma_free(bounce);
    dma_free(setup_buf);
    dma_free(qh);
    return USB_ENOMEM;
}

/* ============================================================
 * Port reset (doc 51 §10)
 * Returns speed enum on success, negative errno on failure.
 * ============================================================ */
static int uhci_port_reset(usb_hcd_t *base, uint8_t port) {
    uhci_hc_t *hc = (uhci_hc_t *)base->priv;
    if (!hc || port >= hc->num_ports) return USB_EINVAL;
    uint16_t off = UHCI_PORTSC1 + 2 * port;

    uint16_t s = inw(hc->io + off);
    if (!(s & UHCI_PORT_CCS)) return USB_ENODEV;

    /* Assert reset, hold ≥10 ms (USB 2.0 §7.1.7.5). */
    outw(hc->io + off, (s & ~(UHCI_PORT_CSC | UHCI_PORT_PEC)) | UHCI_PORT_PR);
    pit_delay_ms(50);

    /* Release reset. */
    s = inw(hc->io + off);
    outw(hc->io + off, s & ~UHCI_PORT_PR);
    pit_delay_ms(10);  /* recovery interval — USB 2.0 §9.2.6.2 */

    /* Enable the port. */
    s = inw(hc->io + off);
    outw(hc->io + off, s | UHCI_PORT_PE);

    /* Wait for enabled + connected stable. */
    for (int i = 0; i < 100; i++) {
        s = inw(hc->io + off);
        if ((s & UHCI_PORT_PE) && (s & UHCI_PORT_CCS)) break;
        pit_delay_ms(1);
    }
    if (!(s & UHCI_PORT_PE)) return USB_EIO;

    /* Clear R/WC change bits. */
    outw(hc->io + off, s | UHCI_PORT_CSC | UHCI_PORT_PEC);

    return (s & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static int uhci_port_status(usb_hcd_t *base, uint8_t port, uint32_t *st) {
    uhci_hc_t *hc = (uhci_hc_t *)base->priv;
    if (!hc || port >= hc->num_ports || !st) return USB_EINVAL;
    *st = inw(hc->io + UHCI_PORTSC1 + 2 * port);
    return USB_OK;
}

static int uhci_port_enable(usb_hcd_t *base, uint8_t port) {
    uhci_hc_t *hc = (uhci_hc_t *)base->priv;
    if (!hc || port >= hc->num_ports) return USB_EINVAL;
    uint16_t off = UHCI_PORTSC1 + 2 * port;
    uint16_t s = inw(hc->io + off);
    outw(hc->io + off, s | UHCI_PORT_PE);
    return USB_OK;
}

/* ============================================================
 * Endpoint open / close (doc 51 §14)
 * ============================================================ */
static int uhci_ep_open(usb_hcd_t *base, usb_device_t *dev,
                        usb_endpoint_t *ep) {
    (void)dev;
    uhci_hc_t *hc = (uhci_hc_t *)base->priv;
    if (!hc || !ep) return USB_EINVAL;

    uhci_ep_priv_t *priv = (uhci_ep_priv_t *)kmalloc(sizeof(*priv));
    if (!priv) return USB_ENOMEM;
    memset(priv, 0, sizeof(*priv));

    priv->qh = (uhci_qh_t *)dma_alloc(sizeof(uhci_qh_t), 16);
    if (!priv->qh) { kfree(priv); return USB_ENOMEM; }
    priv->qh->qelp = TD_LINK_T;
    priv->toggle  = 0;  /* reset on SET_CONFIGURATION — USB 2.0 §9.1.1.5 */

    /* Splice into the right list (interrupt or bulk). */
    if (ep->type == USB_EP_TYPE_INTERRUPT) {
        priv->qh->qhlp = hc->qh_int->qhlp;
        hc->qh_int->qhlp = dma_virt_to_phys(priv->qh) | TD_LINK_Q;
    } else if (ep->type == USB_EP_TYPE_BULK) {
        priv->qh->qhlp = hc->qh_bulk->qhlp;
        hc->qh_bulk->qhlp = dma_virt_to_phys(priv->qh) | TD_LINK_Q;
    } else {
        /* Control endpoints don't get a persistent QH. */
        priv->qh->qhlp = TD_LINK_T;
    }

    /* Link into HC's ep list for IRQ completion walking. */
    priv->next = hc->ep_list_head;
    hc->ep_list_head = priv;

    ep->hcd_priv = priv;
    return USB_OK;
}

static void uhci_ep_close(usb_hcd_t *base, usb_device_t *dev,
                          usb_endpoint_t *ep) {
    (void)dev;
    uhci_hc_t *hc = (uhci_hc_t *)base->priv;
    if (!hc || !ep || !ep->hcd_priv) return;
    uhci_ep_priv_t *priv = (uhci_ep_priv_t *)ep->hcd_priv;

    /* Unlink from HC list. */
    uhci_ep_priv_t **pp = &hc->ep_list_head;
    while (*pp && *pp != priv) pp = &(*pp)->next;
    if (*pp) *pp = priv->next;

    /* TODO: also unlink priv->qh from the schedule chain. v1 leaks the
     * QH spot; revisit when hot-unplug arrives. */
    dma_free(priv->qh);
    kfree(priv);
    ep->hcd_priv = 0;
}

/* ============================================================
 * Async bulk + interrupt submit (doc 51 §12)
 * ============================================================ */
static int uhci_submit_xfer(usb_hcd_t *base, usb_xfer_t *xfer) {
    uhci_hc_t *hc = (uhci_hc_t *)base->priv;
    if (!hc || !xfer || !xfer->dev || !xfer->ep || !xfer->pipe)
        return USB_EINVAL;

    /* xfer->pipe is the endpoint's hcd_priv (uhci_ep_priv_t *). */
    uhci_ep_priv_t *priv = (uhci_ep_priv_t *)xfer->pipe;

    /* Refuse a second submit while one is outstanding (v1 — single
     * outstanding xfer per endpoint). */
    if (priv->current_xfer) return USB_EBUSY;

    usb_device_t   *dev = xfer->dev;
    usb_endpoint_t *ep  = xfer->ep;
    uint8_t  addr    = dev->address;
    uint16_t mps     = ep->max_packet ? ep->max_packet : 64;
    int      ls      = (dev->speed == USB_SPEED_LOW);
    uint8_t  ep_addr = ep->addr & 0x0F;
    uint8_t  pid     = xfer->dir_in ? USB_PID_IN : USB_PID_OUT;

    /* Build TD chain — one TD per mps-sized chunk. */
    uhci_td_t *first = 0, *prev = 0;
    uint32_t  left = xfer->len;
    uint8_t  *p    = (uint8_t *)xfer->data;
    while (left > 0) {
        uint32_t chunk = left > mps ? mps : left;
        uhci_td_t *td = uhci_make_td(addr, ep_addr, priv->toggle, pid,
                                     (uint16_t)chunk,
                                     p ? dma_virt_to_phys(p) : 0, ls);
        if (!td) { /* leak prior chain — acceptable on OOM, rare */ return USB_ENOMEM; }
        if (!first) first = td;
        else        prev->link = dma_virt_to_phys(td) | TD_LINK_VF;
        td->xfer    = xfer;
        prev        = td;
        priv->toggle ^= 1;
        left -= chunk;
        p    += chunk;
    }
    if (!first) return USB_EINVAL;
    prev->ctrl |= TD_CTRL_IOC;

    /* Attach under priv->qh. */
    priv->first_td      = first;
    priv->current_xfer  = xfer;
    priv->qh->qelp      = dma_virt_to_phys(first);
    return USB_OK;
}

/* ============================================================
 * IRQ handler (doc 51 §13)
 * Drains USBSTS, polls PORTSC for hot-plug, walks ep list for
 * completed TDs and fires `done` callbacks.
 * ============================================================ */
static void uhci_irq_handler(void *ctx) {
    uhci_hc_t *hc = (uhci_hc_t *)ctx;
    uint16_t status = inw(hc->io + UHCI_USBSTS);
    if (status == 0) return;  /* not ours — shared IRQ etiquette */

    /* Acknowledge handled bits. */
    outw(hc->io + UHCI_USBSTS, status);

    if (status & UHCI_STS_HCPE) log_s("uhci: HC process error\n");
    if (status & UHCI_STS_HSE)  log_s("uhci: host system error\n");

    /* Walk open endpoints for completed TDs. */
    for (uhci_ep_priv_t *priv = hc->ep_list_head; priv; priv = priv->next) {
        if (!priv->current_xfer || !priv->first_td) continue;
        /* Look at the IOC TD (last in chain) — if Active=0, complete. */
        uhci_td_t *last = priv->first_td;
        while (last->next_sw) last = last->next_sw;
        if (last->ctrl & TD_CTRL_ACTIVE) continue;

        usb_xfer_t *xfer = priv->current_xfer;
        int rc = USB_OK;
        uint32_t actual = 0;
        for (uhci_td_t *t = priv->first_td; t; t = t->next_sw) {
            int s = uhci_td_status(t);
            if (s != USB_OK) { rc = s; break; }
            uint32_t al = (t->ctrl & TD_CTRL_ACTLEN_MASK);
            if (al != 0x7FF) actual += al + 1;
        }
        xfer->actual = actual;
        xfer->status = rc;

        /* Detach + free before callback so done() can resubmit. */
        for (uhci_td_t *t = priv->first_td; t; ) {
            uhci_td_t *nxt = t->next_sw;
            dma_free(t);
            t = nxt;
        }
        priv->first_td     = 0;
        priv->current_xfer = 0;
        priv->qh->qelp     = TD_LINK_T;

        if (xfer->done) xfer->done(xfer, xfer->ctx, USB_CB_IN_ISR);
    }

    /* Hot-plug detection — clear CSC but DON'T enumerate from IRQ
     * context (it would do dma_alloc + pit_delay_ms which are unsafe
     * here). A future task-context port-watcher will pick this up.
     * v1 only handles boot-time enumeration in uhci_probe_pci. */
    for (int p = 0; p < hc->num_ports; p++) {
        uint16_t ps = inw(hc->io + UHCI_PORTSC1 + 2 * p);
        if (ps & UHCI_PORT_CSC)
            outw(hc->io + UHCI_PORTSC1 + 2 * p, ps | UHCI_PORT_CSC);
    }
}

/* ============================================================
 * HC ops vtable
 * ============================================================ */
static usb_hcd_ops_t uhci_ops = {
    .submit_control = uhci_submit_control,
    .submit_xfer    = uhci_submit_xfer,
    .ep_open        = uhci_ep_open,
    .ep_close       = uhci_ep_close,
    .port_reset     = uhci_port_reset,
    .port_status    = uhci_port_status,
    .port_enable    = uhci_port_enable,
    .set_address    = 0,   /* UHCI sends SET_ADDRESS via the control path */
};

/* ============================================================
 * Per-controller bring-up (doc 51 §9)
 * ============================================================ */
static int uhci_init_hc(uhci_hc_t *hc) {
    /* Step 1: disarm BIOS legacy BEFORE any HC I/O. */
    uhci_disarm_legacy(hc->bus, hc->dev, hc->fn);

    /* Step 2: Global Reset — drives USB reset on all ports. */
    outw(hc->io + UHCI_USBCMD, UHCI_CMD_GRESET);
    pit_delay_ms(50);
    outw(hc->io + UHCI_USBCMD, 0);

    /* Step 3: Host Controller Reset (internal). Self-clears. */
    outw(hc->io + UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 1000; i++) {
        if ((inw(hc->io + UHCI_USBCMD) & UHCI_CMD_HCRESET) == 0) break;
        pit_delay_ms(1);
    }
    if (inw(hc->io + UHCI_USBCMD) & UHCI_CMD_HCRESET) return USB_ETIMEDOUT;

    /* Step 4: silence interrupts; clear all USBSTS bits. */
    outw(hc->io + UHCI_USBINTR, 0);
    outw(hc->io + UHCI_USBSTS,  0xFFFF);

    /* Step 5: discover port count (PORTSC bit 7 = always-1 detect). */
    hc->num_ports = 0;
    for (int i = 0; i < 8; i++) {
        uint16_t v = inw(hc->io + UHCI_PORTSC1 + 2 * i);
        if ((v & UHCI_PORT_ALW1) == 0) break;
        hc->num_ports = i + 1;
    }
    if (hc->num_ports < 2) hc->num_ports = 2;
    hc->base.num_ports = hc->num_ports;

    /* Step 6: allocate schedule structures. */
    hc->frame_list = (volatile uint32_t *)dma_alloc(4096, 4096);
    hc->qh_int     = (uhci_qh_t *)dma_alloc(sizeof(uhci_qh_t), 16);
    hc->qh_ctrl    = (uhci_qh_t *)dma_alloc(sizeof(uhci_qh_t), 16);
    hc->qh_bulk    = (uhci_qh_t *)dma_alloc(sizeof(uhci_qh_t), 16);
    if (!hc->frame_list || !hc->qh_int || !hc->qh_ctrl || !hc->qh_bulk)
        return USB_ENOMEM;

    /* Step 7: chain the QHs. (§3.3) */
    hc->qh_int->qhlp  = dma_virt_to_phys(hc->qh_ctrl) | TD_LINK_Q;
    hc->qh_int->qelp  = TD_LINK_T;
    hc->qh_ctrl->qhlp = dma_virt_to_phys(hc->qh_bulk) | TD_LINK_Q;
    hc->qh_ctrl->qelp = TD_LINK_T;
    hc->qh_bulk->qhlp = dma_virt_to_phys(hc->qh_ctrl) | TD_LINK_Q;
    hc->qh_bulk->qelp = TD_LINK_T;

    /* Step 8: point every frame-list entry at qh_int. */
    uint32_t int_phys = dma_virt_to_phys(hc->qh_int) | TD_LINK_Q;
    for (int i = 0; i < 1024; i++) hc->frame_list[i] = int_phys;

    /* Step 9: program FRBASEADD and FRNUM. */
    outl(hc->io + UHCI_FRBASEADD, dma_virt_to_phys((void *)hc->frame_list));
    outw(hc->io + UHCI_FRNUM, 0);

    /* Step 10: SOFMOD default (1 ms exact at 12 MHz). */
    outb(hc->io + UHCI_SOFMOD, 0x40);

    /* Step 11: install IRQ handler. */
    irq_register(hc->irq, uhci_irq_handler, hc);

    /* Step 12: enable interrupts + run. */
    outw(hc->io + UHCI_USBINTR,
         UHCI_INTR_TIMEOUT | UHCI_INTR_IOC | UHCI_INTR_SPI);
    outw(hc->io + UHCI_USBCMD,
         UHCI_CMD_MAXP | UHCI_CMD_CF | UHCI_CMD_RS);

    /* Step 13: verify not halted after a frame. */
    pit_delay_ms(10);
    if (inw(hc->io + UHCI_USBSTS) & UHCI_STS_HALT) return USB_EIO;

    log_s("uhci@"); log_h(hc->io);
    log_s(": ports="); log_h(hc->num_ports);
    log_s(" irq="); log_h(hc->irq);
    log_s(" running\n");
    return USB_OK;
}

/* ============================================================
 * PCI probe (doc 51 §2)
 * ============================================================ */
static int uhci_probe_pci(void) {
    int found = 0;
    klog_stage("uhci: pci_find_class");
    for (int i = 0; ; i++) {
        struct pci_dev pd;
        {
            char suf[24];
            const char hex[] = "0123456789ABCDEF";
            int p = 0;
            suf[p++] = 'i'; suf[p++] = '=';
            suf[p++] = hex[(i >> 4) & 0xF];
            suf[p++] = hex[i & 0xF];
            suf[p++] = 0;
            klog_iter(suf);
        }
        if (pci_find_class(0x0C, 0x03, 0x00, &pd, i) != 0) break;
        if (g_num_hcs >= UHCI_MAX_CONTROLLERS) break;

        /* BAR4 = USBBASE, I/O space. Bit 0 = 1 = IO. (§2.2.2, p.19) */
        uint32_t bar4 = pd.bar[4];
        if ((bar4 & 1) != 1) continue;
        uint16_t io_base = bar4 & 0xFFE0;

        uhci_hc_t *hc = &g_hcs[g_num_hcs++];
        memset(hc, 0, sizeof(*hc));
        hc->io  = io_base;
        hc->bus = pd.bus;
        hc->dev = pd.dev;
        hc->fn  = pd.fn;
        hc->irq = pd.irq;

        hc->base.name        = "uhci";
        hc->base.mmio_or_io  = (void *)(uint32_t)io_base;
        hc->base.ops         = &uhci_ops;
        hc->base.priv        = hc;

        /* Enable IO Space + Bus Master in the PCI command register. */
        uint32_t cmd = pci_cfg_read(pd.bus, pd.dev, pd.fn, 0x04);
        pci_cfg_write(pd.bus, pd.dev, pd.fn, 0x04,
                      (cmd & 0xFFFF0000) | ((cmd | 0x0005) & 0xFFFF));

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
        if (uhci_init_hc(hc) != USB_OK) {
            log_s("uhci: init failed at io="); log_h(io_base); log_s("\n");
            g_num_hcs--;
            continue;
        }
        if (usbcore_register_hcd(&hc->base) != USB_OK) {
            log_s("uhci: usbcore_register_hcd refused\n");
            g_num_hcs--;
            continue;
        }

        /* Initial port-status sweep — devices present at boot raise no
         * change event in UHCI; we must probe each port once. Reset the
         * port first so the device transitions Powered → Default → ready
         * to answer at address 0. (USB 2.0 §9.1.2 step 4, p.243.) */
        for (int p = 0; p < hc->num_ports; p++) {
            uint16_t ps = inw(hc->io + UHCI_PORTSC1 + 2 * p);
            if (!(ps & UHCI_PORT_CCS)) {
                outw(hc->io + UHCI_PORTSC1 + 2 * p,
                     ps | UHCI_PORT_CSC | UHCI_PORT_PEC);
                continue;
            }
            int r = uhci_port_reset(&hc->base, p);
            if (r < 0) {
                log_s("uhci: port_reset failed on port=");
                log_h(p); log_s(" err="); log_h((uint32_t)r); log_s("\n");
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
static int uhci_module_init(void) {
    log_s("uhci.kmd: probing PCI for UHCI controllers\n");
    int n = uhci_probe_pci();
    log_s("uhci.kmd: "); log_h(n); log_s(" controller(s) initialised\n");
    /* Returning 0 even with no controllers — the module stays loaded so
     * it can pick up hot-add controllers later (PCI hot-add is rare on
     * our targets, but the policy choice is "no controllers ≠ failure"). */
    return 0;
}

static void uhci_module_exit(void) {
    log_s("uhci.kmd: exit\n");
    for (int i = 0; i < g_num_hcs; i++) {
        uhci_hc_t *hc = &g_hcs[i];
        irq_unregister(hc->irq, uhci_irq_handler);
        outw(hc->io + UHCI_USBCMD, 0);
        usbcore_unregister_hcd(&hc->base);
        /* DMA blocks leak — acceptable on rare unload. */
    }
    g_num_hcs = 0;
}

module_init(uhci_module_init);
module_exit(uhci_module_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("UHCI 1.1 host controller driver");
MODULE_NAME("uhci");
