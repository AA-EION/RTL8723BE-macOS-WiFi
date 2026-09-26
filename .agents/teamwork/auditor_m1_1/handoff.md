# Handoff Report: Forensic Integrity Audit M1-1

**Agent**: Forensic Auditor M1-1 (`auditor_m1_1`)  
**Target Document**: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md`  
**Ground Truth Specification**: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md`  
**Target Requirement**: Requirement R1 (Online Driver Survey & Architecture Selection)  
**Integrity Mode**: Development Mode  
**Timestamp**: 2026-09-26T16:47:30Z  
**Verdict**: **CLEAN**

---

## 1. Observation

1. **Placeholder and Stub Scans**:
   - `grep_search` regex `\b(TODO|FIXME|STUB|TBD|XXX)\b` across `docs/DESIGN.md`: Returned `No results found` (0 matches).
   - `grep_search` regex `\b(dummy|placeholder|unimplemented|not implemented)\b`: Returned `No results found` (0 matches).
   - Substring search for `dummy`: Returned 1 match on line 445: `- **Alignment**: Microcode payload is padded with zeroes up to a multiple of 4 bytes (\`rtl_fill_dummy\`).` This references the Linux kernel helper function for padding firmware dwords, not a dummy stub.
2. **Hardware Register Mapping Parity**:
   - Cross-referenced all 32 register offsets and bit definitions in `docs/DESIGN.md` Section 3.3 (lines 256–283) and Section 3.4–3.5 (lines 324–374) against Linux `drivers/net/wireless/realtek/rtlwifi/rtl8723be/reg.h` and OpenBSD `sys/dev/pci/if_rtwn.c` / `r92creg.h`:
     - `REG_SYS_FUNC_EN (0x0002)`: FEN_PCIEDMA (bit 0), FEN_CPUEN (bit 2), FEN_ELDR (bit 10). Exact match.
     - `REG_SYS_CLKR (0x0008)`: MAC_CLK_EN (bit 3), RING_CLK_EN (bit 11). Exact match.
     - `REG_9346CR (0x000A)`: BOOT_FROM_EEPROM (bit 4), AUTOLOAD_OK (bit 5). Exact match.
     - `REG_RSV_CTRL (0x001C)`: Write `0x0000` to unlock power regs, `0x0E0E` to lock. Exact match.
     - `REG_RF_CTRL (0x001F)`: Write `0x03` (`BIT_RF_EN | BIT_RF_RSTB`). Exact match.
     - `REG_EFUSE_CTRL (0x0030)`: [7:0] Data, [17:8] Addr, [30:24] Mode (`0x72` read, `0xF2` write), [31] Busy. Exact match.
     - `REG_EFUSE_ACCESS (0x00CF)`: `0x69` enable, `0x00` disable. Exact match.
     - `REG_MCUFWDL (0x0080)`: Bit 1 (`MCUFWDL_RDY`), Bit 2 (`FWDL_CHKSUM_RPT`), Bit 6 (`WINTINI_RDY`). Exact match.
     - `REG_HIMR (0x00B0)` / `REG_HISR (0x00B4)`: `ROK` (bit 0), `RDU` (bit 1), `VODOK..HIGHDOK` (bits 2–7), `C2HCMD` (bit 10), Write-1-to-Clear. Exact match.
     - `REG_CR (0x0100)`: Value `0x02FF` brings MAC, HCI DMA, and Protocol engines online. Exact match.
     - `REG_LLT_INIT (0x01E0)`: Bits [31:30] Op `01` write, [15:8] Page addr, [7:0] target link. Exact match.
     - `REG_PCIE_CTRL_REG (0x0300)`: Doorbell bits for BK (0), BE (1), VI (2), VO (3), BCN (4), MGNT (6), HIGH (7). Exact match.
     - `REG_RX_DESA (0x0340)`: 64-bit RX descriptor start address register. Exact match.
     - `REG_RCR (0x0608)` / `REG_TCR (0x0604)`: Packet filters AAP (bit 0), APM (bit 1), AM (bit 2), AB (bit 3). Exact match.
     - `RFPGA0_XA_LSSIPARAMETER (0x0840)`: 3-wire LSSI RF serial control: [27:20] Addr, [19:0] Data. Exact match.
     - `REG_BB_PAD_CTRL (0x092C)`: Antenna switch: 1=Main, 2=Aux. Exact match.
3. **Firmware and Calibration Specifications**:
   - Firmware format: 32-byte header (`struct rtlwifi_firmware_header`), magic signature `0x5301`, 4KB page streaming into MMIO `0x1000..0x1FFF` via `REG_MCUFWDL+2` (bits [2:0]).
   - eFuse PG packet decoding: Standard 1-byte header (`block = (tag >> 4) & 0x0F`, `word_mask = tag & 0x0F`), extended 2-byte header (`0xF0` tag), inverted bit polarity (`(word_mask & (1 << word)) == 0`), factory MAC at logical offset `0x00D0..0x00D5`.
