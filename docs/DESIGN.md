# Engineering Design Document: Realtek RTL8723BE macOS Wireless LAN Driver

**Document Identifier**: EDD-RTL8723BE-DARWIN-01  
**Revision**: 1.0.0 (Production Engineering Specification)  
**Status**: APPROVED / IMPLEMENTATION BASELINE  
**Author**: Worker M1 (Engineering Architecture & Driver Design Team)  
**Date**: September 26, 2026  
**Target Hardware**: Realtek RTL8723BE PCIe 802.11b/g/n Wireless LAN Adapter  
- Device PCI ID: `0x10EC:0xB723` (`pci10ec,b723`)  
- Subsystem PCI ID: `0x103C:0x804C` (HP Inc.)  
- Physical Bus Location: PCI `2:0:0`, ACPI Path: `IOACPIPlane:/_SB/PCI0@0/RP06@1c0005/PXSX@0`  
- Memory Resources: BAR0 I/O Port (256B), BAR2 64-bit MMIO (16KB at `0xf1100000`)  
- Interrupt Resources: MSI Vector 8 (`IOPCIMessagedInterruptController`), IO-APIC Pin 17  
**Target Operating System**: macOS 26.6.2 (Darwin Kernel Version 25.6.0, Build `25G83`, `x86_64`)  
**Target Bootloader**: OpenCore 1.0+ on `/Volumes/HTOSH/EFI/OC`  

---

