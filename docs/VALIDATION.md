# 1.4.0 validation record — 2026-10-12

This record distinguishes a passing build from a working radio. The full requested
feature set is still incomplete; see FEATURES.md for the implementation gaps.

## Passed on this host

- Both x86_64 kexts built against the macOS 26.5 SDK and passed ad-hoc signature
  verification. The app and CLI built; the signed app bundle verified.
- Production descriptor/DMA/eFuse/table contract tests passed under ASan/UBSan.
- Production crypto tests passed, including CCMP, RFC3394 and buffer bounds.
- The actual production WPA2/RSN/HT/WMM/A-MSDU code passed protocol tests,
  including handshake, retransmission, rekey, replay and malformed input.
- The actual Bluetooth firmware passed parser, truncation, malformed metadata
  and Linux download-fragment contract tests. The pinned firmware hash matched.
- Credential format checks passed. An isolated real login-Keychain item passed
  save/read/update/delete and was removed afterward. Existing app credentials
  were not queried or changed by this test.
- Boot configuration checks passed. The older simulated driver suite passed
  57/57 tests; that simulator is not hardware evidence.
- Plist and shell syntax checks and `git diff --check` passed.
- The installer package was assembled and the DMG passed `hdiutil verify`.
  Ad-hoc signing does not constitute notarization or installed kext approval.

## Live hardware attempt

PCI Wi-Fi `10ec:b723` and USB Bluetooth `0bda:b006` were observed on this host.
The Wi-Fi test flag `rtl8723be_experimental=1` was present. The separate
`rtl8723be_bluetooth=1` flag was absent.

The previous Wi-Fi 1.3.0 instance was initially loaded and disconnected. A
manual administrator-authorized replacement attempt unloaded it, but macOS
refused to load 1.4.0:

```text
Extension with identifiers com.rtl8723be.macos.wifi not approved to load.
Please approve using System Settings. (27)
```

Afterward, `kmutil showloaded` had no RTL driver and the CLI reported no kernel
service. The user approved the extension in System Settings and restarted.
No EFI, NVRAM, security policy or boot argument was changed by this work.

## Passed after that restart

- The manual Wi-Fi load succeeded. Kernel messages identify **1.4.0**, firmware
  v36.0 and initialization stage 11 (ready); this is actual device startup.
- The first scan found 30 APs. Antenna calibration selected AUX/S0 and the
  single-antenna profile, despite the initial two-antenna eFuse indication.
- The user joined the test AP through the app. The real exchange negotiated
  HT20 MCS0–7, WMM, short GI and firmware rate control on channel 4.
- The RTL interface `en2` received an IPv4 DHCP lease and IPv6 autoconfiguration.
  The default route still used Ethernet `en0`; traffic checks explicitly bound
  to `en2`, so Ethernet could not substitute for the Wi-Fi data-path test.
- Software TX CCMP: five router pings passed without loss, followed by a 4 MiB
  HTTPS download returning HTTP 200 in 5.891 seconds (711,990 bytes/s, 5.70 Mb/s).
- After the app was reopened, a CLI disconnect and option-mask change to 3
  were followed by a successful app rejoin; no password was supplied to the
  test commands. The driver then reported hardware TX CCMP active.
- Hardware TX CCMP: five pings passed without loss, and the same 4 MiB download
  returned HTTP 200 in 4.996 seconds (839,593 bytes/s, 6.72 Mb/s). Five additional
  pings with 1400-byte payloads also passed without loss.
- A scan while connected found 20 APs, restored channel 4 and preserved the
  WPA2 link with hardware TX CCMP active. At the final diagnostic snapshot there
  were no reported CRC, MIC, TX or RX errors, and no TX ring-full events.

These small internet transfers establish functional traffic, not peak RF
throughput or a causal speed/CPU improvement from offload. CPU savings were not
measured. Replay-drop counters include duplicate traffic; capture-based replay
and key-renewal validation are still pending.

## Bluetooth load attempt and next restart

The manual Bluetooth helper now enables upload through a property on a temporary
signed copy. A boot-argument change is no longer needed for this manual path.
Its first load exposed a declared USB dependency version of 1.0, below this
host's IOUSBHostFamily minimum compatible version 1.0.1. The dependency was
corrected from the installed Apple plist and the kext rebuilt and verified.

The next attempt passed that dependency check, but macOS refused the separate
`com.rtl8723be.macos.bluetooth` extension with approval error 27. The user reports
that Bluetooth also requests a restart. Firmware upload has therefore **not**
been established. Lilu 1.7.3 is loaded; BlueToolFixup is absent on this boot.
Native controller attachment, classic/LE pairing, audio and simultaneous traffic
remain untested. External BlueToolFixup and the internal USB map must be checked
before native Bluetooth testing, as described in BLUETOOTH.md.

## Audio incident after the Bluetooth approval restart

The user restarted and reported loud internal-speaker noise that could not be
muted. Before any new manual RTL load, neither RTL kext appeared in the loaded
list. VoodooHDA 3.0.3 did appear, with the same executable UUID observed before
the feature work. It logged unsupported PCM/createAudioStream/activateAudioEngine
failures. Unloading VoodooHDA and restarting coreaudiod did not stop the noise.

A temporary copy of the existing audio kext was prepared with input monitoring
disabled, mute/volume fixes enabled and PCM/record mixer defaults zero. Its
administrator load attempt was interrupted by the user's reboot; no completed
load result was captured. The user reports the noise stopped following that
reboot but the internal speakers no longer appear.

Read-only checks after this latest reboot found **no VoodooHDA, no RTL Wi-Fi and
no RTL Bluetooth kext loaded**, no VoodooHDADevice or IOAudioEngine instances,
and an empty system audio-device list. Installed VoodooHDA Info.plist and binary
retain their April 3 modification dates. The original and recovery copy have
identical Mach-O __TEXT/__DATA hashes and executable UUID; ad-hoc re-signing
changes the recovery binary's whole-file hash. The recovery files are ignored
local artifacts and excluded from this repository's installer.

This establishes an audio-driver absence after the emergency intervention, not
the cause of the original noise or of the driver remaining absent after reboot.
See [AUDIO_INCIDENT.md](AUDIO_INCIDENT.md) for exact actions and investigation
priorities. Do not resume radio hardware testing until audio is restored.

## Snapshot verification and next checks

After the latest reboot, `make test` passed all production hardware, crypto,
protocol, Bluetooth firmware, credential-format and two boot-config checks,
plus the 57-test legacy simulation. All 28 pinned upstream files passed hash
verification. No new hardware loads were performed during snapshot preparation.

First restore internal audio and establish why VoodooHDA is absent. Then verify
BlueToolFixup/USB mapping and actual Bluetooth firmware upload before claiming a
native controller. Remaining Wi-Fi checks include b-only/g-only APs, password
replacement/forget, CPU measurements, real rekeys, AP loss, saturated traffic
and sleep/wake. Keep boot-time RTL injection disabled.

GUI layout, native Bluetooth behavior, RF performance and full Wi-Fi 4
interoperability cannot be established by the host test suites.
