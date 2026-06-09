# V86 Monitor — GPF Handler and Instruction Emulation

> The most complex piece: decoding trapped instructions from V86 tasks and emulating them.

**Date:** 2026-04-28
**Status:** Complete — implementation reference

---

## Findings

### How V86 Trapping Works

When a V86 task executes a sensitive instruction, the CPU generates a General Protection Fault (GPF, exception 13). The CPU:

1. Switches to Ring 0 (loads SS0:ESP0 from TSS)
2. Pushes an exception frame onto the Ring 0 stack
3. Jumps to our GPF handler via IDT entry 13

### GPF Stack Frame (from V86 mode)

When the GPF comes from V86 mode (ring change occurs), the CPU pushes (386-bible p.221):

```
[ESP+0]   EIP      — return address (points AFTER the faulting instruction for INT, AT for others)
[ESP+4]   CS       — V86 code segment
[ESP+8]   EFLAGS   — flags (VM bit will be set, confirming V86 origin)
[ESP+12]  ESP      — V86 stack pointer
[ESP+16]  SS       — V86 stack segment
[ESP+20]  ES       — V86 ES register
[ESP+24]  DS       — V86 DS register
[ESP+28]  FS       — V86 FS register
[ESP+32]  GS       — V86 GS register
```

**Key check:** If EFLAGS bit 17 (VM) is set in the saved EFLAGS, the exception came from V86 mode.

### Decoding the Faulting Instruction

The faulting instruction is at the V86 task's CS:IP. Convert to linear address:

```c
uint32_t fault_addr = (saved_cs << 4) + (saved_eip & 0xFFFF);
uint8_t *code = (uint8_t *)fault_addr;  // Identity-mapped in our page tables
```

Then decode the opcode:

| Opcode | Instruction | Bytes | Action |
|--------|------------|-------|--------|
| 0xCD nn | INT nn | 2 | Emulate interrupt nn |
| 0xFA | CLI | 1 | Clear virtual IF |
| 0xFB | STI | 1 | Set virtual IF |
| 0x9C | PUSHF | 1 | Push virtual flags to V86 stack |
| 0x9D | POPF | 1 | Pop flags from V86 stack |
| 0xCF | IRET | 1 | Pop IP, CS, FLAGS from V86 stack |
| 0xEC | IN AL, DX | 1 | Port input (if I/O bitmap traps it) |
| 0xED | IN AX, DX | 1 | Port input word |
| 0xEE | OUT DX, AL | 1 | Port output |
| 0xEF | OUT DX, AX | 1 | Port output word |
| 0xE4 nn | IN AL, nn | 2 | Port input immediate |
| 0xE5 nn | IN AX, nn | 2 | Port input word immediate |
| 0xE6 nn | OUT nn, AL | 2 | Port output immediate |
| 0xE7 nn | OUT nn, AX | 2 | Port output word immediate |

### The GPF Handler — Main Dispatch

```c
void v86_gpf_handler(struct interrupt_frame *frame) {
    // Verify this came from V86 mode
    if (!(frame->eflags & 0x20000))  // VM bit not set
        panic("GPF from protected mode");

    // Get faulting instruction
    uint32_t code_addr = (frame->cs << 4) + (frame->eip & 0xFFFF);
    uint8_t *code = (uint8_t *)code_addr;

    switch (code[0]) {
        case 0xCD:  // INT nn
            v86_emulate_int(frame, code[1]);
            frame->eip += 2;
            break;

        case 0xFA:  // CLI
            frame->eflags &= ~0x200;  // Clear virtual IF
            frame->eip += 1;
            break;

        case 0xFB:  // STI
            frame->eflags |= 0x200;   // Set virtual IF
            frame->eip += 1;
            break;

        case 0x9C:  // PUSHF
            v86_emulate_pushf(frame);
            frame->eip += 1;
            break;

        case 0x9D:  // POPF
            v86_emulate_popf(frame);
            frame->eip += 1;
            break;

        case 0xCF:  // IRET
            v86_emulate_iret(frame);
            // EIP updated by iret emulation, don't advance
            break;

        case 0xEC: case 0xED:  // IN from DX
        case 0xEE: case 0xEF:  // OUT to DX
            v86_emulate_io(frame, code[0]);
            frame->eip += 1;
            break;

        case 0xE4: case 0xE5:  // IN from imm8
        case 0xE6: case 0xE7:  // OUT to imm8
            v86_emulate_io_imm(frame, code[0], code[1]);
            frame->eip += 2;
            break;

        default:
            panic("Unhandled V86 opcode: 0x%02X", code[0]);
    }
}
```

