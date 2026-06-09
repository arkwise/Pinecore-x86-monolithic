/* ELF32 type definitions sufficient for parsing i386 relocatable
 * object files (ET_REL). This is what `i686-elf-gcc -c` produces and
 * what our .kmd kernel modules are. Reference: System V ABI / Intel
 * 386 supplement, plus Linux include/uapi/linux/elf.h.
 *
 * License: GPL-2.0 (pinecore-x86 kernel).
 */
#ifndef PINECORE_ELF_H
#define PINECORE_ELF_H

#include "types.h"

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

#define EI_NIDENT 16

/* ELF header e_ident magic */
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define EV_CURRENT  1

/* e_type */
#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3

/* e_machine */
#define EM_386 3

struct elf32_ehdr {
    uint8_t     e_ident[EI_NIDENT];
    Elf32_Half  e_type;
    Elf32_Half  e_machine;
    Elf32_Word  e_version;
    Elf32_Addr  e_entry;
    Elf32_Off   e_phoff;
    Elf32_Off   e_shoff;
    Elf32_Word  e_flags;
    Elf32_Half  e_ehsize;
    Elf32_Half  e_phentsize;
    Elf32_Half  e_phnum;
    Elf32_Half  e_shentsize;
    Elf32_Half  e_shnum;
    Elf32_Half  e_shstrndx;
};

/* Section header types */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHT_REL      9

/* Section header flags */
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

struct elf32_shdr {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
};

/* Symbol table entry */
#define STN_UNDEF 0

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4

#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xF)

struct elf32_sym {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    uint8_t    st_info;
    uint8_t    st_other;
    Elf32_Half st_shndx;
};

/* Relocation entry (REL, no addend — addend embedded in target) */
#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xFF)

#define R_386_NONE  0
#define R_386_32    1   /* S + A — absolute 32-bit */
#define R_386_PC32  2   /* S + A - P — PC-relative 32-bit (used by CALL/JMP) */
#define R_386_PLT32 4   /* identical to PC32 for our purposes (no PLT) */

struct elf32_rel {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
};

#endif
