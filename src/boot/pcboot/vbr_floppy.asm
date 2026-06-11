; ============================================================================
; vbr_floppy.asm — Pinecore FAT12 floppy Volume Boot Record
;
; Entry from BIOS:
;   CS:IP = 0000:7C00, DL = boot drive (0x00 for floppy A:)
;
; Floppy has no MBR / no partition table — BIOS loads sector 0 directly.
;
; Layout (identical to vbr.asm, so the image builder can stamp the same way):
;   0x000-0x002 : JMP SHORT start; NOP
;   0x003-0x00A : OEM name (mformat fills this)
;   0x00B-0x03D : FAT12 BPB — REWRITTEN BY IMAGE BUILDER
;   0x03E-0x1F7 : boot code
;   0x1F8-0x1F9 : pcboot_sectors (u16, stamped by image builder)
;   0x1FA-0x1FD : pcboot_lba_abs (u32, absolute disk LBA, stamped)
;   0x1FE-0x1FF : 0x55 0xAA
;
; Reads PCBOOT.SYS via single-sector INT 13h AH=02 (CHS) with retry.
; Single-sector is slower but avoids SeaBIOS's floppy DMA-boundary
; rejection (error 0x09) on multi-sector reads.
;
; Trace markers (VGA top row + COM1):
;   'b' VBR entered, 'r' reset OK,
;   '.' per sector read, 'P' all sectors loaded,
;   on error: 1 hex nibble of AH (BIOS error code low nibble) + 'E'.
;
; Hands off to PCBOOT.SYS with:
;   DL = boot drive, EBX = 0 (floppy has no partition offset / hidden=0)
;   VGA cursor pos at [0x500] so PCBOOT.SYS can continue tracing.
; ============================================================================

[bits 16]
[org 0x7C00]

%define COM1        0x3F8
%define LOAD_SEG    0x0800
%define LOAD_OFF    0x0000
%define VGA_BASE    0xB800
%define VGA_POS_VAR 0x0500

%define STAMP_SEC_OFF 0x1F8
%define STAMP_LBA_OFF 0x1FA

%define MAX_RETRIES 3

jmp_start:
    jmp   short start
    nop

; BPB placeholder (offsets 3-0x3D), overwritten by image builder
times (0x3E - 3) db 0

; ============================================================================
; Boot code begins at offset 0x3E
; ============================================================================
start:
    cli
    xor   ax, ax
    mov   ss, ax
    mov   sp, 0x7C00
    mov   ds, ax
    mov   es, ax
    cld
    sti

    mov   [boot_drive], dl

    ; Top-row VGA cursor at column 4 (chars 'b','r','.','.'... so PCBOOT
    ; resumes at the right cell)
    mov   word [VGA_POS_VAR], 8
    mov   al, 'b'
    call  trace

    ; ----- Reset floppy controller (essential on cold boot — motor spin)
    mov   ah, 0x00
    mov   dl, [boot_drive]
    int   0x13
    jc    .err

    mov   al, 'r'
    call  trace

    ; ----- Read geometry directly from BPB on disk (offsets 0x18=spt,
    ; 0x1A=heads). Skips INT 13h AH=08, which on some old BIOSes (AMI 1995)
    ; hangs on floppy. The BPB is mformat-authoritative.
    mov   ax, [0x7C00 + 0x18]
    mov   [bios_spt], ax
    mov   ax, [0x7C00 + 0x1A]
    mov   [bios_heads], ax

    ; Sanity: if BPB has zeros, force 1.44 MB defaults
    cmp   word [bios_heads], 0
    jne   .geom_ok
    mov   word [bios_heads], 2
    mov   word [bios_spt], 18
.geom_ok:

    ; ----- Read PCBOOT.SYS one sector at a time.
    ;       LBA in EAX, sectors-remaining in CX, dest seg in [chs_seg].
    mov   eax, [0x7C00 + STAMP_LBA_OFF]
    mov   [cur_lba], eax
    movzx cx, word [0x7C00 + STAMP_SEC_OFF]
    mov   ax, LOAD_SEG
    mov   [chs_seg], ax

