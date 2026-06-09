/* PCI bus scan + USB host-controller enumeration.
 *
 * Mech-1 config access via ports 0xCF8/0xCFC. We do a single-bus scan
 * for now (buses 0..7) since Vortex86SX-class hardware doesn't have
 * a deep PCI hierarchy. Devices found are logged to COM1; USB host
 * controllers (class 0x0C subclass 0x03) are cached for the USB stack
 * (see Path C — native USB-HID kbd driver).
 */

#include "pci.h"
#include "io.h"
#include "serial.h"
#include "vga.h"
#include "klog.h"

#define MAX_USB_CONTROLLERS 8

static struct pci_dev usb_controllers[MAX_USB_CONTROLLERS];
static int            usb_controller_count = 0;

static inline void pci_outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t pci_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  << 8)  | (off & 0xFC);
    pci_outl(0xCF8, addr);
    return pci_inl(0xCFC);
}

void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  << 8)  | (off & 0xFC);
    pci_outl(0xCF8, addr);
    pci_outl(0xCFC, v);
}

static const char *class_name(uint8_t cls, uint8_t sub, uint8_t prog_if) {
    if (cls == 0x00) return "legacy";
    if (cls == 0x01) return "storage";
    if (cls == 0x02) return "network";
    if (cls == 0x03) return "display";
    if (cls == 0x04) return "multimedia";
    if (cls == 0x05) return "memory";
    if (cls == 0x06) return "bridge";
    if (cls == 0x07) return "comm";
    if (cls == 0x0C && sub == 0x03) {
        switch (prog_if) {
        case PCI_PROGIF_UHCI: return "USB UHCI";
        case PCI_PROGIF_OHCI: return "USB OHCI";
        case PCI_PROGIF_EHCI: return "USB EHCI";
        case PCI_PROGIF_XHCI: return "USB xHCI";
        default:              return "USB ?";
        }
    }
    if (cls == 0x0C) return "serial";
    return "?";
}

static void describe(const struct pci_dev *d) {
    serial_puts("PCI ");
    serial_puthex(d->bus);
    serial_puts(":");
    serial_puthex(d->dev);
    serial_puts(".");
    serial_puthex(d->fn);
    serial_puts("  ");
    serial_puthex(d->vendor);
    serial_puts(":");
    serial_puthex(d->device);
    serial_puts("  class=");
    serial_puthex(d->class_code);
    serial_puts("/");
    serial_puthex(d->subclass);
    serial_puts("/");
    serial_puthex(d->prog_if);
    serial_puts("  ");
    serial_puts(class_name(d->class_code, d->subclass, d->prog_if));
    if (d->class_code == PCI_CLASS_SERIAL_BUS && d->subclass == PCI_SUB_USB) {
        serial_puts("  BAR0=");
        serial_puthex(d->bar[0]);
        serial_puts("  IRQ=");
        serial_puthex(d->irq);
    }
    serial_puts("\n");
}

static void record_usb(const struct pci_dev *d) {
    if (usb_controller_count >= MAX_USB_CONTROLLERS) return;
    usb_controllers[usb_controller_count++] = *d;
}

static void probe_function(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t vid_did = pci_cfg_read(bus, dev, fn, 0x00);
    if (vid_did == 0xFFFFFFFF || vid_did == 0) return;

    struct pci_dev d = {0};
    d.bus      = bus;
    d.dev      = dev;
    d.fn       = fn;
    d.vendor   = vid_did & 0xFFFF;
    d.device   = vid_did >> 16;

    uint32_t cls_word = pci_cfg_read(bus, dev, fn, 0x08);
    d.revision   = cls_word & 0xFF;
    d.prog_if    = (cls_word >> 8)  & 0xFF;
    d.subclass   = (cls_word >> 16) & 0xFF;
    d.class_code = (cls_word >> 24) & 0xFF;

    for (int i = 0; i < 6; i++) {
        d.bar[i] = pci_cfg_read(bus, dev, fn, 0x10 + i * 4);
    }
    d.irq = pci_cfg_read(bus, dev, fn, 0x3C) & 0xFF;

    describe(&d);

    if (d.class_code == PCI_CLASS_SERIAL_BUS && d.subclass == PCI_SUB_USB) {
        record_usb(&d);
    }
}

