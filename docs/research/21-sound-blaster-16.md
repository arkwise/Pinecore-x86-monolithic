# Sound Blaster 16 + OPL3 FM Synthesis

> THE DOS sound card. SB16 DSP handles PCM playback via DMA. OPL3 (YMF262) provides FM synthesis. Nearly every DOS game supports one or both.

**Date:** 2026-05-02
**Status:** Complete — register-level reference for implementation
**Sources:** Creative Labs Sound Blaster Series Hardware Programming Guide; Yamaha YMF262 Application Manual

---

## Findings

### Part 1: Sound Blaster 16 DSP

#### I/O Port Map (base = 0x220 typical)

| Offset | Port | R/W | Purpose |
|--------|------|-----|---------|
| +0x04 | 0x224 | W | Mixer Address Register |
| +0x05 | 0x225 | R/W | Mixer Data Register |
| +0x06 | 0x226 | W | DSP Reset |
| +0x0A | 0x22A | R | DSP Read Data |
| +0x0C | 0x22C | W | DSP Write Data / Command |
| +0x0C | 0x22C | R | DSP Write Buffer Status (bit 7 = busy) |
| +0x0E | 0x22E | R | DSP Read Buffer Status (bit 7 = data ready) |
| +0x0F | 0x22F | R | DSP 16-bit Interrupt Acknowledge |

#### DSP Reset Sequence
1. Write `0x01` to port base+0x06
2. Wait at least 3 microseconds
3. Write `0x00` to port base+0x06
4. Poll base+0x0E until bit 7 is set (data ready)
5. Read base+0x0A — must return `0xAA` (reset successful)
6. Timeout after ~100ms if `0xAA` never appears = no SB present

#### DSP Write Protocol
1. Read base+0x0C — wait until bit 7 = 0 (write buffer free)
2. Write command/data byte to base+0x0C

#### DSP Read Protocol
1. Read base+0x0E — wait until bit 7 = 1 (data available)
2. Read data from base+0x0A

#### Key DSP Commands

| Command | Hex | Purpose |
|---------|-----|---------|
| Get version | 0xE1 | Returns major then minor version byte |
| Set sample rate | 0x41 | Next 2 bytes: rate high, rate low (e.g., 0x2C, 0x00 = 11025 Hz) |
| 8-bit DMA auto-init | 0xC6 | Start 8-bit auto-init playback |
| 16-bit DMA auto-init | 0xB6 | Start 16-bit auto-init playback |
| Set block size | (part of above) | After mode cmd: length_low, length_high (size-1) |
| Pause DMA 8-bit | 0xD0 | Pause playback |
| Resume DMA 8-bit | 0xD4 | Resume playback |
| Pause DMA 16-bit | 0xD5 | Pause 16-bit playback |
| Resume DMA 16-bit | 0xD6 | Resume 16-bit playback |
| Speaker on | 0xD1 | Enable speaker output |
| Speaker off | 0xD3 | Disable speaker output |
| Stop auto-init 8-bit | 0xDA | End auto-init transfer |
| Stop auto-init 16-bit | 0xD9 | End auto-init transfer |

#### 8-bit PCM Playback Sequence (Auto-Init DMA)
1. Reset DSP
2. Send `0xD1` (speaker on)
3. Send `0x41` then sample rate high/low bytes (e.g., 11025 = 0x2B11)
4. Set up ISA DMA channel 1:
   - Mask channel 1: outb(0x0A, 0x05)
   - Reset flip-flop: outb(0x0C, 0xFF)
   - Set address: outb(0x02, lo), outb(0x02, hi) — page reg: outb(0x83, page)
   - Set count: outb(0x03, (count-1) lo), outb(0x03, (count-1) hi)
   - Mode: outb(0x0B, 0x58) — single, auto-init, read (mem→device), channel 1
   - Unmask: outb(0x0A, 0x01)
5. Send `0xC6` (8-bit auto-init output)
6. Send `0x00` (unsigned mono)
7. Send block_size_low, block_size_high (size of half-buffer - 1)

#### 16-bit PCM Playback (DMA Channel 5)
Same concept but uses DMA channel 5 (16-bit):
- Mask: outb(0xD4, 0x01) — channel 5
- Flip-flop: outb(0xD8, 0xFF)
- Address: outb(0xC4, lo), outb(0xC4, hi) — page: outb(0x8B, page)
- Count: outb(0xC6, lo), outb(0xC6, hi) — count in 16-bit words - 1
- Mode: outb(0xD6, 0x59) — auto-init, read, channel 5
- Unmask: outb(0xD4, 0x01)
- DSP command: `0xB6` (16-bit auto-init signed), then `0x10` (signed mono), then block size

