# Handoff Report: Challenger M1-2

**Author**: Challenger M1-2 (critic, specialist)  
**Date**: September 26, 2026  
**Target Document**: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md`  
**Working Directory**: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/challenger_m1_2`  
**Verdict**: **REQUEST_CHANGES**

---

## 1. Observation

Direct observations from `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md` and verification code:

1. **Inbound Pipeline Order (§ 8.6, Lines 1077–1085)**:
   > *"When an 802.11 Data frame arrives via RX DMA:*  
   > *1. Verify CCMP MIC and decrypt payload using TK (or GTK for broadcast/multicast).*  
   > *2. Validate LLC/SNAP header: check if DSAP == 0xAA && SSAP == 0xAA && Control == 0x03 && OUI == 0x000000.*  
   > *3. Extract original 16-bit EtherType:*  
   > *- If EtherType == 0x888E (EAPOL): Intercept immediately..."*  
   Observation: Plaintext EAPOL-Key frames (Message 1 and Message 3) arrive before TK is derived/installed. Passing plaintext EAPOL frames to CCMP decryption results in decryption/MIC failure (`icverror = 1`), discarding the frame.

2. **WPA2 Message 3 Processing (§ 8.3, Lines 1017–1022)**:
   > *`|---- 3. EAPOL-Key Msg 3 (ANonce, Replay C2=C1+1, GTK, MIC) ---->|`*  
   > *`|<--- 4. EAPOL-Key Msg 4 (Replay C2, MIC) ----------------------| Install TK & GTK`*  
   Observation: No guard exists against retransmitted Message 3. Reinstalling TK upon receiving retransmitted Message 3 and resetting `tx_pn = 1` causes keystream reuse (KRACK - CVE-2017-13077 / CVE-2017-13078).

3. **CCMP Replay Protection Formulation (§ 8.5, Lines 1044–1046)**:
   > *- **Replay Protection**: The receiver verifies that the received $PN > \text{last\_accepted\_}PN$. If $PN \le \text{last\_accepted\_}PN$, the packet is discarded as a replay attack.*  
   Observation: A single scalar `last_accepted_PN` is shared across all incoming packets. In 802.11 QoS traffic, higher priority access categories (`AC_VO`) arrive ahead of lower priority categories (`AC_BE`), causing valid lower-priority frames to be dropped as false replays. Furthermore, broadcast frames encrypted with GTK (e.g. ARP, DHCP, mDNS) have an independent sequence number; checking GTK against unicast TK PN drops all broadcast/multicast frames.

4. **Deauth Handling & TX DMA Queues (§ 8.2, Lines 977–1002; § 6.5, Lines 813–820)**:
   > *Transition to DISCONNECTED.*  
   Observation: No TX DMA ring flushing, descriptor reclamation, or `setLinkStatus(kIONetworkLinkInactive)` is specified. Descriptors with `own = 1` remain in hardware rings, causing hardware to transmit stale frames with zeroed keys or in plaintext over the air.

5. **Off-Channel Scanning Link Disruption (§ 7.3, Lines 887–917; § 8.1, Lines 959–973)**:
   > *`setChannel(channel)` writes directly to RF register `0x18` while `startScan()` dwells 60 ms on channels 1–13.*  
   Observation: No 802.11 Null Data frame with Power Management bit ($PM=1$) is sent to the AP prior to leaving the operating channel. The AP drops unacknowledged unicast frames and disassociates the station.

6. **Empirical Execution Command & Output**:
   Command:
   ```sh
   clang++ -std=c++17 -I./tests/mock tests/empirical_challenge_m1_2.cpp tests/mock/mock_crypto.cpp -o /tmp/empirical_challenge_m1_2 && /tmp/empirical_challenge_m1_2
   ```
   Result:
   - Test 1 (Inbound EAPOL Decryption): `FAILED (as expected)` — CCMP decryption rejected plaintext EAPOL frame.
   - Test 2 (QoS EAPOL Header Offset): Offset 24 failed to match LLC in QoS Data; matched at offset 26.
   - Test 3 (Replay Counter Validation): Replayed counter 50 rejected under strict monotonic check.
   - Test 4 (KRACK Nonce Reuse): `Ciphertext XOR matches Plaintext XOR: TRUE (FATAL)`. Keystream reuse confirmed.
   - Test 5 (CCMP Multi-TID / Broadcast Replay): Out-of-order QoS Best-Effort ($PN=195$ after Voice $PN=200$) dropped; Broadcast ARP ($PN=15$ after Unicast $PN=5000$) dropped.
   - Test 6 (Deauth TX Queue Flush): Descriptor `own` remained 1 after deauth; RF plaintext leak confirmed.
   - Test 7 (Off-Channel Scan): AP unicast traffic lost without 802.11 PM=1 signaling.

---

## 2. Logic Chain

