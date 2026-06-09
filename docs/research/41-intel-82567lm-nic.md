# 41 — Intel 82567LM-3 Gigabit Ethernet (OptiPlex 780 / ICH10) — programming surface

Status: research only (no code). Bring-up target for the pinecore-x86 packet driver — the OptiPlex 780's onboard NIC has no DOS packet driver in the wild, and this document captures the chip-level programming surface needed to write one.

Companion docs:
- `42-e1000e-linux-driver-map.md` — Linux driver structural map (skip-list, call graph)
- `43-packet-driver-spec.md` — Crynwr Packet Driver API
- `44-82567lm-port-plan.md` — synthesis + phased plan

Source-of-truth used while writing: a local v6.6 sparse-checkout of `linux/drivers/net/ethernet/intel/e1000e/` at `/Users/chelsonaitcheson/Projects/linux-e1000e-ref/`. Every "e1000e: …" citation below was line-verified against that tree.

---

## 1. Identification — what's actually in an OptiPlex 780

### 1.1 The part
- **Marketing name:** Intel 82567LM-3 Gigabit Ethernet (PHY).
- **Architecture nuance (critical):** the "82567" is *the PHY chip only*. The MAC lives inside the ICH10 / ICH10R Platform Controller Hub Southbridge. PHY ↔ MAC communication crosses the **LCI** (LAN Connect Interface, 10/100) and **GLCI** (Gigabit LAN Connect Interface, 1G) serial buses inside the ICH package. To software, the LAN is a single PCI function exposed by the ICH10.
- **Intel ARK page:** <https://ark.intel.com/content/www/us/en/ark/products/35646/intel-82567lm-gigabit-ethernet-phy.html>.

### 1.2 PCI presence on OptiPlex 780
- **PCI Vendor ID:** `0x8086` (Intel).
- **PCI Device ID:** `0x10DE` — `E1000_DEV_ID_ICH10_D_BM_LM` (e1000e: `hw.h:55`). The OptiPlex 780 is a desktop board with ICH10 → desktop variant → **LM** SKU.
- Sibling device IDs in the same family that other OptiPlex-class boards may carry: `0x10DF` (`ICH10_D_BM_LF`), `0x10CC` / `0x10CD` / `0x10CE` (ICH10R mobile variants `_LM`/`_LF`/`_V`). Source: `hw.h:52-56`.
- **Class:** `0x020000` (Network controller — Ethernet).
- **Bus/Device/Function:** integrated function inside the PCH; on most ICH9/10 boards it appears at **bus 0, device 25 (0x19), function 0** (BIOS-determined). Verify on the live machine via `lspci` (Linux) or our kernel's PCI enumeration.

### 1.3 The integrated PHY identity
- **PHY ID (read via MDIC from MII regs 2/3):** `0x01410CB0` = `BME1000_E_PHY_ID` (e1000e: `defines.h:705`). The "BME1000" PHY family is the one paired with ICH10. Revision-2 variant: `0x01410CB1` = `BME1000_E_PHY_ID_R2` (e1000e: `defines.h:706`).
- Linux dispatches the 82567 PHY through the `igp_3` register layout (e1000e `ich8_phy_ops.read_reg = e1000e_read_phy_reg_igp` at `ich8lan.c:5836`).

### 1.4 Board flags (per Linux's `e1000_ich10_info`)
From `ich8lan.c:5901-5915` (`e1000_ich10_info` struct, the one used when `pci_device_id` matches `0x10DE`):
- `FLAG_HAS_JUMBO_FRAMES` — jumbo supported in principle (but see §10 errata).
- `FLAG_IS_ICH` — the "PHY in the ICH" architecture.
- `FLAG_HAS_WOL` — Wake-on-LAN supported.
- `FLAG_HAS_CTRLEXT_ON_LOAD` — needs the `CTRL_EXT` reload sequence at driver load.
- `FLAG_HAS_AMT` — the chip is wired into Intel AMT / iAMT firmware (this is what drives the §6.2 SWFLAG dance).
- `FLAG_HAS_FLASH` — config + MAC address live in the platform SPI flash GbE region (no separate EEPROM).
- `FLAG_APME_IN_WUC` — APME (power-management enable) bit is in WUC (Wake Up Control) register, not the conventional location.
- `pba = 18` (Packet Buffer Allocation default — 18 KB split for descriptor buffers).
- `max_hw_frame_size = DEFAULT_JUMBO`.

These flags are the chip-level personality knobs. A from-scratch driver must reproduce each one's behavioural consequence.

---

## 2. Architecture overview

Three programmable blocks, all reachable through one PCI function:

