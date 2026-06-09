# Allegro 4.4.3 GUI System — What We Have and What We Need to Build

> Allegro provides drawing primitives, input, and timers. The window manager must be ours.

**Date:** 2026-04-28
**Status:** Complete — portability confirmed in ch-07

---

## Findings

### What Allegro Provides

**Graphics (allegro: src/dos/vesa.c, src/graphics.c):**
- VESA 1.x/2.0/3.0 support with banked and linear framebuffer modes
- Resolutions: 320x200 through 1600x1200 (allegro: src/modesel.c)
- Colour depths: 8, 15, 16, 24, 32-bit
- Sub-bitmaps: can create a bitmap that maps to a region of another bitmap
- Double buffering via off-screen bitmaps + `blit()`
- Drawing primitives: line, rect, circle, polygon, filled versions, text

**GUI System (allegro: src/gui.c, src/guiproc.c):**
Allegro has a dialog-based GUI, NOT a windowing system:
- `DIALOG` struct: proc function, x, y, w, h, fg/bg colour, key, flags, data
- Built-in procs: button, checkbox, radio, text, edit, list, slider, menu, textbox, icon
- Modal: `do_dialog()` blocks until dialog closes
- Non-modal: `init_dialog()` / `update_dialog()` / `shutdown_dialog()`
- Message-driven: MSG_DRAW, MSG_CLICK, MSG_KEY, MSG_IDLE, MSG_GOTFOCUS, etc.
- Focus management for mouse and keyboard

**What it does NOT have:**
- No overlapping windows
- No z-order management
- No window decorations (title bars, borders, close buttons)
- No drag-to-move or resize
- No window manager of any kind

### Input (allegro: src/dos/dkeybd.c, src/dos/dmouse.c)

**Keyboard:**
- Hooks INT 9 (keyboard IRQ) via DPMI
- Scancode-to-ASCII translation
- Key repeat
- `key[]` array for instant state polling
- `readkey()` / `keypressed()` for buffered input

**Mouse:**
- Hooks INT 0x33 (mouse driver)
- Position tracking, button state
- `mouse_x`, `mouse_y`, `mouse_b` globals
- Speed control, range limiting
- Hardware or software cursor

### Timers (allegro: src/dos/dtimer.c)

- Hooks INT 8 (PIT timer) via DPMI
- `install_int()` / `install_int_ex()` — install timer callbacks
- Callbacks run in interrupt context (must be locked in memory)
- `LOCK_FUNCTION()` / `LOCK_VARIABLE()` — prevent paging of interrupt code
- Resolution: configurable, default ~70Hz (matches DOS timer)

### What We Must Build

**Window Manager (not in Allegro):**
1. Window structure: position, size, title, content bitmap, z-order, state (normal/minimized/maximized)
2. Window list with z-order stack
3. Title bar rendering with close/minimize/maximize buttons
4. Window border and resize handles
5. Click-to-focus / raise-to-top
6. Drag title bar to move
7. Dirty rectangle tracking for efficient redraws
8. Desktop background (bottom of z-order)
9. Taskbar (top of z-order, always visible)

**Terminal Widget (not in Allegro):**
1. Fixed-width text buffer (80 columns x 25+ rows)
2. Cursor position and blinking
3. Scrollback buffer
4. Character-by-character rendering into a bitmap
5. ANSI escape code support (optional, for colour)
6. Input queue for keyboard forwarding

### Rendering Strategy

**Double-buffered compositing:**
1. Allocate a screen-sized back buffer
2. Draw desktop background to back buffer
3. For each window (bottom to top in z-order):
   a. Draw window frame (title bar, borders)
   b. Blit window content bitmap to client area
4. Draw taskbar on top
5. Draw mouse cursor on top
6. `blit()` back buffer to screen

**Dirty rectangles for performance:**
- Only redraw regions that changed
- Track: window moved, window content updated, window raised/lowered
- Use `set_clip_rect()` to limit drawing to dirty regions

### Allegro GUI Message System — Can We Reuse It?

Allegro's dialog system uses a message pump (`update_dialog()`) that handles:
- Mouse click routing to the widget under the cursor
- Keyboard focus tracking
- Idle processing
- Draw invalidation

We could potentially implement our windows as custom dialog procedures, but:
- Dialog procs expect flat, non-overlapping layouts
- No z-order or window occlusion support
- The message system doesn't handle window dragging

**Decision: Build our own event loop and window manager. Use Allegro only for drawing, input, and timers.**

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| Allegro GUI | lwp/sources/allegro/src/gui.c | Dialog system, message pump |
| Allegro GUI procs | lwp/sources/allegro/src/guiproc.c | All built-in widget procedures |
| Allegro graphics | lwp/sources/allegro/src/graphics.c | Bitmap creation, sub-bitmaps |
| Allegro VESA | lwp/sources/allegro/src/dos/vesa.c | DOS graphics mode setup |
| Allegro timer | lwp/sources/allegro/src/dos/dtimer.c | Timer interrupt handling |
| Allegro keyboard | lwp/sources/allegro/src/dos/dkeybd.c | Keyboard interrupt handling |
| Allegro mouse | lwp/sources/allegro/src/dos/dmouse.c | Mouse driver |
| Allegro GFX header | lwp/sources/allegro/include/allegro/gfx.h | GFX_DRIVER, capabilities |
| Allegro GUI header | lwp/sources/allegro/include/allegro/gui.h | DIALOG struct, procs |

---

*Last updated: 2026-04-28*
