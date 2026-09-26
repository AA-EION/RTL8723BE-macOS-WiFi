# Realtek RTL8723BE PCIe Wi-Fi Chip Specification

## 1. Executive Summary & Hardware Overview

The Realtek **RTL8723BE** is a highly integrated single-chip 802.11b/g/n 1T1R Wireless LAN and Bluetooth 4.0 combo network controller. In the target host system, the Wi-Fi controller is connected via PCI Express (`pci10ec,b723`, subsystem `103c:804c`, PCI bus address `2:0:0`), while Bluetooth is routed via USB.

### Key Hardware Attributes
- **PCI Vendor ID**: `0x10EC` (Realtek Semiconductor Corp.)
- **PCI Device ID**: `0xB723` (RTL8723BE 802.11b/g/n PCIe Wireless Network Adapter)
- **Subsystem Vendor ID**: `0x103C` (HP Inc.)
- **Subsystem Device ID**: `0x804C`
- **PCI Capabilities**: MSI (Message Signaled Interrupts), IO-APIC legacy pin interrupts, PCI Express ASPM (L0s/L1) and Power Management (D0..D3).
- **Base Address Registers (BARs)**:
  - **BAR0**: 256-byte Legacy I/O port space (`0x0000..0x00FF`).
  - **BAR2**: 16,384-byte (16 KB, `0x4000`) 64-bit Memory-Mapped I/O (MMIO) region (`0x0000..0x3FFF`).
- **Internal MCU**: Realtek proprietary 8051-compatible microcontroller running on-chip firmware (`rtl8723befw.bin` / `rtl8723befw_36.bin`).
- **RF Architecture**: 1T1R (1 Transmit, 1 Receive antenna path), 2.4 GHz ISM band only (Channels 1–13/14), 20 MHz and 40 MHz channel bandwidths.
- **DMA Engine**: Multi-queue scatter-gather DMA engine supporting 64-bit physical addressing, descriptor ring architecture, and hardware packet aggregation (A-MPDU).

---

## 2. Features Discovered

