# MOUNT-STRATEGY — Auto-mount & PCORE.CFG `[mount]` section

Status: **DESIGN — s59 close-out.** Implementation kicks off s60. Ties together the work done in s59 (ATAPI driver in `ata.c`, CHS-fallback path for 86Box ALi machines) with the next milestone (multi-drive FAT discovery + R/O ISO9660 mount).

Cross-refs:
- `docs/research/63-atapi-driver-derivation.md` §12 — ATAPI driver implementation map.
- `docs/research/64-iso9660-driver-derivation.md` §9 — ISO9660 driver outline.
- `src/include/fat.h` — current FAT API (3-letter cap, single-partition mount).
- `src/kernel/main.c:499-525` — current hardcoded mount sequence.

---

## 1. Problem statement

Right now pinecore mounts exactly two drives, both hardcoded:

```c
// src/kernel/main.c (s59 state)
fat_mount_ata(FAT_DRIVE_C, 0, 0);   // ATA drive 0, partition LBA 0 — always
fat_mount_fdc();                    // floppy → A:
```

`FAT_MAX_DRIVES = 3` (A: B: C: only). Second HDDs, second partitions, and CD-ROMs are all unreachable from the shell even though `ata.c` discovers them and `atapi_read` would let us read them. DOS auto-assigns A: B: to floppies, then C: D: E:… to HDD partitions in order, then a letter to each CD-ROM — pinecore needs the same behaviour to feel familiar and to make installer + dev workflows work.

## 2. What DOS actually does (the reference behaviour)

For context — this is the bar:

| Phase  | DOS step                                       | Pinecore equivalent                                  |
|--------|------------------------------------------------|------------------------------------------------------|
| 1      | IO.SYS detects floppies via BIOS INT 13h       | `fdc_init()` + `fdc_detect()`                        |
| 2      | Walks PARTITION TABLE on each fixed disk       | new `mount_walk_mbr(ata_id)`                         |
| 3      | Assigns A:, B: to floppies (always reserved)   | `fat_mount_fdc()` → A:                               |
| 4      | Assigns C:, D:, … to PRIMARY partitions, drive-by-drive | iterate ATA drives, assign letters from C: |
| 5      | LASTDRIVE= in CONFIG.SYS limits the letter range | PCORE.CFG `lastdrive = H` (default H, max Z)        |
| 6      | MSCDEX.EXE loads CD-ROM driver after IO.SYS    | `iso_mount()` runs after FAT mounts                  |

The big simplification we'll make for v1: skip **extended** partitions / logical drives. Almost no FreeDOS install media or test environment uses them — they're a 1990s consumer-DOS holdover. Primary partitions only.

## 3. The auto-mount sequence (v1 design)

```
ata_init()                            // already runs at boot
fdc_init()                            // already runs at boot
mount_init()                          // new — replaces main.c:499-525
  ├─ if fdc_detect():  fat_mount_fdc()  →  A:
  ├─ next_letter = 'C'
  ├─ for each ATA drive 0..N where !atapi:
  │    if mount_walk_mbr(ata_id, &next_letter) < 0:
  │        // No valid partition table — try mounting raw at LBA 0
  │        // (matches the s59 status quo for single-partition images).
  │        try fat_mount_ata(next_letter, ata_id, 0); next_letter++
  ├─ for each ATA drive 0..N where atapi:
  │    iso_mount(next_letter, ata_id); next_letter++    // doc 64
  └─ fat_set_drive(C if mounted else A)
```

### 3.1 `mount_walk_mbr(ata_id, &next_letter)`

```c
int mount_walk_mbr(uint8_t ata_id, char *next_letter) {
    uint8_t mbr[512];
    if (ata_read(ata_id, 0, 1, mbr) != 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;   // no MBR — caller falls back to LBA 0 mount

    int mounted = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *ent = mbr + 0x1BE + i * 16;
        uint8_t type = ent[4];
        if (!is_fat_partition_type(type)) continue;
        uint32_t lba = read_le32(ent + 8);
        if (*next_letter > 'A' + FAT_MAX_DRIVES - 1) break;  // out of letters
        if (fat_mount_ata((*next_letter - 'A'), ata_id, lba) == 0) {
            (*next_letter)++;
            mounted++;
        }
    }
    return mounted > 0 ? 0 : -1;
}

static int is_fat_partition_type(uint8_t t) {
    return t == 0x01 || t == 0x04 || t == 0x06 ||
           t == 0x0B || t == 0x0C || t == 0x0E;   // FAT12/16-small/16/32-LBA/32/16-LBA
}
```

