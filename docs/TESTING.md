# Testing the 1.4.0 development build

This repository records a boot-time login freeze. Keep both drivers inside the
app and load after login only. Hardware tests are distinct from the passing
host regression suites; see FEATURES.md for the incomplete feature set.

## Build and host checks

```sh
make all
make test
./build/test_credentials --keychain
./scripts/build_dmg.sh
```

`make all` builds the Wi-Fi kext, separate Bluetooth firmware kext and CLI.
`make test` runs the actual production descriptor/crypto/protocol/firmware tests
under ASan/UBSan, credential validation, boot configuration checks, and the
historical simulated driver suite. The simulator does not prove real hardware
behavior. The optional `--keychain` test saves/reads/updates/deletes an item under
a unique test service, then removes it; it never queries the app's saved items.

The package and DMG bundle the same 1.4.0 drivers, app, CLI, loaders and guides.
Build/signature/firmware-hash verification does not prove a device started.
The kernel build currently targets x86_64 macOS 26.5; other OS versions are not
validated by compiling the GUI with its macOS 14 deployment target.

## Manual Wi-Fi test

1. Verify normal boot with RTL8723BE injection disabled in OpenCore. Keep the
   existing recovery path available; do not modify the recovery EFI.
2. Ensure the existing OpenCore boot arguments include
   `rtl8723be_experimental=1`. Restart if the flag changes. Do not enable this
   kext in Kernel/Add, install it in /Library/Extensions, reset NVRAM, or change
   security settings simply to bypass a load error.
3. After login, confirm the flag with `sysctl -n kern.bootargs`. Open the new
   app from this repository or the installer. Use its Load Driver button or:

   ```sh
   sudo ./scripts/load_test_kext.sh "$PWD/build/RTL8723BEWiFi.kext"
   ./tools/rtl8723be_cli status
   ./tools/rtl8723be_cli link
   ```

   The helper verifies the bundle version/signature before requesting an unload
   of an existing instance. The app pauses polling during this operation. If
   unload is refused, stop and reboot; do not run an unload loop. Normal cold
   loading after login is preferable for comparing versions. The helper copies
   to a temporary root-owned directory and makes no EFI/NVRAM changes.
4. Check kernel messages, leave the desktop responsive for a minute, and scan:

   ```sh
   log show --last 10m --predicate 'eventMessage CONTAINS "RTL8723BE"'
   ./tools/rtl8723be_cli scan
   ```

5. Join an available WPA2 Personal/CCMP test AP using the app. Check DHCP,
   routing, ping, and bidirectional transfer. Save a password, quit/relaunch,
   rejoin without typing, test replacement/forget and manual disconnect.
6. Test b-only, g-only and HT20 AP settings separately. Record `link` and `diag`
   output plus the AP settings; a displayed HT capability is not a throughput
   result. Compare software and hardware TX encryption using the same AP, signal,
   transfer duration and CPU measurement. Change the option only disconnected.
7. Exercise wrong password, AP loss, GTK renewal, pairwise rekey, saturated TX,
   scanning while connected, and actual sleep/wake. Verify key reinstallation
   does not reset packet numbers and that stale packets never reach the stack.
   Monitor/replay capture validation needs a second radio or AP instrumentation.

If loading requests macOS approval, retain the exact error and use Privacy &
Security if proceeding. Restart when required and check the flags again. A
successful kmutil return is only a load request, not a startup/connection result.
Reboot after a failed/frozen hardware test before trying a different build.

## Bluetooth and simultaneous traffic

Follow BLUETOOTH.md. Manual loading opts in through a temporary signed copy;
native attachment requires compatible external Lilu/BlueToolFixup on macOS 12+, an internal USB
map and a controller cold enough to identify its ROM correctly. Firmware upload,
native controller attachment, classic/LE pairing, SCO/audio, simultaneous Wi-Fi
traffic and sleep/wake must each be checked. Never infer them from USB detection
or the firmware parser tests. The loader does not reset a controller owned by
macOS just to rerun upload.

## Returning to normal use

The installer does not alter EFI/NVRAM. Remove test flags from the OpenCore
configuration you actually boot, retain disabled RTL8723BE injection, and restart.
The existing configure_test_boot.py helper handles the Wi-Fi flag and keeps its
Add/Force entries disabled; it does not manage the separate Bluetooth flag.
Use uninstall.sh to unload both drivers/remove the app and CLI. Forget saved
networks in the app before uninstalling if their Keychain items should be deleted.

OpenCore configuration semantics are described in its
[official reference](https://github.com/acidanthera/OpenCorePkg/blob/master/Docs/Configuration.tex).