| # | Category | Feature | Description | Inputs | Outputs | Error Behavior | Discovered Via |
|---|----------|---------|-------------|--------|---------|----------------|----------------|
| 1 | PCIe Interface | BAR2 64-bit MMIO Access | 16KB MMIO address space for all MAC, BB, RF, DMA, and MCU registers | Physical base address from PCI config BAR2 | 8/16/32-bit MMIO read/write access | Bus fault if access is unaligned or BAR unmapped | Linux `pci.c`, OpenBSD `if_rtwn.c` |
| 2 | PCIe Interface | PCI Config Power & Bus Master Setup | Disable PCIe ASPM clock request (`0x81`), wake from D3 (`0x44`), enable Bus Master & Memory (`0x04`) | PCI configuration space registers `0x04`, `0x44`, `0x81` | PCI bus mastering enabled, MMIO responding | Device unresponsive in D3 state | Linux `pci.c:2184-2190` |
| 3 | Power Management | Power-On Sequence State Machine (`pwrseq`) | Step-by-step state machine transitioning chip from Card Disable / Card Emulation to Active mode | Array of `struct wlan_pwr_cfg` (`rtl8723B_card_enable_flow`) | Hardware clocks, LDOs, and internal power domains activated | Polling timeout (50ms) if voltage rails fail to stabilize | Linux `pwrseqcmd.h`, `rtl8723be/pwrseq.c` |
| 4 | Power Management | Card Disable Sequence | Puts chip into low power mode, stops DMA, shuts down RF and puts MCU into reset | `rtl8723B_card_disable_flow`, `REG_RSV_CTRL (0x1C)` = `0x0E` | Clocks disabled, analog blocks isolated | Leaks power if isolation bit not set | Linux `rtl8723be/hw.c:1150-1200` |
| 5 | Registers | Command Register (`REG_CR`, `0x0100`) | Enables HCI TX/RX DMA, protocol engine, scheduler, MAC TX/RX, software beacon, and security | Word write `0x02FF` to `0x0100` | MAC and DMA sub-engines enabled | Hardware halts DMA if CR cleared | OpenBSD `r92creg.h:351`, Linux `hw.c:856` |
| 6 | Registers | Interrupt Mask & Status (`HIMR`, `HISR`, `0x00B0..0x00B4`) | Manages primary interrupt line for RX OK, RDU, TX queue completions (VO, VI, BE, BK, MGNT, HIGH), C2H | 32-bit bitmask in `REG_HIMR` (0x00B0) | Interrupt signaled via MSI / line; acknowledge via write-1-to-clear in `REG_HISR` | Interrupt storm if not cleared or unmasked unhandled bits | Linux `rtl8723be/reg.h:548-575` |
| 7 | Registers | Extended Interrupt Mask/Status (`HIMRE`, `HISRE`, `0x00B8..0x00BC`) | Manages secondary interrupt line for RX/TX FIFO overflow, RX/TX error flags | 32-bit bitmask in `REG_HIMRE` | Write-1-to-clear in `REG_HISRE` | Unacknowledged overflow blocks subsequent notifications | Linux `rtl8723be/reg.h:577-595` |
| 8 | Registers | System Clock & Function Enable (`SYS_CLKR`, `SYS_FUNC_EN`, `0x0008`, `0x0002`) | Controls analog clocks, CPU clock, MAC clock, PCIe DMA reset | Register writes to `0x0002` and `0x0008` | Clocks gated / ungated | Chip lockup if clock gated during DMA | Linux `rtl8723be/reg.h:60`, OpenBSD `r92creg.h:208` |
| 9 | Firmware Loader | Firmware Binary Structure | Parses 32-byte header: signature `0x5301`, version, subversion, date, ramcodesize | File `rtl8723befw.bin` buffer | Header validated, pointer advanced 32 bytes | Signature mismatch rejected if `(sig & 0xFFF0) != 0x5300` | Linux `rtl8723be/sw.c:185`, `wifi.h:651` |
| 10 | Firmware Loader | 8051 MCU RAM Page Download | Downloads microcode into MCU RAM via 4KB pages written to MMIO address `0x1000..0x1FFF` | Page index in `REG_MCUFWDL+2` (bits 2:0); payload bytes written to `0x1000` | RAM programmed page by page (up to 8 pages) | Page index > max_page (8) rejected | Linux `rtl8723com/fw_common.c:38-70` |
| 11 | Firmware Loader | Checksum & Firmware Ready Handshake | Validates on-chip checksum report, resets 8051 MCU, and polls for firmware ready status | Polling `REG_MCUFWDL (0x0080)` for `FWDL_CHKSUM_RPT` (bit 2) and `WINTINI_RDY` (bit 6) | Firmware ready state `rtlhal->fw_ready = true` | Checksum timeout or ready timeout returns `-EIO` | Linux `rtl8723com/fw_common.c:132-177` |
| 12 | eFuse | Autoload Status Detection | Checks whether hardware automatically loaded EEPROM/eFuse contents at power-up | `REG_9346CR (0x000A)` bits 4 (EEPROM vs eFuse) and 5 (Autoload OK) | Autoload success flag | Hardware uses hardcoded fallback defaults if autoload fails | Linux `rtl8723be/hw.c:2050-2070` |
| 13 | eFuse | Manual eFuse Physical Readout | Performs physical bit-level readout of eFuse array via command register | Write address to `REG_EFUSE_CTRL+1..+2`, write `0x72` to `+3`, poll bit 7 | Read byte returned at `REG_EFUSE_CTRL (0x0030)` | Timeout (100 iterations) returns `0xFF` | Linux `rtlwifi/efuse.c:375-408` |
| 14 | eFuse | PG Packet Parsing (Logical Map Reconstruction) | Decodes variable-length programming packets (standard 1-byte header & extended 2-byte header) into 512B map | Raw physical eFuse byte stream | Decoded 512-byte logical EEPROM shadow table | Corrupt/unterminated header aborts parsing | Linux `rtlwifi/efuse.c:215-320` |
| 15 | eFuse | Calibration Data Layout | Extracts MAC address (`0xD0`), crystal cap (`0xB9`), thermal meter (`0xBA`), TX power per channel (`0x10..0x2D`) | 512-byte logical EEPROM shadow table | Populated `struct rtl_efuse` calibration fields | Fallback to defaults (crystal 0x20, thermal 0x1A, power 0x2D) | Linux `rtl8723be/hw.c:2070-2150`, `reg.h:660-700` |
| 16 | TX DMA Engine | 40-Byte TX Buffer Descriptor | Encapsulates packet length, header offset, queue selection, rate, HW seq, and 64-bit physical address | Memory allocated by host, packet buffer physical address | DMA reads descriptor and transmits frame over RF | Ownership bit collision if CPU writes while OWN=1 | Linux `rtl8723be/trx.h:17-105`, `260-350` |
| 17 | TX DMA Engine | Hardware Queue Priority Rings | Dedicated rings for BK (0), BE (1), VI (2), VO (3), Beacon (4), Management (6), High (7) | Queue start physical address written to `REG_*Q_DESA (0x0308..0x0338)` | Hardware arbitration based on EDCA / priority | Queue stalls if descriptor ring pointers misaligned | Linux `rtl8723be/hw.c:880-896`, `reg.h:143-150` |
| 18 | TX DMA Engine | Doorbell / Polling Trigger | Signals DMA engine that a pending TX descriptor is available for transmission | Write queue bit to `REG_PCIE_CTRL_REG (0x0300)` | DMA initiates memory read from descriptor ring | No transmission if doorbell write omitted | Linux `rtl8723be/trx.c:700-715` |
| 19 | RX DMA Engine | 32-Byte RX Buffer Descriptor | Circular ring receiving frames from air; flags CRC error, ICV error, packet length, shift offset, PHY status | Pre-allocated coherent DMA buffers; physical address in descriptor dwords 6-7 | Hardware writes frame + descriptor fields; clears OWN bit (0) | Dropped packets (RDU interrupt) if ring exhausted | Linux `rtl8723be/trx.h:107-160`, `355-400` |
| 20 | RX DMA Engine | End-of-Ring (`EOR`) Wrapping | Bit 30 of dword 0 in last descriptor of RX ring indicates ring boundary | Set `EOR` bit on index `N-1` | Hardware DMA wraps back to index 0 | DMA runs past end of ring if EOR omitted | Linux `rtl8723be/trx.h:122`, `pci.c:1270` |
| 21 | Baseband | Linked List Table (`LLT`) Initialization | Allocates on-chip packet buffer memory pages between normal queues (0..244) and reserved/beacon ring (245..255) | Register writes to `REG_LLT_INIT (0x01E0)`, boundary `245`, max `255` | Internal buffer pages linked into circular chains | TX FIFO overflow if LLT initialization fails | Linux `rtl8723be/hw.c:740-790` |
| 22 | Baseband & RF | Baseband & Radio Register Initialization | Bulk loading of pre-calibrated register-value tables for MAC, PHY, AGC, and Radio Path A | Tables: `MAC_1T`, `PHY_REG_1T`, `AGCTAB_1T`, `RADIOA_1T` | BB filter parameters, AGC tables, and RF synthesizer loaded | Missing or incorrect tables causes zero sensitivity | Linux `rtl8723be/table.c:1-300`, `phy.c:86-130` |
| 23 | Radio Frequency | 3-Wire Serial RF Register Read/Write | Indirect access to RF synthesizer registers over BB register `0x840` (`RFPGA0_XA_LSSIPARAMETER`) | RF register offset, 20-bit data value | Data written/read across 3-wire LSSI serial bus | Bus contention if accessed without spinlock | Linux `rtl8723com/phy_common.c:65-130` |
| 24 | Radio Frequency | Channel Frequency Setting (Channels 1–13) | Tunes RF synthesizer to 2.4 GHz center frequency via RF register `0x18` (`RF_CHNLBW`) | Channel number (1..13), bandwidth mode (20 MHz / 40 MHz) | Carrier frequency locked to target channel | Out-of-band transmission if channel > 14 | Linux `rtl8723be/phy.c:1337-1420`, `rf.c:15-35` |
| 25 | Radio Frequency | Antenna Path Switch (`0x92C`) | Selects RF front-end antenna path between Main (0x1) and Aux (0x2) for 1T1R hardware | Write `0x1` (Main) or `0x2` (Aux) to BB register `0x92C` | RF path active on selected physical antenna connector | Complete signal loss if set to disconnected antenna port | Linux `rtl8723be/phy.c:2237-2250` |
| 26 | MCU Messaging | Host-to-Controller Mailbox (`H2C`) | 4-channel command mailbox system sending power management, scan, keepalive, and reserved page commands | 4-byte / 8-byte command payload in `REG_HMEBOX_0..3` (`0x01D0..0x01FC`) | Firmware receives command; clears bit in `REG_HMETFR (0x01CC)` | Command dropped if written while previous command pending | Linux `rtl8723be/fw.c:50-130` |
| 27 | MCU Messaging | Controller-to-Host Events (`C2H`) | Asynchronous events reported by MCU via registers `0x01A0..0x01AF` or special RX descriptor packet | Read from `REG_C2HEVT_MSG_NORMAL` (0x01A0), clear via `0x01AF` | Power state reports, rate adaptation reports, scan status | Event overrun if not acknowledged via `0x01AF` | Linux `rtl8723be/reg.h:106-108`, `trx.c:327` |
| 28 | Security Engine | Hardware CAM (Content Addressable Memory) | On-chip hardware encryption/decryption engine for WEP, TKIP, and AES-CCMP | Key data, key index, MACID, algorithm in `REG_CAMWRITE (0x0674)`, command `0x0670` | Wire frames automatically encrypted on TX / decrypted on RX | Decryption failure (ICV error) if CAM entry invalid | Linux `rtlwifi/cam.c:15-200`, `reg.h:170` |

