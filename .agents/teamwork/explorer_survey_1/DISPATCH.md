# Dispatch: Explorer Survey 1

**Working Directory**: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_1
**Task**: Survey macOS Wi-Fi driver architectures & community prior art (itlwm, IOEthernetController, DriverKit, IO80211Family) for macOS 26.6.2 (25G83).

## 2026-09-26T16:17:09Z
You are an Explorer subagent for the Realtek RTL8723BE macOS Wi-Fi driver project.

Your Working Directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_1
Original Request Path: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md

MANDATORY FIRST STEP:
Read /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md thoroughly before proceeding.

Your Mission:
Conduct an authoritative survey of macOS Wi-Fi driver architectures and community prior art relevant to Realtek RTL8723BE on macOS 26.6.2 (Build 25G83, x86_64).

Key Investigation Points:
1. Investigate existing open-source macOS Wi-Fi projects and community ports:
   - itlwm / AirportItlwm (IOEthernetController vs IO80211Family shim)
   - Realtek community kexts (e.g. chris1111 Wireless-USB-Adapter, legacy Realtek PCIe attempts, OpenBSD rtwn ports)
   - DriverKit (PCIDriverKit, NetworkingDriverKit) vs Kernel Extension (kext) on macOS 26.6.2: evaluate system integrity, entitlements, dtrace/kext permissions, and OpenCore kext injection compatibility.
2. Evaluate why IOEthernetController + user-space daemon (HeliPort/CLI) is the gold standard for non-Apple Wi-Fi on modern macOS:
   - Private Apple80211 KPIs in macOS 11+ and macOS 15/26.
   - Skywalk / CoreWLAN / IO80211Family constraints.
   - Network interface abstraction (IONetworkInterface, IOEthernetController, medium dictionary, MTU, packet queuing, outputPacket).
3. Specify the architecture recommendation for RTL8723BE on macOS 26.6.2:
   - Kernel module vs user-space component responsibilities.
   - Control path (IOUserClient / IOCTL / sysctl / UNIX domain socket / BSD socket) for scan triggering, AP list querying, BSSID/SSID selection, and WPA2-PSK handshake parameters.
   - Data path: Ethernet frame encapsulation/decapsulation to/from 802.11 data frames (LLC/SNAP header handling, 802.11 QoS data header, CCMP IV and MIC).

Deliverables:
- Write your comprehensive findings to `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_1/analysis.md`
- Write `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_1/handoff.md` with structured Observation, Logic Chain, Caveats, Conclusion, and Verification.
- Send a completion message back to parent.
