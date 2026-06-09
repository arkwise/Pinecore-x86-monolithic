# PCI Bus Enumeration

> Foundation for all PCI devices — NIC, sound, USB, SVGA. Must enumerate bus to find and configure devices.

**Date:** 2026-05-02
**Status:** Complete — register-level reference for implementation
**Source:** PCI Local Bus Specification 3.0

---

## Findings

### Configuration Space Access

PCI configuration uses two I/O ports for "Configuration Mechanism #1" (the only one used on modern x86):

| Port | Width | Name | Purpose |
|------|-------|------|---------|
| 0xCF8 | 32-bit | CONFIG_ADDRESS | Select bus/device/function/register |
| 0xCFC | 32-bit | CONFIG_DATA | Read/write the selected register |

**CONFIG_ADDRESS format (32-bit):**
```
Bit 31:      Enable (must be 1)
Bits 30-24:  Reserved (0)
Bits 23-16:  Bus number (0-255)
Bits 15-11:  Device number (0-31)
Bits 10-8:   Function number (0-7)
Bits 7-2:    Register offset (6 bits, 4-byte aligned)
Bits 1-0:    00 (always zero — 32-bit aligned access)
```

To read vendor ID from bus 0, device 3, function 0:
```
outl(0xCF8, 0x80001800)   // enable | bus=0 | dev=3<<11 | func=0 | reg=0
val = inl(0xCFC)           // low 16 = vendor ID, high 16 = device ID
```

### Type 0 Configuration Header (256 bytes)

| Offset | Size | Field | Purpose |
|--------|------|-------|---------|
| 0x00 | 2 | Vendor ID | 0xFFFF = no device present |
| 0x02 | 2 | Device ID | Identifies specific device |
| 0x04 | 2 | Command | Enable I/O, memory, bus master |
| 0x06 | 2 | Status | Device status flags |
| 0x08 | 1 | Revision ID | Chip revision |
| 0x09 | 1 | Prog IF | Programming interface |
| 0x0A | 1 | Subclass | Device subclass |
| 0x0B | 1 | Class Code | Device class |
| 0x0C | 1 | Cache Line Size | System cache line |
| 0x0D | 1 | Latency Timer | Bus master latency |
| 0x0E | 1 | Header Type | Bit 7 = multifunction, bits 6-0 = type (0/1/2) |
| 0x0F | 1 | BIST | Self-test |
| 0x10 | 4 | BAR 0 | Base Address Register 0 |
| 0x14 | 4 | BAR 1 | Base Address Register 1 |
| 0x18 | 4 | BAR 2 | Base Address Register 2 |
| 0x1C | 4 | BAR 3 | Base Address Register 3 |
| 0x20 | 4 | BAR 4 | Base Address Register 4 |
| 0x24 | 4 | BAR 5 | Base Address Register 5 |
| 0x2C | 2 | Subsystem Vendor ID | Board vendor |
| 0x2E | 2 | Subsystem Device ID | Board device |
| 0x34 | 1 | Capabilities Pointer | Offset to first capability |
| 0x3C | 1 | Interrupt Line | IRQ assigned by BIOS/firmware |
| 0x3D | 1 | Interrupt Pin | 1=INTA, 2=INTB, 3=INTC, 4=INTD |

### Command Register (offset 0x04)

| Bit | Name | Purpose |
|-----|------|---------|
| 0 | I/O Space Enable | Allow I/O port access |
| 1 | Memory Space Enable | Allow MMIO access |
| 2 | Bus Master Enable | Allow DMA |
| 6 | Parity Error Response | Report parity errors |
| 8 | SERR# Enable | System error reporting |
| 10 | Interrupt Disable | Disable INTx |

**Typical init:** write `0x0007` to enable I/O + Memory + Bus Master.

### BAR Decoding

Bit 0 of each BAR indicates the type:

**Bit 0 = 0: Memory Space**
- Bits 2-1: Type (00=32-bit, 10=64-bit)
- Bit 3: Prefetchable
- Bits 31-4: Base address (16-byte aligned minimum)

**Bit 0 = 1: I/O Space**
- Bits 31-2: Base address (4-byte aligned)

**Size detection algorithm:**
1. Save original BAR value
2. Write 0xFFFFFFFF to BAR
3. Read back — mask low bits (bits 3-0 for memory, bit 0 for I/O)
4. Invert and add 1 = size in bytes
5. Restore original BAR value

### PCI Class Codes (relevant to DOS-era)

| Class | Subclass | Device Type |
|-------|----------|-------------|
| 0x01 | 0x01 | IDE Controller |
| 0x02 | 0x00 | Ethernet Controller |
| 0x03 | 0x00 | VGA Controller |
| 0x04 | 0x01 | Audio Device |
| 0x06 | 0x00 | Host Bridge |
| 0x06 | 0x01 | ISA Bridge |
| 0x06 | 0x04 | PCI-to-PCI Bridge |
| 0x0C | 0x03 | USB Controller |

### Common Device IDs

| Vendor | Device | Description |
|--------|--------|-------------|
| 0x10EC | 0x8139 | Realtek RTL8139 Fast Ethernet |
| 0x8086 | 0x100E | Intel 82540EM Gigabit Ethernet |
| 0x8086 | 0x2415 | Intel AC'97 Audio (ICH) |
| 0x1234 | 0x1111 | QEMU Standard VGA |
| 0x1022 | 0x2000 | AMD PCnet-PCI Ethernet |

### Enumeration Algorithm

```
for bus = 0 to 255:
    for device = 0 to 31:
        vendor = pci_read16(bus, dev, 0, 0x00)
        if vendor == 0xFFFF: continue

        header_type = pci_read8(bus, dev, 0, 0x0E)
        max_func = (header_type & 0x80) ? 7 : 0

        for func = 0 to max_func:
            vendor = pci_read16(bus, dev, func, 0x00)
            if vendor == 0xFFFF: continue

            device_id = pci_read16(bus, dev, func, 0x02)
            class     = pci_read8(bus, dev, func, 0x0B)
            subclass  = pci_read8(bus, dev, func, 0x0A)
            irq       = pci_read8(bus, dev, func, 0x3C)

            register_device(bus, dev, func, vendor, device_id, class, subclass)
```

### Implementation Notes for Pinecore

1. PCI uses 32-bit port I/O (`outl`/`inl`) — we need to add these to `io.h`
2. Enumerate at boot, store found devices in a static array
3. Drivers look up devices by vendor:device ID or class:subclass
4. Must enable bus master (command bit 2) before any DMA device can work
5. IRQ line at offset 0x3C tells which IRQ the BIOS assigned — route to our IDT
6. QEMU emulates RTL8139, AC'97, and PIIX IDE by default with `-net nic,model=rtl8139`

---

*Primary source: PCI Local Bus Specification, Revision 3.0 (PCI-SIG)*
