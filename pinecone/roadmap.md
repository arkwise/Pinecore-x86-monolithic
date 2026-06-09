# Pinecone Desktop Environment — Roadmap

> A Windows 2000-style desktop for the Pinecore OS. Built with Allegro 4.2.2 + DJGPP,
> targets DOS (CWSDPMI host) initially, then Pinecore's native DPMI host.

**Created:** 2026-05-25 (session 34)
**Status:** Phase 1 in progress

---

## Strategic Purpose

Two goals, in order:

1. **Bisect the DPMI host.** DOOM under DOS/32A still `#GP`s at `0xFF:0x913` after 5
   sessions of investigation (see `../docs/research/32-doom-gp-investigation.md`).
   We don't yet know whether the bug is in DOS/32A's `int_main` stack discipline
   (current leading hypothesis H12) or in Pinecore's DPMI host itself. A small,
   instrumented DJGPP+CWSDPMI client we control end-to-end gives us a much smaller
   target to bisect against. If Pinecone runs under CWSDPMI on FreeDOS but fails
   under Pinecore, the bug is in our host. If both fail, the bug is in our client.
   Either way we narrow the search.

2. **Build the actual desktop.** Long term, Pinecone is the user-facing shell for
   Pinecore — Windows 2000-themed, mouse-driven, cooperatively multitasked,
   capable of running real DOS apps under V86. This is what people will see when
   they boot Pinecore.

The phases below are ruthlessly minimal at the start. We add nothing until the
previous phase boots cleanly.

---

## Phase 1 — Bare-Minimum Proof of Life (current)

**Exit criterion:** A `DESKTOP.EXE` binary that, when run, shows a splash, draws
one Win2000-style window with a title bar, polls the mouse and keyboard, and exits
cleanly on ESC. Must run under both CWSDPMI (FreeDOS in QEMU) and Pinecore's DPMI
host.

Scope:
- Single `main.c` (no module split yet)
- Allegro `set_gfx_mode(GFX_AUTODETECT, 640, 480, 16-bit)` via VESA
- Splash screen: navy background, "Pinecone Desktop Environment" centred
- One fake window: Win2000 raised bevel, dark-blue title bar, "About Pinecone" text
- Simple taskbar at bottom with a "Start" button (non-functional)
- Mouse cursor (Allegro built-in)
- Keyboard polling for ESC
- Live coordinate readout to confirm input loop is alive

Out of scope for Phase 1:
- Multiple windows
- Window drag/resize
- Real widgets
- Login screen
- Registry
- Theme system (colors are hardcoded for now)

---

## Phase 2 — Window Manager Core

**Exit criterion:** Two or more overlapping windows with z-order, click-to-focus,
drag-by-title-bar. Mouse cursor stays smooth during compositing.

- `struct window` with x/y/w/h/title/z/state/content_bitmap
- Window list with z-order stack
- Dirty-rectangle redraw
- Drag-to-move via title bar hit-test
- Close button hit-test
- Compositing back buffer

Reference: Pinecore research doc `04-allegro-gui.md` already lays out the
compositor design.

---

## Phase 3 — Widget System

**Exit criterion:** A window can host buttons, labels, edit fields, listboxes,
and route mouse/keyboard events to the focused widget.

- Widget base struct (`widget_t`: rect, parent_window, vtable {draw, click, key})
- Built-in widgets: button, label, edit, listbox, checkbox
- Focus management within a window
- Event routing from WM → window → focused widget
- Win2000 chiseled bevels (raised/sunken) as a primitive

---

## Phase 4 — Theme System

**Exit criterion:** All hardcoded Win2000 colours move into a single `theme.h`,
swappable at runtime.

- Color palette struct
- Themed primitives (`theme_draw_button`, `theme_draw_titlebar`, etc)
- Win2000 palette as default
- (Stretch) NT 4.0 and Win95 alt themes

---

## Phase 5 — Registry

**Exit criterion:** A binary registry file at `\PINECONE\REGISTRY.DAT` storing
hive-style key/value pairs; users and per-user settings live here.

- Hive structure (HKEY_USERS, HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER)
- Atomic write (write-temp-then-rename)
- API: `reg_open`, `reg_read`, `reg_write`, `reg_enum`
- Schema for user accounts: name, password hash (or empty for demo mode), home dir, theme

---

## Phase 6 — Login Screen

**Exit criterion:** Boot lands on a login screen with a username dropdown and a
password field. Selecting "Demo User" enters the desktop with no password.

