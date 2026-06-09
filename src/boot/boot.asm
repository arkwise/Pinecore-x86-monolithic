; boot.asm — Multiboot entry point for QEMU testing
; Real FreeDOS boot stub will replace this later (ch-10)
;
; Sets up stack, loads GDT, jumps to C kernel_main

MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
FLAGS    equ MBALIGN | MEMINFO
MAGIC    equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

; =========================================================
; GDT — Flat model, 4GB segments
; Layout matches ch-10 GDT table (386-bible p.94)
; =========================================================
section .data
align 8
gdt_start:
    ; Null descriptor (index 0, selector 0x00)
    dq 0

    ; Kernel Code (index 1, selector 0x08)
    ; Base=0, Limit=4GB, Execute/Read, Ring 0, 32-bit
    dw 0xFFFF       ; limit[15:0]
    dw 0x0000       ; base[15:0]
    db 0x00         ; base[23:16]
    db 10011010b    ; P=1, DPL=00, S=1, Type=1010 (code, exec/read)
    db 11001111b    ; G=1, D=1, 0, 0, limit[19:16]=F
    db 0x00         ; base[31:24]

    ; Kernel Data (index 2, selector 0x10)
    ; Base=0, Limit=4GB, Read/Write, Ring 0, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b    ; P=1, DPL=00, S=1, Type=0010 (data, read/write)
    db 11001111b
    db 0x00

    ; User Code (index 3, selector 0x18) — for V86 tasks later
    ; Base=0, Limit=4GB, Execute/Read, Ring 3, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b    ; P=1, DPL=11, S=1, Type=1010
    db 11001111b
    db 0x00

    ; User Data (index 4, selector 0x20) — for V86 tasks later
    ; Base=0, Limit=4GB, Read/Write, Ring 3, 32-bit
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b    ; P=1, DPL=11, S=1, Type=0010
    db 11001111b
    db 0x00

    ; TSS entry (index 5, selector 0x28) — filled in by C code
    dq 0
gdt_end:

    ; LDT entry (index 6, selector 0x30) — filled in by DPMI host at runtime
    ; Placed AFTER gdt_end so the initial LGDT limit doesn't include it.
    ; dpmi_init will update gdt_ptr limit when loading the LDT.
global gdt_ldt_slot
gdt_ldt_slot:
    dq 0

gdt_ptr:
    dw gdt_end - gdt_start - 1  ; limit
    dd gdt_start                 ; base

; Selectors
KERNEL_CS equ 0x08
KERNEL_DS equ 0x10

; =========================================================
; Entry point
; =========================================================
section .bss
align 16
resb 16384
stack_top:

section .text
global _start
global gdt_start
global gdt_ptr
extern kernel_main

_start:
    ; Debug: write 'K' to serial (we reached the kernel!)
    mov dx, 0x3F8
    mov al, 'K'
    out dx, al

    ; Skip lgdt when booted from PINE.COM — its GDT is identical.
    ; PINE.COM loads the kernel's GDT after copying to 1MB.
    ; For Multiboot (GRUB), the GDT from boot.asm .data works directly.
    ; Just reload data segments and set stack.
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, stack_top

    ; Debug: write 'L'
    mov dx, 0x3F8
    mov al, 'L'
    out dx, al

    ; Call C entry point
    call kernel_main

    ; Should never return, but just in case
.hang:
    cli
    hlt
    jmp .hang
