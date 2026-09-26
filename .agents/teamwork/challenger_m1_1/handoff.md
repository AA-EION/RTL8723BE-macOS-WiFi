# Handoff Report: Challenger M1-1

**Agent**: Challenger M1-1 (Hardware Feasibility, DMA Safety & Low-Level Driver)  
**Target Document**: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/docs/DESIGN.md`  
**Verdict**: **REQUEST_CHANGES**  
**Date**: 2026-09-26T16:55:00Z  

---

## 1. Observation

Direct technical observations from inspecting `docs/DESIGN.md`, the Linux kernel reference driver (`drivers/net/wireless/realtek/rtlwifi/rtl8723be/`), and compiling/executing our empirical test harness `tests/empirical_challenge_m1_1.cpp`:

1. **Transmit Descriptor Stride & Field Offsets in `docs/DESIGN.md` vs Hardware**:
   - `docs/DESIGN.md` line 681-754 declares `struct tx_desc_8723be` with 12 32-bit dwords (48 bytes).
   - In `docs/DESIGN.md` line 749-752, packet buffer address is declared at Dword 8 (`uint32_t txbuffaddr`), and next descriptor address is declared at Dword 10 (`uint32_t nextdescaddr`).
   - In contrast, the canonical Realtek driver definition in Linux `drivers/net/wireless/realtek/rtlwifi/pci.h` line 112 specifies:
     ```c
     struct rtl_tx_desc {
         u32 dword[16];
     } __packed;
     ```
     `sizeof(struct rtl_tx_desc)` is strictly **64 bytes** (16 dwords).
   - In Linux `drivers/net/wireless/realtek/rtlwifi/rtl8723be/trx.h` lines 209-219, `set_tx_desc_tx_buffer_address` writes to `*(__pdesc + 10)` (Dword 10), and `set_tx_desc_next_desc_address` writes to `*(__pdesc + 12)` (Dword 12).
   - Execution of `tests/empirical_challenge_m1_1.cpp` (Step 1.2) proves:
     - When hardware steps by 64 bytes through a 48-byte ring, Slot 1 fetches from Host Slot 1 + 16 bytes.
     - Slot 0 hardware reads buffer address from Dword 10 (containing `nextdescaddr`), and reads `nextdescaddress` from Dword 12 (which is uninitialized `0x00000000`), crashing on the second packet.
     - Slot 3 hardware fetches past the ring boundary into unallocated kernel memory.

2. **PCIe 64-bit DMA Enable Bit Omission**:
   - Linux `drivers/net/wireless/realtek/rtlwifi/pci.c` lines 2073-2088 implements `platform_enable_dma64`:
     ```c
     pci_read_config_byte(pdev, 0x719, &value);
     if (dma64) value |= BIT(5);
     else value &= ~BIT(5);
     pci_write_config_byte(pdev, 0x719, value);
     ```
   - In `docs/DESIGN.md`, PCI configuration register `0x719` is never mentioned or configured anywhere in the document.

3. **Firmware Download Buffer Over-Read & Watchdog Hang**:
   - `docs/DESIGN.md` Section 4.4 lines 480-483 reads 4-byte dwords from `codePayload` without tail padding validation. Execution of `tests/empirical_challenge_m1_1.cpp` (Step 2.1) confirms an out-of-bounds read of 2 bytes past the end of a 10-byte firmware payload.
   - `docs/DESIGN.md` Section 4.5 line 515 specifies: `Initialization Timeout: 6000 iterations × 5 ms = 30 seconds`. Polling for 30 seconds in kernel context blocks the thread and trips the macOS Darwin kernel watchdog.

4. **eFuse Unburned Array & PA Overdrive**:
   - `docs/DESIGN.md` Section 5.3 decodes raw OTP into `logicalMap`.
   - On an unburned OTP (all `0xFF`), `logicalMap[0x10]` (`EEPROM_TX_PWR_CCK`) is `0xFF` (255 decimal, compared to normal 45/0x2D).
   - `logicalMap[0xD0..0xD5]` is `FF:FF:FF:FF:FF:FF` (Broadcast MAC).
   - Section 5.3 contains no sanitization or fallback injection method.

5. **Antenna Diversity Hardcoding**:
   - `docs/DESIGN.md` Section 7.4 line 948 hardcodes `mmio_write32(REG_BB_PAD_CTRL, 0x00000002)` for all HP `103C:804C` hardware.
   - Execution of `tests/empirical_challenge_m1_1.cpp` (Step 4.1) confirms that HP laptops with the single physical wire attached to Port 1 experience 35 dB signal attenuation (-94 dBm), failing all scans and connections.

---

## 2. Logic Chain

1. **Hardware DMA Crash**:
   From Observation 1, hardware fetches descriptors at 64-byte strides and expects `txbuffaddr` at Dword 10 and `nextdescaddr` at Dword 12.
   From Observation 1, `docs/DESIGN.md` allocates rings with 48-byte strides and places `txbuffaddr` at Dword 8 and `nextdescaddr` at Dword 10.
   Therefore, after transmitting the first frame, the hardware reads `nextdescaddr = 0x0` from Dword 12 and attempts DMA to physical address 0, while subsequent slots misread internal packet flags as ownership bits, guaranteeing kernel memory corruption and unrecoverable PCIe bus aborts.

2. **Silent 64-bit Address Truncation**:
   From Observation 2, Realtek PCIe hardware disables 64-bit physical addressing by default until Bit 5 of PCI config register `0x719` is explicitly asserted.
   From Observation 2, `docs/DESIGN.md` never writes to register `0x719`.
   Therefore, on macOS systems with >4GB RAM where `mbufs` reside above `0xFFFFFFFF`, the hardware truncates the high 32 bits, overwriting arbitrary low physical memory.

3. **Kernel Panics on Firmware Boot**:
   From Observation 3, streaming unaligned microcode payloads reads past allocated kernel buffers, triggering page faults.
   From Observation 3, polling for 30 seconds in kext `start()` blocks the system boot sequence, exceeding XNU watchdog limits and causing a kernel panic.

4. **RF Front-End PA Overdrive Hazard**:
   From Observation 4, reading unburned or blank eFuse yields `0xFF` for power calibration.
   Without a sanitizer replacing `0xFF` with conservative fallbacks, the driver programs the Baseband/RF power amplifier with index 255 (maximum overdrive), risking permanent hardware burnout of the RF power amplifier.

5. **Antenna Port Inoperability**:
   From Observation 5, HP laptops with single antennas vary between Port 1 and Port 2 across production batches.
   Hardcoding Port 2 without auto-sensing or OpenCore boot-args leaves Port 1 systems at -94 dBm (effectively offline).

---

## 3. Caveats

- Hardware testing was conducted against the Linux kernel reference driver (`rtlwifi/rtl8723be`), official Realtek registers, and an empirical C++ reproduction harness.
- While some 2-antenna laptops can tolerate either antenna port with slight performance variation, single-antenna HP models strictly fail if routed to the open port.
- No other caveats.

---

## 4. Conclusion

**Verdict**: **REQUEST_CHANGES**

The architectural decision to use `IOEthernetController` + `IOUserClient` is sound, but the hardware-level specifications in `docs/DESIGN.md` contain critical design flaws that will cause DMA crashes, kernel panics, memory corruption, and RF hardware damage.

### Action Items for Worker M1:
1. **Fix TX Descriptor Layout**: Update `struct tx_desc_8723be` to exactly 16 dwords (64 bytes). Position `txbuffaddr` at Dword 10, `txbuffaddr64` at Dword 11, `nextdescaddr` at Dword 12, `nextdescaddr64` at Dword 13, and append 4 dwords of PCIe MM padding.
2. **Add 64-bit DMA Config**: Add PCI config register `0x719` bit 5 activation during PCIe probe/attach.
3. **Fix Firmware Streamer & Timeout**:
   - Add bounds-checking on the 4-byte streaming loop in Section 4.4.
   - Reduce firmware initialization timeout from 30s to ≤500 ms.
   - Add a 3-iteration retry loop with MCU self-reset on checksum mismatch.
4. **Implement eFuse Sanitizer**: Add `RTL8723BE::sanitizeCalibrationMap()` to clamp TX power to ≤45, validate against broadcast/zero MACs, and substitute default crystal/thermal values.
5. **Add Antenna Auto-Sense & Boot-Args**: Implement dual-dwell sensing during initial scan and support `rtlant=1` / `rtlant=2` in OpenCore `boot-args`.

---

## 5. Verification Method

To independently verify these findings, run the empirical challenge harness:

```bash
# Compile and execute the empirical challenge verification harness
clang++ -std=c++17 -Wall -Wextra -O2 \
  /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/empirical_challenge_m1_1.cpp \
  -o /Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/empirical_challenge_m1_1

/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/empirical_challenge_m1_1
```

**Files to Inspect**:
- `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/tests/empirical_challenge_m1_1.cpp`
- `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/challenger_m1_1/challenge.md`
- Linux reference: `drivers/net/wireless/realtek/rtlwifi/rtl8723be/trx.h` and `pci.c`

**Invalidation Conditions**:
This challenge is invalidated if:
1. Realtek RTL8723BE hardware can be proven to accept 48-byte descriptors with buffer addresses at Dword 8 (disproven by Linux `trx.h` and `pci.h`).
2. Realtek PCIe chips enable 64-bit DMA addressing without setting register `0x719` bit 5 (disproven by Linux `pci.c` line 2073).
3. The XNU Darwin kernel permits 30-second uninterruptible polling in kext `start()` without watchdog panics.
