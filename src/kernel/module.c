/* Pinecore kernel module loader — ELF32 .kmd parser + relocator + lifecycle.
 *
 * License: GPL-2.0.
 *
 * .kmd files are i386 ELF relocatable objects (ET_REL) produced by
 *   i686-elf-gcc -c -fno-common -ffreestanding ... -o foo.kmd
 *
 * Loader flow:
 *   1. Validate ELF header (class, byte order, machine, type).
 *   2. Walk section headers; size + place each loadable section
 *      (PROGBITS or NOBITS, with SHF_ALLOC).
 *   3. Allocate one contiguous block from the kernel heap, copy
 *      PROGBITS, zero NOBITS.
 *   4. Walk relocation tables (SHT_REL only — gcc -c emits REL on i386).
 *      For each reloc: resolve the referenced symbol, compute target
 *      value, patch the in-memory bytes per R_386_32 or R_386_PC32.
 *   5. Locate __pinecore_init / __pinecore_exit / __pinecore_license
 *      symbols and check license compatibility against required
 *      EXPORT_SYMBOL_GPL exports.
 *   6. Call init_fn(). If it returns non-zero, abort + free + report.
 *   7. Append to loaded-module list.
 *
 * Symbol resolution: external symbols (st_shndx == SHN_UNDEF) are
 * looked up in the kernel's .kexport table (built at boot from the
 * linker-collected section).
 */
#include "module.h"
#include "elf.h"
#include "heap.h"
#include "serial.h"

extern int strcmp(const char *a, const char *b);

/* Provided by linker.ld */
extern struct kexport __kexport_start[];
extern struct kexport __kexport_end[];

static int                       kexport_count = 0;
static struct loaded_module     *modules_head  = 0;

/* Sticky flag set by module_load_image when it fails *after* the module
 * was successfully loaded into memory and module_init returned non-zero —
 * as opposed to an earlier failure like unresolved symbols. The autoload
 * loop reads this via module_last_load_was_init_failure() to decide
 * whether to retry the module on subsequent passes. */
static int g_last_load_init_failed = 0;

int module_last_load_was_init_failure(void) {
    return g_last_load_init_failed;
}

/* ------------------------------------------------------------------
 * Diagnostic helpers
 * ------------------------------------------------------------------ */
static void mod_log(const char *s) {
    serial_puts("MODULE: ");
    serial_puts(s);
    serial_puts("\n");
}

static void mod_fail(const char *what) {
    serial_puts("MODULE: FAIL: ");
    serial_puts(what);
    serial_puts("\n");
}

/* ------------------------------------------------------------------
 * Subsystem init — log how many kernel symbols we export
 * ------------------------------------------------------------------ */
void module_init_subsystem(void) {
    kexport_count = (int)(__kexport_end - __kexport_start);
    serial_puts("MODULE: subsystem init — ");
    serial_puthex(kexport_count);
    serial_puts(" kernel symbols exported\n");
}

/* ------------------------------------------------------------------
 * Symbol resolution against .kexport
 * ------------------------------------------------------------------ */
