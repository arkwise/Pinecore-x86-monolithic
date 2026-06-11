; PINE.COM — Pinecore launcher for FreeDOS
;
; 16-bit DOS .COM program that:
; 1. Loads KERNEL.BIN into conventional memory
; 2. Saves real-mode state for return-to-DOS
; 3. Enables A20, sets up GDT/IDT/paging
; 4. Transitions to protected mode
; 5. Copies kernel to 1MB (where linker expects it)
; 6. Jumps to kernel entry point
;
; Build: nasm -f bin -o PINE.COM pine.asm

bits 16
org 0x100

KERNEL_LOAD_SEG equ 0x6000   ; Load kernel at segment 0x6000 (linear 0x60000)
                              ; Must be above FreeDOS buffers to avoid DMA clobber
KERNEL_DEST     equ 0x100000  ; Final kernel address (1MB)
KERNEL_MAX_SIZE equ 0x80000   ; Max 512KB kernel

start:
    ; Debug: write 'A' to serial IMMEDIATELY
    mov dx, 0x3F8
    mov al, 'A'
    out dx, al

    ; Save real-mode stack for return-to-DOS
    mov [cs:saved_ss], ss
    mov [cs:saved_sp], sp

    ; Print startup message
    mov dx, msg_start
    mov ah, 0x09
    int 0x21

    ; --- Load KERNEL.BIN from disk ---

    ; Debug: write '1' to serial (about to open file)
    mov al, '1'
    mov dx, 0x3F8
    out dx, al

    ; Open file
    mov dx, kernel_filename
    mov ax, 0x3D00          ; Open, read-only
    int 0x21
    jc file_error
    mov [file_handle], ax

    ; Debug: write '2' to serial (file opened)
    mov al, '2'
    mov dx, 0x3F8
    out dx, al

    ; Read kernel using INT 15h/87h (BIOS block move to extended memory)
    ; Actually, simpler: read into a small buffer here, then use
    ; rep movsw to copy to the load segment. This avoids DS issues.
    mov word [bytes_loaded], 0
    mov word [bytes_loaded+2], 0
    mov word [load_off], 0
    mov ax, KERNEL_LOAD_SEG
    mov [load_seg], ax

read_loop:
    ; Read 512 bytes into our local buffer (DS:read_buf)
    mov bx, [file_handle]
    mov dx, read_buf
    mov cx, 512
    mov ah, 0x3F
    int 0x21
    jc read_error

    ; AX = bytes read
    cmp ax, 0
    je read_done

    ; Copy from read_buf to load_seg:load_off
    push es
    push ds
    pop es                  ; ES = DS (our segment, for movsb source calc)
    ; Actually: source=DS:read_buf, dest=load_seg:load_off
    mov cx, ax              ; bytes to copy
    mov si, read_buf        ; source offset in DS
    push ax                 ; save byte count
    mov ax, [load_seg]
    mov es, ax              ; ES = destination segment
    mov di, [load_off]      ; DI = destination offset
    rep movsb
    pop ax
    pop es

    ; Track total
    add [bytes_loaded], ax
    adc word [bytes_loaded+2], 0

    ; Advance destination offset
    add [load_off], ax
    ; If offset >= 0x8000, advance segment
    cmp word [load_off], 0x8000
    jb read_loop
    mov ax, [load_off]
    shr ax, 4
    add [load_seg], ax
    mov word [load_off], 0
    jmp read_loop

read_done:
    ; Close file
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21

    ; Print size
    mov dx, msg_loaded
    mov ah, 0x09
    int 0x21

    ; Debug: dump bytes_loaded as hex
    mov dx, 0x3F8
    mov al, 'B'
    out dx, al
    ; Print bytes_loaded (4 bytes as hex)
    mov eax, [bytes_loaded]
    ; Output high word then low word
    push eax
    shr eax, 12
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .bh1
    add al, 7
.bh1: out dx, al
    pop eax
    push eax
    shr eax, 8
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .bh2
    add al, 7
.bh2: out dx, al
    pop eax
    push eax
    shr eax, 4
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .bh3
    add al, 7
