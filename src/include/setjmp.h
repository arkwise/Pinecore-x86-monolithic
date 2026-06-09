#ifndef SETJMP_H
#define SETJMP_H

#include "types.h"

/* Minimal setjmp/longjmp for kernel use.
 * Used by V86 monitor to return from V86 task exit. */

typedef uint32_t jmp_buf[6]; /* EBX, ESI, EDI, EBP, ESP, EIP */

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
