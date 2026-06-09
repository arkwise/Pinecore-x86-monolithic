/* config.c — Pinecore persistent settings (Phase 4.6.5 M3).
 *
 * File format (C:\PCORE.CFG):
 *   # Comment lines start with '#' or ';'
 *   key = value
 *
 * Parser is whitespace-tolerant on both sides of '=' and at line ends.
 * Unknown keys are silently ignored — forward-compatible. Values are
 * trimmed and copied into a fixed-size buffer.
 *
 * Recognised keys (M3):
 *   layout = us | de | ...   (calls keyboard_set_layout)
 *
 * Writer is a complete rewrite — we don't attempt to preserve comments
 * or custom keys yet. Setup app (M4) will own the file fully; for now
 * the user can hand-edit safely between sessions.
 */

#include "types.h"
#include "config.h"
#include "fat.h"
#include "keyboard.h"
#include "serial.h"

/* From libc/string.c — no header for it in this tree. */
extern int strcmp(const char *a, const char *b);

/* Module state — set by config_init(), read by config_is_firstboot(). */
static int g_firstboot = 1;     /* default: assume first boot until proven otherwise */
static int g_kbd_v86   = 0;     /* s51 — V86 BIOS-INT-16h kbd polling: off by default */

/*  — USB stack policy keys (doc 54 §8). Defaults match doc. */
static int g_usb_enable             = 1;
static int g_usb_trace              = 0;
static int g_usb_kbd_initial_delay  = 500;
static int g_usb_kbd_repeat_rate    = 33;

/* Phase 4.8 — network-provider config. Empty/zero means not configured. */
static char     g_net_provider[32]  = {0};
static uint32_t g_net_dns_server    = 0;     /* big-endian IPv4 */

#define CONFIG_PATH    "C:\\PCORE.CFG"
#define CONFIG_BUFSZ   1024     /* file size cap — way more than M3 needs */

/* ---- Internal helpers ---- */

static int is_ws(char c)        { return c == ' ' || c == '\t' || c == '\r'; }
static int is_eol(char c)       { return c == '\n' || c == 0; }
static int is_comment(char c)   { return c == '#' || c == ';'; }

/* Permissive integer parser: decimal by default, hex on "0x"/"0X" prefix.
 * Returns 0 if the value isn't a recognised number; callers that need
 * to distinguish "0" from "missing" should check the source string. */
static int parse_int(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = (v << 4) | d;
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    }
    return neg ? -v : v;
}

static int parse_bool(const char *v) {
    return !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1") ||
           !strcmp(v, "on");
}

/* Parse "A.B.C.D" into a network-order uint32_t. Returns 0 on
 * malformed input (which the caller treats as "not set"). */
static uint32_t parse_ipv4(const char *s) {
    uint32_t out = 0;
    int i;
    for (i = 0; i < 4; i++) {
        int oct = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            oct = oct * 10 + (*s - '0');
            if (oct > 255) return 0;
            digits++;
            s++;
        }
        if (digits == 0) return 0;
        out |= (uint32_t)oct << (i * 8);   /* low byte = first octet → network order in memory */
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    while (*s == ' ' || *s == '\t') s++;
    if (*s) return 0;
    return out;
}

/* In-place trim: returns pointer to first non-ws char; null-terminates
 * past the last non-ws char. */
static char *trim(char *s) {
    char *end;
    while (*s && is_ws(*s)) s++;
    end = s;
    while (*end) end++;
    while (end > s && is_ws(end[-1])) end--;
    *end = 0;
    return s;
}

/* Apply one key=value pair to the live kernel state. Silently ignores
 * unknown keys. */
static void apply_kv(const char *key, const char *val) {
    if (!strcmp(key, "layout")) {
        if (keyboard_set_layout(val) == 0) {
            serial_puts("config: layout = ");
            serial_puts(val);
            serial_puts("\n");
        } else {
            serial_puts("config: unknown layout \"");
            serial_puts(val);
            serial_puts("\", ignoring\n");
        }
    } else if (!strcmp(key, "firstboot")) {
        /* yes/true/1 → firstboot=1; anything else → 0 */
        if (!strcmp(val, "yes") || !strcmp(val, "true") || !strcmp(val, "1"))
            g_firstboot = 1;
        else
            g_firstboot = 0;
        serial_puts("config: firstboot = ");
        serial_puts(g_firstboot ? "yes\n" : "no\n");
    } else if (!strcmp(key, "kbd_v86")) {
        /* s51 — opt-in for V86 BIOS-INT-16h keyboard polling task. */
        g_kbd_v86 = parse_bool(val);
        serial_puts("config: kbd_v86 = ");
        serial_puts(g_kbd_v86 ? "yes\n" : "no\n");
    } else if (!strcmp(key, "usb_enable")) {
        g_usb_enable = parse_bool(val);
        serial_puts("config: usb_enable = ");
        serial_puts(g_usb_enable ? "yes\n" : "no\n");
    } else if (!strcmp(key, "usb_trace")) {
        g_usb_trace = parse_bool(val);
        serial_puts("config: usb_trace = ");
        serial_puts(g_usb_trace ? "yes\n" : "no\n");
    } else if (!strcmp(key, "usb_kbd_initial_delay_ms")) {
        int v = parse_int(val);
        if (v > 0 && v < 5000) g_usb_kbd_initial_delay = v;
    } else if (!strcmp(key, "usb_kbd_repeat_rate_ms")) {
        int v = parse_int(val);
        if (v > 0 && v < 1000) g_usb_kbd_repeat_rate = v;
    } else if (!strcmp(key, "net_provider")) {
        int i = 0;
        while (val[i] && i < (int)sizeof(g_net_provider) - 1) {
            g_net_provider[i] = val[i];
            i++;
        }
        g_net_provider[i] = 0;
        serial_puts("config: net_provider = ");
        serial_puts(g_net_provider);
        serial_puts("\n");
    } else if (!strcmp(key, "net_dns_server")) {
        uint32_t v = parse_ipv4(val);
        if (v != 0) {
            g_net_dns_server = v;
            serial_puts("config: net_dns_server = ");
            serial_puts(val);
            serial_puts("\n");
        } else {
            serial_puts("config: net_dns_server invalid \"");
            serial_puts(val);
            serial_puts("\", ignoring\n");
        }
    }
    /* Future M6+ keys: country, codepage, timezone, ... */
}

