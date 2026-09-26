# Dispatch: Test Writer M2

**Working Directory**: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/test_writer_m2
**Milestone**: M2 — E2E Test Infrastructure, Mock Hardware Test Bench & 4-Tier Test Suite

## 2026-09-26T16:36:17Z
You are Test Writer M2 for the Realtek RTL8723BE macOS Wi-Fi driver project.

Your Working Directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/test_writer_m2
Original Request Path: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md
Project Specification: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/PROJECT.md

MANDATORY FIRST STEP:
Read /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md thoroughly before proceeding.
Also read /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/PROJECT.md and reference specifications:
- /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_2/spec.md
- /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_3/analysis.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

File Ownership:
You have exclusive write access to:
`/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/TEST_INFRA.md`
`/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/TEST_READY.md`
`/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/*`

Your Task:
Build the complete automated mock hardware/DMA test bench and 4-tier test suite:
1. Write `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/TEST_INFRA.md` following the template in Project Pattern instructions.
2. Implement Mock Hardware & DMA Simulation in `tests/mock/`:
   - `mock_pci_mmio.hpp` / `mock_pci_mmio.cpp`: 16KB BAR2 MMIO emulation, register read/write callbacks, power sequence emulation, eFuse OTP simulation with MAC address (0xD0..0xD5), 8051 MCU download handshake emulation with checksum verification.
   - `mock_dma.hpp` / `mock_dma.cpp`: TX/RX descriptor ring memory buffers, 40-byte TX / 32-byte RX layouts, OWN bit arbitration, ring boundary wrap, queue doorbells.
   - `mock_packet_injector.hpp`: Inject synthetic 802.11 Beacons, Probe Responses, Auth/Assoc frames, and EAPOL-Key frames.
3. Implement Test Tiers in `tests/`:
   - `tier1_features/`: ≥5 tests per feature (firmware loader verification, eFuse parsing, DMA ring operations, 802.11 beacon parsing, WPA2 4-way handshake, CCMP crypto).
   - `tier2_boundaries/`: Boundary tests (zero length, max MTU 2304/1514, corrupted CRC/MIC, ring overflow, invalid eFuse header).
   - `tier3_pairwise/`: Pairwise interaction tests (scanning during active DMA, rekeying during packet bursts, antenna switching under load).
   - `tier4_workloads/`: Real-world end-to-end scenarios (full bring-up: power on -> firmware upload -> eFuse read -> channel scan -> association -> WPA2 handshake -> IP/ARP traffic flow).
4. Implement `tests/Makefile` and standalone test runner `tests/test_runner.cpp`.
5. Compile and run the test suite using `clang++ -std=c++17` on macOS. Ensure 100% of tests pass.
6. Write `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/TEST_READY.md` summarizing test runner commands, tier test counts, and feature checklist.
7. Write `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/test_writer_m2/handoff.md` with structured Observation, Logic Chain, Caveats, Conclusion, and Verification.
8. Send a completion message back to parent.
