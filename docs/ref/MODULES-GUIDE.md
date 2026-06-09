# Pinecore Kernel Modules — Developer Guide

> How to write a `.kmd` kernel module for Pinecore.
> Audience: driver developers (open-source or proprietary).
>
> Status: **infrastructure complete, smoke-tested with `hello.kmd`.**
> Future-stable across kernel point releases; ABI evolves only at minor versions.

---

## 1. Overview

A Pinecore kernel module is an **ELF32 i386 relocatable object file** (what `i686-elf-gcc -c` produces) renamed with the `.kmd` extension. Modules are loaded at boot (and eventually at runtime via `insmod`) into the kernel's address space, with all unresolved external symbols satisfied from the kernel's `.kexport` symbol table.

```
   hello.c  ──gcc -c──>  hello.kmd  ──>  staged into  /DRIVERS/HELLO.KMD
                                                            │
                                       Pinecore boot loads ─┘
                                                            ▼
                          parsed, allocated, copied, symbols resolved,
                          relocations applied, MODULE_LICENSE checked,
                                  module_init() called.
```

There is no Pinecore-specific binary format. Build tooling = whatever standard i386 freestanding toolchain you already have. License = your call (see §5).

---

## 2. Quick start — Hello world

`hello.c`:

```c
#include "module.h"

extern void serial_puts(const char *s);
extern void vga_puts(const char *s);

static int hello_init(void) {
    serial_puts("hello.kmd: alive\n");
    vga_puts("  [OK] hello.kmd loaded\n");
    return 0;          /* 0 = success; non-zero aborts the load */
}

static void hello_exit(void) {
    serial_puts("hello.kmd: bye\n");
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Your name");
MODULE_DESCRIPTION("Smoke-test kernel module");
MODULE_NAME("hello");
```

Build:

```
i686-elf-gcc -m32 -march=i386 -ffreestanding -nostdlib \
             -fno-common -fno-builtin -fno-pic \
             -Os -Wall -Wextra -Iinclude \
             -c hello.c -o hello.kmd
```

Stage into image:

```
mcopy -i pinecore-pure-usb.img@@$((63*512)) hello.kmd ::/DRIVERS/HELLO.KMD
```

Reboot; you'll see:

```
MODULE: loaded hello (GPL-family) image=0x... size=0x...
hello.kmd: alive
```

---

## 3. The module ABI

### Required headers

Modules include exactly one Pinecore header: `module.h`. Anything else they need from the kernel must be declared with `extern` (no `string.h`, no `serial.h`, no transitive kernel headers — keeps the ABI surface obvious and stable).

### Required symbols (the lifecycle)

Each module must define **exactly one** init function and **at most one** exit function, declared via:

| Macro | Effect |
|---|---|
| `module_init(fn)` | Declares `__pinecore_init` as an alias for your function. Signature: `int fn(void)`. Return 0 for success; nonzero aborts the load (and calls `module_exit` if defined). |
| `module_exit(fn)` | Declares `__pinecore_exit` as an alias for your function. Signature: `void fn(void)`. Called on module unload. |

### Module metadata strings

| Macro | Effect | Required? |
|---|---|---|
| `MODULE_LICENSE(s)` | Module's license. **Determines symbol-export accessibility** (see §5). | **Yes.** Without it, the module is treated as non-GPL and `EXPORT_SYMBOL_GPL` symbols are unresolvable. |
| `MODULE_NAME(s)` | Short module name. | Recommended. |
| `MODULE_AUTHOR(s)` | Author / contact. | Recommended. |
| `MODULE_DESCRIPTION(s)` | One-line summary. | Recommended. |

Each is a `const char[]` global. The loader looks them up by name; you can also grep them out of a built `.kmd` with `i686-elf-nm` or `strings`.

---

## 4. Kernel symbols available to modules

Modules import kernel functions via plain `extern` declarations, then call them normally. The loader resolves these against the `.kexport` table at load time.

### Current export set (kernel v0.2)

