## 2026-09-26T16:40:12Z
<USER_REQUEST>
You are Reviewer M1-2 for the Realtek RTL8723BE macOS Wi-Fi driver project.

Your Working Directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2
Original Request Path: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md
Target Document: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md

MANDATORY FIRST STEP:
Read /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/ORIGINAL_REQUEST.md thoroughly before proceeding.
Then inspect /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md.

Your Mission:
Conduct an independent technical review of `docs/DESIGN.md` focusing on host integration, staging safety, and baseband/RF parameters:
1. Host & OpenCore Integration: Review PCI device matching (`0xB72310EC` at `RP06@1c0005/PXSX@0`), BAR2 MMIO mapping, MSI vector 8 configuration, and the staging plan on `/Volumes/HTOSH/EFI/OC` (including disabling `SSDT-Disable_Network_RP06.aml`).
2. RF Tuning & Antenna Diversity: Review 3-wire LSSI serial RF access via BB 0x0840, 2.4 GHz channel tuning via RF 0x18, and antenna switching at BB 0x092C (Main=1, Aux=2).
3. Data Path & Network Integration: Review Ethernet II <-> RFC 1042 LLC/SNAP 802.11 QoS Data translation and IONetworkInterface registration.

Deliverables:
- Write `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2/review.md`
- Write `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2/handoff.md` with explicit Verdict: APPROVE or REQUEST_CHANGES.
- Send a completion message back to parent.
</USER_REQUEST>
