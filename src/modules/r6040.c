/* r6040.kmd — Pinecore network provider for RDC R6040 Fast Ethernet.
 *
 * Built-in Fast Ethernet MAC on the DM&P Vortex86SX SoC. This .kmd
 * implements the net_provider_ops vtable (Phase 1: HW skeleton with
 * socket stubs returning PCNET_ENOSYS).
 *
 * PCI:     VID 0x17F3 (RDC) / DID 0x6040
 * BAR:     I/O (256 bytes), 16-bit register access
 * PHY:     Internal, accessed via MDIO at MMDIO+0x20
 * Rings:   64 TX + 64 RX descriptors (32-byte stride, DMA-allocated)
 * IRQ:     Shared INTx, registered via irq_register
 * ISR:     Drains MISR, re-arms; chain shim handles EOI
 *
 * Build:   make modules (via src/Makefile MODULE_SRCS)
 * Load:    auto via PCORE.CFG: net_provider = r6040
 *
 * References (study only, no code copied):
 *   Linux drivers/net/ethernet/rdc/r6040.c (GPL-2.0)
 *   DMP Vortex86SX Software Programming Reference
 *
 * License: GPL-2.0
 */

#include "module.h"
#include "net.h"
#include "types.h"

/* ------------------------------------------------------------------
 * Register offsets (I/O BAR, 16-bit access)
 * ------------------------------------------------------------------ */

#define MCR0        0x00
#define  MCR0_RCVEN  0x0002
#define  MCR0_PROMISC 0x0020
#define  MCR0_HASH_EN 0x0100
#define  MCR0_XMTEN   0x1000
#define  MCR0_FD      0x8000

#define MCR1        0x04
#define  MAC_RST     0x0001

#define MBCR        0x08
#define MT_ICR      0x0C
#define MR_ICR      0x10
#define MTPR        0x14
#define  TM2TX       0x0001

#define MR_BSR      0x18
#define MR_DCR      0x1A
#define MLSR        0x1C

#define MMDIO       0x20
#define  MDIO_WRITE  0x4000
#define  MDIO_READ   0x2000
#define  MDIO_DATA_MASK 0x001F

#define MMRD        0x24
#define MMWD        0x28
#define MTD_SA0     0x2C
#define MTD_SA1     0x30
#define MRD_SA0     0x34
#define MRD_SA1     0x38
#define MISR        0x3C
#define MIER        0x40
#define  RX_FINISH   0x0001
#define  RX_NO_DESC  0x0002
#define  RX_FIFO_FULL 0x0004
#define  RX_EARLY    0x0008
#define  TX_FINISH   0x0010
#define  TX_EARLY    0x0080
#define  EVENT_OVRFL 0x0100
#define  LINK_CHANGED 0x0200
#define  RX_INTS     (RX_FIFO_FULL | RX_NO_DESC | RX_FINISH)
#define  TX_INTS     (TX_FINISH)
#define  INT_MASK    (RX_INTS | TX_INTS)

#define MID_0L      0x68
#define MID_0M      0x6A
#define MID_0H      0x6C

#define PHY_CC      0x88
#define  SCEN        0x8000
#define  PHYAD_SHIFT 8
#define  TMRDIV_SHIFT 0

#define PHY_ST      0x8A
#define MAC_SM      0xAC
#define  MAC_SM_RST  0x0002
#define MD_CSC      0xB6
#define  MD_CSC_DEFAULT 0x0030

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define R6040_VENDOR  0x17F3
#define R6040_DEVICE  0x6040
#define TX_DCNT       64
#define RX_DCNT       64
#define DESC_SIZE     32
#define MAX_BUF_SIZE  0x600
#define MAC_DEF_TIMEOUT 2048
#define MBCR_DEFAULT  0x012A

/* ------------------------------------------------------------------
 * Descriptor format (HW portion: first 16 bytes, ring stride: 32)
 * ------------------------------------------------------------------ */

