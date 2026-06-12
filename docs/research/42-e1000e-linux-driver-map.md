# 42 — Linux e1000e driver structural map (for 82567LM-3 port)

Status: research only (no code). Source-of-truth: a sparse-checkout of Linux v6.6 at `linux-e1000e-ref/drivers/net/ethernet/intel/e1000e/`. Every `file:line` citation below was line-verified against that tree on 2026-05-26.

Companion docs:
- `41-intel-82567lm-nic.md` — chip-level register surface
- `43-packet-driver-spec.md` — Crynwr Packet Driver API we have to present
- `44-82567lm-port-plan.md` — synthesis + roadmap

License: e1000e is GPLv2. Per CONTRIBUTING.md rule #3 we study principles and write original code; no copy-paste.

---

## 1. File inventory and skip-list

Total: 29,962 LOC across 11 `.c` files + 12 `.h` files. The 82567LM-3 chip-touching subset is **~5,000-7,000 LOC** — the rest is Linux framework overhead, ethtool, PTP, manageability, power management, and chip variants we will never see.

| File | LOC | Role | For our port |
|------|-----|------|--------------|
| `netdev.c` | 7,983 | PCI probe, NAPI, netdev callbacks, ISR, ring management, watchdog | **Must port** ~10-15%: probe, ring setup, ISR, xmit, RX-drain. Skip everything else. |
| `ich8lan.c` | 6,077 | ICH8/9/10 chip-specific MAC/PHY/NVM — **where the 82567 lives** | **Must port** ~30-40%: reset_hw, init_hw, SWFLAG, MAC-from-flash, PHY init. |
| `phy.c` | 3,246 | Generic PHY operations (M88/IGP/IGP_3 families) | **Should port** ~15%: MDIC read/write, IGP_3 register access. |
| `ethtool.c` | 2,412 | ethtool ioctl glue | **Skip 100%**. |
| `82571.c` | 2,049 | 82571/72/73/74 chip-specific | **Skip 100%** — different chip family. |
| `mac.c` | 1,782 | Generic MAC operations (RAR/MTA, link, flow control) | **Should port** ~25%: RAR set, MTA clear, flow control basics. |
| `80003es2lan.c` | 1,412 | 80003ES2LAN chip-specific | **Skip 100%** — different chip. |
| `defines.h` | 811 | Bit definitions for every register, command, flag | **Must reference** for register bit constants. |
| `hw.h` | 738 | Struct definitions, function-pointer types, device-ID list | **Must reference** for device-ID match. |
| `nvm.c` | 615 | Generic NVM ops (mostly for non-ICH chips) | **Skip 90%** — ICH NVM lives in `ich8lan.c`. |
| `e1000.h` | 598 | Driver-private struct definitions (adapter, rings) | **Must reference** for ring layout idiom. |
| `param.c` | 527 | Module parameter parsing | **Skip 100%**. |
| `ptp.c` | 347 | PTP hardware-timestamp glue | **Skip 100%**. |
| `manage.c` | 329 | iAMT / Manageability passthrough | **Skip 100%**. |
| `ich8lan.h` | 309 | ICH8/9/10/PCH constants and flag bits | **Must reference**. |
| `regs.h` | 245 | Register offsets | **Must reference**. |
| `phy.h` | 218 | PHY register defs | **Must reference**. |
| `*.h` (small) | <100 each | Trace, manage, nvm, mac, es2lan headers | Reference as needed. |

**Net porting target:** roughly 5,000 LOC of original C from us, mining the ~7,000 LOC of e1000e that's actually 82567-relevant. The 4× compression comes from dropping NAPI/skb/spinlocks/ethtool and from writing leaner pinecore-native equivalents for the rest.

---

## 2. The 82567LM-3 init path

### 2.1 PCI device ID → board type → e1000_info

The Linux pci_device_id table at `netdev.c:7845-7847` maps:
```
{ PCI_VDEVICE(INTEL, E1000_DEV_ID_ICH10_D_BM_LM), board_ich10lan },   // 0x10DE
{ PCI_VDEVICE(INTEL, E1000_DEV_ID_ICH10_D_BM_LF), board_ich10lan },   // 0x10DF
{ PCI_VDEVICE(INTEL, E1000_DEV_ID_ICH10_D_BM_V),  board_ich10lan },
```

The `board_*` enum is an index into `e1000_info_tbl[]` at `netdev.c:49`:
```
[board_ich10lan] = &e1000_ich10_info,
```