.bh3: out dx, al
    pop eax
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .bh4
    add al, 7
.bh4: out dx, al
    mov al, ' '
    out dx, al

    ; --- Enable A20 gate ---
    call enable_a20

    ; Print ready message
    mov dx, msg_switching
    mov ah, 0x09
    int 0x21

    ; Debug: write 'C' to serial (A20 done)
    mov al, 'C'
    mov dx, 0x3F8
    out dx, al

    ; --- Save return-to-DOS info at fixed physical addresses ---
    ; The kernel reads these to find the exit routine.
    ; 0x500 = COM segment, 0x504 = linear addr of return_to_dos
    ; 0x508 = linear addr of gdt_ptr, 0x50C = linear addr of return_pm16
    ; 0x510 = COM segment:offset for return_rm far jump
    cli
    push es
    xor ax, ax
    mov es, ax
    mov [es:0x500], cs              ; COM segment

    ; Linear addr of return_to_dos = CS*16 + return_to_dos
    xor eax, eax
    mov ax, cs
    shl eax, 4
    push eax                         ; save COM base
    add eax, return_to_dos
    mov [es:0x504], eax

    ; Linear addr of gdt_ptr (already computed above in gdt_ptr+2)
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, gdt_ptr
    mov [es:0x508], eax

    ; Linear addr of return_pm16
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, return_pm16
    mov [es:0x50C], eax

    ; Far ptr for return_rm: offset=return_rm, segment=CS
    mov word [es:0x510], return_rm
    mov ax, cs
    mov [es:0x512], ax

    pop eax                          ; discard COM base
    pop es

    ; --- Save real-mode IVT ---
    ; We'll need it for return-to-DOS
    xor ax, ax
    mov ds, ax
    mov si, 0                ; Source: IVT at 0000:0000
    push cs
    pop es
    mov di, saved_ivt        ; Dest: our save area
    mov cx, 256*2            ; 256 vectors * 2 words each
    rep movsw

    ; Restore DS
    push cs
    pop ds

    ; Save PIC masks
    in al, 0x21
    mov [saved_pic1], al
    in al, 0xA1
    mov [saved_pic2], al

    ; Debug: write 'D' to serial (IVT/PIC saved)
    mov al, 'D'
    mov dx, 0x3F8
    out dx, al

    ; --- Set up GDT ---
    ; GDT is in our .COM segment, already defined below
    ; Calculate linear address of GDT
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, gdt_start
    mov [gdt_ptr + 2], eax

    ; --- Set up page directory and page tables ---
    ; Use memory at 0x90000 for page structures (below BIOS area)
    ; Page directory at 0x90000, page table 0 at 0x91000
    call setup_paging

    ; --- Transition to Protected Mode ---

    ; Calculate linear address of pm_entry for the far jump
    ; (.COM offsets are relative to CS, but PM uses linear addresses)
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, pm_entry
    mov [pm_jump_target], eax

    ; Debug: write 'E' to serial (paging + GDT ready)
    mov al, 'E'
    mov dx, 0x3F8
    out dx, al

    ; Load GDT
    lgdt [gdt_ptr]

    ; Debug: write 'F' to serial (GDT loaded)
    mov al, 'F'
    mov dx, 0x3F8
    out dx, al

    ; Set PE bit
    mov eax, cr0
    or al, 1
    mov cr0, eax

    ; Far jump to 32-bit code — uses indirect jump with patched address
    ; Encoding: opcode 0x66 0xFF /5 (JMP m16:32)
    o32 jmp far [pm_jump_addr]

    ; --- Error handlers ---
file_error:
    mov dx, msg_file_err
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

read_error:
    mov dx, msg_read_err
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

pm_jump_addr:
pm_jump_target: dd 0        ; linear address (filled at runtime)
pm_jump_sel:    dw 0x08     ; kernel code selector

; ================================================================
; A20 gate enable — fast method (port 0x92)
; ================================================================
enable_a20:
    in al, 0x92
    test al, 2
    jnz a20_done       ; Already enabled
    or al, 2
    and al, 0xFE        ; Don't reset machine (bit 0)
    out 0x92, al