---

## 3. Detailed Technical Subsystems

### 3.1 PCI Configuration & MMIO Architecture

```
+-----------------------------------------------------------------------------------+
| RTL8723BE PCIe Device (0x10EC:0xB723)                                             |
+-----------------------------------------------------------------------------------+
| PCI Config Space (256 bytes)                                                      |
|   0x04: PCI Command (Set 0x0007: Bus Master + Memory Space + I/O Space)           |
|   0x44: Power Management Control/Status (Write 0x00 to wake from D3hot)          |
|   0x81: PCIe Link Control / Clock Request (Write 0x00 to disable ClkReq)          |
+-----------------------------------------------------------------------------------+
| BAR0: Legacy I/O Port (256 bytes) [Unused in macOS modern drivers]                |
+-----------------------------------------------------------------------------------+
| BAR2: 64-bit Memory-Mapped I/O (16 KB = 0x4000 bytes at base 0xF1100000)          |
|   0x0000 - 0x00FF: System, Power, Clock, eFuse, and Interrupt Control             |
|   0x0100 - 0x01FF: MAC Control, MCU Firmware Download, H2C/C2H Mailboxes, LLT     |
|   0x0200 - 0x02FF: Timer, TSF, and Power Save Control                             |
|   0x0300 - 0x03FF: PCIe DMA Control, Descriptors Start Addresses (DESA), Polling  |
|   0x0400 - 0x05FF: Protocol, Frame Control, Beacon & EDCA Parameters              |
|   0x0600 - 0x06FF: TCR, RCR, CAM Hardware Encryption Engine                       |
|   0x0800 - 0x0FFF: Baseband PHY Registers, AGC Tables, 3-Wire LSSI RF Port       |
|   0x1000 - 0x1FFF: MCU 8051 RAM Download Window (4KB per page)                   |
+-----------------------------------------------------------------------------------+
```

#### Core Register Map

