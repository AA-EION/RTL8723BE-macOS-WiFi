# Test Infrastructure Specification: Realtek RTL8723BE macOS Wi-Fi Driver

**Document Version**: 1.0.0  
**Target Environment**: macOS 26.6.2 (Darwin 25.6.0, x86_64)  
**Target Device**: Realtek RTL8723BE PCIe 802.11b/g/n (`0x10EC:0xB723`, subsystem `103C:804C`)  
**Scope**: Automated Mock Hardware/DMA Test Bench & 4-Tier Regression Test Suite  

---

## 1. Overview & Objectives

The RTL8723BE macOS Wi-Fi driver operates in kernel space managing direct PCI Express Memory-Mapped I/O (MMIO), multi-queue scatter-gather Direct Memory Access (DMA), 8051 MCU microcode handshakes, and cryptographic wireless protocols (802.11i WPA2-PSK / AES-CCMP). Flaws in descriptor ring management, buffer ownership bits, or state machine transitions can trigger kernel panics or system instability.

To guarantee complete functional correctness, memory safety, and protocol fidelity prior to live kernel staging, this test infrastructure provides:
1. **Deterministic User-Space Hardware Emulation**: A full simulation of the RTL8723BE PCIe controller, including 16KB BAR2 MMIO register space, reactive hardware hooks, power-on sequences (`pwrseq`), eFuse OTP calibration readout, 8051 MCU firmware download handshake with checksum validation, and multi-queue TX/RX DMA descriptor rings with hardware OWN bit arbitration.
2. **Synthetic 802.11 & EAPOL Packet Injector**: Programmable generation and injection of IEEE 802.11 Management (Beacons, Probe Requests/Responses, Auth, Assoc, Deauth) and Data/EAPOL frames (WPA2 4-way handshake, AES-CCMP encrypted MSDUs, ARP/IP traffic).
3. **Four-Tier Test Hierarchy**:
   - **Tier 1 (Features)**: Category-partitioned verification of core functional units (≥5 tests per feature).
   - **Tier 2 (Boundaries)**: Extreme values, boundary conditions, zero-length, MTU limits, buffer overflows, and corruption faults.
   - **Tier 3 (Pairwise Interactions)**: Concurrent and orthogonal cross-feature interactions (scanning during DMA, rekeying during bursts, antenna switching under load).
   - **Tier 4 (Workloads)**: Full end-to-end real-world operational workflows from cold power-on to WPA2 association and continuous bidirectional IP traffic.

---

## 2. Mock Hardware & DMA Simulation Architecture

### 2.1 Component Interaction Diagram

```
+---------------------------------------------------------------------------------+
|                                Test Harness Runner                              |
|                       (tests/test_runner.cpp / clang++ -std=c++17)              |
+---------------------------------------------------------------------------------+
         |                                                   |
         v                                                   v
+-------------------------------+                  +-------------------------------+
|  Synthetic Packet Injector    |                  |  Simulated Driver / Controller|
|  (mock_packet_injector.hpp)   |                  |  - Firmware loader            |
|  - 802.11 Beacons / Probes    |                  |  - eFuse & calibration reader |
|  - Auth & Assoc Frames        |                  |  - DMA ring manager           |
|  - EAPOL-Key 4-Way Handshake  |                  |  - 802.11 / WPA2 state machine|
|  - CCMP Encrypted Frames      |                  |  - CCMP crypto engine         |
+-------------------------------+                  +-------------------------------+
         |                                                   |
         | Inject Inbound RX Frames                          | MMIO Reads/Writes
         v                                                   | TX Descriptors & Doorbells
+------------------------------------------------------------v--------------------+
|                         Mock Hardware & DMA Subsystem                           |
|                                                                                 |
|  +---------------------------------------------------------------------------+  |
|  | Mock PCI MMIO (mock_pci_mmio.hpp / .cpp)                                  |  |
|  | - 16KB BAR2 MMIO Array (0x0000 - 0x3FFF)                                  |  |
|  | - Reactive Register Callbacks (REG_CR, REG_MCUFWDL, REG_EFUSE_CTRL)       |  |
|  | - Power Sequence State Machine (CARDDIS -> CARDEMU -> ACT)                |  |
|  | - eFuse OTP Storage & Logical Shadow Mapping (512 bytes)                  |  |
|  | - 8051 MCU Firmware RAM (8 Pages x 4KB), Checksum Engine & Ready Handshake|  |
|  | - Interrupt Controller: HIMR, HISR (write-1-to-clear), MSI Vector Emulation| |
|  +---------------------------------------------------------------------------+  |
|                                                                                 |
|  +---------------------------------------------------------------------------+  |
|  | Mock DMA Engine (mock_dma.hpp / .cpp)                                     |  |
|  | - Physical 64-bit Memory Pool & Address Translation                       |  |
|  | - Multi-Queue TX Rings: BK, BE, VI, VO, BCN, MGNT, HIGH (40-byte descs)   |  |
|  | - RX Ring: 32-byte descriptors with End-Of-Ring (EOR) wrapping            |  |
|  | - Hardware OWN Bit Arbitration (CPU owns: OWN=0; HW owns: OWN=1)          |  |
|  | - Queue Doorbells (REG_PCIE_CTRL_REG 0x0300)                              |  |
|  | - RX Buffer Starvation (RDU) Detection & Overflow Handling                 |  |
|  +---------------------------------------------------------------------------+  |
+---------------------------------------------------------------------------------+
```