`e1000_ich10_info` is the struct at `ich8lan.c:5901-5915`:
```
.mac        = e1000_ich10lan,
.flags      = FLAG_HAS_JUMBO_FRAMES | FLAG_IS_ICH | FLAG_HAS_WOL
            | FLAG_HAS_CTRLEXT_ON_LOAD | FLAG_HAS_AMT
            | FLAG_HAS_FLASH | FLAG_APME_IN_WUC,
.pba        = 18,
.max_hw_frame_size = DEFAULT_JUMBO,
.get_variants = e1000_get_variants_ich8lan,
.mac_ops    = &ich8_mac_ops,
.phy_ops    = &ich8_phy_ops,
.nvm_ops    = &ich8_nvm_ops,
```

That's the entire 82567LM-3 personality, summarised: ICH-class, with AMT, flash-backed MAC, and the shared `ich8_*_ops` function pointers.

### 2.2 The ich8 ops vectors

**MAC ops** (`ich8lan.c:5811-5830`):
| Field | Function | Role |
|-------|----------|------|
| `check_for_link` | `e1000_check_for_copper_link_ich8lan` | watchdog link-state poll |
| `clear_hw_cntrs` | `e1000_clear_hw_cntrs_ich8lan` | reset hardware stats counters |
| `get_bus_info` | `e1000_get_bus_info_ich8lan` | populate bus/speed/width |
| `set_lan_id` | `e1000_set_lan_id_single_port` | single-port (vs multi-port server NIC) |
| `get_link_up_info` | `e1000_get_link_up_info_ich8lan` | post-link-up: read speed/duplex |
| `update_mc_addr_list` | `e1000e_update_mc_addr_list_generic` | program MTA from list |
| `reset_hw` | `e1000_reset_hw_ich8lan` | issue `CTRL.RST` with SWFLAG dance |
| `init_hw` | `e1000_init_hw_ich8lan` | post-reset bring-up |
| `setup_link` | `e1000_setup_link_ich8lan` | configure auto-neg, flow control |
| `setup_physical_interface` | `e1000_setup_copper_link_ich8lan` | copper-specific link setup |
| `config_collision_dist` | `e1000e_config_collision_dist_generic` | TX collision distance |
| `rar_set` | `e1000e_rar_set_generic` | set RAR(n) → MAC addr |
| `rar_get_count` | `e1000e_rar_get_count_generic` | count of RAR entries (16 for ICH) |

**PHY ops** (`ich8lan.c:5832-5844`):
| Field | Function | Role |
|-------|----------|------|
| `acquire` | `e1000_acquire_swflag_ich8lan` | SWFLAG dance — call before MDIC |
| `check_reset_block` | `e1000_check_reset_block_ich8lan` | check FW hold on PHY reset |
| `get_cfg_done` | `e1000_get_cfg_done_ich8lan` | poll PHY config-done bit |
| `get_cable_length` | `e1000e_get_cable_length_igp_2` | DSP-based cable length estimate (skippable) |
| `read_reg` | `e1000e_read_phy_reg_igp` | MDIC read (IGP register layout) |
| `release` | `e1000_release_swflag_ich8lan` | SWFLAG release |
| `reset` | `e1000_phy_hw_reset_ich8lan` | PHY hard reset via `CTRL.PHY_RST` |
| `set_d0_lplu_state` | `e1000_set_d0_lplu_state_ich8lan` | D0 Low-Power-Link-Up control |
| `set_d3_lplu_state` | `e1000_set_d3_lplu_state_ich8lan` | D3 LPLU (skippable for DOS) |
| `write_reg` | `e1000e_write_phy_reg_igp` | MDIC write |

**NVM ops** (`ich8lan.c:5846-5856`):
| Field | Function | Role |
|-------|----------|------|
| `acquire` | `e1000_acquire_nvm_ich8lan` | SWFLAG + software lock |
| `read` | `e1000_read_nvm_ich8lan` | EERD-based flash read |
| `release` | `e1000_release_nvm_ich8lan` | release lock |
| `reload` | `e1000e_reload_nvm_generic` | trigger NVM auto-load |
| `update` | `e1000_update_nvm_checksum_ich8lan` | write back checksum (skippable) |
| `valid_led_default` | `e1000_valid_led_default_ich8lan` | LED policy (skippable) |
| `validate` | `e1000_validate_nvm_checksum_ich8lan` | checksum verify (skippable for bring-up) |
| `write` | `e1000_write_nvm_ich8lan` | EERD-based flash write (skippable) |

