# Hardware Drivers — PIC, PS/2 Keyboard, PS/2 Mouse, PIT Timer

> All four drivers we need to own at Ring 0. No BIOS, no DOS, no reentrancy.

**Date:** 2026-04-28
**Status:** Complete — register-level reference for implementation

---

## 1. PIC (8259A Programmable Interrupt Controller)

### Ports

| Port | PIC | Purpose |
|------|-----|---------|
| 0x20 | Master | Command |
| 0x21 | Master | Data (IMR) |
| 0xA0 | Slave | Command |
| 0xA1 | Slave | Data (IMR) |

### Remapping Sequence (IRQ 0-15 → INT 32-47)

The default PC mapping puts IRQ 0-7 at INT 0-7, which collides with CPU exceptions. Must remap:

```c
void pic_remap(void) {
    // ICW1: Start initialization (both PICs)
    outb(0x20, 0x11);   // Master: init + ICW4 needed
    outb(0xA0, 0x11);   // Slave: init + ICW4 needed

    // ICW2: Set vector offsets
    outb(0x21, 0x20);   // Master: IRQ 0-7 → INT 32-39
    outb(0xA1, 0x28);   // Slave: IRQ 8-15 → INT 40-47

    // ICW3: Set cascade
    outb(0x21, 0x04);   // Master: slave on IRQ 2 (bit 2)
    outb(0xA1, 0x02);   // Slave: connected to master IRQ 2

    // ICW4: Set 8086 mode
    outb(0x21, 0x01);   // Master: 8086 mode
    outb(0xA1, 0x01);   // Slave: 8086 mode

    // Mask all interrupts initially (unmask as drivers are installed)
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}
```

### EOI (End of Interrupt)

```c
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(0xA0, 0x20);  // Slave EOI (for IRQ 8-15)
    outb(0x20, 0x20);      // Master EOI (always)
}
```

### Masking/Unmasking

```c
void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t bit = (irq < 8) ? irq : irq - 8;
    outb(port, inb(port) & ~(1 << bit));
}
```

### IRQ Assignment After Remap

| IRQ | INT | Device | Our Driver |
|-----|-----|--------|-----------|
| 0 | 32 | PIT Timer | Scheduler + GUI timer |
| 1 | 33 | PS/2 Keyboard | Keyboard driver |
| 2 | 34 | Cascade (slave PIC) | — |
| 12 | 44 | PS/2 Mouse | Mouse driver |
| 14 | 46 | Primary IDE | Disk driver |
| 15 | 47 | Secondary IDE | Disk driver |

---

## 2. PIT Timer (8254 Programmable Interval Timer)

### Ports

