/* iwi.kmd — Pinecore driver for Intel PRO/Wireless 2200BG / 2915ABG.
 *
 * v0 — PROBE ONLY. PCI scan + identity report. No MMIO touch, no reset,
 *      no firmware load, no rings. Purpose is to confirm our PCI
 *      scaffolding finds the chip on real Vortex86 hardware (mini-PCI
 *      slot of the eBOX-2300SXA-C) before we start writing into BAR0.
 *
 * Spec-first per docs/research/60-62. Reference sources:
 *   - OpenBSD if_iwi(4) — ~/Projects/openbsd-iwi-ref/  (ISC, Bergamini)
 *   - Linux ipw2200    — ~/Projects/linux-ref/drivers/net/wireless/intel/ipw2x00/
 *
 * Citations: iwi:if_iwi.c:LINE  (study only; no code copied)
 *
 * Hardware (per doc 61 §1):
 *   PCI vendor  : 0x8086 (Intel)
 *   PCI device  : 0x4220  PRO/Wireless 2200BG
 *                  0x4221  PRO/Wireless 2225BG
 *                  0x4223  PRO/Wireless 2915ABG    <-- our test chip
 *                  0x4224  PRO/Wireless 2915ABG (sec)
 *   BAR0        : 32-bit memory-mapped, ~4 KB CSR window
 *   IRQ         : shared PCI INTx
 *   Cfg quirk   : clear high byte of word @ cfg offset 0x40
 *                 (iwi:if_iwi.c:170-173)
 *
 * Roadmap:
 *   v0  probe + report                  <-- this file
 *   v1  BAR0 mmap + iwi_reset + rings   (needs vmm_map_page exported)
 *   v2  firmware load (boot/ucode/main) (needs ~190 KB DMA in chunks)
 *   v3  iwi_config + scan + assoc + WPA2 + net_provider integration
 *
 * License: GPL-2.0
 */

#include "module.h"
#include "types.h"

/* ------------------------------------------------------------------
 * Kernel-symbol externs (resolved by the .kmd loader against kexports)
 * ------------------------------------------------------------------ */

extern uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
extern void     pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);

extern void serial_puts(const char *s);
extern void serial_puthex(uint32_t v);
extern void serial_putc(char c);

extern void klog_stage(const char *s);

/* ------------------------------------------------------------------
 * Match table — Intel WiFi chips this driver handles
 * ------------------------------------------------------------------ */

#define IWI_VENDOR  0x8086

struct iwi_id {
    uint16_t    did;
    const char *name;
};

static const struct iwi_id iwi_match_table[] = {
    { 0x4220, "PRO/Wireless 2200BG" },
    { 0x4221, "PRO/Wireless 2225BG" },
    { 0x4223, "PRO/Wireless 2915ABG" },
    { 0x4224, "PRO/Wireless 2915ABG (sec)" },
    { 0x0000, 0 }
};

/* PCI config-space offsets we use (from PCI 3.0 §6.1 standard cfg header). */
#define PCI_CFG_VID_DID     0x00
#define PCI_CFG_CMD_STATUS  0x04
#define PCI_CFG_REV_CLASS   0x08
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_SUBSYS      0x2C
#define PCI_CFG_QUIRK_40    0x40   /* device-specific, doc 61 §1 */
#define PCI_CFG_INTR        0x3C

/* PCI Command-register bits */
#define PCI_CMD_IO_SPACE    0x0001
#define PCI_CMD_MEM_SPACE   0x0002
#define PCI_CMD_BUS_MASTER  0x0004

/* ------------------------------------------------------------------
 * Module-local state — one chip per system (v0 supports a single iwi)
 * ------------------------------------------------------------------ */

struct iwi_dev {
    int      found;       /* 1 if iwi_pci_scan matched a chip, 0 otherwise */
    uint8_t  bus, dev, fn;
    uint16_t vid;
    uint16_t did;
    const char *name;
    uint32_t bar0_phys;   /* BAR0, low 4 bits masked off */
    uint8_t  bar0_is_mem; /* 1 if memory BAR, 0 if I/O */
    uint16_t subvid;
    uint16_t subdid;
    uint8_t  irq;
    uint8_t  revision;
};

