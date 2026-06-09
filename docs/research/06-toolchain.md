# Toolchain — DJGPP Can't Do Bare Metal, Need i386-elf

> DJGPP is locked to DOS/DPMI. A standalone kernel needs an i386-elf cross-compiler.

**Date:** 2026-04-28
**Status:** Complete — hard constraint identified

---

## Findings

### DJGPP Limitations for Bare Metal

Investigated the DJGPP toolchain at `/Users/chelsonaitcheson/Projects/djgpp_10/`:

**Target:** `i586-pc-msdosdjgpp` — hardcoded to DOS DPMI target

**Linker output formats (from objdump -i):**
- `coff-go32` — COFF object files
- `coff-go32-exe` — COFF executables with DOS stub
- `a.out-i386` — legacy
- `srec`, `binary`, `ihex`, `tekhex` — raw formats

**Cannot produce ELF.** The linker (`ld 2.30`) only supports `i386go32` emulation. No ELF support at all.

**crt0.o is deeply DOS-dependent:**
- Calls INT 0x31 (DPMI) during startup — sets up selectors, allocates memory
- Calls INT 0x21 (DOS) — exit handling
- References `___djgpp_base_address`, `___djgpp_selector_limit`, `sbrk16_*`, `exit16_*`
- Cannot be used without DPMI and DOS underneath

**Linker scripts assume DOS layout:**
- Entry point `start` at 0x1000
- Expects `__environ` and other DJGPP symbols
- Sections aligned for COFF-go32 format

**Even with `-ffreestanding -nostdlib -nostartfiles`:**
- Linker still produces COFF-go32 format
- No way to target ELF or flat binary for bootloader loading

**Freestanding headers available (GCC built-ins):**
- `stdint.h`, `stddef.h`, `stdarg.h`, `stdbool.h`, `float.h` — these work
- Everything in `/include/` (stdio.h, stdlib.h, etc.) depends on `sys/djtypes.h` and DJGPP runtime

**DJGPP has a `__dj_ENFORCE_ANSI_FREESTANDING` macro** that strips some DOS dependencies from headers, but it's not enough for bare metal.

### What We Need Instead

**An i386-elf cross-compiler.** Standard approach for OS development:

**Option A: Build GCC cross-compiler**
- Target: `i686-elf` (32-bit x86, no OS)
- Produces ELF binaries — loadable by GRUB, our own bootloader, or convertible to flat binary
- Freestanding by default — no OS assumptions
- This is the standard OSDev approach (wiki.osdev.org)

**Option B: Use the DJGPP GCC source to build i686-elf**
- The GCC 12.2.0 source is already at `djgpp_10/gnu/gcc-12.20/`
- Reconfigure with `--target=i686-elf` instead of `--target=i586-pc-msdosdjgpp`
- Need to also build binutils for i686-elf target

**Option C: Use LLVM/Clang**
- Clang can target i386-elf natively without a cross-compiler build
- `clang --target=i686-elf -ffreestanding -nostdlib`
- Simpler setup, but less tested for OSDev

### Recommended: Build i686-elf-gcc

```bash
# Typical OSDev cross-compiler build:
export TARGET=i686-elf
export PREFIX=/opt/cross

# Build binutils
cd binutils-2.40
./configure --target=$TARGET --prefix=$PREFIX --with-sysroot --disable-nls
make && make install

# Build GCC (C only, freestanding)
cd gcc-12.2.0
./configure --target=$TARGET --prefix=$PREFIX --disable-nls \
  --enable-languages=c --without-headers
make all-gcc all-target-libgcc
make install-gcc install-target-libgcc
```

### Can We Keep DJGPP for Anything?

Yes — DJGPP is still useful for:
- Building DOS utilities/tools that run under the kernel's V86 mode
- Reference for how DJGPP's libc wraps DOS/DPMI calls
- The Allegro source code (portable parts) can be compiled with either toolchain

But the kernel itself MUST be built with i686-elf-gcc.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| DJGPP GCC config | djgpp_10/i586-pc-msdosdjgpp/bin/gcc | Target triplet, specs |
| DJGPP linker scripts | djgpp_10/i586-pc-msdosdjgpp/lib/ldscripts/ | COFF-go32 format constraints |
| DJGPP crt0.o | djgpp_10/i586-pc-msdosdjgpp/lib/crt0.o | DOS/DPMI dependency in startup |
| GCC source | djgpp_10/gnu/gcc-12.20/ | Could rebuild with --target=i686-elf |

---

*Last updated: 2026-04-28*