1. From Observation 1, because EAPOL frames are unencrypted on the wire during initial association, executing CCMP decryption prior to LLC/SNAP EtherType extraction guarantees that CCMP MIC verification fails and drops the frame. Therefore, the 4-way handshake cannot complete, and the driver cannot associate.
2. From Observation 2, because retransmitted Message 3 is an inherent characteristic of lossy wireless media, reinstalling the TK upon receiving a retransmitted Message 3 resets the transmit PN counter. Encrypting subsequent frames with the same key and previously used PN values results in identical keystream generation ($C_1 \oplus C_2 = P_1 \oplus P_2$), allowing an attacker to decrypt traffic.
3. From Observation 3, IEEE 802.11 QoS scheduling guarantees that packets from different access categories arrive out of strictly sequential PN order, and broadcast traffic uses an independent AP-side counter. Enforcing a single global scalar `last_accepted_PN` guarantees that valid Best-Effort frames and all GTK broadcast frames (ARP, DHCP, Bonjour) are dropped as false replay attacks.
4. From Observation 4, the RTL8723BE PCIe DMA engine operates autonomously once descriptors are armed with `own = 1`. Tearing down security keys without halting DMA and reclaiming descriptors guarantees that in-flight frames are transmitted over the air either unencrypted or encrypted with all-zero keys.
5. From Observation 5, tuning the RF frequency away from the operating channel for 780 ms without commanding the AP to buffer packets via an 802.11 Power Management Null-Data frame guarantees packet loss, TCP timeout, and AP disassociation.
6. Combining Steps 1–5 leads directly to the conclusion that `docs/DESIGN.md` cannot be implemented safely without revisions to these protocol and data path sections.

---

## 3. Caveats

- **RF and Baseband Tables**: The physical layer register initialization tables (`RTL8723BEMAC_1T_ARRAY`, `RTL8723BEPHY_REG_1TARRAY`, etc.) were not bit-by-bit verified against physical silicon register behavior, as live hardware staging is scheduled for Milestone 6.
- **Hardware Crypto Offload**: `docs/DESIGN.md` mentions that hardware CCMP encryption can be offloaded via `REG_SECCFG` and CAM registers (`REG_CAMCMD`, `REG_CAMWRITE`). This review evaluated both software CCMP and the architectural interface to hardware crypto; if hardware CAM crypto is used, the driver must still ensure the hardware CAM preserves separate replay counters per TID and per GTK.

---

## 4. Conclusion

**Verdict**: **REQUEST_CHANGES**

`docs/DESIGN.md` is technically solid in its hardware architecture, PCIe MMIO mapping, 8051 firmware protocol, and eFuse calibration specifications. However, it must be revised to resolve the following blocking issues before driver implementation (Milestone 3/4) begins:

1. **Fix Inbound Pipeline (§ 8.6)**: Invert the RX pipeline to inspect the `Protected Frame` bit and LLC/SNAP header (handling both 24-byte and 26-byte QoS headers) to intercept plaintext EAPOL frames *before* CCMP decryption. Drop unsolicited plaintext data frames when in `CONNECTED` state.
2. **Mitigate KRACK (§ 8.3)**: Implement `fKeyInstalled` tracking. Retransmitted Message 3 must only retransmit Message 4; it must never reinstall TK or reset transmit PN counters.
3. **Enforce Monotonic EAPOL Replay Counters (§ 8.3)**: Track `fLastReplayCounter`; discard EAPOL frames with stagnant or decrementing counters. Reuse SNonce if valid Message 1 is retransmitted.
4. **Implement Per-TID and Per-GTK Replay Counters (§ 8.5)**: Replace scalar `last_accepted_PN` with `fRxPtkPN[16]` and `fRxGtkPN[4]`.
5. **Implement Mandatory AAD Masking (§ 8.5)**: Apply IEEE 802.11i bitmasks to Frame Control (mask Retry, PwrMgt, MoreData to 0), Sequence Control (mask Sequence Number to 0), and QoS Control before hashing. Map QoS TID into Byte 0 of the 13-byte Nonce.
6. **Specify Deauth/Disassoc TX Queue Teardown (§ 8.2, § 6.5)**: Halt TX DMA, iterate through rings to reclaim owned descriptors (`own = 0`), free pending `mbuf`s via `freePacket()`, call `setLinkStatus(kIONetworkLinkInactive)`, and only then clear keys.
7. **Specify 802.11 Power Management Signaling for Background Scans (§ 7.3, § 8.1)**: Transmit 802.11 Null Data with $PM=1$ before channel hop, pause TX queue, and transmit Null Data with $PM=0$ upon return.
8. **Filter Outbound User-Space EAPOL (§ 8.6)**: Drop raw Ethernet frames with EtherType `0x888E` in `outputPacket`.

---

## 5. Verification Method

To independently verify all findings and reproduce the failure modes:

1. **Run the Empirical Verification Harness**:
   ```sh
   clang++ -std=c++17 -I/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/mock \
     /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/empirical_challenge_m1_2.cpp \
     /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/mock/mock_crypto.cpp \
     -o /tmp/empirical_challenge_m1_2 && /tmp/empirical_challenge_m1_2
   ```
2. **Inspect Code and Design Files**:
   - `docs/DESIGN.md`: Check lines 1004–1025 (§ 8.3), 1041–1054 (§ 8.5), and 1075–1087 (§ 8.6).
   - `tests/empirical_challenge_m1_2.cpp`: Review the 7 standalone test implementations and assertions.
3. **Invalidation Conditions**:
   This challenge is invalidated if `docs/DESIGN.md` is updated with:
   - Plaintext EAPOL interception prior to CCMP decryption with 26-byte QoS header support.
   - Protection against TK reinstallation and TX PN reset on retransmitted Message 3.
   - Per-TID `RxPN[16]` and per-GTK `RxGTK_PN[4]` replay counters.
   - TX DMA halting, descriptor reclamation, and `setLinkStatus(Inactive)` on deauthentication.
   - 802.11 Null Data Power Management signaling for channel scanning.