### 2.2 Mock PCI MMIO (`mock_pci_mmio.hpp` / `mock_pci_mmio.cpp`)
- **Memory Model**: 16,384-byte array representing BAR2 (`0x0000` to `0x3FFF`).
- **Accessors**: Byte (`read8`/`write8`), Word (`read16`/`write16`), and Dword (`read32`/`write32`) operations with alignment verification.
- **Power Sequence State Machine**:
  - Tracks power state: `CARDDIS` (0), `CARDEMU` (1), `ACT` (2).
  - Emulates sequencing of `REG_RSV_CTRL (0x001C)`, `REG_APS_FSMCO (0x0004)`, `REG_SYS_FUNC_EN (0x0002)`, `REG_SYS_CLKR (0x0008)`, and `REG_CR (0x0100)`.
  - Enforces clock and LDO stabilization flags before allowing MAC/DMA enable.
- **eFuse OTP Controller Emulation**:
  - Simulates 256 physical bytes of eFuse OTP memory.
  - Intercepts read commands written to `REG_EFUSE_CTRL (0x0030)`.
  - When address and mode `0x72` are written, latches the target byte into `REG_EFUSE_CTRL [7:0]` and clears the busy bit [31].
  - Pre-loads factory MAC address (`00:E0:4C:81:92:23`), crystal trim (`0x28`), thermal meter (`0x1A`), regulatory channel plan (`0x00`), and per-channel TX power levels.
  - Emulates autoload controller flag in `REG_9346CR (0x000A)` bit 5.
- **8051 MCU Firmware Download Handshake**:
  - Simulates 8051 MCU RAM download window at MMIO `0x1000..0x1FFF` (4096 bytes per page).
  - Page selection via `REG_MCUFWDL+2 (0x0082)` bits [2:0].
  - Checksum validation: Calculates running 16-bit word checksum of uploaded microcode. On download completion (clearing `FWDL_EN` in `0x0080`), asserts `FWDL_CHKSUM_RPT` (bit 2).
  - Ready handshake: On MCU self-reset, asserts `WINTINI_RDY` (bit 6) to signal MCU readiness.
- **Host Interrupt Controller**:
  - Emulates `REG_HIMR (0x00B0)` (mask) and `REG_HISR (0x00B4)` (status).
  - Status register supports write-1-to-clear semantics.
  - Dispatches interrupt callbacks when unmasked status bits are asserted.

### 2.3 Mock DMA Engine (`mock_dma.hpp` / `mock_dma.cpp`)
- **Host Memory Simulation**: Contiguous physical host memory address allocator translating 64-bit guest physical addresses to accessible host virtual pointers.
- **TX Descriptor Rings (40-Byte Layout)**:
  - Supports 7 independent hardware priority queues: BK (0), BE (1), VI (2), VO (3), Beacon (4), Management (6), High (7).
  - Tracks base address registers `REG_*Q_DESA (0x0308..0x0338)`.
  - Doorbell processing: Writing queue bitmask to `REG_PCIE_CTRL_REG (0x0300)` triggers immediate DMA traversal.
  - OWN bit arbitration: Validates that host set `OWN = 1`, retrieves packet buffer from physical address, parses MAC/LLC/CCMP payload, clears `OWN = 0`, sets TX status, and raises queue TX_OK interrupt in `HISR`.
