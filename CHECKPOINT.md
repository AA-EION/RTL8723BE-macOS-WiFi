# RTL8723BE macOS Wi-Fi Driver — Project Checkpoint & Resume Guide

> **Working Directory**: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi`
> **Target Hardware**: Realtek `RTL8723BE` PCIe 802.11b/g/n (`pci10ec,b723`, subsystem `103c:804c`, PCI `2:0:0` at `_SB/PCI0@0/RP06@1c0005/PXSX@0`, BAR2 MMIO `0xf1100000`)
> **Target OS**: macOS 26.6.2 (`25G83`, Darwin `25.6.0`, `x86_64`)

---

## Completed Milestones (Committed to Local Git Repository)

### 1. Milestone 1 — Online Survey & Engineering Architecture (`COMPLETED & AUDITED`)
- **Authoritative Design Document**: [docs/DESIGN.md](file:///Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md) (76 KB)
- **Survey & Specs**:
  - [explorer_survey_1/analysis.md](file:///Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_1/analysis.md) — macOS Wi-Fi Driver Architecture Survey (`IOEthernetController` + user-space `IOUserClient` CLI/daemon confirmed as the only viable native kernel architecture on macOS 26.6.2).
  - [explorer_survey_2/spec.md](file:///Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_2/spec.md) — Exact Linux `rtlwifi/rtl8723be` & OpenBSD `rtwn` hardware registers, `pwrseq` power-on table, eFuse decoding (`0x0030`/`0x0034`), 8051 MCU `rtl8723befw.bin` firmware upload & checksum protocol (`REG_MCUFWDL 0x0080`), TX/RX 40-byte/24-byte DMA descriptors, and MAC/BB/RF PHY tables.
  - [explorer_survey_3/analysis.md](file:///Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_3/analysis.md) — Host PCI topology (`pci10ec,b723`), OpenCore EFI kext staging, and `kmutil` KPI dependency resolution (`com.apple.iokit.IONetworkingFamily`, `com.apple.iokit.IOPCIFamily`, `com.apple.kpi.*`).

### 2. Milestone 2 — Hardware Emulation & Verification Test Suite (`COMPLETED`)
- **Test Runner & Makefile**: [tests/Makefile](file:///Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/Makefile), [tests/test_runner](file:///Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/test_runner)
  - Run tests at any time with:
    ```bash
    cd /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests && make test
    ```
- **Simulated Subsystems (`tests/mock/`)**:
  - `mock_pci_mmio.cpp` — Full 16 KB BAR2 MMIO register space + indirect eFuse + 8051 MCU SRAM (`0x1000–0x7FFF`) emulation.
  - `mock_dma.cpp` — 8-ring TX (`BK`, `BE`, `VI`, `VO`, `BCN`, `MGNT`, `HIGH`, `CMD`) and RX `IOBufferMemoryDescriptor` DMA ring simulator.
  - `mock_crypto.cpp` — IEEE 802.11i WPA2-PSK PBKDF2-SHA1, 4-Way EAPOL Handshake PTK/GTK derivation, and AES-CCMP 128-bit encryption/decryption.
  - `mock_packet_injector.cpp` & `simulated_device.cpp` — Full end-to-end virtual `pci10ec,b723` device state machine.

---

## Current / Next Milestones (To Resume If Interrupted)

### 3. Milestone 3–5 — Kernel Extension (`RTL8723BEWiFi.kext`) & Firmware (`rtl8723befw.bin`)
- Source code directory: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/src/`
- Kext output bundle: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/build/RTL8723BEWiFi.kext`
  - `IOEthernetController` subclass (`RTL8723BEController`) matching `IOPCIMatch = 0xB72310EC`
  - `IOUserClient` subclass (`RTL8723BEUserClient`) for scanning, SSID association, and WPA2-PSK configuration
  - Hardware initialization (`rtl8723be_hw.cpp`), `rtl8723befw.bin` loader (`rtl8723be_fw.cpp`), DMA rings (`rtl8723be_dma.cpp`), and 802.11/WPA2 engine (`rtl8723be_80211.cpp`)

### 4. Milestone 6 — User-Space Control CLI (`rtl8723be_cli`) & OpenCore Staging
- CLI tool: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tools/rtl8723be_cli`
- Staging & verification script: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/scripts/verify_and_stage.sh`

---

## How to Resume in a New Session

If your token quota runs out, start a new chat in `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi` and run:
> *"Read `CHECKPOINT.md`, `PROJECT.md`, and `docs/DESIGN.md` in `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi`, check `git log -n 5` and `.agents/teamwork/orchestrator_1/progress.md`, and continue from the latest checkpoint to complete `RTL8723BEWiFi.kext`, `rtl8723be_cli`, and OpenCore staging."*