```
+-------------------------------------------------------------------+
|  ICH10 PCH                                                        |
|  +------------------------------------+   +--------------------+  |
|  |  Integrated MAC                    |   |  SPI Flash         |  |
|  |  (e1000-class register file)       |   |  GbE region        |  |
|  |  - BAR0: MAC CSRs (memory)         |   |  - MAC addr        |  |
|  |  - BAR1: Flash window              |   |  - PHY init params |  |
|  |  - BAR2: IO BAR (IOADDR/IODATA)    |   |  - WoL config      |  |
|  +--------------+---------------------+   +--------------------+  |
|                 |                                                  |
|                 | GLCI (1G)  +  LCI (10/100) serial buses          |
|                 v                                                  |
|         +---------------+                                          |
|         |  82567LM PHY  |  ← package-external part                |
|         +-------+-------+                                          |
+-----------------|----------------------------------------------------+
                  |  copper twisted pair to RJ-45
```

Driver-relevant consequences:
- **All driver-visible programming is to the MAC**. The PHY is reached *only* through the MAC's `MDIC` register (e1000e: `regs.h:13`, offset 0x20), with the SWFLAG semaphore (§6.2) protecting against firmware contention.
- **Kumeran** is an Intel-internal protocol layered on GLCI/LCI for register-style operations between MAC and PHY (clock skew, EEE coordination, K1 idle config). Reached via `KMRNCTRLSTA` (e1000e: `regs.h:206`, offset 0x34) — write the kumeran reg-addr + data, then read back. Helper functions are `e1000e_read_kmrn_reg_locked` / `_write_kmrn_reg_locked` (used at `ich8lan.c:933, 939, 2329, 2339` for K1 idle setup).
- **The MAC is bus-master-capable** (PCI Express device), 64-bit DMA addressing (Linux requests `DMA_BIT_MASK(64)` at `netdev.c:7386`). No 16 MB ISA-DMA limit applies.

---

## 3. Register map (MAC CSRs, BAR0)

All offsets from e1000e `regs.h`. Width is 32 bits unless noted. RW unless noted.

### 3.1 Core control & status
| Offset | Symbol | Meaning |
|--------|--------|---------|
| `0x0000` | `E1000_CTRL` (`regs.h:7`) | Device Control. SLU, ASDE, FRCSPD, RST, PHY_RST, ILOS, FD bits. |
| `0x0008` | `E1000_STATUS` (`regs.h:8`) | Device Status (RO). LU (link up), SPEED, FD, ASDV. |
| `0x0018` | `E1000_CTRL_EXT` | Extended control: DRV_LOAD bit (must set on probe — see `FLAG_HAS_CTRLEXT_ON_LOAD` above). |
| `0x0028` | `E1000_FEXTNVM` (`regs.h:18`) | Future Extended NVM — used for various ICH-specific knobs. |
| `0x0F00` | `E1000_EXTCNF_CTRL` (`regs.h:52`) | Extended Config Control. Hosts the **SWFLAG** bit (0x20) — see §6. |

### 3.2 NVM / EEPROM proxy
| Offset | Symbol | Meaning |
|--------|--------|---------|
| `0x0010` | `E1000_EECD` (`regs.h:9`) | EEPROM/Flash Control. SK/CS/DI/DO bits (legacy "bit-bang" but on ICH this is largely vestigial; MAC address comes through flash, see §7). |
| `0x0014` | `E1000_EERD` (`regs.h:10`) | EEPROM Read trigger. Set START + ADDR; poll DONE; read DATA. |

### 3.3 PHY interface
| Offset | Symbol | Meaning |
|--------|--------|---------|
| `0x0020` | `E1000_MDIC` (`regs.h:13`) | MDI Control — read/write PHY MII registers. See §6.1. |
| `0x0034` | `E1000_KMRNCTRLSTA` (`regs.h:206`) | Kumeran serial-bus control/status. |

### 3.4 Interrupt block
| Offset | Symbol | Meaning |
|--------|--------|---------|
| `0x00C0` | `E1000_ICR` (`regs.h:32`) | Interrupt Cause Read (R/clear). Reading clears bits. |
| `0x00C4` | `E1000_ITR` (`regs.h:33`) | Interrupt Throttle Rate. |
| `0x00D0` | `E1000_IMS` (`regs.h:35`) | Interrupt Mask Set (write 1 to enable). |
| `0x00D8` | `E1000_IMC` (`regs.h:36`) | Interrupt Mask Clear (write 1 to disable). |