struct r6040_desc {
    uint16_t status;
    uint16_t len;
    uint32_t buf;         /* physical address of packet buffer */
    uint32_t ndesc;       /* physical address of next descriptor */
    uint32_t _rsvd;       /* reserved (HW may write) */
    void    *buf_virt;    /* +16: virtual address of buffer (SW) */
    void    *ndesc_virt;  /* +20: virtual ptr to next desc (SW) */
    uint32_t _pad[2];     /* pad to 32 bytes */
};

#define DSC_OWNER_MAC  0x8000
#define DSC_RX_OK      0x4000
#define DSC_RX_ERR     0x0800

/* ------------------------------------------------------------------
 * Private driver state
 * ------------------------------------------------------------------ */

struct r6040_priv {
    uint16_t  iobase;
    uint8_t   irq_line;
    uint8_t   irq_registered;
    uint8_t   mac[6];
    uint16_t  mcr0;
    int       link_up;

    struct r6040_desc *tx_ring;
    struct r6040_desc *rx_ring;
    uint32_t  tx_ring_phys;
    uint32_t  rx_ring_phys;
    uint16_t  tx_cur;
    uint16_t  tx_done;
    uint16_t  rx_cur;
    void     *rx_bufs[RX_DCNT];
    int       started;
};

static struct r6040_priv r6040;

/* ------------------------------------------------------------------
 * Port I/O helpers (backed by exported kernel inw/outw)
 * ------------------------------------------------------------------ */

extern uint16_t inw(uint16_t port);
extern void outw(uint16_t port, uint16_t val);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);

static inline uint16_t r6040_read(uint16_t reg) {
    return inw(r6040.iobase + reg);
}
static inline void r6040_write(uint16_t reg, uint16_t val) {
    outw(r6040.iobase + reg, val);
}

/* ------------------------------------------------------------------
 * MDIO / PHY access
 * ------------------------------------------------------------------ */

static int r6040_phy_read(int phy_addr, int reg, uint16_t *out) {
    int limit = MAC_DEF_TIMEOUT;
    r6040_write(MMDIO, MDIO_READ | reg | (phy_addr << 8));
    while (limit--) {
        if (!(r6040_read(MMDIO) & MDIO_READ))
            break;
    }
    if (limit < 0) return -1;
    *out = r6040_read(MMRD);
    return 0;
}

/* Phase 2: r6040_phy_write will go here */

/* ------------------------------------------------------------------
 * MAC reset
 * ------------------------------------------------------------------ */

static void r6040_reset_mac(void) {
    int limit = MAC_DEF_TIMEOUT;
    uint16_t md_csc = r6040_read(MD_CSC);

    r6040_write(MCR1, MAC_RST);
    while (limit--) {
        if (r6040_read(MCR1) & MAC_RST)
            break;
    }
    /* Reset internal state machine */
    r6040_write(MAC_SM, MAC_SM_RST);
    r6040_write(MAC_SM, 0);

    /* Restore MD_CSC if it was modified */
    if (md_csc != MD_CSC_DEFAULT)
        r6040_write(MD_CSC, md_csc);
}

/* ------------------------------------------------------------------
 * Ring setup helpers
 * ------------------------------------------------------------------ */

static void r6040_init_ring_desc(struct r6040_desc *ring,
                                  uint32_t ring_phys, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        struct r6040_desc *d = &ring[i];
        uint32_t next_phys = ring_phys + (uint32_t)((i + 1) % count) * DESC_SIZE;
        d->ndesc = next_phys;
        d->ndesc_virt = (void *)&ring[(i + 1) % count];
        d->status = 0;
        d->len = 0;
        d->buf = 0;
        d->buf_virt = 0;
    }
}

extern void *dma_alloc(uint32_t size, uint32_t align);
extern void  dma_free(void *p);
extern uint32_t dma_virt_to_phys(void *p);
extern uint32_t dma_free_bytes(void);

