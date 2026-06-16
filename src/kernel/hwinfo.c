/* hwinfo.c — Hardware inventory dump.
 *
 * Builds short lines and feeds them through the caller's emit() so
 * the same dump can land on VGA (boot) or a VT (shell). See hwinfo.h.
 *
 * Each section prints a header line then per-item lines, with a final
 * blank line as separator. PCI section formats lspci-ish:
 *   BB:DD.F  VID:DID  cc.ss.pp  <description>
 * lifting class-code → description from a small static table.
 *
 * Intel WiFi recognition is by VID:DID pair. The known table:
 *   8086:4220  PRO/Wireless 2200BG
 *   8086:4222  PRO/Wireless 2915ABG (Vortex86 rig per memory)
 *   8086:4223  PRO/Wireless 2915ABG
 *   8086:4227  PRO/Wireless 2915ABG
 */

#include "types.h"
#include "hwinfo.h"
#include "pci.h"
#include "ata.h"
#include "fat.h"
#include "module.h"

/* ---- tiny printf-free helpers ---------------------------------------- */

static char *u_to_hex(uint32_t v, int digits, char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--) {
        buf[i] = hex[v & 0xF];
        v >>= 4;
    }
    buf[digits] = '\0';
    return buf;
}

static char *u_to_dec(uint32_t v, char *buf, int buflen) {
    char tmp[12];
    int n = 0, i;
    if (v == 0) { tmp[n++] = '0'; }
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (i = 0; i < n && i < buflen - 1; i++) buf[i] = tmp[n - 1 - i];
    buf[i] = '\0';
    return buf;
}

static char *str_cat(char *dst, const char *src, int *pos, int cap) {
    while (*src && *pos < cap - 1) dst[(*pos)++] = *src++;
    dst[*pos] = '\0';
    return dst;
}

/* ---- PCI class-code descriptions ------------------------------------- */

static const char *pci_class_desc(uint8_t cc, uint8_t sub, uint8_t pi) {
    switch (cc) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (sub) {
        case 0x01: return "Storage: IDE controller";
        case 0x06: return "Storage: SATA";
        case 0x08: return "Storage: NVMe";
        default:   return "Storage";
        }
    case 0x02: return "Network controller";
    case 0x03:
        switch (sub) {
        case 0x00: return "Display: VGA-compatible";
        case 0x01: return "Display: XGA";
        case 0x02: return "Display: 3D";
        default:   return "Display";
        }
    case 0x04: return "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        case 0x80: return "Other bridge";
        default:   return "Bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "Generic system peripheral";
    case 0x09: return "Input device";
    case 0x0A: return "Docking station";
    case 0x0B: return "Processor";
    case 0x0C:
        if (sub == 0x03) {
            switch (pi) {
            case 0x00: return "USB host: UHCI (USB 1.1)";
            case 0x10: return "USB host: OHCI (USB 1.1)";
            case 0x20: return "USB host: EHCI (USB 2.0)";
            case 0x30: return "USB host: xHCI (USB 3.0)";
            default:   return "USB host";
            }
        }
        return "Serial bus controller";
    case 0x0D: return "Wireless controller";
    case 0x0E: return "Intelligent I/O";
    case 0x0F: return "Satellite communication";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing";
    default:   return "?";
    }
}

/* Known Intel WiFi VID:DID pairs (from memory `project_vortex86_wifi_card`
 * + ipw2200 supported list). Hit returns the friendly name; miss returns
 * NULL so the caller falls back to the generic class description. */
static const char *intel_wifi_name(uint16_t vid, uint16_t did) {
    if (vid != 0x8086) return 0;
    switch (did) {
    case 0x4220: return "Intel PRO/Wireless 2200BG";
    case 0x4222: return "Intel PRO/Wireless 2915ABG";
    case 0x4223: return "Intel PRO/Wireless 2915ABG";
    case 0x4227: return "Intel PRO/Wireless 2915ABG";
    /* iwlwifi-era cards — listed for completeness; iwi.kmd won't bind. */
    case 0x4232: return "Intel WiFi Link 5100";
    case 0x4237: return "Intel WiFi Link 5100";
    default:     return 0;
    }
}

/* ---- main dump ------------------------------------------------------- */

