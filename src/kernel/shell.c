/* shell.c — Pinecore Commando
 *
 * Native kernel-mode command interpreter. Runs as a scheduler task
 * on a virtual terminal. Uses FAT driver directly for file operations,
 * VT system for console I/O.
 *
 * (ch-17)
 */

#include "types.h"
#include "shell.h"
#include "vt.h"
#include "sched.h"
#include "fat.h"
#include "serial.h"
#include "pmm.h"
#include "keyboard.h"
#include "rtc.h"
#include "io.h"
#include "module.h"  /* module_resolve — for cmd_wifi → iwi.kmd */
#include "ata.h"     /* ata_get_drive / ata_get_drive_count — for cmd_mount */
#include "hwinfo.h"  /* hwinfo_dump — for cmd_hwinfo */

/* Shell state.
 * `shell_vt` resolves to the current task's bound VT on every read, so
 * multiple shell instances stay isolated. A single static would leak the
 * last shell's VT into all the older instances and route their output
 * (e.g. `top`'s redraw) onto whichever VT happens to be active. */
static inline int shell_vt_get(void) {
    struct task *t = sched_get_task(sched_current());
    return t ? t->vt : -1;
}
#define shell_vt (shell_vt_get())

/* Command history */
#define HIST_MAX 16
#define HIST_LEN 256
static char history[HIST_MAX][HIST_LEN];
static int hist_count = 0;
static int hist_pos = 0;

/* Arrow key scancodes (with KEY_EXTENDED flag) */
#define SC_UP    (0x48 | KEY_EXTENDED)
#define SC_DOWN  (0x50 | KEY_EXTENDED)
#define SC_LEFT  (0x4B | KEY_EXTENDED)
#define SC_RIGHT (0x4D | KEY_EXTENDED)

/* String helpers (must be before hist_add which uses streq) */
static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

