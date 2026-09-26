# Handoff Report: Milestone M1 — Engineering Design Document

**Author**: Worker M1 (`worker_m1`)  
**Date**: 2026-09-26T16:38:50Z  
**Deliverable**: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md`  
**Handoff Type**: Hard (Task Complete)

---

## 1. Observation

1. **Source Directives & Specifications**:
   - `ORIGINAL_REQUEST.md` (lines 21–23): Defined Requirement R1: "Search and evaluate existing open-source macOS drivers, experimental kexts, and Linux/BSD driver implementations ... Select the most reliable architecture for macOS 26.6.2 (25G83) and produce an engineering design document explaining the chosen architecture and hardware bring-up plan."
   - `PROJECT.md` (lines 38–39, 68): Established Milestone M1 ("Architecture Specification & Design Doc (R1)") producing `docs/DESIGN.md`.
   - Dispatch instructions required covering 9 distinct sections: Executive Summary & Problem Formulation, Architecture Selection & Comparative Evaluation, RTL8723BE Hardware Architecture & Registers, 8051 MCU Firmware Handshake Protocol, eFuse & Calibration Mapping, TX/RX DMA Descriptor Rings & Memory Architecture, Baseband & RF Tuning & Antenna Diversity, 802.11 Protocol & Data Path, and User-Space Control & Safe Staging Plan.
2. **Survey Reports Interrogated**:
   - `explorer_survey_1/analysis.md`: Detailed evaluation of `itlwm` vs `AirportItlwm`, failure modes of `IO80211Family` and `DriverKit` on modern macOS (macOS 14+ / 26), and data path encapsulation models.
   - `explorer_survey_2/spec.md`: Exhaustive hardware specification containing MMIO register map, power-on sequence (`pwrseq`), 8051 firmware header and 4KB page download protocol, eFuse PG packet decoding, 40-byte TX / 32-byte RX DMA descriptor layouts, 3-wire LSSI RF access (`0x0840`), RF register `0x18` channel tuning, and antenna switch (`0x092C`).
   - `explorer_survey_3/analysis.md`: Empirical hardware verification on host (`pci10ec,b723`, subsystem `103c:804c`, PCI `2:0:0`, BAR2 16KB at `0xf1100000`, MSI vector 8), OpenCore EFI configuration on `/Volumes/HTOSH/EFI/OC`, discovery of active blocking patch `SSDT-Disable_Network_RP06.aml`, macOS 26.6.2 (Darwin 25.6.0) KPI compatibility (`IONetworkingFamily` 3.4, `IOPCIFamily` 2.9, Apple Clang 21.0.0), and 4-tier mock test harness design.
3. **Execution Artifact**:
   - Produced `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md`.
   - Verified via `wc -l -w -c`: 1267 lines, 9519 words, 76328 bytes.

---

## 2. Logic Chain

1. **Architecture Determination (Section 2)**:
   - Observation 2 revealed that macOS 26.6.2 has stripped legacy `IO80211Family` plugins from kernel collections, and `airportd` strictly enforces private entitlements that reject third-party drivers.
   - Observation 2 also confirmed that `NetworkingDriverKit` lacks public 802.11 Wi-Fi classes and cannot be injected at boot time from OpenCore EFI.
   - Conversely, `IOEthernetController` from `com.apple.iokit.IONetworkingFamily` (v3.4) and `IOPCIDevice` from `com.apple.iokit.IOPCIFamily` (v2.9) are stable, officially exported, public kernel programming interfaces.
   - Therefore, subclassing `IOEthernetController` with an in-driver 802.11 management/crypto engine and an `IOUserClient` control plane (the `itlwm` architecture) is the only viable, reliable, and native path on macOS 26.6.2.
2. **Hardware Register & Power State Design (Section 3)**:
   - Physical BAR2 MMIO is located at `0xf1100000` (16 KB).
   - Core registers (`REG_CR`, `REG_SYS_CLKR`, `REG_SYS_FUNC_EN`, `REG_HIMR`/`REG_HISR`, `REG_9346CR`, `REG_LLT_INIT`, `REG_PCIE_CTRL_REG`) were mapped with exact offsets and bitfields.
   - The hardware power state machine transitions `CARDDIS` -> `CARDEMU` -> `ACT` through unlocking `REG_RSV_CTRL (0x001C)`, disabling APS, ungating clocks, and initializing the Linked List Table (pages 0..244 for normal traffic, 245..255 for beacon/mgmt).
3. **Firmware Handshake (Section 4)**:
   - Realtek 8051 MCU requires uploading `rtl8723befw.bin` (version 36) with 32-byte header verification (`signature & 0xFFF0 == 0x5300`).
   - Microcode is streamed via 4KB swappable pages into MMIO window `0x1000..0x1FFF` selected by `REG_MCUFWDL+2`.
   - Boot handshake polls `FWDL_CHKSUM_RPT` (bit 2), asserts `MCUFWDL_RDY`, executes MCU self-reset, and verifies `WINTINI_RDY` (bit 6).
4. **eFuse & Calibration Layout (Section 5)**:
   - Explains OTP manual indirect readout via `REG_EFUSE_CTRL (0x0030)` and PG packet reconstruction of the 512-byte logical EEPROM shadow map.
   - Maps MAC address (`0xD0..0xD5`), crystal trim (`0xB9`), thermal meter (`0xBA`), and CCK/HT40 TX power tables (`0x10..0x1A`), with defensive fallback values.
5. **DMA Descriptor Ring Mechanics (Section 6)**:
   - Allocates 256-byte aligned, physically contiguous host memory for 7 TX priority queues and 1 RX ring.
   - Specifies exact bitfields for 40-byte TX descriptors (`struct tx_desc_8723be`) and 32-byte RX descriptors (`struct rx_desc_8723be`).
   - Documents ownership bit arbitration (`own`), doorbell triggers in `REG_PCIE_CTRL_REG (0x0300)`, and the mandatory End-of-Ring (`eor`) bit on index $N-1$ to prevent host memory corruption.
6. **RF Tuning & Antenna Diversity (Section 7)**:
   - Formulates bulk table loading (MAC, PHY, AGC, Radio A).
   - Specifies 3-wire LSSI serial RF access via Baseband register `0x0840`.
   - Implements 2.4 GHz channel tuning (channels 1–13) via RF register `0x18`.
   - Solves the HP laptop single-antenna flaw (`103c:804c`) by providing runtime antenna switching at Baseband register `0x092C` (Main = 1, Aux = 2; defaulting to Aux).
7. **802.11 Stack & Crypto Engine (Section 8)**:
   - Details active/passive scanner across channels 1–13, Open System Auth, and Association state machine.
   - Formulates WPA2-PSK 4-way handshake (EAPOL-Key Messages 1–4), PBKDF2-HMAC-SHA1 PMK derivation, and IEEE 802.11i PRF-512 PTK derivation (KCK, KEK, TK).
   - Details AES-128 CCM encryption/decryption, 48-bit Packet Number ($PN$) replay protection, and Ethernet II <-> RFC 1042 LLC/SNAP encapsulation.
8. **UserClient IPC & Safe Staging (Section 9)**:
   - Specifies 7 typed `IOUserClient` methods and `rtl8723be_cli` syntax.
   - Details EFI staging on `/Volumes/HTOSH/EFI/OC`, specifically setting `SSDT-Disable_Network_RP06.aml` to `Enabled = false` in `config.plist`.
9. **Traceability Matrix (Section 10)**:
   - Maps every requirement to driver source files and test verification tiers.

---

## 3. Caveats

- **No Caveats regarding document completeness**: All 9 required sections and technical parameters requested in Milestone M1 have been documented with full mathematical and bitfield rigor.
- **Hardware Variation Note**: While HP subsystem `103c:804c` typically routes single antennas to Aux (Port 2), certain board revisions may route to Port 1; the design specifies both a default configuration and a dynamic runtime switch (`rtl8723be_cli ant <1|2>`) to accommodate any variation.

---

## 4. Conclusion

Milestone M1 (Requirement R1) is **100% complete**. The Engineering Design Document at `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md` provides an exhaustive, authoritative, and implementation-ready technical specification for all subsequent implementation milestones (M2 through M6).

---

## 5. Verification Method

To independently verify the deliverable:
1. **File Existence & Non-Emptiness**:
   ```sh
   ls -la /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md
   wc -l -w -c /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md
   ```
   *Expected*: ≥ 1,000 lines, ≥ 70,000 bytes.
2. **Section Completeness Verification**:
   Inspect headers in `docs/DESIGN.md` to confirm all 9 required sections and the Traceability Matrix are present:
   ```sh
   grep -E "^## [0-9]+" /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md
   ```
   *Expected output*: Sections 1 through 10.
3. **Invalidation Condition**:
   The design would be invalidated only if Apple deprecates `IOEthernetController` from `IONetworkingFamily` 3.4 in a subsequent macOS major release, or if the physical PCI endpoint does not map BAR2 MMIO.
