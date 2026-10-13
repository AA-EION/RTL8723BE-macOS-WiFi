# Internal audio incident and recovery audit — 2026-10-12

Status: the user reports that reboot stopped the loud sound. Internal speakers
are unavailable. The latest read-only snapshot shows VoodooHDA is not loaded and
macOS has no audio devices. Cause of the original noise remains unresolved.
Restore internal audio before continuing radio hardware tests.

## Sequence and observed results

1. Wi-Fi 1.4.0 had passed manual post-login startup, WPA2 association, DHCP,
   saved-password rejoin and interface-bound traffic with software and hardware
   TX CCMP. Neither radio driver is intentionally installed for boot loading.
2. The separate Bluetooth firmware-loader load failed on an IOUSBHostFamily
   dependency declaration. Its minimum version was corrected to 1.0.1 using the
   installed Apple dependency plist. The next load failed with approval error
   27. The user approved it and rebooted. No successful firmware upload or native
   Bluetooth controller attachment was established.
3. After this approval reboot the user reported loud internal-speaker noise and
   inability to mute. Before a new manual RTL load, `kmutil showloaded` contained
   neither RTL kext. It contained VoodooHDA 3.0.3, identifier
   `org.voodoo.driver.VoodooHDA`, executable UUID
   `CFC08089-4AF0-3075-9699-FACE60E43383`. That same UUID/version had been observed
   before this feature work. This does not exclude a transient earlier load.
4. Audio logs identified Intel Sunrise Point-LP HDA `8086:9d70` and reported
   `channel doesn't support PCM stream format`, `createAudioStream failed`,
   `activateAudioEngine failed` and an audio-engine failure for channel 3.
   An earlier boot had codec-2 timeout/not-responding/probing-aborted errors.
   These are diagnostic clues, not a confirmed cause or a codec identification.
5. AppleScript mute/zero-volume attempts did not stop the noise. The audio
   device list was empty and volume queries returned missing values/connection
   errors, so there was no demonstrated working software mixer.
6. An administrator-authorized VoodooHDA unload completed successfully:

   ```sh
   /usr/bin/kmutil unload -b org.voodoo.driver.VoodooHDA
   ```

   The next loaded list and registry checks showed VoodooHDA absent. The user
   confirmed that the loud sound continued.
7. An administrator-authorized audio-service restart completed successfully:

   ```sh
   /bin/launchctl kickstart -k system/com.apple.audio.coreaudiod
   ```

   Further mute/volume-zero attempts still had no usable audio device. They did
   not establish that the noise stopped.
8. A temporary recovery kext was made from the installed VoodooHDA bundle, with
   the five plist changes below. A local helper was launched through
   `osascript ... with administrator privileges`. It checked/unloaded either
   RTL driver if present, copied the recovery bundle into a new
   `/private/tmp/audio-recovery.XXXXXX` directory, set root:wheel ownership and
   removed group/other write access, then attempted `kmutil load -p` on it.
   The process had no captured completion output before the reboot. Do not
   infer a successful load, a successful hardware reset, or persistence from
   the launch alone. The original process is no longer available after reboot.
9. The user reports rebooting during the Voodoo load, after which the noise
   stopped but internal speakers disappeared. Current read-only evidence:
   VoodooHDA and both RTL kexts absent from the loaded list; no VoodooHDADevice
   or IOAudioEngine instances; `system_profiler SPAudioDataType` has no devices.
   No `audio-recovery.*` directory was found in `/private/tmp` afterward.

## Exact temporary audio changes

The local recovery files are `build/VoodooHDA-audio-recovery.kext` and
`build/stop_rtl_and_reset_audio.sh`. They are ignored by Git, preserved locally
for investigation and never copied into the RTL app, package or DMG. Do not
run the helper again without reviewing the present audio failure first.

Under `IOKitPersonalities/VoodooHDA`:

