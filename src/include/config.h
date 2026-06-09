#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

/* Pinecore persistent configuration — Phase 4.6.5 M3.
 *
 * File: C:\PCORE.CFG  (plain text, key=value pairs, # comments).
 * Read once at boot (after FAT mount, before shell start). Saves are
 * triggered by individual setters (e.g. keyboard layout change) and
 * rewrite the whole file from current kernel state.
 *
 * The file is optional — if missing, defaults are used and the kernel
 * keeps running. The setup app (M4) is what populates it the first
 * time. */

/* Read C:\PCORE.CFG if present and apply settings. Silent if missing. */
void config_init(void);

/* Write all current settings back to C:\PCORE.CFG. Returns 0 on
 * success, -1 on FAT error. Called after any setter that should
 * persist (e.g. cmd_layout when the user runs `layout de`). */
int  config_save(void);

/* M4 — first-boot detection. Returns 1 if PCORE.CFG was missing at
 * boot OR explicitly says `firstboot = yes`. Cleared by config_save()
 * (a successful save flips firstboot to "no" in the file). */
int  config_is_firstboot(void);

/* s51 — opt-in for the V86 BIOS-INT-16h keyboard polling task.
 * Returns 1 if PCORE.CFG has `kbd_v86 = yes`. Default OFF because
 * the polling task can hang real hardware (BIOS INT 16h on some
 * boards CLIs while waiting on USB controller). Enable only after
 * the panic-handler infrastructure is in so a hang is recoverable. */
int  config_kbd_v86_enabled(void);

/*  — USB stack policy (doc 54 §8).
 *
 *   usb_enable             : master switch, default 1 (load USB modules)
 *   usb_trace              : verbose COM1 diagnostics, default 0
 *   usb_kbd_initial_delay  : ms before typematic kicks in (HID host-side
 *                            repeat — keyboard doesn't auto-repeat), 500
 *   usb_kbd_repeat_rate    : ms between repeats, default 33 (~30 cps)
 *
 * The DMA region's address/size are NOT yet exposed as config keys —
 * the compile-time DMA_REGION_BASE / DMA_REGION_SIZE in dma.h cover the
 * common case. Future change if memory-constrained boards need to tune. */
int  config_usb_enabled(void);
int  config_usb_trace_enabled(void);
int  config_usb_kbd_initial_delay_ms(void);
int  config_usb_kbd_repeat_rate_ms(void);

/* Phase 4.8 — network-provider .kmd to load at boot. Returns the
 * configured short name (e.g. "e1000e", "pktdrv") or NULL if no
 * `net_provider` key is set. main.c uses this to load
 * /DRIVERS/<name>.KMD after the module subsystem is up. */
const char *config_net_provider(void);

/* Nameserver for the kernel DNS resolver, big-endian IPv4. Returns 0
 * if no `net_dns_server` key is set (DNS will fail with EHOSTUNREACH
 * until configured). */
uint32_t    config_net_dns_server(void);

#endif
