# FAT Filesystem Driver — Reading and Writing FAT12/16/32

> We need our own FAT driver because DOS is not reentrant. FAT is well-documented and straightforward.

**Date:** 2026-04-28
**Status:** Complete — structural reference for implementation

---

## Findings

### Boot Sector / BPB (BIOS Parameter Block)

Every FAT volume starts with a boot sector containing the BPB. Key fields:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 3 | Jump | JMP instruction + NOP |
| 0x03 | 8 | OEM ID | e.g., "MSDOS5.0" |
| 0x0B | 2 | BytsPerSec | Bytes per sector (usually 512) |
| 0x0D | 1 | SecPerClus | Sectors per cluster (1, 2, 4, 8, 16, 32, 64) |
| 0x0E | 2 | RsvdSecCnt | Reserved sectors before first FAT (FAT16=1, FAT32=32 typical) |
| 0x10 | 1 | NumFATs | Number of FATs (usually 2) |
| 0x11 | 2 | RootEntCnt | Root directory entries (FAT12/16 only, 0 for FAT32) |
| 0x13 | 2 | TotSec16 | Total sectors (16-bit, 0 if using 32-bit field) |
| 0x15 | 1 | Media | Media descriptor (0xF8 = hard disk, 0xF0 = floppy) |
| 0x16 | 2 | FATSz16 | FAT size in sectors (FAT12/16 only, 0 for FAT32) |
| 0x18 | 2 | SecPerTrk | Sectors per track |
| 0x1A | 2 | NumHeads | Number of heads |
| 0x1C | 4 | HiddSec | Hidden sectors before partition |
| 0x20 | 4 | TotSec32 | Total sectors (32-bit) |

**FAT32-specific fields (offset 0x24+):**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x24 | 4 | FATSz32 | FAT size in sectors (FAT32) |
| 0x2C | 4 | RootClus | Root directory first cluster (usually 2) |
| 0x30 | 2 | FSInfo | FSInfo sector number |
| 0x32 | 2 | BkBootSec | Backup boot sector |

**Boot sector signature:** 0x55AA at offset 0x1FE

### FAT Type Determination

The type is determined by CLUSTER COUNT, not by a label string:

```c
root_dir_sectors = ((RootEntCnt * 32) + (BytsPerSec - 1)) / BytsPerSec;
fat_size = FATSz16 ? FATSz16 : FATSz32;
total_sectors = TotSec16 ? TotSec16 : TotSec32;
data_sectors = total_sectors - RsvdSecCnt - (NumFATs * fat_size) - root_dir_sectors;
cluster_count = data_sectors / SecPerClus;

if (cluster_count < 4085)       → FAT12
else if (cluster_count < 65525) → FAT16
else                            → FAT32
```

Source: Microsoft FAT specification, confirmed in DJGPP libc getfatsz.c

### Layout on Disk

```
[ Boot Sector | Reserved ] [ FAT 1 ] [ FAT 2 ] [ Root Dir (FAT12/16) ] [ Data Region ]
  RsvdSecCnt sectors        FATSz     FATSz      RootEntCnt*32/512       Clusters 2..N
```

Key calculations:
```c
first_fat_sector = RsvdSecCnt;
root_dir_sector = RsvdSecCnt + (NumFATs * fat_size);  // FAT12/16 only
first_data_sector = root_dir_sector + root_dir_sectors; // FAT12/16
first_data_sector = RsvdSecCnt + (NumFATs * fat_size);  // FAT32 (root is in data)
```

### FAT Entry Format

Each FAT entry holds the next cluster number in a file's chain:

**FAT12 (12-bit entries, packed):**
```c
// For cluster N:
offset = N + (N / 2);  // 1.5 bytes per entry
entry = *(uint16_t*)(fat_buffer + offset);
if (N & 1)
    entry >>= 4;       // Odd cluster: high 12 bits
else
    entry &= 0x0FFF;   // Even cluster: low 12 bits

// Special values:
// 0x000 = free cluster
// 0xFF8-0xFFF = end of chain
// 0xFF7 = bad cluster
```

**FAT16 (16-bit entries):**
```c
entry = ((uint16_t*)fat_buffer)[N];
// 0x0000 = free, 0xFFF8-0xFFFF = end of chain, 0xFFF7 = bad
```

**FAT32 (32-bit entries, only lower 28 bits used):**
```c
entry = ((uint32_t*)fat_buffer)[N] & 0x0FFFFFFF;
// 0x00000000 = free, 0x0FFFFFF8-0x0FFFFFFF = end of chain, 0x0FFFFFF7 = bad
```

