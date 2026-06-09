#!/usr/bin/env python3
"""
build-pure-usb.py — Builds pinecore-pure-usb.img.

A 64 MB raw disk image with:
  LBA 0       : MBR (our boot code + partition table)
  LBA 63      : VBR (our boot code + mformat-generated BPB + stamped LBA/count)
  LBA 64..    : FAT16 filesystem
                - PCBOOT.SYS  (mcopy'd first → lands at cluster 2)
                - KERNEL.BIN
                - any additional files

No FreeDOS in the boot chain. Designed for direct dd to a real USB stick
for Vortex86 testing.
"""

import argparse, os, shutil, struct, subprocess, sys, tempfile

SECTOR_SIZE    = 512
IMG_SIZE_MB    = 64
PARTITION_LBA  = 63               # standard MBR partition-1 LBA
PT_OFFSET      = 0x1BE            # partition table within MBR
SIG_OFF        = 0x1FE
STAMP_SEC_OFF  = 0x1F8            # u16: pcboot sector count
STAMP_LBA_OFF  = 0x1FA            # u32: pcboot absolute LBA


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mbr",    required=True, help="mbr.bin (512 bytes)")
    ap.add_argument("--vbr",    required=True, help="vbr.bin (512 bytes)")
    ap.add_argument("--pcboot", required=True, help="pcboot.bin (multiple of 512)")
    ap.add_argument("--kernel", required=True, help="kernel.pure.flat")
    ap.add_argument("--out",    required=True, help="output image")
    ap.add_argument("--apps",   nargs="*", default=[], help="extra files to mcopy")
    ap.add_argument("--from-image", help="mirror all files from an existing FAT image (e.g., pinecore-pure-hdd.img)")
    ap.add_argument("--modules-dir", help="stage all *.kmd files from this dir into /DRIVERS/")
    args = ap.parse_args()

    mbr_bin    = open(args.mbr, "rb").read()
    vbr_bin    = open(args.vbr, "rb").read()
    pcboot_bin = open(args.pcboot, "rb").read()
    assert len(mbr_bin)    == SECTOR_SIZE,       f"MBR must be {SECTOR_SIZE} bytes"
    assert len(vbr_bin)    == SECTOR_SIZE,       f"VBR must be {SECTOR_SIZE} bytes"
    assert len(pcboot_bin) % SECTOR_SIZE == 0,   "PCBOOT.SYS must be sector-aligned"

    img_size = IMG_SIZE_MB * 1024 * 1024
    print(f"[build] Creating sparse {IMG_SIZE_MB} MB image: {args.out}")
    with open(args.out, "wb") as f:
        f.truncate(img_size)

    # ---- MBR boot code + signature
    write_at(args.out, 0, mbr_bin[:PT_OFFSET])
    write_at(args.out, SIG_OFF, b"\x55\xAA")

    # ---- Partition table entry: bootable, FAT16-LBA (0x0E), LBA 63..end
    part_size = img_size // SECTOR_SIZE - PARTITION_LBA
    pt_entry = struct.pack(
        "<B 3B B 3B I I",
        0x80,                       # boot flag
        0, 1, 1,                    # start CHS (legacy, unused under LBA)
        0x0E,                       # type: FAT16 LBA
        0xFE, 0xFF, 0xFF,           # end CHS (legacy)
        PARTITION_LBA,              # start LBA
        part_size,                  # sector count
    )
    write_at(args.out, PT_OFFSET, pt_entry)
    print(f"[build] Partition 1: type=0x0E start={PARTITION_LBA} sectors={part_size}")

    # ---- mformat the partition area
    offset_bytes = PARTITION_LBA * SECTOR_SIZE
    mtools_target = f"{args.out}@@{offset_bytes}"
    print(f"[build] mformat FAT16 at byte offset {offset_bytes}")
    subprocess.check_call([
        "mformat", "-i", mtools_target, "-v", "PINECORE", "::",
    ])

    # ---- Read mformat-generated BPB
    bpb = read_at(args.out, offset_bytes, SECTOR_SIZE)
    bps             = struct.unpack("<H", bpb[0x0B:0x0D])[0]
    spc             = bpb[0x0D]
    reserved        = struct.unpack("<H", bpb[0x0E:0x10])[0]
    num_fats        = bpb[0x10]
    root_entries    = struct.unpack("<H", bpb[0x11:0x13])[0]
    fat16_spf       = struct.unpack("<H", bpb[0x16:0x18])[0]
    hidden_sectors  = struct.unpack("<I", bpb[0x1C:0x20])[0]
    large_total     = struct.unpack("<I", bpb[0x20:0x24])[0]
    print(f"[build] BPB: bps={bps} spc={spc} reserved={reserved} num_fats={num_fats}")
    print(f"        root_entries={root_entries} fat16_spf={fat16_spf}")
    print(f"        hidden_sectors={hidden_sectors} large_total={large_total}")

    if bps != SECTOR_SIZE:
        sys.exit(f"[build] FATAL: unsupported bytes_per_sector={bps}")

    # ---- Patch hidden_sectors so the FAT driver knows the partition offset.
    # Patch BOTH the in-memory bpb copy (used for the VBR stamp later) and
    # the on-disk bytes — otherwise the VBR stamp below will undo the disk
    # patch when it writes the saved bpb[0x03:0x3E] back.
    if hidden_sectors != PARTITION_LBA:
        print(f"[build] Patching BPB hidden_sectors {hidden_sectors} → {PARTITION_LBA}")
        bpb = bpb[:0x1C] + struct.pack("<I", PARTITION_LBA) + bpb[0x20:]
        write_at(args.out, offset_bytes + 0x1C, struct.pack("<I", PARTITION_LBA))
        hidden_sectors = PARTITION_LBA

    # ---- mcopy PCBOOT.SYS FIRST so it gets cluster 2
    print(f"[build] mcopy {args.pcboot} → ::/PCBOOT.SYS")
    subprocess.check_call([
        "mcopy", "-i", mtools_target, "-o", args.pcboot, "::/PCBOOT.SYS",
    ])

    # ---- mcopy kernel
    print(f"[build] mcopy {args.kernel} → ::/KERNEL.BIN")
    subprocess.check_call([
        "mcopy", "-i", mtools_target, "-o", args.kernel, "::/KERNEL.BIN",
    ])

    # ---- Mirror everything else from a reference image (apps, docs, configs)
    if args.from_image:
        extract = tempfile.mkdtemp(prefix="pure_mirror_")
        try:
            print(f"[build] Mirror: extracting all files from {args.from_image} → {extract}")
            # `mcopy -s -i src ::/ extract/` recursively copies root contents
            subprocess.check_call([
                "mcopy", "-s", "-Q", "-n", "-i", args.from_image,
                "::/", extract + "/",
            ])
            for entry in sorted(os.listdir(extract)):
                if entry.upper() in ("KERNEL.BIN", "PCBOOT.SYS", "PINE.COM"):
                    continue  # already in image (or unwanted)
                if entry.startswith(".") or entry.upper() == ".FSEVENTSD":
                    continue  # skip macOS metadata
                src_path = os.path.join(extract, entry)
                print(f"[build]   mcopy {entry} → ::/{entry}")
                subprocess.check_call([
                    "mcopy", "-s", "-Q", "-n", "-i", mtools_target,
                    src_path, "::/",
                ])
        finally:
            shutil.rmtree(extract, ignore_errors=True)

    # ---- Stage kernel modules into /DRIVERS/
    if args.modules_dir and os.path.isdir(args.modules_dir):
        kmds = sorted(
            f for f in os.listdir(args.modules_dir) if f.lower().endswith(".kmd")
        )
        if kmds:
            print(f"[build] mmd ::/DRIVERS (skip if exists)")
            subprocess.run(
                ["mmd", "-i", mtools_target, "::/DRIVERS"],
                check=False,
            )
            for k in kmds:
                src = os.path.join(args.modules_dir, k)
                print(f"[build]   mcopy {k} → ::/DRIVERS/{k.upper()}")
                subprocess.check_call([
                    "mcopy", "-i", mtools_target, "-o", src,
                    f"::/DRIVERS/{k.upper()}",
                ])

    # ---- Extra apps
    for app in args.apps:
        name = os.path.basename(app).upper()
        print(f"[build] mcopy {app} → ::/{name}")
        subprocess.check_call([
            "mcopy", "-i", mtools_target, "-o", app, f"::/{name}",
        ])

    # ---- Locate PCBOOT.SYS starting cluster in the root directory
    root_dir_lba     = PARTITION_LBA + reserved + num_fats * fat16_spf
    root_dir_sectors = (root_entries * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE
    root_dir = read_at(
        args.out, root_dir_lba * SECTOR_SIZE, root_dir_sectors * SECTOR_SIZE
    )
    pcboot_cluster = find_file_cluster(root_dir, b"PCBOOT  SYS")
    if pcboot_cluster is None:
        sys.exit("[build] FATAL: PCBOOT.SYS not found in root dir after mcopy")
    print(f"[build] PCBOOT.SYS first cluster = {pcboot_cluster}")
    if pcboot_cluster != 2:
        print(f"[build] WARNING: expected cluster 2, got {pcboot_cluster}")

    data_start_lba = root_dir_lba + root_dir_sectors
    pcboot_lba_abs = data_start_lba + (pcboot_cluster - 2) * spc
    pcboot_sectors = (len(pcboot_bin) + SECTOR_SIZE - 1) // SECTOR_SIZE
    print(f"[build] PCBOOT.SYS @ LBA {pcboot_lba_abs} ({pcboot_sectors} sectors)")

    # ---- Stamp our VBR's boot code, preserving mformat's BPB
    # VBR layout in our assembled vbr.bin:
    #   0x000-0x002 : jmp/nop  (keep)
    #   0x003-0x03D : BPB placeholder (REPLACE with mformat's real BPB)
    #   0x03E-0x1F7 : boot code (keep)
    #   0x1F8-0x1F9 : sectors  (stamp)
    #   0x1FA-0x1FD : LBA      (stamp)
    #   0x1FE-0x1FF : 0xAA55   (keep)
    vbr_stamped = bytearray(vbr_bin)
    vbr_stamped[0x03:0x3E] = bpb[0x03:0x3E]
    vbr_stamped[STAMP_SEC_OFF:STAMP_SEC_OFF+2] = struct.pack("<H", pcboot_sectors)
    vbr_stamped[STAMP_LBA_OFF:STAMP_LBA_OFF+4] = struct.pack("<I", pcboot_lba_abs)
    assert len(vbr_stamped) == SECTOR_SIZE
    write_at(args.out, offset_bytes, bytes(vbr_stamped))

    print(f"[build] DONE: {args.out}")
    sz = os.path.getsize(args.out)
    print(f"        Size: {sz} bytes ({sz // (1024*1024)} MB)")


def write_at(path, offset, data):
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(data)


def read_at(path, offset, length):
    with open(path, "rb") as f:
        f.seek(offset)
        return f.read(length)


def find_file_cluster(root_dir, name11):
    """Return starting cluster of a file matching the 11-byte 8.3 name."""
    for i in range(0, len(root_dir), 32):
        e = root_dir[i:i+32]
        if not e:
            break
        if e[0] == 0x00:
            return None
        if e[0] == 0xE5:
            continue
        if e[0x0B] & 0x08:        # volume label
            continue
        if e[:11] == name11:
            return struct.unpack("<H", e[0x1A:0x1C])[0]
    return None


if __name__ == "__main__":
    main()