| Symbol | Header (ref) | Tag | Notes |
|---|---|---|---|
| `kmalloc(size_t)` | `heap.h` | open | Returns NULL on failure; zero-initialised. |
| `kfree(void *)` | `heap.h` | open | Safe to call with NULL. |
| `serial_puts(const char *)` | `serial.h` | open | Logging to COM1. |
| `serial_puthex(uint32_t)` | `serial.h` | open | Prints `0x` + 8 hex digits. |
| `vga_puts(const char *)` | `vga.h` | open | Logging to VGA text mode. |
| `strcmp`, `strlen` | `libc/string.c` | open | Standard semantics. |
| `memcpy`, `memset` | `libc/string.c` | open | Standard semantics. |

This set is the bootstrap minimum (9 symbols). Subsystem-specific exports (USB, PCI, IRQ, DMA, block, kbd) are added as the subsystems land. Check the current state at runtime by running the (not-yet-built) `lsmod -e` or by reading `__kexport_start..__kexport_end` in the kernel binary.

### Discovering exports

Until `lsmod -e` exists, the canonical list is `src/kernel/kexports.c` plus any `EXPORT_SYMBOL[_GPL]()` lines colocated with their subsystem.

---

## 5. License model (`EXPORT_SYMBOL` vs `EXPORT_SYMBOL_GPL`)

The kernel itself is **GPL-2.0**. Modules can be:

| `MODULE_LICENSE(...)` value | Treated as | Can use `EXPORT_SYMBOL` | Can use `EXPORT_SYMBOL_GPL` |
|---|---|---|---|
| `"GPL"`, `"GPL v2"`, `"GPL-2.0"`, `"LGPL"`, `"Dual BSD/GPL"` … (any string containing "GPL") | GPL-family | ✅ | ✅ |
| `"Proprietary"`, `"Closed"`, anything without "GPL" | non-GPL | ✅ | ❌ |
| (missing) | non-GPL | ✅ | ❌ |

The detection is **substring match on "GPL" / "gpl"** in the license string — so `"LGPL-2.1"` and `"GPL v3"` and `"Dual GPL/BSD"` all qualify.

### Why two tags?

- `EXPORT_SYMBOL(x)` is for stable, ABI-narrow primitives (memory, logging, time, basic I/O). The header glue to call these is intended to be permissively-licensed (LGPL-style), so closed-source drivers can link against the shim. **Use this by default when adding exports.**

- `EXPORT_SYMBOL_GPL(x)` is for symbols that expose kernel internals deeply enough that a closed-source caller would be a derivative work in spirit (think: scheduler internals, page-table manipulation, vendor-specific quirks tables). **Use this when in doubt about whether the boundary is clean.**

### What this gives the ecosystem