CHS-fallback works through the new `ata_program_address` (s59) so this `ata_read` call functions on 86Box's ALi-PRO II machine just like on QEMU.

### 3.2 ATAPI / ISO mount

After the FAT pass, walk ATAPI drives and call `iso_mount()` for each. The ISO driver itself is doc-64 work; `mount_init` just calls into it. If `iso_mount()` returns failure (no media, not ISO9660, blank CD), log and skip — don't burn a letter on a failed mount.

### 3.3 R/O drives that aren't CD

Future: USB mass-storage (msc.kmd) and floppy with write-protect tab. Both look like ATA drives to `mount_init`. R/O-ness is a property of the volume layer, not the mount layer. Punt to `fat.c`'s open-for-write path returning `-EACCES`.

## 4. PCORE.CFG override block

Once `mount_init` works, add operator override via PCORE.CFG. Keys land in `config.c`'s `apply_kv` dispatch table:

```
# PCORE.CFG (excerpt)
mount.a = fdc                  # floppy
mount.c = ata0:1               # ATA drive 0, MBR partition 1
mount.d = ata0:2
mount.e = ata1:1
mount.f = atapi0               # ATAPI drive 0 (ISO9660)

# Override behaviour:
#   - any mount.X key marks that letter as MANUAL
#   - the auto-mount pass FILLS letters that aren't manually claimed
#   - this matches what DOS effectively does (auto-by-default, CONFIG.SYS overrides)
lastdrive = H                  # cap the auto-mount letter range; default 'H'
```

### 4.1 Why layered (auto + override) vs pure-override

DOS effectively does layered: `CONFIG.SYS` doesn't define every drive, just adds (network mounts via NDIS, CD via MSCDEX). Forcing `mount.*` to be all-or-nothing makes a missing `mount.c = ata0:1` line silently break boot — bad UX. Layered: missing lines fall back to auto.

If the user really wants to *block* auto-mount on a letter, they can use:
```
mount.f = none
```
which marks F: as claimed-but-empty.

### 4.2 Parser shape in `config.c`

Add to `apply_kv`:
```c
} else if (!strncmp(key, "mount.", 6)) {
    char letter = key[6];
    if (letter >= 'a' && letter <= 'z') letter -= 32;
    if (letter < 'A' || letter >= 'A' + FAT_MAX_DRIVES) return;  // out of range
    mount_override_set(letter, val);   // stores raw value string in a table
} else if (!strcmp(key, "lastdrive")) {
    if (val[0] >= 'A' && val[0] <= 'Z') g_lastdrive = val[0];
}
```

`mount_override_set` keeps an `(letter, backend, ata_id, partition_idx)` table that `mount_init` consults before doing auto-discovery on that letter.

Parser for the value (`ata0:1`, `atapi0`, `fdc`, `none`):

```
"fdc"         → backend=FDC
"none"        → backend=BLOCKED
"ataN:M"      → backend=ATA,   ata_id=N, partition_idx=M (1-based)
"atapiN"      → backend=ATAPI, ata_id=N
```

Anything else → `serial_puts("mount.X = invalid \"...\", ignoring\n")`.

## 5. Header / API changes

### 5.1 `src/include/fat.h`

```c
- #define FAT_MAX_DRIVES     3     /* A=0, B=1, C=2 */
+ #define FAT_MAX_DRIVES     8     /* A=0..H=7 */
```

Bump `volumes[FAT_MAX_DRIVES]` — already correctly sized in `fat.c`. `parse_drive_letter` already handles `A..A+FAT_MAX_DRIVES`; just gets more range.

### 5.2 `src/include/mount.h` (new)

```c
#ifndef MOUNT_H
#define MOUNT_H

#include "types.h"

/* Called once from main.c after ata_init + fdc_init + config_init.
 * Walks the MBR of each ATA drive, mounts each FAT partition into the
 * next-free letter starting at C:, then runs iso_mount on each ATAPI
 * drive. Honors PCORE.CFG mount.* overrides if present.
 *
 * Logs each mount/skip decision to COM1 (visible in 86Box serial.log
 * + QEMU stdio); writes a summary table to VGA via print_ok. */
void mount_init(void);

/* Called by config.c when it sees a `mount.X = …` key. Stores the raw
 * value for mount_init to consume. */
void mount_override_set(char letter, const char *value);

#endif
```

