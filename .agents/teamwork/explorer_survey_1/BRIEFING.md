# BRIEFING — 2026-09-26T16:25:40Z

## Mission
Authoritative survey of macOS Wi-Fi driver architectures and community prior art relevant to Realtek RTL8723BE on macOS 26.6.2 (Build 25G83, x86_64).

## 🔒 My Identity
- Archetype: Explorer
- Roles: Read-only investigation, architectural survey, synthesis
- Working directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_1
- Original parent: 97aa8b74-6088-4e6c-8c61-400af4865bc2
- Milestone: Survey & Architecture Selection (R1)

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Output reports to analysis.md and handoff.md in own directory
- Never touch source code or other agents' directories
- Adhere strictly to the 5-component handoff structure

## Current Parent
- Conversation ID: 97aa8b74-6088-4e6c-8c61-400af4865bc2
- Updated: 2026-09-26T16:25:40Z

## Investigation State
- **Explored paths**:
  - System environment (`macOS 26.6.2`, build `25G83`, Darwin `25.6.0`, x86_64)
  - PCIe hardware node `pci10ec,b723` at `2:0:0` (BAR2 MMIO `0xf1100000`, 16KB, MSI support)
  - System kernel collections (`BootKernelExtensions.kc`) and `IO80211Family.kext`
  - Xcode SDKs: DriverKit SDK (`NetworkingDriverKit.framework`, lack of WLAN classes) and macOS SDK (`Kernel.framework`, `IOKit/network/IOEthernetController.h`)
  - OpenIntelWireless (`itlwm` vs `AirportItlwm`, `HeliPort` IOUserClient IPC)
  - Realtek prior art (`chris1111/Wireless-USB-Adapter`, OpenBSD `rtwn`, Linux mainline `rtlwifi/rtl8723be`)
- **Key findings**:
  - `IOEthernetController` kext + user-space client is the only stable, viable architecture on modern macOS (macOS 14+ and 26.6.2).
  - DriverKit is unviable due to absence of public WLAN frameworks and inability of OpenCore to inject `.dext` bundles.
  - `AirportItlwm` / `IO80211Family` shimming is broken due to Skywalk migration and excision of legacy 802.11 kexts.
  - Linux `rtlwifi/rtl8723be` is the sole complete reference for RTL8723BE hardware bring-up, firmware loading, and antenna diversity.
- **Unexplored areas**: None within the survey scope; complete architectural blueprint delivered.

## Key Decisions Made
- Architecture Recommendation: `IOEthernetController` kext (`RTL8723BE.kext`) with custom `IOUserClient` (`RTL8723BEUserClient`) and user-space CLI/HeliPort client.
- Hardware Bring-up Source: Port Linux `rtlwifi/rtl8723be` + `rtl8723befw.bin`.
- Data Path: Ethernet II <-> RFC 1042 LLC/SNAP + 802.11 QoS Data with WPA2-CCMP crypto.

## Artifact Index
- DISPATCH.md — record of incoming tasks
- BRIEFING.md — persistent working memory
- progress.md — liveness heartbeat
- analysis.md — comprehensive technical findings
- handoff.md — structured handoff report
