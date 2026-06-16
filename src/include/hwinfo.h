#ifndef HWINFO_H
#define HWINFO_H

/* hwinfo — combined lspci/lsusb-style hardware inventory.
 *
 * Walks PCI bus, ATA/ATAPI drives, mounted FAT volumes, loaded .kmd
 * modules, and Intel WiFi cards (recognised by VID:DID), feeding each
 * line through the caller-supplied emit() callback.
 *
 * Two call sites:
 *   - kernel boot tail (main.c) passes emit = vga_puts so the dump
 *     lands on the VGA screen before the shell starts. Needed on
 *     real hardware that lacks a USB keyboard (Vortex86) — the user
 *     photographs the screen to capture the diagnostic.
 *   - `hwinfo` builtin in shell.c passes an emit that routes through
 *     the active VT.
 *
 * No state — pure read of detected hardware via existing kernel APIs
 * (pci.c, ata.c, fat.c, module.c).
 */
void hwinfo_dump(void (*emit)(const char *));

#endif