| Setting | Installed original | Temporary recovery copy |
|---|---|---|
| DisableInputMonitor | false | true |
| VoodooHDAEnableMuteFix | false | true |
| VoodooHDAEnableVolumeChangeFix | false | true |
| MixerValues/PCM | 100 | 0 |
| MixerValues/Rec | 70 | 0 |

The copy was ad-hoc signed. No executable logic was patched or rebuilt. Its
Mach-O __TEXT and __DATA bytes and UUID match the installed original; signing
changes __LINKEDIT/signature data, so the whole-file hashes differ:

- Installed executable SHA-256:
  `eebc409454c848af576a9f2ea16e7ce06ac136c4a30b22bf178c1f2028198474`.
- Recovery executable SHA-256:
  `84ee676ef120b0c52fa95772441f2f7decdb281e302d4c7fe998a10f8e0218a1`.
- Both __TEXT SHA-256:
  `0f3e2761367ae3c5f7137c95a6900e34cc6089bdcbe080fb259798c50b1a3000`.
- Both __DATA SHA-256:
  `6e5f692be800ad035e38cbd102badc999f687e7a2a64898e60063326ddd7ba8a`.

Installed `/Library/Extensions/VoodooHDA.kext/Contents/Info.plist` and
`Contents/MacOS/VoodooHDA` still have modification dates April 3, 2026 at 18:18:12
as reported by this host. They were not overwritten in this intervention.
Unload/load commands can affect system extension management state; that state
has not yet been audited, and unchanged installed files do not prove it unchanged.

## Source used to select the recovery settings

Actual upstream [CloverHackyColor/VoodooHDA](https://github.com/CloverHackyColor/VoodooHDA)
was inspected at commit `beb415a15eca37c5aedb30c1cac95851cce53c96`, cached locally
under `.references/VoodooHDA`. The installed 3.0.3 binary was not rebuilt from
this checkout; source correspondence to that installed build is not proven.

- `tranc/VoodooHDADevice.cpp`: `init` parses monitor/mute/volume options;
  `initMixerDefaultValues` reads zero-valued mixer defaults; `mixerSetDefaults`
  applies them. `start`/`stop` call `resetController`.
- `tranc/Parser.cpp`: `DisableInputMonitor` disables specified monitor widgets.
  Codec matching and effect on this host were not verified.

## What the RTL work changed, and what remains unknown

The RTL source/app/installer and post-login loaders were changed. The Bluetooth
helper enables `FirmwareUploadEnabled=true` on a temporary signed radio-kext
copy; its shipped personality lacks that property. No EFI, NVRAM, boot argument,
USB mapping, installed audio plist or installed audio binary was edited during
this feature work or emergency intervention. The installer only installs the
RTL app/CLI and removes its own obsolete boot-time Wi-Fi copy. No BlueToolFixup
was installed; Lilu 1.7.3 was observed, BlueToolFixup was absent.

Temporal proximity to Bluetooth approval does not identify the cause. Neither
radio was loaded at the inspected incident points, but this is not a complete
boot history. Do not dismiss the user's report or claim either culpability or
exoneration without evidence. Similarly, the absence of VoodooHDA explains why
no speaker device is exposed now, but why it remained absent after reboot is
still unknown.

## First investigation in the next session

Capture current loaded kexts, HDA PCI/provider tree, narrowly filtered boot logs,
installed versus staged VoodooHDA plist/signatures, kernel-collection membership
and extension approval state before modifying anything. Determine whether
VoodooHDA is excluded from the current collection, refused by policy, unmatched
to its provider or failing startup. Do not jump to a guessed layout ID or codec.
Inspect existing local EFI backups only for audio-related differences; they may
contain machine identifiers and must never be published. No active EFI mutation
was performed in this intervention.

Plan audio recovery from the installed original and real VoodooHDA source with
the loud-noise history in mind. Preserve backups, keep radio tests paused and
coordinate any live audio load/reboot with the user. Verify speaker enumeration,
mute, volume and normal audio explicitly; silence alone is not restored audio.
Avoid raw guessed MMIO/codec writes, broad staging deletion, unrelated kext
replacement or blindly rerunning the interrupted helper.
