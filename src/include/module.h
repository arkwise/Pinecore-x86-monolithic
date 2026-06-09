/* Pinecore kernel module ABI.
 *
 * .kmd modules are ELF32 relocatable i386 object files produced by
 *   i686-elf-gcc -c -fno-common -ffreestanding -Iinclude module.c -o foo.kmd
 *
 * Each module declares a license + an init/exit pair using the macros
 * below. The kernel iterates a known section in the loaded module to
 * find these entry points.
 *
 * Kernel exports live in the .kexport section (linker-collected at
 * build time) — modules look up unresolved external references against
 * that table during relocation.
 *
 * Boundary: kernel = GPL-2.0; module ABI shim (this header + the few
 * kernel symbols exported with EXPORT_SYMBOL) = LGPL-2.1 so closed
 * drivers can link. Symbols exported with EXPORT_SYMBOL_GPL are only
 * resolvable by modules whose MODULE_LICENSE() declares a GPL-family
 * license.
 */
#ifndef PINECORE_MODULE_H
#define PINECORE_MODULE_H

#include "types.h"

/* ------------------------------------------------------------------
 * Macros for .kmd module authors
 * ------------------------------------------------------------------ */

/* Each module declares one init and (optionally) one exit. The kernel
 * looks for symbols named __pinecore_init / __pinecore_exit. */
#define module_init(fn) \
    int __pinecore_init(void) __attribute__((alias(#fn)))

#define module_exit(fn) \
    void __pinecore_exit(void) __attribute__((alias(#fn)))

/* Plain string globals the loader reads to check license/name. */
#define MODULE_LICENSE(s) \
    const char __pinecore_license[] = (s)
#define MODULE_AUTHOR(s) \
    const char __pinecore_author[] = (s)
#define MODULE_DESCRIPTION(s) \
    const char __pinecore_description[] = (s)
#define MODULE_NAME(s) \
    const char __pinecore_name[] = (s)

/* ------------------------------------------------------------------
 * Macros for kernel side — declare a symbol exportable to modules.
 * Lives in .kexport section, walked at boot to populate the symbol
 * lookup table.
 * ------------------------------------------------------------------ */
struct kexport {
    const char *name;
    void       *addr;
    uint32_t    gpl_only;     /* 1 if EXPORT_SYMBOL_GPL */
};

#define EXPORT_SYMBOL(sym) \
    __attribute__((used, section(".kexport"))) \
    static const struct kexport __kexport_##sym \
        = { #sym, (void *)&(sym), 0 }

#define EXPORT_SYMBOL_GPL(sym) \
    __attribute__((used, section(".kexport"))) \
    static const struct kexport __kexport_##sym \
        = { #sym, (void *)&(sym), 1 }

/* ------------------------------------------------------------------
 * Kernel-side loader API
 * ------------------------------------------------------------------ */

struct loaded_module {
    char           name[32];
    void          *image;          /* allocated block holding .text/.data/.bss */
    uint32_t       image_size;
    int          (*init_fn)(void);
    void         (*exit_fn)(void);
    int            init_returned;  /* what init_fn returned */
    int            license_gpl;    /* 1 if license is GPL-compatible */
    /* Symbols this module exports to other modules. Points into the
     * module's image at the relocated `.kexport` section; count is the
     * number of `struct kexport` entries there. Zero if the module
     * exports nothing (no EXPORT_SYMBOL macros in its source). */
    const struct kexport *kexports;
    uint32_t       kexport_count;
    struct loaded_module *next;
};

/* Walk the .kexport section and build the runtime lookup table. Call
 * once at boot, before any module load. */
void module_init_subsystem(void);

/* Resolve an exported kernel symbol by name. Returns NULL if missing
 * or if `for_gpl_module` is 0 and the export is GPL-only. */
void *module_resolve(const char *name, int for_gpl_module);

/* Load a .kmd image from a memory buffer. Performs full ELF parse,
 * memory allocation, copy, symbol resolution, relocation, then runs
 * module_init. Returns NULL on any failure (with a diagnostic on
 * serial). On success, the module is linked into the loaded-modules
 * list (queryable via module_list_head). */
struct loaded_module *module_load_image(const char *display_name,
                                        const void *buf, uint32_t size);

/* For debugging / `lsmod`-style listing. */
struct loaded_module *module_list_head(void);

#endif
