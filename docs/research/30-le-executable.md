# LE (Linear Executable) Format — DOS/4GW Game Loader

> The binary format used by DOOM, Duke Nukem 3D, and most DOS/4GW games. Contains 32-bit protected mode code in a container with an MZ real-mode stub.

**Date:** 2026-05-02
**Status:** Complete — byte-level loading reference for implementation
**Sources:** IBM OS/2 Linear Executable Module Format (1991); DOS/4GW documentation; Watcom C/C++ Linker Reference

---

## File Layout

```
+---------------------+  offset 0
| MZ DOS stub         |  (DOS/4GW loader or small stub)
| MZ[0x3C] = offset   |  → points to LE header
+---------------------+
| LE Header (176 B)   |  signature 'LE' (0x454C)
+---------------------+
| Object Table        |  24 bytes per object (code, data, BSS)
+---------------------+
| Object Page Table   |  8 bytes per page, maps to file data
+---------------------+
| Fixup Page Table    |  DWORD per page + sentinel, indexes fixup records
+---------------------+
| Fixup Record Table  |  variable-length relocation records
+---------------------+
| Page Data           |  actual code/data pages
+---------------------+
```

## LE Header (176 bytes)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0x00 | 2 | Signature | 'LE' = 0x454C |
| 0x08 | 2 | CPU type | 2=386 |
| 0x0A | 2 | OS type | 3=DOS/4GW |
| 0x14 | 4 | num_pages | Total memory pages |
| 0x18 | 4 | eip_object | Object # for entry point (1-based) |
| 0x1C | 4 | eip_offset | EIP offset within object |
| 0x20 | 4 | esp_object | Object # for stack (1-based) |
| 0x24 | 4 | esp_offset | ESP offset within object |
| 0x28 | 4 | page_size | Usually 4096 |
| 0x40 | 4 | object_table_off | From LE header start |
| 0x44 | 4 | num_objects | Typically 2-4 |
| 0x48 | 4 | page_table_off | Object page table offset |
| 0x68 | 4 | fixup_page_off | Fixup page table offset |
| 0x6C | 4 | fixup_rec_off | Fixup record table offset |
| 0x80 | 4 | data_pages_off | **Absolute** file offset of page data |

## Object Table Entry (24 bytes each)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0x00 | 4 | virtual_size | Size in bytes |
| 0x04 | 4 | reloc_base | Preferred load address |
| 0x08 | 4 | flags | See below |
| 0x0C | 4 | page_table_index | 1-based index into page table |
| 0x10 | 4 | num_pages | Pages in this object |

**Object flags:** Bit 0=read, 1=write, 2=exec, 13=USE32 (32-bit default)

**Typical DOOM layout:**
- Object 1: Code (flags 0x2005 — readable, executable, USE32)
- Object 2: Data (flags 0x2003 — readable, writable, USE32)
- Object 3: BSS (flags 0x2003 — zero-filled)

## Object Page Table Entry (8 bytes, LE format)

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 4 | page_data_offset (1-based page number) |
| 0x04 | 2 | data_size (0 = full page) |
| 0x06 | 2 | flags (0=normal, 3=zero-fill) |

File offset of page data: `data_pages_off + (page_data_offset - 1) * page_size`

## Fixup Records (Internal Reference — most common)

```
Byte 0: source_type (bits 0-3: 0x07=32-bit offset, 0x08=32-bit self-relative)
Byte 1: target_flags (bits 0-1: 0x00=internal)
Byte 2-3: source_offset within page
Byte 4: target_object (1-based)
Byte 5-8: target_offset (16 or 32 bit, per flag bit 3)
```

**Patching a 32-bit offset fixup (type 0x07):**
```
*(uint32_t *)(page_base + source_offset) = object_base[target_obj-1] + target_offset
```

**Patching a self-relative fixup (type 0x08):**
```
addr = page_base + source_offset
*(uint32_t *)addr = target_addr - (addr + 4)
```

## Loading Algorithm for Pinecore

Since we ARE the kernel (not a DOS extender), we skip the MZ stub entirely:

```
1. Open the .EXE file, read MZ header
2. Read DWORD at MZ offset 0x3C → LE header file offset
3. Read LE header, verify signature 0x454C
4. For each object:
   a. Allocate memory (physical pages from PMM, above 4MB)
   b. Zero-fill entire allocation
   c. Load each page from file into object memory
5. Apply all fixup records (almost all type 0x07 for DOS games)
6. Create Ring 3 flat selectors: CS(base=0, limit=4GB), DS=SS=same
7. Set EIP = object_base[eip_object-1] + eip_offset
8. Set ESP = object_base[esp_object-1] + esp_offset
9. IRET to Ring 3 at the entry point
```

## Key Insight for Pinecore

DOS/4GW games expect a flat memory model (base=0, limit=4GB). Our kernel already has flat segments for Ring 0. We just need Ring 3 equivalents:
- CS = GDT code selector, DPL=3, base=0, limit=4GB
- DS = SS = ES = GDT data selector, DPL=3, base=0, limit=4GB

We already have these in our GDT (entries 0x18 and 0x20 — user code/data). So we can load LE objects at any linear address above 4MB and jump directly — the fixups will resolve to correct addresses.

No LDT needed for flat-model games. No DOS/4GW needed. We are the extender.

---

*Primary sources: IBM OS/2 Linear Executable Module Format specification (1991); Watcom C/C++ Linker Reference; DOS/4GW Professional documentation (Tenberry Software)*
