; Multiboot header — lets QEMU boot our kernel directly with -kernel flag
; This is just for toolchain testing, NOT the real FreeDOS boot stub

MBALIGN  equ 1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ 1 << 1            ; provide memory map
FLAGS    equ MBALIGN | MEMINFO
MAGIC    equ 0x1BADB002        ; multiboot magic number
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 16
resb 16384                     ; 16KB stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top         ; set up stack
    call kernel_main           ; call C entry point
.hang:
    cli
    hlt
    jmp .hang
