; ============================================================================
; pcboot.asm — Pinecore Stage-2 Boot Loader (PCBOOT.SYS equivalent)
;
; Entry from VBR:
;   CS:IP = 0x0800:0x0000, DL = boot drive, EBX = partition LBA
;
; Job:
;   1. Set up stack + segments + A20
;   2. Copy BPB from 0:0x7C00 into local storage
;   3. Read FAT16 FAT + root directory from disk (low-memory buffers)
;   4. Find KERNEL.BIN in root directory
;   5. Walk cluster chain, read each cluster into a low-memory kernel buffer
;      (we cap the kernel at 320 KB; that's well above the current 156 KB)
;   6. Set up GDT + identity-mapped paging (first 4MB)
;   7. Enter PM, rep movsd the kernel buffer to 0x100000+
;   8. Far jump to 0x08:0x100010 (skip kernel's multiboot header)
;
; Kernel entry contract identical to PINE.COM (see boot.asm: GDT 0x08/0x10).
;
; Trace markers to COM1 (0x3F8):
;   's' start, 'B' BPB copied, 'A' A20,
;   'F' FAT read, 'D' root dir read, 'K' KERNEL.BIN found,
;   '.' per cluster, '|' chain done,
;   'G' GDT loaded, 'P' paging up, 'J' jumping to kernel.
;
; Size budget: <= 32 sectors (16 KB). VBR enforces this.
; ============================================================================

[bits 16]
[org 0x0000]                      ; runs at 0x0800:0x0000

%define COM1                0x3F8
%define VGA_BASE            0xB800
%define VGA_POS_VAR         0x0500            ; shared VGA cursor from MBR/VBR
%define KERNEL_DEST         0x100000          ; linear 1MB
%define BPB_SRC             0x7C00            ; VBR still here at boot
%define FAT_BUF_LIN         0x10000           ; FAT buffer (linear)
%define ROOT_BUF_LIN        0x30000           ; root dir buffer (linear; past FAT)
%define KERNEL_BUF_LIN      0x40000           ; kernel staging buffer (320 KB cap)
%define KERNEL_BUF_END      0x90000           ; one past last byte of buffer
%define PAGE_DIR_LIN        0x90000           ; page directory (PINE.COM convention)
%define PAGE_TBL_LIN        0x91000           ; first page table

%define FAT_BUF_SEG         (FAT_BUF_LIN >> 4)
%define ROOT_BUF_SEG        (ROOT_BUF_LIN >> 4)

; GDT selectors (must match src/boot/boot.asm)
%define SEL_CODE32          0x08
%define SEL_DATA32          0x10
%define SEL_CODE16          0x30
%define SEL_DATA16          0x38

; ============================================================================
; Entry
; ============================================================================
start:
    cli
    xor   ax, ax
    mov   ss, ax
    mov   sp, 0x7C00              ; stack grows down from 0x7C00, away from us
    mov   ax, cs                  ; CS = 0x0800
    mov   ds, ax
    mov   es, ax
    cld
    sti

    mov   [boot_drive], dl
    mov   [partition_lba], ebx

    mov   al, 's'
    call  trace

    ; --- Query LBA extensions
    push  dx
    mov   ah, 0x41
    mov   bx, 0x55AA
    int   0x13
    pop   dx
    mov   byte [has_lba], 0
    jc    .no_lba_init
    cmp   bx, 0xAA55
    jne   .no_lba_init
    mov   byte [has_lba], 1
.no_lba_init:

    ; --- Query BIOS geometry (used by CHS path)
    push  dx
    mov   ah, 0x08
    mov   dl, [boot_drive]
    int   0x13
    jc    .geom_default
    movzx ax, dh
    inc   ax
    mov   [bios_heads], ax
    movzx ax, cl
    and   ax, 0x3F
    mov   [bios_spt], ax
    pop   dx
    jmp   .geom_done
.geom_default:
    pop   dx
    mov   word [bios_heads], 16
    mov   word [bios_spt], 63
.geom_done:

    ; --- Copy BPB from 0:0x7C00+11 → DS:bpb_copy
    push  ds
    push  es
    xor   ax, ax
    mov   ds, ax                  ; DS = 0 (source)
    mov   ax, cs
    mov   es, ax                  ; ES = our seg (dest)
    mov   si, BPB_SRC + 11
    mov   di, bpb_copy
    mov   cx, 51
    rep   movsb
    pop   es
    pop   ds

    mov   al, 'B'
    call  trace

    ; --- Enable A20 (port 0x92 fast method)
    in    al, 0x92
    test  al, 2
    jnz   .a20_done
    or    al, 2
    and   al, 0xFE                ; don't reset machine
    out   0x92, al
.a20_done:
    mov   al, 'A'
    call  trace

    ; ----------------------------------------------------------------------
    ; Compute on-disk locations of FAT, root dir, data area.
    ;   fat_start  = partition_lba + reserved_sectors
    ;   root_start = fat_start + num_fats * fat16_spf
    ;   data_start = root_start + root_dir_sectors
    ;   root_dir_sectors = ceil(root_entries * 32 / bytes_per_sector)
    ; ----------------------------------------------------------------------
    movzx eax, word [root_entries_lc]
    shl   eax, 5                  ; *32
    add   eax, 511
    shr   eax, 9                  ; /512
    mov   [root_dir_sectors], eax

    mov   eax, [partition_lba]
    movzx ecx, word [reserved_lc]
    add   eax, ecx
    mov   [fat_start_lba], eax

    movzx ecx, byte [num_fats_lc]
    movzx edx, word [fat16_spf_lc]
    imul  ecx, edx
    add   eax, ecx
    mov   [root_start_lba], eax

    mov   ecx, [root_dir_sectors]
    add   eax, ecx
    mov   [data_start_lba], eax

    ; ----------------------------------------------------------------------
    ; Read entire FAT into FAT_BUF via chunked reads
    ; ----------------------------------------------------------------------
    mov   eax, [fat_start_lba]
    movzx ecx, word [fat16_spf_lc]
    mov   dx, FAT_BUF_SEG
    call  read_chunked
    jc    fatal

    mov   al, 'F'
    call  trace

    ; ----------------------------------------------------------------------
    ; Read root directory into ROOT_BUF via chunked reads
    ; ----------------------------------------------------------------------
    mov   eax, [root_start_lba]
    mov   ecx, [root_dir_sectors]
    mov   dx, ROOT_BUF_SEG
    call  read_chunked
    jc    fatal

    mov   al, 'D'
    call  trace

    ; ----------------------------------------------------------------------
    ; Find KERNEL.BIN in root dir.
    ; Entry = 32 bytes:
    ;   +0    : 8-byte name (space-padded uppercase)
    ;   +8    : 3-byte ext  (space-padded uppercase)
    ;   +0x1A : starting cluster (16-bit)
    ;   +0x1C : file size (32-bit)
    ; ----------------------------------------------------------------------
    push  es
    mov   ax, ROOT_BUF_SEG
    mov   es, ax
    xor   di, di
    movzx ecx, word [root_entries_lc]
.dir_loop:
    cmp   byte [es:di], 0         ; end of directory
    je    .not_found
    cmp   byte [es:di], 0xE5      ; deleted
    je    .next
    mov   si, kernel_name
    push  di
    push  cx
    mov   cx, 11
    repe  cmpsb
    pop   cx
    pop   di
    je    .found
.next:
    add   di, 32
    dec   cx
    jnz   .dir_loop
.not_found:
    pop   es
    mov   al, '!'
    call  trace
    mov   al, 'N'
    call  trace
    jmp   halt

.found:
    mov   ax, [es:di + 0x1A]      ; starting cluster
    mov   [kernel_cluster], ax
    mov   eax, [es:di + 0x1C]
    mov   [kernel_size], eax
    pop   es

    ; Sanity check: kernel must fit in our buffer
    cmp   eax, KERNEL_BUF_END - KERNEL_BUF_LIN
    jbe   .size_ok
    mov   al, '!'
    call  trace
    mov   al, 'Z'                 ; Z = kernel too big
    call  trace
    jmp   halt
.size_ok:
    mov   al, 'K'
    call  trace

    ; ----------------------------------------------------------------------
    ; Walk cluster chain.
    ; For each cluster (< 0xFFF8):
    ;   on-disk sector  = data_start_lba + (cluster - 2) * SPC
    ;   dest linear     = kernel_load_lin (advances by SPC*512)
    ;   load into       = (linear >> 4) : 0
    ;   FAT[cluster] is at byte offset (cluster*2) inside FAT_BUF
    ; ----------------------------------------------------------------------
    mov   dword [kernel_load_lin], KERNEL_BUF_LIN

.chain:
    movzx eax, word [kernel_cluster]
    cmp   ax, 0xFFF8
    jae   .chain_done

    ; Compute on-disk LBA
    sub   eax, 2
    movzx ecx, byte [spc_lc]
    imul  eax, ecx
    add   eax, [data_start_lba]

    ; Compute destination segment (kernel_load_lin >> 4)
    mov   ebx, [kernel_load_lin]
    shr   ebx, 4
    mov   dx, bx

    ; ECX = SPC sectors to read
    movzx ecx, byte [spc_lc]
    call  read_chunked
    jc    fatal

    ; Advance kernel_load_lin by SPC*512
    movzx eax, byte [spc_lc]
    shl   eax, 9
    add   [kernel_load_lin], eax

    ; --- Look up next cluster in FAT16
    push  es
    mov   ax, FAT_BUF_SEG
    mov   es, ax
    movzx ebx, word [kernel_cluster]
    shl   ebx, 1
    mov   ax, [es:bx]
    mov   [kernel_cluster], ax
    pop   es

    mov   al, '.'
    call  trace
    jmp   .chain

.chain_done:
    mov   al, '|'
    call  trace

    ; ----------------------------------------------------------------------
    ; Set up identity-mapped paging for first 4MB
    ; ----------------------------------------------------------------------
    push  es
    mov   ax, PAGE_DIR_LIN >> 4
    mov   es, ax
    xor   di, di
    xor   eax, eax
    mov   cx, 1024
    rep   stosd

    mov   dword [es:0], PAGE_TBL_LIN | 7

    mov   ax, PAGE_TBL_LIN >> 4
    mov   es, ax
    xor   di, di
    mov   eax, 7
    mov   cx, 1024
.fill_pt:
    stosd
    add   eax, 0x1000
    loop  .fill_pt
    pop   es

    mov   al, 'P'
    call  trace

    ; ----------------------------------------------------------------------
    ; Set up GDT and prepare PM transition
    ; ----------------------------------------------------------------------
    xor   eax, eax
    mov   ax, cs
    shl   eax, 4
    add   eax, gdt_start
    mov   [gdt_ptr + 2], eax
    lgdt  [gdt_ptr]

    ; Compute linear addr of pm_entry32 for the far jump
    xor   eax, eax
    mov   ax, cs
    shl   eax, 4
    add   eax, pm_entry32
    mov   [pm_target], eax

    mov   al, 'G'
    call  trace

    ; Stash kernel_load_lin in EBX so 32-bit PM code can use it without
    ; segment-base translation (DS goes flat after the PM transition).
    mov   ebx, [kernel_load_lin]

    cli
    mov   eax, cr0
    or    al, 1
    mov   cr0, eax
    o32 jmp far [pm_jump_far]

; ============================================================================
; 32-bit Protected Mode entry
; ============================================================================
[bits 32]
pm_entry32:
    mov   ax, SEL_DATA32
    mov   ds, ax
    mov   es, ax
    mov   fs, ax
    mov   gs, ax
    mov   ss, ax
    mov   esp, 0x8FFF0            ; temp stack below page tables (just below 0x90000)

    ; 'J' trace before kernel jump (COM1 only; VGA cursor is in PM-flat now)
    mov   dx, COM1
    mov   al, 'J'
    out   dx, al

    ; --- Copy kernel image from KERNEL_BUF_LIN → KERNEL_DEST.
    ; EBX holds kernel_load_lin (linear end of kernel in staging buffer).
    ; Copy actual_size bytes, then zero from end-of-kernel up to 0x180000
    ; so that the kernel's .bss area (which ends at _kernel_end ≈ 0x17DD68)
    ; is fully zeroed before the kernel starts running.
    mov   esi, KERNEL_BUF_LIN
    mov   edi, KERNEL_DEST
    mov   ecx, ebx                ; ecx = kernel_load_lin
    sub   ecx, KERNEL_BUF_LIN     ; ecx = actual_kernel_size bytes
    mov   edx, ecx                ; save for zero step
    shr   ecx, 2
    rep   movsd                   ; copy kernel

    ; Zero from KERNEL_DEST + actual_size to KERNEL_DEST + 0x80000 (512 KB
    ; total). Covers all of .bss with margin; heap is at 0x17E000 onward
    ; and gets re-initialized by the kernel, so a few KB of overlap is OK.
    ; EDI already points at KERNEL_DEST + actual_size (advanced by movsd).
    mov   ecx, 0x80000            ; total 512 KB range from KERNEL_DEST
    sub   ecx, edx                ; ecx = bytes left to zero
    shr   ecx, 2
    xor   eax, eax
    rep   stosd

    ; --- Enable paging
    mov   eax, PAGE_DIR_LIN
    mov   cr3, eax
    mov   eax, cr0
    or    eax, 0x80000000
    mov   cr0, eax
    mov   eax, cr3
    mov   cr3, eax                ; flush TLB

    ; Far jump to kernel — skip 16-byte multiboot header at start of .text
    jmp   SEL_CODE32:(KERNEL_DEST + 0x10)

; ============================================================================
; 16-bit helpers
; ============================================================================
[bits 16]

; ----------------------------------------------------------------------------
; read_via_dap — INT 13h AH=42h read. Inputs from DAP at `dap`.
; Returns CF=1 on error. Preserves nothing else.
; ----------------------------------------------------------------------------
read_via_dap:
    pusha
    mov   si, dap
    mov   dl, [boot_drive]
    mov   ah, 0x42
    int   0x13
    popa
    ret

; ----------------------------------------------------------------------------
; read_chunked — dispatcher. LBA if available, else CHS.
;   EAX = starting LBA, ECX = sector count, DX = dest segment (off 0)
; Returns CF=1 on error. Trashes EAX, EBX, ECX, EDX.
; ----------------------------------------------------------------------------
read_chunked:
    cmp   byte [has_lba], 0
    jne   read_chunked_lba
    jmp   read_chunked_chs

; ----------------------------------------------------------------------------
; read_chunked_lba — INT 13h AH=42h, ≤ 64 sectors per call.
; ----------------------------------------------------------------------------
%define CHUNK_MAX 64
read_chunked_lba:
.loop:
    test  ecx, ecx
    jz    .done
    push  ecx
    cmp   ecx, CHUNK_MAX
    jbe   .small
    mov   ecx, CHUNK_MAX
.small:
    mov   [dap_count_w], cx
    mov   word [dap_dest_off], 0
    mov   [dap_dest_seg], dx
    mov   [dap_lba_lo], eax
    mov   dword [dap_lba_hi], 0
    pusha
    call  read_via_dap
    jc    .err_popa
    popa
    movzx ebx, cx
    add   eax, ebx
    shl   ebx, 5
    add   dx, bx
    pop   ebx
    sub   ebx, ecx
    mov   ecx, ebx
    jmp   .loop
.err_popa:
    popa
    pop   ebx
    stc
    ret
.done:
    clc
    ret

; ----------------------------------------------------------------------------
; read_chunked_chs — INT 13h AH=02h, 1 sector per call (slow but portable).
; ----------------------------------------------------------------------------
read_chunked_chs:
.loop:
    test  ecx, ecx
    jz    .done
    push  eax
    push  ecx
    push  dx
    call  lba_to_chs
    pop   dx
    mov   ah, 0x02
    mov   al, 1
    mov   ch, [chs_cyl_low]
    mov   cl, [chs_sec_cyl_hi]
    mov   dh, [chs_head]
    push  dx                      ; save dest seg
    mov   dl, [boot_drive]
    pop   bx                      ; bx = dest seg
    push  es
    mov   es, bx
    xor   bx, bx
    int   0x13
    pop   es
    pop   ecx
    pop   eax
    jc    .err
    inc   eax
    dec   ecx
    add   dx, 32                  ; advance 512 bytes = 32 paragraphs
    jmp   .loop
.done:
    clc
    ret
.err:
    stc
    ret

; ----------------------------------------------------------------------------
; lba_to_chs — convert EAX (LBA) → CHS bytes in chs_* vars.
; ----------------------------------------------------------------------------
lba_to_chs:
    xor   edx, edx
    movzx ebx, word [bios_spt]
    div   ebx
    mov   cl, dl
    inc   cl                      ; sector 1-based
    xor   edx, edx
    movzx ebx, word [bios_heads]
    div   ebx
    mov   [chs_head], dl
    mov   [chs_cyl_low], al
    shr   eax, 8
    shl   al, 6
    or    cl, al
    mov   [chs_sec_cyl_hi], cl
    ret

; ----------------------------------------------------------------------------
; trace — AL = char. Print to VGA top row (continuing MBR/VBR cursor) + COM1.
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

fatal:
    mov   al, '!'
    call  trace
    mov   al, 'R'
    call  trace
halt:
    cli
    hlt
    jmp   halt

; ============================================================================
; Data
; ============================================================================
align 4

boot_drive:        db 0
partition_lba:     dd 0
has_lba:           db 0
bios_heads:        dw 0
bios_spt:          dw 0
chs_head:          db 0
chs_cyl_low:       db 0
chs_sec_cyl_hi:    db 0

kernel_cluster:    dw 0
kernel_size:       dd 0
kernel_load_lin:   dd 0

kernel_name:       db 'KERNEL  BIN'

fat_start_lba:     dd 0
root_start_lba:    dd 0
data_start_lba:    dd 0
root_dir_sectors:  dd 0

align 4
dap:
    db 0x10
    db 0
dap_count_w:    dw 0
dap_dest_off:   dw 0
dap_dest_seg:   dw 0
dap_lba_lo:     dd 0
dap_lba_hi:     dd 0

; ----------------------------------------------------------------------------
; BPB copy — same order as on-disk BPB starting at offset 11
; ----------------------------------------------------------------------------
bpb_copy:
bps_lc:            dw 0
spc_lc:            db 0
reserved_lc:       dw 0
num_fats_lc:       db 0
root_entries_lc:   dw 0
small_total_lc:    dw 0
media_lc:          db 0
fat16_spf_lc:      dw 0
spt_lc:            dw 0
heads_lc:          dw 0
hidden_lc:         dd 0
large_total_lc:    dd 0
drive_num_lc:      db 0
reserved2_lc:      db 0
ext_sig_lc:        db 0
volume_id_lc:      dd 0
volume_label_lc:   times 11 db 0
fs_type_lc:        times 8  db 0

align 4
pm_jump_far:
pm_target:    dd 0
pm_sel:       dw SEL_CODE32

align 8
gdt_start:
    dq 0                          ; 0x00 null

    ; 0x08 kernel code 32-bit flat ring0
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

    ; 0x10 kernel data 32-bit flat ring0
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

    ; 0x18 user code ring3
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b
    db 11001111b
    db 0x00

    ; 0x20 user data ring3
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b
    db 11001111b
    db 0x00

    ; 0x28 TSS slot
    dq 0

    ; 0x30 16-bit code
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 00000000b
    db 0x00

    ; 0x38 16-bit data
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 00000000b
    db 0x00
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd 0

; ============================================================================
; Pad to 32 sectors (16 KB) so the image builder doesn't need to round.
; ============================================================================
times (32*512) - ($ - $$) db 0
