# PC Speaker + PIT Channel 2

> The simplest audio output on a PC. Many DOS programs use it for beeps, alerts, and even crude PCM playback. Driven by PIT channel 2 gated through port 0x61.

**Date:** 2026-05-02
**Status:** Complete — register-level reference for implementation
**Source:** Intel 8254 PIT datasheet; IBM PC Technical Reference

---

## Findings

### Hardware Path

```
PIT Channel 2 (0x42) → Gate (port 0x61 bit 0) → Speaker Enable (port 0x61 bit 1) → Speaker
```

Both the gate AND the speaker enable must be on for sound.

### Ports

| Port | R/W | Purpose |
|------|-----|---------|
| 0x42 | W | PIT Channel 2 counter (write low byte then high byte) |
| 0x43 | W | PIT Control Word (shared by all 3 channels) |
| 0x61 | R/W | System Control Port B |

### Port 0x61 Bits (relevant)

| Bit | Name | Purpose |
|-----|------|---------|
| 0 | GATE2 | PIT channel 2 gate enable (1=counting) |
| 1 | SPKR | Speaker data enable (1=connected to PIT output) |

### PIT Control Word for Speaker (0x43)

For square wave tone generation:
```
0xB6 = 10 11 011 0
       ^  ^  ^   ^
       |  |  |   binary (not BCD)
       |  |  mode 3 (square wave)
       |  lobyte/hibyte access
       channel 2
```

### Frequency and Divisor

```
PIT clock = 1,193,182 Hz
divisor = 1193182 / desired_frequency
```

| Note | Frequency | Divisor |
|------|-----------|---------|
| C4 (middle C) | 262 Hz | 4554 |
| D4 | 294 Hz | 4058 |
| E4 | 330 Hz | 3616 |
| F4 | 349 Hz | 3418 |
| G4 | 392 Hz | 3043 |
| A4 | 440 Hz | 2712 |
| B4 | 494 Hz | 2415 |
| C5 | 523 Hz | 2281 |

### Play a Tone

```
1. outb(0x43, 0xB6)                    -- PIT: channel 2, lobyte/hibyte, square wave
2. outb(0x42, divisor & 0xFF)          -- counter low byte
3. outb(0x42, (divisor >> 8) & 0xFF)   -- counter high byte
4. outb(0x61, inb(0x61) | 0x03)        -- enable gate + speaker
```

### Stop the Tone

```
outb(0x61, inb(0x61) & ~0x03)          -- disable gate + speaker
```

### Crude PCM Playback Trick

Some DOS games play samples through the PC speaker by:
1. Setting a high-frequency timer interrupt (e.g., 44100 Hz via PIT channel 0)
2. In the ISR, toggling port 0x61 bit 1 based on the sample's amplitude
3. 1-bit audio — toggle on for positive samples, off for negative

This produces crude but recognizable audio. Quality is terrible but it works on every PC.

### Implementation Notes for Pinecore

1. We already use PIT channel 0 for the tick counter — channel 2 is independent
2. Speaker beep is useful for error alerts, POST codes, and simple music
3. `beep(frequency, duration_ms)` — trivial to implement
4. Don't leave the speaker on — it's physically annoying
5. The speaker read-back (port 0x61 bit 5) can be used to check if the timer output is high/low

---

*Primary sources: Intel 8254 Programmable Interval Timer datasheet; IBM Personal Computer Technical Reference*