- **RX Descriptor Ring (32-Byte Layout)**:
  - Base address register `REG_RX_DESA (0x0340)`.
  - Ring size configurable (default 64 or 256 descriptors), strictly aligned to 256-byte boundaries.
  - End-of-Ring (`EOR` bit 30) triggers ring index wrap back to slot 0.
  - Buffer replenishment & starvation: If injector receives frame but current descriptor has `OWN == 0`, asserts Rx Descriptor Unavailable (`RDU` bit 1 in `HISR`) and increments drop counter.

### 2.4 Mock Packet Injector (`mock_packet_injector.hpp`)
- **802.11 Frame Generation**:
  - Beacons & Probe Responses: Frame Control, BSSID, SSID IE, Supported Rates IE, DS Parameter Set (channel), RSN IE (WPA2-PSK CCMP).
  - Authentication Frames: Open System (Seq 1 / Seq 2).
  - Association Request / Response: Capabilities, AID, status codes.
  - Deauthentication Frames: Reason codes (e.g. Reason 2: Previous authentication invalid).
- **WPA2 4-Way Handshake Simulation**:
  - EAPOL-Key Msg 1 (ANonce injection).
  - EAPOL-Key Msg 2 capture & verification (SNonce, MIC verification).
  - EAPOL-Key Msg 3 (AP MIC, encrypted GTK under KEK).
  - EAPOL-Key Msg 4 capture & verification (handshake completion).
- **Data & Protocol Injection**:
  - CCMP Data Frames: IEEE 802.11 QoS Data headers, RFC 1042 LLC/SNAP headers (`0xAA-AA-03-00-00-00`), CCMP 8-byte header, AES-CCM encrypted payload, and 8-byte MIC.
  - ARP Request / Reply frames encapsulated in 802.11.

---

## 3. Four-Tier Test Suite Specification

### 3.1 Tier 1: Core Feature Verification (≥5 Tests per Feature)

