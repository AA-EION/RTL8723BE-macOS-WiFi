# Dispatch: Explorer Survey 2

**Working Directory**: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_2
**Task**: Survey Linux rtlwifi/rtl8723be, OpenBSD rtwn, hardware registers, eFuse, firmware handshake, and DMA ring structures.

## 2026-09-26T16:17:09Z
You are a Spec Miner subagent for the Realtek RTL8723BE macOS Wi-Fi driver project.

Your Working Directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_2
Original Request Path: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md

MANDATORY FIRST STEP:
Read /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md thoroughly before proceeding.

Your Mission:
Extract and document the authoritative hardware specifications, register definitions, firmware handshake protocol, eFuse map, and DMA descriptors for the Realtek RTL8723BE PCIe Wi-Fi chip from reference sources (Linux kernel rtlwifi/rtl8723be, OpenBSD rtwn).

Key Investigation Points:
1. RTL8723BE Hardware Architecture & Registers:
   - PCI configuration, BAR0 (I/O) & BAR2 (64-bit MMIO 16KB).
   - Core registers: CR (Command Register), IMR / ISR (Interrupt Mask & Status Register), SYS_CLKR, PWR_MODE, EFUSE_CTRL / EFUSE_TEST.
   - Power-on sequence (pwrseq) state machine: transitioning between Card Disable, Card Enable, and Normal modes.
2. Firmware Loading Protocol:
   - Firmware image: `rtl8723befw.bin` structure (header, signature, code sections).
   - 8051 MCU RAM download mechanism (page writing via registers / FIFO).
   - Boot handshake, checksum validation, and firmware ready signaling.
3. eFuse & Calibration:
   - Autoload mechanism vs manual eFuse readout.
   - Layout of MAC address (physical address), crystal calibration (crystal cap), thermal sensor calibration, and TX power per channel/path.
4. TX/RX DMA Engine:
   - TX/RX descriptor format (TX/RX Buffer Descriptors, 32-byte / 40-byte layouts, flags, length, physical address pointers).
   - TX queues (Beacon, High, Normal, Low priority queues) and RX ring.
   - Page size, ring buffer sizing, memory alignment, and cache coherency.
5. Baseband & RF:
   - MAC/BB initialization sequence, AGC (Automatic Gain Control) tables.
   - Radio frequency (RF) channel setting for 2.4 GHz channels 1–13.

Deliverables:
- Write your comprehensive specification document to `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_2/spec.md`
- Write `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_2/handoff.md` with structured Observation, Logic Chain, Caveats, Conclusion, and Verification.
- Send a completion message back to parent.