### 5.3 `src/kernel/main.c`

Replace lines 499–525 with a single `mount_init()` call. Ordering: `config_init()` must run BEFORE `mount_init()` so overrides are in place. That means moving the existing `config_init()` call up before mount. Today: `ata_init → fdc_init → mounts → config_init`. New: `ata_init → fdc_init → config_init → mount_init`.

That re-ordering is the only behavioural risk of this design — `config_init` currently reads C: via `fat_open(CONFIG_PATH)`, which means it depends on C: being mounted. Need to either:
- (a) defer config_init's actual file read to a second pass after mount_init, OR
- (b) have config_init still do its own one-shot mount of C: to read PCORE.CFG, then unmount, then mount_init does the real thing.

**Recommendation: (a).** Split `config_init()` into `config_init_defaults()` (called early — populates defaults including empty mount-override table) and `config_load()` (called by mount_init after the boot drive is mounted — actually reads PCORE.CFG and applies overrides to live state). For `mount.*` overrides specifically, mount_init has to call `config_load` BEFORE the auto-mount pass, which is a bootstrap loop: we need C: to read overrides for C:. Resolved by: mount the kernel's boot drive first (ata0:1 as C:, hardcoded), then read C:\PCORE.CFG, then unmount C: + redo with overrides if `mount.c` says otherwise. Cheap because `fat_mount_ata` is idempotent on the same `(drive, ata_id, lba)`.

## 6. Diagnostics

Every mount decision goes to COM1 in one structured line so 86Box's serial.log and QEMU's stdio capture the full enumeration:

```
mount: scan ata0 — MBR signature OK, 1 FAT partition
mount:   C: ← ata0 p1 (FAT16, 64 MB)
mount: scan ata1 — no MBR signature, falling back to LBA 0
mount:   D: ← ata1 p0 (FAT12, 1.4 MB)
mount: scan atapi0 — ISO9660 PVD found, label "PINECORE-INST"
mount:   E: ← atapi0 (ISO9660, 312 MB)
mount: A: ← fdc (FAT12, 1.4 MB)
mount: default = C:
```

VGA gets a one-line summary per mount via the existing `print_ok` ladder so first-boot users see what mounted without reading the serial log.

## 7. Test plan

1. QEMU `-hda pinecore-hdd.img -cdrom test.iso` → expect C: (FAT) + D: (ISO).
2. QEMU `-hda multi-part.img` (MBR with 2 FAT16 partitions) → expect C: + D:.
3. 86Box ALi-PRO II machine + 1.4 MB floppy + 32 MB IDE HDD → expect A: + C: (via CHS-fallback path).
4. QEMU `-hda hdd.img` with PCORE.CFG containing `mount.d = atapi0` → D: should map to the CD even though auto-order would have given the CD to D: anyway (validates override path runs).
5. QEMU with no media → boot completes, shell prompt at A:> (or no drive prompt) without panic.

## 8. Milestones

- **M1 — Multi-letter FAT auto-mount** (no ISO, no overrides yet). Bump FAT_MAX_DRIVES, add `mount.c` + `mount.h`, switch main.c. Validates the MBR-walk + CHS-fallback together. Estimated ~150 LOC.
- **M2 — PCORE.CFG `mount.*` overrides + `lastdrive`**. Adds the override table + parser glue in config.c. ~80 LOC.
- **M3 — ISO9660 driver (doc 64 implementation) + R/O mount.** New `iso9660.c` + `iso9660.h`; `mount_init` calls `iso_mount` for ATAPI drives; the existing FAT-API call sites in shell.c need a vfs dispatch (doc 64 §9.1 option A). ~500 LOC ISO + ~80 LOC vfs.
- **M4 — El Torito + bootable CD path** (doc 65, future). Out of scope for this design.

M1 + M2 are mostly mechanical and can land in a single session. M3 is a full session of its own (driver + vfs refactor). Punt M4 to whenever we want pinecore to boot off CD.

---

*Author: s59 close-out.  Last updated: 2026-06-16.*
