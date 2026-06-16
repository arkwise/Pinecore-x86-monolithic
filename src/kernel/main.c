/* main.c -- Kernel entry point
 *
 * Called from boot.asm after GDT is loaded.
 * Init sequence: serial, VGA, IDT, PIC, PIT, PMM, VMM, heap.
 */

#include "types.h"
#include "serial.h"
#include "vga.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "keyboard.h"
#include "mouse.h"
#include "ata.h"
#include "fat.h"
#include "mount.h"
#include "dos.h"
#include "tss.h"
#include "v86.h"
#include "comload.h"
#include "exeload.h"
#include "sched.h"
#include "vt.h"
#include "shell.h"
#include "fdc.h"
#include "rtc.h"
#include "vbe.h"
#include "dpmi.h"
#include "vcpi.h"
#include "dma.h"
#include "pci.h"
#include "module.h"
#include "config.h"
#include "net.h"
#include "v86_kbd.h"
#include "klog.h"
#include "hwinfo.h"

extern uint32_t _kernel_end;  /* from linker script */

/* Kernel mode — set at compile time via -DKERNEL_MODE_PURE or _DOS */
#ifdef KERNEL_MODE_PURE
#define KERNEL_MODE_NAME "PURE"
#define KERNEL_MODE_IS_PURE 1
#else
#define KERNEL_MODE_NAME "DOS"
#define KERNEL_MODE_IS_PURE 0
#endif

/* 32 MB default memory for QEMU testing */
#define DEFAULT_MEM_KB (32 * 1024)

/* Heap sits right after kernel in memory */
#define HEAP_SIZE (256 * 1024)  /* 256 KB kernel heap */

/* PIT tick handler -- IRQ 0, INT 32 */
static void timer_handler(uint32_t int_no, uint32_t err_code,
                          uint32_t eip, uint32_t cs, uint32_t eflags) {
    (void)int_no; (void)err_code; (void)eip; (void)cs; (void)eflags;

    extern void pit_tick(void);
    pit_tick();
    pic_eoi(0);
}

static void print_ok(const char *what) {
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("  [");
    vga_set_color(VGA_LGREEN, VGA_BLACK);
    vga_puts("OK");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("] ");
    vga_puts(what);
    vga_putc('\n');
}

/* Return-to-DOS: jump to PINE.COM's return routine.
 * Address stored at physical 0x504 by PINE.COM before entering PM.
 * GDT pointer at 0x508. Only works when booted from FreeDOS (not Multiboot). */
void pinecore_exit(void) {
    uint32_t return_addr = *(volatile uint32_t *)0x504;
    uint32_t gdt_addr    = *(volatile uint32_t *)0x508;

    if (return_addr == 0) {
        /* Not booted from PINE.COM — can't return to DOS */
        serial_puts("EXIT: no return-to-DOS path (Multiboot?)\n");
        serial_puts("EXIT: halting.\n");
        __asm__ volatile("cli; hlt");
        return;
    }

    serial_puts("EXIT: returning to FreeDOS...\n");

    __asm__ volatile("cli");

    /* Reload PINE.COM's GDT (has 16-bit segments at 0x30/0x38) */
    __asm__ volatile("lgdt (%0)" : : "r"(gdt_addr));

    /* Jump to return_to_dos in PINE.COM */
    {
        struct { uint32_t offset; uint16_t selector; } __attribute__((packed))
            target = { return_addr, 0x08 };
        __asm__ volatile("ljmp *%0" : : "m"(target));
    }

    /* Never reached */
    while (1) __asm__ volatile("hlt");
}

/* Idle task — runs when no other task is READY.
 * Just halts the CPU until the next interrupt. */
static void idle_entry(void) {
    while (1)
        __asm__ volatile("sti; hlt");
}

/* Auto-load every .KMD found in \DRIVERS\ on the current drive.
 *
 * Multi-pass to satisfy ordering: a module whose unresolved symbols come
 * from another not-yet-loaded module fails the first pass. As long as
 * each pass loads at least one previously-failed module, we retry. This
 * is enough to handle usbcore → uhci/hid/msc without a real dependency
 * graph — MODULE_DEPENDS comes later. FAT directory-entry order is
 * arbitrary (creation order, not alphabetical) so we cannot rely on
 * filename sorting. */