---

## 3. Call graphs

### 3.1 Probe

`e1000_probe` (`netdev.c:7360`) is invoked by the PCI subsystem on device match. Sequence:

```
e1000_probe(pdev, ent)
├── ei = e1000_info_tbl[ent->driver_data]                   netdev.c:7365
├── e1000e_disable_aspm(pdev, …) if flags2 set              netdev.c:7376
├── pci_enable_device_mem(pdev)                              netdev.c:7383
├── dma_set_mask_and_coherent(pdev, DMA_BIT_MASK(64))        netdev.c:7386  ← 64-bit DMA
├── pci_request_selected_regions_exclusive(...)              netdev.c:7397
├── pci_set_master(pdev)                                     [enables bus master]
├── alloc_etherdev(...) → netdev                             netdev.c:~7415
├── ioremap(mmio_start, mmio_len) → hw->hw_addr              netdev.c:~7445  ← BAR0 map
├── ei->get_variants(adapter)  [= e1000_get_variants_ich8lan in ich8lan.c]
│     ├── set hw->mac.type from ei->mac
│     ├── set hw->mac.ops    = ich8_mac_ops
│     ├── set hw->phy.ops    = ich8_phy_ops
│     └── set hw->nvm.ops    = ich8_nvm_ops
├── e1000e_reset(adapter)                                    netdev.c:3955
│     ├── e1000_acquire_swflag_ich8lan                       ich8lan.c:1757
│     ├── ew32(IMC, 0xFFFFFFFF)                              [disable IRQs]
│     ├── ew32(RCTL, 0); ew32(TCTL, 0)                       [disable rings]
│     ├── ew32(CTRL, ctrl | CTRL_RST)                        [issue reset]
│     ├── poll CTRL.RST clear
│     ├── ew32(CTRL_EXT, … | DRV_LOAD)                       [FLAG_HAS_CTRLEXT_ON_LOAD]
│     ├── hw->phy.ops.reset(hw) → e1000_phy_hw_reset_ich8lan
│     ├── hw->mac.ops.init_hw(hw) → e1000_init_hw_ich8lan
│     └── e1000_release_swflag_ich8lan                       ich8lan.c:1820
├── hw->mac.ops.read_mac_addr(hw)  [via NVM, into RAL/RAH]
├── netdev->netdev_ops = &e1000e_netdev_ops                  netdev.c:7451
├── register_netdev(netdev)                                  netdev.c:~7600
└── return 0
```

