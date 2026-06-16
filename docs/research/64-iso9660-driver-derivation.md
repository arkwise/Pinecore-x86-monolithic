# 64 — ISO 9660 / ECMA-119 — driver derivation

Status: **Pass 1 skeleton — sections written, citation backfill pending, no code yet.** Companion to doc 63 (ATAPI). Runs on top of `atapi_read` (already in `src/kernel/ata.c` as of s59). The goal: a read-only filesystem layer that lets `fat_mount`-shaped code mount a data CD as a drive letter (D:/E:/…), exposing files through the same `fat_open`/`fat_read`/`fat_find_first`/`fat_find_next` API surface that FAT volumes use.

Companion docs:
- `63-atapi-driver-derivation.md` — block transport (`atapi_read` returns 2 KB sectors).
- `65-el-torito-boot-record.md` — bootable-CD format; uses the same `atapi_read`. Out-of-scope for the read driver but the boot record at LBA 17 (0x11) lives in the same volume-descriptor table this doc maps.
- `refs/cdrom-stack/ecma-119-iso9660.pdf` — primary source (ECMA International, 4th Edition, June 2019, ~95 pp.).
- `refs/cdrom-stack/el-torito-1.0.pdf` — secondary reference for the boot-record VD type.

Citation format: `(ECMA-119 §x.y, p.NN)` — page numbers from the local 4th-edition PDF; section numbers are stable across editions.

Reference reads (sanity-check, never source):
- Linux `fs/isofs/inode.c` — primary VD parse + ROCK/Joliet detection.
- FreeBSD `sys/fs/cd9660/cd9660_vfsops.c` — same shape, different naming.
- mkisofs / genisoimage source — most useful for confirming on-disk byte layout when reading our own test images.

---

## 1. Architecture in one paragraph

ISO 9660 is a fixed-format read-only directory tree starting at LBA 16. Every metadata structure stores numbers in **both** little-endian and big-endian forms back-to-back ("both-byte order") so any host reads its native side. Every disk block is exactly 2048 bytes (= one ATAPI sector — that's not a coincidence; ISO 9660 was designed around CD-ROM's logical block size). Files are stored as contiguous extents (no fragmentation), so a directory entry's `extent_lba` + `data_length` is enough to read a file with a single `atapi_read(lba, count_in_2K_blocks, buf)` per fetch. There are no FAT-style allocation tables and no cluster chains.

Three extensions sit on top of the base ISO 9660 metadata, all advertised through additional Volume Descriptors found by scanning LBA 16…:

- **Joliet** (Microsoft, 1995) — UCS-2 (UTF-16BE) long names up to 64 chars. Lives in a Supplementary VD with escape sequence `%/E` or `%/@` etc. (ECMA-119 Annex C).
- **Rock Ridge** (POSIX overlay) — SUSP system-use area in each directory entry, carries POSIX names + mode bits + symlinks. Mainly Unix CDs; almost never on FreeDOS install media.
- **El Torito** (boot record VD at LBA 17) — points at a boot catalog, used by the BIOS not the kernel.

