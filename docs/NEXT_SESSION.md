# Continuation prompt

Paste the following into a new session opened in this repository:

```text
Continue the pending RTL8723BE macOS Wi-Fi/Bluetooth work in this repo. Read
docs/AUDIO_INCIDENT.md, docs/VALIDATION.md, docs/FEATURES.md, docs/BLUETOOTH.md,
docs/UPSTREAM.md and docs/upstream-sources.json before making changes. This is
the experimental 1.4.0 snapshot, not a complete or stable release.

FIRST restore my internal audio. After the Bluetooth approval reboot my speakers
made loud uncontrollable noise. The previous agent unloaded VoodooHDA 3.0.3 and
restarted coreaudiod; I confirmed the noise continued. It then attempted to load
a temporary copy of the installed VoodooHDA with DisableInputMonitor=true,
VoodooHDAEnableMuteFix=true, VoodooHDAEnableVolumeChangeFix=true, PCM=0 and Rec=0.
I rebooted during that attempt. The noise stopped, but internal speakers are now
missing. Its load completion was not captured. Do not assume that temporary copy
loaded successfully or caused the original noise.

The latest checks found VoodooHDA and both RTL kexts NOT loaded, no audio engines
and no system audio devices. Installed /Library/Extensions/VoodooHDA.kext files
were not overwritten and retain April 3 timestamps. Installed/recovery executable
UUID and __TEXT/__DATA hashes match; recovery ad-hoc signing changes its whole
file hash. The exact commands, settings, hashes, prior PCM/codec errors and all
known uncertainties are in docs/AUDIO_INCIDENT.md. Local ignored evidence remains
in build/VoodooHDA-audio-recovery.kext, build/stop_rtl_and_reset_audio.sh and
.references/VoodooHDA (upstream commit recorded in that document). Do NOT blindly
rerun that helper. Do not load either radio while diagnosing audio.

Backtrack current versus staged audio bundles, loaded and on-disk kernel
collections, policy/approval state and the actual HDA provider. Find why VoodooHDA
stayed unloaded after reboot, then restore speakers/mute/volume with a reviewed
recovery using actual source. Read audio-related EFI backup differences if needed,
but never print or publish machine identifiers. No EFI/NVRAM/USB-map/boot-argument
edits were made in this work. Coordinate live audio loads or reboots with me given
the actual prior noise incident. Don't guess codec/layout/MMIO writes. Silence
without an enumerated, functional speaker device is not a successful repair.

Then finish the originally requested real Linux/BSD-based feature port: full
802.11b/g/n Wi-Fi 4 behavior, WPA2, per-network password saving/auto-join in the
app, justified CPU/radio offloads and performance/QOL, a real native AirPort
adapter if feasible, plus a separate same-repo/same-installer Bluetooth driver
using native macOS controls where possible. Existing Wi-Fi is IOEthernetController;
do not pretend changing PCI identity makes it a native Apple Wi-Fi card.

Implemented so far: production WPA2/RSN rekey/replay handling; HT20 MCS0–7/WMM/SGI;
RX A-MSDU validation; firmware RA/RSSI; optional pairwise hardware TX CCMP;
timeouts/TX pressure/sleep-wake code; app Keychain credentials, auto-join and
radio settings; separate RTL8723B HCI firmware uploader and Bluetooth app UI.
Both radio kexts ship inside the app and load manually after login, never at boot.

Actual Wi-Fi 1.4.0 hardware startup, WPA2 join/DHCP, password reuse after app
relaunch, software/hardware TX CCMP traffic and scan while connected passed.
Limited 4-MiB internet tests were ~5.70 and ~6.72 Mb/s; they prove traffic, not
peak rate or CPU gains. The tested card is PCI 10ec:b723, Bluetooth USB 0bda:b006,
one antenna selected AUX/S0. See the validation record for evidence and limits.

Bluetooth firmware upload/native attachment have NOT passed hardware testing.
Its IOUSBHostFamily minimum was fixed to 1.0.1; the subsequent load was approval
blocked. Approval reboot preceded the audio incident. Lilu 1.7.3 was present,
BlueToolFixup absent. Verify actual native-stack requirements and internal USB
mapping first. The manual BT helper opts in only its temporary signed copy via
FirmwareUploadEnabled; no Bluetooth boot flag change is needed. Full dynamic
Linux Bluetooth coexistence is not ported; only an optional single-antenna PTA
baseline exists.

Still missing: Block Ack agreements/reorder, TX A-MPDU, HT40/coexistence/channel
policy, fragmented MSDUs, native IO80211/AirPort integration, full BT coexistence,
and broad interop/performance/power validation. ADDBA is explicitly refused and
fragments dropped until supported. Test real rekeys, b-only/g-only APs, AP loss,
saturated traffic, credential replace/forget, CPU savings and sleep/wake. Never
advertise capabilities that aren't implemented and validated.

Use actual pinned Linux v6.12 and OpenBSD code, not guesses. All 28 files in
docs/upstream-sources.json were hash verified and cached in .references/; run
python3 scripts/fetch_upstream.py to verify/refetch. Existing tests compile real
production hardware/protocol/crypto/firmware code under ASan/UBSan. make test
passed, including two boot-config tests and the 57-test legacy mock suite; mock
tests are not hardware proof. The app, both kexts, CLI, package and DMG were built
and signature/DMG checked. No audio recovery artifact belongs in the installer.

Keep docs and validation accurate, preserve my existing unrelated changes, and
continue through implementation and meaningful tests. Restore audio first.
```
