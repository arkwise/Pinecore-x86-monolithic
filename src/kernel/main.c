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
            }
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

    serial_putc('\n');
    serial_init();
    serial_puts("\n=== Pinecore kernel booting [");
    serial_puts(KERNEL_MODE_NAME);
    serial_puts(" mode] ===\n\n");

    /* VGA text mode */
    vga_init();
    vga_set_color(VGA_LGREEN, VGA_BLACK);
    vga_puts("Pinecore v0.2");
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

    /* FAT filesystem — mount all available drives */
    if (ata_get_drive_count() > 0) {
        klog_stage("init: FAT mount C: (HDD)");
        serial_puts("FAT mount C: (HDD)...\n");
        if (fat_mount_ata(FAT_DRIVE_C, 0, 0) == 0) {
            print_ok("FAT C: - HDD mounted");
        } else {
            serial_puts("FAT C: mount failed\n");
        }
    }

    if (fdc_detect()) {
        klog_stage("init: FAT mount A: (floppy)");
        serial_puts("FAT mount A: (floppy)...\n");
        if (fat_mount_fdc() == 0) {
            print_ok("FAT A: - floppy mounted");
        } else {
            serial_puts("FAT A: floppy mount failed\n");
        }
    }

    /* Set default drive: C: if available, else A: */
    if (fat_is_mounted(FAT_DRIVE_C))
        fat_set_drive(FAT_DRIVE_C);
    else if (fat_is_mounted(FAT_DRIVE_A))
        fat_set_drive(FAT_DRIVE_A);

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

    klog_stage("starting scheduler — entering shell");
    serial_puts("\n*** Starting Pinecore [");
    serial_puts(KERNEL_MODE_NAME);
    serial_puts("] ***\n\n");
    sched_start();  /* never returns — jumps to shell task */

    /* Should never reach here */
    serial_puts("ERROR: sched_start returned!\n");
    while (1) __asm__ volatile ("cli; hlt");
}