| Register Name | Offset | Size | Description | Key Bits / Values |
|---|---|---|---|---|
| `REG_SYS_FUNC_EN` | `0x0002` | 16-bit | System Function Enable | Bit 0: PCIe DMA enable; Bit 2: CPU/MCU reset & clock enable |
| `REG_SYS_CLKR` | `0x0008` | 16-bit | System Clock Register | Bit 3: MAC clock enable; Bit 11: Ring enable |
| `REG_9346CR` | `0x000A` | 8-bit | EEPROM / eFuse Command & Status | Bit 4: Boot from EEPROM (1) / eFuse (0); Bit 5: Autoload OK (1) / Fail (0) |
| `REG_RSV_CTRL` | `0x001C` | 16-bit | Reserved / Power Lock Control | `0x001C` write `0x00`: Unlock power/clock regs; write `0x0E`: Lock |
| `REG_RF_CTRL` | `0x001F` | 8-bit | RF Control Register | `0x00`: RF power off; `0x03`: RF power on |
| `REG_EFUSE_CTRL` | `0x0030` | 32-bit | eFuse Controller | [7:0] Data; [17:8] Address; [30:24] Mode (`0x72` read, `0xF2` write); [31] Busy |
| `REG_EFUSE_TEST` | `0x0034` | 32-bit | eFuse Test & Bank Selection | Bank select bits [9:8] |
| `REG_HSIMR` | `0x0058` | 32-bit | High-Speed Interrupt Mask | Bit 6: Radio ON/OFF; Bit 7: Power Down INT enable |
| `REG_HSISR` | `0x005C` | 32-bit | High-Speed Interrupt Status | Write-1-to-clear status bits matching HSIMR |
| `REG_MCUFWDL` | `0x0080` | 32-bit | MCU Firmware Download Control | Bit 0: FW DL enable; Bit 1: MCU ready; Bit 2: Checksum report OK; Bit 6: Firmware init ready; Bit 7: FW reset |
| `REG_MCUFWDL+2` | `0x0082` | 8-bit | MCU Firmware Page Select | Bits [2:0]: Page index (0..7) |
| `REG_HIMR` | `0x00B0` | 32-bit | Host Interrupt Mask Register | Bit 0: ROK (Rx OK); Bit 1: RDU; Bits 2-7: TX OK (VO, VI, BE, BK, MGNT, HIGH); Bit 10: C2HCMD |
| `REG_HISR` | `0x00B4` | 32-bit | Host Interrupt Status Register | Status flags matching HIMR; write-1-to-clear |
| `REG_HIMRE` | `0x00B8` | 32-bit | Host Interrupt Mask Extension | Bit 8: RXFOVW (Rx FIFO overflow); Bit 9: TXFOVW; Bit 10: RXERR; Bit 11: TXERR |
| `REG_HISRE` | `0x00BC` | 32-bit | Host Interrupt Status Extension | Status flags matching HIMRE; write-1-to-clear |
| `REG_EFUSE_ACCESS` | `0x00CF` | 8-bit | eFuse Protection Switch | Write `0x69`: Enable eFuse read/write; write `0x00`: Disable |
| `REG_CR` | `0x0100` | 16-bit | Main Command Register | `0x0100`: TRX enable (`0xFF`); `0x0100` (word): `0x02FF` (TRX + MAC enable) |
| `REG_C2HEVT_MSG_NORMAL` | `0x01A0` | 32-bit | C2H Event Message Buffer | MCU-to-Host event payload |
| `REG_C2HEVT_CLEAR` | `0x01AF` | 8-bit | C2H Event Clear Trigger | Write `0x00` to acknowledge and clear C2H event |
| `REG_HMETFR` | `0x01CC` | 32-bit | Host-to-MCU Mailbox Status | Bits [3:0]: Mailbox 0..3 empty status (0 = ready for host write) |
| `REG_HMEBOX_0..3` | `0x01D0` | 32-bit each | H2C Command Mailbox 0..3 | Dword 0 of command payload (triggers MCU interrupt) |
| `REG_LLT_INIT` | `0x01E0` | 32-bit | Linked List Table Operation | [7:0] Data; [15:8] Page Addr; [31:30] Op (`1` write, `0` idle) |
| `REG_HMEBOX_EXT_0..3` | `0x01F0` | 32-bit each | H2C Command Mailbox Ext 0..3 | Dword 1 of command payload (written before HMEBOX) |
| `REG_PCIE_CTRL_REG` | `0x0300` | 16-bit | PCIe Queue Doorbell / Polling | Bit 0: BK; Bit 1: BE; Bit 2: VI; Bit 3: VO; Bit 4: BCN; Bit 6: MGNT; Bit 7: HIGH |
| `REG_INT_MIG` | `0x0304` | 32-bit | Interrupt Mitigation | Write `0x00000000` to disable interrupt coalescing |
| `REG_BCNQ_DESA` | `0x0308` | 32-bit | Beacon Queue Descriptor Base | Physical 32-bit base address of Beacon TX ring |
| `REG_HQ_DESA` | `0x0310` | 32-bit | High Priority Queue Base | Physical 32-bit base address of High Priority TX ring |
| `REG_MGQ_DESA` | `0x0318` | 32-bit | Management Queue Base | Physical 32-bit base address of Management TX ring |
| `REG_VOQ_DESA` | `0x0320` | 32-bit | Voice (AC_VO) Queue Base | Physical 32-bit base address of Voice TX ring |
| `REG_VIQ_DESA` | `0x0328` | 32-bit | Video (AC_VI) Queue Base | Physical 32-bit base address of Video TX ring |
| `REG_BEQ_DESA` | `0x0330` | 32-bit | Best Effort (AC_BE) Base | Physical 32-bit base address of Best Effort TX ring |
| `REG_BKQ_DESA` | `0x0338` | 32-bit | Background (AC_BK) Base | Physical 32-bit base address of Background TX ring |
| `REG_RX_DESA` | `0x0340` | 32-bit | RX Ring Descriptor Base | Physical 32-bit base address of RX ring |
| `REG_TCR` | `0x0604` | 32-bit | Transmit Configuration Register | Controls early mode, aggregation limits, and TX DMA burst |
| `REG_RCR` | `0x0608` | 32-bit | Receive Configuration Register | Filter flags: Accept APM (unicast), AB (broadcast), AM (multicast), AAP (promisc) |
| `REG_CAMCMD` | `0x0670` | 32-bit | Security CAM Command Register | Bit 31: Write/Read; Bit 30: Reset all entries; Bits [5:0]: CAM index |
| `REG_CAMWRITE` | `0x0674` | 32-bit | Security CAM Write Data | Key material / MACID / security algorithm |
| `REG_SECCFG` | `0x0680` | 16-bit | Security Configuration Register | Bit 2: TX sec enable; Bit 3: RX sec enable |
| `RFPGA0_XA_LSSIPARAMETER`| `0x0840` | 32-bit | 3-Wire LSSI RF Interface | Bits [27:20]: RF Register offset; Bits [19:0]: 20-bit RF data |
| `REG_FW_START_ADDR` | `0x1000` | 4096-byte | MCU RAM Download Window | 4KB window mapped to MCU RAM based on `REG_MCUFWDL+2` |

---

### 3.2 Power-On Sequence State Machine (`pwrseq`)

The chip transitions through three hardware power domains:
1. **Card Disable (`CARDDIS`)**: Core voltage rails off, PCIe clock requests suspended, internal clocks gated, analog blocks isolated from digital logic.
2. **Card Emulation (`CARDEMU`)**: Clocks enabled, LDO regulators active, internal RAM accessible, MCU halted in download mode.
3. **Active (`ACT`)**: Full radio and MAC operational mode; TX/RX DMA enabled, RF PLL locked.

```
       +--------------------+
       |    Card Disable    |
       |     (CARDDIS)      |
       +--------------------+
             |        ^
   CARDDIS_TO_CARDEMU | CARDEMU_TO_CARDDIS
             v        |
       +--------------------+
       |   Card Emulation   |
       |     (CARDEMU)      |
       +--------------------+
             |        ^
   CARDEMU_TO_ACT     | ACT_TO_CARDEMU
             v        |
       +--------------------+
       |    Active (ACT)    |
       |   Normal Wi-Fi     |
       +--------------------+
```

#### Bring-Up Execution Steps (`_rtl8723be_init_mac`):
1. **Unlock Control Registers**: Write `0x00` to `REG_RSV_CTRL (0x001C)`.
2. **Disable Auto Power Down**: Read `REG_APS_FSMCO+1 (0x0005)`, clear Bit 7 (`& ~BIT(7)`), write back.
3. **Execute Enable Pwrseq Flow**: Execute `rtl8723B_card_enable_flow`:
   - `CARDDIS_TO_CARDEMU`: Clear suspend and power-down enable (`0x0005` bits 3,7 = 0); start PCIe DMA (`0x0301` = `0x00`).
   - `CARDEMU_TO_ACT`: Clear analog isolation (`0x0000` bit 5 = 0); disable software LPS (`0x0005` bits 2,3,4 = 0); poll power ready bit (`0x0006` bit 1 == 1, timeout 50ms).
