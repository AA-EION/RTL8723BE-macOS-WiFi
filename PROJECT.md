# Project: Realtek RTL8723BE macOS Wi-Fi Driver

## Architecture

### Global Architecture Overview
The RTL8723BE driver follows the proven high-stability macOS Wi-Fi architecture pioneered by `itlwm`:
- **Kernel Space (`RTL8723BE.kext`)**:
  - Subclasses `IOEthernetController` (from `com.apple.iokit.IONetworkingFamily` 3.4).
  - Direct PCIe bus attachment via `IOPCIDevice` (`com.apple.iokit.IOPCIFamily` 2.9).
  - Maps 16KB BAR2 64-bit MMIO at `0xf1100000`.
  - Registers MSI interrupt source (`IOFilterInterruptEventSource`).
  - Implements complete RTL8723BE hardware state machine: power sequence (`pwrseq`), eFuse reader (MAC address & calibration), 8051 MCU firmware upload (`rtl8723befw.bin`), and TX/RX DMA descriptor rings.
  - Implements an embedded 802.11 management engine: beacon/probe parser, active/passive scanner, 802.11 auth/assoc state machine, and WPA2-PSK (RSN 4-way handshake + CCMP/AES crypto).
  - Bridges Ethernet II frames from macOS networking (`IONetworkInterface`) to 802.11 QoS Data frames with RFC 1042 LLC/SNAP encapsulation.
  - Exposes an `IOUserClient` (`RTL8723BEUserClient`) for user-space control and monitoring.
- **User Space (`rtl8723be_cli` / Companion Daemon)**:
  - Communicates with `RTL8723BEUserClient` via `IOConnectCallMethod`.
  - Commands: `scan`, `list`, `connect <SSID> [password]`, `status`, `disconnect`, `ant <1|2>`.
  - Compatible with community frontends such as `HeliPort`.
- **Test Infrastructure (`tests/`)**:
  - Standalone mock hardware execution harness simulating BAR2 MMIO registers, 8051 MCU download handshake, eFuse OTP readout, DMA descriptor ring queues, and 802.11/EAPOL frame injection.
  - 4-Tier test suite: Category-Partition (Tier 1), Boundary/Corner (Tier 2), Cross-Feature Combinations (Tier 3), Real-World Application Workloads (Tier 4).
  - Adversarial coverage hardening (Tier 5).

### Data & Control Flow
1. **Control Flow**:
   User CLI -> `IOConnectCallMethod` -> `RTL8723BEUserClient` -> `RTL8723BE::triggerScan()` / `connectToNetwork()` -> 802.11 State Machine -> Hardware TX Queue (Beacon/Management) -> MMIO Doorbell.
2. **Outbound Data Flow**:
   macOS TCP/IP Stack -> `IONetworkInterface` -> `RTL8723BE::outputPacket(mbuf)` -> Ethernet II to 802.11 LLC/SNAP Encapsulation -> CCMP Encryption (IV insertion, AES-CCM MIC) -> TX Descriptor (40-byte) -> DMA transfer -> MMIO Doorbell (`REG_PCIE_CTRL_REG`).
3. **Inbound Data Flow**:
   Hardware RX DMA -> RX Descriptor (32-byte) -> Interrupt (`HISR` RX_OK) -> CCMP Decryption & MIC check -> 802.11 LLC/SNAP Decapsulation -> Intercept EAPOL-Key frames (to WPA2 state machine) or standard Ethernet packets -> `fEthernetInterface->inputPacket(mbuf)`.

