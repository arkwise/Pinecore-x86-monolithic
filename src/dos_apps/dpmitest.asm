; DPMITEST.COM — Test DPMI mode switch on Pinecore
;
; 1. Detects DPMI via INT 2Fh/1687h
; 2. Allocates private data, calls DPMI entry point
; 3. If successful: now in 16-bit Protected Mode
; 4. Writes "PM!" to serial port (direct OUT — proves we're in PM with IOPL=3)
; 5. Calls INT 21h/09h to print a message (proves IDT routing works)
; 6. Exits via INT 21h/4Ch
;
; Build: nasm -f bin -o DPMITEST.COM dpmitest.asm

bits 16
org 0x100

start:
    ; Print banner via DOS
    mov dx, msg_banner
    mov ah, 0x09
    int 0x21

    ; Step 1: Detect DPMI
    mov ax, 0x1687
    int 0x2F
    test ax, ax
    jnz .no_dpmi

    ; Save entry point and paragraph count
    mov [entry_off], di
    mov [entry_seg], es
    mov [priv_para], si

    ; Print detection message
    push ds
    pop es              ; restore ES (INT 2Fh changed it)
    mov dx, msg_found
    mov ah, 0x09
    int 0x21

    ; Print version info
    mov dx, msg_ver
    mov ah, 0x09
    int 0x21

    ; Step 2: Allocate private data (SI paragraphs)
    mov bx, [priv_para]
    test bx, bx
    jz .skip_alloc
    mov ah, 0x48
    int 0x21
    jc .alloc_fail
    mov es, ax          ; ES = private data segment for DPMI host
.skip_alloc:

    ; Step 3: Enter protected mode
    mov dx, msg_entering
    mov ah, 0x09
    int 0x21

    ; AX=0 for 16-bit client (our COM code is 16-bit)
    mov ax, 0x0000
    call far [entry_off]

    ; If carry set = failure
    jc .switch_fail

    ; =============================================
    ; WE ARE NOW IN 16-BIT PROTECTED MODE!
    ; CS = LDT code selector
    ; DS = LDT data selector
    ; Segment bases = original real-mode segment * 16
    ; =============================================

    ; Write "PM!" to serial port (COM1 = 0x3F8)
    ; This proves: (a) we're in PM, (b) IOPL=3 allows port I/O
    mov dx, 0x3F8
    mov al, 'P'
    out dx, al
    mov al, 'M'
    out dx, al
    mov al, '!'
    out dx, al
    mov al, 10
    out dx, al

    ; Print success via INT 21h/09h
    ; In PM, DS is an LDT selector. Our data is still at the same
    ; offsets because the LDT base matches our original segment.
    mov dx, msg_pm_ok
    mov ah, 0x09
    int 0x21

    ; Exit cleanly
    mov ax, 0x4C00
    int 0x21

; ---- Error paths (still in V86/real mode) ----

.no_dpmi:
    mov dx, msg_no_dpmi
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

.alloc_fail:
    mov dx, msg_no_alloc
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

.switch_fail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

; ---- Data ----
entry_off:  dw 0
entry_seg:  dw 0
priv_para:  dw 0

msg_banner:   db 'DPMITEST — Pinecore DPMI Test', 13, 10, '$'
msg_found:    db 'DPMI 0.9 host found!', 13, 10, '$'
msg_ver:      db 'Requesting 16-bit PM entry...', 13, 10, '$'
msg_entering: db 'Calling DPMI mode switch...', 13, 10, '$'
msg_pm_ok:    db '*** PROTECTED MODE ACTIVE ***', 13, 10
              db 'INT 21h works from PM!', 13, 10, '$'
msg_no_dpmi:  db 'No DPMI host detected.', 13, 10, '$'
msg_no_alloc: db 'Cannot allocate DPMI data.', 13, 10, '$'
msg_fail:     db 'DPMI mode switch FAILED!', 13, 10, '$'
