; setjmp.asm -- minimal setjmp/longjmp
;
; setjmp saves callee-saved registers + return address
; longjmp restores them and returns to the setjmp call site

section .text

global setjmp
global longjmp

; int setjmp(jmp_buf env)
; env is a uint32_t[6]: EBX, ESI, EDI, EBP, ESP, EIP
setjmp:
    mov eax, [esp+4]     ; eax = &env
    mov [eax+0],  ebx
    mov [eax+4],  esi
    mov [eax+8],  edi
    mov [eax+12], ebp
    lea ecx, [esp+4]     ; ESP at point of return (after popping env arg)
    mov [eax+16], ecx
    mov ecx, [esp]        ; return address
    mov [eax+20], ecx
    xor eax, eax          ; return 0
    ret

; void longjmp(jmp_buf env, int val)
longjmp:
    mov edx, [esp+4]     ; edx = &env
    mov eax, [esp+8]     ; eax = val (return value)
    test eax, eax
    jnz .nonzero
    inc eax               ; if val==0, return 1
.nonzero:
    mov ebx, [edx+0]
    mov esi, [edx+4]
    mov edi, [edx+8]
    mov ebp, [edx+12]
    mov esp, [edx+16]
    jmp [edx+20]          ; jump to saved EIP