4. **Multi-Function Configuration**: Set Bit 3 of `REG_MULTI_FUNC_CTRL (0x0020)`.
5. **Wake APS FSM**: Set Bit 4 of `REG_APS_FSMCO (0x0004)`.
6. **Command Enable**: Write `0xFF` to `REG_CR (0x0100)`; delay 2ms.
7. **Hardware Sequence Enable**: Write `0x7F` to `REG_HWSEQ_CTRL (0x0023)`; delay 2ms.
8. **Clock Stabilization**: If `REG_SYS_CFG+3` bit 0 set, write Bit 6 to `0x7C`. Set Bit 3 of `REG_SYS_CLKR (0x0008)`. Clear Bit 4 of `REG_GPIO_MUXCFG+1 (0x0041)`.
9. **Enable MAC and TRX**: Write 16-bit word `0x02FF` to `REG_CR (0x0100)`.
10. **Initialize LLT (Linked List Table)**: Call `_rtl8723be_llt_table_init(hw)` to structure chip internal buffer RAM.

---

### 3.3 Firmware Loading Protocol (`rtl8723befw.bin`)

#### Firmware File Structure
The binary file consists of a 32-byte header followed by 8051 code sections:

```c
struct rtlwifi_firmware_header {
    __le16 signature;       /* 0x5301 for RTL8723BE */
    u8     category;        /* 0x10 */
    u8     function;        /* 0x00 */
    __le16 version;         /* e.g., 15 or 36 */
    u8     subversion;      /* Subversion number */
    u8     rsvd1;
    u8     month;           /* Build timestamp month */
    u8     date;            /* Build timestamp date */
    u8     hour;            /* Build timestamp hour */
    u8     minute;          /* Build timestamp minute */
    __le16 ramcodesize;     /* Size of microcode following header */
    __le16 rsvd2;
    __le32 svnindex;        /* SVN repository revision */
    __le32 rsvd3;
    __le32 rsvd4;
    __le32 rsvd5;
} __attribute__((packed)); /* Total 32 bytes */
```

#### Download Algorithm:
1. **Validate Header**: Check `(le16_to_cpu(header->signature) & 0xFFF0) == 0x5300`. Skip 32 bytes to obtain microcode payload.
2. **Reset Existing MCU State**: If `REG_MCUFWDL (0x0080) & BIT(7)` is set, perform `rtl8723be_firmware_selfreset`:
   - Clear and set Bit 0 of `REG_RSV_CTRL+1 (0x001D)` with 50µs delay.
   - Clear and set Bit 2 of `REG_SYS_FUNC_EN+1 (0x0003)`.
   - Write `0x00` to `REG_MCUFWDL (0x0080)`.
3. **Enter Download Mode**:
   - Write Bit 2 (`0x04`) to `REG_SYS_FUNC_EN+1 (0x0003)` (enable MCU clock).
   - Write Bit 0 (`0x01`) to `REG_MCUFWDL (0x0080)` (enable FW download).
   - Clear Bit 3 (`& 0xF7`) of `REG_MCUFWDL+2 (0x0082)` (reset page index).
4. **Pad Microcode Buffer**: Pad trailing bytes with zeroes so total size is a multiple of 4 (`rtl_fill_dummy`).
5. **Page-by-Page RAM Download**:
   - Page size is **4096 bytes** (`0x1000`). Maximum page count is **8**.
   - For each page `page = 0 .. (num_pages - 1)`:
     - Set page number in bits [2:0] of `REG_MCUFWDL+2 (0x0082)`: `(read8(0x0082) & 0xF8) | (page & 0x07)`.
     - Write chunk bytes directly to MMIO address `0x1000 + offset` (1-byte or 4-byte writes).
6. **Exit Download Mode**:
   - Clear Bit 0 of `REG_MCUFWDL (0x0080)`.
   - Write `0x00` to `REG_MCUFWDL+1 (0x0081)`.
7. **Checksum & Readiness Handshake (`rtl8723_fw_free_to_go`)**:
   - Poll `REG_MCUFWDL (0x0080)` until Bit 2 (`FWDL_CHKSUM_RPT`) is 1 (up to 6000 cycles with 5µs delay). Fail if timeout.
   - Set Bit 1 (`MCUFWDL_RDY`) and clear Bit 6 (`WINTINI_RDY`) in `REG_MCUFWDL (0x0080)`.
   - Trigger MCU reset via `rtl8723be_firmware_selfreset`.
   - Poll `REG_MCUFWDL (0x0080)` until Bit 6 (`WINTINI_RDY`) becomes 1 (up to 6000 cycles with 5ms delay).
   - Once Bit 6 is set, the MCU 8051 firmware is active and initialized.

---

### 3.4 eFuse Architecture & Calibration Map

The RTL8723BE contains an on-chip one-time programmable (OTP) eFuse memory with 256 physical bytes (`EFUSE_REAL_CONTENT_LEN`), expandable logically into a 512-byte EEPROM shadow map (`HWSET_MAX_SIZE`).

#### Autoload vs Manual Readout
At power-up, the internal hardware autoload controller reads physical eFuse and mirrors key fields to MAC registers. Bit 5 of `REG_9346CR (0x000A)` indicates if autoload completed successfully.

If manual eFuse readout is performed:
1. Write `0x69` to `REG_EFUSE_ACCESS (0x00CF)`.
2. Write low byte of address to `REG_EFUSE_CTRL+1 (0x0031)`.
3. Write high bits [9:8] of address to `REG_EFUSE_CTRL+2 (0x0032)` while preserving bits [7:2].
4. Write `0x72` to `REG_EFUSE_CTRL+3 (0x0033)` to trigger read.
5. Poll until Bit 7 of `REG_EFUSE_CTRL+3` is 1 (up to 100 tries).
6. Read data byte from `REG_EFUSE_CTRL (0x0030)`.
7. Write `0x00` to `REG_EFUSE_ACCESS (0x00CF)` upon completion.

