# Allegro Portability — Stripping to a Bare-Metal Software Renderer

> 95% of Allegro's rendering code has zero DOS dependencies. We just need a thin platform layer.

**Date:** 2026-04-28
**Status:** Complete — portability map established

---

## Findings

### The Architecture

Allegro separates rendering from platform via a **VTable dispatch system**:

```
Your code → Allegro API (textout, blit, draw_sprite) → VTable function pointers → C rendering code
                                                                                    ↓
                                                                             Writes pixels to BITMAP.dat memory
```

The BITMAP struct (allegro: include/allegro/gfx.h:274-289) is just:
- `w`, `h` — dimensions
- `dat` — pointer to pixel memory (can be ANY memory — malloc, framebuffer, etc.)
- `line[]` — array of pointers to each scanline
- `vtable` — function pointer table for drawing operations
- `seg` — DOS segment selector (stub to 0 on bare metal)
- `clip`, `cl/cr/ct/cb` — clipping rectangle

**The `dat` pointer is the key.** Point it at your VESA linear framebuffer and Allegro draws directly to the screen.

### 100% Portable (No DOS Dependencies)

These files compile and run on bare metal with zero changes:

**`src/c/` — Pure C software renderer (the core):**
| File | What It Does |
|------|-------------|
| `cgfx8.c`, `cgfx15.c`, `cgfx16.c`, `cgfx24.c`, `cgfx32.c` | Lines, rects, circles, arcs, polygons (all colour depths) |
| `cblit8.c`, `cblit16.c`, `cblit24.c`, `cblit32.c` | Bitmap blitting |
| `cscan8.c` through `cscan32.c` | Polygon/shape scanline filling |
| `cspr8.c` through `cspr32.c`, `ccsprite.c` | Sprite drawing with transparency |
| `czscan8.c` through `czscan32.c` | Z-buffer operations |
| `cmisc.c` | Miscellaneous portable routines |
| `cstretch.c` | Stretch blitting |

Only includes: `<string.h>` and Allegro's own headers. No I/O, no BIOS, no interrupts.

**Core library files:**
| File | What It Does | Dependencies |
|------|-------------|-------------|
| `color.c` | makecol(), getr/g/b, palette tables | `<limits.h>`, `<string.h>`, `<math.h>` only |
| `blit.c` | Format conversion blitting, vtable dispatch | `<string.h>` only |
| `text.c` | textout_ex(), textprintf_ex() | Delegates to font vtable |
| `font.c`, `fontbmp.c` | Font loading, bitmap font rendering | Platform-agnostic |
| `vtable.c`, `vtable8.c`-`vtable32.c` | VTable registry, function pointer tables | Self-contained |
| `graphics.c` | create_bitmap(), create_bitmap_ex() | `malloc()` only (bitmap allocation) |
| `gfx.c` | drawing_mode(), set_clip_rect() | Minor stubs needed (vsync, gfx_driver) |

**`src/i386/` — x86 ASM optimised versions:**
- Same functions as `src/c/` but faster
- Pure CPU instructions — no BIOS, no I/O ports
- Optional — C versions work fine

### 100% DOS-Locked (Must Replace)

| File | What It Does | Why It's DOS-Locked |
|------|-------------|-------------------|
| `src/dos/vesa.c` | VESA mode setting | Uses INT 10h via DPMI, `dosmemput()`, `dosmemget()` |
| `src/dos/dtimer.c` | Timer interrupt | Hooks INT 8 via DPMI |
| `src/dos/dkeybd.c` | Keyboard | Hooks INT 9 via DPMI |
| `src/dos/dmouse.c` | Mouse | Uses INT 33h via DPMI |
| `src/dos/djirq.c` | IRQ wrappers | DJGPP-specific interrupt handling |
| `src/dos/dsystem.c` | System init/cleanup | DOS signal handling, console state |
| All other `src/dos/*.c` | Various DOS platform | DMA, sound, joystick — all DOS-specific |

### Our Platform Layer (~200 lines)

Replace all of `src/dos/` with a thin bare-metal platform layer:

```c
// vesa_bare.c — the only platform code we need

// 1. Set VESA mode (call INT 10h via V86 task, or use VBE 2.0 LFB)
BITMAP *init_screen(int w, int h, int bpp) {
    // Set VESA mode via our V86 monitor
    uint32_t framebuffer_addr = set_vesa_mode(w, h, bpp);

    // Create Allegro bitmap pointing to VRAM
    BITMAP *screen = create_bitmap_ex(bpp, w, h);
    screen->dat = (void *)framebuffer_addr;
    for (int i = 0; i < h; i++)
        screen->line[i] = (uint8_t *)framebuffer_addr + (i * w * (bpp / 8));
    screen->vtable = _get_vtable(bpp);
    return screen;
}

// 2. Vsync via VGA port (no BIOS needed)
void vsync(void) {
    while (!(inb(0x3DA) & 0x08));  // wait for retrace start
    while ((inb(0x3DA) & 0x08));   // wait for retrace end
}

// 3. That's it — Allegro's C renderer handles everything else
```

### What About malloc()?

Allegro's `create_bitmap()` uses `malloc()`. On bare metal we need a kernel heap:
- Simple: linked-list allocator (sbrk-style) — ~100 lines
- Better: slab allocator for common sizes — ~300 lines
- The allocation pattern is simple — bitmaps are created infrequently and are large

### Font Rendering

Allegro's font system is fully portable:
- `FONT` struct has a vtable with `render()`, `char_length()`, `text_length()`
- Bitmap fonts are just pixel data — no DOS dependency
- Allegro's built-in 8x8 font (`font`) is available without any file I/O
- For custom fonts: load from a datafile or embed as a C array

---

## Summary: What To Port vs What To Write

| Component | Approach | Lines (est.) |
|-----------|----------|-------------|
| Software renderer | Port from Allegro `src/c/` — copy directly | ~15,000 lines (already written) |
| VTable system | Port from Allegro — copy directly | ~1,000 lines |
| Bitmap management | Port from Allegro `graphics.c` | ~500 lines |
| Color/text/font | Port from Allegro — copy directly | ~2,000 lines |
| VESA platform layer | Write new | ~200 lines |
| Keyboard driver | Write new (PS/2) | ~200 lines |
| Mouse driver | Write new (PS/2) | ~200 lines |
| Timer driver | Write new (PIT) | ~100 lines |
| Malloc/heap | Write new | ~100-300 lines |
| **Total new code** | | **~800-1000 lines** |
| **Total ported code** | | **~18,500 lines (already exists)** |

The ratio is excellent — we write ~1000 lines of platform code and get ~18,500 lines of tested rendering for free.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| Allegro BITMAP struct | lwp/sources/allegro/include/allegro/gfx.h:274-289 | Struct layout, vtable pointer |
| Allegro C renderer | lwp/sources/allegro/src/c/ | All portable drawing code |
| Allegro VTable | lwp/sources/allegro/src/vtable*.c | Function pointer dispatch |
| Allegro VESA (reference only) | lwp/sources/allegro/src/dos/vesa.c | What to replace |
| Allegro font system | lwp/sources/allegro/src/font.c, text.c | Portable font rendering |

---

*Last updated: 2026-04-28*