ICR/IMS bit set (e1000e `defines.h:382-454`):
- `E1000_ICR_TXDW = 0x01` — TX descriptor written back (TX complete).
- `E1000_ICR_LSC = 0x04` — Link Status Change.
- `E1000_ICR_RXDMT0 = 0x10` — RX descriptor minimum threshold (ring nearly empty — back-pressure).
- `E1000_ICR_RXT0 = 0x80` — RX timer interrupt (packets ready to drain).

### 3.5 RX path
| Offset | Symbol | Meaning |
|--------|--------|---------|
| `0x0100` | `E1000_RCTL` (`regs.h:42`) | Receive Control. EN, UPE, MPE, LPE, BAM, BSIZE, etc. |
| `0x2800` | `E1000_RDBAL(0)` (`regs.h:82`) | RX Descriptor Base Address Low (queue 0). |
| `0x2804` | `E1000_RDBAH(0)` | RX Descriptor Base Address High (used iff 64-bit DMA). |
| `0x2808` | `E1000_RDLEN(0)` (`regs.h:86`) | RX Descriptor ring length (bytes — multiple of 128). |
| `0x2810` | `E1000_RDH(0)` (`regs.h:88`) | RX Descriptor Head (NIC writes). |
| `0x2818` | `E1000_RDT(0)` (`regs.h:90`) | RX Descriptor Tail (driver writes). |
| `0x2828` | `E1000_RXDCTL(0)` | RX Descriptor Control (prefetch thresholds, queue enable). |
| `0x05200` | `E1000_MTA` (`regs.h:196`) | Multicast Table Array (128 entries × 32 bits = 4096-bit hash). |
| `0x05400` | `E1000_RAL(0)` (`regs.h:108`) | Receive Address Low (entry 0). Pairs with `RAH(0)` at +4. 16 entries for ICH. |
| `0x05404` | `E1000_RAH(0)` (`regs.h:110`) | Receive Address High. Top bit = AV (Address Valid). |
| `0x05600` | `E1000_VFTA` (`regs.h:198`) | VLAN Filter Table Array (128 entries). |

RCTL key bits (e1000e `defines.h:116-129`):
- `E1000_RCTL_EN = 0x02` — receiver enable.
- `E1000_RCTL_UPE = 0x08` — unicast promiscuous enable.
- `E1000_RCTL_MPE = 0x10` — multicast promiscuous enable.
- `E1000_RCTL_LPE = 0x20` — long packet enable (jumbo) — **see §10 errata: avoid on 82567 -3 SKUs**.
- `E1000_RCTL_BAM = 0x8000` — broadcast accept mode.

### 3.6 TX path
| Offset | Symbol | Meaning |
|--------|--------|---------|
| `0x0400` | `E1000_TCTL` (`regs.h:47`) | Transmit Control. EN, PSP, CT, COLD, SWXOFF, RTLC. |
| `0x3800` | `E1000_TDBAL(0)` (`regs.h:94`) | TX Descriptor Base Address Low. |
| `0x3804` | `E1000_TDBAH(0)` | TX Descriptor Base Address High. |
| `0x3808` | `E1000_TDLEN(0)` (`regs.h:98`) | TX Descriptor ring length (bytes). |
| `0x3810` | `E1000_TDH(0)` (`regs.h:100`) | TX Descriptor Head (NIC writes). |
| `0x3818` | `E1000_TDT(0)` (`regs.h:102`) | TX Descriptor Tail (driver writes — kicks transmit). |
| `0x3828` | `E1000_TXDCTL(0)` | TX Descriptor Control (prefetch thresholds, queue enable). |

### 3.7 Firmware coordination
| Offset | Symbol | Meaning |
|--------|--------|---------|
| `0x5B54` | `E1000_FWSM` (`regs.h:215`) | Firmware Semaphore — tells software whether AMT/ME firmware holds the device. |

---

## 4. Descriptor formats

### 4.1 Legacy RX descriptor (16 bytes)
```
offset 0x00: 64-bit physical buffer address    (driver writes; NIC reads)
offset 0x08: 16-bit length                     (NIC writes after RX)
offset 0x0A: 16-bit fragment checksum
offset 0x0C: 8-bit status (DD, EOP, IXSM, VP, …)
offset 0x0D: 8-bit error
offset 0x0E: 16-bit VLAN tag
```
Status `DD` (Descriptor Done) bit = 0x01 — set by NIC when packet has landed. Driver reads, processes, then writes `RDT` to recycle.

### 4.2 Legacy TX descriptor (16 bytes)
```
offset 0x00: 64-bit physical buffer address    (driver writes)
offset 0x08: 16-bit length
offset 0x0A: 8-bit CSO (checksum offset)
offset 0x0B: 8-bit CMD (EOP, IFCS, IC, RS, DEXT, VLE, IDE)
offset 0x0C: 8-bit STA (DD)
offset 0x0D: 8-bit CSS (checksum start)
offset 0x0E: 16-bit VLAN tag
```
Driver sets `CMD.EOP=1` on the last segment of a frame, `CMD.IFCS=1` to ask NIC to compute FCS, `CMD.RS=1` to ask for write-back-on-complete (so we can poll `STA.DD`).

