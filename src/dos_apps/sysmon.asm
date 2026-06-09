; SYSMON.COM — DOS Real-Mode System Monitor
; Simple text-mode display using INT 21h only.
; Press Q or ESC to quit.
;
; Build: nasm -f bin -o SYSMON.COM sysmon.asm

bits 16
org 0x100

start:
main_loop:
    ; Clear screen via INT 10h mode set
    mov ax, 0x0003
    int 0x10

    ; Print header
    mov dx, hdr
    mov ah, 0x09
    int 0x21

    ; Read uptime from BIOS data area (0040:006C), updated by PIT at ~20Hz
    push es
    mov ax, 0x0040
    mov es, ax
    mov ax, [es:0x006C]   ; low 16 bits of tick counter
    pop es
    ; ticks / 18 ≈ seconds
    xor dx, dx
    mov cx, 18
    div cx                ; ax = seconds
    push ax
    xor dx, dx
    mov cx, 60
    div cx                ; ax=min, dx=sec
    call print_dec
    mov dl, 'm'
    mov ah, 0x02
    int 0x21
    mov dl, ' '
    int 0x21
    pop ax
    xor dx, dx
    mov cx, 60
    div cx
    mov ax, dx
    call print_dec
    mov dl, 's'
    mov ah, 0x02
    int 0x21

    ; Memory bar
    mov dx, mem_hdr
    mov ah, 0x09
    int 0x21

    mov cx, 30           ; bar width
    mov ax, [frame]
    and ax, 15
    add ax, 15           ; 15-30 filled
    call print_bar

    ; CPU bar
    mov dx, cpu_hdr
    mov ah, 0x09
    int 0x21

    mov cx, 30
    mov ax, [frame]
    shr ax, 2
    and ax, 15
    add ax, 5            ; 5-20 filled
    call print_bar

    ; Fake task list
    mov dx, taskhdr
    mov ah, 0x09
    int 0x21

    ; Spinner
    mov bx, [frame]
    and bx, 3
    mov al, [spinner + bx]
    mov dl, al
    mov ah, 0x02
    int 0x21

    ; Footer
    mov dx, footer
    mov ah, 0x09
    int 0x21

    inc word [frame]

    ; Check for Q key
    mov ah, 0x0B
    int 0x21
    cmp al, 0xFF
    jne main_loop
    mov ah, 0x08
    int 0x21
    cmp al, 'q'
    je .quit
    cmp al, 'Q'
    je .quit
    cmp al, 27
    je .quit
    jmp main_loop

.quit:
    mov ax, 0x0003
    int 0x10
    mov ax, 0x4C00
    int 0x21

; ================================================================
; Print AX as decimal
print_dec:
    push bx
    push cx
    push dx
    mov cx, 0
    mov bx, 10
.loop:
    xor dx, dx
    div bx
    push dx
    inc cx
    or ax, ax
    jnz .loop
.out:
    pop dx
    add dl, '0'
    mov ah, 0x02
    int 0x21
    loop .out
    pop dx
    pop cx
    pop bx
    ret

; Print progress bar: AX=filled, CX=total width
print_bar:
    push ax
    push cx
    mov bx, ax          ; save filled count BEFORE int 21h clobbers AX
    mov dl, '['
    mov ah, 0x02
    int 0x21
    xor si, si
.lp:
    cmp si, cx
    jge .done
    cmp si, bx
    jge .empty
    mov dl, '|'
    jmp .wr
.empty:
    mov dl, ' '
.wr:
    mov ah, 0x02
    int 0x21
    inc si
    jmp .lp
.done:
    mov dl, ']'
    mov ah, 0x02
    int 0x21
    pop cx
    pop ax
    ret

; ================================================================
; Data
frame: dw 0
delay_outer: dw 0
spinner: db '|/-\'

hdr: db 13,10
     db '  ===================================',13,10
     db '   DOS System Monitor  (SYSMON.COM)',13,10
     db '  ===================================',13,10
     db 13,10
     db '  Uptime: $'

mem_hdr: db 13,10,13,10
         db '  Memory  $'

cpu_hdr: db 13,10
         db '  CPU     $'

taskhdr: db 13,10,13,10
         db '  ---- Process List ----',13,10
         db '  PID  NAME            STATE',13,10
         db '  ---  ----            -----',13,10
         db '  001  COMMAND.COM     RUN',13,10
         db '  002  SYSMON.COM      RUN',13,10
         db '  003  (idle)          WAIT',13,10
         db 13,10
         db '  Status: $'

footer: db 13,10,13,10
        db '  Real Mode DOS | 640K | No Protected Mode',13,10
        db '  Press Q to quit',13,10
        db '$'