static struct iwi_dev iwi;

/* ------------------------------------------------------------------
 * Match a vid/did against the table
 * ------------------------------------------------------------------ */

static const struct iwi_id *iwi_lookup(uint16_t vid, uint16_t did) {
    int i;
    if (vid != IWI_VENDOR)
        return 0;
    for (i = 0; iwi_match_table[i].did != 0; i++) {
        if (iwi_match_table[i].did == did)
            return &iwi_match_table[i];
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Scan buses 0..7 (matches pci_init scan range, src/kernel/pci.c:134)
 * Returns 0 on first match (fills `iwi`), -1 if nothing found.
 * ------------------------------------------------------------------ */

static int iwi_pci_scan(void) {
    uint8_t bus, dev, fn;
    uint32_t id, cmd, bar0, intr, subsys;
    uint16_t vid, did;
    const struct iwi_id *m;

    for (bus = 0; bus < 8; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (fn = 0; fn < 8; fn++) {
                id = pci_cfg_read(bus, dev, fn, PCI_CFG_VID_DID);
                vid = id & 0xFFFF;
                did = (id >> 16) & 0xFFFF;
                if (vid == 0xFFFF)
                    continue;

                m = iwi_lookup(vid, did);
                if (!m)
                    continue;

                /* Found one. Capture everything we need from cfg
                 * space and stop. v0 doesn't support multi-NIC. */
                iwi.bus  = bus;
                iwi.dev  = dev;
                iwi.fn   = fn;
                iwi.vid  = vid;
                iwi.did  = did;
                iwi.name = m->name;

                bar0 = pci_cfg_read(bus, dev, fn, PCI_CFG_BAR0);
                /* PCI 3.0 §6.2.5: BAR low bit clear => memory BAR;
                 * memory BARs use low 4 bits as type/prefetch flags,
                 * I/O BARs use low 2 bits. iwi is memory; mask 4 bits. */
                iwi.bar0_is_mem = (bar0 & 0x1) ? 0 : 1;
                iwi.bar0_phys   = bar0 & ~0xFu;

                intr = pci_cfg_read(bus, dev, fn, PCI_CFG_INTR);
                iwi.irq = intr & 0xFF;

                subsys     = pci_cfg_read(bus, dev, fn, PCI_CFG_SUBSYS);
                iwi.subvid = subsys & 0xFFFF;
                iwi.subdid = (subsys >> 16) & 0xFFFF;

                iwi.revision = pci_cfg_read(bus, dev, fn,
                                            PCI_CFG_REV_CLASS) & 0xFF;

                /* Enable Memory Space + Bus Master in PCI command reg
                 * so future BAR0 access works once we map it. Leave
                 * I/O Space bit alone — iwi has no I/O BAR. */
                cmd = pci_cfg_read(bus, dev, fn, PCI_CFG_CMD_STATUS);
                pci_cfg_write(bus, dev, fn, PCI_CFG_CMD_STATUS,
                              cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
                iwi.found = 1;
                return 0;
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------
 * Apply the doc-61 §1 cfg-quirk: clear high byte of word @ cfg 0x40.
 * Per iwi:if_iwi.c:170-173 — chip-specific, must precede iwi_reset.
 * Safe to do now (no MMIO involved); leaving it here so v0 already
 * lands the workaround.
 * ------------------------------------------------------------------ */

static void iwi_apply_cfg_quirk(void) {
    uint32_t v = pci_cfg_read(iwi.bus, iwi.dev, iwi.fn, PCI_CFG_QUIRK_40);
    v &= ~0x0000FF00u;
    pci_cfg_write(iwi.bus, iwi.dev, iwi.fn, PCI_CFG_QUIRK_40, v);
}

/* ------------------------------------------------------------------
 * Diagnostic report — serial + klog status line
 * ------------------------------------------------------------------ */

static void iwi_report(void) {
    serial_puts("iwi: found ");
    serial_puthex(iwi.vid); serial_putc(':');
    serial_puthex(iwi.did); serial_puts(" (");
    serial_puts(iwi.name);  serial_puts(") at ");
    serial_puthex(iwi.bus); serial_putc(':');
    serial_puthex(iwi.dev); serial_putc('.');
    serial_puthex(iwi.fn);  serial_puts("\n");

    serial_puts("iwi:   subsystem ");
    serial_puthex(iwi.subvid); serial_putc(':');
    serial_puthex(iwi.subdid); serial_puts(" rev ");
    serial_puthex(iwi.revision); serial_puts("\n");

    serial_puts("iwi:   BAR0 ");
    serial_puthex(iwi.bar0_phys);
    serial_puts(iwi.bar0_is_mem ? " (mem)" : " (io)");
    serial_puts(" IRQ ");
    serial_puthex(iwi.irq); serial_puts("\n");
}

/* ------------------------------------------------------------------
 * iwi_test — exported, called from the `wifi` Commando builtin.
 *
 * Accepts a print callback (the shell's vt-aware print) so test
 * output goes to the active terminal, not just serial.
 * ------------------------------------------------------------------ */

typedef void (*iwi_print_fn)(const char *);

/* Tiny local hex emitter — keeps the module self-contained instead of
 * pulling in a kernel sprintf. 8 hex digits, no prefix. */
static void emit_hex8(iwi_print_fn p, uint32_t v) {
    static const char d[] = "0123456789ABCDEF";
    char buf[9];
    int i;
    for (i = 7; i >= 0; i--) {
        buf[i] = d[v & 0xF];
        v >>= 4;
    }
    buf[8] = 0;
    p(buf);
}

void iwi_test(iwi_print_fn p) {
    if (!iwi.found) {
        p("iwi: no Intel 2200/2915 found on PCI bus\n");
        p("     (this is normal in QEMU; real test = Vortex86 mini-PCI)\n");
        return;
    }

    p("iwi: ");
    p(iwi.name);
    p("\n  vendor:device  ");
    emit_hex8(p, iwi.vid);
    p(":");
    emit_hex8(p, iwi.did);
    p("\n  location       bus ");
    emit_hex8(p, iwi.bus);
    p(" dev ");
    emit_hex8(p, iwi.dev);
    p(" fn ");
    emit_hex8(p, iwi.fn);
    p("\n  subsystem      ");
    emit_hex8(p, iwi.subvid);
    p(":");
    emit_hex8(p, iwi.subdid);
    p(" rev ");
    emit_hex8(p, iwi.revision);
    p("\n  BAR0           ");
    emit_hex8(p, iwi.bar0_phys);
    p(iwi.bar0_is_mem ? " (mem)\n" : " (io)\n");
    p("  IRQ            ");
    emit_hex8(p, iwi.irq);
    p("\n  state          v0 probe — MMIO not yet mapped, no reset yet\n");
    p("                 (v1 work: BAR0 mmap + iwi_reset + ring alloc)\n");
}

EXPORT_SYMBOL_GPL(iwi_test);

/* ------------------------------------------------------------------
 * Module entry / exit
 * ------------------------------------------------------------------ */

static int iwi_modinit(void) {
    klog_stage("iwi: pci scan");
    serial_puts("iwi: probing for Intel PRO/Wireless 2200/2915...\n");

    if (iwi_pci_scan() != 0) {
        serial_puts("iwi: no supported chip found (module stays loaded "
                    "so `wifi` cmd can report)\n");
        klog_stage("iwi: none");
        /* Return 0 — keep the module loaded so `wifi` builtin can still
         * call iwi_test (which will say "not found"). Cost: ~1.5 KB
         * resident. */
        return 0;
    }

    iwi_report();

    /* Cfg-space quirk doesn't touch MMIO, so it's safe in v0. */
    iwi_apply_cfg_quirk();
    serial_puts("iwi: cfg-quirk @0x40 applied\n");

    klog_stage("iwi: probed");
    serial_puts("iwi: v0 probe complete (no MMIO touch yet)\n");
    return 0;
}

static void iwi_modexit(void) {
    serial_puts("iwi: exiting\n");
}

module_init(iwi_modinit);
module_exit(iwi_modexit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("Intel PRO/Wireless 2200BG/2915ABG (v0 probe only)");
MODULE_NAME("iwi");