These layouts are e1000-family standard; details in Intel 8254x SDM `317453` §3.2-3.3 (mirror: <https://pdos.csail.mit.edu/6.828/2019/readings/hardware/8254x_GBe_SDM.pdf>). The 82567 inherits them unchanged.

Extended TX descriptors (TSO, IPv4/TCP checksum offload, context-descriptor + data-descriptor pairs) exist but are optional — a minimum-viable packet driver doesn't need them.

---

## 5. Reset & init sequence (the master recipe)

Mirrors `e1000_reset_hw_ich8lan` + `e1000_init_hw_ich8lan` (both wired in `ich8_mac_ops` at `ich8lan.c:5811-5830`):

1. **Disable interrupts:** write `IMC = 0xFFFFFFFF` (clear all mask bits).
2. **Disable RX/TX rings:** `RCTL &= ~RCTL_EN`, `TCTL &= ~TCTL_EN`. Wait for any in-flight bus master cycles to drain.
3. **Disable bus mastering in PCI command register** (set PCI cfg 0x04 bit 2 to 0).
4. **Acquire SWFLAG** (§6.2) — required before touching PHY or many MAC CSRs on ICH parts.
5. **Reset:** `CTRL |= CTRL_RST` (bit 26). Wait ~3 ms. Read it back; bit clears when reset complete.
6. **Re-enable interrupts off:** `IMC = 0xFFFFFFFF`; read `ICR` to clear pending.
7. **Set `CTRL_EXT.DRV_LOAD`** (because `FLAG_HAS_CTRLEXT_ON_LOAD`).
8. **PHY init:** read PHY ID via MDIC (verify against `BME1000_E_PHY_ID = 0x01410CB0`). Apply 82567-specific PHY workarounds (e1000e: `e1000_phy_hw_reset_ich8lan` in `ich8_phy_ops.reset` at `ich8lan.c:5839`).
9. **Kumeran K1 idle config:** read `KMRNCTRLSTA[E1000_KMRNCTRLSTA_K1_CONFIG]`, set/clear `K1_ENABLE` per link speed (e1000e: `ich8lan.c:2327-2340`).
10. **Read MAC address** from flash GbE region into `RAL(0)`/`RAH(0)` (§7).
11. **Clear MTA** (multicast table — 128 dwords at `0x5200..0x53FF` → 0).
12. **Configure RX ring:**
    - Allocate descriptor ring (multiple of 128 bytes, e.g. 256 descriptors × 16 B = 4096 B = 1 page).
    - Allocate RX buffer pool (e.g. 256 × 2 KB).
    - Write physical address into `RDBAL(0)` / `RDBAH(0)`, length into `RDLEN(0)`.
    - Set `RDH = 0`, `RDT = N-1` (so NIC has N-1 buffers to fill).
    - Write `RXDCTL(0).ENABLE = 1`; wait for `RXDCTL.ENABLE` to read back as 1.
13. **Configure TX ring:**
    - Allocate descriptor ring + TX buffer pool similarly.
    - Write `TDBAL(0)` / `TDBAH(0)` / `TDLEN(0)`.
    - `TDH = TDT = 0`.
    - `TXDCTL(0).ENABLE = 1`.
    - `TCTL = TCTL_EN | TCTL_PSP | (15 << CT_SHIFT) | (64 << COLD_SHIFT)`.
14. **Enable RX:** `RCTL = RCTL_EN | RCTL_BAM | (BSIZE 2048) | …` (matching `set_rcv_mode` mode 3 by default).
15. **Enable interrupts:** `IMS = IMS_RXT0 | IMS_TXDW | IMS_LSC | IMS_RXDMT0`.
16. **Release SWFLAG.**
17. **Set link up:** `CTRL |= CTRL_SLU` (Set Link Up) | `CTRL_ASDE` (Auto-Speed Detection Enable). Wait for `STATUS.LU` = 1 (poll or wait for LSC interrupt).
18. **Re-enable bus mastering** in PCI command register.

Failure modes worth instrumenting:
- Step 5 reset can hang if SWFLAG (step 4) was skipped on an AMT-active board.
- Step 8 PHY ID read may return all-1s if firmware holds the PHY — see errata §10.
- Step 17 link-up may never assert if auto-negotiation fails (cable issue / no link partner). Don't block init forever — set a 5-second timeout and proceed.

---

## 6. PHY access — the hard part

### 6.1 MDIC register (offset 0x20, e1000e `regs.h:13`)

Format (32 bits):
```
bit 31: I  - Interrupt Enable
bit 30: R  - Ready (set by NIC when done; software clears via Read)
bit 29: E  - Error
bit 28-27: OP (01 = write, 10 = read)
bit 26-21: PHY address (5 bits) — for 82567 internal PHY = 0x01 typically
bit 25-21: actually the PHY addr field, see datasheet
bit 20-16: REGADDR (5 bits) — the MII register number
bit 15-0:  DATA (16 bits — written or read back)
```

Operation:
- **Write:** set OP=01, REGADDR, DATA; write to MDIC. Poll until R=1.
- **Read:** set OP=10, REGADDR; write to MDIC. Poll until R=1. Read DATA field from MDIC.

Polling timeout: a few hundred microseconds; if R never asserts, the firmware is holding the bus. Treat as failure.

### 6.2 SWFLAG semaphore — co-arbitration with the Management Engine

ICH9/10 boards with AMT/iAMT firmware have a second master on the PHY bus: the Intel ME. Without arbitration, MDIC operations interleave, MDIC reads return stale or all-1s data, and on bad timing the LAN-on-Motherboard hangs.

The arbitration is via the `EXTCNF_CTRL.SWFLAG` bit (`E1000_EXTCNF_CTRL = 0x0F00`, `regs.h:52`; `E1000_EXTCNF_CTRL_SWFLAG = 0x20`, `defines.h:337`).

Linux's algorithm — `e1000_acquire_swflag_ich8lan` at `ich8lan.c:1757-1810`:

1. Take a software-only spinlock (`__E1000_ACCESS_SHARED_RESOURCE`) — multi-CPU defence inside the driver. **Not needed for our single-threaded ISR.**
2. **Wait for FW release:** read `EXTCNF_CTRL`; if `SWFLAG` set, FW currently owns; spin with 1 ms delays up to `PHY_CFG_TIMEOUT` (100 attempts ≈ 100 ms).
3. **Stake claim:** write back `EXTCNF_CTRL` with `SWFLAG` set.
4. **Verify acquisition:** read back; if `SWFLAG` reads as 1, we own it. Otherwise loop with 1 ms delay up to `SW_FLAG_TIMEOUT` (≈100 ms).
5. **On failure:** log `"Failed to acquire the semaphore, FW or HW has it: FWSM=… EXTCNF_CTRL=…"`, clear `SWFLAG` defensively, return error.

Release — `e1000_release_swflag_ich8lan` at `ich8lan.c:1820-1842`:
- Read `EXTCNF_CTRL`. If `SWFLAG` still set, clear it and write back. If not set, FW stole it back — log a warning ("Semaphore unexpectedly released by sw/fw/hw").

**For pinecore on the OptiPlex 780:** AMT may or may not be active. The cheapest bring-up test is: try acquiring SWFLAG once; if it succeeds in <1 ms, we're effectively the only master and the dance is overhead-only. If it takes >10 ms, AMT is contending and we must do the full algorithm faithfully. Either way, **always release on the same call path** — leaving SWFLAG set hangs ME, which can brick the board until power cycle.

### 6.3 Kumeran (KMRNCTRLSTA)
`KMRNCTRLSTA = 0x0034` (`regs.h:206`). Format: writing this register simultaneously selects a kumeran "register index" and either reads or writes its value.

Used at ICH8/9/10 link-up time to configure K1 idle (the inter-packet idle behaviour on the GLCI bus). See `ich8lan.c:933-951` and `2327-2340`. For a minimum-viable driver: copy Linux's bit-for-bit setup. The K1 idle settings affect performance, not correctness.

### 6.4 PHY MII register set (standard 802.3 + IGP_3 extensions)
For the 82567, MII register numbers 0-15 are the IEEE-standard set; 16+ are IGP/IGP_3 vendor-specific:
- **0 (PHY_CTRL):** reset bit, power-down, auto-neg enable/restart, speed (FE), duplex.
- **1 (PHY_STATUS):** link status, auto-neg complete, fault, ability bits.
- **2 (PHY_ID1):** OUI bits 18-3 — should read `0x0141` (Marvell-derived Intel PHY OUI).
- **3 (PHY_ID2):** OUI bits 2-0, model, revision — combined yields `BME1000_E_PHY_ID = 0x01410CB0`.
- **4 (AUTONEG_ADV):** advertised abilities (10/100 base-T, full/half).
- **9 (CTRL_1000T):** 1000BASE-T control — advertise gigabit ability, master/slave preference.
- **10 (STATUS_1000T):** local-receiver-ok, remote-receiver-ok, master/slave config result.
- **17+ (IGP_3 vendor extensions):** PHY-specific power management, DSP-style equalization controls. Linux uses these for SmartSpeed, LPLU (Low Power Link Up), downshift. **A minimum-viable DOS driver can ignore these** — defaults are good enough.

---

## 7. NVM (MAC address) via flash GbE region

The 82567 has **no real EEPROM**. The MAC address and configuration data live in a dedicated "GbE region" of the platform's SPI flash, which the ICH10 exposes either:
- **Indirectly via EERD** (`E1000_EERD = 0x14`, `regs.h:10`) — emulated EEPROM read interface. Write `START + ADDR<<8`; poll `DONE`; read DATA. The hardware translates EERD commands into flash reads underneath.
- **Directly via BAR1** (a memory BAR exposing a window into the flash region).

Linux uses both depending on chip generation. For ICH8/9/10 specifically the path is `e1000_read_nvm_ich8lan` (in `ich8_nvm_ops.read` at `ich8lan.c:5849`).

MAC address is at NVM offset 0..2 (3 × 16-bit words = 6 bytes). Read at init:
1. Acquire NVM lock (`e1000_acquire_nvm_ich8lan` — `ich8_nvm_ops.acquire` at `ich8lan.c:5848`). This calls into the SWFLAG dance plus a software-only lock.
2. Read words 0, 1, 2 → 6 bytes of MAC.
3. Write the 4 low bytes to `RAL(0)`, the 2 high bytes + `AV=1` to `RAH(0)`.
4. Release NVM lock.

NVM checksum validation (`e1000_validate_nvm_checksum_ich8lan` at `ich8_nvm_ops.validate` — `ich8lan.c:5853`) sums words 0..0x3F; should equal `NVM_SUM = 0xBABA`. Useful at init but not critical — bad checksum means board needs Intel-issued reflash, not driver workaround.

**Practical shortcut for our bring-up:** the MAC address is also printed on a sticker inside the OptiPlex 780. For the very first run we can hardcode it and skip NVM access entirely, then add NVM-read once basic traffic works.

---

## 8. Interrupts

### 8.1 Programming model
Three registers: `ICR` (read = read + clear), `IMS` (write 1 to enable), `IMC` (write 1 to disable). Cause bits in §3.4.

The ISR algorithm:
1. Read `ICR`. If 0, this wasn't us — chain to next handler (relevant for shared PCI IRQ).
2. Bits set tell us what happened. Process each:
   - `RXT0` → drain RX ring → fire packet-driver upcalls.
   - `TXDW` → harvest TX descriptors that have `DD=1` → free buffers, signal `as_send_pkt` callers.
   - `LSC` → re-read PHY status, update link state.
   - `RXDMT0` → ring nearly empty (normally a back-pressure hint; usually combined with re-arming RDT).
3. The read of `ICR` already cleared the bits.
4. Send 8259 EOI (kernel responsibility).

### 8.2 ITR (Interrupt Throttle Rate)
`E1000_ITR = 0xC4` (`regs.h:33`). Sets minimum interval between interrupts (units of 256 ns). Reasonable default: write `0x100` (= 256 × 256 ns ≈ 65 µs minimum interval, ~15K interrupts/sec ceiling). Critical on Gigabit — without throttling a busy network can pin the CPU on interrupts alone.

### 8.3 IRQ routing on OptiPlex 780
- The ICH10 LAN device's interrupt pin is routed through PCI legacy IRQ steering. The BIOS programs it; we read the result from PCI config space offset `0x3C` (`Interrupt Line`).
- Typically lands on IRQ 11 or IRQ 5 on this board (BIOS-dependent — read it; don't hardcode).
- The legacy line is likely **shared** with USB or another PCI device. Our ISR must check `ICR != 0` before claiming ownership.
- **MSI / MSI-X**: the 82567 supports MSI per Linux config. **Skip for DOS** — legacy INTx is simpler and still works on this hardware.

---

## 9. Power management surface — what to disable

| Feature | Linux ref | Disposition for our DOS driver |
|---------|-----------|--------------------------------|
| ASPM L0s / L1 (PCIe link power states) | `netdev.c:e1000e_disable_aspm` | Disable. We never want the link sleeping. PCI cfg 0x80 + Link Control. |
| EEE (Energy Efficient Ethernet, 802.3az) | `ich8lan.c:e1000_set_eee_pchlan` (PCH only) | Not relevant on ICH10/82567 — EEE arrived with the PCH (Lynx Point) generation. |
| WoL (Wake-on-LAN) | `FLAG_HAS_WOL` set | Leave disabled. We're running, not sleeping. |
| LPLU (Low Power Link Up) | `e1000_set_d0_lplu_state_ich8lan`, `_set_d3_lplu_state_ich8lan` | Set D0 to "not LPLU"; we want full gigabit when up. |
| RuntimePM / SuspendResume | `netdev.c:e1000_suspend / _resume` | Not applicable. |

---

## 10. Errata of interest

Sources: 82567 Specification Update (Intel doc 321791-001 — mirror: <https://www.mouser.com/pdfdocs/82567specupdate-2.pdf>), Linux e1000e workarounds, Launchpad bug 445572.

### 10.1 Jumbo frame breakage on 82567 -3 SKUs
- **Symptom:** enabling `RCTL.LPE` (long packet) on 82567LM-3 / 82567V-3 causes RX corruption — frames are received with garbled length.
- **Linux workaround:** `e1000_ich10_info.flags` includes `FLAG_HAS_JUMBO_FRAMES`, but the driver has runtime guards to clamp MTU on -3 SKUs. See Launchpad bug 445572 (<https://bugs.launchpad.net/bugs/445572>).
- **Our action:** keep RX buffer size at standard 2 KB; refuse jumbo (`max_hw_frame_size <= 1518`) for our packet driver. Trivial workaround.

### 10.2 PHY-read returns all-ones with ME contention
- **Symptom:** `MDIC` read returns `0xFFFF` for PHY ID or other registers.
- **Cause:** ME/firmware holding the PHY bus despite our SWFLAG acquisition; happens during BIOS POST or in early boot.
- **Workaround:** retry MDIC read up to 3 times with 100 µs delays before declaring failure.

### 10.3 ICR read missing interrupts
- **Symptom:** interrupts fire, ICR reads as 0 anyway — possible on some 82567/82574 silicon during register-write/read races.
- **Linux fix:** see <https://patchwork.ozlabs.org/project/netdev/patch/20180305185726.4776-5-jeffrey.t.kirsher@intel.com/> — read `ICR` and also check `STATUS` and queue tail pointers to detect "phantom" RX completions.
- **Our action:** harmless to ignore initially; cache the "last RDT seen" and on each ISR check whether RDT advanced even if ICR reads 0.

### 10.4 ASPM-induced disconnects
- **Symptom:** with ASPM L1 enabled the link sporadically drops to 10 Mbit or disconnects.
- **Workaround:** disable ASPM (PCI cfg, Link Control register, bits 0-1 → 00). See `e1000e_disable_aspm` in `netdev.c`.

### 10.5 GbE region SPI-flash protection on locked BIOSes
- **Symptom:** corporate-BIOS OptiPlex 780s may have the GbE flash region write-protected; MAC change attempts will silently fail.
- **Workaround:** read-only is fine for a packet driver. Don't try to write back the MAC.

---

## 11. Quick-reference card for the implementer

**The 9 things you cannot skip:**
1. Acquire SWFLAG before any PHY MDIC operation. Release on every exit path.
2. Set `CTRL_EXT.DRV_LOAD` at init (because `FLAG_HAS_CTRLEXT_ON_LOAD`).
3. Read MAC from flash GbE region (via EERD) before enabling RX.
4. Configure RDBAL/RDLEN/RDH/RDT with **physical** addresses, not linear.
5. Enable Bus Master in PCI command register before kicking TDT.
6. Throttle interrupts via ITR — non-optional on gigabit.
7. Check `ICR != 0` first in the ISR (handle shared-IRQ case).
8. Disable ASPM via PCI Link Control register.
9. **Do not enable jumbo (`RCTL.LPE`) on the 82567 -3 SKUs.**

**The 6 things you can defer:**
1. NVM checksum validation (read MAC anyway).
2. Kumeran K1 idle config (link still comes up at sub-optimal but functional setting).
3. Multicast hash table — set to all-zeros; receivers either rely on BAM or set MPE.
4. VLAN filtering — leave all-zeros.
5. WoL / power management.
6. Extended/TSO TX descriptors — legacy descriptors transmit fine.

---

## 12. References

### Intel primary documents
- **Intel ARK** — 82567LM Gigabit Ethernet Phy: <https://ark.intel.com/content/www/us/en/ark/products/35646/intel-82567lm-gigabit-ethernet-phy.html>
- **Intel 82567 GbE PHY datasheet** (Intel doc 318343): <https://www.intel.com/content/dam/doc/datasheet/82567-gbe-phy-datasheet.pdf> — primary chip-level reference.
- **Intel ICH8/9/10 + 82566/67/62V Software Developer's Manual** (Intel doc 316080) — landing: <https://www.intel.ca/content/www/ca/en/embedded/products/networking/i-o-controller-hub-8-9-10-82566-82567-82562v-software-dev-manual.html>. PDF gated by Intel login; not directly retrievable.
- **Intel ICH10 family datasheet** (Intel doc 319973-003): <https://www.intel.sg/content/dam/doc/datasheet/io-controller-hub-10-family-datasheet.pdf>.
- **Intel ICH10 specification update** (Intel doc 319974): <https://www.intel.com/content/dam/www/public/us/en/documents/specification-updates/io-controller-hub-10-family-specification-update.pdf>.
- **Intel 82567 Specification Update** (Intel doc 321791-001) — mirror: <https://www.mouser.com/pdfdocs/82567specupdate-2.pdf>.
- **Intel 8254x Gigabit Ethernet SDM** (Intel doc 317453) — register family ancestor — MIT mirror: <https://pdos.csail.mit.edu/6.828/2019/readings/hardware/8254x_GBe_SDM.pdf>.

### Linux source citations (all from v6.6, `linux-e1000e-ref/drivers/net/ethernet/intel/e1000e/`)
- `hw.h:55` — `E1000_DEV_ID_ICH10_D_BM_LM = 0x10DE` (the OptiPlex 780 PCI ID).
- `hw.h:144` — `e1000_ich10lan` mac_type symbol.
- `defines.h:705` — `BME1000_E_PHY_ID = 0x01410CB0` (the integrated PHY identity).
- `defines.h:337` — `E1000_EXTCNF_CTRL_SWFLAG = 0x20`.
- `defines.h:382,387` — `E1000_ICR_TXDW`, `E1000_ICR_RXT0`.
- `defines.h:116,118,119,120,129` — `E1000_RCTL_EN/UPE/MPE/LPE/BAM`.
- `regs.h:7,8,9,10,13,18,32,33,35,36,42,47,52,82-102,108-110,196,198,206,215` — all MAC CSR offsets cited in §3.
- `ich8lan.c:1751-1810` — `e1000_acquire_swflag_ich8lan` (SWFLAG acquire).
- `ich8lan.c:1820-1842` — `e1000_release_swflag_ich8lan` (SWFLAG release).
- `ich8lan.c:933-951` and `2327-2340` — Kumeran K1 idle usage examples.
- `ich8lan.c:5811-5830` — `ich8_mac_ops` vector (reset_hw, init_hw, setup_link, etc.).
- `ich8lan.c:5832-5844` — `ich8_phy_ops` vector.
- `ich8lan.c:5846-5856` — `ich8_nvm_ops` vector.
- `ich8lan.c:5901-5915` — `e1000_ich10_info` struct.
- `netdev.c:7360` — `e1000_probe` entry.
- `netdev.c:7386` — `dma_set_mask_and_coherent(..., DMA_BIT_MASK(64))` — confirms 64-bit DMA.
- `netdev.c:7845-7847` — PCI device-ID-to-board-type mapping for ICH10 SKUs.

### Cross-reference
- `42-e1000e-linux-driver-map.md` — call graphs and file inventory.
- `43-packet-driver-spec.md` — DOS-side API the driver presents.
- `44-82567lm-port-plan.md` — synthesis + roadmap insertion.
- `20-pci-bus.md` — our PCI config-space access.

---

## 13. What still needs verification

1. **Live PCI ID on a physical OptiPlex 780.** Verified the device ID `0x10DE` is correctly classified as `ICH10_D_BM_LM` per Linux, and Dell service docs identify the 780's NIC as 82567LM-3. **Confirm on hardware before committing.**
2. **AMT firmware status on the actual board.** The SWFLAG dance complexity is gated by whether the ME is contending. Run a single MDIC read at init and time the SWFLAG acquisition.
3. **Bit positions inside `EXTCNF_CTRL` beyond `SWFLAG`.** Only the SWFLAG bit (`0x20`) is confirmed from the source. `EXTCNF_CTRL.OEM_WRITE_ENABLE` and other fields are used elsewhere in `ich8lan.c` — read them out before we touch them.
4. **`MDIC` PHY address field width.** The above doc gives the canonical e1000 format; the 82567's integrated PHY may use a non-default PHY address. Verify by reading PHY ID with addresses 0x01 and 0x02 — whichever returns `0x01410CB0` is correct.
5. **OptiPlex 780's interrupt routing.** Read PCI cfg 0x3C at probe; do not hardcode.
6. **Whether the GbE flash region is BIOS-write-protected** on a stock 780. Read-only access works regardless; write attempts may need a BIOS reflash to enable.
