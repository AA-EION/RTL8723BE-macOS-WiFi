# Original User Request

## Initial Request — 2026-09-26T16:14:40Z

Enable and make fully functional the internal Realtek RTL8723BE PCIe Wireless Network Adapter (`pci10ec,b723`, subsystem `103c:804c`, PCI path `_SB/PCI0@0/RP06@1c0005/PXSX@0` at PCI `2:0:0`) on this Hackintosh running macOS 26.6.2 (`25G83`, x86_64). First investigate all existing community macOS drivers/ports for Realtek PCIe `rtl8723be` and related chipsets; if no ready-made solution works on macOS 26.6.2, evaluate and choose the best driver architecture (`IOEthernetController` kext + user-space client/HeliPort/daemon, `DriverKit` PCIe extension, or `IO80211Family`/SkyWalk shim) and port the open-source Linux/BSD `rtl8723be` driver and `rtl8723befw.bin` firmware into a complete, working macOS Wi-Fi driver solution.

Working directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi
Integrity mode: development

## Hardware & Environment Reference

- **Target Device**: Realtek `RTL8723BE` PCIe 802.11b/g/n Wireless LAN Adapter
  - `vendor-id`: `0x10ec` (`<ec100000>`), `device-id`: `0xb723` (`<23b70000>`)
  - `subsystem-vendor-id`: `0x103c`, `subsystem-id`: `0x804c`
  - `IOName`: `pci10ec,b723`, `pcidebug`: `2:0:0`, `acpi-path`: `IOACPIPlane:/_SB/PCI0@0/RP06@1c0005/PXSX@0`
  - Memory resources: BAR0 I/O port (`256` bytes) + BAR2 64-bit MMIO (`16384` bytes at `0xf1100000`), MSI (`IOPCIMessagedInterruptController`) & IO-APIC interrupt support.
- **OS & Kernel Environment**: macOS 26.6.2 (Build `25G83`), `x86_64`, OpenCore with `Lilu` (1.7.3), `AMFIPass` (1.4.1), `RestrictEvents` (1.1.7), `VirtualSMC` (1.3.8), `RealtekRTL8111` (2.4.2).

## Requirements

### R1. Online Driver Survey & Architecture Selection
Search and evaluate existing open-source macOS drivers, experimental kexts, and Linux/BSD driver implementations (`torvalds/linux` `drivers/net/wireless/realtek/rtlwifi/rtl8723be` + `rtl_pci` + `btcoexist`, `rtw88`, OpenBSD `rtwn`, and `itlwm`/`HeliPort` `IOEthernetController` architectures). Select the most reliable architecture for macOS 26.6.2 (`25G83`) and produce an engineering design document explaining the chosen architecture and hardware bring-up plan.

### R2. Full RTL8723BE Hardware Driver, Firmware & 802.11 Stack Implementation
Implement a complete, non-stubbed macOS driver matching `pci10ec,b723` with embedded or loadable `rtl8723befw.bin` firmware. The implementation must cover:
1. PCIe bus attachment (`IOPCIDevice` / `PCIDriverKit`), MMIO BAR mapping, power management, and interrupt handling (`IOFilterInterruptEventSource` / MSI).
2. Complete RTL8723BE hardware initialization: power-on sequence (`pwrseq`), eFuse/EEPROM MAC address & calibration readout, 8051 MCU firmware upload (`rtl8723befw.bin`) and checksum/boot handshake, MAC/BB/RF (`phy_init`, AGC, TX power, channel tuning for 2.4 GHz Channels 1–13) initialization, and TX/RX DMA descriptor ring allocation and management.
3. Complete 802.11 management and data path: beacon/probe-response parsing for active/passive network scanning, 802.11 authentication/association state machine, WPA2-PSK (RSN 4-way handshake + CCMP/AES encryption/decryption or hardware crypto offload), and Ethernet<->802.11 frame translation connected to a macOS network interface (`IONetworkInterface`).

### R3. Network Control Utility & Safe Verification/Staging Pipeline
Provide a companion control tool/daemon (CLI utility and/or HeliPort-compatible client) capable of triggering Wi-Fi scans, displaying discovered SSIDs/BSSIDs/RSSI, and connecting to WPA2 networks. Because loading an untested PCIe DMA kext can panic the host kernel, verify symbol resolution, KPI compatibility, and hardware state-machine/DMA logic thoroughly via `kmutil` diagnostics and automated hardware-register emulation tests before staging in OpenCore EFI and performing controlled live loading.

## Acceptance Criteria

### Build, KPI & Completeness Verification
- [ ] Zero placeholder stubs (`TODO`, `FIXME`, fake scan lists, or no-op register writes) in the hardware initialization, firmware loader, DMA ring manager, or 802.11/WPA2 stack.
- [ ] Driver compiles cleanly into a valid macOS `.kext` (or `.dext`) bundle targeting macOS 26.6.2 (`x86_64`) with `Info.plist` matching `IOPCIMatch` = `0xB72310EC`.
- [ ] `kmutil print-diagnostics` / `kextutil -n -t` confirms all Kernel Programming Interfaces (KPIs) and symbols resolve cleanly against the running macOS `25G83` kernel with zero missing dependencies.
- [ ] Automated test suite (including a mock PCIe MMIO/DMA hardware harness testing the firmware loader, eFuse parser, TX/RX ring descriptors, 802.11 beacon parser, and WPA2 4-way handshake + CCMP crypto) passes 100% of tests.

### Live Hardware & System Integration Verification
- [ ] Driver is safely staged in OpenCore EFI (`EFI/OC/Kexts` + `config.plist` or root load script) and loaded on the host once pre-load safety checks pass.
- [ ] `ioreg -l` confirms the driver attaches to `pci10ec,b723` (`RP06@1c0005/PXSX@0`), reads the hardware MAC address from eFuse, boots `rtl8723befw.bin`, and registers an active `IONetworkInterface` (`enX`).
- [ ] Running the Wi-Fi scan utility performs a real 2.4 GHz hardware scan on `pci10ec,b723` and outputs discovered access points, with full connection/DHCP bring-up support.