static int strstart(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static void hist_add(const char *cmd) {
    int i;
    if (!cmd[0]) return;  /* don't store empty lines */
    /* Don't store duplicate of last command */
    if (hist_count > 0) {
        int prev = (hist_count - 1) % HIST_MAX;
        if (streq(history[prev], cmd)) return;
    }
    i = hist_count % HIST_MAX;
    {
        int j;
        for (j = 0; cmd[j] && j < HIST_LEN - 1; j++)
            history[i][j] = cmd[j];
        history[i][j] = '\0';
    }
    hist_count++;
}

/* ================================================================
 * Output helpers
 * ================================================================ */

static void print(const char *s) {
    vt_puts(shell_vt, s);
}

static void print_num(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { vt_putc(shell_vt, '0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) vt_putc(shell_vt, buf[--i]);
}

static void print_hex(uint32_t val) {
    const char *hex = "0123456789ABCDEF";
    int i;
    print("0x");
    for (i = 28; i >= 0; i -= 4)
        vt_putc(shell_vt, hex[(val >> i) & 0xF]);
}

static void newline(void) { vt_putc(shell_vt, '\n'); }

/* ================================================================
 * Command line reading
 * ================================================================ */

/* Forward declaration */
static void prompt(void);

/* Clear current line on screen and reprint from buffer */
static void redraw_line(const char *buf, int len) {
    int i;
    /* Move cursor to start of input (after prompt) */
    /* Erase by printing spaces, then reprint */
    vt_putc(shell_vt, '\r');
    prompt();
    for (i = 0; i < len; i++)
        vt_putc(shell_vt, buf[i]);
    /* Clear any leftover characters from previous longer line */
    for (i = len; i < 80; i++)
        vt_putc(shell_vt, ' ');
    /* Move cursor back to end of actual text */
    vt_putc(shell_vt, '\r');
    prompt();
    for (i = 0; i < len; i++)
        vt_putc(shell_vt, buf[i]);
}

static int readline(char *buf, int max) {
    int pos = 0;
    int browse = 0;  /* history browse position (0 = current line) */
    struct key_event ev;

    buf[0] = '\0';

    while (1) {
        while (!vt_poll_key(shell_vt, &ev)) {
            sched_block(BLOCK_KEYBOARD, shell_vt);
        }

        if (!ev.pressed) continue;

        /* Enter — submit */
        if (ev.ascii == '\r' || ev.ascii == '\n') {
            newline();
            buf[pos] = '\0';
            hist_add(buf);
            return pos;
        }

        /* Backspace */
        if (ev.ascii == 8 || ev.scancode == 0x0E) {
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                vt_putc(shell_vt, '\b');
            }
            continue;
        }

        /* Up arrow — previous command */
        if (ev.scancode == SC_UP) {
            if (browse < hist_count && browse < HIST_MAX) {
                browse++;
                int idx = (hist_count - browse) % HIST_MAX;
                if (idx < 0) idx += HIST_MAX;
                /* Copy history entry to buffer */
                pos = 0;
                while (history[idx][pos] && pos < max - 1) {
                    buf[pos] = history[idx][pos];
                    pos++;
                }
                buf[pos] = '\0';
                redraw_line(buf, pos);
            }
            continue;
        }

        /* Down arrow — next command */
        if (ev.scancode == SC_DOWN) {
            if (browse > 1) {
                browse--;
                int idx = (hist_count - browse) % HIST_MAX;
                if (idx < 0) idx += HIST_MAX;
                pos = 0;
                while (history[idx][pos] && pos < max - 1) {
                    buf[pos] = history[idx][pos];
                    pos++;
                }
                buf[pos] = '\0';
                redraw_line(buf, pos);
            } else if (browse == 1) {
                browse = 0;
                pos = 0;
                buf[0] = '\0';
                redraw_line(buf, 0);
            }
            continue;
        }

        /* Tab — filename completion */
        if (ev.ascii == '\t') {
            /* Find the last word in buffer (the partial filename) */
            int word_start = pos;
            while (word_start > 0 && buf[word_start - 1] != ' ') word_start--;
            {
                char partial[64];
                int plen = pos - word_start;
                int k;
                struct fat_find ff;
                char match[13];
                int match_count = 0;

                for (k = 0; k < plen && k < 63; k++)
                    partial[k] = buf[word_start + k];
                partial[plen] = '\0';

                /* Build wildcard pattern */
                char pattern[64];
                for (k = 0; k < plen && k < 58; k++)
                    pattern[k] = partial[k];
                pattern[k++] = '*';
                pattern[k++] = '.';
                pattern[k++] = '*';
                pattern[k] = '\0';

                if (fat_find_first(plen > 0 ? pattern : "*.*", &ff) == 0) {
                    /* Use first match */
                    for (k = 0; ff.name[k] && k < 12; k++)
                        match[k] = ff.name[k];
                    match[k] = '\0';
                    match_count = 1;

                    /* Count total matches */
                    while (fat_find_next(&ff) == 0) match_count++;

                    /* Replace partial with match */
                    pos = word_start;
                    for (k = 0; match[k] && pos < max - 1; k++)
                        buf[pos++] = match[k];
                    buf[pos] = '\0';
                    redraw_line(buf, pos);
                }
            }
            continue;
        }

        /* Printable character */
        if (ev.ascii >= 32 && ev.ascii < 127 && pos < max - 1) {
            buf[pos++] = ev.ascii;
            buf[pos] = '\0';
            vt_putc(shell_vt, ev.ascii);
            browse = 0;  /* reset history browsing on new input */
        }
    }
}

/* ================================================================
 * String helpers
 * ================================================================ */

/* ================================================================
 * Built-in commands
 * ================================================================ */

/* Paged output helper for long listings. Prints `s`, then if we've drawn
 * roughly a screen since the last pause, shows `-- MORE -- (Enter/Space
 * = next, Q = stop)` and waits for a key. Returns 0 to keep going,
 * non-zero if the user cancelled. */
static int pager_lines = 0;
#define PAGER_RESET()  (pager_lines = 0)
static int pager_print(const char *s) {
    print(s);
    /* Count newlines we just emitted */
    while (*s) {
        if (*s == '\n') pager_lines++;
        s++;
    }
    if (pager_lines >= VT_ROWS - 3) {
        struct key_event ev;
        vt_set_color(shell_vt, 0, 7);   /* inverted bar */
        print(" -- MORE -- ");
        vt_set_color(shell_vt, 7, 0);
        for (;;) {
            while (!vt_poll_key(shell_vt, &ev))
                sched_block(BLOCK_KEYBOARD, shell_vt);
            if (!ev.pressed) continue;
            if (ev.ascii == 'q' || ev.ascii == 'Q') {
                print("\n");
                pager_lines = 0;
                return 1;
            }
            if (ev.ascii == '\n' || ev.ascii == '\r' || ev.ascii == ' ')
                break;
        }
        /* Backspace over the "-- MORE -- " bar, then redraw blanks. */
        print("\r            \r");
        pager_lines = 0;
    }
    return 0;
}

static void cmd_help(const char *args) {
    (void)args;
    PAGER_RESET();
    vt_set_color(shell_vt, 14, 0);
    if (pager_print("Pinecore Commando v0.2.0.a - built-in commands\n")) return;
    vt_set_color(shell_vt, 7, 0);
    if (pager_print("\n  File system:\n")) return;
    if (pager_print("    ls / dir [path]    List directory\n")) return;
    if (pager_print("    cat / type <file>  Print file contents\n")) return;
    if (pager_print("    cd <dir>           Change directory\n")) return;
    if (pager_print("    pwd                Print working directory\n")) return;
    if (pager_print("    cp / copy <s> <d>  Copy file\n")) return;
    if (pager_print("    mv / ren <s> <d>   Move or rename file\n")) return;
    if (pager_print("    rm / del <file>    Delete file\n")) return;
    if (pager_print("    mkdir / md <dir>   Create directory\n")) return;
    if (pager_print("    rmdir / rd <dir>   Remove directory\n")) return;
    if (pager_print("    touch <file>       Create empty file\n")) return;
    if (pager_print("\n  System:\n")) return;
    if (pager_print("    ver / uname        Kernel version\n")) return;
    if (pager_print("    mem                Memory info\n")) return;
    if (pager_print("    ps                 List running tasks\n")) return;
    if (pager_print("    top                Live system monitor\n")) return;
    if (pager_print("    date / time        Show date / time\n")) return;
    if (pager_print("    clear / cls        Clear screen\n")) return;
    if (pager_print("    echo <text>        Print text\n")) return;
    if (pager_print("\n  Localization:\n")) return;
    if (pager_print("    layout             Show / list / set keyboard layout (us, de, ...)\n")) return;
    if (pager_print("\n  Hardware:\n")) return;
    if (pager_print("    hwinfo / lspci     Hardware inventory (PCI, ATA, USB, modules)\n")) return;
    if (pager_print("\n  Storage:\n")) return;
    if (pager_print("    mount / drives     List mounted volumes + detected ATA/ATAPI drives\n")) return;
    if (pager_print("\n  Network:\n")) return;
    if (pager_print("    wifi               Probe + report Intel 2200/2915 WiFi state (iwi.kmd)\n")) return;
    if (pager_print("\n  Sessions:\n")) return;
    if (pager_print("    dos                Open COMMAND.COM on a new VT\n")) return;
    if (pager_print("    shell              Open new Pinecore Commando on a new VT\n")) return;
    if (pager_print("    vt                 List virtual terminals\n")) return;
    if (pager_print("    exit               Close this terminal\n")) return;
    if (pager_print("    quit / reboot      Exit to FreeDOS / restart\n")) return;
    if (pager_print("\n  Hotkeys:\n")) return;
    if (pager_print("    Ctrl+1..6 switch VT  Ctrl+C new DOS  Ctrl+N new Commando  Ctrl+X close\n")) return;
    if (pager_print("    Tab = filename completion\n")) return;
}

static void cmd_ver(const char *args) {
    (void)args;
    vt_set_color(shell_vt, 10, 0);  /* light green */
    print("Pinecore Kernel v0.2.0.a (");
    print(__DATE__);
    print(")\n");
    vt_set_color(shell_vt, 7, 0);   /* light grey */
    print("i386 preemptive multitasking microkernel, DPMI 0.9 host\n");
    print("Built with i686-elf-gcc, NASM\n");
}

static void cmd_ps(const char *args) {
    (void)args;
    int i;
    static const char *st[] = { "unused", "ready", "RUN", "block", "dead" };
    static const char *ty[] = { "kern", "v86" };
    static const char *br[] = { "-", "kbd", "sleep", "vt-req", "fdc" };

    vt_set_color(shell_vt, 14, 0);
    print(" ID  TYPE  STATE  VT  WAIT     NAME\n");
    vt_set_color(shell_vt, 7, 0);
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        struct task *t = sched_get_task(i);
        if (!t || t->state == TASK_UNUSED) continue;
        print(" ");
        print_num(i);
        print("   ");
        print((unsigned)t->type < 2 ? ty[t->type] : "?");
        print("  ");
        print((unsigned)t->state < 5 ? st[t->state] : "?");
        print("   ");
        if (t->vt < 0) print("-"); else print_num(t->vt);
        print("   ");
        print((unsigned)t->block_reason < 5 ? br[t->block_reason] : "?");
        {
            int k = 0;
            const char *r = (unsigned)t->block_reason < 5 ? br[t->block_reason] : "?";
            while (r[k]) k++;
            while (k < 8) { vt_putc(shell_vt, ' '); k++; }
        }
        print(t->name);
        print("\n");
    }
}

static void cmd_clear(const char *args) {
    (void)args;
    vt_clear(shell_vt);
}

static void cmd_ls(const char *args) {
    struct fat_find ff;
    const char *path = skip_spaces(args);
    int count = 0;
    uint32_t total_size = 0;

    /* Use *.* if no path given */
    if (!*path) path = "*.*";

    if (fat_find_first(path, &ff) == 0) {
        do {
            /* Color: directories = blue, else white */
            if (ff.attr & FAT_ATTR_DIRECTORY)
                vt_set_color(shell_vt, 9, 0);  /* light blue */
            else
                vt_set_color(shell_vt, 15, 0);  /* white */

            print(ff.name);

            if (ff.attr & FAT_ATTR_DIRECTORY) {
                print("/");
            }

            /* Pad name to 16 chars */
            {
                int len = 0;
                const char *p = ff.name;
                while (*p++) len++;
                if (ff.attr & FAT_ATTR_DIRECTORY) len++;
                while (len < 16) { vt_putc(shell_vt, ' '); len++; }
            }

            if (!(ff.attr & FAT_ATTR_DIRECTORY)) {
                vt_set_color(shell_vt, 7, 0);
                print_num(ff.size);
                total_size += ff.size;
            }

            newline();
            count++;
        } while (fat_find_next(&ff) == 0);

        vt_set_color(shell_vt, 7, 0);
        print_num(count);
        print(" file(s), ");
        print_num(total_size);
        print(" bytes\n");
    } else {
        print("No files found.\n");
    }

    vt_set_color(shell_vt, 7, 0);
}

static void cmd_cat(const char *args) {
    const char *filename = skip_spaces(args);
    int fd;
    char buf[512];
    int n;

    if (!*filename) { print("Usage: cat <file>\n"); return; }

    fd = fat_open(filename, 0);
    if (fd < 0) {
        print("cat: ");
        print(filename);
        print(": not found\n");
        return;
    }

    while ((n = fat_read(fd, buf, sizeof(buf) - 1)) > 0) {
        int i;
        for (i = 0; i < n; i++) {
            if (buf[i] == '\r') continue;
            vt_putc(shell_vt, buf[i]);
        }
    }
    newline();
    fat_close(fd);
}

static void cmd_cd(const char *args) {
    const char *dir = skip_spaces(args);
    if (!*dir) {
        /* Print current directory */
        char cwd_buf[260];
        fat_getcwd(cwd_buf, sizeof(cwd_buf));
        print(cwd_buf);
        newline();
        return;
    }

    /* Check for drive switch (e.g., "C:" or "A:" or "D:\path"). Accepts
     * any drive letter from A up to FAT_MAX_DRIVES — future ISO9660 +
     * USB mounts will land here too. */
    if (dir[0] && dir[1] == ':' && (dir[2] == '\0' || dir[2] == '/' || dir[2] == '\\')) {
        char letter = dir[0];
        if (letter >= 'a' && letter <= 'z') letter -= 32;
        int d = -1;
        if (letter >= 'A' && letter < 'A' + FAT_MAX_DRIVES)
            d = letter - 'A';
        if (d >= 0 && fat_is_mounted(d)) {
            fat_set_drive(d);
            if (dir[2]) fat_chdir(dir + 2);
            return;
        }
    }

    if (fat_chdir(dir) != 0) {
        print("cd: ");
        print(dir);
        print(": not found\n");
    }
}

static void cmd_pwd(const char *args) {
    char cwd_buf[260];
    (void)args;
    fat_getcwd(cwd_buf, sizeof(cwd_buf));
    print(cwd_buf);
    newline();
}

static void cmd_echo(const char *args) {
    print(skip_spaces(args));
    newline();
}

static void cmd_mem(const char *args) {
    (void)args;
    print("Physical memory:\n");
    print("  Free pages:  ");
    print_hex(pmm_get_free_count());
    newline();
    print("  Page size:   4096 bytes\n");
}

/* ================================================================
 * TOP — live system monitor (like htop)
 * ================================================================ */

static void draw_bar(int width, int filled, uint8_t fill_color, uint8_t empty_color) {
    int i;
    vt_putc(shell_vt, '[');
    for (i = 0; i < width; i++) {
        if (i < filled) {
            vt_set_color(shell_vt, fill_color, 0);
            vt_putc(shell_vt, '|');
        } else {
            vt_set_color(shell_vt, empty_color, 0);
            vt_putc(shell_vt, ' ');
        }
    }
    vt_set_color(shell_vt, 7, 0);
    vt_putc(shell_vt, ']');
}

static void draw_pie(int cx, int cy, int pct) {
    /* Text-mode "pie chart" using block characters */
    /* 3x3 block with fill based on percentage */
    const char *full  = "\xDB";  /* █ full block */
    const char *half  = "\xB1";  /* ▒ medium shade */
    const char *light = "\xB0";  /* ░ light shade */
    const char *empty = " ";
    int cells = (pct * 9) / 100;  /* 9 cells in 3x3 */
    int r, c, idx;
    (void)full; (void)half; (void)light; (void)empty;

    for (r = 0; r < 3; r++) {
        vt_set_cursor(shell_vt, cx, cy + r);
        for (c = 0; c < 3; c++) {
            idx = r * 3 + c;
            if (idx < cells) {
                vt_set_color(shell_vt, 10, 0);  /* green */
                vt_putc(shell_vt, 0xDB);  /* full block */
            } else if (idx == cells) {
                vt_set_color(shell_vt, 2, 0);   /* dark green */
                vt_putc(shell_vt, 0xB1);  /* medium shade */
            } else {
                vt_set_color(shell_vt, 8, 0);   /* dark grey */
                vt_putc(shell_vt, 0xB0);  /* light shade */
            }
        }
    }
    vt_set_color(shell_vt, 7, 0);
}

static void cmd_top(const char *args) {
    struct key_event ev;
    int frame = 0;
    const char *spinner = "|/-\\";
    extern uint64_t rtc_get_ticks(void);
    extern uint32_t pit_get_ticks(void);
    (void)args;

    while (1) {
        uint64_t rtc = rtc_get_ticks();
        uint32_t pit = pit_get_ticks();
        uint32_t uptime_sec = pit / 100;
        uint32_t free_pages = pmm_get_free_count();
        uint32_t total_pages = 8192;  /* 32MB / 4KB */
        uint32_t used_pages = total_pages - free_pages;
        int mem_pct = (used_pages * 100) / total_pages;
        int i;

        vt_clear(shell_vt);

        /* Header */
        vt_set_color(shell_vt, 14, 0);  /* yellow */
        print("  Pinecore System Monitor");
        vt_set_color(shell_vt, 8, 0);
        print("  [");
        vt_putc(shell_vt, spinner[frame % 4]);
        print("]  Press Q to quit\n\n");

        /* Uptime */
        vt_set_color(shell_vt, 15, 0);
        print("  Uptime: ");
        vt_set_color(shell_vt, 11, 0);  /* cyan */
        print_num(uptime_sec / 60);
        print("m ");
        print_num(uptime_sec % 60);
        print("s");
        vt_set_color(shell_vt, 8, 0);
        print("  (RTC ticks: ");
        print_num((uint32_t)(rtc & 0xFFFFFFFF));
        print(")\n\n");

        /* CPU bar — fake usage based on scheduler activity */
        vt_set_color(shell_vt, 15, 0);
        print("  CPU  ");
        {
            /* Simulate CPU usage — show how much time is in user tasks vs idle */
            int cpu_pct = 15 + (frame % 20);  /* oscillating demo */
            draw_bar(40, (cpu_pct * 40) / 100, 10, 8);
            print(" ");
            print_num(cpu_pct);
            print("%");
        }
        print("\n");

        /* Memory bar */
        print("  Mem  ");
        draw_bar(40, (mem_pct * 40) / 100, 12, 8);
        print(" ");
        print_num(used_pages * 4);
        print("K / ");
        print_num(total_pages * 4);
        print("K\n\n");

        /* Pie charts side by side */
        vt_set_color(shell_vt, 15, 0);
        print("  CPU          Memory\n");
        {
            int cpu_pct = 15 + (frame % 20);
            draw_pie(2, 10, cpu_pct);
            /* Label under CPU pie */
            vt_set_cursor(shell_vt, 2, 13);
            vt_set_color(shell_vt, 10, 0);
            print_num(cpu_pct);
            print("%");

            draw_pie(15, 10, mem_pct);
            vt_set_cursor(shell_vt, 15, 13);
            vt_set_color(shell_vt, 12, 0);
            print_num(mem_pct);
            print("%");
        }

        /* Task list */
        vt_set_cursor(shell_vt, 0, 15);
        vt_set_color(shell_vt, 14, 0);
        print("  PID  NAME            STATE      VT\n");
        vt_set_color(shell_vt, 8, 0);
        print("  ---  ----            -----      --\n");

        for (i = 0; i < 16; i++) {
            struct task *t = sched_get_task(i);
            if (!t) continue;

            vt_set_color(shell_vt, 15, 0);
            print("  ");
            print_num(i);
            print("    ");

            vt_set_color(shell_vt, 11, 0);
            {
                const char *n = t->name;
                int len = 0;
                while (*n) { vt_putc(shell_vt, *n++); len++; }
                while (len < 16) { vt_putc(shell_vt, ' '); len++; }
            }

            switch (t->state) {
            case TASK_RUNNING:
                vt_set_color(shell_vt, 10, 0);
                print("RUNNING    ");
                break;
            case TASK_READY:
                vt_set_color(shell_vt, 14, 0);
                print("READY      ");
                break;
            case TASK_BLOCKED:
                vt_set_color(shell_vt, 12, 0);
                print("BLOCKED    ");
                break;
            default:
                vt_set_color(shell_vt, 8, 0);
                print("DEAD       ");
                break;
            }

            vt_set_color(shell_vt, 7, 0);
            if (t->vt >= 0) {
                print("VT");
                print_num(t->vt + 1);
            } else {
                print("--");
            }
            newline();
        }

        /* IRQ info */
        vt_set_cursor(shell_vt, 0, 23);
        vt_set_color(shell_vt, 8, 0);
        print("  Scheduler: IRQ8/RTC 8192Hz | PIT: 100Hz (free) | FDC: IRQ6 | KB: IRQ1");

        frame++;

        /* Wait ~250ms then redraw (check for Q to quit) */
        {
            int wait;
            for (wait = 0; wait < 25; wait++) {
                if (vt_poll_key(shell_vt, &ev)) {
                    if (ev.pressed && (ev.ascii == 'q' || ev.ascii == 'Q')) {
                        vt_set_color(shell_vt, 7, 0);
                        vt_clear(shell_vt);
                        return;
                    }
                }
                /* Sleep ~10ms */
                {
                    volatile int d;
                    for (d = 0; d < 500000; d++) ;
                }
            }
        }
    }
}

static void cmd_dos(const char *args) {
    /* dos                  → COMMAND.COM (FreeCom default)
     * dos <name>           → <NAME>.COM in a new DOS VT
     *
     * Supports the shell-switch demo: `dos drdos`, `dos fdos`, `dos msdos`,
     * `dos opendos`. The .COM extension is appended automatically when the
     * caller didn't include one. */
    const char *p = skip_spaces(args);
    if (!*p) {
        print("Opening COMMAND.COM...\n");
        vt_create_dos();
        return;
    }
    char bin[64];
    int i = 0;
    int has_dot = 0;
    while (*p && *p != ' ' && i < (int)sizeof(bin) - 5) {
        if (*p == '.') has_dot = 1;
        bin[i++] = *p++;
    }
    if (!has_dot) {
        bin[i++] = '.'; bin[i++] = 'C'; bin[i++] = 'O'; bin[i++] = 'M';
    }
    bin[i] = '\0';
    print("Opening ");
    print(bin);
    print("...\n");
    if (vt_create_dos_exec(bin, skip_spaces(p)) < 0) {
        print("dos: ");
        print(bin);
        print(": failed to launch\n");
    }
}

static void cmd_setup(const char *args) {
    /* Phase 4.6.5 M4 — re-run the first-boot setup wizard on demand.
     * Auto-runs at boot when config_is_firstboot() is true. */
    (void)args;
    extern void setup_run(int vt_num);
    setup_run(shell_vt);
}

static void cmd_shell(const char *args) {
    (void)args;
    print("Opening new Pinecore Commando...\n");
    vt_create_shell();
}

static void cmd_rm(const char *args) {
    const char *filename = skip_spaces(args);
    if (!*filename) { print("Usage: rm <file>\n"); return; }
    if (fat_delete(filename) < 0) {
        print("rm: ");
        print(filename);
        print(": failed\n");
    }
}

static void cmd_mkdir(const char *args) {
    const char *dir = skip_spaces(args);
    if (!*dir) { print("Usage: mkdir <dir>\n"); return; }
    if (fat_mkdir(dir) < 0) {
        print("mkdir: ");
        print(dir);
        print(": failed\n");
    }
}

static void cmd_rmdir(const char *args) {
    const char *dir = skip_spaces(args);
    if (!*dir) { print("Usage: rmdir <dir>\n"); return; }
    if (fat_rmdir(dir) < 0) {
        print("rmdir: ");
        print(dir);
        print(": failed (not empty?)\n");
    }
}

static void cmd_touch(const char *args) {
    const char *filename = skip_spaces(args);
    int fd;
    if (!*filename) { print("Usage: touch <file>\n"); return; }
    fd = fat_open(filename, 1);  /* write/create mode */
    if (fd >= 0)
        fat_close(fd);
    else {
        print("touch: ");
        print(filename);
        print(": failed\n");
    }
}

static void cmd_cp(const char *args) {
    const char *p = skip_spaces(args);
    char src[128], dst[128];
    int si = 0, di = 0;
    int sfd, dfd;
    char buf[512];
    int n;

    /* Parse source */
    while (*p && *p != ' ' && si < 127) src[si++] = *p++;
    src[si] = '\0';
    p = skip_spaces(p);
    /* Parse dest */
    while (*p && *p != ' ' && di < 127) dst[di++] = *p++;
    dst[di] = '\0';

    if (!src[0] || !dst[0]) { print("Usage: cp <src> <dst>\n"); return; }

    sfd = fat_open(src, 0);
    if (sfd < 0) { print("cp: can't open "); print(src); newline(); return; }
    dfd = fat_open(dst, 1);
    if (dfd < 0) { fat_close(sfd); print("cp: can't create "); print(dst); newline(); return; }

    while ((n = fat_read(sfd, buf, sizeof(buf))) > 0)
        fat_write(dfd, buf, n);

    fat_close(sfd);
    fat_close(dfd);
}

static void cmd_mv(const char *args) {
    const char *p = skip_spaces(args);
    char src[128], dst[128];
    int si = 0, di = 0;

    while (*p && *p != ' ' && si < 127) src[si++] = *p++;
    src[si] = '\0';
    p = skip_spaces(p);
    while (*p && *p != ' ' && di < 127) dst[di++] = *p++;
    dst[di] = '\0';

    if (!src[0] || !dst[0]) { print("Usage: mv <src> <dst>\n"); return; }

    /* Try rename first (same directory) */
    if (fat_rename(src, dst) == 0) return;

    /* Fallback: copy then delete */
    {
        int sfd = fat_open(src, 0);
        int dfd;
        char buf[512];
        int n;
        if (sfd < 0) { print("mv: can't open "); print(src); newline(); return; }
        dfd = fat_open(dst, 1);
        if (dfd < 0) { fat_close(sfd); print("mv: can't create "); print(dst); newline(); return; }
        while ((n = fat_read(sfd, buf, sizeof(buf))) > 0)
            fat_write(dfd, buf, n);
        fat_close(sfd);
        fat_close(dfd);
        fat_delete(src);
    }
}

static void cmd_date(const char *args) {
    uint16_t year;
    uint8_t month, day;
    (void)args;
    rtc_read_date(&year, &month, &day);
    print_num(year);
    vt_putc(shell_vt, '-');
    if (month < 10) vt_putc(shell_vt, '0');
    print_num(month);
    vt_putc(shell_vt, '-');
    if (day < 10) vt_putc(shell_vt, '0');
    print_num(day);
    newline();
}

static void cmd_time(const char *args) {
    uint8_t hour, min, sec;
    (void)args;
    rtc_read_time(&hour, &min, &sec);
    if (hour < 10) vt_putc(shell_vt, '0');
    print_num(hour);
    vt_putc(shell_vt, ':');
    if (min < 10) vt_putc(shell_vt, '0');
    print_num(min);
    vt_putc(shell_vt, ':');
    if (sec < 10) vt_putc(shell_vt, '0');
    print_num(sec);
    newline();
}

static void cmd_quit(const char *args) {
    (void)args;
    print("Exiting Pinecore...\n");
    {
        extern void pinecore_exit(void);
        pinecore_exit();
    }
    /* If pinecore_exit returns (Multiboot, no DOS to return to) */
    print("No return-to-DOS path (Multiboot boot).\n");
    print("Use 'reboot' instead.\n");
}

static void cmd_reboot(const char *args) {
    (void)args;
    print("Rebooting...\n");
    /* Triple fault: load null IDT and trigger interrupt */
    {
        struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
        __asm__ volatile("lidt %0; int $3" : : "m"(null_idt));
    }
    /* Fallback: keyboard controller reset */
    outb(0x64, 0xFE);
    while (1) __asm__ volatile("hlt");
}

/* `mount` builtin: list mounted FAT volumes + detected drives.
 *
 * Two sections:
 *   1. Mounted volumes (drive letter, FS type, source, capacity, free)
 *   2. Detected drives (ATA channel/slave or FDC) — including ATAPI
 *      drives that aren't yet mountable (ISO9660 pending).
 *
 * Capacity is computed from total_clusters * sec_per_clus * 512 (KB),
 * free likewise. We assume 512-byte sectors (only sector size our FAT
 * driver handles). Switches `active_drive` to each mounted slot via
 * fat_set_drive() to query its info, then restores. */
static void cmd_mount(const char *args) {
    int saved;
    int d;
    int any_mounted = 0;
    int n_ata;

    (void)args;

    saved = fat_get_drive();

    print("Mounted volumes:\n");
    for (d = 0; d < FAT_MAX_DRIVES; d++) {
        int      is_floppy = 0;
        uint8_t  ata_id    = 0;
        uint32_t part_lba  = 0;
        uint32_t spc, total_clus, free_clus;
        uint32_t total_kb, free_kb;
        int      type;

        if (!fat_is_mounted(d)) continue;
        any_mounted = 1;

        fat_set_drive(d);
        fat_get_source(d, &is_floppy, &ata_id, &part_lba);
        type       = fat_get_type();
        spc        = fat_get_sec_per_clus();
        total_clus = fat_get_total_clusters();
        free_clus  = fat_count_free_clusters();
        total_kb   = (total_clus * spc) / 2;   /* clusters * (spc * 512 / 1024) */
        free_kb    = (free_clus  * spc) / 2;

        print("  ");
        vt_putc(shell_vt, (char)('A' + d));
        print(":  FAT");
        print_num((uint32_t)type);
        print("  ");
        if (is_floppy) {
            print("floppy           ");
        } else {
            print("ata");
            print_num((uint32_t)ata_id);
            print(" lba=");
            print_hex(part_lba);
            print("  ");
        }
        print(" ");
        print_num(total_kb);
        print(" KB total, ");
        print_num(free_kb);
        print(" KB free");
        if (d == saved) print("   *");
        newline();
    }
    if (!any_mounted)
        print("  (none)\n");

    print("\nDetected drives:\n");
    n_ata = ata_get_drive_count();
    if (n_ata == 0) {
        print("  (no ATA)\n");
    } else {
        int slot;
        for (slot = 0; slot < 4; slot++) {
            const struct ata_drive *ad = ata_get_drive((uint8_t)slot);
            if (!ad || !ad->present) continue;
            print("  ata");
            print_num((uint32_t)slot);
            print("  ");
            print(ad->atapi ? "ATAPI" : "ATA  ");
            print(" ");
            print(ad->model);
            print(" (");
            print_num(ad->sectors);
            print(" x ");
            print_num(ad->sector_size);
            print(" B)");
            if (ad->atapi) print("  [ISO9660 not yet]");
            newline();
        }
    }

    /* Restore active drive */
    fat_set_drive(saved);
    print("\nCurrent: ");
    vt_putc(shell_vt, (char)('A' + saved));
    print(":\n");
}

/* `hwinfo` builtin: re-run the boot-time hardware inventory dump on
 * demand. Same output as the auto-dump after autoload (main.c) — just
 * routed through the shell's `print` so it lands on the current VT
 * instead of via vga_puts. */
static void cmd_hwinfo_emit(const char *s) {
    print(s);
}

static void cmd_hwinfo(const char *args) {
    (void)args;
    hwinfo_dump(cmd_hwinfo_emit);
}

/* Phase 11 (WiFi) — `wifi` builtin: probe + report state of iwi.kmd.
 *
 * Calls iwi_test() in the loaded iwi.kmd module (EXPORT_SYMBOL_GPL).
 * The module decides what to print; we just give it our vt-aware print
 * so output lands in the active terminal. If iwi.kmd is not loaded
 * (PCORE.CFG suppressed it, or it never got staged), say so.
 *
 * Future subcommands once iwi.kmd matures:
 *   wifi              → state (current)
 *   wifi scan         → scan results
 *   wifi connect SSID → connect (open / WPA2-PSK)
 *   wifi disconnect   → drop association
 *   wifi status       → link quality / RSSI / current rate
 */
static void cmd_wifi(const char *args) {
    (void)args;
    typedef void (*iwi_test_fn)(void (*)(const char *));
    iwi_test_fn test = (iwi_test_fn)module_resolve("iwi_test", 1);
    if (!test) {
        print("wifi: iwi.kmd not loaded\n");
        print("  - is IWI.KMD in \\DRIVERS\\ on this disk?\n");
        print("  - check serial log for autoload messages\n");
        return;
    }
    test(print);
}

/* Phase 4.6.5 M2 — `layout` builtin: list / set / get keyboard layouts.
 *   layout            → show active layout
 *   layout list       → list all available layouts
 *   layout <id>       → switch active layout to <id> (e.g. "us", "de")
 * Persistence (PCORE.CFG) lands in M3. */
static void cmd_layout(const char *args) {
    const char *p = args;
    while (*p == ' ') p++;

    if (!*p || !strstart(p, "current")) {
        if (!*p) {
            print("Active layout: ");
            print(keyboard_get_layout_id());
            print("  (");
            print(keyboard_get_layout_name());
            print(")\n");
            return;
        }
    }

    if (strstart(p, "list")) {
        int n = keyboard_layout_count();
        int i;
        print("Available layouts:\n");
        for (i = 0; i < n; i++) {
            const struct keyboard_layout *L = keyboard_layout_at(i);
            if (!L) continue;
            print("  ");
            print(L->id);
            print("  ");
            print(L->name);
            if (L == (const void *)0) continue;  /* defensive */
            if (L->id[0] == keyboard_get_layout_id()[0] &&
                L->id[1] == keyboard_get_layout_id()[1])
                print("   [active]");
            newline();
        }
        return;
    }

    /* Otherwise treat first 2 chars as layout id to set */
    {
        char id[3];
        id[0] = p[0];
        id[1] = (p[0] && p[1]) ? p[1] : 0;
        id[2] = 0;
        if (keyboard_set_layout(id) == 0) {
            extern int config_save(void);    /* config.h — kept forward-decl'd
                                                here to avoid a header churn */
            print("Layout set to ");
            print(keyboard_get_layout_id());
            print("  (");
            print(keyboard_get_layout_name());
            print(")\n");
            if (config_save() == 0)
                print("Saved to C:\\PCORE.CFG (persists across reboot).\n");
            else
                print("(Warning: could not save PCORE.CFG.)\n");
        } else {
            print("Unknown layout: ");
            print(id);
            print("\nTry: layout list\n");
        }
    }
}

static void cmd_vt(const char *args) {
    int i;
    (void)args;
    print("Virtual Terminals:\n");
    for (i = 0; i < VT_MAX; i++) {
        struct vt *v = vt_get(i);
        if (!v) continue;
        print("  VT");
        vt_putc(shell_vt, '1' + i);
        if (i == vt_get_active())
            print(" *active* ");
        else
            print("          ");
        if (v->type == VT_SHELL) print("[Pinecore Commando]");
        else if (v->type == VT_DOS) print("[COMMAND.COM]");
        newline();
    }
    print("\nCtrl+1-6 switch, Ctrl+C DOS, Ctrl+N Shell, Ctrl+X close\n");
}

/* ================================================================
 * Command dispatcher
 * ================================================================ */

static void execute(const char *cmdline) {
    const char *cmd = skip_spaces(cmdline);
    const char *args;
    int i;

    if (!*cmd) return;  /* empty line */

    /* Standalone drive-letter switch (DOS convention: `D:` alone changes
     * the current drive, just like FreeCom). Detect "X:" or "X:" followed
     * only by whitespace; cmd_cd still handles `cd X:\path` for combined
     * switch+chdir. */
    if (cmd[0] && cmd[1] == ':' &&
        (cmd[2] == '\0' || cmd[2] == ' ' || cmd[2] == '\t' ||
         cmd[2] == '\r' || cmd[2] == '\n')) {
        char letter = cmd[0];
        if (letter >= 'a' && letter <= 'z') letter -= 32;
        if (letter >= 'A' && letter < 'A' + FAT_MAX_DRIVES) {
            int d = letter - 'A';
            if (fat_is_mounted(d)) {
                fat_set_drive(d);
                return;
            }
            print("Invalid drive specification\n");
            return;
        }
        print("Invalid drive specification\n");
        return;
    }

    /* Find end of command word */
    args = cmd;
    while (*args && *args != ' ') args++;
    if (*args == ' ') args++;

    /* Dispatch */
    if (strstart(cmd, "help"))  { cmd_help(args); return; }
    if (strstart(cmd, "ver"))   { cmd_ver(args); return; }
    if (strstart(cmd, "uname")) { cmd_ver(args); return; }
    if (strstart(cmd, "ps"))    { cmd_ps(args); return; }
    if (strstart(cmd, "clear")) { cmd_clear(args); return; }
    if (strstart(cmd, "cls"))   { cmd_clear(args); return; }
    if (strstart(cmd, "ls"))    { cmd_ls(args); return; }
    if (strstart(cmd, "dir"))   { cmd_ls(args); return; }
    if (strstart(cmd, "cat"))   { cmd_cat(args); return; }
    if (strstart(cmd, "type"))  { cmd_cat(args); return; }
    if (strstart(cmd, "cd"))    { cmd_cd(args); return; }
    if (strstart(cmd, "pwd"))   { cmd_pwd(args); return; }
    if (strstart(cmd, "echo"))   { cmd_echo(args); return; }
    if (strstart(cmd, "mem"))    { cmd_mem(args); return; }
    if (strstart(cmd, "rm"))     { cmd_rm(args); return; }
    if (strstart(cmd, "del"))    { cmd_rm(args); return; }
    if (strstart(cmd, "mkdir"))  { cmd_mkdir(args); return; }
    if (strstart(cmd, "md"))     { cmd_mkdir(args); return; }
    if (strstart(cmd, "rmdir"))  { cmd_rmdir(args); return; }
    if (strstart(cmd, "rd"))     { cmd_rmdir(args); return; }
    if (strstart(cmd, "touch"))  { cmd_touch(args); return; }
    if (strstart(cmd, "cp"))     { cmd_cp(args); return; }
    if (strstart(cmd, "copy"))   { cmd_cp(args); return; }
    if (strstart(cmd, "mv"))     { cmd_mv(args); return; }
    if (strstart(cmd, "ren"))    { cmd_mv(args); return; }
    if (strstart(cmd, "date"))   { cmd_date(args); return; }
    if (strstart(cmd, "time"))   { cmd_time(args); return; }
    if (strstart(cmd, "quit"))   { cmd_quit(args); return; }
    if (strstart(cmd, "reboot")) { cmd_reboot(args); return; }
    if (strstart(cmd, "dos"))    { cmd_dos(args); return; }
    if (strstart(cmd, "shell"))  { cmd_shell(args); return; }
    if (strstart(cmd, "vt"))     { cmd_vt(args); return; }
    if (strstart(cmd, "top"))    { cmd_top(args); return; }
    if (strstart(cmd, "layout")) { cmd_layout(args); return; }  /* Phase 4.6.5 M2 */
    if (strstart(cmd, "setup"))  { cmd_setup(args); return; }   /* Phase 4.6.5 M4 */
    if (strstart(cmd, "wifi"))   { cmd_wifi(args); return; }    /* Phase 11 WiFi (iwi.kmd) */
    if (strstart(cmd, "mount"))  { cmd_mount(args); return; }
    if (strstart(cmd, "drives")) { cmd_mount(args); return; }
    if (strstart(cmd, "hwinfo")) { cmd_hwinfo(args); return; }
    if (strstart(cmd, "lspci"))  { cmd_hwinfo(args); return; }
    if (strstart(cmd, "exit"))  {
        print("Closing terminal...\n");
        if (vt_count_active() <= 1) {
            print("Can't close last terminal.\n");
            return;
        }
        {
            int my_vt = shell_vt;
            vt_destroy(my_vt);
            sched_exit();  /* never returns */
        }
        return;
    }

    /* Unknown built-in -- try to EXEC as a disk binary.
     * COMSPEC-style resolution: if `cmd` already has an extension we try
     * that exact name; otherwise we probe `<cmd>.COM`, `.EXE`, `.BAT`
     * in turn. On match, spawn a new DOS VT running the binary. */
    {
        char fn[64];
        int has_dot = 0, k;
        const char *exts[3] = { ".COM", ".EXE", ".BAT" };
        int ne;
        for (k = 0; cmd[k] && k < (int)sizeof(fn) - 5; k++) {
            fn[k] = cmd[k];
            if (cmd[k] == '.') has_dot = 1;
        }
        fn[k] = '\0';

        for (ne = 0; ne < (has_dot ? 1 : 3); ne++) {
            char path[80];
            int p = 0, q;
            for (q = 0; fn[q] && p < (int)sizeof(path) - 1; q++)
                path[p++] = fn[q];
            if (!has_dot) {
                for (q = 0; exts[ne][q] && p < (int)sizeof(path) - 1; q++)
                    path[p++] = exts[ne][q];
            }
            path[p] = '\0';

            int fh = fat_open(path, 0 /* read */);
            if (fh >= 0) {
                fat_close(fh);
                print("Launching ");
                print(path);
                print("...\n");
                if (vt_create_dos_exec(path, skip_spaces(args)) < 0) {
                    print("exec: ");
                    print(path);
                    print(": launch failed\n");
                }
                return;
            }
        }
    }

    /* Truly unknown */
    print(cmd);
    (void)i;
    print(": command not found\n");
}

/* ================================================================
 * Shell prompt
 * ================================================================ */

static void prompt(void) {
    char cwd_buf[260];
    const char *drive_letter = "?";

    /* Show drive letter */
    switch (fat_get_drive()) {
    case 0: drive_letter = "A"; break;
    case 1: drive_letter = "B"; break;
    case 2: drive_letter = "C"; break;
    }

    fat_getcwd(cwd_buf, sizeof(cwd_buf));

    vt_set_color(shell_vt, 10, 0);  /* green */
    print("pine:");
    print(drive_letter);
    vt_putc(shell_vt, ':');
    print(cwd_buf);
    print("$ ");
    vt_set_color(shell_vt, 15, 0);  /* white */
}

/* ================================================================
 * Shell entry point — called as a scheduler task
 * ================================================================ */

void shell_entry(void) {
    char cmdline[256];
    extern int config_is_firstboot(void);
    extern void setup_run(int vt_num);

    /* Debug: confirm shell task started */
    serial_puts("SHELL: entry reached on vt=");
    serial_puthex(shell_vt);
    serial_puts("\n");

    /* Phase 4.6.5 M4: first-boot setup. If PCORE.CFG is missing or has
     * `firstboot=yes`, take over the VT for a keyboard-layout picker
     * before showing the normal banner. */
    if (config_is_firstboot()) {
        setup_run(shell_vt);
    }

    /* Banner — DOS-style multi-line, FreeCOM-ish.
     *
     * Build stamp comes from kernel/build_info.c which is auto-regenerated
     * on every `make` invocation (see Makefile FORCE rule). Using
     * __DATE__/__TIME__ here would expand to whatever shell.c was last
     * compiled, which goes stale across incremental builds. */
    {
        extern const char         pinecore_build_date[];
        extern const char         pinecore_build_time[];
        extern const char         pinecore_build_git[];
        extern const unsigned long pinecore_build_seq;

        vt_set_color(shell_vt, 14, 0);  /* yellow */
        print("\nPinecore Commando v0.2.0.a - i386 native [");
        print(pinecore_build_date);
        print(" ");
        print(pinecore_build_time);
        print(" git:");
        print(pinecore_build_git);
        print(" seq:");
        {
            /* uint32 → decimal — kernel doesn't have snprintf here */
            unsigned long v = pinecore_build_seq;
            char buf[12];
            int i = 0;
            if (v == 0) buf[i++] = '0';
            else {
                char tmp[12]; int j = 0;
                while (v) { tmp[j++] = '0' + (v % 10); v /= 10; }
                while (j) buf[i++] = tmp[--j];
            }
            buf[i] = 0;
            print(buf);
        }
        print("]\n");
    }
    vt_set_color(shell_vt, 7, 0);
    print("(C) 2026 Pinecore Project. Native command shell. Type 'help' for commands.\n");
    vt_set_color(shell_vt, 8, 0);
    print("Type 'help' for commands. Ctrl+C drops to a DOS prompt.\n\n");
    vt_set_color(shell_vt, 7, 0);

    /* Banner */
    vt_set_color(shell_vt, 14, 0);  /* yellow */
    print("  Pinecore Commando v0.2.0.a\n");
    vt_set_color(shell_vt, 8, 0);   /* dark grey */
    print("  Type 'help' for commands, Alt+C for COMMAND.COM\n\n");
    vt_set_color(shell_vt, 7, 0);

    /* Main loop */
    while (1) {
        prompt();
        readline(cmdline, sizeof(cmdline));
        execute(cmdline);
    }
}