void *module_resolve(const char *name, int for_gpl_module) {
    /* Kernel-side exports first (lower latency, never change). */
    for (int i = 0; i < kexport_count; i++) {
        const struct kexport *e = &__kexport_start[i];
        if (strcmp(e->name, name) == 0) {
            if (e->gpl_only && !for_gpl_module) return 0;
            return e->addr;
        }
    }
    /* Then each loaded module's own .kexport — order is load order, which
     * for the autoload path is FAT directory order. usbcore must come
     * before its HCD / class consumers; we rely on alphabetical 8.3
     * naming today (USBCORE < UHCI < HID < MSC). MODULE_DEPENDS will
     * make this explicit later. */
    for (struct loaded_module *m = modules_head; m; m = m->next) {
        for (uint32_t i = 0; i < m->kexport_count; i++) {
            const struct kexport *e = &m->kexports[i];
            if (strcmp(e->name, name) == 0) {
                if (e->gpl_only && !for_gpl_module) return 0;
                return e->addr;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------
 * ELF helpers
 * ------------------------------------------------------------------ */
static int elf_validate(const struct elf32_ehdr *eh) {
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1
     || eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        mod_fail("bad ELF magic"); return 0;
    }
    if (eh->e_ident[4] != ELFCLASS32)  { mod_fail("not ELF32");    return 0; }
    if (eh->e_ident[5] != ELFDATA2LSB) { mod_fail("not little-endian"); return 0; }
    if (eh->e_type != ET_REL)          { mod_fail("not ET_REL");   return 0; }
    if (eh->e_machine != EM_386)       { mod_fail("not i386");     return 0; }
    return 1;
}

static const char *get_strtab(const struct elf32_ehdr *eh,
                              const struct elf32_shdr *sh,
                              uint32_t strtab_idx) {
    return (const char *)((const uint8_t *)eh + sh[strtab_idx].sh_offset);
}

/* Is "GPL"-family license? Used for symbol gating. */
static int license_is_gpl(const char *s) {
    /* Accept "GPL", "GPL v2", "GPL-2.0", "LGPL", "Dual BSD/GPL", … */
    if (!s) return 0;
    while (*s) {
        if ((s[0] == 'G' || s[0] == 'g')
         && (s[1] == 'P' || s[1] == 'p')
         && (s[2] == 'L' || s[2] == 'l')) return 1;
        s++;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * The main loader
 * ------------------------------------------------------------------ */
struct loaded_module *module_load_image(const char *display_name,
                                        const void *buf, uint32_t size) {
    g_last_load_init_failed = 0;
    if (size < sizeof(struct elf32_ehdr)) { mod_fail("buffer too small"); return 0; }

    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)buf;
    if (!elf_validate(eh)) return 0;

    const struct elf32_shdr *sh = (const struct elf32_shdr *)
        ((const uint8_t *)buf + eh->e_shoff);
    uint32_t nsec = eh->e_shnum;
    const char *shstr = get_strtab(eh, sh, eh->e_shstrndx);

    /* Locate the symbol table + its string table (assume one of each) */
    uint32_t symtab_idx = 0, strtab_idx = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) { symtab_idx = i; strtab_idx = sh[i].sh_link; }
    }
    if (!symtab_idx) { mod_fail("no symbol table"); return 0; }

    const struct elf32_sym *sym = (const struct elf32_sym *)
        ((const uint8_t *)buf + sh[symtab_idx].sh_offset);
    uint32_t nsym = sh[symtab_idx].sh_size / sizeof(struct elf32_sym);
    const char *strtab = get_strtab(eh, sh, strtab_idx);

    /* Compute total image size: sum of allocatable sections, each
     * respecting its sh_addralign. */
    uint32_t image_size = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        uint32_t a = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
        image_size = (image_size + a - 1) & ~(a - 1);
        image_size += sh[i].sh_size;
    }
    if (image_size == 0) { mod_fail("empty module"); return 0; }

    /* Allocate one block, zero it (handles NOBITS for free). */
    uint8_t *image = (uint8_t *)kmalloc(image_size);
    if (!image) { mod_fail("out of memory"); return 0; }
    for (uint32_t i = 0; i < image_size; i++) image[i] = 0;

    /* Per-section base offsets into the image (only ALLOC sections). */
    uint32_t *sec_base = (uint32_t *)kmalloc(nsec * sizeof(uint32_t));
    if (!sec_base) { kfree(image); mod_fail("out of memory (sec_base)"); return 0; }
    for (uint32_t i = 0; i < nsec; i++) sec_base[i] = 0;

    uint32_t cursor = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        uint32_t a = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
        cursor = (cursor + a - 1) & ~(a - 1);
        sec_base[i] = (uint32_t)image + cursor;
        if (sh[i].sh_type == SHT_PROGBITS) {
            const uint8_t *src = (const uint8_t *)buf + sh[i].sh_offset;
            uint8_t *dst = image + cursor;
            for (uint32_t b = 0; b < sh[i].sh_size; b++) dst[b] = src[b];
        }
        /* NOBITS already zero from initial zero-fill */
        cursor += sh[i].sh_size;
    }

    /* Find the license string by walking the symbol table. */
    int license_gpl = 0;
    int (*init_fn)(void) = 0;
    void (*exit_fn)(void) = 0;
    for (uint32_t i = 1; i < nsym; i++) {
        const char *n = strtab + sym[i].st_name;
        if (sym[i].st_shndx == STN_UNDEF) continue;
        if (sym[i].st_shndx >= nsec) continue;
        uint32_t base = sec_base[sym[i].st_shndx];
        if (!base) continue;
        if (strcmp(n, "__pinecore_license") == 0) {
            const char *lic = (const char *)(base + sym[i].st_value);
            license_gpl = license_is_gpl(lic);
        } else if (strcmp(n, "__pinecore_init") == 0) {
            init_fn = (int (*)(void))(base + sym[i].st_value);
        } else if (strcmp(n, "__pinecore_exit") == 0) {
            exit_fn = (void (*)(void))(base + sym[i].st_value);
        }
    }

    /* Walk all REL sections, apply relocations.
     * For each REL section: sh_info = the section it relocates;
     *                       sh_link = the symbol-table section. */
    for (uint32_t i = 0; i < nsec; i++) {
        if (sh[i].sh_type != SHT_REL) continue;
        uint32_t target_sec = sh[i].sh_info;
        if (target_sec >= nsec) continue;
        uint32_t target_base = sec_base[target_sec];
        if (!target_base) continue;  /* relocations against non-loaded section */

        const struct elf32_rel *rel = (const struct elf32_rel *)
            ((const uint8_t *)buf + sh[i].sh_offset);
        uint32_t nrel = sh[i].sh_size / sizeof(struct elf32_rel);

        for (uint32_t r = 0; r < nrel; r++) {
            uint32_t sym_idx = ELF32_R_SYM(rel[r].r_info);
            uint32_t r_type  = ELF32_R_TYPE(rel[r].r_info);
            if (sym_idx >= nsym) {
                kfree(sec_base); kfree(image);
                mod_fail("bad reloc sym index"); return 0;
            }
            const struct elf32_sym *s = &sym[sym_idx];
            const char *sname = strtab + s->st_name;

            /* Resolve symbol → S */
            uint32_t S;
            if (s->st_shndx == STN_UNDEF) {
                void *kp = module_resolve(sname, license_gpl);
                if (!kp) {
                    serial_puts("MODULE: unresolved symbol: ");
                    serial_puts(sname);
                    serial_puts("\n");
                    kfree(sec_base); kfree(image); return 0;
                }
                S = (uint32_t)kp;
            } else {
                if (s->st_shndx >= nsec || !sec_base[s->st_shndx]) {
                    kfree(sec_base); kfree(image);
                    mod_fail("symbol in non-loaded section"); return 0;
                }
                S = sec_base[s->st_shndx] + s->st_value;
            }

            /* Patch target — A (addend) is embedded in the 4 bytes
             * being relocated. */
            uint32_t  P    = target_base + rel[r].r_offset;
            uint32_t *slot = (uint32_t *)P;
            uint32_t  A    = *slot;
            switch (r_type) {
            case R_386_NONE: break;
            case R_386_32:    *slot = S + A;        break;
            case R_386_PC32:
            case R_386_PLT32: *slot = S + A - P;    break;
            default:
                serial_puts("MODULE: unsupported reloc type ");
                serial_puthex(r_type);
                serial_puts("\n");
                kfree(sec_base); kfree(image); return 0;
            }
        }
    }

    /* Locate this module's own `.kexport` section so its symbols become
     * resolvable by later-loaded modules. The section name is in shstrtab
     * (eh->e_shstrndx). We use the relocated sec_base for the address. */
    const struct kexport *mod_kexports = 0;
    uint32_t mod_kexport_count = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        if (sh[i].sh_size == 0) continue;
        const char *secname = shstr + sh[i].sh_name;
        if (strcmp(secname, ".kexport") != 0) continue;
        mod_kexports      = (const struct kexport *)(uint32_t)sec_base[i];
        mod_kexport_count = sh[i].sh_size / sizeof(struct kexport);
        break;
    }

    kfree(sec_base);

    if (!init_fn) {
        mod_fail("no __pinecore_init"); kfree(image); return 0;
    }

    /* Allocate + populate loaded_module record */
    struct loaded_module *lm = (struct loaded_module *)
        kmalloc(sizeof(struct loaded_module));
    if (!lm) { mod_fail("oom for lm"); kfree(image); return 0; }
    for (int i = 0; i < (int)sizeof(*lm); i++) ((uint8_t *)lm)[i] = 0;

    int n = 0;
    while (display_name && display_name[n] && n < (int)sizeof(lm->name) - 1) {
        lm->name[n] = display_name[n]; n++;
    }
    lm->name[n] = 0;
    lm->image          = image;
    lm->image_size     = image_size;
    lm->init_fn        = init_fn;
    lm->exit_fn        = exit_fn;
    lm->license_gpl    = license_gpl;
    lm->kexports       = mod_kexports;
    lm->kexport_count  = mod_kexport_count;

    /* s54 Tier-1: fold-hash the source buffer for supply-chain evidence.
     * Not a cryptographic hash — purpose is to detect "the .kmd on disk
     * was swapped since last boot." Cheap (one pass over the bytes) and
     * deterministic. The 32-bit fold mixes every byte and is enough to
     * catch accidental substitution; against a targeted attacker who
     * controls the .kmd it's only useful when compared against a known
     * baseline captured in a previous boot log. A future hardened mode
     * will add real SHA + allow-list-by-hash. */
    uint32_t fold = 0x811C9DC5u;   /* FNV-1a seed */
    {
        const uint8_t *b = (const uint8_t *)buf;
        for (uint32_t i = 0; i < size; i++) {
            fold ^= b[i];
            fold *= 0x01000193u;
        }
    }

    serial_puts("MODULE: loaded ");
    serial_puts(lm->name);
    serial_puts(" (");
    serial_puts(license_gpl ? "GPL-family" : "non-GPL");
    serial_puts(") image=");
    serial_puthex((uint32_t)image);
    serial_puts(" size=");
    serial_puthex(image_size);
    serial_puts(" fnv=");
    serial_puthex(fold);
    if (mod_kexport_count) {
        serial_puts(" exports=");
        serial_puthex(mod_kexport_count);
    }
    serial_puts("\n");

    /* Run init */
    lm->init_returned = init_fn();
    if (lm->init_returned != 0) {
        serial_puts("MODULE: init returned ");
        serial_puthex((uint32_t)lm->init_returned);
        serial_puts(" — unloading\n");
        if (exit_fn) exit_fn();
        kfree(image); kfree(lm);
        g_last_load_init_failed = 1;
        return 0;
    }

    /* Link into list (head insertion). */
    lm->next = modules_head;
    modules_head = lm;
    return lm;
}

struct loaded_module *module_list_head(void) {
    return modules_head;
}
