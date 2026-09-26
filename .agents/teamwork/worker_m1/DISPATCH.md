## 2026-09-26T16:36:17Z

You are Worker M1 for the Realtek RTL8723BE macOS Wi-Fi driver project.

Your Working Directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/worker_m1
Original Request Path: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md
Project Specification: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/PROJECT.md

MANDATORY FIRST STEP:
Read /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md thoroughly before proceeding.
Also read /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/PROJECT.md and the survey reports:
- /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_1/analysis.md
- /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_2/spec.md
- /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_3/analysis.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

File Ownership:
You have exclusive write access to:
`/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md`

Your Task:
Author the comprehensive Engineering Design Document at `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md` fulfilling Requirement R1:
1. Executive Summary & Problem Formulation.
2. Architecture Selection & Comparative Evaluation:
   - Detailed technical analysis of IOEthernetController + IOUserClient + companion daemon vs IO80211Family shim vs DriverKit.
   - Exact macOS 26.6.2 (Darwin 25.6.0, 25G83) KPI mappings (com.apple.iokit.IONetworkingFamily 3.4, com.apple.iokit.IOPCIFamily 2.9, Apple Clang 21.0.0).
3. RTL8723BE Hardware Architecture & Registers:
   - Device PCI IDs (0x10EC:0xB723, subsystem 103C:804C, BAR2 16KB MMIO at 0xf1100000, MSI interrupt vector 8).
   - Core MMIO registers (CR, SYS_CLKR, SYS_FUNC_EN, HIMR/HISR, 9346CR, LLT_INIT, PCIE_CTRL_REG).
   - Complete power-on state machine sequence (pwrseq: CARDDIS -> CARDEMU -> ACT).
4. 8051 MCU Firmware Handshake Protocol:
   - Header format, 0x5301 signature, 4KB page download via REG_MCUFWDL+2 into MMIO 0x1000..0x1FFF.
   - Checksum handshake (FWDL_CHKSUM_RPT), self-reset, and WINTINI_RDY verification.
5. eFuse & Calibration Mapping:
   - OTP access protocol via REG_EFUSE_CTRL (0x0030).
   - Logical map decoding: MAC address (0xD0..0xD5), crystal cap trim (0xB9), thermal meter (0xBA), TX power (0x10..0x2D).
6. TX/RX DMA Descriptor Rings & Memory Architecture:
   - 40-byte TX descriptors, 32-byte RX descriptors, 64-bit DMA addresses, OWN bit arbitration, 256-byte alignment, ring wrapping, and doorbells.
7. Baseband, RF Tuning & Antenna Diversity:
   - Bulk table loading, 3-wire LSSI serial RF access via 0x0840, 2.4 GHz channel tuning (channels 1–13 via RF 0x18), and antenna switching (0x092C: 1=Main, 2=Aux).
8. 802.11 Protocol & Data Path:
   - Active/passive scan engine, Auth/Assoc state machine, WPA2-PSK 4-way handshake (EAPOL-Key Msg 1..4), PMK/PTK derivation, CCMP (AES-128 CCM) encryption/decryption.
   - Ethernet II <-> RFC 1042 LLC/SNAP 802.11 QoS Data translation.
9. User-Space Control & Safe Staging Plan:
   - IOUserClient IPC protocol and CLI command set.
   - OpenCore staging plan on /Volumes/HTOSH/EFI/OC (disabling SSDT-Disable_Network_RP06.aml in config.plist).