void hwinfo_dump(void (*emit)(const char *)) {
    char line[96];
    int  pos;
    int  i;

    if (!emit) return;

    emit("\n");
    emit("==== HWINFO — Pinecore hardware inventory ====\n");
    emit("\n");

    /* -------- PCI devices (bus 0 only — pinecore is single-tier) -------- */
    emit("PCI devices:\n");
    {
        int    found = 0;
        uint8_t bus, dev, fn;
        for (bus = 0; bus < 8; bus++) {
            for (dev = 0; dev < 32; dev++) {
                int max_fn = 1;
                for (fn = 0; fn < max_fn; fn++) {
                    uint32_t id = pci_cfg_read(bus, dev, fn, 0x00);
                    uint16_t vid = id & 0xFFFF;
                    uint16_t did = (id >> 16) & 0xFFFF;
                    uint32_t cls, hdr;
                    uint8_t  cc, sub, pi;
                    const char *desc;
                    const char *wifi;
                    char hex4[5];

                    if (vid == 0xFFFF || vid == 0x0000) continue;

                    cls = pci_cfg_read(bus, dev, fn, 0x08);
                    pi  = (cls >> 8)  & 0xFF;
                    sub = (cls >> 16) & 0xFF;
                    cc  = (cls >> 24) & 0xFF;

                    hdr = pci_cfg_read(bus, dev, fn, 0x0C);
                    if (fn == 0 && (((hdr >> 16) & 0xFF) & 0x80)) max_fn = 8;

                    desc = pci_class_desc(cc, sub, pi);
                    wifi = intel_wifi_name(vid, did);

                    pos = 0;
                    str_cat(line, "  ", &pos, sizeof(line));
                    line[pos++] = "0123456789ABCDEF"[(bus >> 4) & 0xF];
                    line[pos++] = "0123456789ABCDEF"[bus & 0xF];
                    line[pos++] = ':';
                    line[pos++] = "0123456789ABCDEF"[(dev >> 4) & 0xF];
                    line[pos++] = "0123456789ABCDEF"[dev & 0xF];
                    line[pos++] = '.';
                    line[pos++] = "0123456789ABCDEF"[fn & 0xF];
                    str_cat(line, "  ", &pos, sizeof(line));
                    u_to_hex(vid, 4, hex4); str_cat(line, hex4, &pos, sizeof(line));
                    line[pos++] = ':';
                    u_to_hex(did, 4, hex4); str_cat(line, hex4, &pos, sizeof(line));
                    str_cat(line, "  ", &pos, sizeof(line));
                    line[pos++] = "0123456789ABCDEF"[(cc >> 4) & 0xF];
                    line[pos++] = "0123456789ABCDEF"[cc & 0xF];
                    line[pos++] = '.';
                    line[pos++] = "0123456789ABCDEF"[(sub >> 4) & 0xF];
                    line[pos++] = "0123456789ABCDEF"[sub & 0xF];
                    line[pos++] = '.';
                    line[pos++] = "0123456789ABCDEF"[(pi >> 4) & 0xF];
                    line[pos++] = "0123456789ABCDEF"[pi & 0xF];
                    str_cat(line, "  ", &pos, sizeof(line));
                    str_cat(line, wifi ? wifi : desc, &pos, sizeof(line));
                    line[pos++] = '\n';
                    line[pos]   = '\0';
                    emit(line);
                    found++;
                }
            }
        }
        if (!found) emit("  (no PCI devices)\n");
    }
    emit("\n");

    /* -------- USB controllers (already collected at pci_init) -------- */
    emit("USB host controllers:\n");
    {
        int n = pci_usb_count();
        if (n == 0) {
            emit("  (none detected)\n");
        } else {
            for (i = 0; i < n; i++) {
                const struct pci_dev *u = pci_usb_get(i);
                const char *kind;
                char hex4[5];

                if (!u) continue;
                switch (u->prog_if) {
                case PCI_PROGIF_UHCI: kind = "UHCI"; break;
                case PCI_PROGIF_OHCI: kind = "OHCI"; break;
                case PCI_PROGIF_EHCI: kind = "EHCI"; break;
                case PCI_PROGIF_XHCI: kind = "xHCI"; break;
                default:              kind = "?";    break;
                }
                pos = 0;
                str_cat(line, "  ", &pos, sizeof(line));
                line[pos++] = "0123456789ABCDEF"[(u->bus >> 4) & 0xF];
                line[pos++] = "0123456789ABCDEF"[u->bus & 0xF];
                line[pos++] = ':';
                line[pos++] = "0123456789ABCDEF"[(u->dev >> 4) & 0xF];
                line[pos++] = "0123456789ABCDEF"[u->dev & 0xF];
                line[pos++] = '.';
                line[pos++] = "0123456789ABCDEF"[u->fn & 0xF];
                str_cat(line, "  ", &pos, sizeof(line));
                str_cat(line, kind, &pos, sizeof(line));
                str_cat(line, "  ", &pos, sizeof(line));
                u_to_hex(u->vendor, 4, hex4); str_cat(line, hex4, &pos, sizeof(line));
                line[pos++] = ':';
                u_to_hex(u->device, 4, hex4); str_cat(line, hex4, &pos, sizeof(line));
                str_cat(line, "  IRQ ", &pos, sizeof(line));
                {
                    char dec[8];
                    u_to_dec((uint32_t)u->irq, dec, sizeof(dec));
                    str_cat(line, dec, &pos, sizeof(line));
                }
                line[pos++] = '\n';
                line[pos]   = '\0';
                emit(line);
            }
        }
    }
    emit("\n");

    /* -------- Storage (ATA + ATAPI) -------- */
    emit("Storage:\n");
    {
        int found = 0;
        for (i = 0; i < 4; i++) {
            const struct ata_drive *d = ata_get_drive((uint8_t)i);
            char dec[12];
            if (!d || !d->present) continue;
            found++;
            pos = 0;
            str_cat(line, "  ata", &pos, sizeof(line));
            u_to_dec((uint32_t)i, dec, sizeof(dec));
            str_cat(line, dec, &pos, sizeof(line));
            str_cat(line, "  ", &pos, sizeof(line));
            str_cat(line, d->atapi ? "ATAPI" : "ATA  ", &pos, sizeof(line));
            str_cat(line, "  ", &pos, sizeof(line));
            str_cat(line, d->model, &pos, sizeof(line));
            str_cat(line, "  (", &pos, sizeof(line));
            u_to_dec(d->sectors, dec, sizeof(dec));
            str_cat(line, dec, &pos, sizeof(line));
            str_cat(line, " x ", &pos, sizeof(line));
            u_to_dec(d->sector_size, dec, sizeof(dec));
            str_cat(line, dec, &pos, sizeof(line));
            str_cat(line, " B)", &pos, sizeof(line));
            if (d->chs_only) str_cat(line, "  [CHS-only]", &pos, sizeof(line));
            line[pos++] = '\n';
            line[pos]   = '\0';
            emit(line);
        }
        if (!found) emit("  (no ATA/ATAPI drives)\n");
    }
    emit("\n");

    /* -------- Mounted FAT volumes -------- */
    emit("Mounted volumes:\n");
    {
        int any = 0;
        for (i = 0; i < FAT_MAX_DRIVES; i++) {
            int      is_floppy = 0;
            uint8_t  ata_id    = 0;
            uint32_t part_lba  = 0;
            char hex8[9];

            if (!fat_is_mounted(i)) continue;
            any = 1;
            fat_get_source(i, &is_floppy, &ata_id, &part_lba);

            pos = 0;
            str_cat(line, "  ", &pos, sizeof(line));
            line[pos++] = 'A' + (char)i;
            str_cat(line, ":  ", &pos, sizeof(line));
            if (is_floppy) {
                str_cat(line, "floppy", &pos, sizeof(line));
            } else {
                str_cat(line, "ata", &pos, sizeof(line));
                {
                    char dec[8];
                    u_to_dec((uint32_t)ata_id, dec, sizeof(dec));
                    str_cat(line, dec, &pos, sizeof(line));
                }
                str_cat(line, "  lba=0x", &pos, sizeof(line));
                u_to_hex(part_lba, 8, hex8);
                str_cat(line, hex8, &pos, sizeof(line));
            }
            line[pos++] = '\n';
            line[pos]   = '\0';
            emit(line);
        }
        if (!any) emit("  (none)\n");
    }
    emit("\n");

    /* -------- Loaded .kmd modules -------- */
    emit("Loaded modules:\n");
    {
        struct loaded_module *m = module_list_head();
        int any = 0;
        while (m) {
            any = 1;
            pos = 0;
            str_cat(line, "  ", &pos, sizeof(line));
            str_cat(line, m->name, &pos, sizeof(line));
            str_cat(line, "  exports=", &pos, sizeof(line));
            {
                char dec[8];
                u_to_dec(m->kexport_count, dec, sizeof(dec));
                str_cat(line, dec, &pos, sizeof(line));
            }
            if (!m->license_gpl)
                str_cat(line, "  [non-GPL]", &pos, sizeof(line));
            line[pos++] = '\n';
            line[pos]   = '\0';
            emit(line);
            m = m->next;
        }
        if (!any) emit("  (no modules loaded)\n");
    }
    emit("\n");

    emit("==== end hwinfo ====\n");
}