| Port | Purpose |
|------|---------|
| 0x40 | Channel 0 data (system timer, connected to IRQ 0) |
| 0x41 | Channel 1 data (DRAM refresh, don't touch) |
| 0x42 | Channel 2 data (PC speaker) |
| 0x43 | Command register |

### Base Frequency

PIT oscillator = **1,193,182 Hz**. Divider = oscillator / desired_frequency.

### Initialization

```c
void pit_init(uint32_t frequency_hz) {
    uint16_t divisor = 1193182 / frequency_hz;

    // Command: channel 0, access mode lo/hi, mode 3 (square wave), binary
    outb(0x43, 0x36);

    // Send divisor (low byte first, then high byte)
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

// For 100 Hz (10ms ticks): divisor = 11932
// For 1000 Hz (1ms ticks): divisor = 1193
```

### Timer ISR (IRQ 0 → INT 32)

```c
void timer_isr(void) {
    tick_count++;

    // Preemptive scheduling: save current task, pick next, restore
    task_switch_if_needed();

    // GUI timer callbacks (Allegro-style)
    run_timer_callbacks();

    pic_send_eoi(0);
}
```

---

## 3. PS/2 Keyboard Driver

### Ports

| Port | Purpose |
|------|---------|
| 0x60 | Data (read scancode, write commands to keyboard) |
| 0x64 | Status (read) / Command (write to 8042 controller) |

### Status Register (port 0x64 read)

| Bit | Meaning |
|-----|---------|
| 0 | Output buffer full (1 = data ready to read from 0x60) |
| 1 | Input buffer full (1 = controller busy, wait before writing) |
| 5 | Auxiliary data (1 = mouse data, 0 = keyboard data) |

### Keyboard ISR (IRQ 1 → INT 33)

```c
// Global state
uint8_t key_state[256];  // 1 = pressed, 0 = released
uint8_t shift_state;     // bit 0=shift, bit 1=ctrl, bit 2=alt

void keyboard_isr(void) {
    uint8_t scancode = inb(0x60);

    if (scancode & 0x80) {
        // Key release (bit 7 set)
        key_state[scancode & 0x7F] = 0;
        update_modifiers(scancode & 0x7F, 0);
    } else {
        // Key press
        key_state[scancode] = 1;
        update_modifiers(scancode, 1);
        enqueue_keypress(scancode);
    }

    pic_send_eoi(1);
}
```

### Scancode Set 1 → ASCII (partial)

```
0x01=Esc  0x02='1' 0x03='2' 0x04='3' 0x05='4' 0x06='5' 0x07='6'
0x08='7'  0x09='8' 0x0A='9' 0x0B='0' 0x0C='-' 0x0D='=' 0x0E=BS
0x0F=Tab  0x10='q' 0x11='w' 0x12='e' 0x13='r' 0x14='t' 0x15='y'
0x16='u'  0x17='i' 0x18='o' 0x19='p' 0x1A='[' 0x1B=']' 0x1C=Enter
0x1D=LCtrl 0x1E='a' 0x1F='s' 0x20='d' 0x21='f' 0x22='g' 0x23='h'
0x24='j'  0x25='k' 0x26='l' 0x27=';' 0x28=''' 0x29='`' 0x2A=LShift
0x2B='\'  0x2C='z' 0x2D='x' 0x2E='c' 0x2F='v' 0x30='b' 0x31='n'
0x32='m'  0x33=',' 0x34='.' 0x35='/' 0x36=RShift 0x38=LAlt 0x39=Space
```

Extended keys: preceded by 0xE0 (arrow keys, Home, End, PgUp, PgDn, Insert, Delete).

### Waiting Before Writing to Controller

```c
void kbd_wait_write(void) {
    while (inb(0x64) & 0x02);  // Wait for input buffer empty
}
```

---

## 4. PS/2 Mouse Driver

### Shared Ports with Keyboard

Same 8042 controller. To send commands to mouse instead of keyboard:
1. Write 0xD4 to port 0x64 (auxiliary device prefix)
2. Write data byte to port 0x60

### Mouse Initialization

```c
void mouse_init(void) {
    // Enable auxiliary device (mouse port)
    kbd_wait_write();
    outb(0x64, 0xA8);       // Enable auxiliary port

    // Enable IRQ 12 for mouse
    kbd_wait_write();
    outb(0x64, 0x20);       // Read controller config
    while (!(inb(0x64) & 0x01));
    uint8_t config = inb(0x60);
    config |= 0x02;          // Enable auxiliary interrupt (bit 1)
    kbd_wait_write();
    outb(0x64, 0x60);       // Write controller config
    kbd_wait_write();
    outb(0x60, config);

    // Send "enable data reporting" to mouse
    mouse_write(0xF4);       // Enable data reporting
    mouse_read_ack();        // Wait for 0xFA acknowledgement
}

void mouse_write(uint8_t data) {
    kbd_wait_write();
    outb(0x64, 0xD4);       // Prefix: next byte goes to mouse
    kbd_wait_write();
    outb(0x60, data);
}
```

### Mouse Packet Format (3 bytes)

```
Byte 0 (status):
  Bit 0: Left button   (1 = pressed)
  Bit 1: Right button  (1 = pressed)
  Bit 2: Middle button (1 = pressed)
  Bit 3: Always 1
  Bit 4: X sign        (1 = negative movement)
  Bit 5: Y sign        (1 = negative movement)
  Bit 6: X overflow
  Bit 7: Y overflow

Byte 1: X delta (signed 8-bit)
Byte 2: Y delta (signed 8-bit)
```

### Mouse ISR (IRQ 12 → INT 44)

```c
static int mouse_cycle = 0;
static uint8_t mouse_packet[3];
int mouse_x = 0, mouse_y = 0, mouse_buttons = 0;

void mouse_isr(void) {
    uint8_t data = inb(0x60);

    mouse_packet[mouse_cycle] = data;
    mouse_cycle++;

    if (mouse_cycle == 3) {
        mouse_cycle = 0;

        // Extract button state
        mouse_buttons = mouse_packet[0] & 0x07;

        // Extract movement (sign-extend using status bits)
        int dx = mouse_packet[1];
        int dy = mouse_packet[2];
        if (mouse_packet[0] & 0x10) dx |= 0xFFFFFF00;  // Sign extend X
        if (mouse_packet[0] & 0x20) dy |= 0xFFFFFF00;  // Sign extend Y

        // Update position (Y is inverted on PS/2)
        mouse_x += dx;
        mouse_y -= dy;

        // Clamp to screen bounds
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= screen_width) mouse_x = screen_width - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= screen_height) mouse_y = screen_height - 1;
    }

    pic_send_eoi(12);
}
```

---

## Summary: All Drivers

| Driver | IRQ | INT (remapped) | Ports | Est. Lines |
|--------|-----|----------------|-------|-----------|
| PIC | — | — | 0x20/21, 0xA0/A1 | ~50 |
| PIT Timer | 0 | 32 | 0x40, 0x43 | ~40 |
| PS/2 Keyboard | 1 | 33 | 0x60, 0x64 | ~150 |
| PS/2 Mouse | 12 | 44 | 0x60, 0x64 | ~120 |
| **Total** | | | | **~360** |

Combined with ATA/IDE (~300 lines), total driver code: **~660 lines of C**.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| Allegro dkeybd.c | lwp/sources/allegro/src/dos/dkeybd.c | Keyboard scancode handling patterns |
| Allegro dmouse.c | lwp/sources/allegro/src/dos/dmouse.c | Mouse packet parsing, coordinate tracking |
| Allegro dtimer.c | lwp/sources/allegro/src/dos/dtimer.c | PIT programming patterns |
| 386 Bible Ch.8 | i386-bible/pages/page_0145-0158 | I/O port access |
| OSDev PS/2 | wiki.osdev.org/PS/2_Keyboard, PS/2_Mouse | PS/2 protocol reference |
| OSDev 8259 PIC | wiki.osdev.org/8259_PIC | PIC remapping reference |

---

*Last updated: 2026-04-28*
