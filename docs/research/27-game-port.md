# Game Port / Joystick

> Standard IBM PC game port at I/O 0x201. Reads analog joystick axes and digital buttons. Simple one-shot timer interface.

**Date:** 2026-05-02
**Status:** Complete — register-level reference for implementation
**Source:** IBM PC Technical Reference Manual; Game Port specification

---

## Findings

### Port

Single I/O port: **0x201**

### Reading Buttons (digital, instant)

Read port 0x201. Bits 4-7 are button states (active low = pressed when 0):

| Bit | Button |
|-----|--------|
| 4 | Joystick A, Button 1 |
| 5 | Joystick A, Button 2 |
| 6 | Joystick B, Button 1 |
| 7 | Joystick B, Button 2 |

```
val = inb(0x201)
button1_pressed = !(val & 0x10)
button2_pressed = !(val & 0x20)
```

### Reading Axes (analog, timed)

The game port uses one-shot timers. Each axis has an RC circuit (resistor = joystick potentiometer, capacitor on the card). The time it takes to discharge is proportional to joystick position.

| Bit | Axis |
|-----|------|
| 0 | Joystick A, X axis |
| 1 | Joystick A, Y axis |
| 2 | Joystick B, X axis |
| 3 | Joystick B, Y axis |

**Read sequence:**
```
1. Write ANY value to port 0x201 (triggers all 4 one-shots)
2. Immediately start counting
3. Repeatedly read port 0x201
4. For each axis bit (0-3): count iterations until the bit goes LOW (0)
5. The count = axis position (higher = more deflection)
```

**Typical values:**
- Center: ~300-500 counts
- Full left/up: ~20-50 counts
- Full right/down: ~600-900 counts
- No joystick: bit stays high indefinitely (timeout after ~65536)

### Calibration

```
1. Ask user to center joystick → read all 4 axes → store as center_x, center_y
2. Ask user to move to upper-left corner → store as min_x, min_y
3. Ask user to move to lower-right corner → store as max_x, max_y
4. Normalize: position = (raw - center) / (max - center) for positive, etc.
```

### Timing Considerations

The one-shot discharge time depends on the CPU speed. Faster CPUs count more iterations for the same position. Two approaches:
1. **Use PIT for timing:** Read PIT counter before and after, compute elapsed time instead of iteration count
2. **Calibrate per-machine:** Always calibrate at startup (most DOS games do this)

### Implementation Notes for Pinecore

1. Dead simple hardware — single port, no IRQ, no DMA
2. **Polling only** — no interrupt support. Read in game loop or on timer.
3. Timeout at ~65536 reads to handle "no joystick connected"
4. QEMU doesn't emulate the game port well — better tested on real hardware or DOSBox
5. Many DOS games also support keyboard as an alternative to joystick

---

*Primary source: IBM Personal Computer Technical Reference Manual, Game Adapter section*
