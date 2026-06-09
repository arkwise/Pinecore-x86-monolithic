; isr_stubs.asm -- Interrupt service routine entry points
;
; CPU exceptions 0-31: some push error codes, some don't.
; We normalize by pushing a dummy error code for those that don't.
; IRQs 0-15 (INT 32-47): never push error codes.
;
; All stubs jump to a common handler that saves registers,
; calls the C dispatcher, restores registers, and irets.

section .text

extern isr_dispatch

; sched_switch_to(uint32_t esp) — load a task's stack and IRET into it
; Called from sched_start to begin the first task.
; Same restore sequence as isr_common: pop segs, popa, skip 8, iret
global sched_switch_to
sched_switch_to:
    mov esp, [esp+4]    ; load the task's saved ESP (first argument)
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; skip int_no + err_code
    iret

; Macro for exceptions WITHOUT error code
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push dword 0        ; dummy error code
    push dword %1       ; interrupt number
    jmp isr_common
%endmacro

; Macro for exceptions WITH error code (CPU pushes it)
%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push dword %1       ; interrupt number (error code already on stack)
    jmp isr_common
%endmacro

; Macro for IRQ stubs
%macro IRQ_STUB 2
global isr_stub_%1
isr_stub_%1:
    push dword 0        ; dummy error code
    push dword %1       ; interrupt number (%1 = 32+irq)
    jmp isr_common
%endmacro

; CPU exceptions 0-31
ISR_NOERR 0   ; Division by zero
ISR_NOERR 1   ; Debug
ISR_NOERR 2   ; NMI
ISR_NOERR 3   ; Breakpoint
ISR_NOERR 4   ; Overflow
ISR_NOERR 5   ; Bound range exceeded
ISR_NOERR 6   ; Invalid opcode
ISR_NOERR 7   ; Device not available
ISR_ERR   8   ; Double fault
ISR_NOERR 9   ; Coprocessor segment overrun
ISR_ERR   10  ; Invalid TSS
ISR_ERR   11  ; Segment not present
ISR_ERR   12  ; Stack segment fault
ISR_ERR   13  ; General protection fault
ISR_ERR   14  ; Page fault
ISR_NOERR 15  ; Reserved
ISR_NOERR 16  ; x87 FPU error
ISR_ERR   17  ; Alignment check
ISR_NOERR 18  ; Machine check
ISR_NOERR 19  ; SIMD FPU exception
ISR_NOERR 20  ; Virtualization exception
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; IRQs 0-15 -> INT 32-47
IRQ_STUB 32, 0   ; Timer
IRQ_STUB 33, 1   ; Keyboard
IRQ_STUB 34, 2   ; Cascade
IRQ_STUB 35, 3   ; COM2
IRQ_STUB 36, 4   ; COM1
IRQ_STUB 37, 5   ; LPT2
IRQ_STUB 38, 6   ; Floppy
IRQ_STUB 39, 7   ; LPT1 / spurious
IRQ_STUB 40, 8   ; CMOS RTC
IRQ_STUB 41, 9   ; Free
IRQ_STUB 42, 10  ; Free
IRQ_STUB 43, 11  ; Free
IRQ_STUB 44, 12  ; PS/2 Mouse
IRQ_STUB 45, 13  ; FPU
IRQ_STUB 46, 14  ; ATA primary
IRQ_STUB 47, 15  ; ATA secondary

; Pinecore network syscall vector — INT 0x80 (DPL=3 gate installed by
; net_init). Caller sets EBX = pointer to struct net_syscall_frame, then
; INT 0x80. Falls through isr_common -> isr_dispatch_inner where n==128
; routes into net_dispatch.
ISR_NOERR 128

; Common handler -- saves all regs, calls C, restores, irets
; isr_dispatch returns new ESP (may be different if context switch happened)
isr_common:
    pusha               ; push EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    push ds
    push es
    push fs
    push gs

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Pass pointer to stack frame as argument
    ; isr_dispatch returns new ESP in EAX (for context switch)
    push esp
    call isr_dispatch
    mov esp, eax        ; EAX = new ESP (same or different task's stack)

    pop gs
    pop fs
    pop es
    pop ds
    popa

    add esp, 8          ; pop error code and interrupt number
    iret