## Table of Contents
1. [Executive Summary & Problem Formulation](#1-executive-summary--problem-formulation)
2. [Architecture Selection & Comparative Evaluation](#2-architecture-selection--comparative-evaluation)
   - 2.1 Technical Evaluation of Driver Paradigms
   - 2.2 Why IO80211Family & DriverKit Fail on Modern macOS
   - 2.3 The IOEthernetController + UserClient Architectural Pattern
   - 2.4 macOS 26.6.2 Kernel Programming Interface (KPI) Mappings & Toolchain
3. [RTL8723BE Hardware Architecture & Registers](#3-rtl8723be-hardware-architecture--registers)
   - 3.1 Device Identification, Bus Topology & Memory Resources
   - 3.2 MMIO Register Map (BAR2 16KB Window)
   - 3.3 Core Register Definitions & Bitmask Specifications
   - 3.4 Hardware Power-On State Machine (`pwrseq`)
   - 3.5 Linked List Table (LLT) Internal Buffer Configuration
4. [8051 MCU Firmware Handshake Protocol](#4-8051-mcu-firmware-handshake-protocol)
   - 4.1 On-Chip Microcontroller Architecture & Role
   - 4.2 Firmware Binary Format & Header Specification
   - 4.3 MCU Reset & Download Mode Initiation
   - 4.4 4KB Page Streaming Engine into MMIO 0x1000..0x1FFF
   - 4.5 Checksum Handshake, Self-Reset & WINTINI_RDY Verification
5. [eFuse & Factory Calibration Mapping](#5-efuse--factory-calibration-mapping)
   - 5.1 Physical OTP Array vs. Logical EEPROM Shadow Map
   - 5.2 Autoload Detection & Manual Indirect Access Protocol (`REG_EFUSE_CTRL`)
   - 5.3 Physical PG Packet Decoding Algorithm
   - 5.4 Logical Calibration Map Layout & Default Fallbacks
6. [TX/RX DMA Descriptor Rings & Memory Architecture](#6-txrx-dma-descriptor-rings--memory-architecture)
   - 6.1 DMA Ring Architecture & 256-Byte Boundary Alignment
   - 6.2 Hardware Priority Queues & Base Address Registers
   - 6.3 40-Byte Transmit Descriptor Layout (`struct tx_desc_8723be`)
   - 6.4 32-Byte Receive Descriptor Layout (`struct rx_desc_8723be`)
   - 6.5 Ownership Bit Arbitration, Ring Wrapping (EOR) & Doorbell Mechanics
   - 6.6 Ring Starvation, Buffer Replenishment & Interrupt Mitigation
7. [Baseband, RF Tuning & Antenna Diversity](#7-baseband-rf-tuning--antenna-diversity)
   - 7.1 Bulk Register Table Initialization Sequences
   - 7.2 3-Wire LSSI Serial RF Access Protocol (`0x0840`)
   - 7.3 2.4 GHz Frequency Synthesizer & Channel Tuning (Channels 1–13)
   - 7.4 RF Front-End Antenna Diversity & HP Single-Antenna Switching (`0x092C`)
8. [802.11 Protocol & Data Path Engine](#8-80211-protocol--data-path-engine)
   - 8.1 Active / Passive Channel Scanning Engine & Beacon/Probe Cache
   - 8.2 Authentication & Association State Machine
   - 8.3 WPA2-PSK 4-Way Handshake Engine (EAPOL-Key Messages 1–4)
   - 8.4 Key Derivation Functions: PBKDF2-HMAC-SHA1 & PRF-512
   - 8.5 CCMP (AES-128 CCM) Encryption / Decryption & Replay Defense
   - 8.6 Ethernet II <-> RFC 1042 LLC/SNAP 802.11 QoS Data Frame Translation
9. [User-Space Control Plane & Safe Staging Plan](#9-user-space-control-plane--safe-staging-plan)
   - 9.1 `IOUserClient` IPC Interface Specification (`RTL8723BEUserClient`)
   - 9.2 Command-Line Interface (`rtl8723be_cli`) Architecture
   - 9.3 Diagnostic Verification & Symbol Resolution Pipeline
   - 9.4 OpenCore EFI Safe Staging & Host Bring-Up Protocol
10. [Traceability & Verification Matrix](#10-traceability--verification-matrix)

---

## 1. Executive Summary & Problem Formulation

### 1.1 The Technical Challenge
The Realtek RTL8723BE is a highly integrated single-chip PCI Express 802.11b/g/n (1T1R, 2.4 GHz) Wireless LAN and Bluetooth combo controller. In the host machine—a MacBookPro14,1 Hackintosh platform based on an HP laptop motherboard—the Wi-Fi controller is attached to PCI Express Root Port 6 (`_SB/PCI0@0/RP06@1c0005/PXSX@0` at PCI address `2:0:0`), with vendor ID `0x10EC`, device ID `0xB723`, subsystem vendor ID `0x103C`, and subsystem ID `0x804C`.

Historically, running non-Apple Wi-Fi chipsets on macOS has presented extreme challenges. Apple's native Wi-Fi architecture (`IO80211Family.kext`, `airportd`, `CoreWLAN.framework`) is strictly proprietary, undocumented, and designed solely around Apple-branded Broadcom and Qualcomm Atheros chipsets. In macOS 14.0 (Sonoma), 15.0 (Sequoia), and macOS 26.6.2 (Darwin 25.6.0, `25G83`), Apple executed a complete architectural overhaul of its wireless networking stack:
1. Legacy PCI Wi-Fi drivers (`AirPortBrcm4360`, `AirPortAtheros40`) were excised from `BootKernelExtensions.kc`.
2. The user-space daemon `airportd` introduced strict code-signing and entitlement validation, rejecting third-party drivers attempting to implement `IO80211APIUserClient`.
3. macOS transitioned its internal networking data plane to `IOSkywalkFamily` and user-space DriverKit extensions (`com.apple.DriverKit-AppleBCMWLAN.dext`).

Consequently, community attempts to inject legacy `IO80211Family` shims (such as `AirportItlwm`) require catastrophic system modifications: forcing SIP/AMFI off, blocking `IOSkywalkFamily`, injecting obsolete macOS Ventura frameworks, and running OpenCore Legacy Patcher (OCLP) root patches. For Realtek PCIe hardware, **no native macOS driver has ever existed**; users were universally advised to replace the internal M.2/PCIe card or purchase an external USB dongle.

### 1.2 Engineering Mission & Scope
This project designs and implements a complete, native, production-grade driver solution for the Realtek RTL8723BE on macOS 26.6.2. 

The driver:
1. Subclasses `IOEthernetController` (from `com.apple.iokit.IONetworkingFamily` 3.4), presenting an authentic IEEE 802.3 Ethernet interface to the XNU BSD network stack.
2. Directly commands the RTL8723BE hardware via `com.apple.iokit.IOPCIFamily` (2.9), mapping the 16KB BAR2 64-bit MMIO aperture (`0xf1100000`) and handling MSI interrupts.
3. Incorporates a full in-kernel 802.11 management engine: 2.4 GHz active/passive scanner (channels 1–13), Open System Authentication and Association state machine, software/hardware WPA2-PSK 4-way handshake engine, AES-128 CCMP cipher engine, and RFC 1042 LLC/SNAP frame encapsulator.
4. Exposes an `IOUserClient` control interface enabling a companion command-line utility (`rtl8723be_cli`) and community frontends (e.g., `HeliPort`) to control scanning, network selection, and antenna switching.
5. Implements antenna diversity control to resolve the single-antenna signal drop specific to HP laptops (`103c:804c`).
6. Provides an end-to-end user-space hardware mock emulation test suite verifying 100% of driver state-machine paths before staging into OpenCore EFI.

---

## 2. Architecture Selection & Comparative Evaluation

### 2.1 Technical Evaluation of Driver Paradigms

Three distinct architectural patterns were evaluated for bringing up the RTL8723BE on macOS 26.6.2:

```
Pattern A: IOEthernetController + IOUserClient (CHOSEN)
+---------------------------------------------------------------------------------+
| macOS BSD TCP/IP Stack (en1) <-> IOEthernetController (RTL8723BE.kext)           |
|                                         ^                                       |
|                                         | IOConnectCallMethod (IPC)             |
|                                         v                                       |
| User-Space CLI / HeliPort      <-> RTL8723BEUserClient                          |
+---------------------------------------------------------------------------------+

Pattern B: IO80211Family Shim (AirportItlwm Style) [REJECTED]
+---------------------------------------------------------------------------------+
| macOS airportd / CoreWLAN    <-> IO80211Family.kext (Private Apple Vtables)    |
|                                         |                                       |
|                                         v (Fails entitlement check on macOS 26) |
|                              AirPort_RTL8723BE.kext (Kernel Panics / Broken)    |
+---------------------------------------------------------------------------------+

Pattern C: DriverKit Dext (.dext) [REJECTED]
+---------------------------------------------------------------------------------+
| User-Space dext process      <-> PCIDriverKit (Requires restricted entitlements)|
|                                         |                                       |
|                                         v (Cannot be injected from OpenCore EFI)|
|                              NetworkingDriverKit (No public 802.11 API)        |
+---------------------------------------------------------------------------------+
```

### 2.2 Deep Comparative Evaluation Matrix

| Technical Criterion | Pattern A: `IOEthernetController` + Daemon | Pattern B: `IO80211Family` Shim | Pattern C: `DriverKit` Extension (`.dext`) |
| :--- | :--- | :--- | :--- |
| **Kernel Subclass** | `IOEthernetController` (`IONetworkingFamily`) | `IO80211Controller` (`IO80211Family`) | `IOUserNetworkEthernet` (`NetworkingDriverKit`) |
| **KPI Availability** | **Public, frozen, stable** Apple KPIs | **Private, undocumented, highly unstable** | Public DriverKit C++ APIs |
| **macOS 26.6.2 Compatibility** | **100% Native Out-of-the-Box** | **Broken** (Excised from kernel cache) | Restricted (Requires Apple signing entitlements) |
| **System Integrity (SIP/AMFI)** | Works with **Full SIP & AMFI enabled** | Requires `csr-active-config=0x7F`, AMFI disabled | Requires AMFI developer mode / SIP disabled |
| **OpenCore EFI Staging** | **Drop-in kext** injection via `config.plist` | Requires framework overrides & OCLP patches | **Impossible** (dexts cannot be injected from EFI) |
| **OS Point-Update Resilience** | **Immune** to macOS minor/major OS updates | Breaks on nearly every minor dot-release | Immune to kernel changes, but app-bound |
| **Wi-Fi Protocol Management** | Managed in-driver (Scan, Auth, Assoc, WPA2) | Delegated to `airportd` / `CoreWLAN` | Managed in user-space dext process |
| **Hardware MMIO & DMA Latency** | **Direct zero-copy** kernel physical mapping | Direct kernel physical mapping | Mediated through Mach IPC and memory descriptors |
| **Failure Mode** | Network link drops; host kernel remains stable | **Host kernel panics** on sleep/wake or vtable shift | Process crashes and restarts via `sysextd` |
| **Verdict** | **SELECTED ARCHITECTURE** | **REJECTED (Fundamentally Unviable)** | **REJECTED (Cannot Inject via EFI)** |

### 2.3 Why IO80211Family and DriverKit are Inviable on macOS 26.6.2

#### The Inviability of `IO80211Family`
In macOS 14+ through macOS 26.6.2, Apple replaced the legacy `IO80211Controller` interfaces with internal `IOSkywalkFamily` channels. `IO80211Family.kext` no longer exists in `/System/Library/Extensions` as a standalone loadable personality for third-party hardware. Furthermore, Apple introduced hardened entitlement verification: when an application or daemon connects to `IO80211APIUserClient`, `airportd` validates whether the client holds private Apple entitlements (`com.apple.private.apple80211.internal`). Third-party kexts cannot satisfy this requirement. Attempting to bypass this by injecting legacy macOS 13 Ventura `IO80211Family` kexts induces binary symbol clashes, kernel memory corruption on sleep/wake, and fatal kernel traps.

#### The Inviability of DriverKit
DriverKit (`.dext`) is designed for user-space system extensions bundled inside signed macOS application bundles installed into `/Applications`. However:
1. `PCIDriverKit` requires the restricted entitlement `com.apple.developer.driverkit.transport.pci`, which Apple only grants to registered hardware vendors.
2. `NetworkingDriverKit` only exposes `IOUserNetworkEthernet`. Apple explicitly keeps wireless networking (`IOUserNetworkWLAN`) private and internal to `IOSkywalkFamily`.
3. OpenCore EFI **cannot inject DriverKit extensions**. OpenCore operates at UEFI boot time before the Darwin kernel launches; DriverKit extensions are spawned late in user space by `sysextd` after `/System` and `/Data` volumes mount. Thus, a bootable, self-contained driver in OpenCore EFI requires a Kernel Extension (kext).

### 2.4 macOS 26.6.2 Kernel Programming Interface (KPI) Mappings & Toolchain

The RTL8723BE driver links exclusively against the stable, officially exported Apple Kernel Programming Interfaces present in Darwin 25.6.0 (`xnu-12377.161.14~5`):

```
                   +--------------------------------------------+
                   |             RTL8723BE.kext                 |
                   +--------------------------------------------+
                                   |            |
         +-------------------------+            +--------------------------+
         v                                                                 v
+-----------------------------------+             +----------------------------------+
|   com.apple.iokit.IOPCIFamily     |             | com.apple.iokit.IONetworkingFamily|
|   Version: 2.9 (Loaded in kc)     |             | Version: 3.4 (Loaded in kc)      |
+-----------------------------------+             +----------------------------------+
| - IOPCIDevice                     |             | - IOEthernetController           |
| - configRead16 / configWrite16    |             | - IOEthernetInterface            |
| - mapDeviceMemoryWithRegister     |             | - IONetworkMedium                |
| - setBusMasterEnable              |             | - IOMbufMemoryCursor             |
| - registerInterrupt               |             | - allocatePacket / freePacket    |
+-----------------------------------+             +----------------------------------+
         |                                                                 |
         +-------------------------+            +--------------------------+
                                   v            v
                   +--------------------------------------------+
                   |             System.kext PlugIns            |
                   +--------------------------------------------+
                   | - com.apple.kpi.bsd (25.6.0): mbuf, lock   |
                   | - com.apple.kpi.iokit (25.6.0): IOWorkLoop |
                   | - com.apple.kpi.libkern (25.6.0): OSObject |
                   | - com.apple.kpi.mach (25.6.0): clock, time |
                   +--------------------------------------------+
```

#### Build Toolchain Specification
- **Compiler**: Apple Clang version 21.0.0 (Apple LLVM 21.0.0, Xcode 26.5).
- **Target Architecture**: `x86_64-apple-macos14.0` (Binary compatible across Darwin 23.x, 24.x, and 25.x/26.x).
- **Compilation Flags**:
  ```sh
  -mkernel -fapple-kext -fno-rtti -fno-exceptions -nostdinc++ -nostdinc \
  -isystem $(SDKPATH)/System/Library/Frameworks/Kernel.framework/Headers \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -O2
  ```
- **Linker Flags**:
  ```sh
  -target x86_64-apple-macos14.0 -mkernel -fapple-kext -nostdlib \
  -Xlinker -kext -Xlinker -x -Xlinker -headerpad_max_install_names
  ```
- **Symbol Resolution Verification**: Validated cleanly against `/System/Library/KernelCollections/BootKernelExtensions.kc` using `kmutil libraries -p RTL8723BE.kext`.

---

## 3. RTL8723BE Hardware Architecture & Registers

### 3.1 Device Identification, Bus Topology & Memory Resources

Interrogation of the target hardware via the I/O Registry (`ioreg`) confirms the physical endpoint attributes:
- **PCI Address**: `2:0:0` (Bus 2, Device 0, Function 0).
- **ACPI Path**: `IOACPIPlane:/_SB/PCI0@0/RP06@1c0005/PXSX@0`.
- **PCI Identifiers**:
  - Vendor ID: `0x10EC` (Realtek Semiconductor Corp.).
  - Device ID: `0xB723` (RTL8723BE PCIe Wireless Network Adapter).
  - Subsystem Vendor ID: `0x103C` (HP Inc.).
  - Subsystem Device ID: `0x804C`.
  - Revision ID: `0x00`.
  - Class Code: `0x028000` (Network Controller / 802.11 Wireless).
- **Memory Resources**:
  - **BAR0**: 256-byte Legacy I/O port window at `0x4000` (`0x00004000 - 0x000040FF`).
  - **BAR2**: 16,384-byte (16 KB, `0x4000`) 64-bit Non-prefetchable MMIO window at base `0xf1100000` (`0xf1100000 - 0xf1103FFF`).
- **Interrupt Routing**:
  - MSI Vector: Vector 8 on `IOPCIMessagedInterruptController` (Capability Offset `0x50`).
  - Legacy Pin Interrupt: IO-APIC Pin 17 (`io-apic-0`).

### 3.2 MMIO Register Map (BAR2 16KB Window)

The 16KB MMIO address space mapped from BAR2 is partitioned into eight functional sub-blocks:

```
0x0000 +---------------------------------------------------------+
       | System Control, Clocks, Power, eFuse & Host Interrupts  | (0x0000 - 0x00FF)
0x0100 +---------------------------------------------------------+
       | MAC Command, MCU Firmware DL, H2C/C2H Mailboxes, LLT    | (0x0100 - 0x01FF)
0x0200 +---------------------------------------------------------+
       | Timers, Time Synchronization Function (TSF), Power Save | (0x0200 - 0x02FF)
0x0300 +---------------------------------------------------------+
       | PCIe DMA Control, Descriptor Start Addresses, Doorbells | (0x0300 - 0x03FF)
0x0400 +---------------------------------------------------------+
       | 802.11 Protocol Configuration, EDCA Parameters, BCN     | (0x0400 - 0x05FF)
0x0600 +---------------------------------------------------------+
       | TCR, RCR Packet Filters, Security CAM Engine            | (0x0600 - 0x06FF)
0x0800 +---------------------------------------------------------+
       | Baseband PHY Registers, AGC Tables, 3-Wire LSSI RF Port | (0x0800 - 0x0FFF)
0x1000 +---------------------------------------------------------+
       | 8051 MCU RAM Download Window (4KB Swappable Page)       | (0x1000 - 0x1FFF)
0x2000 +---------------------------------------------------------+
       | Reserved / Expansion Region                             | (0x2000 - 0x3FFF)
0x4000 +---------------------------------------------------------+
```

### 3.3 Core Register Definitions & Bitmask Specifications

| Register Name | Offset | Width | Reset Value | Bitfield Definition & Functional Description |
| :--- | :--- | :--- | :--- | :--- |
| `REG_SYS_FUNC_EN` | `0x0002` | 16-bit | `0x0000` | **System Function Enable Register**<br>• Bit 0: `FEN_PCIEDMA` (Enable PCIe DMA engine)<br>• Bit 1: `FEN_DIO_PCIE` (Enable digital I/O)<br>• Bit 2: `FEN_CPUEN` (Enable 8051 CPU clock & unreset)<br>• Bit 3: `FEN_DCORE` (Enable digital core domain)<br>• Bit 10: `FEN_ELDR` (Enable eFuse loader) |
| `REG_SYS_CLKR` | `0x0008` | 16-bit | `0x0000` | **System Clock Register**<br>• Bit 0: `SYS_CLKSEL` (0: 80MHz, 1: 40MHz)<br>• Bit 3: `MAC_CLK_EN` (Ungate MAC master clock)<br>• Bit 8: `ANA8M` (8MHz analog clock enable)<br>• Bit 11: `RING_CLK_EN` (DMA descriptor ring clock enable) |
| `REG_9346CR` | `0x000A` | 8-bit | `0x00` | **EEPROM / eFuse Command & Status**<br>• Bit 3: `EECS` (Chip select)<br>• Bit 4: `BOOT_FROM_EEPROM` (0: eFuse OTP, 1: EEPROM)<br>• Bit 5: `AUTOLOAD_OK` (1: Hardware autoload completed successfully)<br>• Bits [7:6]: Mode (`00`: Normal, `01`: Write, `10`: Read) |
| `REG_RSV_CTRL` | `0x001C` | 16-bit | `0x0E0E` | **Power & Register Access Lock**<br>• Write `0x0000` to unlock power-state and clock registers.<br>• Write `0x0E0E` to lock registers. |
| `REG_RF_CTRL` | `0x001F` | 8-bit | `0x00` | **RF Power Control**<br>• Write `0x00`: RF power down.<br>• Write `0x03`: RF power up (analog PLL active). |
| `REG_EFUSE_CTRL` | `0x0030` | 32-bit | `0x00000000` | **eFuse Indirect Access Controller**<br>• Bits [7:0]: `EFUSE_DATA` (Read/write data byte)<br>• Bits [17:8]: `EFUSE_ADDR` (10-bit physical eFuse address, 0..511)<br>• Bits [30:24]: `EFUSE_MODE` (`0x72`: Read, `0xF2`: Write)<br>• Bit 31: `EFUSE_BUSY` (1: Busy, 0: Ready) |
| `REG_EFUSE_TEST` | `0x0034` | 32-bit | `0x00000000` | **eFuse Test & Bank Selection**<br>• Bits [9:8]: Bank select (Bank 0: `00`, Bank 1: `01`) |
| `REG_EFUSE_ACCESS` | `0x00CF` | 8-bit | `0x00` | **eFuse Protection Switch**<br>• Write `0x69`: Enable eFuse read/write circuitry.<br>• Write `0x00`: Disable eFuse circuitry (low-power state). |
| `REG_MCUFWDL` | `0x0080` | 32-bit | `0x00000000` | **MCU Firmware Download Control Register**<br>• Bit 0: `FWDL_EN` (Enable FW download window at 0x1000)<br>• Bit 1: `MCUFWDL_RDY` (Host signals FW download complete)<br>• Bit 2: `FWDL_CHKSUM_RPT` (Hardware reports checksum matched)<br>• Bit 6: `WINTINI_RDY` (8051 MCU signals firmware initialized and ready)<br>• Bit 7: `RAM_DL_SEL` (FW download active / MCU reset flag) |
| `REG_MCUFWDL+2` | `0x0082` | 8-bit | `0x00` | **MCU RAM Page Select**<br>• Bits [2:0]: `RAM_PAGE_SEL` (Selects active 4KB RAM page: 0..7) |
| `REG_HIMR` | `0x00B0` | 32-bit | `0x00000000` | **Host Interrupt Mask Register (Primary)**<br>• Bit 0: `IMR_ROK` (Receive OK)<br>• Bit 1: `IMR_RDU` (Receive Descriptor Unavailable)<br>• Bit 2: `IMR_VODOK` (Voice TX Descriptor OK)<br>• Bit 3: `IMR_VIDOK` (Video TX Descriptor OK)<br>• Bit 4: `IMR_BEDOK` (Best Effort TX Descriptor OK)<br>• Bit 5: `IMR_BKDOK` (Background TX Descriptor OK)<br>• Bit 6: `IMR_MGNTDOK` (Management TX Descriptor OK)<br>• Bit 7: `IMR_HIGHDOK` (High Priority TX Descriptor OK)<br>• Bit 10: `IMR_C2HCMD` (MCU C2H Command Event) |
| `REG_HISR` | `0x00B4` | 32-bit | `0x00000000` | **Host Interrupt Status Register (Primary)**<br>• Mirrors HIMR bit layout.<br>• **Write-1-to-Clear (W1C)** semantics. |
| `REG_HIMRE` | `0x00B8` | 32-bit | `0x00000000` | **Host Interrupt Mask Extension**<br>• Bit 8: `IMR_RXFOVW` (RX FIFO Overflow)<br>• Bit 9: `IMR_TXFOVW` (TX FIFO Overflow)<br>• Bit 10: `IMR_RXERR` (RX Error)<br>• Bit 11: `IMR_TXERR` (TX Error) |
| `REG_HISRE` | `0x00BC` | 32-bit | `0x00000000` | **Host Interrupt Status Extension** (W1C flags matching HIMRE). |
| `REG_CR` | `0x0100` | 16-bit | `0x0000` | **MAC Core Command Register**<br>• Bit 0: `CR_HCI_TXDMA_EN` (Enable HCI TX DMA)<br>• Bit 1: `CR_HCI_RXDMA_EN` (Enable HCI RX DMA)<br>• Bit 2: `CR_TXDMA_EN` (Enable MAC TX DMA)<br>• Bit 3: `CR_RXDMA_EN` (Enable MAC RX DMA)<br>• Bit 4: `CR_PROTOCOL_EN` (Enable 802.11 Protocol Engine)<br>• Bit 5: `CR_SCHEDULE_EN` (Enable Hardware Scheduler)<br>• Bit 8: `CR_MAC_TX_EN` (Enable MAC Transmit)<br>• Bit 9: `CR_MAC_RX_EN` (Enable MAC Receive)<br>• Value `0x02FF`: Fully enables all MAC, DMA, and Protocol engines. |
| `REG_LLT_INIT` | `0x01E0` | 32-bit | `0x00000000` | **Linked List Table Initialization Register**<br>• Bits [7:0]: `LLT_DATA` (Target page link index)<br>• Bits [15:8]: `LLT_PAGE_ADDR` (Buffer page address 0..255)<br>• Bits [31:30]: `LLT_OP` (`01`: Write entry, `00`: Idle) |
| `REG_PCIE_CTRL_REG` | `0x0300` | 16-bit | `0x0000` | **PCIe Queue Doorbell / Polling Register**<br>• Bit 0: `PCIE_POLL_BK` (Trigger BK Queue TX DMA)<br>• Bit 1: `PCIE_POLL_BE` (Trigger BE Queue TX DMA)<br>• Bit 2: `PCIE_POLL_VI` (Trigger VI Queue TX DMA)<br>• Bit 3: `PCIE_POLL_VO` (Trigger VO Queue TX DMA)<br>• Bit 4: `PCIE_POLL_BCN` (Trigger Beacon Queue TX DMA)<br>• Bit 6: `PCIE_POLL_MGNT` (Trigger Management Queue TX DMA)<br>• Bit 7: `PCIE_POLL_HIGH` (Trigger High Priority Queue TX DMA) |
| `REG_INT_MIG` | `0x0304` | 32-bit | `0x00000000` | **Interrupt Mitigation Control**<br>• Bits [7:0]: RX packet count threshold before interrupt.<br>• Bits [15:8]: RX timer threshold in units of 32 µs. |
| `REG_RCR` | `0x0608` | 32-bit | `0x00000000` | **Receive Configuration Register**<br>• Bit 0: `RCR_AAP` (Accept all packets / promiscuous)<br>• Bit 1: `RCR_APM` (Accept packet matching physical MAC)<br>• Bit 2: `RCR_AM` (Accept multicast frames)<br>• Bit 3: `RCR_AB` (Accept broadcast frames)<br>• Bit 5: `RCR_ACRC32` (Accept error packets - 0: reject, 1: accept)<br>• Bit 14: `RCR_APP_PHYST` (Append 24-byte PHY status header)<br>• Bit 29: `RCR_ENCS` (Enable carrier sense) |
| `REG_TCR` | `0x0604` | 32-bit | `0x00000000` | **Transmit Configuration Register**<br>• Bits [10:8]: `TCR_MXDMA` (Max DMA burst size: 7 = unlimited/512B)<br>• Bit 25: `TCR_DISREQ` (Disable RTS packet request) |
| `REG_CAMCMD` | `0x0670` | 32-bit | `0x00000000` | **Security CAM Command Register**<br>• Bits [5:0]: CAM index (0..31)<br>• Bit 30: `CAM_CLR` (Clear all CAM entries)<br>• Bit 31: `CAM_POLL` (1: Write, 0: Read) |
| `REG_CAMWRITE` | `0x0674` | 32-bit | `0x00000000` | **Security CAM Data Write Window** |
| `REG_SECCFG` | `0x0680` | 16-bit | `0x0000` | **Security Engine Configuration**<br>• Bit 2: `TX_SEC_EN` (Enable TX hardware encryption)<br>• Bit 3: `RX_SEC_EN` (Enable RX hardware decryption) |
| `RFPGA0_XA_LSSIPARAMETER` | `0x0840` | 32-bit | `0x00000000` | **3-Wire LSSI RF Serial Control**<br>• Bits [19:0]: `RF_DATA` (20-bit serial payload)<br>• Bits [27:20]: `RF_ADDR` (8-bit RF register offset) |
| `REG_BB_PAD_CTRL` | `0x092C` | 32-bit | `0x00000000` | **Antenna Switch Matrix Control**<br>• `0x00000001`: Route RF Path A to Main Antenna (Port 1)<br>• `0x00000002`: Route RF Path A to Aux Antenna (Port 2) |

### 3.4 Hardware Power-On State Machine (`pwrseq`)

The RTL8723BE contains an internal hardware power controller with three primary power domains:
1. **`CARDDIS` (Card Disable)**: Deep power-down; core voltages collapsed, PCIe clock request disabled, crystal oscillator halted.
2. **`CARDEMU` (Card Emulation)**: Core logic powered, internal low-dropout (LDO) regulators stabilized, 40MHz/80MHz clocks active, MCU halted in download mode.
3. **`ACT` (Active)**: Full operational state; radio frequency synthesizer locked, Baseband DSP active, TX/RX DMA engines running.

```
       +-------------------------------------------------+
       |           CARDDIS (Card Disable State)          |
       | Core LDOs Off, Clocks Gated, Analog Isolated    |
       +-------------------------------------------------+
                                |
             [Step 1: CARDDIS -> CARDEMU Transition]
             • Write 0x00 to REG_RSV_CTRL (0x001C)
             • Clear Bit 7 of REG_APS_FSMCO+1 (0x0005)
             • Clear Bits 3,7 of 0x0005 (Suspend & PwrDown)
             • Set 0x0301 = 0x00 (Enable PCIe DMA clocking)
                                v
       +-------------------------------------------------+
       |          CARDEMU (Card Emulation State)         |
       | Core Logic Active, Clocks Running, MCU in Reset |
       +-------------------------------------------------+
                                |
             [Step 2: CARDEMU -> ACT Transition]
             • Clear Bit 5 of 0x0000 (Analog isolation off)
             • Clear Bits 2,3,4 of 0x0005 (Disable SW LPS)
             • Poll 0x0006 Bit 1 == 1 (50ms timeout: Pwr OK)
             • Set Bit 3 of REG_MULTI_FUNC_CTRL (0x0020)
             • Set Bit 4 of REG_APS_FSMCO (0x0004)
             • Set Bit 3 of REG_SYS_CLKR (0x0008: MAC Clock)
             • Write 0x02FF to REG_CR (0x0100: MAC/TRX Enable)
                                v
       +-------------------------------------------------+
       |               ACT (Active State)                |
       | Radio Synth Locked, LLT Initialized, DMA Armed  |
       +-------------------------------------------------+
```

#### Step-by-Step Implementation Sequence (`rtl8723be_pwrseq_enable`):
1. **Unlock Power Registers**: Write `0x00` to `REG_RSV_CTRL (0x001C)`.
2. **Disable Automatic Power-Down (APS)**:
   ```cpp
   uint8_t aps = mmio_read8(REG_APS_FSMCO + 1);
   aps &= ~(1 << 7); // Clear Bit 7
   mmio_write8(REG_APS_FSMCO + 1, aps);
   ```
3. **Transition `CARDDIS -> CARDEMU`**:
   - Clear suspend and power-down enable: Write `0x00` to `0x0005` bits 3 and 7.
   - Start PCIe DMA clock: Write `0x00` to `0x0301`.
   - Ungate CPU clock: Set Bit 2 of `REG_SYS_FUNC_EN + 1 (0x0003)`.
4. **Transition `CARDEMU -> ACT`**:
   - Release analog isolation: Clear Bit 5 of `0x0000`.
   - Disable low power state: Clear Bits 2, 3, 4 of `0x0005`.
   - Poll power stability: Loop up to 50 iterations with 1ms delay checking whether Bit 1 of `0x0006` is `1`. If timeout, fail initialization.
5. **Clock and Core Activation**:
   - Write `0x7F` to `REG_HWSEQ_CTRL (0x0023)`; delay 2ms.
   - Set Bit 3 of `REG_SYS_CLKR (0x0008)` to enable the MAC master clock.
   - Clear Bit 4 of `REG_GPIO_MUXCFG + 1 (0x0041)`.
   - Write `0x02FF` to `REG_CR (0x0100)` to bring MAC, DMA, and Protocol engines online.
6. **Initialize LLT Buffer Structure**: Execute LLT table configuration.

### 3.5 Linked List Table (LLT) Internal Buffer Configuration

The RTL8723BE contains an on-chip SRAM packet buffer segmented into 256 physical pages (128 bytes per page). The Linked List Table (`REG_LLT_INIT`, `0x01E0`) configures how these pages are chained into ring buffers:
- **Normal Packet Queues (BK, BE, VI, VO, MGNT, HIGH)**: Pages `0 .. 244`.
- **Reserved / Beacon Queues**: Pages `245 .. 255`.

```cpp
IOReturn RTL8723BE::initLLTTable() {
    // 1. Link normal queue pages: Page[i] points to Page[i + 1]
    for (uint32_t i = 0; i < 244; i++) {
        uint32_t value = (1 << 30) | (i << 8) | (i + 1); // Op=Write(01), Addr=i, Data=i+1
        mmio_write32(REG_LLT_INIT, value);
        if (!pollLLTReady()) return kIOReturnTimeout;
    }
    // Terminate normal queue ring
    mmio_write32(REG_LLT_INIT, (1 << 30) | (244 << 8) | 0xFF);
    if (!pollLLTReady()) return kIOReturnTimeout;

    // 2. Link reserved / beacon queue pages: Page[i] points to Page[i + 1]
    for (uint32_t i = 245; i < 255; i++) {
        uint32_t value = (1 << 30) | (i << 8) | (i + 1);
        mmio_write32(REG_LLT_INIT, value);
        if (!pollLLTReady()) return kIOReturnTimeout;
    }
    // Terminate beacon queue ring
    mmio_write32(REG_LLT_INIT, (1 << 30) | (255 << 8) | 0xFF);
    return pollLLTReady() ? kIOReturnSuccess : kIOReturnTimeout;
}
```

---

## 4. 8051 MCU Firmware Handshake Protocol

### 4.1 On-Chip Microcontroller Architecture & Role

The RTL8723BE embeds an 8-bit Intel 8051-compatible microcontroller responsible for:
1. Dynamic power management, RF calibration tracking, and thermal compensation.
2. Low-level hardware scanning and adaptive rate control feedback.
3. Host-to-Controller (`H2C`) and Controller-to-Host (`C2H`) asynchronous messaging.
4. Wi-Fi / Bluetooth coexistence arbitration over internal shared hardware lines.

Without successfully uploading and executing the firmware (`rtl8723befw.bin` / version 36), the radio frequency synthesizer will not transmit packets, and Baseband AGC tracking remains disabled.

### 4.2 Firmware Binary Format & Header Specification

The firmware binary file consists of a 32-byte descriptor header (`struct rtlwifi_firmware_header`) followed by raw 8051 executable microcode:

```c
struct rtlwifi_firmware_header {
    uint16_t signature;       /* Magic: 0x5301 for RTL8723BE (little-endian) */
    uint8_t  category;        /* Category indicator: 0x10 */
    uint8_t  function;        /* Function sub-code: 0x00 */
    uint16_t version;         /* Firmware version (e.g., 36) */
    uint8_t  subversion;      /* Subversion number */
    uint8_t  rsvd1;
    uint8_t  month;           /* Build month */
    uint8_t  date;            /* Build day */
    uint8_t  hour;            /* Build hour */
    uint8_t  minute;          /* Build minute */
    uint16_t ramcodesize;     /* Code payload size in bytes (excluding header) */
    uint16_t rsvd2;
    uint32_t svnindex;        /* SVN repository revision number */
    uint32_t rsvd3;
    uint32_t rsvd4;
    uint32_t rsvd5;
} __attribute__((packed));    /* Size exactly 32 bytes */
```

#### Header Validation:
```cpp
bool RTL8723BE::validateFirmwareHeader(const uint8_t *fwBuf, size_t fwLen) {
    if (fwLen < sizeof(struct rtlwifi_firmware_header)) return false;
    const auto *hdr = reinterpret_cast<const struct rtlwifi_firmware_header *>(fwBuf);
    uint16_t sig = OSSwapLittleToHostInt16(hdr->signature);
    if ((sig & 0xFFF0) != 0x5300) return false; // Must match 0x5301
    uint16_t codeSize = OSSwapLittleToHostInt16(hdr->ramcodesize);
    if (fwLen < sizeof(struct rtlwifi_firmware_header) + codeSize) return false;
    return true;
}
```

### 4.3 MCU Reset & Download Mode Initiation

Before streaming firmware pages, any currently executing MCU firmware must be halted and reset:
1. **MCU Self-Reset Sequence (`firmwareSelfReset`)**:
   - Pulse Bit 0 of `REG_RSV_CTRL + 1 (0x001D)` low then high with a 50 µs delay.
   - Toggle Bit 2 of `REG_SYS_FUNC_EN + 1 (0x0003)` to reset MCU clocking.
   - Clear `REG_MCUFWDL (0x0080)` to `0x00000000`.
2. **Enter Download Mode**:
   - Write `0x04` to `REG_SYS_FUNC_EN + 1 (0x0003)` (enable MCU clock).
   - Set Bit 0 (`FWDL_EN`) in `REG_MCUFWDL (0x0080)`.
   - Clear Bit 3 (`& 0xF7`) of `REG_MCUFWDL + 2 (0x0082)` (reset page index to 0).

### 4.4 4KB Page Streaming Engine into MMIO 0x1000..0x1FFF

The 8051 MCU RAM is addressed via a 4,096-byte (4 KB) sliding window mapped at MMIO offset `0x1000 .. 0x1FFF`. Firmware larger than 4KB is uploaded in sequential pages:
- **Page Size**: `4096` bytes (`0x1000`).
- **Maximum Pages**: `8` (32 KB total addressing limit).
- **Alignment**: Microcode payload is padded with zeroes up to a multiple of 4 bytes (`rtl_fill_dummy`).

```
Firmware Payload Buffer (e.g. 14,336 bytes = 3.5 pages)
+-------------------+-------------------+-------------------+----------+
|   Page 0 (4096B)  |   Page 1 (4096B)  |   Page 2 (4096B)  |P3 (2048B)|
+-------------------+-------------------+-------------------+----------+
          |                   |                   |              |
          v                   v                   v              v
    [Select Page 0]     [Select Page 1]     [Select Page 2] [Select Page 3]
  REG_MCUFWDL+2 = 0   REG_MCUFWDL+2 = 1   REG_MCUFWDL+2 = 2 REG_MCUFWDL+2 = 3
          |                   |                   |              |
          +-------------------+-------------------+--------------+
                              |
                              v Stream 32-bit dwords
               +-----------------------------+
               | BAR2 MMIO 0x1000 .. 0x1FFF  |
               +-----------------------------+
```

```cpp
IOReturn RTL8723BE::downloadFirmwarePages(const uint8_t *codePayload, size_t codeSize) {
    size_t pageCount = (codeSize + 4095) / 4096;
    if (pageCount > 8) return kIOReturnNoMemory;

    for (uint8_t page = 0; page < pageCount; page++) {
        // 1. Select page index in REG_MCUFWDL+2 [bits 2:0]
        uint8_t pageReg = mmio_read8(REG_MCUFWDL + 2);
        pageReg = (pageReg & 0xF8) | (page & 0x07);
        mmio_write8(REG_MCUFWDL + 2, pageReg);

        // 2. Stream chunk into 0x1000..0x1FFF window
        size_t offset = page * 4096;
        size_t chunkLen = std::min((size_t)4096, codeSize - offset);
        
        for (size_t i = 0; i < chunkLen; i += 4) {
            uint32_t val = *reinterpret_cast<const uint32_t *>(codePayload + offset + i);
            mmio_write32(0x1000 + i, val);
        }
    }

    // 3. Exit download mode
    uint8_t fwdl = mmio_read8(REG_MCUFWDL);
    mmio_write8(REG_MCUFWDL, fwdl & ~0x01); // Clear FWDL_EN
    mmio_write8(REG_MCUFWDL + 1, 0x00);
    return kIOReturnSuccess;
}
```

### 4.5 Checksum Handshake, Self-Reset & WINTINI_RDY Verification

Once page streaming concludes, the driver executes the `rtl8723_fw_free_to_go` handshake:

```
Driver Host                                               RTL8723BE 8051 MCU
    |                                                             |
    |---- 1. Poll REG_MCUFWDL Bit 2 (FWDL_CHKSUM_RPT) ---------->| (Hardware computes
    |<--- Returns 1 (Checksum OK, within 6000 cycles / 30ms) -----|  internal CRC)
    |                                                             |
    |---- 2. Set Bit 1 (MCUFWDL_RDY) & Clear Bit 6 (WINTINI_RDY) ->|
    |                                                             |
    |---- 3. Trigger MCU Self-Reset (firmwareSelfReset) --------->| (MCU resets & executes
    |                                                             |  reset vector)
    |---- 4. Poll REG_MCUFWDL Bit 6 (WINTINI_RDY) --------------->|
    |<--- Returns 1 (Firmware Initialized, within 6000 cycles) ---| (MCU enters run loop)
    |                                                             |
[Driver marks Firmware ACTIVE]
```

- **Checksum Timeout**: 6000 iterations $\times$ 5 µs = 30 ms. If bit 2 fails to assert, download is aborted (`kIOReturnIOError`).
- **Initialization Timeout**: 6000 iterations $\times$ 5 ms = 30 seconds. If bit 6 fails to assert, driver logs firmware hang.

---

## 5. eFuse & Factory Calibration Mapping

### 5.1 Physical OTP Array vs. Logical EEPROM Shadow Map

The RTL8723BE contains an on-chip One-Time Programmable (OTP) eFuse matrix consisting of **256 physical bytes** (`EFUSE_REAL_CONTENT_LEN`). Because physical eFuse bits cannot be rewritten from 1 to 0, Realtek utilizes a **Packet-based Programming (PG)** format that dynamically expands the 256 physical bytes into a virtual **512-byte logical EEPROM shadow map** (`HWSET_MAX_SIZE`).

### 5.2 Autoload Detection & Manual Indirect Access Protocol (`REG_EFUSE_CTRL`)

At power-up, the internal hardware autoload sequencer reads the eFuse array and mirrors the MAC address and configuration registers into the MAC register block:
- **Autoload Status**: Read `REG_9346CR (0x000A)`.
  - Bit 4 (`BOOT_FROM_EEPROM`): `0` = eFuse OTP, `1` = External 9346 EEPROM.
  - Bit 5 (`AUTOLOAD_OK`): `1` = Hardware autoload succeeded; `0` = Autoload failed (driver must use fallback calibration values).

#### Manual Indirect Physical Readout Protocol:
To read physical eFuse bytes (e.g., when reconstructing the 512-byte map):
```cpp
uint8_t RTL8723BE::readPhysicalEfuseByte(uint16_t addr) {
    // 1. Enable eFuse access circuitry
    mmio_write8(REG_EFUSE_ACCESS, 0x69);

    // 2. Write address to REG_EFUSE_CTRL [bits 17:8]
    mmio_write8(REG_EFUSE_CTRL + 1, addr & 0xFF);
    uint8_t highAddr = (mmio_read8(REG_EFUSE_CTRL + 2) & 0xFC) | ((addr >> 8) & 0x03);
    mmio_write8(REG_EFUSE_CTRL + 2, highAddr);

    // 3. Set read command (0x72) in bits [30:24]
    mmio_write8(REG_EFUSE_CTRL + 3, 0x72);

    // 4. Poll Bit 7 of REG_EFUSE_CTRL+3 (busy cleared / ready)
    uint32_t timeout = 100;
    while ((mmio_read8(REG_EFUSE_CTRL + 3) & 0x80) == 0 && timeout--) {
        IODelay(10);
    }

    // 5. Read byte from REG_EFUSE_CTRL (0x0030)
    uint8_t data = mmio_read8(REG_EFUSE_CTRL);

    // 6. Disable access circuitry
    mmio_write8(REG_EFUSE_ACCESS, 0x00);
    return data;
}
```

### 5.3 Physical PG Packet Decoding Algorithm

The physical eFuse array contains variable-length PG packets. The driver decodes these packets into the 512-byte logical map:

```
Standard 1-Byte Header (Value 0x00 .. 0xEF):
+-------------------------------+-------------------------------+
|      Offset Block [7:4]       |       Word Mask [3:0]         |
+-------------------------------+-------------------------------+
  Block: Target offset = Block * 16 (0 .. 240)
  Word Mask: 4 bits corresponding to four 16-bit words (8 bytes)

Extended 2-Byte Header (Value 0xF0 .. 0xFE in Byte 0):
+-------------------------------+-------------------------------+
|     0xF0 .. 0xFE (Byte 0)     |  Word Mask [7:4] | Block [3:0]| (Byte 1)
+-------------------------------+-------------------------------+
  Enables addressing offsets beyond 240 up to 512 bytes.

Termination Byte:
  0xFF indicates empty / unprogrammed end of eFuse.
```

```cpp
void RTL8723BE::decodeEfuseLogicalMap(const uint8_t *rawEfuse, uint8_t *logicalMap) {
    memset(logicalMap, 0xFF, 512); // Initialize with 0xFF (unprogrammed)
    size_t idx = 0;

    while (idx < 256) {
        uint8_t tag = rawEfuse[idx++];
        if (tag == 0xFF) break; // End of programmed eFuse

        uint8_t block = 0;
        uint8_t wordMask = 0;

        if ((tag & 0xF0) == 0xF0) {
            // Extended 2-byte header
            if (idx >= 256) break;
            uint8_t ext = rawEfuse[idx++];
            block = ((tag & 0x0F) << 4) | (ext & 0x0F);
            wordMask = (ext >> 4) & 0x0F;
        } else {
            // Standard 1-byte header
            block = (tag >> 4) & 0x0F;
            wordMask = tag & 0x0F;
        }

        uint16_t baseOffset = block * 16;
        for (int word = 0; word < 4; word++) {
            if ((wordMask & (1 << word)) == 0) {
                // Word is present: 2 data bytes follow
                if (idx + 1 >= 256) break;
                uint8_t lowByte = rawEfuse[idx++];
                uint8_t highByte = rawEfuse[idx++];
                if (baseOffset + word * 2 + 1 < 512) {
                    logicalMap[baseOffset + word * 2] = lowByte;
                    logicalMap[baseOffset + word * 2 + 1] = highByte;
                }
            }
        }
    }
}
```

### 5.4 Logical Calibration Map Layout & Default Fallbacks

| Logical Offset | Length | Calibration Field | Description | Conservative Default Fallback |
| :--- | :--- | :--- | :--- | :--- |
| `0x0010 - 0x0015` | 6 B | `EEPROM_TX_PWR_CCK` | CCK base TX power index for 6 channel groups | `0x2D` (45 decimal) |
| `0x0016 - 0x001A` | 5 B | `EEPROM_TX_PWR_HT40`| HT40 base TX power index for 5 channel groups | `0x2D` (45 decimal) |
| `0x001B` | 1 B | `EEPROM_TX_PWR_DIFF`| Difference for HT20 / OFDM relative to HT40 | `0x02` |
| `0x00B8` | 1 B | `EEPROM_CHANNEL_PLAN`| Regulatory domain (`0x00`: World 1–13, `0x08`: FCC) | `0x00` (World 13 channels) |
| `0x00B9` | 1 B | `EEPROM_XTAL_TRIM` | Crystal oscillator capacitive load trim | `0x20` (if unprogrammed `0xFF`) |
| `0x00BA` | 1 B | `EEPROM_THERMAL_METER`| Factory thermal sensor baseline calibration | `0x1A` (26 decimal) |
| `0x00BB` | 1 B | `EEPROM_IQK_LCK` | IQK / LCK factory calibration flags | `0x00` |
| `0x00C1` | 1 B | `EEPROM_RF_BOARD_OPT`| RF board configuration / regulatory options | `0x00` |
| `0x00C3` | 1 B | `EEPROM_RF_BT_SETTING`| BT / Antenna setting (Bit 0: antenna count) | `0x01` (Single antenna) |
| `0x00D0 - 0x00D5` | 6 B | `EEPROM_MAC_ADDR` | Factory Wi-Fi Station Physical MAC Address | Read from MAC regs or fallback |
| `0x00D6 - 0x00D7` | 2 B | `EEPROM_VID` | PCI Vendor ID mirror (`0x10EC`) | `0x10EC` |
| `0x00D8 - 0x00D9` | 2 B | `EEPROM_DID` | PCI Device ID mirror (`0xB723`) | `0xB723` |
| `0x00DA - 0x00DB` | 2 B | `EEPROM_SVID` | PCI Subsystem Vendor ID mirror (`0x103C`) | `0x103C` |
| `0x00DC - 0x00DD` | 2 B | `EEPROM_SMID` | PCI Subsystem Device ID mirror (`0x804C`) | `0x804C` |

---

## 6. TX/RX DMA Descriptor Rings & Memory Architecture

### 6.1 DMA Ring Architecture & 256-Byte Boundary Alignment

The RTL8723BE PCIe DMA engine operates entirely over circular descriptor rings located in host memory.
- **Physical Memory Allocation**: Allocated via `IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0x000000FFFFFFFFULL)`.
- **Memory Alignment**: Hardware requires that the base physical address of every descriptor ring be **strictly aligned to a 256-byte boundary** (`0x100`).
- **64-bit Addressing**: Hardware descriptors contain dual 32-bit fields (`buffaddr` and `buffaddr64`) supporting full 64-bit physical memory addressing.

```
Host Coherent DMA Memory (256-byte aligned base)
+--------------------------------------------------------------------------+
| Slot 0 (Desc 0) | Slot 1 (Desc 1) | ... | Slot N-2        | Slot N-1 (EOR) |
+--------------------------------------------------------------------------+
       |                 |                       |                 |
       v                 v                       v                 v
  Buffer 0          Buffer 1               Buffer N-2        Buffer N-1
  (2KB mbuf)        (2KB mbuf)             (2KB mbuf)        (2KB mbuf)
```

### 6.2 Hardware Priority Queues & Base Address Registers

The RTL8723BE provides seven hardware transmit queues implementing Enhanced Distributed Channel Access (EDCA) priority scheduling, plus one receive ring:

| Queue Name | Queue Identifier | Ring Size | MMIO Base Address Register | Polling Doorbell Bit in `0x0300` |
| :--- | :--- | :--- | :--- | :--- |
| **Background (`AC_BK`)** | `BK_QUEUE` (0) | 128 descs | `REG_BKQ_DESA` (`0x0338`) | Bit 0 (`BIT(0)`) |
| **Best Effort (`AC_BE`)** | `BE_QUEUE` (1) | 256 descs | `REG_BEQ_DESA` (`0x0330`) | Bit 1 (`BIT(1)`) |
| **Video (`AC_VI`)** | `VI_QUEUE` (2) | 128 descs | `REG_VIQ_DESA` (`0x0328`) | Bit 2 (`BIT(2)`) |
| **Voice (`AC_VO`)** | `VO_QUEUE` (3) | 128 descs | `REG_VOQ_DESA` (`0x0320`) | Bit 3 (`BIT(3)`) |
| **Beacon (`BCN`)** | `BEACON_QUEUE` (4) | 2 descs | `REG_BCNQ_DESA` (`0x0308`) | Bit 4 (`BIT(4)`) |
| **Management (`MGNT`)**| `MGNT_QUEUE` (6) | 128 descs | `REG_MGQ_DESA` (`0x0318`) | Bit 6 (`BIT(6)`) |
| **High Priority (`HIGH`)**| `HIGH_QUEUE` (7)| 128 descs | `REG_HQ_DESA` (`0x0310`) | Bit 7 (`BIT(7)`) |
| **Receive Ring (`RX`)** | `RX_QUEUE` (N/A) | 256 descs | `REG_RX_DESA` (`0x0340`) | Auto-fetching (continuous) |

### 6.3 40-Byte Transmit Descriptor Layout (`struct tx_desc_8723be`)

Every packet queued for transmission is preceded by a 40-byte hardware descriptor (`struct tx_desc_8723be`). The structure occupies 12 32-bit dwords (48 bytes padded in memory):

```c
struct tx_desc_8723be {
    /* Dword 0 */
    uint32_t pktsize:16;        /* Total frame byte count (payload + MAC header) */
    uint32_t offset:8;          /* Descriptor offset: sizeof(struct tx_desc_8723be) = 40 */
    uint32_t bmc:1;             /* Broadcast / Multicast frame flag */
    uint32_t htc:1;             /* High Throughput Control field present */
    uint32_t lastseg:1;         /* Last segment of frame (1 = unfragmented) */
    uint32_t firstseg:1;        /* First segment of frame (1 = unfragmented) */
    uint32_t linip:1;           /* IP checksum calculation offload */
    uint32_t noacm:1;           /* Bypasses Admission Control Mandatory */
    uint32_t gf:1;              /* GreenField mode enable */
    uint32_t own:1;             /* Ownership bit: 1 = DMA owns, 0 = Host CPU owns */

    /* Dword 1 */
    uint32_t macid:7;           /* Target station MACID (0..127) */
    uint32_t queuesel:5;        /* Queue: BE=0, BK=2, VI=5, VO=7, BCN=16, HIGH=17, MGNT=18 */
    uint32_t rdg_nav_ext:1;     /* Reverse Direction Grant NAV extension */
    uint32_t lsig_txop_en:1;    /* L-SIG TXOP protection */
    uint32_t pifs:1;            /* PIFS timing */
    uint32_t rateid:5;          /* Rate adaptive table index */
    uint32_t navusehdr:1;       /* Use NAV duration from MAC header */
    uint32_t en_desc_id:1;      /* Enable descriptor ID */
    uint32_t sectype:2;         /* Security Type: 0=None, 1=WEP, 2=TKIP, 3=AES-CCMP */
    uint32_t pktoffset:8;       /* Packet offset spacing */

    /* Dword 2 */
    uint32_t rsvd_dw2_0:12;
    uint32_t agg_en:1;          /* A-MPDU aggregation enable */
    uint32_t bk:1;              /* Break aggregate */
    uint32_t rsvd_dw2_1:6;
    uint32_t ampdudensity:3;    /* MPDU start spacing density */
    uint32_t rsvd_dw2_2:9;

    /* Dword 3 */
    uint32_t rsvd_dw3_0:16;
    uint32_t seq:12;            /* 802.11 sequence number */
    uint32_t rsvd_dw3_1:3;
    uint32_t hwseq_en:1;        /* 1 = Hardware generates sequence number; 0 = Driver */

    /* Dword 4 */
    uint32_t rtsrate:5;         /* RTS transmit rate index */
    uint32_t rsvd_dw4_0:6;
    uint32_t cts2self:1;        /* CTS-to-self protection enable */
    uint32_t rts_en:1;          /* RTS/CTS protection enable */
    uint32_t hw_rts_en:1;       /* Hardware RTS enable */
    uint32_t port_id:3;         /* Port ID */
    uint32_t rsvd_dw4_1:15;

    /* Dword 5 */
    uint32_t txrate:6;          /* Initial transmit rate (CCK=0..3, OFDM=4..11, MCS0..7=12..19) */
    uint32_t shortgi:1;         /* Short Guard Interval (400ns) */
    uint32_t rsvd_dw5_0:1;
    uint32_t txrate_fb_lmt:5;   /* Rate fallback limit retry count */
    uint32_t rsvd_dw5_1:19;

    /* Dword 6 */
    uint32_t rsvd_dw6;

    /* Dword 7 */
    uint32_t txbuffersize:16;   /* Buffer size in bytes */
    uint32_t rsvd_dw7:16;

    /* Dwords 8 - 11: 64-bit Buffer Physical Addresses */
    uint32_t txbuffaddr;        /* Low 32 bits of physical packet buffer */
    uint32_t txbuffaddr64;      /* High 32 bits of physical packet buffer */
    uint32_t nextdescaddr;      /* Low 32 bits of next TX descriptor (chaining) */
    uint32_t nextdescaddr64;    /* High 32 bits of next TX descriptor */
} __attribute__((packed));
```

### 6.4 32-Byte Receive Descriptor Layout (`struct rx_desc_8723be`)

Every incoming packet transferred from radio into host memory is documented by a 32-byte RX descriptor:

```c
struct rx_desc_8723be {
    /* Dword 0 */
    uint32_t length:14;         /* Total received frame length (including 4-byte CRC) */
    uint32_t crc32:1;           /* CRC32 error flag (1 = CRC failed, 0 = OK) */
    uint32_t icverror:1;        /* ICV / MIC error flag (1 = decryption failed) */
    uint32_t drv_infosize:4;    /* Driver info / PHY status size in units of 8 bytes */
    uint32_t security:3;        /* Decryption status: 0=None, 1=WEP, 2=TKIP, 4=AES-CCMP */
    uint32_t qos:1;             /* QoS data frame */
    uint32_t shift:2;           /* Buffer shift offset (0..2 bytes) for IP header alignment */
    uint32_t phystatus:1;       /* PHY status report appended to frame */
    uint32_t swdec:1;           /* Software decrypted */
    uint32_t rsvd_dw0:2;
    uint32_t eor:1;             /* End of Ring flag (1 = last descriptor in ring) */
    uint32_t own:1;             /* Ownership bit: 1 = DMA owns; 0 = Host CPU owns */

    /* Dword 1 */
    uint32_t macid:7;           /* Source MACID */
    uint32_t rsvd_dw1_0:8;
    uint32_t paggr:1;           /* Frame was part of an A-MPDU aggregate */
    uint32_t rsvd_dw1_1:13;
    uint32_t type:2;            /* 802.11 Frame Type: 00=Management, 01=Control, 10=Data */
    uint32_t mc:1;              /* Multicast address match */

    /* Dword 2 */
    uint32_t seq:12;            /* 802.11 sequence number */
    uint32_t frag:4;            /* Fragment number */
    uint32_t rsvd_dw2:12;
    uint32_t rpt_sel:1;         /* Report select: 0 = Normal packet, 1 = C2H event report */
    uint32_t rsvd_dw2_1:3;

    /* Dword 3 */
    uint32_t rxmcs:6;           /* Received MCS index / legacy rate */
    uint32_t rxht:1;            /* 1 = 802.11n HT packet, 0 = Legacy 802.11b/g */
    uint32_t rsvd_dw3_0:2;
    uint32_t bandwidth:1;       /* Bandwidth: 0 = 20 MHz, 1 = 40 MHz */
    uint32_t rsvd_dw3_1:22;

    /* Dword 4 */
    uint32_t rsvd_dw4;

    /* Dword 5 */
    uint32_t tsfl;              /* MAC Time Synchronization Function (TSF) lower 32 bits */

    /* Dwords 6 - 7: 64-bit Buffer Physical Addresses */
    uint32_t bufferaddress;     /* Low 32 bits of physical host RX buffer */
    uint32_t bufferaddress64;   /* High 32 bits of physical host RX buffer */
} __attribute__((packed));
```

### 6.5 Ownership Bit Arbitration, Ring Wrapping (EOR) & Doorbell Mechanics

#### Ownership Protocol:
- **TX Descriptor**:
  1. The host driver inspects `desc->own`. If `1`, the ring is full; the driver halts transmission flow.
  2. The host writes packet data, length, flags, and physical address.
  3. Memory barrier (`OSSynchronizeIO()`) is executed.
  4. The host sets `desc->own = 1`.
  5. The host rings the doorbell by writing the queue bit to `REG_PCIE_CTRL_REG (0x0300)`.
  6. The DMA engine transmits the frame and clears `desc->own = 0`.
  7. Hardware raises interrupt `IMR_*DOK` in `REG_HISR`.
- **RX Descriptor**:
  1. The driver pre-allocates contiguous 2KB buffers, writes their physical addresses to `bufferaddress`, sets `desc->own = 1`, and sets `desc->eor = 1` on index $N - 1$.
  2. When a wireless frame arrives, hardware writes data to the buffer, populates descriptor status, and sets `desc->own = 0`.
  3. Hardware raises interrupt `IMR_ROK` in `REG_HISR`.
  4. Driver workloop iterates through descriptors while `desc->own == 0`.
  5. Driver extracts packet, attaches new buffer, sets `desc->own = 1`, and advances ring index.

#### The Crucial End-of-Ring (`EOR`) Rule:
Bit 30 (`eor`) of Dword 0 must be set to `1` **strictly on descriptor index $N - 1$** of the RX ring. If `eor` is omitted, the hardware DMA controller will wrap past the allocated buffer bounds into adjacent kernel memory, inducing catastrophic physical memory corruption and host kernel panics.

### 6.6 Ring Starvation, Buffer Replenishment & Interrupt Mitigation

- **Receive Descriptor Unavailable (`IMR_RDU`)**:
  If the host fails to process incoming frames rapidly enough, the RX ring exhausts available descriptors (`own == 0` for all slots). The hardware drops incoming packets on the wire and asserts Bit 1 (`IMR_RDU`) in `REG_HISR`.
  - *Recovery*: The driver workloop must replenish all exhausted descriptors, write `1` to clear `HISR_RDU`, and trigger RX re-polling by writing `0x01` to `REG_CR + 1`.
- **Interrupt Mitigation**:
  To prevent interrupt storms under Gigabit or high-throughput A-MPDU bursts, `REG_INT_MIG (0x0304)` is programmed:
  - Packet count threshold: 4 packets.
  - Timer delay: $4 \times 32\ \mu\text{s} = 128\ \mu\text{s}$.

---

## 7. Baseband, RF Tuning & Antenna Diversity

### 7.1 Bulk Register Table Initialization Sequences

The Baseband and Radio Frequency subsystems require bulk programming of pre-calibrated factory register tables:
1. `RTL8723BEMAC_1T_ARRAY` (103 entries): Sets MAC clock distribution, FIFO cut-through thresholds, and contention timers.
2. `RTL8723BEPHY_REG_1TARRAY` (193 entries): Programs Baseband DSP filters, ADC/DAC sample rates, OFDM equalizers, and CCK correlators.
3. `RTL8723BEAGCTAB_1TARRAY` (131 entries): Configures Automatic Gain Control curves and Low Noise Amplifier (LNA) gain steps.
4. `RTL8723BE_RADIOA_1TARRAY` (136 entries): Configures RF Radio Path A synthesizer registers over the 3-wire LSSI serial bus.
5. `RTL8723BEPHY_REG_ARRAY_PG` (18 entries): Baseband power-by-rate compensation entries.

### 7.2 3-Wire LSSI Serial RF Access Protocol (`0x0840`)

Radio Frequency registers are not mapped directly into the PCI MMIO space. Instead, they are accessed through a serial 3-wire Low-Speed Serial Interface (LSSI) managed by Baseband register `0x0840` (`RFPGA0_XA_LSSIPARAMETER`):

```
Baseband Register 0x0840 (32 bits)
+-----------------------+---------------------------------------+
|  RF Addr [27:20] (8b) |          RF Data [19:0] (20 bits)     |
+-----------------------+---------------------------------------+
```

```cpp
void RTL8723BE::writeRFRegister(uint8_t offset, uint32_t data) {
    IOLockLock(fRFLock);
    // Format: [27:20] = RF Register Offset, [19:0] = 20-bit Data
    uint32_t lssiValue = ((static_cast<uint32_t>(offset) << 20) | (data & 0x000FFFFF)) & 0x0FFFFFFF;
    mmio_write32(RFPGA0_XA_LSSIPARAMETER, lssiValue);
    IODelay(1); // 1 µs serial bus write settling delay
    IOLockUnlock(fRFLock);
}

uint32_t RTL8723BE::readRFRegister(uint8_t offset) {
    IOLockLock(fRFLock);
    // RF read uses PI read protocol mediated through Baseband 0x08B8 / 0x0840
    uint32_t lssiValue = (static_cast<uint32_t>(offset) << 20) & 0x0FF00000;
    mmio_write32(RFPGA0_XA_LSSIPARAMETER, lssiValue);
    IODelay(10);
    uint32_t data = mmio_read32(0x08B8) & 0x000FFFFF; // 20-bit read result
    IOLockUnlock(fRFLock);
    return data;
}
```

### 7.3 2.4 GHz Frequency Synthesizer & Channel Tuning (Channels 1–13)

Channel tuning programs the RF PLL synthesizer via RF register `0x18` (`RF_CHNLBW`):

```cpp
IOReturn RTL8723BE::setChannel(uint8_t channel, uint8_t bandwidth) {
    if (channel < 1 || channel > 13) return kIOReturnBadArgument;

    // 1. Read current RF register 0x18
    uint32_t rf18 = readRFRegister(0x18);
    rf18 &= 0xFFF00000; // Clear channel bits [9:0] and bandwidth bits [11:10]

    // 2. Set channel number in bits [9:0]
    rf18 |= (channel & 0x3FF);

    // 3. Set channel bandwidth
    if (bandwidth == kChannelBandwidth20MHz) {
        rf18 |= (1 << 10) | (1 << 11); // 20 MHz mode
    } else {
        rf18 |= (1 << 10);              // 40 MHz mode
    }

    // 4. Write back to RF 0x18
    writeRFRegister(0x18, rf18);
    IODelay(10000); // 10 ms PLL synthesizer lock time

    // 5. Update Baseband TX power per channel from eFuse calibration
    applyTxPowerForChannel(channel);
    return kIOReturnSuccess;
}
```

### 7.4 RF Front-End Antenna Diversity & HP Single-Antenna Switching (`0x092C`)

#### The HP Single-Antenna Flaw:
On laptop motherboards manufactured by HP (Subsystem `103c:804c`), cost-reduction engineering frequently installed only **one physical antenna wire** inside the laptop display lid. On many production batches, this single wire is physically plugged into **Antenna Port 2 (Auxiliary)** rather than Port 1 (Main).
- If the driver defaults to Main Antenna (`0x00000001` at `0x092C`), the RF front-end is routed to an unpopulated, open SMA connector. Signal attenuation exceeds 35 dB, resulting in RSSI readings of -90 dBm to -98 dBm (inability to discover or associate with access points).
- When the driver switches to Aux Antenna (`0x00000002` at `0x092C`), signal strength immediately recovers to nominal levels (-40 dBm to -65 dBm).

```
   RTL8723BE RF Output
          |
          v
   [RF SPDT Switch Matrix (Controlled by BB 0x092C)]
          |
          +-----------------------+
          |                       |
      (Port 1)                (Port 2)
    Main Antenna            Aux Antenna
   [UNCONNECTED]       [CONNECTED DISPLAY WIRE]
   (Signal -95 dBm)        (Signal -50 dBm)
```

#### Antenna Control API:
```cpp
void RTL8723BE::setAntennaPath(uint8_t ant) {
    if (ant == 1) {
        // Main Antenna
        mmio_write32(REG_BB_PAD_CTRL, 0x00000001);
        fActiveAntenna = 1;
    } else if (ant == 2) {
        // Aux Antenna (Default for HP 103C:804C)
        mmio_write32(REG_BB_PAD_CTRL, 0x00000002);
        fActiveAntenna = 2;
    }
}
```

---

## 8. 802.11 Protocol & Data Path Engine

### 8.1 Active / Passive Channel Scanning Engine & Beacon/Probe Cache

The driver implements an autonomous 802.11 scan engine operating over 2.4 GHz Channels 1 through 13:
1. **Channel Dwelling**: Driver steps through channels $1 \dots 13$, dwelling for 60 ms per channel.
2. **Active Scan**: Transmits an 802.11 Probe Request frame with broadcast destination (`FF:FF:FF:FF:FF:FF`) and wildcard SSID (`0` length) on the active channel via `MGNT_QUEUE`.
3. **Passive Scan**: Listens silently on each channel for periodic 802.11 Beacon frames.
4. **Information Element (IE) Parser**: Extracts:
   - Tag 0: SSID (up to 32 bytes).
   - Tag 1: Supported Rates (1, 2, 5.5, 11 Mbps CCK; 6, 9, 12, 18, 24, 36, 48, 54 Mbps OFDM).
   - Tag 3: DSSS Parameter Set (Current Channel).
   - Tag 45 / 61: HT Capabilities & HT Information (802.11n MCS support).
   - Tag 48: RSN Information Element (WPA2-PSK, AES-CCMP cipher, AKM suite `00-0F-AC:2`).
5. **Scan Cache**: Discovered BSSIDs are cached in a thread-safe table with exponential moving average (EMA) RSSI smoothing:
   $$\text{RSSI}_{\text{smoothed}} = \alpha \cdot \text{RSSI}_{\text{new}} + (1 - \alpha) \cdot \text{RSSI}_{\text{prev}}, \quad \alpha = 0.3$$

### 8.2 Authentication & Association State Machine

```
   +-------------------+
   |   DISCONNECTED    |
   +-------------------+
             | Connect command received (SSID, Passphrase)
             v
   +-------------------+
   |   AUTHENTICATING  |
   +-------------------+
             | Send Auth Frame (Algorithm: Open System, Seq: 1)
             | Wait for Auth Frame (Seq: 2, Status: 0 = Success)
             v
   +-------------------+
   |    ASSOCIATING    |
   +-------------------+
             | Send Assoc Req (Cap, Listen Interval, SSID, Rates, RSN IE)
             | Wait for Assoc Resp (Status: 0, Assigned AID: 1..2007)
             v
   +-------------------+
   |  WPA2_HANDSHAKE   |
   +-------------------+
             | Execute 4-Way EAPOL-Key Handshake (Messages 1..4)
             v
   +-------------------+
   |     CONNECTED     | ---> Call setLinkStatus(Active) [Triggers macOS DHCP]
   +-------------------+
```

### 8.3 WPA2-PSK 4-Way Handshake Engine (EAPOL-Key Messages 1–4)

```
Access Point (AP)                                            Station (RTL8723BE)
      |                                                               |
      |---- 1. EAPOL-Key Msg 1 (ANonce, Replay Counter C1) ---------->|
      |                                                               | Derive PTK:
      |                                                               | KCK, KEK, TK
      |                                                               | Generate SNonce
      |<--- 2. EAPOL-Key Msg 2 (SNonce, RSN IE, Replay C1, MIC) ------| Compute MIC (KCK)
      |                                                               |
      | Verify MIC (KCK)                                              |
      | Encrypt GTK under KEK                                         |
      |---- 3. EAPOL-Key Msg 3 (ANonce, Replay C2=C1+1, GTK, MIC) ---->|
      |                                                               | Verify MIC
      |                                                               | Decrypt GTK (KEK)
      |<--- 4. EAPOL-Key Msg 4 (Replay C2, MIC) ----------------------| Install TK & GTK
      |                                                               |
[AP installs PTK/GTK]                                         [Station installs PTK/GTK]
      |                                                               |
      |<================= Encrypted CCMP Data Frames ================>|
```

### 8.4 Key Derivation Functions: PBKDF2-HMAC-SHA1 & PRF-512

#### 1. Pairwise Master Key (PMK) Derivation (RFC 2898 / RFC 6070):
$$\text{PMK} = \text{PBKDF2-HMAC-SHA1}(\text{Passphrase}, \text{SSID}, \text{Iterations} = 4096, \text{KeyLen} = 32\ \text{bytes})$$

#### 2. Pairwise Transient Key (PTK) Derivation (IEEE 802.11i PRF-512):
$$\text{PTK} = \text{PRF-512}(\text{PMK}, \text{"Pairwise key expansion"}, \min(\text{MAC}_{\text{STA}}, \text{MAC}_{\text{AP}}) \parallel \max(\text{MAC}_{\text{STA}}, \text{MAC}_{\text{AP}}) \parallel \min(\text{ANonce}, \text{SNonce}) \parallel \max(\text{ANonce}, \text{SNonce}))$$

The resulting 512 bits (64 bytes) are partitioned:
- **Bits $0 \dots 127$ (16 bytes)**: **KCK (Key Confirmation Key)** — Used to compute and verify the 16-byte HMAC-SHA1 MIC in EAPOL-Key frames.
- **Bits $128 \dots 255$ (16 bytes)**: **KEK (Key Encryption Key)** — Used with NIST AES Key Wrap (RFC 3394) to decrypt the Group Transient Key (GTK) delivered in Message 3.
- **Bits $256 \dots 383$ (16 bytes)**: **TK (Temporal Key)** — Used for AES-128 CCM unicast data packet encryption and decryption.
- **Bits $384 \dots 511$ (16 bytes)**: Reserved / TX/RX MIC keys (used only for TKIP).

### 8.5 CCMP (AES-128 CCM) Encryption / Decryption & Replay Defense

Every 802.11 data frame transmitted or received over a protected WPA2 link is processed via Counter Mode with Cipher Block Chaining Message Authentication Code Protocol (CCMP):
- **Packet Number (PN)**: A 48-bit monotonically increasing counter. Transmit frames increment PN by 1 per frame.
- **Replay Protection**: The receiver verifies that the received $PN > \text{last\_accepted\_}PN$. If $PN \le \text{last\_accepted\_}PN$, the packet is discarded as a replay attack.
- **8-Byte CCMP Header**: Inserted directly after the 802.11 MAC header:
  - Byte 0: $PN_0$
  - Byte 1: $PN_1$
  - Byte 2: `0x00` (Reserved)
  - Byte 3: `0x20` (ExtIV flag set = 1; KeyID bits [7:6] = 0)
  - Bytes 4–7: $PN_2, PN_3, PN_4, PN_5$
- **AES-128 CCM Authentication**: Additional Authenticated Data (AAD) is constructed from the 802.11 header (Frame Control, Addresses 1, 2, 3, Sequence Control, and QoS Control).
- **MIC Verification**: An 8-byte Message Integrity Code (MIC) is calculated across AAD and payload. Transmit frames append the MIC; receive frames verify and strip the MIC before decapsulation.

### 8.6 Ethernet II <-> RFC 1042 LLC/SNAP 802.11 QoS Data Frame Translation

```
Ethernet II Frame (From macOS TCP/IP Stack via outputPacket):
+--------------------+--------------------+--------------------+-----------------------+
| Destination MAC    | Source MAC         | EtherType (2B)     | Payload Data          |
| (6 Bytes)          | (6 Bytes)          | e.g. 0x0800 (IPv4) | (46 - 1500 Bytes)     |
+--------------------+--------------------+--------------------+-----------------------+
                                  |
                                  v Encapsulation & Encryption Pipeline
802.11 QoS Data Frame (On the Wire):
+--------------------+--------------------+--------------------+-----------------------+
| 802.11 MAC Header  | CCMP Header (8B)   | LLC/SNAP (8B)      | Encrypted Payload     |
| (26 Bytes)         | PN0..PN5, ExtIV    | AA-AA-03-00-00-00  | (AES-128-CCM)         |
| Type: QoS Data     |                    | EtherType          |                       |
+--------------------+--------------------+--------------------+-----------------------+
| CCMP MIC (8 Bytes) | 802.11 FCS (4B)    |
| (Integrity Check)  | (Hardware CRC32)   |
+--------------------+--------------------+
```

#### Inbound Decapsulation & EAPOL Interception:
When an 802.11 Data frame arrives via RX DMA:
1. Verify CCMP MIC and decrypt payload using TK (or GTK for broadcast/multicast).
2. Validate LLC/SNAP header: check if `DSAP == 0xAA && SSAP == 0xAA && Control == 0x03 && OUI == 0x000000`.
3. Extract original 16-bit EtherType:
   - If `EtherType == 0x888E` (**EAPOL**): **Intercept immediately**. Route frame to in-kernel WPA2 4-way handshake engine. Do NOT forward to BSD stack.
   - For all other protocols (e.g., `0x0800` IPv4, `0x0806` ARP, `0x86DD` IPv6):
     - Allocate an `mbuf_t` via `allocatePacket(ethernet_len)`.
     - Synthesize standard Ethernet II frame: Destination MAC + Source MAC + EtherType + Decrypted Payload.
     - Deliver to macOS via `fEthernetInterface->inputPacket(m, ethernet_len, 0)`.

---

## 9. User-Space Control Plane & Safe Staging Plan

### 9.1 `IOUserClient` IPC Interface Specification (`RTL8723BEUserClient`)

To enable user-space administration without exposing unsafe raw memory interfaces, the driver exports a specialized `IOUserClient` subclass (`RTL8723BEUserClient`) exposing seven typed, bounds-checked RPC methods:

```cpp
enum RTL8723BEMethodIndex {
    kMethodGetInfo        = 0,  // Retrieve MAC address, firmware version, link state
    kMethodStartScan      = 1,  // Trigger channel 1-13 active/passive scan
    kMethodGetScanResults = 2,  // Query discovered BSSID array
    kMethodConnect        = 3,  // Initiate association to SSID with WPA2 passphrase
    kMethodDisconnect     = 4,  // Teardown association and send Deauth
    kMethodGetStats       = 5,  // Query current RSSI, noise floor, TX/RX counters
    kMethodSetAntenna     = 6,  // Switch RF antenna path (1 = Main, 2 = Aux)
    kNumberOfMethods
};
```

#### External Method Dispatch Table:
```cpp
const IOExternalMethodDispatch RTL8723BEUserClient::sMethods[kNumberOfMethods] = {
    { // kMethodGetInfo
        (IOExternalMethodAction)&RTL8723BEUserClient::sGetInfo,
        0, 0,                                          // 0 scalar in, 0 struct in
        0, sizeof(RTL8723BEDriverInfo)                 // 0 scalar out, struct out
    },
    { // kMethodStartScan
        (IOExternalMethodAction)&RTL8723BEUserClient::sStartScan,
        0, sizeof(RTL8723BEScanRequest),
        0, 0
    },
    { // kMethodGetScanResults
        (IOExternalMethodAction)&RTL8723BEUserClient::sGetScanResults,
        0, 0,
        0, sizeof(RTL8723BEScanResults)
    },
    { // kMethodConnect
        (IOExternalMethodAction)&RTL8723BEUserClient::sConnect,
        0, sizeof(RTL8723BEConnectParams),
        0, 0
    },
    { // kMethodDisconnect
        (IOExternalMethodAction)&RTL8723BEUserClient::sDisconnect,
        0, 0,
        0, 0
    },
    { // kMethodGetStats
        (IOExternalMethodAction)&RTL8723BEUserClient::sGetStats,
        0, 0,
        0, sizeof(RTL8723BEStatistics)
    },
    { // kMethodSetAntenna
        (IOExternalMethodAction)&RTL8723BEUserClient::sSetAntenna,
        1, 0,                                          // 1 scalar in (antenna ID: 1 or 2)
        0, 0
    }
};
```

### 9.2 Command-Line Interface (`rtl8723be_cli`) Architecture

The companion command-line utility `rtl8723be_cli` connects to `RTL8723BEUserClient` via `IOServiceGetMatchingDictionary`, `IOServiceGetMatchingService`, and `IOServiceOpen`:

```sh
# Trigger Wi-Fi scan on 2.4 GHz channels 1-13
rtl8723be_cli scan

# Display discovered access points
rtl8723be_cli list
# Output:
# BSSID              CH  RSSI  SECURITY   SSID
# e0:28:6d:42:1a:80   6   -54  WPA2-PSK   MyHomeNetwork
# 14:cc:20:9b:3e:10  11   -72  WPA2-PSK   OfficeGuest

# Connect to target network
rtl8723be_cli connect "MyHomeNetwork" "SecretPassphrase123"

# Query active link statistics
rtl8723be_cli status
# Output:
# State:       CONNECTED
# SSID:        MyHomeNetwork
# BSSID:       e0:28:6d:42:1a:80
# Channel:     6 (2437 MHz)
# RSSI:        -52 dBm
# Antenna:     2 (Auxiliary)
# IP Address:  192.168.1.145 (DHCP assigned via en1)

# Manually switch antenna path (for troubleshooting single-antenna laptops)
rtl8723be_cli ant 2
```

### 9.3 Diagnostic Verification & Symbol Resolution Pipeline

Before deploying the compiled kernel extension to EFI, the binary undergoes strict pre-flight validation:
1. **Symbol Resolution Check**:
   ```sh
   kmutil libraries -p /path/to/RTL8723BE.kext
   ```
   Validates that 100% of imported symbols resolve directly against `/System/Library/KernelCollections/BootKernelExtensions.kc`.
2. **Kextutil Sanity Verification**:
   ```sh
   kextutil -n -t /path/to/RTL8723BE.kext
   ```
   Verifies bundle structure, `Info.plist` syntax, and declared KPI library versions.
3. **Automated User-Space Test Suite**:
   Executes the 4-Tier mock hardware test runner (`tests/test_runner`) validating:
   - Tier 1: Pure algorithmic units (eFuse PG parser, PBKDF2, PRF-512, AES-CCMP).
   - Tier 2: Mock MMIO register state machine, power sequence, firmware upload, DMA rings.
   - Tier 3: 802.11 scan, Auth/Assoc, WPA2 4-way handshake, and LLC/SNAP packet loopback.
   - Tier 4: Edge-case fuzzing, replay rejection, and memory boundary tests.

### 9.4 OpenCore EFI Safe Staging & Host Bring-Up Protocol

To transition safely to live hardware without risk of host panics:

```
[Phase 1: Automated Regression]
  └── 100% pass on Tier 1–4 Mock Hardware Test Suite

[Phase 2: Pre-Flight Symbol Validation]
  └── kmutil libraries -p confirms zero missing dependencies against running kernel

[Phase 3: OpenCore EFI Configuration on /Volumes/HTOSH/EFI/OC]
  ├── Step 3.1: Mount EFI partition (FAT32 labeled HTOSH at /Volumes/HTOSH).
  ├── Step 3.2: Edit /Volumes/HTOSH/EFI/OC/config.plist:
  │             Under ACPI -> Add:
  │             Locate entry: SSDT-Disable_Network_RP06.aml
  │             Change: <key>Enabled</key><true/> -> <false/>
  ├── Step 3.3: Copy compiled RTL8723BE.kext to /Volumes/HTOSH/EFI/OC/Kexts/.
  └── Step 3.4: Add RTL8723BE.kext entry to config.plist under Kernel -> Add:
                - BundlePath: "RTL8723BE.kext"
                - ExecutablePath: "Contents/MacOS/RTL8723BE"
                - PlistPath: "Contents/Info.plist"
                - Enabled: <true/>

[Phase 4: Live Host Bring-Up & Verification]
  ├── Step 4.1: Reboot host into macOS 26.6.2.
  ├── Step 4.2: Interrogate IORegistry:
  │             ioreg -r -n RTL8723BE
  │             Confirm attachment to IOPCIDevice (RP06@1c0005/PXSX@0).
  ├── Step 4.3: Verify kernel logs:
  │             log show --predicate 'sender == "RTL8723BE"' --last boot
  │             Confirm eFuse MAC read, firmware v36 loaded, and link ready.
  ├── Step 4.4: Verify BSD network interface creation:
  │             ifconfig en1 (or en2)
  └── Step 4.5: Run rtl8723be_cli scan; verify live access point discovery and DHCP bring-up.
```

---

## 10. Traceability & Verification Matrix

| Requirement ID | Specification Item | Architecture Component | Implementation File(s) | Verification Method |
| :--- | :--- | :--- | :--- | :--- |
| **R1.1** | Architecture Selection & Comparison | `IOEthernetController` + `IOUserClient` | `docs/DESIGN.md` § 2 | Survey review & KPI compatibility matrix |
| **R1.2** | macOS 26.6.2 KPI Mappings | `IONetworkingFamily` 3.4, `IOPCIFamily` 2.9 | `driver/Info.plist`, `Makefile` | `kmutil libraries -p` validation |
| **R2.1** | PCIe Attachment & MMIO Mapping | `IOPCIDevice`, 16KB BAR2 MMIO | `driver/RTL8723BE.cpp`, `rtl8723be_reg.h` | Tier 2 Mock MMIO test & `ioreg` inspection |
| **R2.2** | MSI Interrupt Configuration | `IOFilterInterruptEventSource` | `driver/RTL8723BE.cpp` | Tier 2 Interrupt masking & W1C test |
| **R2.3** | Power-On State Machine (`pwrseq`) | `CARDDIS` -> `CARDEMU` -> `ACT` | `driver/hw/rtl8723be_pwrseq.cpp` | Tier 2 `Test_Power_Sequence` |
| **R2.4** | 8051 MCU Firmware Loader | Page download to 0x1000..0x1FFF | `driver/hw/rtl8723be_fw.cpp` | Tier 2 `Test_8051_Firmware_Upload` |
| **R2.5** | eFuse & Calibration Decoding | OTP PG Packet parser (0xD0 MAC, etc.)| `driver/hw/rtl8723be_efuse.cpp` | Tier 1 `Test_eFuse_Decoder` |
| **R2.6** | Baseband & RF Bulk Table Init | 5 register tables loading | `driver/hw/rtl8723be_phy.cpp` | Tier 2 Table write verification |
| **R2.7** | 3-Wire LSSI RF & Channel Tuning | RF 0x18, Channels 1–13 | `driver/hw/rtl8723be_phy.cpp` | Tier 2 `Test_Channel_Tuning` |
| **R2.8** | Antenna Diversity (HP Single Wire) | RF Switch 0x092C (Main=1, Aux=2) | `driver/hw/rtl8723be_phy.cpp` | Tier 2 `Test_Antenna_Switch` |
| **R2.9** | TX/RX DMA Ring Management | 40B TX / 32B RX descs, EOR, Doorbell| `driver/hw/rtl8723be_dma.cpp` | Tier 2 `Test_TX_DMA_Rings`, `Test_RX_DMA_Rings` |
| **R2.10**| 802.11 Active/Passive Scanner | Channel dwell, Beacon/Probe cache | `driver/net/rtl8723be_scan.cpp` | Tier 3 `Test_Scan_Lifecycle` |
| **R2.11**| Auth / Assoc State Machine | Open System Auth, Assoc Req/Resp | `driver/net/rtl8723be_assoc.cpp` | Tier 3 `Test_Auth_Assoc_Sequence` |
| **R2.12**| WPA2-PSK 4-Way Handshake | EAPOL-Key M1..M4, PMK/PTK/GTK | `driver/net/rtl8723be_wpa2.cpp` | Tier 1/3 `Test_Crypto_PRF512`, `Test_WPA2_4Way` |
| **R2.13**| CCMP (AES-128 CCM) Engine | CCMP Header, PN Replay, MIC | `driver/net/rtl8723be_crypto.cpp`| Tier 1/3 `Test_Crypto_AES_CCMP` |
| **R2.14**| Ethernet <-> 802.11 LLC/SNAP Bridge | RFC 1042 LLC/SNAP, EAPOL filter | `driver/net/rtl8723be_bridge.cpp`| Tier 1/3 `Test_Ethernet_Translation`, loopback |
| **R3.1** | `IOUserClient` Control IPC | Methods 0..6 (Scan, Connect, etc.) | `driver/RTL8723BEUserClient.cpp` | Tier 3 UserClient dispatch test |
| **R3.2** | Companion CLI Utility | `rtl8723be_cli` (`scan`, `connect`) | `client/rtl8723be_cli.cpp` | CLI integration test |
| **R3.3** | Automated Mock Test Suite | Tiers 1–4 Unit, Bus, State, Fuzz | `tests/test_runner.cpp` | 100% pass on `make -C tests run` |
| **R3.4** | OpenCore EFI Staging & SSDT Disable | Disable `SSDT-Disable_Network_RP06` | `/Volumes/HTOSH/EFI/OC/config.plist` | Post-boot `ioreg` & live attach verification |

---
*End of Engineering Design Document EDD-RTL8723BE-DARWIN-01.*