#define AUTOLOAD_MAX_MODULES 32

static void autoload_drivers(void) {
    char saved_cwd[FAT_MAX_PATH];
    struct fat_find find;
    char names[AUTOLOAD_MAX_MODULES][16];
    uint32_t sizes[AUTOLOAD_MAX_MODULES];
    uint8_t  loaded[AUTOLOAD_MAX_MODULES];
    int n = 0;
    int k;

    if (fat_getcwd(saved_cwd, sizeof(saved_cwd)) < 0)
        saved_cwd[0] = 0;

    if (fat_chdir("\\DRIVERS") < 0) {
        serial_puts("autoload: no \\DRIVERS directory — skipping\n");
        return;
    }

    /* Pass 0: collect module names + sizes. */
    if (fat_find_first("*.KMD", &find) == 0) {
        do {
            if (find.attr & FAT_ATTR_DIRECTORY) continue;
            if (n >= AUTOLOAD_MAX_MODULES) break;
            for (k = 0; k < 15 && find.name[k]; k++) names[n][k] = find.name[k];
            names[n][k] = 0;
            sizes[n]  = find.size;
            loaded[n] = 0;
            n++;
        } while (fat_find_next(&find) == 0);
    }

    if (n == 0) {
        serial_puts("autoload: no .KMD files found\n");
        goto restore;
    }

    /* s54 Tier-1: kmd allow-list. PCORE.CFG `kmd_allow = NAME1 NAME2 ...`
     * (or implicitly enabled by `hardened = yes`) restricts which kmds
     * the kernel will trust. Modules not on the list are permanently
     * skipped — prevents a USB-stick-swap or FAT corruption from
     * introducing an arbitrary Ring-0 .kmd. Applied here so the file
     * isn't even read into memory. */
    if (config_kmd_allowlist_active()) {
        int refused = 0;
        for (int i = 0; i < n; i++) {
            if (loaded[i]) continue;
            if (!config_kmd_is_allowed(names[i])) {
                loaded[i] = 2;
                refused++;
                serial_puts("autoload: refused ");
                serial_puts(names[i]);
                serial_puts(" (not on kmd_allow list)\n");
            }
        }
        if (refused) {
            serial_puts("autoload: kmd_allow active — refused ");
            serial_puthex((uint32_t)refused);
            serial_puts(" module(s)\n");
        }
    }

    /* PCORE.CFG `usb_enable = no` suppresses the USB stack — used when
     * BIOS USB-legacy on the target hangs the V86 INT 16h path, or when
     * coexisting with another driver project. Skip-list is by filename
     * because the autoload loop runs before any module has loaded, so
     * we cannot ask the modules about their category. */
    if (!config_usb_enabled()) {
        extern int strcmp(const char *, const char *);
        static const char *usb_modules[] = {
            "USBCORE.KMD", "UHCI.KMD", "OHCI.KMD", "EHCI.KMD",
            "XHCI.KMD",    "HID.KMD",  "MSC.KMD",  0
        };
        int skipped = 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; usb_modules[j]; j++) {
                if (strcmp(names[i], usb_modules[j]) == 0) {
                    loaded[i] = 2;  /* permanently skipped */
                    skipped++;
                    break;
                }
            }
        }
        if (skipped) {
            klog_stage("autoload: USB off");
            serial_puts("autoload: usb_enable=no — skipped ");
            serial_puthex((uint32_t)skipped);
            serial_puts(" USB module(s)\n");
        }
    } else {
        klog_stage("autoload: USB on");
        serial_puts("autoload: usb_enable=yes — loading USB stack\n");
    }

    serial_puts("autoload: scanning \\DRIVERS\\*.KMD — ");
    serial_puthex((uint32_t)n);
    serial_puts(" file(s)\n");

    /* Retry passes until no further progress. Each pass attempts every
     * not-yet-loaded module; success means a previously-blocking export
     * just became visible and another module may now resolve. */
    int progress = 1;
    int pass = 0;
    while (progress) {
        progress = 0;
        pass++;
        for (int i = 0; i < n; i++) {
            if (loaded[i]) continue;
            klog_iter(names[i]);
            int fd = fat_open(names[i], 0);
            if (fd < 0) {
                if (pass == 1) {
                    serial_puts("  open failed: "); serial_puts(names[i]); serial_puts("\n");
                }
                loaded[i] = 2;  /* permanent failure */
                continue;
            }
            void *buf = kmalloc(sizes[i]);
            if (!buf) {
                fat_close(fd);
                serial_puts("  kmalloc failed: "); serial_puts(names[i]); serial_puts("\n");
                loaded[i] = 2;
                continue;
            }
            if ((uint32_t)fat_read(fd, buf, sizes[i]) != sizes[i]) {
                kfree(buf); fat_close(fd);
                serial_puts("  short read: "); serial_puts(names[i]); serial_puts("\n");
                loaded[i] = 2;
                continue;
            }
            fat_close(fd);

            if (module_load_image(names[i], buf, sizes[i]) != NULL) {
                loaded[i] = 1;
                progress = 1;
            } else if (module_last_load_was_init_failure()) {
                /* Deterministic failure — the module loaded fine but its
                 * own module_init returned an error (e.g. NULL.KMD's
                 * net_register_provider returning EADDRINUSE because
                 * LOOPBACK already took the single provider slot, or
                 * R6040.KMD's PCI probe finding no hardware). No future
                 * pass will change this, so mark permanent. */
                loaded[i] = 2;
                serial_puts("autoload: ");
                serial_puts(names[i]);
                serial_puts(" — init returned error, not retrying\n");
            }
            /* Else: unresolved-symbol / other recoverable failure;
             * leave loaded[i] == 0 so the next pass retries once more
             * exports may have become visible. */
            kfree(buf);
        }
    }

    /* Final report. */
    for (int i = 0; i < n; i++) {
        if (loaded[i] == 0) {
            serial_puts("autoload: gave up on ");
            serial_puts(names[i]);
            serial_puts(" (unresolved deps?)\n");
        }
    }