---

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Online Driver Survey & Prior Art | Comparative evaluation of itlwm, DriverKit, IO80211Family, OpenBSD rtwn, and Linux rtlwifi | M1 | Survey (R1) |
| 2 | Engineering Design Document | Complete architecture specification, hardware bring-up plan, and KPI mappings in `docs/DESIGN.md` | M1 | Survey (R1) |
| 3 | E2E Mock Hardware Test Harness | User-space PCIe MMIO BAR2 simulation, 8051 MCU download handshake, and eFuse simulation | M2 | Survey (R3) |
| 4 | E2E Mock DMA & Packet Injection | TX/RX descriptor ring simulation, OWN bit arbitration, and 802.11 frame injection harness | M2 | Survey (R3) |
| 5 | Tier 1-4 Test Suite Implementation | ≥5 tests per feature, boundary tests, pairwise combinations, and real-world application scenarios | M2 | Survey (R3) |
| 6 | PCIe Device Matching & MMIO Mapping | Match `0xB72310EC` on `RP06@1c0005/PXSX@0`, map BAR2 16KB MMIO, enable bus mastering | M3 | Survey (R2.1) |
| 7 | Power Management & Interrupt Setup | MSI vector allocation / IO-APIC routing, ASPM disable, power-state transitions | M3 | Survey (R2.1) |
| 8 | Power-On State Machine (`pwrseq`) | Transition CARDDIS -> CARDEMU -> ACT, reset MAC, ungate clocks, partition LLT memory | M3 | Survey (R2.2) |
| 9 | 8051 MCU Firmware Loader | Page-wise upload of `rtl8723befw.bin` to MMIO 0x1000..0x1FFF, checksum handshake, MCU reset & ready poll | M3 | Survey (R2.2) |
| 10 | eFuse & Calibration Parser | Read OTP via `REG_EFUSE_CTRL`, extract MAC address (0xD0..0xD5), crystal cap (0xB9), thermal & TX power | M3 | Survey (R2.2) |
| 11 | MAC/BB/RF Bulk Init & Channel Tuning | Load 5 register tables, 3-wire LSSI RF access via 0x0840, 2.4 GHz channel tuning (1–13), antenna selection | M3 | Survey (R2.2) |
| 12 | TX/RX DMA Ring Manager | 40-byte TX descriptors, 32-byte RX descriptors, 256-byte ring alignment, OWN bit handling, doorbell | M3 | Survey (R2.2) |
| 13 | 802.11 Active & Passive Scanner | Beacon & Probe-Response parsing, channel dwell timer, SSID/BSSID/RSSI scan results cache | M4 | Survey (R2.3) |
| 14 | 802.11 Auth & Assoc State Machine | Open System Authentication, Association Request/Response, Capability & Supported Rates IEs | M4 | Survey (R2.3) |
| 15 | WPA2-PSK 4-Way Handshake Engine | EAPOL-Key exchange (Msg 1..4), PMK derivation (PBKDF2 HMAC-SHA1), PTK derivation, GTK install | M4 | Survey (R2.3) |
| 16 | CCMP (AES-128 CCM) Crypto Engine | CCMP encapsulation (8-byte header, 8-byte MIC), AES-128 encryption/decryption, replay check | M4 | Survey (R2.3) |
| 17 | Ethernet <-> 802.11 Frame Translation | RFC 1042 LLC/SNAP header encapsulation/decapsulation, QoS Data headers, Ethernet II bridge | M4 | Survey (R2.3) |
| 18 | `IOEthernetController` Integration | Register `IONetworkInterface`, MTU configuration, link status reporting, `outputPacket` pipeline | M4 | Survey (R2.3) |
| 19 | `IOUserClient` Interface | Custom user-client exposing scalar/struct methods for scan, list, connect, status, and antenna control | M5 | Survey (R3) |
| 20 | Companion Control Daemon & CLI | Standalone `rtl8723be_cli` utility providing user commands (`scan`, `list`, `connect`, `status`, `ant`) | M5 | Survey (R3) |
| 21 | KPI Compatibility & Diagnostic Validation | `kmutil print-diagnostics` and `kextutil` symbol verification against macOS 26.6.2 kernel | M5 | Survey (R3) |
| 22 | Automated Test Suite 100% Pass | All mock hardware and protocol test tiers pass with zero failures | M6 | Survey (R3) |
| 23 | OpenCore Staging & Live Hardware Verification | Disable `SSDT-Disable_Network_RP06.aml` in `/Volumes/HTOSH/EFI/OC/config.plist`, stage kext, verify live attach | M6 | Survey (R3) |

---

## Milestones

| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Architecture Specification & Design Doc (R1) | Comparative survey evaluation and comprehensive engineering design document `docs/DESIGN.md` | none | IN_PROGRESS |
| M2 | E2E Test Infrastructure & Mock Hardware Harness | Standalone mock MMIO/DMA test harness, packet injector, and Tier 1-4 test suite; publish `TEST_READY.md` | none | IN_PROGRESS |
| M3 | RTL8723BE Driver Core, Firmware & DMA Engine (R2.1, R2.2) | PCIe matching, MMIO mapping, pwrseq, firmware loader, eFuse reader, MAC/BB/RF init, and TX/RX DMA rings | M1 | PLANNED |
| M4 | 802.11 Protocol Engine, WPA2-PSK & Ethernet Bridge (R2.3) | Scanner, Auth/Assoc state machine, WPA2 4-way handshake, CCMP crypto, LLC/SNAP encapsulation, `IOEthernetController` | M3 | PLANNED |
| M5 | Companion Control CLI, UserClient & KPI Diagnostics (R3) | `RTL8723BEUserClient`, `rtl8723be_cli` tool, `kmutil print-diagnostics` validation on macOS 26.6.2 | M4 | PLANNED |
| M6 | Final Verification, OpenCore Staging & Live Bring-up | 100% E2E test suite pass, Tier 5 adversarial hardening, OpenCore staging on `/Volumes/HTOSH`, live hardware bring-up | M2, M5 | PLANNED |

---

## Interface Contracts

### Driver Core (`RTL8723BE`) <-> `IOEthernetController`
- `bool init(OSDictionary * properties);`
- `bool start(IOService * provider);`
- `void stop(IOService * provider);`
- `void free();`
- `IOReturn getHardwareAddress(IOEthernetAddress * addrP);`
- `UInt32 outputPacket(mbuf_t m, void * param);`
- `IOReturn enable(IONetworkInterface * netif);`
- `IOReturn disable(IONetworkInterface * netif);`
- `bool setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium);`