### Emulating INT nn

This is the most important case — FREECOM's primary I/O mechanism:

```c
void v86_emulate_int(struct interrupt_frame *frame, uint8_t int_num) {
    switch (int_num) {
        case 0x21:  // DOS API
            dos_emulate_int21(frame);
            break;

        case 0x10:  // BIOS Video
            video_emulate_int10(frame);
            break;

        case 0x16:  // BIOS Keyboard
            kbd_emulate_int16(frame);
            break;

        case 0x20:  // DOS Terminate
            v86_task_exit(frame);
            break;

        default:
            // For unhandled INTs: push flags, CS, IP onto V86 stack
            // and redirect to the real IVT entry (if we want pass-through)
            v86_reflect_interrupt(frame, int_num);
            break;
    }
}
```

### Emulating PUSHF

Push the current V86 flags (with virtual IF) onto the V86 stack:

```c
void v86_emulate_pushf(struct interrupt_frame *frame) {
    uint32_t stack_addr = (frame->ss << 4) + ((frame->esp - 2) & 0xFFFF);
    uint16_t flags = (uint16_t)(frame->eflags & 0xFFFF);
    *(uint16_t *)stack_addr = flags;
    frame->esp = (frame->esp - 2) & 0xFFFF;
}
```

### Emulating POPF

Pop flags from V86 stack, but protect Ring 0 bits:

```c
void v86_emulate_popf(struct interrupt_frame *frame) {
    uint32_t stack_addr = (frame->ss << 4) + (frame->esp & 0xFFFF);
    uint16_t flags = *(uint16_t *)stack_addr;
    frame->esp = (frame->esp + 2) & 0xFFFF;

    // Preserve VM, IOPL, and other protected bits
    // Only allow V86 task to change: CF, PF, AF, ZF, SF, TF, IF, DF, OF
    uint32_t mask = 0x0000FD5;  // Bits the V86 task can modify
    frame->eflags = (frame->eflags & ~mask) | (flags & mask);
}
```

### Emulating IRET

Pop IP, CS, and FLAGS from the V86 stack:

```c
void v86_emulate_iret(struct interrupt_frame *frame) {
    uint32_t stack_addr = (frame->ss << 4) + (frame->esp & 0xFFFF);
    uint16_t *stack = (uint16_t *)stack_addr;

    frame->eip = stack[0];                          // Pop IP
    frame->cs = stack[1];                           // Pop CS
    uint16_t new_flags = stack[2];                  // Pop FLAGS
    frame->esp = (frame->esp + 6) & 0xFFFF;

    // Same protection as POPF
    uint32_t mask = 0x0000FD5;
    frame->eflags = (frame->eflags & ~mask) | (new_flags & mask);
}
```

### Reflecting Interrupts (Pass-Through)

For INTs we don't handle, push a fake IRET frame onto the V86 stack and redirect to the real IVT:

```c
void v86_reflect_interrupt(struct interrupt_frame *frame, uint8_t int_num) {
    // Push flags, CS, IP onto V86 stack (like a real INT would)
    uint32_t stack_addr = (frame->ss << 4) + ((frame->esp - 6) & 0xFFFF);
    uint16_t *stack = (uint16_t *)stack_addr;
    stack[0] = frame->eip & 0xFFFF;       // Return IP
    stack[1] = frame->cs & 0xFFFF;         // Return CS
    stack[2] = frame->eflags & 0xFFFF;     // Return FLAGS
    frame->esp = (frame->esp - 6) & 0xFFFF;

    // Read real IVT entry (first 1024 bytes of memory)
    uint16_t *ivt = (uint16_t *)(int_num * 4);
    frame->eip = ivt[0];     // IP from IVT
    frame->cs = ivt[1];      // CS from IVT
}
```

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| 386 Bible Ch.15 | i386-bible/pages/page_0217-0223 | V86 GPF trapping, sensitive instructions |
| CWSDPMI tables.asm | cwsdpmi-master/src/tables.asm:100-126 | Exception frame parsing, INT decoding |
| CWSDPMI exphdlr.c | cwsdpmi-master/src/exphdlr.c | GPF handling, virtual IF management |
| CWSDPMI dpmisim.asm | cwsdpmi-master/src/dpmisim.asm | Real-mode interrupt simulation patterns |

---

*Last updated: 2026-04-28*