#### eFuse Logical Layout (512-Byte Map)

| Offset Range | Size (Bytes) | Field Name | Description | Default Fallback |
|---|---|---|---|---|
| `0x0010 - 0x0015` | 6 | `EEPROM_TX_PWR_INX` | CCK base TX power level for 6 channel groups | `0x2D` (45 dec) |
| `0x0016 - 0x001A` | 5 | `EEPROM_TXPOWERHT40_1S` | HT40 1S base TX power level for 5 channel groups | `0x2D` |
| `0x001B` | 1 | `EEPROM_TXPOWERHT20DIFF` | TX power difference for HT20 vs HT40 / OFDM | `0x02` / `0x04` |
| `0x00B8` | 1 | `EEPROM_CHANNELPLAN` | Regulatory domain / channel plan | `0x00` (World 13) |
| `0x00B9` | 1 | `EEPROM_XTAL_8723BE` | Crystal oscillator capacitive load trim | `0x20` (if 0xFF) |
| `0x00BA` | 1 | `EEPROM_THERMAL_METER` | Factory thermal sensor reference calibration | `0x1A` |
| `0x00BB` | 1 | `EEPROM_IQK_LCK` | IQK / LCK factory calibration flag | `0x00` |
| `0x00C1` | 1 | `EEPROM_RF_BOARD_OPTION` | Board configuration / Regulatory option (bits 0..2) | `0x00` |
| `0x00C3` | 1 | `EEPROM_RF_BT_SETTING` | Bluetooth & Antenna option (bit 0: ant_num, bit 6: path) | `0x01` |
| `0x00C4` | 1 | `EEPROM_VERSION` | EEPROM map version number | `0x01` |
| `0x00C5` | 1 | `EEPROM_CUSTOMER_ID` | Customer OEM identifier | `0x00` |
| `0x00D0 - 0x00D5` | 6 | `EEPROM_MAC_ADDR` | Factory Wi-Fi physical MAC address | Random / fallback |
| `0x00D6 - 0x00D7` | 2 | `EEPROM_VID` | PCI Vendor ID mirror (`0x10EC`) | `0x10EC` |
| `0x00D8 - 0x00D9` | 2 | `EEPROM_DID` | PCI Device ID mirror (`0xB723`) | `0xB723` |
| `0x00DA - 0x00DB` | 2 | `EEPROM_SVID` | PCI Subsystem Vendor ID mirror (`0x103C`) | `0x103C` |
| `0x00DC - 0x00DD` | 2 | `EEPROM_SMID` | PCI Subsystem Device ID mirror (`0x804C`) | `0x804C` |

---

### 3.5 TX / RX DMA Engine & Descriptors

The DMA engine utilizes circular descriptor rings allocated in coherent, cache-consistent, physically contiguous host memory aligned to 256-byte boundaries (`0x100`).

#### 3.5.1 TX Queue Mapping & Registers

| Queue Name | Queue ID | Index in `tx_ring[]` | Start Address Register | Doorbell Bit in `0x0300` | Default Count |
|---|---|---|---|---|---|
| Background (`AC_BK`) | `0` | `BK_QUEUE` | `REG_BKQ_DESA (0x0338)` | `BIT(0)` | 128 |
| Best Effort (`AC_BE`) | `1` | `BE_QUEUE` | `REG_BEQ_DESA (0x0330)` | `BIT(1)` | 256 |
| Video (`AC_VI`) | `2` | `VI_QUEUE` | `REG_VIQ_DESA (0x0328)` | `BIT(2)` | 128 |
| Voice (`AC_VO`) | `3` | `VO_QUEUE` | `REG_VOQ_DESA (0x0320)` | `BIT(3)` | 128 |
| Beacon | `4` | `BEACON_QUEUE` | `REG_BCNQ_DESA (0x0308)` | `BIT(4)` | 2 |
| Management | `6` | `MGNT_QUEUE` | `REG_MGQ_DESA (0x0318)` | `BIT(6)` | 128 |
| High Priority | `7` | `HIGH_QUEUE` | `REG_HQ_DESA (0x0310)` | `BIT(7)` | 128 |
| RX Ring | N/A | `RX_MPDU_QUEUE` | `REG_RX_DESA (0x0340)` | Auto-fetching | 256 / 512 |

#### 3.5.2 40-Byte TX Buffer Descriptor Layout (`struct tx_desc_8723be`)

| Dword | Bits | Field Name | Description |
|---|---|---|---|
| **0** | [15:0] | `pktsize` | Total packet size in bytes (payload + MAC header) |
| | [23:16] | `offset` | Header offset (normally 40 = `sizeof(struct tx_desc_8723be)`) |
| | [24] | `bmc` | Broadcast or Multicast packet flag |
| | [25] | `htc` | HTC field present in MAC header |
| | [26] | `lastseg` | Last segment of frame (set to 1 for unfragmented) |
| | [27] | `firstseg` | First segment of frame (set to 1 for unfragmented) |
| | [28] | `linip` | IP header checksum calculation |
| | [31] | `own` | **Ownership bit**: 1 = DMA engine owns; 0 = Host CPU owns |
| **1** | [6:0] | `macid` | Station MACID (0..127) |
| | [12:8] | `queuesel` | Queue select: BE=0, BK=2, VI=5, VO=7, BCN=16, HIGH=17, MGNT=18 |
| | [20:16] | `rateid` | Rate table index |
| | [23:22] | `sectype` | Security type: 0=None, 1=WEP40, 2=TKIP, 3=AES-CCMP |
| | [31:24] | `pktoffset` | Packet offset / Early mode spacing |
| **2** | [12] | `agg_en` | A-MPDU aggregation enable |
| | [22:20] | `ampdudensity` | Minimum MPDU start spacing density |
| **3** | [27:16] | `seq` | IEEE 802.11 sequence number |
| | [31] | `hwseq_en` | Hardware sequence number generation enable (1=HW, 0=Driver) |
| **4** | [4:0] | `rtsrate` | RTS transmission rate |
| | [11] | `cts2self` | CTS-to-self protection enable |
| | [12] | `rts_en` | RTS/CTS protection enable |
| **5** | [5:0] | `txrate` | Transmit rate index (CCK 0..3, OFDM 4..11, MCS0..7 = 12..19) |
| | [6] | `shortgi` | Short Guard Interval (400ns) enable for 802.11n |
| | [12:8] | `txrate_fb_lmt` | Rate fallback limit retry count |
| **7** | [15:0] | `txbuffersize` | Size of DMA buffer pointed to by `txbuffaddr` |
| **8** | [31:0] | `txbuffaddr` | Low 32 bits of physical host memory buffer address |
| **9** | [31:0] | `txbufferaddr64`| High 32 bits of physical host memory buffer address |
| **10**| [31:0] | `nextdescaddress` | Low 32 bits of next TX descriptor physical address (chaining) |
| **11**| [31:0] | `nextdescaddress64` | High 32 bits of next TX descriptor physical address |

