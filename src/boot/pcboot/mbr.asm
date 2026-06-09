; ============================================================================
; mbr.asm — Pinecore native MBR (LBA-then-CHS hybrid + VGA trace)
;
; Entry from BIOS:
;   CS:IP = 0000:7C00, DL = BIOS boot drive (typically 0x80)
;
; Job:
;   1. Relocate self from 0x7C00 → 0x0600 (free 0x7C00 for VBR)
;   2. Scan partition table for active partition (boot indicator 0x80)
;   3. Read partition VBR at 0x7C00 — LBA via INT 13h AH=42h if supported,
;      else CHS via INT 13h AH=02h using the partition entry's CHS bytes.
;   4. Far jump 0:7C00 to VBR, with:
;        DL = boot drive
;        DS:SI = partition table entry
;
; Visible to user (without COM1):
;   Top-left of VGA text screen shows milestone chars:
;     'M' MBR entered + relocated
;     'F' active partition found
;     'L' attempting LBA read   →  'V' on success
;     'C' falling back to CHS   →  'V' on success
;     'X' = no active partition
;     'E' = read or signature error
;
; Also traces same chars to COM1 (0x3F8) for capture when available.
;
; Layout: 446 bytes code + 64-byte partition table + 0x55 0xAA signature.
; ============================================================================

[bits 16]
[org 0x0600]

%define COM1        0x3F8
%define BOOT_LOAD   0x7C00
%define RELOC_DEST  0x0600
%define PT_OFFSET   0x1BE
%define DAP_ADDR    0x0500
%define VGA_BASE    0xB800

start:
    cli
    xor   ax, ax
    mov   ss, ax
    mov   sp, BOOT_LOAD
    mov   ds, ax
    mov   es, ax
    cld
    sti

    ; Relocate 512 bytes from 0x7C00 → 0x0600, continue at low copy
    mov   si, BOOT_LOAD
    mov   di, RELOC_DEST
    mov   cx, 256
    rep   movsw
    jmp   0:relocated

relocated:
    ; Init VGA cursor position to top-left
    mov   word [vga_pos], 0
    mov   al, 'M'
    call  trace

    ; Scan partition table for active entry
    mov   si, RELOC_DEST + PT_OFFSET
    mov   cx, 4
.scan:
    cmp   byte [si], 0x80
    je    .found
    add   si, 16
    loop  .scan

    mov   al, 'X'
    call  trace
    jmp   halt

.found:
    mov   al, 'F'
    call  trace

    ; SI = active partition entry
    ;   +1..+3  : start CHS (head, sec|cyl_hi, cyl_lo)
    ;   +8..+11 : start LBA (32-bit)

    ; --- Try LBA first
    push  si
    push  dx
    mov   ah, 0x41
    mov   bx, 0x55AA
    int   0x13
    pop   dx
    pop   si
    jc    .use_chs
    cmp   bx, 0xAA55
    jne   .use_chs

    mov   al, 'L'
    call  trace
    call  read_lba
    jc    .use_chs                ; if LBA path failed, try CHS
    jmp   .read_ok

.use_chs:
    mov   al, 'C'
    call  trace
    call  read_chs
    jc    .err

.read_ok:
    ; Verify VBR signature
    cmp   word [BOOT_LOAD + 0x1FE], 0xAA55
    jne   .err

    mov   al, 'V'
    call  trace

    ; Hand off to VBR. DL = boot drive, DS:SI = partition entry.
    jmp   0:BOOT_LOAD

.err:
    mov   al, 'E'
    call  trace
halt:
    cli
    hlt
    jmp   halt

; ----------------------------------------------------------------------------
; read_lba — INT 13h AH=42h, 1 sector from partition LBA into 0:7C00.
; Inputs: SI = partition entry, DL = boot drive
; Returns CF=1 on error.
; ----------------------------------------------------------------------------
read_lba:
    mov   di, DAP_ADDR
    mov   byte [di+0], 0x10
    mov   byte [di+1], 0
    mov   word [di+2], 1
    mov   word [di+4], BOOT_LOAD
    mov   word [di+6], 0
    mov   eax, [si+8]
    mov   [di+8], eax
    xor   eax, eax
    mov   [di+12], eax
    push  si
    mov   si, DAP_ADDR
    mov   ah, 0x42
    int   0x13
    pop   si
    ret

; ----------------------------------------------------------------------------
; read_chs — INT 13h AH=02h using partition entry's start CHS bytes.
; Inputs: SI = partition entry, DL = boot drive
; Returns CF=1 on error.
; ----------------------------------------------------------------------------
read_chs:
    push  bx
    mov   dh, [si+1]              ; head
    mov   cl, [si+2]              ; sector + cyl_hi
    mov   ch, [si+3]              ; cyl_lo
    mov   bx, BOOT_LOAD
    mov   ax, 0x0201              ; AH=02 read, AL=1 sector
    int   0x13
    pop   bx
    ret

; ----------------------------------------------------------------------------
; trace — AL = char. Print to VGA top row AND COM1.
; Preserves all regs.
; ----------------------------------------------------------------------------
trace:
    push  ax
    push  bx
    push  es
    ; VGA write
    push  ax
    mov   bx, VGA_BASE
    mov   es, bx
    mov   bx, [vga_pos]
    mov   ah, 0x0F                ; white on black
    mov   [es:bx], ax
    add   word [vga_pos], 2
    pop   ax
    pop   es
    pop   bx
    ; COM1 write
    push  dx
    mov   dx, COM1 + 5
.wait:
    in    al, dx
    test  al, 0x20
    jz    .wait
    pop   dx
    pop   ax
    push  ax
    push  dx
    mov   dx, COM1
    out   dx, al
    pop   dx
    pop   ax
    ret

vga_pos: dw 0

; ----------------------------------------------------------------------------
; Pad to 446 bytes, then 64-byte partition table area, then signature.
; ----------------------------------------------------------------------------
times 446 - ($ - $$) db 0
times 64 db 0
dw 0xAA55
