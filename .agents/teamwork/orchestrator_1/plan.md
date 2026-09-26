# Project Plan: Realtek RTL8723BE macOS Wi-Fi Driver

## Objective
Design, implement, verify, and safely stage a production-grade macOS Wi-Fi driver for the Realtek RTL8723BE PCIe adapter (`pci10ec,b723`, subsystem `103c:804c`) running on macOS 26.6.2 (`25G83`, x86_64), meeting all requirements R1, R2, and R3 in `ORIGINAL_REQUEST.md`.

## Pattern: Project Orchestration (Dual Track)
- Track 1: Implementation Track (Survey -> Core Driver -> Firmware & Hardware Init -> 802.11 Stack & Crypto -> User Daemon & Staging -> Full E2E & Live Pass)
- Track 2: E2E Testing Track (Requirement-driven test harness, mock MMIO/DMA PCIe test bench, 802.11 frame and WPA2 handshake test cases)

## Step-by-Step Milestones

### Phase 0: Survey & Scope Mapping (Current)
- Dispatch 3 Explorers:
  - Explorer 1: Survey existing community macOS Wi-Fi drivers/kexts (itlwm, AppleAirPortOpenBSD, Realtek kexts), DriverKit vs IOEthernetController vs IO80211Family shims on macOS 26.6.2 (25G83).
  - Explorer 2: Technical analysis of Linux `rtlwifi/rtl8723be` + `rtl_pci` and OpenBSD `rtwn`, register maps, eFuse structures, firmware protocol for `rtl8723befw.bin`, RF/BB parameters.
  - Explorer 3: Target host environment, OpenCore setup, PCI topology (`2:0:0`, `_SB/PCI0@0/RP06@1c0005/PXSX@0`), MMIO BAR2 (`0xf1100000`), MSI/interrupts, and macOS kernel build/KPI requirements.
- Merge findings into `PROJECT.md` Architecture and Feature Inventory.

### Milestone 1: Engineering Design Document & Architecture Selection (R1)
- Write comprehensive architecture design document (`docs/DESIGN.md`).
- Select IOEthernetController + user-space daemon / HeliPort interface architecture, matching proven `itlwm` architecture while implementing native RTL8723BE hardware driver.

### Milestone 2: E2E Test Infrastructure & Mock Hardware Test Bench
- Setup test runner, mock PCIe MMIO registers, mock DMA ring simulation, packet injection harness.
- Implement Tier 1-4 tests (Category-Partition, BVA, Pairwise, Real-World scenarios) and publish `TEST_READY.md`.

### Milestone 3: Hardware Initialization, Firmware Loader & DMA Engine (R2.1, R2.2)
- PCIe device matching (`0xB72310EC`), BAR2 MMIO mapping, interrupt registration.
- Power sequence (`pwrseq`), eFuse reading (MAC address & calibration data), MCU firmware upload handshake for `rtl8723befw.bin`.
- TX/RX DMA descriptor rings, buffer queues, interrupt handler (TX/RX completion).

### Milestone 4: 802.11 Protocol Engine, Scanner, State Machine & WPA2-PSK Crypto (R2.3)
- 802.11 frame generation and parsing (Beacon, Probe Request/Response, Auth, Assoc).
- Active and passive channel scanning on 2.4 GHz channels 1–13.
- Authentication/Association state machine.
- WPA2-PSK 4-way handshake, RSN IE processing, CCMP (AES-128) crypto encryption/decryption, MIC verification.
- `IONetworkInterface` Ethernet packet translation (802.3 <-> 802.11).

### Milestone 5: Companion Control Daemon / Utility & Safe Verification Pipeline (R3)
- Command-line utility / daemon for scanning, listing APs, status reporting, and connecting to networks.
- Verification via `kmutil print-diagnostics` / `kextutil` against macOS 26.6.2 KPIs.
- 100% pass on automated mock test suite.

### Milestone 6: OpenCore Safe Staging & Live Hardware Bring-up
- Stage driver into OpenCore EFI (`EFI/OC/Kexts/`) and configure loader safely.
- Live verification: attach to hardware at `2:0:0`, read MAC, upload firmware, register network interface `enX`, perform live scan.