#### 3.5.3 32-Byte RX Buffer Descriptor Layout (`struct rx_desc_8723be`)

| Dword | Bits | Field Name | Description |
|---|---|---|---|
| **0** | [13:0] | `length` | Total received byte length (includes 4-byte 802.11 FCS CRC) |
| | [14] | `crc32` | CRC32 / FCS error flag (1 = CRC failed, 0 = OK) |
| | [15] | `icverror` | ICV / MIC error flag (1 = decryption failed) |
| | [19:16] | `drv_infosize` | Length of driver info / PHY status header in units of 8 bytes |
| | [22:20] | `security` | Decryption status: 0=None, 1=WEP40, 2=TKIP, 4=AES-CCMP |
| | [25:24] | `shift` | Buffer byte shift (0..2 bytes) to align payload IP header |
| | [26] | `phystatus` | PHY status report attached at start of packet payload |
| | [27] | `swdec` | Frame was decrypted by software |
| | [30] | `eor` | **End of Ring flag**: Set to 1 on last descriptor of RX ring |
| | [31] | `own` | **Ownership bit**: 1 = DMA engine owns; 0 = Packet ready for CPU |
| **1** | [6:0] | `macid` | Source MACID |
| | [15] | `paggr` | Frame is an A-MPDU subframe |
| | [30:29] | `type` | 802.11 frame type: 00=Management, 01=Control, 10=Data |
| | [31] | `mc` | Multicast destination address |
| **2** | [11:0] | `seq` | 802.11 sequence number |
| | [15:12] | `frag` | 802.11 fragment number |
| | [28] | `rpt_sel` | **Report Select**: 0 = Normal RX packet, 1 = C2H event report |
| **3** | [5:0] | `rxmcs` | Received MCS index or legacy rate |
| | [6] | `rxht` | 1 = 802.11n HT packet, 0 = Legacy 802.11b/g packet |
| | [9] | `bandwidth` | Channel bandwidth: 0 = 20 MHz, 1 = 40 MHz |
| **5** | [31:0] | `tsfl` | MAC Time Sync Function (TSF) timestamp lower 32 bits |
| **6** | [31:0] | `bufferaddress` | Low 32 bits of physical host memory buffer address |
| **7** | [31:0] | `bufferaddress64` | High 32 bits of physical host memory buffer address |

---

### 3.6 Baseband & Radio Frequency Subsystem

#### 3.6.1 Table Initializations
The PHY layer is brought up by sequential loading of 5 static register tables:
1. `RTL8723BEMAC_1T_ARRAY`: 103 register pairs programming MAC clock domains, FIFO boundaries, and timing registers.
2. `RTL8723BEPHY_REG_1TARRAY`: 193 register pairs programming Baseband DSP filters, ADC/DAC sample rates, OFDM equalizer coefficients, and CCK Barker correlators.
3. `RTL8723BEAGCTAB_1TARRAY`: 131 register pairs configuring the Automatic Gain Control (AGC) step curves and LNA switching points.
4. `RTL8723BE_RADIOA_1TARRAY`: 136 register pairs written via 3-wire LSSI to Radio Path A synthesizer registers.
5. `RTL8723BEPHY_REG_ARRAY_PG`: 18 register pairs programming power-by-rate base values.

#### 3.6.2 3-Wire LSSI Serial Interface
Because RF registers are accessed through a serial bus mediated by the Baseband processor, reading and writing RF registers is performed via Baseband MMIO register `0x0840` (`RFPGA0_XA_LSSIPARAMETER`):

```c
void rtl8723_phy_rf_serial_write(struct ieee80211_hw *hw, enum radio_path rfpath,
                                 u32 offset, u32 data)
{
    u32 data_and_addr;
    offset &= 0xFF;
    /* Format: [27:20] = offset (8 bits), [19:0] = data (20 bits) */
    data_and_addr = ((offset << 20) | (data & 0x000FFFFF)) & 0x0FFFFFFF;
    rtl_set_bbreg(hw, 0x0840, 0xFFFFFFFF, data_and_addr);
}
```

#### 3.6.3 2.4 GHz Channel Tuning (Channels 1–13)
Channel tuning is executed via RF register `RF_CHNLBW` (`0x18`):

```c
void rtl8723be_set_channel(struct ieee80211_hw *hw, u8 channel, u8 bandwidth)
{
    u32 val;
    /* Read current RF 0x18 */
    val = rtl8723_phy_rf_serial_read(hw, RF90_PATH_A, 0x18);
    val &= 0xFFF00000; /* Preserve high bits */
    val |= (channel & 0x3FF); /* Bits [9:0] = Channel number (1..13) */

    if (bandwidth == HT_CHANNEL_WIDTH_20) {
        val |= (1 << 10) | (1 << 11); /* 20 MHz bandwidth mode */
    } else {
        val |= (1 << 10);              /* 40 MHz bandwidth mode */
    }

    rtl8723_phy_rf_serial_write(hw, RF90_PATH_A, 0x18, val);
    udelay(10000); /* 10ms PLL settling time */

    /* Update Baseband TX power per channel */
    rtl8723be_phy_set_txpower_level(hw, channel);
}
```

#### 3.6.4 Single / Dual Antenna Switching (`0x92C`)
In laptops equipped with a single Wi-Fi antenna wire, the antenna may be connected to either the Main or Aux connector on the M.2/mini-PCIe card. Baseband register `0x92C` switches the internal RF switch matrix:
- **Main Antenna (Left)**: Write `0x00000001` to BB register `0x092C`.
- **Aux Antenna (Right)**: Write `0x00000002` to BB register `0x092C`.

