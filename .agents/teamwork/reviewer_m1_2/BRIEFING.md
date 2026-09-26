# BRIEFING — 2026-09-26T16:45:25Z

## Mission
Conduct an independent technical review and adversarial challenge of `docs/DESIGN.md` focusing on host integration, staging safety, and baseband/RF parameters.

## 🔒 My Identity
- Archetype: reviewer_and_adversarial_critic
- Roles: reviewer, critic
- Working directory: /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2
- Original parent: 97aa8b74-6088-4e6c-8c61-400af4865bc2
- Milestone: M1
- Instance: 2 of 2

## 🔒 Key Constraints
- Review-only — do NOT modify implementation code
- Check for integrity violations (hardcoded test results, facade implementations, shortcuts, fabricated verification, self-certifying work)
- Adhere strictly to the communication guideline: send_message to parent for coordination, files for content delivery
- Do not write source/test files into .agents/teamwork

## Current Parent
- Conversation ID: 97aa8b74-6088-4e6c-8c61-400af4865bc2
- Updated: 2026-09-26T16:45:25Z

## Review Scope
- **Files to review**: `docs/DESIGN.md`
- **Reference documents**: `ORIGINAL_REQUEST.md`, `PROJECT.md`, `TEST_INFRA.md`
- **Review criteria**:
  1. Host & OpenCore Integration: PCI device matching (`0xB72310EC` at `RP06@1c0005/PXSX@0`), BAR2 MMIO mapping, MSI vector 8 configuration, staging plan on `/Volumes/HTOSH/EFI/OC` (including disabling `SSDT-Disable_Network_RP06.aml`).
  2. RF Tuning & Antenna Diversity: 3-wire LSSI serial RF access via BB 0x0840, 2.4 GHz channel tuning via RF 0x18, antenna switching at BB 0x092C (Main=1, Aux=2).
  3. Data Path & Network Integration: Ethernet II <-> RFC 1042 LLC/SNAP 802.11 QoS Data translation and IONetworkInterface registration.
  4. Adversarial stress-testing & failure modes.

## Key Decisions Made
- Confirmed zero integrity violations in `docs/DESIGN.md` and test harness code.
- Empirically verified target hardware endpoint `pci10ec,b723`, BAR2 MMIO base `0xf1100000`, MSI vector 8, and active `SSDT-Disable_Network_RP06.aml` in OpenCore.
- Documented 2 Major and 3 Minor implementation guardrails (independent replay counters for TK vs GTK, `IODeviceMemory` index 1 vs register 0x18, explicit MSI `intIndex = 1`, LSSI busy polling, MPDU fragment handling).
- Issued Verdict: **APPROVE**.

## Artifact Index
- `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2/DISPATCH.md` — Inbound task dispatch record
- `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2/BRIEFING.md` — Situational awareness memory
- `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2/progress.md` — Liveness and heartbeat tracker
- `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2/review.md` — Complete technical review & adversarial critique report
- `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/reviewer_m1_2/handoff.md` — 5-Component hard handoff report with verdict

## Review Checklist
- **Items reviewed**: `docs/DESIGN.md`, `ORIGINAL_REQUEST.md`, `PROJECT.md`, `TEST_INFRA.md`, `/Volumes/HTOSH/EFI/OC/config.plist`, IORegistry `ioreg -l`, `IOPCIDevice.h`, `IOFilterInterruptEventSource.h`.
- **Verdict**: APPROVE
- **Unverified claims**: None; all empirical claims checked against live host.

## Attack Surface
- **Hypotheses tested**:
  - EAPOL flood DoS -> Mitigated by workloop rate-limiting.
  - Antenna switch RF VSWR impedance spike during TX -> Mitigated by pausing TX queue.
  - Shared replay counter between TK and GTK -> Identified as Major Finding 1; mitigated by separate replay tracking.
  - BAR2 index mismatch (`mapDeviceMemoryWithIndex(2)` panic) -> Identified as Major Finding 2; mitigated by `mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2)`.
  - Default `intIndex = 0` binding to legacy pin -> Identified as Minor Finding 3; mitigated by explicit `intIndex = 1`.
- **Vulnerabilities found**: All residual risks documented with concrete mitigations.
- **Untested angles**: Live physical transmission (deferred to M6).