extern void pit_delay_ms(uint32_t ms);
extern void serial_puts(const char *s);
extern void serial_puthex(uint32_t v);
extern void serial_putc(char c);

extern void *memset(void *s, int c, uint32_t n);
extern void *memcpy(void *d, const void *s, uint32_t n);

extern uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
extern void     pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
extern int  irq_register(uint8_t irq, void (*handler)(void *), void *ctx);
extern int  irq_unregister(uint8_t irq, void (*handler)(void *));
extern int  net_register_provider(const struct net_provider *p);
extern void net_unregister_provider(const struct net_provider *p);

/* ------------------------------------------------------------------
 * PCI scan — find R6040 on bus 0
 * ------------------------------------------------------------------ */

static int r6040_pci_find(void) {
    uint8_t dev;
    for (dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read(0, dev, 0, 0);
        uint16_t vid = id & 0xFFFF;
        uint16_t did = (id >> 16) & 0xFFFF;
        if (vid == R6040_VENDOR && did == R6040_DEVICE) {
            uint32_t bar0 = pci_cfg_read(0, dev, 0, 0x10);
            uint32_t cmd  = pci_cfg_read(0, dev, 0, 0x04);

            r6040.iobase   = (uint16_t)(bar0 & ~3);
            r6040.irq_line = (uint8_t)(pci_cfg_read(0, dev, 0, 0x3C) & 0xFF);

            /* Enable bus master + I/O space in PCI command reg */
            pci_cfg_write(0, dev, 0, 0x04, cmd | 0x0007);

            serial_puts("r6040: found at ");
            serial_puthex(vid); serial_putc(':');
            serial_puthex(did); serial_puts(" bus=0 dev=");
            serial_puthex(dev); serial_puts(" iobase=");
            serial_puthex(r6040.iobase); serial_puts(" irq=");
            serial_puthex(r6040.irq_line); serial_puts("\n");

            if (!r6040.iobase) {
                serial_puts("r6040: BAR0 is zero or not I/O — abort\n");
                return -1;
            }
            return 0;
        }
    }
    serial_puts("r6040: not found on bus 0\n");
    return -1;
}

/* ------------------------------------------------------------------
 * Read MAC address from hardware
 * ------------------------------------------------------------------ */

static void r6040_read_mac(void) {
    r6040.mac[0] = r6040_read(MID_0L) & 0xFF;
    r6040.mac[1] = (r6040_read(MID_0L) >> 8) & 0xFF;
    r6040.mac[2] = r6040_read(MID_0M) & 0xFF;
    r6040.mac[3] = (r6040_read(MID_0M) >> 8) & 0xFF;
    r6040.mac[4] = r6040_read(MID_0H) & 0xFF;
    r6040.mac[5] = (r6040_read(MID_0H) >> 8) & 0xFF;
}

/* ------------------------------------------------------------------
 * ISR
 * ------------------------------------------------------------------ */

static void r6040_isr(void *ctx) {
    (void)ctx;
    uint16_t status = r6040_read(MISR);

    if (status == 0 || status == 0xFFFF)
        return;

    if (status & LINK_CHANGED) {
        uint16_t phy_st = r6040_read(PHY_ST);
        r6040.link_up = (phy_st & 0x0001) ? 1 : 0;
    }

    if (status & RX_NO_DESC)
        serial_puts("r6040: RX no desc\n");
    if (status & RX_FIFO_FULL)
        serial_puts("r6040: RX FIFO full\n");
}

/* ------------------------------------------------------------------
 * net_provider_ops — HW lifecycle
 * ------------------------------------------------------------------ */