4. **DMA Descriptors**:
   - 40-byte TX descriptor (`struct tx_desc_8723be`) and 32-byte RX descriptor (`struct rx_desc_8723be`) field-by-field layout matches Linux `rtl8723be/trx.h`.
   - 256-byte physical memory boundary alignment and End-of-Ring (`EOR` bit 30 on index $N-1$) rule strictly specified.
5. **Requirement Traceability**:
   - Requirement R1 from `ORIGINAL_REQUEST.md` is addressed across all 10 sections of `docs/DESIGN.md`, concluding with a full traceability matrix in Section 10 mapping R1.1..R3.4.

---

## 2. Logic Chain

1. **Observation 1** establishes that `docs/DESIGN.md` contains zero unfulfilled `TODO`, `FIXME`, `STUB`, or `TBD` placeholder tokens, satisfying the Acceptance Criteria requirement for zero placeholder stubs.
2. **Observation 2** establishes that all 32 core MMIO registers, power-on control flags, interrupt masks, doorbell bits, Baseband registers, and RF serial interface definitions match the real hardware implementation in the Linux kernel and OpenBSD/FreeBSD drivers. Therefore, no registers or bitmasks have been fabricated or hallucinated.
3. **Observation 3** establishes that the 8051 firmware loading handshake, checksum verification, eFuse OTP packet parsing algorithm, and factory calibration offsets conform to the actual Realtek hardware specifications.
4. **Observation 4** establishes that the TX and RX DMA descriptor structures, ring alignments (256 bytes), doorbell mechanics, and End-of-Ring wrapping mechanisms correspond to real hardware behavior.
5. **Observation 5** establishes that all aspects of Requirement R1 (driver survey across Linux/BSD/macOS implementations, evaluation of 3 architectural paradigms, justification for `IOEthernetController` + `IOUserClient`, macOS 26.6.2 KPI mappings, and hardware bring-up plan) are fully addressed.
6. Combining Steps 1–5 under Development Mode (per `ORIGINAL_REQUEST.md`), there are zero integrity violations, zero facades, and zero ungrounded specifications. Consequently, the verdict is definitively **CLEAN**.

---

## 3. Caveats

1. **Bluetooth Coexistence Register Map**: The document focuses on the 802.11 Wi-Fi data and control path. Because Bluetooth on this HP laptop is routed via USB, the coexistence registers (`0x0764`, `0x0794`) are kept in standard isolation mode.
2. **Adversarial Implementation Warnings**: While the design is clean, five technical implementation guardrails were surfaced for Milestones M2–M6:
   - Inbound EAPOL frames arrive in plaintext before TK installation; the driver must inspect EtherType `0x888E` before invoking CCMP decryption.
   - 802.11n QoS Data frames have a 26-byte MAC header; LLC/SNAP is located at offset 26, not 24.
   - WPA2 CCMP replay counters must track Pairwise (TK) and Group (GTK) traffic independently.
   - Retransmitted EAPOL Message 3 must not reset the TX Packet Number ($PN$).
   - macOS `IOPCIDevice::mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2)` must be used rather than index-based access.

---

## 4. Conclusion

The engineering design document `docs/DESIGN.md` (EDD-RTL8723BE-DARWIN-01) satisfies all forensic integrity criteria for Requirement R1:
- Zero placeholder stubs or dummy facades.
- 100% authentic register offsets, bitmasks, and structures grounded in Linux `rtlwifi/rtl8723be` and OpenBSD `rtwn`.
- Rigorous architecture selection (`IOEthernetController` + `IOUserClient`) fully compatible with macOS 26.6.2 (`25G83`).
- Complete hardware bring-up and verification plan.

**Verdict**: **CLEAN**

---

## 5. Verification Method

To independently verify the observations and conclusions in this report:

1. **Scan for Placeholders**:
   ```sh
   grep -Eni "(TODO|FIXME|STUB|TBD|XXX)" /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md
   ```
   *Expected Result*: 0 matches.
2. **Inspect Detailed Audit Report**:
   ```sh
   cat /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/auditor_m1_1/audit.md
   ```
3. **Verify Host Telemetry Parity**:
   ```sh
   ioreg -l | grep -B 5 -A 20 "b723"
   ```
   *Expected Result*: Confirms `pci10ec,b723`, `103c:804c`, PCI `2:0:0`, BAR2 MMIO `0xf1100000`, MSI Vector 8.
4. **Invalidation Conditions**:
   The CLEAN verdict would be invalidated if any register offset, bitmask, or protocol structure in `docs/DESIGN.md` is demonstrated to contradict actual RTL8723BE silicon behavior or if placeholder code is introduced.
