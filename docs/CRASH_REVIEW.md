# Crash review — 2026-09-26

Review baseline: `982d2de`. No matching panic report was found in the standard
system/user DiagnosticReports directories. Shutdown-stall reports exist but are
not proof of this driver's crash. Findings below are source-confirmed defects;
identifying the exact observed crash requires a panic backtrace or last boot log.

User reports running `sudo ./scripts/stage_opencore.sh --load`, approving the
kext, rebooting successfully to login, then a complete freeze after entering the
password. This timing is consistent with interface activation, but does not
prove which driver path stalled.

## Critical findings

1. **TX DMA layout is wrong.** `TxDesc40` puts the data pointer at byte 32,
   allocates only 40 bytes per entry, and has no circular next-descriptor pointer.
   RTL8723BE uses a 40-byte control header, data pointer at byte 40 and next pointer
   at byte 48. Linux allocates a larger PCI descriptor slot and explicitly links
   every entry. Once TX is polled, the original driver lets hardware interpret the
   following entry's control words as DMA addresses. This can cause DMA faults,
   invalid memory access, or a device/bus hang.
2. **DMA enabled before valid rings; never revoked before free.** Startup enables
   PCI bus mastering and MAC/DMA before allocating/programming descriptors.
   Cleanup releases DMA memory without clearing PCI bus mastering. Failed starts
   and unloads can leave a device accessing invalid or released memory.
3. **Failed initialization is ignored.** A failed MMIO probe, firmware download,
   or LLT initialization does not abort startup. The embedded firmware has
   signature `0x5301`, version 15; the old startup log claimed version 36.
4. **Deployment makes the defect persistent.** The former staging script mounted
   disk0s1 and replaced any matching mounted EFI automatically, then enabled the
   kext at every boot. Even `--load` also modified EFI first. This explains how a
   bad experimental build can require a separate boot EFI to recover.
5. **Tests do not exercise the kernel driver.** `tests/mock/` implements a second
   driver and second descriptor definitions. It wraps TX by arithmetic instead
   of following PCI descriptor links, hiding the layout defect. Passing these
   tests is not evidence of boot safety.

## Containment

Hardware startup now requires an explicit experimental boot argument, checked
before the superclass starts or PCI registers are touched. Staging writes only
inside the repository and emits a disabled OpenCore entry. No live kext load,
unload, EFI mount/edit or reboot was performed during this review. Previously
published binaries do not acquire these changes automatically.

## Sources

- [Linux v6.12 RTL8723BE descriptor accessors](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/realtek/rtlwifi/rtl8723be/trx.h)
- [Linux v6.12 PCI TX ring allocation/linking](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/realtek/rtlwifi/pci.c)
- [Linux v6.12 RTL8723BE register map](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/realtek/rtlwifi/rtl8723be/reg.h)

## Source fixes and validation

The PCI TX slot is now 64 bytes with a 40-byte control header, buffer pointer at
40, next pointer at 48, circular links, buffer length, and distinct queue-selector
values. RX accounts for the PHY driver-info prefix. DMA allocation checks prepare
success, virtual address, alignment and the entire 32-bit physical range. Both
interrupt banks are masked during startup/shutdown. PCI bus mastering stays off
until rings are programmed and is cleared before their release. Startup now
aborts on power/firmware/LLT failures, discovers PCI capability/interrupt offsets,
and checks the BAR length and lock allocations. LLT writes poll completion and
close the reserved-page list. Incorrect MULTI_FUNC_CTRL/HWSEQ_CTRL offsets are
corrected to 0x68/0x423.

`make test-hardware` tests the production header with AddressSanitizer and
UndefinedBehaviorSanitizer: raw byte offsets, hardware-style next-link traversal,
queue selectors, RX layout and DMA address boundary checks. It does not run the
IOKit lifecycle or validate physical hardware. The legacy simulation remains
separate and still cannot establish hardware safety.

## Remaining release blockers

- No captured panic/backtrace proves the exact login freeze. No live hardware
  test has been attempted, and no fix here should be called hardware-verified.
- User-client calls, network output, scan RX polling and interrupt RX processing
  are not serialized on a common command gate; stopping the device may race
  clients/output. The scan spins for 13 x 40 ms in IODelay and directly competes
  with the interrupt consumer. These paths need redesign and lifecycle tests.
- Power sequencing, LLT buffer-boundary setup, PHY/RF calibration, firmware
  startup, PCI DMA mapping through IODMACommand/IOMMU, sleep/wake and full hardware
  DMA-quiescence remain to be validated. Clearing bus mastering is necessary;
  a short delay alone is not proof that all outstanding transactions drained.
- WPA2 has synthetic nonces and other incomplete protocol handling. The project
  is not ready for normal network use even if booting becomes stable.
- Historical bundled apps/DMGs and EFI copies contain old binaries. Rebuilding
  source does not update the user's installed kext or the published release.

## Additional production-code memory review

The EAPOL GTK handler supplied a 64-byte stack buffer to `aes_key_unwrap`, which
could copy up to 128 bytes without knowing the destination capacity. The API now
requires capacity, and the handler passes `sizeof(unwrapped)`. A separate
production test exercises a valid wrapped 128-byte payload against the 64-byte
limit under ASan/UBSan.

While adding the test, the **unmodified production AES decrypt failed the RFC
3394 section 4.1 known-answer vector**, despite all 57 simulator tests passing.
It mixed inverse-transformed round keys before InvMixColumns, effectively mixing
them twice. Decrypt now uses the original round keys in the standard inverse
cipher order. Both wrap and unwrap match the published vector. This is a
separate protocol defect, not evidence that WPA2 caused the reported login
freeze. The stack-overwrite finding is a source-level capacity defect; the old
broken decrypt rejected the standard vector before reaching its output copy.

## Verification results

- Forced source rebuild of the kext; ad-hoc signature checked. SDK header
  override warnings remain; no source compile errors.
- Production descriptor/ring contract: passed with ASan/UBSan.
- Production crypto known-answer and capacity tests: passed with ASan/UBSan.
- Legacy simulator: 57/57 passed (limited coverage as described above).
- Staging script: shell syntax valid; `--load` rejects with exit status 2 before
  building, mounting EFI, requesting privilege, or attempting a kernel load.
- `kmutil print-diagnostics` resolves dependencies (`Dependencies: OK`), but
  rejects load authentication for this user-owned, ad-hoc-signed development
  bundle (ownership and signature errors). This is not a successful load check;
  no ownership changes or signing-policy changes were made.
- No installation or live hardware validation performed.

Crypto oracle: [RFC 3394 section 4.1](https://www.rfc-editor.org/rfc/rfc3394.html#section-4.1).