| Feature # | Feature Name | Test Identifier | Test Description | Authoritative Oracle |
|---|---|---|---|---|
| **F1** | Firmware Loader | `test_fw_valid_header` | Validates 32-byte header parsing (`0x5301` signature, version, size) | Linux `rtl8723be/sw.c:185` |
| | | `test_fw_page_download` | Downloads microcode pages into MMIO `0x1000..0x1FFF` across 8 pages | Linux `fw_common.c:38-70` |
| | | `test_fw_checksum_success` | Validates MCU checksum handshake reporting bit 2 in `REG_MCUFWDL` | Linux `fw_common.c:132-177` |
| | | `test_fw_ready_handshake` | Asserts MCU reset sequence and ready bit 6 in `REG_MCUFWDL` | Linux `fw_common.c:150` |
| | | `test_fw_self_reset` | Tests firmware self-reset procedure when MCU is already running | Linux `hw.c:1200` |
| | | `test_fw_page_overflow` | Verifies rejection when page count exceeds maximum allowable (8) | Linux `fw_common.c:55` |
| **F2** | eFuse & Calibration | `test_efuse_read_mac` | Reads physical eFuse via `0x0030`, extracts MAC `0xD0..0xD5` | Realtek OTP spec / eFuse map |
| | | `test_efuse_autoload_status` | Verifies autoload detection in `REG_9346CR` bit 5 | Linux `hw.c:2050` |
| | | `test_efuse_pg_packet_decode` | Decodes variable-length PG packets into 512-byte logical EEPROM shadow | Linux `efuse.c:215-320` |
| | | `test_efuse_calibration_fields` | Extracts crystal cap (`0xB9`), thermal meter (`0xBA`), channel plan (`0xB8`) | Linux `hw.c:2070-2150` |
| | | `test_efuse_tx_power_tables` | Extracts CCK and HT40 base TX power across all channel groups | Linux `reg.h:660-700` |
| **F3** | DMA Ring Operations | `test_dma_tx_desc_layout` | Verifies 40-byte TX descriptor layout, fields, and 256-byte alignment | Linux `trx.h:17-105` |
| | | `test_dma_tx_doorbell_own` | Verifies doorbell trigger on `0x0300`, OWN bit arbitration, and TX_OK ISR | Linux `trx.c:700` |
| | | `test_dma_tx_ring_wrap` | Enqueues past ring capacity; verifies wrap to index 0 without overrun | Linux `pci.c:1200` |
| | | `test_dma_rx_eor_wrap` | Verifies 32-byte RX ring End-Of-Ring (`eor`) bit 30 circular wrapping | Linux `trx.h:122` |
| | | `test_dma_rx_packet_reception`| Injects packet; verifies length, shift offset, CRC status, and OWN release | Linux `trx.h:107-160` |
| | | `test_dma_tx_priority_queues` | Verifies multi-queue arbitration order (VO > VI > BE > BK) | 802.11e EDCA / WMM spec |
| **F4** | 802.11 Beacon Parser | `test_beacon_fixed_fields` | Parses Timestamp, Beacon Interval (100 TU), Capabilities (ESS, Privacy) | IEEE 802.11-2016 §9.3.3.3 |
| | | `test_beacon_ssid_ie` | Parses SSID Information Element (Tag 0, length, string) | IEEE 802.11-2016 §9.4.2.2 |
| | | `test_beacon_rates_ie` | Parses Supported Rates and Extended Supported Rates (Tags 1, 50) | IEEE 802.11-2016 §9.4.2.3 |
| | | `test_beacon_channel_ie` | Parses DS Parameter Set (Tag 3, current operating channel 1–13) | IEEE 802.11-2016 §9.4.2.4 |
| | | `test_beacon_rsn_ie_wpa2` | Parses WPA2 RSN IE (Tag 48: AES-CCMP pairwise, CCMP group, PSK AKM) | IEEE 802.11-2016 §9.4.2.25 |
| | | `test_beacon_hidden_ssid` | Verifies correct handling of hidden/broadcast SSIDs (length 0 or nulls) | IEEE 802.11-2016 §9.4.2.2 |
| **F5** | WPA2 4-Way Handshake | `test_wpa2_pmk_pbkdf2` | Derives 256-bit PMK via PBKDF2-HMAC-SHA1 (4096 iterations) | RFC 6070 / IEEE 802.11i |
| | | `test_wpa2_ptk_prf512` | Derives 512-bit PTK (KCK, KEK, TK) using IEEE 802.11i PRF-512 | IEEE 802.11-2016 §12.7.1.3 |
| | | `test_wpa2_msg1_reception` | Receives EAPOL-Key M1 (AP ANonce); validates state transition & SNonce gen | IEEE 802.11-2016 §12.7.6.2 |
| | | `test_wpa2_msg2_mic` | Generates EAPOL-Key M2; verifies HMAC-SHA1 MIC computed with KCK | IEEE 802.11-2016 §12.7.6.3 |
| | | `test_wpa2_msg3_gtk_decrypt`| Receives M3; verifies MIC and unwraps GTK via AES Key Wrap (RFC 3394) | RFC 3394 / 802.11i §12.7.6.4 |
| | | `test_wpa2_msg4_completion` | Sends M4; confirms state transition to CONNECTED and key activation | IEEE 802.11-2016 §12.7.6.5 |
| **F6** | CCMP Crypto Engine | `test_ccmp_header_and_pn` | Constructs 8-byte CCMP header, Key ID 0, and 48-bit Packet Number (PN) | IEEE 802.11-2016 §12.5.3.3 |
| | | `test_ccmp_nonce_construction`| Formats 13-byte Nonce (Priority, A2/STA MAC, PN) | IEEE 802.11-2016 §12.5.3.3.3|
| | | `test_ccmp_aad_construction` | Formats Additional Authentication Data (AAD) from 802.11 MAC header | IEEE 802.11-2016 §12.5.3.3.2|
| | | `test_ccmp_encrypt_vector` | Encrypts payload & computes 8-byte MIC against IEEE 802.11 Annex J test vector| IEEE 802.11-2016 Annex J / RFC 3610 |
| | | `test_ccmp_decrypt_vector` | Decrypts ciphertext & validates MIC against known test vector | IEEE 802.11-2016 Annex J |
| | | `test_ccmp_replay_detection` | Injects frame with stale/duplicate PN; verifies frame rejection | IEEE 802.11-2016 §12.5.3.4.4|