static int r6040_start(void) {
    int i;

    serial_puts("r6040: starting...\n");

    if (r6040.started) {
        serial_puts("r6040: already started\n");
        return 0;
    }

    /* ---- Allocate DMA descriptor rings ---- */

    r6040.tx_ring = dma_alloc(TX_DCNT * DESC_SIZE, 32);
    r6040.rx_ring = dma_alloc(RX_DCNT * DESC_SIZE, 32);
    if (!r6040.tx_ring || !r6040.rx_ring) {
        serial_puts("r6040: failed to alloc descriptor rings\n");
        goto fail_free;
    }
    r6040.tx_ring_phys = dma_virt_to_phys(r6040.tx_ring);
    r6040.rx_ring_phys = dma_virt_to_phys(r6040.rx_ring);

    /* ---- Allocate RX buffers ---- */
    for (i = 0; i < RX_DCNT; i++) {
        r6040.rx_bufs[i] = dma_alloc(MAX_BUF_SIZE, 16);
        if (!r6040.rx_bufs[i]) {
            serial_puts("r6040: failed to alloc RX buf ");
            serial_puthex(i); serial_puts("\n");
            goto fail_rx_bufs;
        }
    }

    serial_puts("r6040: DMA rings at ");
    serial_puthex(r6040.tx_ring_phys);
    serial_puts(" (TX) / ");
    serial_puthex(r6040.rx_ring_phys);
    serial_puts(" (RX) dma_free=");
    serial_puthex(dma_free_bytes());
    serial_puts("\n");

    /* ---- Init descriptor rings ---- */
    r6040_init_ring_desc(r6040.tx_ring, r6040.tx_ring_phys, TX_DCNT);
    r6040_init_ring_desc(r6040.rx_ring, r6040.rx_ring_phys, RX_DCNT);

    /* Pre-fill RX descriptors with buffer addresses, owned by MAC */
    for (i = 0; i < RX_DCNT; i++) {
        struct r6040_desc *d = &r6040.rx_ring[i];
        d->buf = dma_virt_to_phys(r6040.rx_bufs[i]);
        d->buf_virt = r6040.rx_bufs[i];
        d->status = DSC_OWNER_MAC;
    }
    r6040.rx_cur = 0;

    /* ---- Reset MAC ---- */
    r6040_write(MIER, 0x0000);     /* mask all interrupts */
    r6040_reset_mac();
    pit_delay_ms(10);

    /* ---- Init MAC registers ---- */
    r6040_write(MBCR, MBCR_DEFAULT);
    r6040_write(MR_BSR, MAX_BUF_SIZE);

    /* Write ring base addresses */
    r6040_write(MTD_SA0, r6040.tx_ring_phys & 0xFFFF);
    r6040_write(MTD_SA1, (r6040.tx_ring_phys >> 16) & 0xFFFF);
    r6040_write(MRD_SA0, r6040.rx_ring_phys & 0xFFFF);
    r6040_write(MRD_SA1, (r6040.rx_ring_phys >> 16) & 0xFFFF);

    /* Interrupt thresholds: fire immediately */
    r6040_write(MT_ICR, 0);
    r6040_write(MR_ICR, 0);

    /* ---- Verify PHY ---- */
    {
        uint16_t phy_id1, phy_id2;
        if (r6040_phy_read(0, 2, &phy_id1) == 0 &&
            r6040_phy_read(0, 3, &phy_id2) == 0) {
            uint32_t phy_id = ((uint32_t)phy_id1 << 16) | phy_id2;
            serial_puts("r6040: PHY ID = ");
            serial_puthex(phy_id);
            serial_puts("\n");
        } else {
            serial_puts("r6040: PHY read failed\n");
        }
    }

    /* ---- Install ISR ---- */
    if (irq_register(r6040.irq_line, r6040_isr, 0) != 0) {
        serial_puts("r6040: irq_register failed\n");
        goto fail_irq;
    }
    r6040.irq_registered = 1;

    /* ---- Enable RX + TX ---- */
    r6040.mcr0 = MCR0_XMTEN;
    r6040_write(MCR0, r6040.mcr0);
    r6040_write(MCR0, r6040.mcr0 | MCR0_RCVEN);

    /* Unmask interrupts */
    r6040_write(MIER, INT_MASK);

    /* ---- Print status ---- */
    serial_puts("r6040: MAC ");
    for (i = 0; i < 6; i++) {
        serial_puthex(r6040.mac[i]);
        if (i < 5) serial_putc(':');
    }
    serial_puts("\n");

    /* Check link state */
    {
        uint16_t phy_st = r6040_read(PHY_ST);
        r6040.link_up = (phy_st & 0x0001) ? 1 : 0;
        serial_puts("r6040: link ");
        serial_puts(r6040.link_up ? "UP" : "DOWN");
        serial_puts("\n");
    }

    r6040.started = 1;
    serial_puts("r6040: started OK\n");
    return 0;

fail_irq:
fail_rx_bufs:
    for (i = 0; i < RX_DCNT; i++)
        if (r6040.rx_bufs[i])
            dma_free(r6040.rx_bufs[i]);
    if (r6040.tx_ring) dma_free(r6040.tx_ring);
    if (r6040.rx_ring) dma_free(r6040.rx_ring);
    r6040.tx_ring = 0;
    r6040.rx_ring = 0;
fail_free:
    r6040.started = 0;
    return -1;
}