.read_loop:
    test  cx, cx
    jz    .done

    push  cx
    mov   eax, [cur_lba]
    call  lba_to_chs              ; sets cyl_low/sec_cyl_hi/head
    pop   cx

    mov   bx, MAX_RETRIES
.retry:
    push  bx
    push  cx
    mov   ah, 0x02
    mov   al, 1
    mov   ch, [cyl_low]
    mov   cl, [sec_cyl_hi]
    mov   dh, [head]
    mov   dl, [boot_drive]
    push  es
    mov   es, [chs_seg]
    xor   bx, bx
    int   0x13
    pop   es
    pop   cx
    pop   bx
    jnc   .read_ok

    ; retry: reset + try again
    push  bx
    push  cx
    mov   ah, 0
    mov   dl, [boot_drive]
    int   0x13
    pop   cx
    pop   bx
    dec   bx
    jnz   .retry
    jmp   .err

.read_ok:
    mov   al, '.'
    call  trace
    ; Advance: cur_lba++, chs_seg += 32 paras, cx--
    inc   dword [cur_lba]
    add   word [chs_seg], 32
    dec   cx
    jmp   .read_loop

.done:
    xor   ebx, ebx                ; floppy: hand off partition_lba = 0
    mov   dl, [boot_drive]
    mov   al, 'P'
    call  trace
    jmp   LOAD_SEG:LOAD_OFF

.err:
    ; Print AH low nibble as 1 hex char + 'E'
    mov   al, ah
    and   al, 0x0F
    add   al, '0'
    cmp   al, '9'
    jbe   .e_ok
    add   al, 7
.e_ok:
    call  trace
    mov   al, 'E'
    call  trace
halt:
    cli
    hlt
    jmp   halt

; ----------------------------------------------------------------------------
; lba_to_chs — convert EAX (LBA) → cyl_low / sec_cyl_hi / head fields.
; Trashes EAX, EBX, ECX, EDX.
; ----------------------------------------------------------------------------
lba_to_chs:
    xor   edx, edx
    movzx ebx, word [bios_spt]
    div   ebx                     ; eax = lba/spt, edx = sec_idx
    mov   cl, dl
    inc   cl                      ; sector (1-based) in CL
    xor   edx, edx
    movzx ebx, word [bios_heads]
    div   ebx                     ; eax = cyl, edx = head
    mov   [head], dl
    mov   [cyl_low], al
    shr   eax, 8
    shl   al, 6                   ; cyl_hi[1:0] → CL[7:6]
    or    cl, al
    mov   [sec_cyl_hi], cl
    ret

; ----------------------------------------------------------------------------
; trace — AL = char. VGA + COM1.
; Preserves AX, BX, CX, DX, ES.
; ----------------------------------------------------------------------------
trace:
    push  ax
    push  bx
    push  es
    push  ax
    mov   bx, VGA_BASE
    mov   es, bx
    mov   bx, [VGA_POS_VAR]
    mov   ah, 0x0F
    mov   [es:bx], ax
    add   word [VGA_POS_VAR], 2
    pop   ax
    pop   es
    pop   bx
    push  dx
    mov   dx, COM1 + 5
.tw:
    in    al, dx
    test  al, 0x20
    jz    .tw
    pop   dx
    pop   ax
    push  ax
    push  dx
    mov   dx, COM1
    out   dx, al
    pop   dx
    pop   ax
    ret

; ----------------------------------------------------------------------------
; locals
; ----------------------------------------------------------------------------
boot_drive:       db 0
bios_heads:       dw 0
bios_spt:         dw 0
cur_lba:          dd 0
chs_seg:          dw 0
head:             db 0
cyl_low:          db 0
sec_cyl_hi:       db 0

; ----------------------------------------------------------------------------
; Pad to STAMP_SEC_OFF (0x1F8), then stamps, then signature
; ----------------------------------------------------------------------------
times STAMP_SEC_OFF - ($ - $$) db 0
pcboot_sectors_stamp:  dw 0
pcboot_lba_abs_stamp:  dd 0
dw 0xAA55
