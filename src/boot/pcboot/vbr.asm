; ============================================================================
; vbr.asm — Pinecore FAT16 Volume Boot Record (LBA + CHS hybrid + VGA trace)
;
; Entry from MBR:
;   CS:IP = 0000:7C00, DL = boot drive, DS:SI = MBR partition table entry
;
; Layout:
;   0x000-0x002 : JMP SHORT start; NOP
;   0x003-0x00A : OEM name ("PCBOOT  ")
;   0x00B-0x03D : FAT16 BPB — REWRITTEN BY IMAGE BUILDER
;   0x03E-0x1F7 : boot code
;   0x1F8-0x1F9 : pcboot_sectors (u16, stamped by image builder)
;   0x1FA-0x1FD : pcboot_lba_abs (u32, absolute disk LBA, stamped)
;   0x1FE-0x1FF : 0x55 0xAA
;
; Visible VGA progress (positions 4..n on top row):
;   'b' VBR entered
;   'L' attempting LBA read   →  'P' on success (PCBOOT.SYS loaded)
;   'C' falling back to CHS   →  'P' on success
;   'E' = read failure
;
; Hands off to PCBOOT.SYS with:
;   DL = boot drive, EBX = partition LBA (= BPB hidden_sectors)
;   VGA write position at [0x500] so PCBOOT.SYS can continue tracing.
; ============================================================================

[bits 16]
[org 0x7C00]

%define COM1        0x3F8
%define LOAD_SEG    0x0800
%define LOAD_OFF    0x0000
%define DAP_ADDR    0x7E00
%define VGA_BASE    0xB800
%define VGA_POS_VAR 0x0500        ; shared VGA cursor pos with PCBOOT.SYS

%define BPB_HIDDEN  0x1C
%define STAMP_SEC_OFF 0x1F8
%define STAMP_LBA_OFF 0x1FA

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

    ; Continue VGA trace at position 4 (right after MBR's 'M' 'F' 'L/C' 'V')
    mov   word [VGA_POS_VAR], 8
    mov   al, 'b'
    call  trace

    ; ----- Query BIOS geometry for CHS fallback
    push  dx
    mov   ah, 0x08
    int   0x13
    jc    .geom_fail
    movzx ax, dh
    inc   ax
    mov   [bios_heads], ax
    movzx ax, cl
    and   ax, 0x3F
    mov   [bios_spt], ax
    pop   dx
    jmp   .geom_ok
.geom_fail:
    pop   dx
    mov   word [bios_heads], 16    ; reasonable default
    mov   word [bios_spt], 63
.geom_ok:

    ; ----- Check LBA extensions
    push  dx
    mov   ah, 0x41
    mov   bx, 0x55AA
    int   0x13
    pop   dx
    jc    .no_lba
    cmp   bx, 0xAA55
    jne   .no_lba

    ; --- LBA path
    mov   al, 'L'
    call  trace

    mov   di, DAP_ADDR
    mov   byte [di+0], 0x10
    mov   byte [di+1], 0
    mov   ax, [0x7C00 + STAMP_SEC_OFF]
    mov   [di+2], ax
    mov   word [di+4], LOAD_OFF
    mov   word [di+6], LOAD_SEG
    mov   eax, [0x7C00 + STAMP_LBA_OFF]
    mov   [di+8], eax
    xor   eax, eax
    mov   [di+12], eax

    mov   si, DAP_ADDR
    mov   ah, 0x42
    mov   dl, [boot_drive]
    int   0x13
    jnc   .read_ok
    ; LBA read failed, fall through to CHS
.no_lba:
    ; --- CHS path
    mov   al, 'C'
    call  trace

    ; Read PCBOOT.SYS via CHS. One CHS call per track (≤spt sectors).
    mov   eax, [0x7C00 + STAMP_LBA_OFF]
    movzx ecx, word [0x7C00 + STAMP_SEC_OFF]
    mov   bx, LOAD_SEG
    mov   [chs_seg], bx
    mov   word [chs_off], LOAD_OFF
.chs_loop:
    test  ecx, ecx
    jz    .read_ok

    ; Compute CHS for current LBA
    push  ecx
    call  lba_to_chs            ; sets [chs_cyl_low], [chs_sec_cyl_hi], [chs_head]
    pop   ecx

    ; Compute sectors-in-this-track = bios_spt - (sector-1)
    mov   ax, [bios_spt]
    movzx bx, byte [chs_sec_cyl_hi]
    and   bx, 0x3F
    dec   bx                    ; (sector-1) = sector offset within track
    sub   ax, bx                ; remaining sectors in this track
    ; clamp to ECX
    cmp   eax, ecx
    jbe   .have_count
    mov   eax, ecx
.have_count:
    mov   [chs_count], al

    ; Build INT 13h regs
    mov   ah, 0x02
    mov   al, [chs_count]
    mov   ch, [chs_cyl_low]
    mov   cl, [chs_sec_cyl_hi]
    mov   dh, [chs_head]
    mov   dl, [boot_drive]
    mov   bx, [chs_off]
    push  es
    mov   es, [chs_seg]
    int   0x13
    pop   es
    jc    .err

    ; Advance LBA, count, seg by sectors-read
    movzx ebx, byte [chs_count]
    mov   eax, [chs_cur_lba]
    add   eax, ebx
    mov   [chs_cur_lba], eax
    sub   ecx, ebx
    ; seg += sectors * 32 paragraphs
    shl   bx, 5
    add   [chs_seg], bx
    jmp   .chs_loop

.read_ok:
    ; Hand off
    mov   ebx, [0x7C00 + BPB_HIDDEN]
    mov   dl, [boot_drive]
    mov   al, 'P'
    call  trace
    jmp   LOAD_SEG:LOAD_OFF

.err:
    mov   al, 'E'
    call  trace
halt:
    cli
    hlt
    jmp   halt

; ----------------------------------------------------------------------------
; lba_to_chs — convert EAX (LBA) → CHS bytes in chs_* fields.
; Also stashes EAX into chs_cur_lba so caller can advance it.
; ----------------------------------------------------------------------------
lba_to_chs:
    mov   [chs_cur_lba], eax
    xor   edx, edx
    movzx ebx, word [bios_spt]
    div   ebx                     ; EAX = LBA/spt, EDX = sec_idx (0-based)
    mov   cl, dl
    inc   cl                      ; sector (1-based)
    xor   edx, edx
    movzx ebx, word [bios_heads]
    div   ebx                     ; EAX = cyl, EDX = head
    mov   [chs_head], dl
    mov   [chs_cyl_low], al
    shr   eax, 8
    shl   al, 6                   ; cyl_hi bits → CL[7:6]
    or    cl, al
    mov   [chs_sec_cyl_hi], cl
    ret

; ----------------------------------------------------------------------------
; trace — AL = char. VGA top-row + COM1.
; Preserves all regs.
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
chs_seg:          dw 0
chs_off:          dw 0
chs_count:        db 0
chs_head:         db 0
chs_cyl_low:      db 0
chs_sec_cyl_hi:   db 0
chs_cur_lba:      dd 0

; ----------------------------------------------------------------------------
; Pad to STAMP_SECTORS_OFF (0x1F8), then stamps, then signature
; ----------------------------------------------------------------------------
times STAMP_SEC_OFF - ($ - $$) db 0
pcboot_sectors_stamp:  dw 0
pcboot_lba_abs_stamp:  dd 0
dw 0xAA55