- NT-style centred dialog with logo
- Username dropdown populated from registry
- Password field (masked)
- Demo Mode user always present, no password
- On success: enter Phase 2+ desktop

---

## Phase 7 — Cooperative Multitasking

**Exit criterion:** Multiple Pinecone "apps" (windows hosting independent event
loops) run concurrently inside a single CWSDPMI/Pinecore process via a
cooperative scheduler.

- App registration API
- Event-loop yield discipline
- Per-app message queue
- Idle handler

---

## Phase 8 — V86 Integration (Pinecore-only)

**Exit criterion:** Launching a DOS app from Pinecone spawns a V86 task on
Pinecore (via the existing `vt_create_dos_exec` path) and the app runs in its
own window. (Initially text mode only — graphical V86 apps need a windowed
VGA/VESA emulation we don't yet have.)

This phase only lights up when running on Pinecore. Under DOS/CWSDPMI it's a
no-op.

---

## Phase 9 — Polish

- Splash bitmap (replace text-only splash)
- Cursor theme
- Sound (startup chime)
- File manager window
- Settings window backed by registry
- Win2000 sounds and icon set

---

## Build & Test Targets

| Target | What it does |
|--------|--------------|
| `make` | Build DESKTOP.EXE (DJGPP cross-compile from macOS) |
| `make freedos-img` | Build a FreeDOS QEMU image with DESKTOP.EXE + CWSDPMI.EXE |
| `make run-freedos` | Boot the FreeDOS image in QEMU |
| `make pinecore-img` | Copy DESKTOP.EXE onto Pinecore's HDD image (no CWSDPMI — Pinecore is the host) |
| `make run-pinecore` | Boot Pinecore in QEMU with DESKTOP.EXE on disk |
| `make clean` | Wipe build artifacts |

---

## Toolchain (Pinned)

| Tool | Path | Notes |
|------|------|-------|
| Cross-gcc | `/Users/chelsonaitcheson/Projects/djgpp_12/bin/i586-pc-msdosdjgpp-gcc` | GCC 12.2.0, Mach-O native |
| DJGPP env | `/Users/chelsonaitcheson/Projects/djgpp_12/djgpp.env` | export DJGPP=this |
| Allegro lib | `/Users/chelsonaitcheson/Projects/djgpp_12/lib/liballeg.a` | 4.2.2 |
| CWSDPMI | `/Users/chelsonaitcheson/Projects/djgpp_12/bin/cwsdpmi.exe` | Bundled with FreeDOS test image |

---

## Reference Docs (in parent project)

- `../docs/research/04-allegro-gui.md` — what Allegro gives us, what we must build
- `../docs/research/07-allegro-portability.md` — portability map
- `../docs/research/08-windows98-model.md` — desktop architecture model
- `../docs/research/29-dpmi-host.md` — Pinecore's DPMI host spec
- `../docs/research/32-doom-gp-investigation.md` — why we need a smaller test client

---

# Addendum — App Suite + Driver Roadmap (s40)

The phases above are the **engine layer** (window manager, widgets, theme system). This addendum is the **content layer** (Win95-equivalent bundled apps) and the **driver layer** (real-hardware support). Both are what the audience actually sees.

## Thesis

Make Pinecore a *viable* desktop OS by shipping the minimum Windows 95-equivalent toolset plus a **self-hosting dev kit**. After that, the community fills in the rest. Three deliverables make the platform credible:

1. **Working DOS prompt** — real `COMMAND.COM` in V86 with a tty bridged to a desktop window. NTVDM-style.
2. **Visual IDE with GCC integration** — drag-drop widget editor, code editor, build through DJGPP, run with one click. Once a Pinecore user can build a Pinecone app *on* Pinecore, the platform reproduces itself.
3. **Enough bundled apps** that the desktop *feels* usable: Notepad, Calc, Paint, Explorer, Prompt, plus the IDE.

The current immediate-mode desktop runs as a DPMI client (`DESKTOP.EXE`); each "app" is a window inside it, not a separate process. Process isolation lands in Pineapple 3 — see `project_pineapple3_widget_editor` memory.

## Modular architecture (in progress, s40)

`pinecone/src/main.c` currently ≈2300 LOC. Refactoring into:

```
src/
  main.c                  ← init + main loop only
  core/
    palette.{h,c}         ← Win2K color table
    draw.{h,c}            ← gradients, raised/sunken bevels
    input.{h,c}           ← mouse + hover + click-consumption state
    widgets.{h,c}         ← button, check, radio, slider, combo, list, tabs
    window.{h,c}          ← window_t, draw_window_frame, z-order
    dropdown.{h,c}        ← deferred drop-down renderer
  shell/
    splash.{h,c}          ← boot splash + Doom-fire
    desktop.{h,c}         ← desktop icons + wallpaper
    start.{h,c}           ← start menu + submenus
    taskbar.{h,c}         ← taskbar + clock + task tiles
  apps/
    about.{h,c}           ← About Pinecone
    bugreport.{h,c}       ← Bug Report dialog
    shutdown.{h,c}        ← Shut Down confirm
    widgetgallery.{h,c}   ← current Widget Gallery sampler
    [new apps below land here]
```

**Invariant: widget function signatures stay stable.** The Visual IDE's code generator targets the `core/widgets.h` API exactly.

## Phase A — Core Apps (~Win95 minimum to call it a desktop)

| # | App | Effort | What it does | Win95 equiv | Depends on |
|---|---|---|---|---|---|
| A1 | **Calculator** | 0.5 sess | Basic + scientific. Keyboard via `key[]` bypass. | `calc.exe` | widget set |
| A2 | **Notepad** | 1 sess | Single-doc editor. Open/Save via DPMI 0x300 → INT 21h. Word wrap. | `notepad.exe` | text edit widget |
| A3 | **Pinecone Explorer** | 1.5 sess | File browser. Promotes the Widget-Gallery Files tab to a standalone window. Tree + icon/list/details panes. | `explorer.exe` | tree-view widget |
| A4 | **MS-DOS Prompt (Tier 1)** | 1 sess | Terminal widget + 8 built-ins (DIR/CD/CLS/TYPE/ECHO/HELP/COPY/EXIT) via DPMI 0x300. Walks the real FAT. | `command.com` window | terminal widget, key[] bypass |
| A5 | **MS-DOS Prompt (Tier 3)** | 3 sess | Real `COMMAND.COM` in V86, prompt window is its tty. New kernel DPMI services: `spawn_v86_with_tty / read_tty / write_tty`. Headless VT. | `cmd.exe`/NTVDM | A4 + kernel work |
| A6 | **Paint** | 2 sess | Pencil/line/rect/fill/eraser/picker. Load/save PCX or BMP via INT 21h. | `mspaint.exe` | extra primitives |
| A7 | **Find** | 0.5 sess | Recursive file find via INT 21h 4Eh/4Fh. Already stubbed in Start menu. | `find.exe` | A3 |
| A8 | **Run...** | 0.25 sess | Modal "Run a program" → EXEC. Already stubbed. | `run.exe` | A4 or A5 |
| A9 | **Control Panel** | 1.5 sess | Container for applets: Display, Sound, Time/Date, About. | `control.exe` | small widgets |

**Phase A acceptance:** every Start-menu item opens something. User can: open Notepad, type a file, save it, browse to it in Explorer, double-click to reopen.

## Phase B — The Killer App: Pinecone IDE

**~6 sessions total.** This is the platform's pitch.

### B1 — Code editor (1.5 sess)
Multi-tab text editor with syntax highlighting (C, ASM, Makefile, shell). Open/Save via INT 21h. Built on Notepad's text-edit widget.

### B2 — Visual widget editor (2 sess)
Canvas with drag-drop palette: button / check / radio / slider / combo / list / tabs / group / progress / textfield. Selected widget → property pane (size, label, tooltip, initial state, group, hook name). **Output is C code** matching `core/widgets.h` — each widget becomes a `static int g_state;` + a call in `draw_FORM(BITMAP*)` + a `// TODO: on_click_NAME` hook stub. Save as `.PFM` (Pinecone Form text format).

### B3 — Build integration (1 sess)
"Build" button: writes a temp Makefile, invokes DJGPP `gcc` via EXEC chain (Tier-3 Prompt is a hard dep). Output pane shows compiler errors with clickable file:line jump to editor.

### B4 — Run / Debug (1.5 sess)
"Run" button: spawns the built EXE as a new DPMI client window. Basic single-step debugger via `INT 3` traps using DPMI's existing exception delivery.

**Phase B acceptance:** boot Pinecore, open IDE, drag widgets onto a form, write a click handler in C, Build → Run, the new app appears as a window. Closes the "self-hosting platform" loop.

## Phase C — Polish (raises "demo" to "fun to use")

| # | App | Effort | Notes |
|---|---|---|---|
| C1 | **Solitaire** | 1 sess | The Win95 icon. Card sprites, drag-drop stacks. |
| C2 | **Minesweeper** | 0.5 sess | Even simpler. |
| C3 | **CD Player / Media Player** | 2 sess | Blocked on Phase D audio driver. |
| C4 | **HyperTerminal** | 1.5 sess | Serial comms — useful for debugging Pinecore itself. |
| C5 | **Sound Recorder** | 1 sess | Blocked on audio driver. |
| C6 | **Character Map** | 0.25 sess | Pick glyphs from the loaded font. |
| C7 | **System Information** | 0.5 sess | CPU/RAM/drives/IRQ map. |
| C8 | **Pinecore Help (.HLP viewer)** | 1.5 sess | Hypertext help. F1 from anywhere. |

## Phase D — Driver Roadmap (usability = hardware support)

Strategy: **start with what the user already has, build outward**.

### Tier 1 — Already targeted (sources cloned locally, see memory)
| Driver | Source | Target hardware | Memory ref |
|---|---|---|---|
| Intel e1000e (82567LM-3) | Linux 6.6 sparse-checkout at `~/Projects/linux-e1000e-ref/` | T400 / X200 / ICH9 NICs | `reference_linux_e1000e_clone` |
| USB HID + xHCI/UHCI | USBDDOS fork at `~/Projects/USBDDOS-master/` | USB-only laptops | `reference_usbddos_clone` |

### Tier 2 — Audience-essential (Phase D close-in)
| Driver | Why | Reference |
|---|---|---|
| **Intel HDA** (Realtek codecs) | Sound — gates Paint/MediaPlayer/games | Linux `sound/pci/hda/` |
| **AHCI SATA** + **NVMe** | Storage on post-IDE hardware; gates "install onto modern laptops" | Linux `drivers/ata/`, `drivers/nvme/` |
| **Intel GMA modeset** (900/950/X3xxx) | Real graphics on 2005-2012 laptops; replaces Bochs VBE | Linux `drivers/gpu/drm/i915/` |
| **PS/2 → USB transition** | PS/2 driver fallback on USB-only laptops | local USBDDOS work |

### Tier 3 — Demo / cred (Phase D far-end)
| Driver | Why | Reference |
|---|---|---|
| **3dfx Voodoo Banshee/3/5** | DOSCore games team audience; first 3D | DOSBox-X Voodoo emulation |
| **ATI Rage Pro / Rage 128** | XFree86-era 3D | XFree86 `rage128` |
| **Intel iwlwifi** (5300/5350/6230) | Modern WiFi | Linux `drivers/net/wireless/intel/iwlwifi/` |
| **MediaTek WiFi** | Different chipset family | Linux `drivers/net/wireless/mediatek/` |

### Driver invariant
Each kernel driver exposes both layers:
1. **Modern chip-level interface** in `src/kernel/drivers/`
2. **DOS-API shim**: INT 60h packet driver / SB16 port emulation (0x220/IRQ5/DMA1) / VBE 2.0 INT 10h / INT 13h block — unmodified DOS apps see "the right hardware."

This is **NTVDM-on-native-kernel** from `project_platform_thesis`. Real drivers + DOS shims = DOS apps run on hardware DOS doesn't natively support.

## Cross-cutting blockers
| Blocker | Affects | Status |
|---|---|---|
| s38 `_stubinfo` size-shift bug | DJGPP `keypressed()/readkey()` — A2, A4, B1, B2 | Priority 1 in `SESSION-STATE`. Workaround: `key[]`-array bypass + hand-rolled scancode→ASCII (~150 LOC). |
| Headless V86 + DPMI tty services | A5, B3, B4 | New kernel work, ~1 session in `vt.c`+`dpmi.c`. Spec: `spawn_v86_with_tty`, `read_tty`, `write_tty`, headless VT flag. |
| Pineapple 3 kernel-task model | True multi-app desktop with process isolation | Post-Pinecore 2. Today everything is one DPMI client. |

## Suggested execution order

1. **Refactor `main.c` → modular layout** (this session)
2. **Notepad (A2)** — proves text editing + key[] bypass
3. **MS-DOS Prompt Tier 1 (A4)** — terminal widget + DPMI INT 21h
4. **Calculator (A1)** — quick polished win
5. **Pinecone Explorer (A3)** — file browser
6. **Kernel: headless V86 tty services** — unlocks A5 + B3/B4
7. **MS-DOS Prompt Tier 3 (A5)** — real `COMMAND.COM`
8. **IDE — Code Editor (B1)**
9. **IDE — Visual Widget Editor (B2)**
10. **IDE — Build / Run (B3, B4)** — self-hosting milestone
11. **Game apps (C1, C2)** — polish for v0.2.0 demo
12. **Driver work (Phase D)** — parallel branch from any point
