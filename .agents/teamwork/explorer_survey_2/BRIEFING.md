# BRIEFING — 2026-09-26T16:17:09Z

## Mission
Extract and document authoritative hardware specifications, registers, firmware handshake, eFuse map, DMA descriptors, and MAC/BB/RF initialization for RTL8723BE PCIe Wi-Fi chip from Linux rtlwifi/rtl8723be and OpenBSD rtwn.

## 🔒 My Identity
- Archetype: SPECIFICATION MINER
- Roles: Teamwork specialist, external domain expert
- Working directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_2
- Original parent: 97aa8b74-6088-4e6c-8c61-400af4865bc2
- Milestone: Hardware & Driver Specification Discovery (Explorer Survey 2)

## 🔒 Key Constraints
- Authoritative specification mining only; do not implement driver code.
- Probe all features thoroughly, including edge cases and error behaviors.
- Write outputs to spec.md and handoff.md in own folder.
- Send messages only to parent via send_message.

## Current Parent
- Conversation ID: 97aa8b74-6088-4e6c-8c61-400af4865bc2
- Updated: 2026-09-26T16:34:00Z

## Task Summary
- **What to build**: Comprehensive hardware specification report (`spec.md`) and handoff report (`handoff.md`) covering RTL8723BE PCIe architecture, MMIO registers, power sequence, firmware loading & handshake, eFuse structure & calibration, TX/RX DMA rings & descriptors, and MAC/BB/RF initializations.
- **Success criteria**: Detailed, non-stubbed, exact register addresses, bitmasks, structures, and algorithms ready for macOS driver implementation. [COMPLETED]
- **Interface contracts**: Linux kernel `drivers/net/wireless/realtek/rtlwifi/rtl8723be`, `rtl_pci`, OpenBSD `sys/dev/pci/if_rtwn.c` / `rtwn_pci.c` / `r92creg.h`.

## Key Decisions Made
- Extracted and verified full hardware specs from Linux kernel rtlwifi/rtl8723be and OpenBSD rtwn/r92c.
- Analyzed binary headers of official `rtl8723befw.bin` (signature 0x5301, 32-byte header, 4KB page download).
- Documented full 40-byte TX and 32-byte RX buffer descriptor formats with 64-bit DMA addressing and EOR ring boundary handling.
- Documented RF channel tuning (RF register 0x18 via BB 0x840) and single-antenna switching (BB 0x92C).

## Artifact Index
- spec.md — Authoritative RTL8723BE Hardware and Protocol Specification
- handoff.md — 5-Component handoff report
- progress.md — Task checklist and status