restore:
    if (saved_cwd[0]) fat_chdir(saved_cwd);
    else              fat_chdir("\\");
}

void kernel_main(void) {
    uint32_t heap_start_addr;
    uint32_t free_pages;

    /* Diagnostic VGA marker — row 0, col 79 (far right, won't collide
     * with PCBOOT's `@PG...J` trace at cols 8-21). Visible only if the
     * kernel reaches its C entry on real hardware. Paired with a '2'
     * after stack_chk_init below: '1' alone = stack_chk_init faulted
     * (likely Vortex86 RDTSC #UD). '12' = canary survived, crash later.
     * Cyan-on-black so it's distinguishable from PCBOOT's bright-white. */
    *((volatile uint16_t *)(0xB8000 + 79 * 2)) = 0x0B31;  /* '1' */

    serial_putc('\n');
    serial_init();
    serial_puts("\n=== Pinecore kernel booting [");
    serial_puts(KERNEL_MODE_NAME);
    serial_puts(" mode] ===\n\n");

    /* Seed the stack canary as the very first thing after serial.
     * Implementation in stack_chk.c — uses CMOS+PIC entropy (not
     * RDTSC, which 486-class CPUs may #UD on). */
    {
        extern void stack_chk_init(void);
        stack_chk_init();
    }

    /* Diagnostic VGA marker '2' next to '1'. Visible iff
     * stack_chk_init returned cleanly. */
    *((volatile uint16_t *)(0xB8000 + 78 * 2)) = 0x0B32;  /* '2' */

    /* VGA text mode */
    vga_init();
    vga_set_color(VGA_LGREEN, VGA_BLACK);
    vga_puts("Pinecore v0.2.0.a");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    if (KERNEL_MODE_IS_PURE) {
        vga_puts(" [PURE]");
    } else {
        vga_puts(" [DOS]");
    }
    vga_putc('\n');
    vga_set_color(VGA_DGRAY, VGA_BLACK);
    if (KERNEL_MODE_IS_PURE)
        vga_puts("32-bit protected mode kernel\n\n");
    else
        vga_puts("DOS compatibility kernel\n\n");

    /* IDT + PIC */
    klog_stage("init: IDT");
    serial_puts("IDT init...\n");
    idt_init();
    print_ok("IDT - 48 gates");

    klog_stage("init: TSS");
    serial_puts("TSS init...\n");
    tss_init();
    print_ok("TSS - task state segment");

    klog_stage("init: PIC remap");
    serial_puts("PIC remap...\n");
    pic_init();

    /* Mask all IRQs initially */
    pic_mask(0); pic_mask(1); pic_mask(2); pic_mask(3);
    pic_mask(4); pic_mask(5); pic_mask(6); pic_mask(7);
    pic_mask(8); pic_mask(9); pic_mask(10); pic_mask(11);
    pic_mask(12); pic_mask(13); pic_mask(14); pic_mask(15);
    pic_unmask(IRQ_CASCADE);
    print_ok("PIC - remapped to INT 32-47");

    /* PIT timer at 100 Hz */
    klog_stage("init: PIT 100Hz");
    serial_puts("PIT init...\n");
    isr_register(32, timer_handler);
    pit_init(100);
    print_ok("PIT - 100 Hz timer (audio/timing)");

    /* RTC periodic interrupt — scheduler preemption clock */
    klog_stage("init: RTC 8192Hz");
    serial_puts("RTC init...\n");
    rtc_init(8192);
    print_ok("RTC - 8192 Hz preemption clock");

    /* Arm the boot watchdog. From here until sched_start() the kernel
     * must call klog_stage() or klog_iter() at least once every 15s
     * or the RTC IRQ fires kernel_panic_watchdog() with the last
     * stage label — a BSOD instead of a silent CLI'd hang. Disarmed
     * just before sched_start() (idle != hang). */
    klog_watchdog_arm(15);

    /* Physical memory manager */
    klog_stage("init: PMM");
    serial_puts("PMM init...\n");
    pmm_init(DEFAULT_MEM_KB);

    /* Mark first 1MB as used (BIOS, IVT, VGA, etc.) */
    pmm_mark_region_used(0, 0x100000);

    /* Mark kernel region as used (1MB to _kernel_end + heap) */
    heap_start_addr = ((uint32_t)&_kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    pmm_mark_region_used(0x100000, (heap_start_addr + HEAP_SIZE) - 0x100000);

    /* Mark usable RAM as free (after kernel + heap to end of RAM) */
    pmm_mark_region_free(heap_start_addr + HEAP_SIZE,
                         (DEFAULT_MEM_KB * 1024) - (heap_start_addr + HEAP_SIZE));

    free_pages = pmm_get_free_count();
    print_ok("PMM - physical page allocator");

    serial_puts("  Free pages: ");
    serial_puthex(free_pages);
    serial_puts("\n");

    /* Virtual memory -- identity-map first 4MB, enable paging */
    klog_stage("init: VMM (paging)");
    serial_puts("VMM init...\n");
    vmm_init();
    print_ok("VMM - paging enabled");

    /* Kernel heap */
    klog_stage("init: heap");
    serial_puts("Heap init...\n");
    heap_init(heap_start_addr, HEAP_SIZE);
    print_ok("Heap - 256 KB kernel malloc");

    /* DMA region — bus-master allocations for USB / NIC / audio */
    klog_stage("init: DMA region");
    serial_puts("DMA init...\n");
    dma_init();
    print_ok("DMA - 256 KB region at 0x00200000");

    /* Kernel-module subsystem — walks .kexport, builds symbol table */
    klog_stage("init: module subsystem");
    serial_puts("Module subsystem init...\n");
    module_init_subsystem();
    print_ok("Modules - kexports indexed");

    /* Test heap */
    {
        void *a = kmalloc(64);
        void *b = kmalloc(128);
        serial_puts("  kmalloc(64)  = ");
        serial_puthex((uint32_t)a);
        serial_puts("\n  kmalloc(128) = ");
        serial_puthex((uint32_t)b);
        serial_puts("\n");
        kfree(a);
        kfree(b);
        serial_puts("  kfree OK\n");
    }

    /* Enable interrupts */
    klog_stage("init: enabling IRQs (sti)");
    serial_puts("Enabling interrupts...\n");
    __asm__ volatile ("sti");

    /* Keyboard */
    klog_stage("init: keyboard (PS/2)");
    serial_puts("Keyboard init...\n");
    keyboard_init();
    print_ok("Keyboard - PS/2 scancode set 1");

    /* Mouse */
    klog_stage("init: mouse (PS/2)");
    serial_puts("Mouse init...\n");
    mouse_init();
    print_ok("Mouse - PS/2 3-byte packets");

    /* ATA/IDE disk */
    klog_stage("init: ATA/IDE probe");
    serial_puts("ATA init...\n");
    ata_init();
    if (ata_get_drive_count() > 0) {
        const struct ata_drive *d = ata_get_drive(0);
        print_ok("ATA - drive detected");

        /* Test: read MBR (sector 0) */
        {
            uint8_t mbr[512];
            if (ata_read(0, 0, 1, mbr) == 0) {
                if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
                    serial_puts("  MBR signature valid (0x55AA)\n");
                    print_ok("Disk read - MBR verified");
                } else {
                    serial_puts("  MBR read OK but no boot signature\n");
                    print_ok("Disk read - sector 0 OK");
                }
            } else {
                serial_puts("  MBR read FAILED\n");
            }
        }
        (void)d;
    } else {
        print_ok("ATA - no drives");
    }

    /* Always init FDC — we want both A: and C: available */
    klog_stage("init: FDC (floppy)");
    serial_puts("FDC init...\n");
    fdc_init();
    if (fdc_detect())
        print_ok("FDC - floppy drive detected");
    else
        print_ok("FDC - no floppy drive");

    /* Drive auto-mount — walks each ATA drive's MBR, mounts every FAT
     * partition into the next free letter starting at C:; floppy claims
     * A:. Replaces the s59 hardcoded fat_mount_ata(FAT_DRIVE_C, 0, 0) +
     * fat_mount_fdc() block. ATAPI drives are listed but not mounted
     * until the ISO9660 driver lands (M3). See docs/design/MOUNT-STRATEGY.md. */
    klog_stage("init: mount (auto-discover)");
    mount_init();
    if (fat_is_mounted(FAT_DRIVE_C))
        print_ok("Mount - C: ready");
    else if (fat_is_mounted(FAT_DRIVE_A))
        print_ok("Mount - A: ready (no HDD)");
    else
        print_ok("Mount - no drives mounted");

    /* PCORE.CFG — read config keys (must come after FAT mount) */
    klog_stage("init: PCORE.CFG parse");
    serial_puts("Config init...\n");
    config_init();
    print_ok("Config - PCORE.CFG parsed");

    /* Quick test: read HELLO.TXT from default drive */
    {
        int fd = fat_open("HELLO.TXT", 0);
        if (fd >= 0) {
            char buf[128];
            int n, k;
            for (k = 0; k < 128; k++) buf[k] = 0;
            n = fat_read(fd, buf, 127);
            if (n > 0) {
                serial_puts("  Read HELLO.TXT: \"");
                serial_puts(buf);
                serial_puts("\"\n");
            }
            fat_close(fd);
        }
    }

    /* VESA/VBE graphics (detect only — don't set mode yet) */
    klog_stage("init: VBE detect");
    serial_puts("VBE init...\n");
    if (vbe_init())
        print_ok("VBE - Bochs VBE SVGA detected");
    else
        print_ok("VBE - no Bochs VBE (real VBE needs V86)");

    /* PCI bus enumeration — needed for any bus-master driver */
    klog_stage("init: PCI scan");
    serial_puts("PCI init...\n");
    pci_init();
    klog_iter("");
    print_ok("PCI - bus enumerated");

    /* DOS INT 21h emulation */
    klog_stage("init: DOS INT 21h");
    serial_puts("DOS emulation init...\n");
    dos_init();
    print_ok("DOS - INT 21h emulation (40+ functions)");

    /* V86 monitor */
    klog_stage("init: V86 monitor");
    serial_puts("V86 init...\n");
    v86_init();
    print_ok("V86 - monitor ready");

    /* VCPI 1.0 server — only in pure mode (DOS mode has stale handler issues) */
    if (KERNEL_MODE_IS_PURE) {
        klog_stage("init: VCPI 1.0 server");
        serial_puts("VCPI init...\n");
        vcpi_init();
        print_ok("VCPI - 1.0 server (PM DOS apps)");
    }

    /* DPMI 0.9 host */
    klog_stage("init: DPMI 0.9 host");
    serial_puts("DPMI init...\n");
    dpmi_init();
    print_ok("DPMI - 0.9 host (32-bit PM apps)");

    /* Network syscall dispatch — installs INT 0x80, prepares fd table */
    klog_stage("init: net (INT 0x80)");
    serial_puts("Net init...\n");
    net_init();
    print_ok("Net - INT 0x80 syscall dispatch ready");

    /* Preemptive scheduler */
    klog_stage("init: scheduler");
    serial_puts("Scheduler init...\n");
    sched_init();
    print_ok("Scheduler - preemptive multitasking ready");

    /* V86 BIOS-INT-16h keyboard polling — opt-in via PCORE.CFG */
    if (config_kbd_v86_enabled()) {
        klog_stage("init: V86 kbd polling");
        serial_puts("V86 kbd init...\n");
        v86_kbd_init();
        print_ok("V86 kbd - BIOS INT 16h polling task");
    }

    /* Auto-load all .KMD modules from \DRIVERS\ on the current drive */
    klog_stage("init: autoload \\DRIVERS\\*.KMD");
    autoload_drivers();
    klog_iter("");

    /* Hardware inventory dump. Lands on VGA + serial before the shell
     * splash so real hardware that lacks a keyboard (Vortex86 USB-only
     * boards) still surfaces what was detected — user photographs the
     * screen to capture PCI / ATA / WiFi / module state for triage. */
    klog_stage("init: hwinfo dump");
    hwinfo_dump(vga_puts);
    hwinfo_dump(serial_puts);

    /* DOS mode: verify COMMAND.COM is readable */
    if (!KERNEL_MODE_IS_PURE) {
        int fd = fat_open("COMMAND.COM", 0);
        if (fd >= 0) {
            fat_close(fd);
            print_ok("FAT - COMMAND.COM found");
        }
    }

    /* ================================================================
     * Virtual Terminal System + Pinecore Shell
     * ================================================================ */
    klog_stage("init: VT subsystem");
    serial_puts("VT init...\n");
    vt_init();
    print_ok("VT - virtual terminal system");

    /* Create VT1 with Pinecore shell */
    {
        int vt0 = vt_create(VT_SHELL);
        vt_switch(vt0);
        keyboard_set_vt_mode(1);

        sched_create_kernel_task("pine-shell", shell_entry, vt0);
        sched_create_kernel_task("vt-manager", vt_manager_entry, -1);
        sched_create_kernel_task("idle", idle_entry, -1);

        /* In PURE mode, auto-launch COMMAND.COM for AUTOEXEC.BAT */
        if (KERNEL_MODE_IS_PURE) {
            extern int vt_create_dos(void);
            vt_create_dos();
        }
    }

    klog_stage("starting scheduler - entering shell");
    serial_puts("\n*** Starting Pinecore [");
    serial_puts(KERNEL_MODE_NAME);
    serial_puts("] ***\n\n");
    /* Disarm boot watchdog: a quiescent shell waiting on input is not
     * a hang. Any future runtime watchdog work would re-arm against a
     * different progress metric (e.g. scheduler tick count). */
    klog_watchdog_disarm();
    sched_start();  /* never returns — jumps to shell task */

    /* Should never reach here */
    serial_puts("ERROR: sched_start returned!\n");
    while (1) __asm__ volatile ("cli; hlt");
}
