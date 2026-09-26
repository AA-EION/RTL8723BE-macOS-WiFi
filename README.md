# Realtek RTL8723BE PCIe (`pci10ec,b723`) macOS Wi-Fi Driver & Wireless Utility

Complete native macOS Kernel Extension (`RTL8723BEWiFi.kext`), embedded 8051 MCU firmware (`rtl8723befw.bin`), user-space CLI (`rtl8723be_cli`), and native macOS GUI application (`RTL8723BE Wireless Utility.app` inside `RTL8723BE_WiFi_Installer.dmg`) for the **Realtek RTL8723BE PCIe 802.11b/g/n Wireless LAN Adapter** (`vendor-id: 0x10ec`, `device-id: 0xb723`, HP subsystem `0x103c:0x804c`) on macOS (including **macOS 26.6.2 / Darwin 25.6.0**).

---

## Features

- **`RTL8723BEWiFi.kext` (`IOEthernetController` + `IOUserClient`)**:
  - Matches `IOPCIMatch = 0xB72310EC` (`pci10ec,b723`) and attaches directly to the PCIe BAR2 16 KB MMIO space.
  - Full RTL8723BE hardware bring-up ported from Linux `drivers/net/wireless/realtek/rtlwifi/rtl8723be` and OpenBSD `rtwn`:
    - Hardware power sequence (`pwrseq` / `initLLT`)
    - Indirect eFuse (`0x0030`/`0x0034`) MAC address and RF calibration decoding
    - Embedded 28,224-byte `rtl8723befw.bin` 8051 MCU firmware upload and checksum/boot handshake (`REG_MCUFWDL 0x0080`)
    - Baseband/AGC/RF table programming and 2.4 GHz Channels 1–13 3-wire LSSI synthesizer tuning
    - **HP Single-Antenna Diversity Fix (`103c:804c`)**: Hardware RF switch between `Antenna #1 (Main)` and `Antenna #2 (Aux)`
    - 8-ring TX DMA (`TxDesc40`) and RX DMA (`RxDesc32`) ring management via `IOBufferMemoryDescriptor`
    - Complete IEEE 802.11 Beacon/Probe scanner, Open System Auth/Assoc state machine, and IEEE 802.11i WPA2-PSK (PBKDF2-SHA1, EAPOL 4-Way Handshake with KRACK replay mitigation, and RFC 1042 LLC/SNAP + AES-CCMP 128-bit encryption/decryption).
- **Native macOS GUI App (`RTL8723BE Wireless Utility.app` inside `dist/RTL8723BE_WiFi_Installer.dmg`)**:
  - macOS Menu Bar Wi-Fi status icon + Main Dashboard Window.
  - One-click **2.4 GHz Network Scanner** and **WPA2-PSK Password Prompt**.
  - Live hardware telemetry (MAC address, BSSID, channel, RSSI dBm, TX/RX packet counters) and **Antenna #1 (Main) / Antenna #2 (Aux - HP)** toggle.
  - Built-in **One-Click OpenCore EFI Installer** (`EFI/OC/Kexts` + `config.plist`) and **One-Click Administrator Kext Loader** (`kmutil load`).
- **57-Test Hardware Emulation & Protocol Verification Suite (`tests/test_runner`)**:
  - 100% pass rate (`57/57` tests) across virtual PCIe MMIO/eFuse, 8051 MCU firmware loader, TX/RX DMA descriptor rings, 802.11 beacon parser, WPA2 4-way handshake, and AES-CCMP crypto.

---

## Quick Installation (DMG GUI Installer)

1. Download or open **`dist/RTL8723BE_WiFi_Installer.dmg`**.
2. Drag **`RTL8723BE Wireless Utility.app`** to the **`Applications`** folder.
3. Launch **`RTL8723BE Wireless Utility.app`** from `/Applications` (or the macOS Menu Bar Wi-Fi icon):
   - Click **"Install Kext to OpenCore EFI"** to automatically copy `RTL8723BEWiFi.kext` into `EFI/OC/Kexts/` and register it in `EFI/OC/config.plist`, **or**
   - Click **"Load Kext Now (kmutil load)"** to stage and load the kext immediately with administrator privileges.
4. On HP laptops (`103c:804c`), keep the top-right antenna toggle on **`Ant #2 (Aux - HP)`**.
5. Click **"Scan Now"** to sweep 2.4 GHz channels 1–13 and click **"Connect"** next to your Wi-Fi network.

---

## Building from Source

```bash
# Build RTL8723BEWiFi.kext and tools/rtl8723be_cli
make clean && make all

# Run the 57-test hardware register / DMA / 802.11 / WPA2-CCMP verification suite
make test

# Build the native macOS GUI application (.app) and distributable Disk Image (.dmg)
./scripts/build_dmg.sh
```

---

## Repository Structure

- [`src/`](src/) — Kernel extension C++ source (`RTL8723BE.cpp`, `RTL8723BEUserClient.cpp`, `RTL8723BE_crypto.cpp`, `RTL8723BE_firmware.cpp`, `RTL8723BE_tables.cpp`, `Info.plist`)
- [`gui/`](gui/) — Native macOS Swift/AppKit/SwiftUI GUI application (`RTL8723BEApp.swift`, `RTL8723BEBridge.h`, `Info.plist`)
- [`tools/`](tools/) — Command-line utility (`rtl8723be_cli.cpp`)
- [`scripts/`](scripts/) — `build_dmg.sh` (DMG packager) and `stage_opencore.sh` (OpenCore EFI & `kmutil` verifier)
- [`tests/`](tests/) — 57-test mock PCIe MMIO/DMA/802.11/WPA2 hardware verification suite
- [`docs/DESIGN.md`](docs/DESIGN.md) — 76 KB Engineering Design & Hardware Register Specification
