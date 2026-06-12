# Virtual Terminals + Shell System

> Multiple text-mode terminals with hotkey switching. Kernel-mode shell alongside COMMAND.COM V86 sessions.

**Date:** 2026-04-30
**Status:** Architecture designed, implementation not started

---

## Concept

Like Linux VTs (Ctrl+Alt+F1-F6) but for Pinecore:
- Multiple independent terminals, each with its own 80x25 text buffer
- Each terminal runs either the Pinecore Commando (kernel mode) or COMMAND.COM (V86 mode)
- Alt+1..6 switches between terminals
- Active terminal renders to VGA (0xB8000), inactive terminals buffer silently
- Demonstrates the power of Ring 0: preemptive multitasking, own drivers, multiple environments

---

## Components

### 1. VT Manager (`src/kernel/vt.c`)

Central manager for all virtual terminals.

**Data structures:**
```c
#define VT_MAX 6
#define VT_COLS 80
#define VT_ROWS 25
#define VT_BUFSIZE (VT_COLS * VT_ROWS * 2)  /* char + attr pairs */

enum vt_type { VT_PINECORE, VT_COMMAND };

struct vt {
    uint8_t  active;          /* 1 = running */
    enum vt_type type;        /* kernel shell or V86 COMMAND.COM */
    uint8_t  text_buf[VT_BUFSIZE];  /* shadow text buffer */
    uint8_t  cursor_x, cursor_y;
    uint8_t  color;           /* current text attribute */

    /* For VT_PINECORE: shell state */
    char     cmdline[256];    /* current input line */
    uint8_t  cmdlen;          /* current input position */
    char     cwd[260];        /* working directory */
    char     history[16][256]; /* command history */
    int      hist_count, hist_pos;

    /* For VT_COMMAND: V86 task ID */
    int      v86_task_id;
    int      dos_task_id;
};
```

**Operations:**
- `vt_init()` — create default VTs (VT1=pinecore, VT2=command.com)
- `vt_switch(int n)` — swap active VT (copy shadow↔VGA)
- `vt_putc(int vt, char c)` — write character to VT's buffer
- `vt_getc(int vt)` — read from VT's keyboard queue
- `vt_render(int vt)` — copy shadow buffer to VGA (for active VT)

### 2. Pinecore Commando (`src/kernel/shell.c`)

Kernel-mode shell — runs directly, no V86 overhead.

**Command table:**
| Command | Action | Implementation |
|---------|--------|---------------|
| `ls [path]` | List directory | `fat_find_first/next()` |
| `cat <file>` | Print file | `fat_open/read/close()` |
| `cp <src> <dst>` | Copy file | `fat_open` + `fat_read` + `fat_write` |
| `mv <src> <dst>` | Move file | `fat_rename()` |
| `rm <file>` | Delete file | `fat_delete()` |
| `mkdir <dir>` | Create directory | `fat_mkdir()` |
| `rmdir <dir>` | Remove directory | `fat_rmdir()` |
| `cd <dir>` | Change directory | update shell cwd |
| `pwd` | Print working dir | print shell cwd |
| `echo <text>` | Print text | direct VT output |
| `clear` | Clear screen | zero VT buffer |
| `help` | List commands | print command table |
| `ver` | Version info | print Pinecore version |
| `date` | Show date | read CMOS RTC |
| `time` | Show time | read CMOS RTC |
| `mem` | Memory info | dump PMM stats |
| `vt` | List terminals | show VT status |
| `dos` | Open COMMAND.COM | create new VT_COMMAND |
| `exit` | Close terminal | destroy VT (or return to FreeDOS if last) |
| `reboot` | Reboot system | keyboard controller reset |

**Shell prompt:** `pine:/path$ ` (green on black, Unix-style)

**Features:**
- Command line editing (backspace, left/right arrows)
- Command history (up/down arrows, stored per-VT)
- Tab completion (filenames from FAT directory listing)
- Colored output (ls shows directories in blue, executables in green)
- Simple globbing (* and ? in filenames)

### 3. Keyboard Integration

The keyboard driver already has a ring buffer. For VTs:
- Keyboard IRQ handler checks for Alt+1..6 → calls `vt_switch()`
- Other keys go to the active VT's input queue
- Each VT has its own key buffer

### 4. Console I/O Routing

Currently `dos_set_console()` sets global callbacks for putchar/getchar/kbhit. With VTs:
- Each V86 COMMAND.COM task binds to a specific VT
- `dos_putchar` writes to that VT's text buffer (not directly to VGA)
- `dos_getchar` reads from that VT's keyboard queue
- The Pinecore Commando reads/writes its own VT directly

---

## Implementation Order

1. **VT data structure + manager** — create/destroy VTs, switch active, buffer management
2. **VT rendering** — copy shadow buffer to VGA on switch, write to shadow when inactive
3. **Keyboard routing** — Alt+N hotkeys, per-VT key queues
4. **Pinecore Commando core** — command line reader, parser, dispatcher
5. **Basic commands** — ls, cd, pwd, cat, echo, clear, help, ver
6. **File commands** — cp, mv, rm, mkdir (needs FAT write)
7. **COMMAND.COM VT** — bind V86 task to a VT, route console I/O
8. **Polish** — command history, tab completion, colored output

---

## Existing Infrastructure We Use

| Component | Already Have | Need For VTs |
|-----------|-------------|-------------|
| Keyboard driver | Ring buffer, scancode→ASCII | Alt+N detection, per-VT queues |
| VGA driver | putc, clear, cursor, color | Shadow buffer rendering |
| V86 task struct | text_buf[4000], cursor_x/y | Already there! Just need to use it |
| FAT driver | open, read, find, seek, mkdir | Already there for shell commands |
| DOS emulation | console callbacks | Route per-VT instead of global |

---

## Key Design Decisions

```
DECIDED: VTs use 80x25 text mode (no graphics for VT system)
DECIDED: Alt+1..6 for switching (not Ctrl+Alt to avoid conflicts)
DECIDED: VT1 = Pinecore Commando by default, VT2 = COMMAND.COM
DECIDED: Shell runs in kernel mode (no V86, direct driver access)
DECIDED: Each COMMAND.COM gets its own V86 task + DOS task
```

---

## Key References

| Source | Covers |
|--------|--------|
| Linux VT subsystem | Conceptual model for terminal switching |
| v86.h text_buf[] | Per-task text buffer already exists |
| keyboard.c | Ring buffer, scancode handling |
| vga.c | Text mode rendering |
| fat.c | File operations for shell commands |
| dos.c console callbacks | Per-task I/O routing |

---

*Last updated: 2026-04-30*