#### IRQ Handling
- SB16 typically uses IRQ 5, 7, 9, or 10 (configurable via mixer register 0x80)
- On interrupt: read base+0x0E (8-bit) or base+0x0F (16-bit) to acknowledge
- IRQ fires when DMA block transfer completes — fill the other half of the double buffer

#### Mixer Registers (write index to base+0x04, read/write data at base+0x05)

| Register | Purpose |
|----------|---------|
| 0x22 | Master Volume (bits 7-4 = left, 3-0 = right) |
| 0x04 | Voice Volume (DAC) |
| 0x26 | FM Volume |
| 0x28 | CD Volume |
| 0x2E | Line Volume |
| 0x80 | IRQ select (bit 0=IRQ2, 1=IRQ5, 2=IRQ7, 3=IRQ10) |
| 0x81 | DMA select (bit 0=DMA0, 1=DMA1, 3=DMA3 for 8-bit; bit 5=DMA5, 6=DMA6, 7=DMA7 for 16-bit) |

---

### Part 2: OPL3 FM Synthesis (Yamaha YMF262)

#### I/O Ports

| Port | Purpose |
|------|---------|
| 0x388 | Register select (bank 0, operators 1-18) |
| 0x389 | Data write (bank 0) |
| 0x38A | Register select (bank 1, operators 19-36) |
| 0x38B | Data write (bank 1) |

#### Write Protocol
1. Write register index to 0x388 (or 0x38A for bank 1)
2. Wait at least 3.3 microseconds (read port 0x388 six times as delay)
3. Write data to 0x389 (or 0x38B)
4. Wait at least 23 microseconds (read port 0x388 thirty-five times as delay)

#### OPL3 Register Map (per channel, 9 channels in each bank)

**Operator Registers (each channel has 2 operators: modulator + carrier):**

| Register | Bits | Purpose |
|----------|------|---------|
| 0x20+op | 7-4: Multi, 3-0: tremolo/vibrato/sustain/KSR | Multiplier + flags |
| 0x40+op | 7-6: Key scale level, 5-0: Total level (volume, 0=max) | Volume/attenuation |
| 0x60+op | 7-4: Attack rate, 3-0: Decay rate | ADSR attack/decay |
| 0x80+op | 7-4: Sustain level, 3-0: Release rate | ADSR sustain/release |
| 0xE0+op | 2-0: Waveform select (0=sine, 1=half-sine, 2=abs-sine, 3=pulse-sine) | Waveform |

**Channel Registers:**

| Register | Bits | Purpose |
|----------|------|---------|
| 0xA0+ch | 7-0: F-Number low 8 bits | Frequency low byte |
| 0xB0+ch | 5: Key On, 4-2: Block(octave), 1-0: F-Num high 2 bits | Key on + frequency |
| 0xC0+ch | 3-1: Feedback, 0: Synthesis type (0=FM, 1=additive) | Feedback + connection |

#### Frequency Calculation
```
F-Number = frequency * 2^(20-block) / 49716
```
Where block (octave) = 0-7. Choose block to keep F-Number in 0-1023 range.

Example: A4 = 440 Hz
- Block 4: F-Number = 440 * 2^16 / 49716 = 580

#### Playing a Simple Tone
1. Write 0x01 to register 0x105 (OPL3 mode enable, via bank 1)
2. Set operator 0 (modulator): total level = 0x00 (max volume), attack = 0xF0 (instant), sustain = 0x00
3. Set operator 3 (carrier for channel 0): same envelope settings
4. Set channel 0 feedback/connection: 0xC0+0 = 0x31 (feedback 6, additive)
5. Set frequency: 0xA0+0 = F-Num low, 0xB0+0 = Key-On + block + F-Num high
6. To stop: clear Key-On bit in 0xB0+channel

#### Operator Offset Table (channel → operator register offsets)
```
Channel:  0  1  2  3  4  5  6  7  8
Op 1:     0  1  2  8  9  A 10 11 12
Op 2:     3  4  5  B  C  D 13 14 15
```

---

### Implementation Notes for Pinecore

1. **Detection:** Try DSP reset at 0x220. If 0xAA comes back, SB is present. Read version with 0xE1.
2. **DMA buffers** must be below 16MB (ISA DMA), must not cross 64KB boundary — same constraints as FDC
3. **Double buffering:** Allocate two half-buffers. Fill one while the other plays. IRQ signals when to swap.
4. **OPL3 detection:** Write 0x04 to register 0x04 (timer control), read back status. OPL3 returns specific pattern.
5. **QEMU SB16:** `-device sb16` emulates SB16 at 0x220, IRQ 5, DMA 1/5. OPL3 at 0x388.

---

*Primary sources: Creative Labs Sound Blaster Series Hardware Programming Guide; Yamaha YMF262 (OPL3) Application Manual; Intel 8237A DMA Controller datasheet*
