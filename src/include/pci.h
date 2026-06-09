#ifndef PINECORE_PCI_H
#define PINECORE_PCI_H

#include "types.h"

/* PCI config-space access via mech-1 (ports 0xCF8/0xCFC).
 * Bus/dev/fn pack into the standard CONFIG_ADDRESS layout. */
uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);

/* PCI class codes we care about */
#define PCI_CLASS_DISPLAY     0x03
#define PCI_CLASS_SERIAL_BUS  0x0C
#define PCI_SUB_USB           0x03   /* under class 0x0C: USB host controller */

/* PCI USB programming-interface codes (under class 0C / subclass 03) */
#define PCI_PROGIF_UHCI       0x00   /* USB 1.1 Universal HC (Intel)    */
#define PCI_PROGIF_OHCI       0x10   /* USB 1.1 Open HC (Compaq/Microsoft) */
#define PCI_PROGIF_EHCI       0x20   /* USB 2.0 Enhanced HC             */
#define PCI_PROGIF_XHCI       0x30   /* USB 3.0 eXtensible HC           */

struct pci_dev {
    uint8_t  bus, dev, fn;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint32_t bar[6];
    uint8_t  irq;
};

/* Scan all PCI buses, print every device found, and populate the
 * USB-controller list (queryable via pci_usb_count/pci_usb_get). */
void pci_init(void);

/* USB controller table populated by pci_init. */
int                  pci_usb_count(void);
const struct pci_dev *pci_usb_get(int index);

/* Find the Nth PCI device matching the (class, subclass, prog_if)
 * triple. `index` is 0-based; returns 0 on match (and fills *out),
 * -1 if no Nth match exists. Used by uhci.kmd (doc 51 §2) and future
 * HCD/NIC modules to enumerate their kind of device.
 *
 * Pass prog_if = 0xFF to wildcard the programming interface byte. */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   struct pci_dev *out, int index);

#endif