- Open-source drivers (GPL/LGPL/dual) get the full kernel API.
- Proprietary drivers get a stable LGPL-equivalent shim: memory, logging, time, I/O.
- The kernel-internal interfaces stay reserved for GPL code (preserving the project's GPL license).

The "is it GPL?" check happens **once at load time** against `__pinecore_license`. Mismatch on an `EXPORT_SYMBOL_GPL` symbol = load failure with a clear diagnostic.

---

## 6. Where modules live & how they load

### On-disk layout

Modules ship as files in `\DRIVERS\` on the boot partition:

```
\DRIVERS\
   HELLO.KMD
   USBCORE.KMD            (forthcoming)
   UHCI.KMD               (forthcoming)
   OHCI.KMD               (forthcoming)
   HID.KMD                (forthcoming)
```

The Pinecore image builder (`tools/build-pure-usb.py`) stages `src/modules/*.kmd` into `\DRIVERS\` when invoked with `--modules-dir`.

### Boot-time loading

At boot, after FAT mount + PCI scan + module-subsystem init, the kernel currently loads `\DRIVERS\HELLO.KMD` as a smoke test. This hardcoded path will be replaced by a directory iterator in the next iteration. Future loading sequence will:

1. Read `\DRIVERS\*.KMD` in dependency order (modules can declare deps via `MODULE_DEPENDS(...)` — not yet implemented).
2. Call `module_load_image()` for each.
3. Track refcount + dependency edges for `rmmod`.

### Manual loading (planned)

A shell `insmod path/to/foo.kmd` command will let you load modules at runtime. `rmmod` will tear them down (calling `module_exit`, dropping refcount, freeing memory) provided no other module depends on them.

---

## 7. Build conventions for module authors

Required compiler flags:

| Flag | Why |
|---|---|
| `-m32 -march=i386` | i386 ABI (matches the kernel) |
| `-ffreestanding -nostdlib` | No hosted libc |
| `-fno-common` | All globals must have a defining instance; common symbols complicate relocation |
| `-fno-builtin` | No `__builtin_memcpy` etc. — calls must go through the kernel-exported versions |
| `-fno-pic` | Modules load at a fixed address chosen by the kernel; no GOT/PLT |
| `-Iinclude` | So `#include "module.h"` works |
| `-Os` or `-O2` | Optional; either is fine |
| `-Wall -Wextra` | Recommended |

Linker considerations: **you don't link the module.** `gcc -c` produces a `.o` (a `REL` ELF); rename it to `.kmd`. No `ld` step.

Module must be **self-contained** (no calls to library functions, no global constructors). Use `extern` for every kernel function you call.

---

## 8. Relocations and what works

The loader implements three i386 ELF relocation types:

| Type | Formula | Used by |
|---|---|---|
| `R_386_NONE` | no-op | (filler) |
| `R_386_32` | `S + A` (absolute) | global variable address-of, function pointer initialisers |
| `R_386_PC32` / `R_386_PLT32` | `S + A - P` (PC-relative) | direct CALL/JMP to symbols |

Anything gcc emits for ordinary C without TLS, without dynamic linking, without C++ exceptions/RTTI fits in those three types. Symbols are resolved against (a) symbols defined in the module itself and (b) the kernel's `.kexport` table.

---

## 9. Limitations (current, will change)

| | Current | Future |
|---|---|---|
| Number of modules loaded | hardcoded to one (HELLO.KMD) | directory iteration |
| `rmmod` | not implemented | M4 |
| Module dependencies | informal | `MODULE_DEPENDS("usbcore")` |
| Per-section page protection | none (everything RWX) | distinct R-X .text / RW .data when paging gets per-page flags |
| TLS / global ctors | unsupported | unlikely in kernel modules |
| C++ | works as long as no RTTI/exceptions/globals-with-ctors | won't support full C++ |
| Dynamic symbol lookup from a module | not exposed | `module_resolve(name)` may be exported |
| Symbol versioning | not implemented | considered for v1.0 |

---

## 10. Debugging

A loaded module's `.text` lives somewhere in the kernel heap. If it crashes:

- Pinecore's panic screen (s50 infrastructure) prints `CS:EIP` and a register dump. If `EIP` is in the kernel-heap range (typically `0x17E000`+), the fault is inside a module.
- `serial_puthex((uint32_t)image)` from the loader log tells you the module's base; subtract to get the offset into the module's `.text`.
- Disassemble the module: `i686-elf-objdump -d hello.kmd`. The offset matches a function + offset in the relocatable.
- The loader logs `MODULE: loaded <name> ... image=0x... size=0x...` on every successful load. Save serial captures during bring-up to correlate later panics.

Useful pre-load checks before flashing:

```
i686-elf-readelf -h hello.kmd      # validate it's REL i386
i686-elf-readelf -s hello.kmd      # check __pinecore_init is present
i686-elf-readelf -r hello.kmd      # check relocations are sane
```

---

## 11. Why `.kmd` (not `.ko`)

Pinecore branding choice. The format is conceptually a Linux `.ko` (relocatable ELF, runtime-resolved), but the extension is `.kmd` (Pinecore **K**ernel **M**oDule) to make the heritage but-not-identity explicit. Any tooling that handles ELF `.o` files handles `.kmd`.

---

## 12. License of this document

The kernel itself is GPL-2.0. This developer guide is intended to be reusable / mirrorable: license is **CC-BY-4.0**. The headers (`module.h`, `elf.h`) referenced by modules are LGPL-2.1 so that closed-source drivers can include them.

---

## 13. References

- Kernel implementation: `src/kernel/module.c`, `src/include/module.h`, `src/include/elf.h`
- Initial exports: `src/kernel/kexports.c`
- Smoke test: `src/modules/hello.c`
- Image staging: `tools/build-pure-usb.py --modules-dir`
- Linker hook: `src/linker.ld` (`.kexport` section)
- Auto-memory: `project_kmd_module_loader_landed.md`
- USB stack plan that lands as `.kmd` files: `docs/research/48-usb-port-plan.md`