static void r6040_stop(void) {
    int i;
    if (!r6040.started) return;

    serial_puts("r6040: stopping...\n");

    /* Mask interrupts */
    r6040_write(MIER, 0x0000);

    /* Reset MAC */
    r6040_reset_mac();

    /* Unregister IRQ */
    if (r6040.irq_registered) {
        irq_unregister(r6040.irq_line, r6040_isr);
        r6040.irq_registered = 0;
    }

    /* Free DMA memory */
    for (i = 0; i < RX_DCNT; i++)
        if (r6040.rx_bufs[i])
            dma_free(r6040.rx_bufs[i]);
    if (r6040.tx_ring) dma_free(r6040.tx_ring);
    if (r6040.rx_ring) dma_free(r6040.rx_ring);
    r6040.tx_ring = 0;
    r6040.rx_ring = 0;

    r6040.started = 0;
    serial_puts("r6040: stopped\n");
}

/* ------------------------------------------------------------------
 * net_provider_ops — socket stubs (Phase 1: all return ENOSYS)
 * ------------------------------------------------------------------ */

static int stub_sock_create(int domain, int type, int protocol,
                             pcnet_sock_t *out)
{
    (void)domain; (void)type; (void)protocol; (void)out;
    return PCNET_ENOSYS;
}
static int stub_sock_close(pcnet_sock_t s) { (void)s; return PCNET_ENOSYS; }
static int stub_sock_shutdown(pcnet_sock_t s, int how)
    { (void)s; (void)how; return PCNET_ENOSYS; }
static int stub_sock_bind(pcnet_sock_t s, const struct sockaddr *addr,
                           pcnet_socklen_t addrlen)
    { (void)s; (void)addr; (void)addrlen; return PCNET_ENOSYS; }
static int stub_sock_connect(pcnet_sock_t s, const struct sockaddr *addr,
                              pcnet_socklen_t addrlen)
    { (void)s; (void)addr; (void)addrlen; return PCNET_ENOSYS; }
static int stub_sock_listen(pcnet_sock_t s, int backlog)
    { (void)s; (void)backlog; return PCNET_ENOSYS; }
static int stub_sock_accept(pcnet_sock_t s, pcnet_sock_t *new_sock,
                             struct sockaddr *addr, pcnet_socklen_t *addrlen)
    { (void)s; (void)new_sock; (void)addr; (void)addrlen; return PCNET_ENOSYS; }
static int stub_sock_getsockname(pcnet_sock_t s, struct sockaddr *addr,
                                  pcnet_socklen_t *addrlen)
    { (void)s; (void)addr; (void)addrlen; return PCNET_ENOSYS; }
static int stub_sock_getpeername(pcnet_sock_t s, struct sockaddr *addr,
                                  pcnet_socklen_t *addrlen)
    { (void)s; (void)addr; (void)addrlen; return PCNET_ENOSYS; }