If the wrong antenna is selected, received signal strength drops by 30–40 dB, rendering access points undetectable. A complete driver must allow toggling this register or probing both paths during scan.

---

## 4. Edge Cases Observed & Documented

| # | Feature | Input / Condition | Observed Behavior |
|---|---------|-------------------|-------------------|
| 1 | eFuse Autoload | Corrupted or unwritten eFuse (bit 5 of `0x000A` is 0) | Chip fails autoload; driver must fall back to conservative defaults: Crystal cap = `0x20`, Thermal meter = `0x1A`, Power level = `0x2D` (`45`). Driver must not crash or leave fields zeroed. |
| 2 | Firmware Download | MCU busy or previously loaded firmware running (`REG_MCUFWDL & BIT(7)`) | Firmware download fails if not reset. Driver must perform `rtl8723be_firmware_selfreset` (resetting `REG_RSV_CTRL+1` bit 0 and `REG_SYS_FUNC_EN+1` bit 2) before initiating download. |
| 3 | Firmware Checksum | Firmware binary corrupted or truncated | Polling `REG_MCUFWDL` for `FWDL_CHKSUM_RPT` (bit 2) times out after 6000 cycles (~30ms) and aborts hardware initialization with `-EIO`. |
| 4 | TX Descriptor Ring | Transmit queue ring full (`own` bit remains 1 on next slot) | Host driver must stop transmit queue (`netif_stop_queue` / pause flow) until interrupt indicates descriptor completion (`IMR_*DOK`). Overwriting descriptor causes DMA memory corruption. |
| 5 | RX Descriptor Ring | RX ring exhausted under heavy load (`IMR_RDU` interrupt) | Hardware drops incoming packets on wire and asserts `IMR_RDU` (Rx Descriptor Unavailable). Driver must refill ring descriptors and clear RDU bit in `REG_HISR`. |
| 6 | RX Packet Alignment | Variable length driver info header (`drv_infosize`) and shift padding | 802.11 header offset varies based on `(drv_infosize * 8) + shift`. Failing to apply both offsets causes parsing failure of frame control, MAC addresses, and payload. |
| 7 | Antenna Connector Mismatch | Single antenna wire connected to Aux port while driver defaults to Main | Device exhibits -85 to -95 dBm RSSI or completely fails to discover SSIDs. Flipping `0x092C` to `0x2` immediately restores nominal signal (-45 to -60 dBm). |
| 8 | Channel Selection | Out of range channel (e.g. Channel 14 in non-Japan domains) | RF synthesizer PLL may fail to lock; driver must clamp channels strictly to 1–13 according to `rtlefuse->channel_plan`. |
| 9 | High-Speed Interrupts | Link power state transition asserts `HSISR` | If `REG_HSISR` (0x005C) is not acknowledged when `IMR_HSISR_IND_ON_INT` (bit 15) fires in `REG_HISR`, host receives continuous level-triggered interrupt loop. |
| 10 | End-of-Ring (`EOR`) | Last descriptor in RX ring omitted `EOR` flag | DMA engine continues past ring buffer boundary, corrupting adjacent host kernel memory. Bit 30 must always be set on descriptor index `RX_COUNT - 1`. |

---

## 5. macOS Driver Porting Blueprint & Architectural Mapping

For macOS 26.6.2 (`25G83`), the standard IOKit network driver architecture is an `IOEthernetController` subclass attaching to `IOPCIDevice`.

```
+-------------------------------------------------------------------+
| macOS User Space (CLI Utility / HeliPort / NetworkExtension)      |
+-------------------------------------------------------------------+
                                  ^ (IOUserClient / BSD socket)
                                  v
+-------------------------------------------------------------------+
| RTL8723BE macOS Kernel Driver (IOEthernetController)              |
+-------------------------------------------------------------------+
| 802.11 Management Engine:                                         |
|   - Active/Passive Scan State Machine (Channels 1-13)             |
|   - Authentication & Association 802.11 Frame Generators          |
|   - Software / Hardware WPA2-PSK 4-Way Handshake & CCMP Crypto    |
|   - Ethernet II <-> 802.11 Frame Encapsulation/Decapsulation      |
+-------------------------------------------------------------------+
| RTL8723BE Hardware Engine:                                        |
|   - PCIe MMIO Accessors (IOMemoryMap on BAR2)                     |
|   - Interrupt Service Routine (IOFilterInterruptEventSource / MSI)|
|   - Power Sequence State Machine (`pwrseq`)                       |
|   - Embedded Firmware Loader (`rtl8723befw.bin`)                  |
|   - eFuse Parser & Factory Calibration Extractor                  |
|   - TX DMA Ring Manager (BK, BE, VI, VO, MGNT, HIGH rings)        |
|   - RX DMA Ring Manager (256 descriptors, EOR wrapping)           |
|   - MAC/BB/RF Initializer (Tables, AGC, 3-wire LSSI channel tune) |
|   - Antenna Path Control (Main / Aux toggle at 0x092C)            |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
| Realtek RTL8723BE PCIe Hardware (10EC:B723)                       |
+-------------------------------------------------------------------+
```

### Direct IOKit Mapping:
- **PCI Attachment**: Inherit from `IOEthernetController`, match `pci10ec,b723`.
- **MMIO Access**: Call `provider->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2)`.
- **Interrupts**: Register `IOFilterInterruptEventSource` with `provider->getWorkLoop()`. In filter routine, read and clear `REG_HISR` (`0x00B4`), then signal workloop thread.
- **DMA Buffers**: Allocate via `IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0x000000FFFFFFFFULL)`. Ensure physical address is aligned to 256 bytes (`0x100`).
- **Firmware Loading**: Embed `rtl8723befw.bin` directly as static `const unsigned char rtl8723befw_bin[]` within the kext binary to eliminate file-system dependencies during early boot.
- **Network Interface**: Register `IONetworkInterface` created by `attachInterface(&netif)`. Packets received from RX DMA are converted to `mbuf_t` via `allocatePacket()` and passed upstream via `netif->inputPacket(m)`.