/* ---- Public API ---- */

void config_init(void) {
    int fd;
    char buf[CONFIG_BUFSZ + 1];
    int n;
    char *p;

    fd = fat_open(CONFIG_PATH, 0);  /* read mode */
    if (fd < 0) {
        serial_puts("config: no PCORE.CFG — firstboot=yes\n");
        g_firstboot = 1;
        return;
    }
    /* File exists — default to firstboot=no unless the file itself
     * explicitly says yes. apply_kv() will flip it back if so. */
    g_firstboot = 0;

    n = fat_read(fd, buf, CONFIG_BUFSZ);
    fat_close(fd);

    if (n <= 0) {
        serial_puts("config: PCORE.CFG empty\n");
        return;
    }
    buf[n] = 0;

    serial_puts("config: parsing PCORE.CFG (");
    {
        char nstr[12];
        int i = 0, v = n;
        if (v == 0) nstr[i++] = '0';
        else {
            char tmp[12]; int j = 0;
            while (v) { tmp[j++] = '0' + (v % 10); v /= 10; }
            while (j) nstr[i++] = tmp[--j];
        }
        nstr[i] = 0;
        serial_puts(nstr);
    }
    serial_puts(" bytes)\n");

    /* Line-by-line parse */
    p = buf;
    while (*p) {
        char *line_end = p;
        char *eq;
        char *key, *val;

        /* Find end of line */
        while (*line_end && !is_eol(*line_end)) line_end++;
        if (*line_end) { *line_end = 0; line_end++; }

        /* Skip whitespace-only or comment lines */
        {
            char *q = p;
            while (*q && is_ws(*q)) q++;
            if (!*q || is_comment(*q)) { p = line_end; continue; }
        }

        /* Split on '=' */
        eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq != '=') { p = line_end; continue; }  /* malformed */

        *eq = 0;
        key = trim(p);
        val = trim(eq + 1);

        if (*key && *val) apply_kv(key, val);

        p = line_end;
    }
}

int config_save(void) {
    int fd;
    char buf[256];
    char *p = buf;
    const char *layout_id = keyboard_get_layout_id();

    /* Compose file content. M3: just the layout line + a header comment.
     * M6+ will add more keys; format is line-based so append is trivial. */
    {
        const char *header =
            "# Pinecore configuration - written by kernel. Hand-edit OK.\n"
            "# Recognised keys: layout, firstboot\n";
        const char *kw_layout = "layout = ";
        const char *kw_fb     = "firstboot = no\n";
        while (*header)  *p++ = *header++;
        while (*kw_layout) *p++ = *kw_layout++;
        while (*layout_id) *p++ = *layout_id++;
        *p++ = '\n';
        while (*kw_fb)    *p++ = *kw_fb++;
        *p = 0;
    }
    /* A successful save means the user has gone through setup (or
     * intentionally changed something) — clear the firstboot flag in
     * memory so subsequent main.c checks don't relaunch setup. */
    g_firstboot = 0;

    /* fat_open mode != 0 creates if missing. We also need to truncate
     * if the file already exists and was longer; fat_write currently
     * overwrites from position 0 but doesn't truncate. For M3 the
     * file content size is fixed (~80 bytes), so the only way a stale
     * tail matters is if the user shortens the layout. Acceptable for
     * now; M4 setup app will write the full canonical file. */
    fd = fat_open(CONFIG_PATH, 1);
    if (fd < 0) {
        serial_puts("config: save FAILED (fat_open)\n");
        return -1;
    }
    if (fat_write(fd, buf, (uint32_t)(p - buf)) < 0) {
        fat_close(fd);
        serial_puts("config: save FAILED (fat_write)\n");
        return -1;
    }
    fat_close(fd);
    serial_puts("config: saved\n");
    return 0;
}

int config_is_firstboot(void) {
    return g_firstboot;
}

int config_kbd_v86_enabled(void) {
    return g_kbd_v86;
}

int config_usb_enabled(void)              { return g_usb_enable; }
int config_usb_trace_enabled(void)        { return g_usb_trace; }
int config_usb_kbd_initial_delay_ms(void) { return g_usb_kbd_initial_delay; }
int config_usb_kbd_repeat_rate_ms(void)   { return g_usb_kbd_repeat_rate; }

const char *config_net_provider(void) {
    return g_net_provider[0] ? g_net_provider : NULL;
}

uint32_t config_net_dns_server(void) {
    return g_net_dns_server;
}