static pcnet_ssize_t stub_sock_send(pcnet_sock_t s, const void *buf,
                                     uint32_t len, int flags)
    { (void)s; (void)buf; (void)len; (void)flags; return PCNET_ENOSYS; }
static pcnet_ssize_t stub_sock_recv(pcnet_sock_t s, void *buf, uint32_t len,
                                     int flags)
    { (void)s; (void)buf; (void)len; (void)flags; return PCNET_ENOSYS; }
static pcnet_ssize_t stub_sock_sendto(pcnet_sock_t s, const void *buf,
                                       uint32_t len, int flags,
                                       const struct sockaddr *to,
                                       pcnet_socklen_t tolen)
    { (void)s; (void)buf; (void)len; (void)flags; (void)to; (void)tolen;
      return PCNET_ENOSYS; }
static pcnet_ssize_t stub_sock_recvfrom(pcnet_sock_t s, void *buf,
                                         uint32_t len, int flags,
                                         struct sockaddr *from,
                                         pcnet_socklen_t *fromlen)
    { (void)s; (void)buf; (void)len; (void)flags; (void)from; (void)fromlen;
      return PCNET_ENOSYS; }
static int stub_sock_poll(pcnet_sock_t s)
    { (void)s; return PCNET_ENOSYS; }
static int stub_sock_setsockopt(pcnet_sock_t s, int level, int optname,
                                 const void *optval, pcnet_socklen_t optlen)
    { (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
      return PCNET_ENOSYS; }
static int stub_sock_getsockopt(pcnet_sock_t s, int level, int optname,
                                 void *optval, pcnet_socklen_t *optlen)
    { (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
      return PCNET_ENOSYS; }

static const struct net_provider_ops r6040_ops = {
    .start          = r6040_start,
    .stop           = r6040_stop,
    .sock_create    = stub_sock_create,
    .sock_close     = stub_sock_close,
    .sock_shutdown  = stub_sock_shutdown,
    .sock_bind      = stub_sock_bind,
    .sock_connect   = stub_sock_connect,
    .sock_listen    = stub_sock_listen,
    .sock_accept    = stub_sock_accept,
    .sock_getsockname = stub_sock_getsockname,
    .sock_getpeername = stub_sock_getpeername,
    .sock_send      = stub_sock_send,
    .sock_recv      = stub_sock_recv,
    .sock_sendto    = stub_sock_sendto,
    .sock_recvfrom  = stub_sock_recvfrom,
    .sock_poll      = stub_sock_poll,
    .sock_setsockopt = stub_sock_setsockopt,
    .sock_getsockopt = stub_sock_getsockopt,
};

static const struct net_provider r6040_provider = {
    .name          = "r6040",
    .description   = "RDC R6040 Fast Ethernet (Vortex86SX) — HW skeleton",
    .version_major = 0,
    .version_minor = 1,
    .version_patch = 0,
    .abi_version   = NET_PROVIDER_ABI_VERSION,
    .stability     = NET_PROVIDER_EXPERIMENTAL,
    .caps          = 0,
    .ops           = &r6040_ops,
};

/* ------------------------------------------------------------------
 * Module entry points
 * ------------------------------------------------------------------ */

static int r6040_init(void) {
    serial_puts("r6040: probing for RDC R6040...\n");

    if (r6040_pci_find() != 0)
        return -1;

    r6040_read_mac();

    serial_puts("r6040: registering provider...\n");
    if (net_register_provider(&r6040_provider) != 0) {
        serial_puts("r6040: net_register_provider failed\n");
        return -1;
    }

    serial_puts("r6040: module loaded OK\n");
    return 0;
}

static void r6040_exit(void) {
    serial_puts("r6040: exiting...\n");
    net_unregister_provider(&r6040_provider);
    serial_puts("r6040: module unloaded\n");
}

module_init(r6040_init);
module_exit(r6040_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("RDC R6040 Fast Ethernet network provider (Vortex86SX)");
MODULE_NAME("r6040");