---

### 3.2 Tier 2: Boundary & Corner Case Tests

| Test Identifier | Boundary Condition Tested | Expected Behavior |
|---|---|---|
| `test_bound_zero_length_frame` | Zero-length packet or descriptor buffer length = 0 | Driver/DMA ignores or rejects frame with error; no memory fault |
| `test_bound_max_mtu_ethernet` | Standard maximum Ethernet frame (1514 bytes) | Successful encapsulation into 802.11 Data frame, encryption, and TX |
| `test_bound_max_msdu_80211` | Maximum 802.11 MSDU payload (2304 bytes) | Successful CCMP encryption, descriptor transmission, and decryption |
| `test_bound_oversized_frame` | Oversized frame (> 2304 bytes MSDU) | Driver drops packet, flags length error, preserves ring state |
| `test_bound_corrupted_crc32` | Inbound RX frame with corrupted 802.11 CRC32/FCS | Hardware sets `crc32 = 1` in RX desc; driver discards frame |
| `test_bound_corrupted_ccmp_mic`| Inbound CCMP frame with altered payload / invalid MIC | Decryption fails MIC check; frame discarded; no packet passed up |
| `test_bound_rx_ring_starvation`| All RX ring descriptors owned by CPU (`OWN = 0`) | Hardware asserts `RDU` interrupt in `HISR`; increments drop counter |
| `test_bound_tx_ring_full` | All TX ring slots occupied (`OWN = 1`) | Driver halts transmission, sets queue stopped flag, avoids overwrite |
| `test_bound_corrupt_efuse` | Incomplete / truncated PG packet stream in eFuse | Parser safely terminates; falls back to default factory calibration |
| `test_bound_invalid_fw_sig` | Firmware binary with invalid signature (`0xFFFF` vs `0x5301`) | Firmware loader aborts immediately; returns error code; MCU held in reset |
| `test_bound_unaligned_mmio` | Unaligned 16-bit or 32-bit MMIO access (e.g. offset `0x0001` for 32-bit) | Mock MMIO flags alignment warning/fault; maintains state integrity |

---

### 3.3 Tier 3: Pairwise Interaction Tests

| Test Identifier | Primary Feature | Concurrent / Interacting Feature | Expected System Behavior |
|---|---|---|---|
| `test_pair_scan_during_tx_dma` | 2.4 GHz active channel scan (hopping Ch 1..13) | Continuous background TX DMA data stream | Driver pauses/resumes queue or tags probe frames cleanly without ring desync |
| `test_pair_rekey_during_traffic`| WPA2 GTK group rekeying (EAPOL M1/M2) | High-throughput bidirectional packet burst | Unicast traffic continues using PTK; new GTK installed seamlessly without packet loss |
| `test_pair_antenna_switch_load` | Antenna toggle (Main <-> Aux via BB `0x092C`) | Active bidirectional streaming | RF switch completes without MMIO race; subsequent packets reflect updated RSSI |
| `test_pair_suspend_dma_active` | Power-down / suspend sequence (ACT -> CARDDIS) | Pending unacknowledged TX descriptors | Driver safely drains or cancels pending descriptors; halts DMA cleanly |
| `test_pair_multicast_unicast_mix`| Multicast beacon / ARP broadcast reception | Unicast CCMP data flow with unique PN sequence | Correct broadcast filter matching; unicast PN sequences remain strictly monotonic |

---

### 3.4 Tier 4: Real-World Workload Scenarios