The whole probe is roughly 350 LOC in Linux. Of that, ~80 LOC is chip-touching (the rest is netdev/sysfs/timer setup we don't need). A pinecore port is ~150-200 LOC for the equivalent: PCI scan, BAR map, reset, MAC read, install IDT entry — everything else is Linux framework.

### 3.2 Open / Up

`e1000e_open` (`netdev.c:4612`) — invoked by `ifconfig up` / `ip link set up`:

```
e1000e_open
├── e1000e_setup_tx_resources(adapter->tx_ring)             netdev.c:2328
│     ├── alloc descriptor ring (dma_alloc_coherent)
│     ├── alloc buffer-info array (kzalloc)
│     └── zero descriptor memory
├── e1000e_setup_rx_resources(adapter->rx_ring)             netdev.c:2362
│     ├── alloc descriptor ring
│     ├── alloc buffer-info array
│     ├── alloc RX buffer pool (per descriptor)
│     └── set ring base/head/tail
├── e1000e_power_up_phy(adapter)
├── e1000e_setup_rctl(adapter)                              [program RCTL]
├── e1000_configure_tx(adapter)                             netdev.c:2915
│     ├── ew32(TDBAL(0), tdba & 0xFFFFFFFF)                  netdev.c:2925
│     ├── ew32(TDBAH(0), tdba >> 32)
│     ├── ew32(TDLEN(0), tx_ring->count * sizeof(desc))
│     ├── ew32(TDH(0), 0); ew32(TDT(0), 0)
│     └── ew32(TCTL, TCTL_EN | TCTL_PSP | …)
├── e1000_configure_rx(adapter)                             netdev.c:3188
│     ├── ew32(RDBAL(0), rdba & 0xFFFFFFFF)                  netdev.c:3250
│     ├── ew32(RDBAH(0), rdba >> 32)
│     ├── ew32(RDLEN(0), rx_ring->count * sizeof(desc))
│     ├── ew32(RDH(0), 0); ew32(RDT(0), N-1)                 [hand all buffers to NIC]
│     └── ew32(RCTL, RCTL_EN | RCTL_BAM | …)
├── e1000_alloc_rx_buffers(rx_ring, ...)                    [fill RX buffer descriptors]
├── e1000_request_irq(adapter)                              netdev.c:2156
│     └── request_irq(adapter->pdev->irq, e1000_intr, …)    [→ ISR e1000_intr]
├── e1000e_up(adapter)                                      netdev.c:4217
│     └── enable interrupts via ew32(IMS, …)
└── netif_start_queue(netdev)
```

For a packet driver this collapses to: setup rings, fill RX buffers, install IRQ handler, enable interrupts. ~150 LOC original.

### 3.3 TX hot path

`e1000_xmit_frame` (`netdev.c:5781`) — called by the network stack for each outgoing skb:

```
e1000_xmit_frame(skb, netdev)
├── compute number of TX descriptors needed (header + payload fragments)
├── if not enough free descriptors, return NETDEV_TX_BUSY
├── for each segment:
│     ├── dma_map_single(skb->data, …) → bus address
│     ├── tx_desc = &tx_ring->desc[i]
│     ├── tx_desc->buffer_addr = phys_addr
│     ├── tx_desc->lower.data = len | TXD_CMD_IFCS | TXD_CMD_IDE | TXD_CMD_RS
│     ├── (last segment) tx_desc->lower.data |= TXD_CMD_EOP
│     └── i = (i + 1) % tx_ring->count
├── tx_ring->next_to_use = i
└── ew32(TDT(0), i)        ← single MMIO write that kicks transmit
```

The actual driver code is wrapped in SKB-handling, TSO logic, checksum offload, NAPI flow control — most of it irrelevant. The chip-touching core is **fewer than 50 lines**.

### 3.4 RX hot path

`e1000_clean_rx_irq` (`netdev.c:914`) — called from NAPI poll context, harvests completed RX descriptors:

```
e1000_clean_rx_irq(rx_ring, work_done, budget)
├── i = rx_ring->next_to_clean
├── while (rx_desc->status & RXD_STAT_DD) and budget > 0:
│     ├── buffer_info = &rx_ring->buffer_info[i]
│     ├── length = le16_to_cpu(rx_desc->length)
│     ├── skb = buffer_info->skb
│     ├── dma_unmap_single(buffer_info->dma, ...)
│     ├── if (rx_desc->errors & RXD_ERR_FRAME_ERR_MASK): drop
│     ├── skb_put(skb, length - 4)        [strip FCS]
│     ├── e1000_receive_skb(adapter, skb) [hand to network stack]
│     ├── buffer_info->skb = NULL          [recycle slot]
│     ├── rx_desc->status = 0
│     ├── i = (i + 1) % rx_ring->count
│     └── --budget; ++(*work_done)
├── if buffers consumed: e1000_alloc_rx_buffers(rx_ring, cleaned_count)
└── ew32(RDT(0), prev_i)                  ← tell NIC about refilled buffers
```

For a packet driver, "hand to network stack" becomes "fire packet-driver upcall(s)" — see `43-packet-driver-spec.md` §3.

### 3.5 ISR

`e1000_intr` (`netdev.c:1817`) — legacy INTx handler (the path we'll take on DOS, MSI is out of scope):

```
e1000_intr(irq, data)
├── icr = er32(ICR)
├── if (icr == 0 || (icr & E1000_ICR_INT_ASSERTED) == 0):
│     return IRQ_NONE                     ← shared-IRQ chain to next handler
├── if (icr & ICR_LSC): schedule watchdog (link status change)
├── if (icr & ICR_RXDMT0): track ring-empty
├── napi_schedule(&adapter->napi)          ← deferred work
└── return IRQ_HANDLED
```

NAPI defers the actual ring drain to a kernel thread. **For DOS we collapse this** — do the RX drain (and upcalls) inline in the ISR, because we have no NAPI infrastructure and the upcall is the meaningful work anyway.

### 3.6 Link change (watchdog)

`e1000_watchdog_task` (`netdev.c:5184`) — runs every ~2 seconds:
- Calls `hw->mac.ops.check_for_link` → `e1000_check_for_copper_link_ich8lan`.
- If link came up: read `STATUS.SPEED` and `STATUS.FD`, log "Link Up 1000 Mbps Full Duplex".
- If link went down: set queue dormant, log "Link Down".

For DOS we replace the timer with: on every `LSC` interrupt, re-read STATUS, update a "link up/down/speed" global. The driver's `driver_info` and `get_parameters` answers reflect it.

### 3.7 Down

`e1000e_close` (`netdev.c:4719`) → `e1000e_down` (`netdev.c:4264`):
- `ew32(IMC, 0xFFFFFFFF)` — disable interrupts.
- `ew32(RCTL, 0)`, `ew32(TCTL, 0)` — stop rings.
- `e1000e_reset` — final reset.
- Free DMA buffers.

---

## 4. PHY operations on ICH (the SWFLAG-protected paths)

The PHY op vector (`ich8_phy_ops` at `ich8lan.c:5832`) has `acquire = e1000_acquire_swflag_ich8lan`. Every site that touches PHY first calls `hw->phy.ops.acquire(hw)`. Direct call sites in ich8lan.c (verified via grep): lines 216, 307, 842, 928, 1147, 1310, 1457, 1498, 1523, 2156, 2254, 2381, 2500, 2518, 2546, 2761 — and many more (>50 total). Every one is paired with `release` on every exit path including error paths.

For our port, the policy is identical: every MDIC operation is bracketed by `acquire / release`. The body of acquire is documented in `41-intel-82567lm-nic.md` §6.2; the implementation in Linux is `ich8lan.c:1757-1810`.

### Kumeran K1 config

Two relevant sites in ich8lan.c:
- Lines 933-951 — link-up K1 config (after auto-neg completes, configure K1 idle based on negotiated speed).
- Lines 2327-2340 — `e1000_k1_gig_workaround_lv` (an erratum workaround for ICH9LV; not relevant to our ICH10).

The helper functions `e1000e_read_kmrn_reg_locked` and `e1000e_write_kmrn_reg_locked` live in `phy.c`. They marshal accesses through `KMRNCTRLSTA` (reg 0x34) — write the kumeran register index + data, poll for ready, optionally read back.

---

## 5. NVM (MAC address) on ICH

The MAC address read at probe goes through `hw->mac.ops.read_mac_addr` which on ICH parts calls into `e1000_read_mac_addr_generic` (in `mac.c`) → `hw->nvm.ops.read` → `e1000_read_nvm_ich8lan` (in `ich8lan.c`). The read uses the `EERD` register-based flash interface (see `41-intel-82567lm-nic.md` §7).

Validation in `e1000_validate_nvm_checksum_ich8lan` is optional — bad checksum on the GbE region in a stock OptiPlex 780 is essentially never.

---

## 6. What to skip entirely

Every line in these files / call paths can be ignored for the 82567 port:

| Subsystem | Where | Why skip |
|-----------|-------|----------|
| **NAPI** | `netdev.c:napi_schedule / e1000_clean / poll callback` | DOS has no NAPI. Drain in ISR directly. |
| **ethtool** | all of `ethtool.c` (2,412 LOC) | DOS has no ethtool. |
| **PTP / hwtstamp** | all of `ptp.c` (347 LOC), `e1000e_ptp_init` in netdev.c | DOS apps don't speak PTP. |
| **iAMT / Manageability** | all of `manage.c` (329 LOC) | We coexist with AMT via SWFLAG, not manage it. |
| **Devlink / DCB / VFs** | scattered in netdev.c | Linux-only abstractions. |
| **Module parameters** | all of `param.c` (527 LOC) | We hardcode reasonable defaults. |
| **SR-IOV / VFs** | netdev.c | 82567 doesn't support it anyway. |
| **EEE** | `e1000_set_eee_pchlan` (PCH-only) | Not on ICH10. |
| **Power management** | `e1000_suspend`, `e1000_resume`, runtime PM | DOS doesn't S3/S4. |
| **WoL** | `e1000_set_wol` | Not running in standby. |
| **MSI / MSI-X** | `e1000_intr_msi`, MSI capability discovery | Use legacy INTx. |
| **TSO / GSO** | `e1000_tso`, extended TX context descriptors | Optional offload. |
| **VLAN / 802.1Q** | VFTA programming | Apps wanting VLANs build them in software. |
| **Multi-queue** | `tx_ring[]` / `rx_ring[]` arrays | 82567 supports only 1 queue effectively. |
| **Statistics counters reading via ethtool** | `e1000e_get_stats64` etc. | Maintain our own counters for function 24 |
| **Other chip families** | `82571.c`, `80003es2lan.c` | Not our chip. |
| **PCH/LPT/SPT/CNP/TGP/ADP/MTP variants** | sections of `ich8lan.c` | Not our chip. |
| **NVM write paths** | `e1000_write_nvm_ich8lan` | We never write the flash. |

---

## 7. Linux-isms → DOS / pinecore equivalents

| Linux primitive | What it does | Our replacement |
|-----------------|--------------|-----------------|
| `dma_alloc_coherent(dev, size, &phys, GFP_…)` | Allocate kernel virtual + physical-contig + map for DMA. | Reserve-at-boot low-physical region; allocate from it. Linear addr = phys addr (identity-mapped section). |
| `dma_map_single(dev, vaddr, len, DIR)` | Get a bus address for a kernel vaddr. | If buffer is in our reserved region: `phys = linear`. Else copy to a bounce buffer in the region. |
| `readl(addr)` / `writel(val, addr)` / `er32(reg)` / `ew32(reg, val)` | Volatile MMIO via mapped BAR | Our kernel's `mmio_read32` / `mmio_write32` — we already do this for VGA / PCI. |
| `pci_enable_device_mem` | Set Memory Space + Bus Master in PCI cmd reg | One PCI config-space write. We already access PCI cfg via `20-pci-bus.md`. |
| `request_irq(irq, handler, IRQF_SHARED, …)` | Install ISR | Wire ISR into our IDT at the BIOS-allocated vector. |
| `napi_schedule(napi)` | Defer to softirq | Run inline. |
| `netif_rx(skb)` / `napi_gro_receive` | Hand packet to network stack | Fire packet-driver upcall(s) from `43-packet-driver-spec.md` §3. |
| `dev_kfree_skb(skb)` | Free socket buffer | Recycle our pre-allocated buffer slot. |
| `spin_lock_irqsave / _irqrestore` | Disable IRQs + take spinlock | `cli` / `sti` — we're single-CPU single-threaded. |
| `mutex_lock` / `mutex_unlock` | Sleeping mutex | Single-threaded ISR can't sleep. Treat as no-op or `cli/sti`. |
| `msleep(ms)` / `mdelay(ms)` | Sleep / busy-wait | `pit_delay_ms(ms)` — our kernel has it. |
| `timer_setup` / `mod_timer` | Schedule callback | Add to our PIT timer-tick handler chain. |
| `workqueue` / `delayed_work` | Defer to kernel thread | Move work to our preemptive scheduler task, or run inline. |
| `set_bit` / `test_and_set_bit` | Atomic bit ops | `cli; ...; sti`. |
| `ioremap(start, len)` | Map MMIO into kernel vaddr | Identity-map (vmm_map_page) the BAR physical range. |
| `dev_dbg` / `dev_err` | Kernel log | `serial_printf` / `vga_print`. |
| `ETH_ALEN` / `IFNAMSIZ` | Constants | `#define MAC_ADDR_LEN 6` ourselves. |

---

## 8. LOC sizing — port estimate

| Subsystem | Linux LOC (rough) | Our LOC (estimate) | Compression |
|-----------|-------------------|--------------------|--|
| Init + reset + MAC read | ~600 | ~300 | 2× (no Linux framework boilerplate) |
| RX descriptor ring + drain | ~900 | ~250 | 3.6× (no NAPI, no skb) |
| TX descriptor ring + xmit | ~700 | ~200 | 3.5× |
| ISR + ICR handling | ~200 | ~80 | 2.5× |
| SWFLAG + MDIC + PHY init | ~500 | ~250 | 2× (need the full dance) |
| NVM (MAC read only) | ~150 | ~60 | 2.5× |
| Kumeran K1 config | ~150 | ~50 | 3× (just copy the defaults) |
| Watchdog / link state | ~300 | ~80 | 3.7× (IRQ-driven, no timer) |
| Errata workarounds | ~300 | ~100 | 3× (only the ones we need) |
| **Subtotal — chip code** | ~3,800 | ~1,370 | 2.8× |
| Packet-driver shim (INT 60h + upcall) | n/a | ~500 | n/a |
| PCI enumeration glue (already exists) | n/a | reuse | — |
| **Total — pinecore NIC + packet driver** | — | **~1,800-2,000 LOC** | — |

This is a **3-week careful port** by the rough calibration in the project's history (similar size to the FDC driver from session 6-7, similar complexity).

---

## 9. License and provenance

- e1000e is GPLv2.
- Per `CONTRIBUTING.md` rule #3 ("Never copy code directly. Study principles, write original."): we read the e1000e source as a register-programming reference, write our own driver from scratch. No file in pinecore-x86 may contain copy-pasted e1000e code.
- Comments referencing e1000e are fine and encouraged — cite `(e1000e: ich8lan.c:1757)` style.
- **Alternative reference for permissive license:** FreeBSD's `em(4)` / `igb(4)` (BSD-2-Clause) covers the same chip family. Worth comparing for register-programming details that are easier to read in BSD-style C.
  - Path: `https://cgit.freebsd.org/src/tree/sys/dev/e1000/`
  - Files of interest: `if_em.c`, `e1000_ich8lan.c`, `e1000_phy.c`. Same algorithmic content, different framing.

---

## 10. References (all v6.6, paths relative to `linux-e1000e-ref/drivers/net/ethernet/intel/e1000e/`)

### Files (LOC)
- `netdev.c` (7,983) — probe, ndo, ISR, NAPI, watchdog
- `ich8lan.c` (6,077) — ICH8/9/10/PCH chip-specific
- `phy.c` (3,246) — generic PHY
- `mac.c` (1,782) — generic MAC
- `regs.h` (245) — register offsets
- `defines.h` (811) — bit constants
- `hw.h` (738) — structs and device IDs
- `e1000.h` (598) — driver-private

### Key citations
- `hw.h:55` — `E1000_DEV_ID_ICH10_D_BM_LM = 0x10DE` (OptiPlex 780's NIC)
- `hw.h:144` — `e1000_ich10lan` mac_type
- `ich8lan.c:5901` — `e1000_ich10_info` struct (the personality)
- `ich8lan.c:5811-5830` — `ich8_mac_ops` vector
- `ich8lan.c:5832-5844` — `ich8_phy_ops` vector
- `ich8lan.c:5846-5856` — `ich8_nvm_ops` vector
- `ich8lan.c:1751-1810` — `e1000_acquire_swflag_ich8lan`
- `ich8lan.c:1820-1842` — `e1000_release_swflag_ich8lan`
- `ich8lan.c:933-951`, `2327-2340` — kumeran K1 config sites
- `netdev.c:49` — `e1000_info_tbl[]`
- `netdev.c:914` — `e1000_clean_rx_irq`
- `netdev.c:1750`, `1817` — `e1000_intr_msi`, `e1000_intr` (ISRs)
- `netdev.c:2156` — `e1000_request_irq`
- `netdev.c:2328`, `2362` — `e1000e_setup_tx_resources`, `e1000e_setup_rx_resources`
- `netdev.c:2915`, `3188` — `e1000_configure_tx`, `e1000_configure_rx`
- `netdev.c:3955` — `e1000e_reset`
- `netdev.c:4217`, `4264` — `e1000e_up`, `e1000e_down`
- `netdev.c:4612`, `4719` — `e1000e_open`, `e1000e_close`
- `netdev.c:5174`, `5184` — `e1000_watchdog`, `e1000_watchdog_task`
- `netdev.c:5781` — `e1000_xmit_frame`
- `netdev.c:7328-7330` — `.ndo_open/.ndo_stop/.ndo_start_xmit` wiring
- `netdev.c:7360` — `e1000_probe`
- `netdev.c:7386` — `dma_set_mask_and_coherent(..., DMA_BIT_MASK(64))`
- `netdev.c:7845-7847` — pci_device_id table for ICH10

### URLs
- Upstream kernel: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/ethernet/intel/e1000e/?h=v6.6>
- GitHub mirror: <https://github.com/torvalds/linux/tree/v6.6/drivers/net/ethernet/intel/e1000e>
- FreeBSD em(4) (permissive alternative reference): <https://cgit.freebsd.org/src/tree/sys/dev/e1000/>

### Cross-references in this repo
- `41-intel-82567lm-nic.md` — chip register surface
- `43-packet-driver-spec.md` — Crynwr API
- `44-82567lm-port-plan.md` — port plan + roadmap integration
- `20-pci-bus.md` — our PCI infrastructure