a20_done:
    ret

; ================================================================
; Set up identity-mapped paging for first 4MB
; Page directory at 0x90000, page table at 0x91000
; ================================================================
setup_paging:
    ; Clear page directory (4KB)
    push es
    mov ax, 0x9000
    mov es, ax
    xor di, di
    xor eax, eax
    mov cx, 1024
    rep stosd

    ; First page directory entry → page table at 0x91000
    mov dword [es:0], 0x91000 | 7  ; Present, R/W, User

    ; Fill page table 0: identity-map first 4MB (1024 * 4KB pages)
    mov ax, 0x9100
    mov es, ax
    xor di, di
    mov eax, 7              ; First page: 0x00000 | Present, R/W, User
    mov cx, 1024
.fill_pt:
    stosd
    add eax, 0x1000         ; Next 4KB page
    loop .fill_pt

    pop es
    ret

; ================================================================
; 32-bit Protected Mode Entry
; ================================================================
bits 32

pm_entry:
    ; Debug: write 'G' to serial (WE'RE IN PM!)
    mov dx, 0x3F8
    mov al, 'G'
    out dx, al

    ; Load data segments
    mov ax, 0x10            ; Flat data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000        ; Temporary stack below page tables

    ; Debug: write 'H' (segments loaded)
    mov dx, 0x3F8
    mov al, 'H'
    out dx, al

    ; Enable paging
    mov eax, 0x90000        ; Page directory address
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000      ; Set PG bit
    mov cr0, eax

    ; Flush TLB
    mov eax, cr3
    mov cr3, eax

    ; Debug: write 'I' (paging enabled)
    mov dx, 0x3F8
    mov al, 'I'
    out dx, al

    ; --- Copy kernel from 0x30000 to 0x100000 (1MB) ---
    mov esi, 0x60000        ; Source: where we loaded KERNEL.BIN
    mov edi, KERNEL_DEST    ; Dest: 1MB
    mov ecx, [0x10000 - 16 + 12]  ; Use bytes_loaded... actually let's just copy max
    mov ecx, KERNEL_MAX_SIZE / 4
    rep movsd

    ; --- Jump to kernel entry point ---
    ; Our kernel's entry is at 1MB + offset from linker script
    ; The multiboot entry in boot.asm starts at _start
    ; We need to call kernel_main directly (skip multiboot header)
    ;
    ; Our linker.ld puts .multiboot first, then .text
    ; boot.asm has: _start (multiboot header, 48 bytes), then start32 (GDT setup)
    ; But we've already set up GDT, so we need to find kernel_main
    ;
    ; Simplest: add a known entry point to our kernel
    ; For now: jump to start of .text section after multiboot header
    ; The multiboot header is 48 bytes, then boot.asm code begins
    ;
    ; Actually, let's jump to the Multiboot entry — it sets up GDT
    ; (redundantly, but harmlessly) and calls kernel_main
    ; Zero BSS region.
    ; BSS start = kernel_dest + flat_binary_size, rounded up to 4KB page.
    ; bytes_loaded (in 16-bit data) has the file size.
    ; Read it from our .COM segment (still accessible via DS set earlier).
    xor eax, eax
    mov ax, [0x60000 - 16 + 8]  ; Can't easily read 16-bit var from PM.
    ; Just use the copied file size: flat binary ends at KERNEL_DEST + size.
    ; Round up to next 4K page boundary, then zero to 0x150000.
    ; SAFE approach: start zeroing at KERNEL_DEST + flat_size (page aligned).
    ; Flat binary can be up to 80KB, so start BSS zeroing at KERNEL_DEST + 0x14000
    ; (covers up to 80KB of code+data, zeros everything after).
    mov edi, KERNEL_DEST + 0x40000
    xor eax, eax
    mov ecx, (0x180000 - KERNEL_DEST - 0x20000) / 4
    rep stosd

    ; Debug: dump start of kernel at 0x30000 (should be multiboot: 02 B0 AD 1B)
    ; Then dump 0x3A000 (should be GDT)
    mov dx, 0x3F8
    mov al, 'S'
    out dx, al
    mov esi, 0x60000     ; start of loaded kernel
    mov ecx, 4           ; just 4 bytes
.dump_loop:
    mov al, [esi]
    ; Output as 2 hex digits
    push eax
    shr al, 4
    add al, '0'
    cmp al, '9'
    jbe .d1
    add al, 7
.d1: out dx, al
    pop eax
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .d2
    add al, 7
.d2: out dx, al
    inc esi
    loop .dump_loop
    mov al, '|'
    out dx, al
    ; Dump 0x38000 (second batch start, should be .rodata)
    mov al, 'R'
    out dx, al
    mov esi, 0x6A000    ; GDT should be at offset 0xA000 from load base
    mov ecx, 8
.dump2:
    mov al, [esi]
    push eax
    shr al, 4
    add al, '0'
    cmp al, '9'
    jbe .d3
    add al, 7
.d3: out dx, al
    pop eax
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .d4
    add al, 7
.d4: out dx, al
    inc esi
    loop .dump2
    mov al, '|'
    out dx, al

    ; Skip kernel GDT load — PINE.COM's GDT is identical.
    ; The kernel's boot.asm also skips lgdt.
    ; tss_init() will update the GDT entry via gdt_start symbol later.

    ; Debug: write 'J'
    mov dx, 0x3F8
    mov al, 'J'
    out dx, al

    jmp 0x08:KERNEL_DEST + 0x10  ; Skip multiboot header (entry at _start)

    ; Should never reach here
    cli
    hlt

; ================================================================
; Return-to-DOS — PM → 16-bit PM → Real Mode → DOS
;
; Called from kernel. Uses GDT entries 0x30/0x38 (16-bit code/data).
; The kernel stores the linear address of this routine and calls it
; to exit Pinecore and return to the FreeDOS C:\> prompt.
;
; Sequence: disable paging → load 16-bit GDT segments → clear PE →
;           far jmp to real mode → restore IVT/PIC → INT 21h/4Ch
; ================================================================
bits 32
align 16
return_to_dos:
    cli

    ; 1. Disable paging
    mov eax, cr0
    and eax, 0x7FFFFFFF      ; Clear PG
    mov cr0, eax
    xor eax, eax
    mov cr3, eax              ; Flush TLB

    ; 2. Load 16-bit data segment (GDT selector 0x38)
    mov ax, 0x38
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; 3. Far jump to 16-bit PM code (GDT selector 0x30)
    ;    Uses indirect far jump — address stored at 0x50C during init
    mov dword [0x520], 0     ; temp: will load from 0x50C
    mov eax, [0x50C]          ; linear addr of return_pm16
    mov [0x520], eax
    mov word [0x524], 0x30    ; 16-bit code selector
    jmp far [0x520]           ; indirect far jump to 16-bit PM

bits 16
align 16
return_pm16:
    ; Now in 16-bit protected mode with base=0 segments
    ; 4. Clear PE bit
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax

    ; 5. Far jump to real mode — uses stored segment:offset at 0x510
    jmp far [0x510]           ; indirect far jump: offset=0x510, segment=0x512

return_rm:
    ; 6. Now in real mode! Restore segment registers
    mov ax, cs
    mov ds, ax
    mov es, ax

    ; 7. Load real-mode IDT (IVT at 0000:0000, limit 0x3FF)
    lidt [rm_idt_ptr]

    ; 8. Restore saved IVT
    push ds
    xor ax, ax
    mov es, ax
    xor di, di
    push cs
    pop ds
    mov si, saved_ivt
    mov cx, 512
    rep movsw
    pop ds

    ; 9. Remap PIC back to BIOS defaults (IRQ 0-7 → INT 08h-0Fh)
    mov al, 0x11
    out 0x20, al              ; ICW1 master
    out 0xA0, al              ; ICW1 slave
    mov al, 0x08
    out 0x21, al              ; ICW2 master: IRQ 0 → INT 08h
    mov al, 0x70
    out 0xA1, al              ; ICW2 slave: IRQ 8 → INT 70h
    mov al, 0x04
    out 0x21, al              ; ICW3 master: slave on IRQ 2
    mov al, 0x02
    out 0xA1, al              ; ICW3 slave: cascade identity
    mov al, 0x01
    out 0x21, al              ; ICW4: 8086 mode
    out 0xA1, al

    ; 10. Restore saved PIC masks
    mov al, [saved_pic1]
    out 0x21, al
    mov al, [saved_pic2]
    out 0xA1, al

    ; 11. Restore stack
    mov ss, [saved_ss]
    mov sp, [saved_sp]

    ; 12. Enable interrupts
    sti

    ; 13. Print goodbye
    mov dx, msg_returned
    mov ah, 0x09
    int 0x21

    ; 14. Exit to DOS
    mov ax, 0x4C00
    int 0x21

rm_idt_ptr:
    dw 0x03FF                 ; limit (256 * 4 - 1)
    dd 0x00000000             ; base = 0

msg_returned: db 'Returned to FreeDOS.', 13, 10, '$'

bits 16

; ================================================================
; Data
; ================================================================

kernel_filename:  db 'KERNEL.BIN', 0
file_handle:      dw 0
bytes_loaded:     dd 0
load_seg:         dw 0
load_off:         dw 0

saved_ss:         dw 0
saved_sp:         dw 0
saved_pic1:       db 0
saved_pic2:       db 0

msg_start:        db 'Pinecore Kernel Loader v0.2.0.a', 13, 10
                  db 'Loading KERNEL.BIN...', 13, 10, '$'
msg_loaded:       db 'Kernel loaded.', 13, 10, '$'
msg_switching:    db 'Switching to protected mode...', 13, 10, '$'
msg_file_err:     db 'Error: KERNEL.BIN not found!', 13, 10, '$'
msg_read_err:     db 'Error: Failed to read KERNEL.BIN!', 13, 10, '$'

; ================================================================
; GDT — same layout as our kernel's boot.asm
; ================================================================
align 8
gdt_start:
    ; Entry 0: Null descriptor
    dq 0

    ; Entry 1 (0x08): Kernel code — 32-bit, flat, Ring 0
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 10011010b    ; P=1, DPL=0, S=1, Type=Execute/Read
    db 11001111b    ; G=1, D=1, Limit 19:16 = 0xF
    db 0x00         ; Base 31:24

    ; Entry 2 (0x10): Kernel data — 32-bit, flat, Ring 0
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b    ; P=1, DPL=0, S=1, Type=Read/Write
    db 11001111b
    db 0x00

    ; Entry 3 (0x18): User code — 32-bit, flat, Ring 3
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b    ; P=1, DPL=3, S=1, Type=Execute/Read
    db 11001111b
    db 0x00

    ; Entry 4 (0x20): User data — 32-bit, flat, Ring 3
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b    ; P=1, DPL=3, S=1, Type=Read/Write
    db 11001111b
    db 0x00

    ; Entry 5 (0x28): TSS — filled by kernel later
    dq 0

    ; Entry 6 (0x30): 16-bit code — base=0, limit=0xFFFF, Ring 0
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 10011010b    ; P=1, DPL=0, S=1, Type=Execute/Read
    db 00000000b    ; G=0, D=0 (16-bit), Limit 19:16 = 0
    db 0x00         ; Base 31:24

    ; Entry 7 (0x38): 16-bit data — base=0, limit=0xFFFF, Ring 0
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b    ; P=1, DPL=0, S=1, Type=Read/Write
    db 00000000b    ; G=0, D=0 (16-bit)
    db 0x00
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1  ; Limit
    dd 0                         ; Base (filled at runtime)

; ================================================================
; Read buffer (512 bytes for file reading)
; ================================================================
read_buf: times 512 db 0

; ================================================================
; Saved IVT (256 * 4 bytes = 1KB)
; ================================================================
saved_ivt: times 1024 db 0