Our v1 driver reads the **base ISO 9660 short-name path only** — `FILENAME.EXT;1` (semicolon-version always 1 on data CDs, stripped at read time). Joliet adds two lines of code (re-parse if the SVD is present, prefer its root over the PVD's), so we'll do that in v1.1. Rock Ridge is deferred — pinecore is a DOS-style environment, the 8.3 ceiling already matches the user's expectations.

```
              LBA  0  ── boot sector / system area (32 KB, ignored by ISO 9660)
              LBA 16  ── Primary Volume Descriptor      (type 1)
              LBA 17  ── Boot Record VD (optional)      (type 0)
              LBA 18  ── Supplementary VD (Joliet)      (type 2, if present)
              ...
              LBA n   ── Volume Descriptor Set Terminator (type 255)
              ...
              ────────  ROOT DIRECTORY EXTENT  ────
              ────────  PATH TABLE             ────
              ────────  FILE DATA              ────
```

---

## 2. The byte primitives ISO 9660 uses

Three numeric encodings recur (ECMA-119 §7):

- **`int8`** — unsigned byte. Used for filename lengths.
- **`int16_LSB_MSB`** / **`int32_LSB_MSB`** — "both-byte order". 4 or 8 bytes total: LE form, then BE form, of the same value. We read the LE half and discard the BE half.
- **`d-characters`** — restricted ASCII: `A`–`Z`, `0`–`9`, `_`. Used in volume identifiers.
- **`a-characters`** — broader ASCII set including `!"%&'()*+,-./:;<=>?`. Used in publisher/preparer/applic fields.

Date-time fields come in two flavours (ECMA-119 §8.4.26):
- **17-byte ASCII** (`dec_datetime`) — `YYYYMMDDHHMMSSCC` + GMT offset byte. Used in volume descriptors.
- **7-byte binary** (`dir_datetime`) — years-since-1900, month, day, hour, minute, second, GMT-offset. Used in directory entries.

Strings are space-padded, NOT NUL-terminated, in the metadata. Lengths come from the spec, not from a terminator.

---

## 3. Volume descriptor scan

Read sectors starting at **LBA 16**, one 2048-byte sector at a time. Each sector is a Volume Descriptor (ECMA-119 §8.1):

| Byte    | Field                | Notes                                        |
|---------|----------------------|----------------------------------------------|
| 0       | Type                 | 0=Boot, 1=Primary, 2=Supplementary (Joliet candidate), 3=Volume Partition, 255=Terminator |
| 1..5    | Standard Identifier  | always `"CD001"` for ISO 9660                |
| 6       | Version              | always `0x01`                                |
| 7..2047 | Type-specific data   | see §4 for PVD, §5 for SVD/Joliet            |

Scan loop: read one sector → check StdId == `"CD001"` → switch on type → stop on type 255 or after 32 sectors. (Real CDs typically have 2–4 VDs.)

A bare data CD with no Joliet/Boot extensions has exactly two: PVD at 16, Terminator at 17.

---

## 4. Primary Volume Descriptor — what we need

PVD bytes (ECMA-119 §8.4):

| Offset | Size | Field                                         | Notes                          |
|--------|------|-----------------------------------------------|--------------------------------|
| 0      | 1    | Type = 1                                      | —                              |
| 1      | 5    | StdId = `"CD001"`                             | —                              |
| 6      | 1    | Version = 1                                   | —                              |
| 8      | 32   | System Identifier                             | space-padded ASCII             |
| 40     | 32   | **Volume Identifier**                         | the disc label (`PINECORE_INSTALL`); space-padded |
| 80     | 8    | Volume Space Size (both-byte order)           | total LBAs on the volume       |
| 120    | 4    | Volume Set Size (BBO)                         | usually 1                      |
| 124    | 4    | Volume Sequence Number (BBO)                  | usually 1                      |
| 128    | 4    | **Logical Block Size (BBO)**                  | always 2048 on CD; sanity-check |
| 132    | 8    | **Path Table Size (BBO)**                     | bytes in the path table        |
| 140    | 4    | Loc of L-path Table (LE)                      | LBA of LE-form path table      |
| 156    | 34   | **Root Directory Record** (one fixed-size dir-entry) | inline, NOT a pointer — parse directly |
| 318    | 128  | Volume Set ID                                 | strA                           |
| 446    | 128  | Publisher ID                                  | strA                           |
| 574    | 128  | Data Preparer ID                              | strA                           |
| 702    | 128  | Application ID                                | strA                           |
| 813    | 17   | Volume Creation Date                          | dec_datetime                   |

The driver stashes (root_lba, root_size, vol_label, block_size) and is done with the PVD.

---

## 5. Supplementary Volume Descriptor (Joliet)

Same byte layout as PVD except:
- byte 0 type = 2,
- byte 88..120 holds an **Escape Sequence** field (`%/@`, `%/C`, or `%/E` → UCS-2 levels 1/2/3).

If an SVD with a Joliet escape sequence is present, the driver:
1. Replaces the root extent pointer with the SVD's root directory record (offset 156 inside the SVD sector).
2. Decodes filenames as UCS-2 big-endian, 2 bytes per char; for the v1.1 driver we strip to lower 8 bits (Latin-1) for display.
3. Treats all directory traversal from this point as Joliet.

If no Joliet SVD: just use the PVD root. That's the "base ISO 9660 short-name" mode.

---

## 6. Directory record layout

Each directory entry is variable-length (ECMA-119 §9.1):

| Offset | Size | Field                            | Notes                                   |
|--------|------|----------------------------------|-----------------------------------------|
| 0      | 1    | **Length of Directory Record**   | 0 = end of records in this sector; pad to next sector |
| 1      | 1    | Extended Attr Record Length      | usually 0                               |
| 2      | 8    | **Location of Extent (BBO)**     | LBA of file data                        |
| 10     | 8    | **Data Length (BBO)**            | size in bytes                           |
| 18     | 7    | Recording Date (dir_datetime)    | —                                       |
| 25     | 1    | **File Flags**                   | bit 0=hidden, bit 1=**directory**, bit 7=multi-extent |
| 26     | 1    | File Unit Size (interleaved)     | 0 on standard CDs                       |
| 27     | 1    | Interleave Gap Size              | 0                                       |
| 28     | 4    | Volume Sequence Number (BBO)     | 1                                       |
| 32     | 1    | **Length of File Identifier**    | filename length in bytes                |
| 33     | N    | **File Identifier**              | special: `\0` = `.`, `\1` = `..`        |
| 33+N   | 0/1  | Padding (only if N is even)      | keeps even alignment                    |
| ...    | M    | **System Use Area**              | empty in base ISO, holds SUSP/RR fields when present |

File identifier conventions:
- Single byte `0x00` = `.` (current directory entry — first entry in every directory).
- Single byte `0x01` = `..` (parent — second entry).
- Otherwise: `NAME.EXT;1` for files (semicolon-version always 1 on data CDs), `NAME` for subdirectories. Strip the `;version` suffix at read time.

Directory entries can span sector boundaries only when an entry's length is followed by zero-padding until the next sector boundary — a zero-length byte terminates the sector's entry list. Sector-boundary walking is a per-byte loop, not a per-entry one.

---

## 7. Path table (the shortcut directory index)

ISO 9660 stores a flat list of all directories with their parent index (ECMA-119 §9.4). For a v1 read driver we **don't need it** — directory-tree walks are linear from the root directory extent. The path table matters for boot-time disc structure indexing where CD-ROM seeks are expensive; on emulated/modern drives the seek is free. We skip the path table entirely.

Mention here for completeness: PVD bytes 140–144 (`L-Path Table location`) is where it lives. Skipping the path table is what every Linux/BSD ISO9660 driver also does for the read path.

---

## 8. Reading a file end-to-end

```
1. caller: cd_open("D:\\INSTALL\\KERNEL.BIN", O_RDONLY)
2. driver: walk the path "INSTALL" then "KERNEL.BIN"
   a. start at root extent (LBA from PVD)
   b. for each path component:
      - atapi_read(extent_lba, ceil(extent_size / 2048), buf)
      - linear-scan directory records (§6)
      - case-fold compare against component name (ISO 9660 uppercases; DOS apps usually pass uppercase already)
      - on match: if file flag bit 1 set, descend; else this is the file
3. once found: store extent_lba + data_length in a file handle
4. cd_read(handle, n_bytes):
   - compute (offset_lba, byte_in_sector) from handle->position
   - atapi_read(extent_lba + offset_lba, 1 sector at a time, temp buf)
   - memcpy from temp buf + byte_in_sector into caller's buf
   - advance handle->position; loop until n_bytes done or EOF
5. cd_close: clear handle.active
```

No FAT chain walk. No cluster math. Extents are contiguous LBAs, so `lba_in_file = extent_lba + (position / 2048)`. The driver is mostly a directory-entry parser + a wrapper around `atapi_read`.

---

## 9. Driver derivation mapping → kernel files

Proposed new files (none yet exist):

| New file          | Purpose                                                              |
|-------------------|----------------------------------------------------------------------|
| `src/include/iso9660.h` | Public API (mount, open, read, find_first/next, close).        |
| `src/kernel/iso9660.c` | The driver. ~500 LOC estimated based on Linux isofs (~3 KLOC, but most of that is ROCK + Joliet — base is ~600 LOC). |

Function-level outline for `iso9660.c`:

```
int  iso_mount(int drive_letter, uint8_t ata_id);
        - atapi_read sectors 16..N until type 255
        - parse PVD (§4), stash root_lba/root_size/block_size
        - if Joliet SVD seen, prefer it (§5)
        - mark volumes[drive_letter] = ISO type
int  iso_open(int drive, const char *path, struct iso_file *out);
        - walk components (§8)
        - extent_lba + data_length → out
int  iso_read(struct iso_file *h, void *buf, uint32_t n);
        - per-sector atapi_read into a static bounce buffer
        - memcpy into caller, advance position
int  iso_find_first(int drive, const char *dir_path, struct iso_find *out);
int  iso_find_next(struct iso_find *out);
```

### 9.1 Coexistence with `fat.c`

Two options:

**A. Separate API, dispatched by mount type.** Add a `vfs.c` layer above `fat.c` and `iso9660.c`; `vfs_open(path)` looks at the drive letter's type and routes to either FAT or ISO. Cleaner; ~80 LOC of glue; requires touching every existing `fat_open` call site.

**B. Extend `fat.c`'s drive table.** Add a `fs_type` field to `volumes[]`; existing `fat_*` entry points dispatch internally based on type. Smaller diff to existing callers; muddier separation of concerns; bloats `fat.c`.

**Recommendation: A.** The vfs layer is ~80 LOC of indirection now; trying to retro-fit it after option B's name-collisions and union-typed handles are baked in costs more than the LOC saved. Pinecore is still small enough that the refactor is mechanical.

### 9.2 Bounce buffer

`atapi_read` follows the s53.usb.b HCD bounce-buffer contract (memory `project_hcd_bounce_buffer_contract`). For ISO9660 we allocate one 2 KB static buffer in `iso9660.c`'s `.bss`, which lives in the kernel's identity-mapped low region — safe for IDE PIO since the ATA driver does port-IO (not DMA) and there's no addressing constraint.

Future migration path if we ever switch ATA to UDMA: change the static buffer to a `dma_alloc(2048, 16)` allocation in `iso_mount`. The rest of the driver stays.

---

## 10. What this doc deliberately doesn't cover

- **Rock Ridge / SUSP**: deferred. Pinecore is DOS-shaped; 8.3 names are the expected user experience. Rock Ridge adds ~300 LOC of System Use Sharing Protocol parsing for almost no user-visible win.
- **Multi-session discs**: a multi-session CD has PVDs at multiple LBAs; we'd add a scan for the last session via READ TOC (MMC-2 §6.1.20). Out-of-scope for v1 — installer CDs are single-session.
- **CD-RW / packet-written discs (UDF)**: completely different filesystem. Not covered by ECMA-119. If we ever need to read a packet-written CD, that's a doc-66 + new `udf.c`.
- **Hybrid HFS/ISO discs** (old Mac install media): same data as ISO but in a parallel hierarchy. We read the ISO half; HFS is ignored.
- **Writing**: read-only. Burning support is a different stack entirely.

---

## 11. Citation backlog

Section numbers in §3–§9 are sourced from ECMA-119 4th edition's table of contents and from cross-reading public driver code. Page-number backfill (`(p.?)`) is a Pass-2 task — read the local PDF section-by-section and stamp the printed page number. The PDF is in `refs/cdrom-stack/ecma-119-iso9660.pdf` (~95 pp., short doc, ~30 min job).

---

*Last updated: 2026-06-16 (s59 close — Pass 1 skeleton written; Pass 2 backfill + implementation pending)*