### Hardware Layer (`RTL8723BE_HW`) <-> 802.11 Engine (`RTL8723BE_80211`)
- `IOReturn hw_init();`
- `IOReturn hw_power_on();`
- `IOReturn hw_download_firmware(const uint8_t * fw_buf, size_t fw_len);`
- `IOReturn hw_read_efuse(uint8_t * mac_out, rtl8723be_calib_t * calib_out);`
- `IOReturn hw_set_channel(uint8_t channel);`
- `IOReturn hw_set_antenna(uint8_t ant_sel);` // 1 = Main, 2 = Aux
- `IOReturn hw_transmit_mgmt(const uint8_t * frame, size_t len);`
- `IOReturn hw_transmit_data(const uint8_t * frame, size_t len, uint8_t queue);`
- `void hw_rx_packet_callback(const uint8_t * frame, size_t len, int8_t rssi);`

### 802.11 Engine <-> `IOUserClient` (`RTL8723BEUserClient`)
- `kMethodScan`: triggers channel 1–13 scan sweep.
- `kMethodGetScanResults`: copies cached BSSID/SSID/RSSI/Security records to user buffer.
- `kMethodConnect`: accepts `rtl_connect_params_t` { ssid, ssid_len, bssid, psk_passphrase }.
- `kMethodGetStatus`: returns current state (DISCONNECTED, SCANNING, AUTHENTICATING, ASSOCIATING, 4WAY_HANDSHAKE, CONNECTED).
- `kMethodSetAntenna`: selects antenna 1 (Main) or 2 (Aux).

---

## Code Layout
```
/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/
├── docs/
│   └── DESIGN.md                               # Architecture & Engineering Design Document (M1)
├── driver/
│   ├── Info.plist                              # Kext bundle metadata (IOPCIMatch = 0xB72310EC)
│   ├── Makefile                                # Kernel extension build script
│   ├── RTL8723BE.hpp                           # Main IOEthernetController driver class
│   ├── RTL8723BE.cpp
│   ├── RTL8723BEUserClient.hpp                 # User-space IPC client class
│   ├── RTL8723BEUserClient.cpp
│   ├── hw/
│   │   ├── rtl8723be_reg.h                     # Register offsets, bitmasks, MMIO helpers
│   │   ├── rtl8723be_pwrseq.hpp                # Power-on state machine
│   │   ├── rtl8723be_pwrseq.cpp
│   │   ├── rtl8723be_fw.hpp                    # Firmware loader & embedded firmware array
│   │   ├── rtl8723be_fw.cpp
│   │   ├── rtl8723be_efuse.hpp                 # eFuse parser & calibration data
│   │   ├── rtl8723be_efuse.cpp
│   │   ├── rtl8723be_phy.hpp                   # MAC/BB/RF initialization & channel tuning
│   │   ├── rtl8723be_phy.cpp
│   │   ├── rtl8723be_dma.hpp                   # TX/RX DMA descriptor rings & doorbell
│   │   └── rtl8723be_dma.cpp
│   └── net/
│       ├── ieee80211.h                         # 802.11 frame structures & IEs
│       ├── rtl8723be_scan.hpp                  # Channel scanner & AP cache
│       ├── rtl8723be_scan.cpp
│       ├── rtl8723be_assoc.hpp                 # Auth / Assoc state machine
│       ├── rtl8723be_assoc.cpp
│       ├── rtl8723be_wpa2.hpp                  # WPA2-PSK 4-way handshake engine
│       ├── rtl8723be_wpa2.cpp
│       ├── rtl8723be_crypto.hpp                # CCMP / AES-128 CCM & MIC
│       ├── rtl8723be_crypto.cpp
│       ├── rtl8723be_bridge.hpp                # Ethernet <-> 802.11 LLC/SNAP bridge
│       └── rtl8723be_bridge.cpp
├── client/
│   ├── Makefile                                # CLI build script
│   ├── rtl8723be_cli.cpp                       # Network control CLI tool
│   └── user_client_proto.h                     # Shared UserClient IPC protocol definitions
└── tests/
    ├── Makefile                                # Test harness build script
    ├── mock/
    │   ├── mock_pci_mmio.hpp                   # Mock BAR2 MMIO register simulation
    │   ├── mock_pci_mmio.cpp
    │   ├── mock_dma.hpp                        # Mock DMA memory & descriptor ring simulation
    │   ├── mock_dma.cpp
    │   └── mock_packet_injector.hpp            # 802.11 / EAPOL frame injector
    ├── tier1_features/                         # Tier 1 tests (5+ tests per feature)
    ├── tier2_boundaries/                       # Tier 2 tests (boundary & edge cases)
    ├── tier3_pairwise/                         # Tier 3 tests (cross-feature interactions)
    ├── tier4_workloads/                        # Tier 4 tests (real-world application scenarios)
    └── test_runner.cpp                         # Main automated test runner executable
```