| Test Identifier | Scenario Workflow | Success Criteria |
|---|---|---|
| `test_workload_cold_bringup` | 1. Power on (`CARDDIS` -> `CARDEMU` -> `ACT`)<br>2. 8051 Firmware upload & checksum handshake<br>3. eFuse readout & calibration load<br>4. DMA descriptor ring initialization | Controller transitions to ready state; MAC/BB registers programmed; zero hardware errors |
| `test_workload_scan_discovery` | 1. Trigger active scan sweep across channels 1–13<br>2. Inject 10 AP beacons with varied SSIDs, RSSIs, and security modes<br>3. Collect scan results | 10 distinct APs discovered; duplicate beacons merged; RSSI averaged; sorted correctly |
| `test_workload_open_association` | 1. Initiate connection to Open (unsecured) network<br>2. Probe Req/Resp -> Auth (Seq 1/2) -> Assoc Req/Resp<br>3. Verify state transition | Associated state reached; link status reported valid; DHCP/data path ready |
| `test_workload_wpa2_full_connect`| 1. Associate with WPA2-PSK AP (`"HomeNetwork"`, pass `"secretPassphrase"`)<br>2. Execute complete 4-way handshake (M1..M4)<br>3. Verify PTK/GTK installation | Complete handshake success; keys installed; link state transitions to `CONNECTED` |
| `test_workload_bidirectional_arp_ip`| 1. Establish connected link<br>2. Host transmits ARP Request (Ethernet II -> 802.11 LLC/SNAP -> CCMP Encrypt -> TX DMA)<br>3. Mock injects ARP Reply (RX DMA -> Decrypt -> Decapsulate -> Host Ethernet) | Both packets transmitted and received with 100% byte fidelity and verified MIC |
| `test_workload_ap_deauth_recovery` | 1. Established connection active<br>2. Inbound Deauth frame injected from AP<br>3. Verify teardown, key flush, link down, and automated reconnection attempt | Driver cleans up connection; drops link state; initiates new scan and re-associates |

---

## 4. Authoritative Output Derivation & Oracle Reference

All expected values and test assertions are derived from authoritative open-source implementations and international specifications:
1. **IEEE 802.11-2016 Standard**:
   - Frame Formats: Section 9.2 (MAC frame formats), Section 9.3 (Management frames).
   - Security: Section 12.5.3 (CCMP processing), Section 12.7.1 (PRF-512), Section 12.7.6 (4-way handshake).
   - Test Vectors: Annex J (CCMP test vectors with exact hex inputs, keys, nonces, and ciphertexts).
2. **RFC Specifications**:
   - RFC 6070: PBKDF2-HMAC-SHA1 test vectors.
   - RFC 3610: Counter with CBC-MAC (CCM) verification vectors.
   - RFC 3394: AES Key Wrap Algorithm test vectors for EAPOL GTK unwrapping.
3. **Linux Kernel `rtlwifi/rtl8723be` & `rtl_pci`**:
   - Register definitions (`reg.h`, `r8723b_reg.h`).
   - 40-byte TX and 32-byte RX descriptor bit definitions (`trx.h`, `trx.c`).
   - eFuse memory map and PG packet parsing algorithm (`efuse.c`, `hw.c`).
   - Firmware download handshake timing and control bits (`fw_common.c`, `sw.c`).
4. **OpenBSD Kernel `if_rtwn.c` / `r92creg.h`**:
   - Power-on sequence register states and command register configurations.

---

## 5. Build, Execution & Test Runner Architecture

### 5.1 Test Framework
The test harness uses a custom, lightweight, zero-dependency C++17 test framework (`tests/mock/test_framework.hpp`) providing:
- Test registration and categorization (`REGISTER_TEST(Category, TestName)`).
- Standard assertion macros (`ASSERT_TRUE`, `ASSERT_FALSE`, `ASSERT_EQ`, `ASSERT_NE`, `ASSERT_MEMEQ`, `ASSERT_THROW`).
- Per-tier and per-feature execution filtering (`--tier=1`, `--feature=efuse`, etc.).
- Detailed ANSI color console reporting, test durations, and failure diagnosis.
- Standard exit codes (0 = all tests passed, 1 = one or more failures).

### 5.2 Build Command & Toolchain
- **Compiler**: macOS Apple Clang (`clang++ -std=c++17 -Wall -Wextra -O2`).
- **Build Targets**:
  - `make` or `make all`: Compiles all mock hardware, crypto engines, test tiers, and the standalone `test_runner` executable.
  - `make test`: Builds and runs the full test suite.
  - `make clean`: Removes build objects and binaries.

---

*TEST_INFRA.md complete. Maintained by Test Writer M2.*