### Directory Entry Format (32 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 8 | Name | Filename (space-padded, first byte special: 0x00=end, 0xE5=deleted) |
| 0x08 | 3 | Ext | Extension (space-padded) |
| 0x0B | 1 | Attr | Attributes: 0x01=RO, 0x02=Hidden, 0x04=System, 0x08=VolLabel, 0x10=Dir, 0x20=Archive |
| 0x0C | 1 | NTRes | Reserved (Windows NT case flags) |
| 0x0D | 1 | CrtTimeTenth | Creation time tenths of second |
| 0x0E | 2 | CrtTime | Creation time (hour:5, min:6, sec/2:5) |
| 0x10 | 2 | CrtDate | Creation date (year-1980:7, month:4, day:5) |
| 0x12 | 2 | LstAccDate | Last access date |
| 0x14 | 2 | FstClusHI | First cluster high 16 bits (FAT32 only, 0 for FAT12/16) |
| 0x16 | 2 | WrtTime | Last write time |
| 0x18 | 2 | WrtDate | Last write date |
| 0x1A | 2 | FstClusLO | First cluster low 16 bits |
| 0x1C | 4 | FileSize | File size in bytes (0 for directories) |

**Long Filename (LFN) entries:** Attribute byte = 0x0F, contains 13 UTF-16 chars across multiple 32-byte entries preceding the 8.3 entry.

### Cluster to Sector Calculation

```c
sector = ((cluster - 2) * SecPerClus) + first_data_sector;
```

### Reading a File — Algorithm

```
1. Parse boot sector, determine FAT type, calculate layout offsets
2. Find file: search root directory entries for matching 8.3 name
3. Get first cluster from directory entry (FstClusHI << 16 | FstClusLO)
4. For each cluster in chain:
   a. Calculate sector: (cluster - 2) * SecPerClus + first_data_sector
   b. Read SecPerClus sectors from disk
   c. Copy to output buffer (handle partial last cluster using FileSize)
   d. Read FAT[cluster] to get next cluster
   e. If next cluster >= end-of-chain marker, stop
```

### Writing a File — Algorithm

```
1. Find free directory entry (first byte = 0x00 or 0xE5)
2. Scan FAT for free clusters (entry = 0x0000)
3. For each chunk of data:
   a. Allocate a free cluster (mark it in FAT)
   b. Chain it to previous cluster (set FAT[prev] = current)
   c. Write data to cluster's sectors on disk
4. Set FAT[last_cluster] = end-of-chain marker
5. Fill directory entry: name, ext, attr, first cluster, file size, dates
6. Write updated FAT to disk (both copies)
7. Write updated directory sector to disk
```

### What We Need for FREECOM Compatibility

FREECOM uses these INT 21h file operations (ch-09):

| INT 21h | Function | Our FAT driver must |
|---------|----------|-------------------|
| AH=39h | MKDIR | Create directory entry with Attr=0x10, allocate cluster |
| AH=3Ah | RMDIR | Remove directory entry, free clusters |
| AH=3Bh | CHDIR | Track current directory per-task |
| AH=3Ch | Create file | New directory entry + allocate first cluster |
| AH=3Dh | Open file | Find directory entry, create file handle |
| AH=3Eh | Close file | Flush buffers, update directory entry |
| AH=3Fh | Read file | Follow FAT chain, read clusters |
| AH=40h | Write file | Allocate clusters as needed, write data |
| AH=41h | Delete file | Mark directory entry deleted (0xE5), free FAT chain |
| AH=42h | Seek | Update file position in handle table |
| AH=43h | Get/set attributes | Read/write Attr byte in directory entry |
| AH=47h | Get current directory | Return tracked CWD string |
| AH=4Eh | Find first | Search directory for matching pattern |
| AH=4Fh | Find next | Continue directory search |
| AH=56h | Rename | Modify directory entry name/location |
| AH=57h | Get/set file date | Read/write date/time in directory entry |

**Estimated implementation size:** ~1500-2500 lines of C for a read/write FAT16/32 driver with directory support.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| Microsoft FAT Spec | fatgen103.pdf (public) | Official BPB, directory, FAT chain format |
| DJGPP getfatsz.c | djgpp libc source | FAT type determination algorithm |
| DJGPP getfstyp.c | djgpp libc source | Boot sector parsing |
| FREECOM dir.c | freecom-master/cmd/dir.c | Directory traversal patterns |

---

*Last updated: 2026-04-28*
