/* Kernel symbol exports — the bootstrap ABI for .kmd modules.
 *
 * License: GPL-2.0 for the kernel; symbols here are tagged with either
 *   EXPORT_SYMBOL      (LGPL-friendly, closed-source modules may use)
 * or
 *   EXPORT_SYMBOL_GPL  (GPL-family modules only)
 *
 * Organisation: grouped by subsystem. The full surface synthesized
 * across docs 50-54 is documented in docs/research/54-usbcore-env-
 * synthesis.md §7. As subsystems grow we should colocate their exports
 * with their code (matches Linux's pattern); for now everything lives
 * here so the list is easy to audit.
 */

#include "module.h"
#include "heap.h"
#include "serial.h"
#include "vga.h"
#include "dma.h"
#include "irq.h"
#include "pit.h"
#include "pci.h"
#include "int13.h"
#include "keyboard.h"
#include "mouse.h"
#include "klog.h"

/* ---- Memory + heap ---- */
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);

/* ---- DMA ---- */
EXPORT_SYMBOL(dma_alloc);
EXPORT_SYMBOL(dma_free);
EXPORT_SYMBOL(dma_virt_to_phys);
EXPORT_SYMBOL(dma_free_bytes);

/* ---- Port I/O (non-inline wrappers in port_io.c) ---- */
extern uint8_t  inb(uint16_t);
extern void     outb(uint16_t, uint8_t);
extern uint16_t inw(uint16_t);
extern void     outw(uint16_t, uint16_t);
extern uint32_t inl(uint16_t);
extern void     outl(uint16_t, uint32_t);
EXPORT_SYMBOL(inb);
EXPORT_SYMBOL(outb);
EXPORT_SYMBOL(inw);
EXPORT_SYMBOL(outw);
EXPORT_SYMBOL(inl);
EXPORT_SYMBOL(outl);

/* ---- PCI ---- */
EXPORT_SYMBOL(pci_cfg_read);
EXPORT_SYMBOL(pci_cfg_write);
EXPORT_SYMBOL(pci_find_class);
EXPORT_SYMBOL(pci_usb_count);
EXPORT_SYMBOL(pci_usb_get);

/* ---- IRQ + timing ---- */
EXPORT_SYMBOL(irq_register);
EXPORT_SYMBOL(irq_unregister);
EXPORT_SYMBOL(irq_eoi);
EXPORT_SYMBOL(irq_mask);
EXPORT_SYMBOL(irq_unmask);

EXPORT_SYMBOL(pit_ticks_get);
EXPORT_SYMBOL(pit_delay_ms);
EXPORT_SYMBOL(pit_register_periodic);
EXPORT_SYMBOL(pit_unregister_periodic);

/* ---- Logging ---- */
EXPORT_SYMBOL(serial_puts);
EXPORT_SYMBOL(serial_puthex);
EXPORT_SYMBOL(serial_putc);
EXPORT_SYMBOL(vga_puts);
EXPORT_SYMBOL(klog_stage);
EXPORT_SYMBOL(klog_iter);

/* ---- DOS hand-off sinks (the API surface USB class drivers feed) ---- */
EXPORT_SYMBOL(keyboard_inject_key);                 /* (s51 Path B + HID) */
EXPORT_SYMBOL(keyboard_inject_scancode_sequence);   /* ( — multi-byte HID keys) */
EXPORT_SYMBOL(mouse_inject);                        /* ( — HID mouse) */
EXPORT_SYMBOL(int13h_register_disk);                /* ( — MSC) */
EXPORT_SYMBOL(int13h_unregister_disk);
EXPORT_SYMBOL(int13h_lookup);

/* ---- string helpers (libc) ---- */
extern int    strcmp(const char *, const char *);
extern unsigned long strlen(const char *);
extern void  *memcpy(void *, const void *, unsigned long);
extern void  *memset(void *, int, unsigned long);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memset);
