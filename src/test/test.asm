; test.com -- Simple DOS COM program for testing our V86 monitor
; Assemble: nasm -f bin -o TEST.COM test.asm
;
; Prints a message using INT 21h/09h and exits with INT 21h/4Ch

org 0x100

    ; Print hello message using INT 21h/09h
    mov ah, 09h
    mov dx, msg
    int 21h

    ; Print via INT 21h/40h (write to stdout)
    mov ah, 40h
    mov bx, 1
    mov cx, msg2_len
    mov dx, msg2
    int 21h

    ; Exit with code 0
    mov ax, 4C00h
    int 21h

msg  db 'Hello from TEST.COM!', 0Dh, 0Ah, '$'
msg2 db 'Write handle OK', 0Dh, 0Ah
msg2_len equ $ - msg2