void pci_init(void) {
    serial_puts("PCI scan...\n");
    /* Single-tier scan (bus 0..7). Multi-function devices are detected
     * via header-type bit 7 — we don't bother walking those for now. */
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            /* Status line update so a hang inside probe_function or a
             * stuck pci_cfg_read shows the offending BDF on screen. */
            char suf[24];
            const char hex[] = "0123456789ABCDEF";
            int p = 0;
            suf[p++] = 'b'; suf[p++] = 'u'; suf[p++] = 's'; suf[p++] = '=';
            suf[p++] = hex[(bus >> 4) & 0xF]; suf[p++] = hex[bus & 0xF];
            suf[p++] = ' '; suf[p++] = 'd'; suf[p++] = 'e'; suf[p++] = 'v'; suf[p++] = '=';
            suf[p++] = hex[(dev >> 4) & 0xF]; suf[p++] = hex[dev & 0xF];
            suf[p++] = 0;
            klog_iter(suf);

            probe_function(bus, dev, 0);
            uint32_t hdr = pci_cfg_read(bus, dev, 0, 0x0C);
            uint8_t  htype = (hdr >> 16) & 0xFF;
            if (htype & 0x80) {
                for (uint8_t fn = 1; fn < 8; fn++) {
                    probe_function(bus, dev, fn);
                }
            }
        }
    }
    serial_puts("PCI: ");
    serial_puthex(usb_controller_count);
    serial_puts(" USB host controller(s) found\n");

    /* VGA summary so the user can see USB info without COM1 */
    if (usb_controller_count == 0) {
        vga_puts("  [..] PCI: no USB controllers detected\n");
        return;
    }
    for (int i = 0; i < usb_controller_count; i++) {
        const struct pci_dev *d = &usb_controllers[i];
        const char *hc;
        switch (d->prog_if) {
        case PCI_PROGIF_UHCI: hc = "UHCI"; break;
        case PCI_PROGIF_OHCI: hc = "OHCI"; break;
        case PCI_PROGIF_EHCI: hc = "EHCI"; break;
        case PCI_PROGIF_XHCI: hc = "xHCI"; break;
        default:              hc = "USB?"; break;
        }
        char line[80];
        const char *p;
        int n = 0;
        line[n++] = ' ';
        line[n++] = ' ';
        line[n++] = '[';
        for (p = hc; *p; p++) line[n++] = *p;
        line[n++] = ']';
        line[n++] = ' ';
        for (p = "USB HC at "; *p; p++) line[n++] = *p;
        const char hex[] = "0123456789ABCDEF";
        line[n++] = hex[(d->bus >> 4) & 0xF];
        line[n++] = hex[d->bus & 0xF];
        line[n++] = ':';
        line[n++] = hex[(d->dev >> 4) & 0xF];
        line[n++] = hex[d->dev & 0xF];
        line[n++] = '.';
        line[n++] = hex[d->fn & 0xF];
        for (p = "  VID:"; *p; p++) line[n++] = *p;
        for (int s = 12; s >= 0; s -= 4) line[n++] = hex[(d->vendor >> s) & 0xF];
        line[n++] = ' ';
        line[n++] = 'I';
        line[n++] = 'R';
        line[n++] = 'Q';
        line[n++] = ':';
        line[n++] = hex[(d->irq >> 4) & 0xF];
        line[n++] = hex[d->irq & 0xF];
        line[n++] = '\n';
        line[n]   = '\0';
        vga_puts(line);
    }
}

int pci_usb_count(void) {
    return usb_controller_count;
}

const struct pci_dev *pci_usb_get(int index) {
    if (index < 0 || index >= usb_controller_count) return 0;
    return &usb_controllers[index];
}

/*  — generic class-match enumerator. Re-scans the bus on each
 * call (cheap: single-tier scan, ~10ms). prog_if=0xFF wildcards.
 * Returns 0 on match (fills *out), -1 if no Nth match. */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   struct pci_dev *out, int index) {
    int seen = 0;
    if (!out) return -1;

    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint8_t  max_fn = 1;
            uint32_t hdr;

            hdr = pci_cfg_read(bus, dev, 0, 0x0C);
            if (((hdr >> 16) & 0x80)) max_fn = 8;

            for (uint8_t fn = 0; fn < max_fn; fn++) {
                uint32_t vid_did = pci_cfg_read(bus, dev, fn, 0x00);
                uint32_t cls_word;
                uint8_t cls, sub, pi;
                int i;

                if (vid_did == 0xFFFFFFFFu || vid_did == 0) continue;
                cls_word = pci_cfg_read(bus, dev, fn, 0x08);
                pi  = (cls_word >> 8)  & 0xFF;
                sub = (cls_word >> 16) & 0xFF;
                cls = (cls_word >> 24) & 0xFF;

                if (cls != class_code) continue;
                if (sub != subclass)   continue;
                if (prog_if != 0xFF && pi != prog_if) continue;

                if (seen++ < index) continue;

                /* Populate the out struct. */
                out->bus      = bus;
                out->dev      = dev;
                out->fn       = fn;
                out->vendor   = vid_did & 0xFFFF;
                out->device   = vid_did >> 16;
                out->class_code = cls;
                out->subclass = sub;
                out->prog_if  = pi;
                out->revision = cls_word & 0xFF;
                for (i = 0; i < 6; i++)
                    out->bar[i] = pci_cfg_read(bus, dev, fn, 0x10 + i * 4);
                out->irq = pci_cfg_read(bus, dev, fn, 0x3C) & 0xFF;
                return 0;
            }
        }
    }
    return -1;
}
