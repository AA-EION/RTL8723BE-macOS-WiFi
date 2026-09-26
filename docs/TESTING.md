# Testing build 1.0.1 after the internal EFI repair

The internal Kingston SATA SSD's EFI is disk0s1. Its RTL8723BE entry was disabled
on 2026-09-26. The USB recovery EFI was not modified. The internal boot has not yet
been verified by rebooting. This build still has unvalidated hardware paths and
can freeze the system during a manual hardware test.

## 1. Verify normal internal-disk boot first

Restart and choose the internal SSD's OpenCore boot entry in the firmware boot
menu, rather than the USB. If needed, disconnect the USB while powered off and
reconnect it for recovery. Log in and verify that the desktop stays responsive.
The Realtek driver is disabled on this boot, so its Wi-Fi will not work yet.

The original internal EFI is backed up under the repository's ignored
`.local-backups/internal-efi-20260926-150505/EFI/`. A copy of the original config
also resides at `EFI/OC/config.before-rtl8723be-disable.plist`. Restoring that
original config re-enables the old crashing driver; retain it for comparison,
not as the normal boot config.

## 2. Test the GUI without hardware activation

Open the newly rebuilt `build/RTL8723BE Wireless Utility.app` in this repository.
Use this copy, not a previously installed copy in Applications or an old DMG.
It should open and report the driver inactive. This is expected. The Testing
Guide button opens these instructions. The app no longer installs anything into
EFI. It only attempts a manual driver load when you explicitly click Load Test
Driver and the experimental flag is present in the current boot.

## 3. Prepare a manual hardware-test boot

Save your work and keep the recovery USB available. From the repository:

```sh
sudo diskutil mount disk0s1
python3 scripts/configure_test_boot.py --config /Volumes/EFI/EFI/OC/config.plist --apply
```

The helper backs up the config, preserves other boot flags, adds
`rtl8723be_experimental=1`, and **keeps the RTL8723BE Kernel/Add and Force entries
disabled**. It does not install or load a driver. Reboot through the internal EFI
again, log in, and check:

```sh
sysctl -n kern.bootargs
```

The result must include `rtl8723be_experimental=1`. If absent, do not load the
kext; confirm which OpenCore EFI booted. Do not reset all NVRAM or change SIP/AMFI
just to get past an error.

## 4. Load only after login, with logs visible

In a Terminal, start a kernel log stream before loading:

```sh
sudo log stream --style compact --level debug --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "RTL8723BE"' | tee /tmp/rtl8723be-test.log
```

In another Terminal, from the repository:

```sh
sudo ./scripts/load_test_kext.sh "$PWD/build/RTL8723BEWiFi.kext"
./tools/rtl8723be_cli status
```

Alternatively use **Load Test Driver (after login)** in the new GUI and approve
the macOS administrator prompt. Both routes use the same loader; choose one.
The loader uses a fresh temporary directory, checks version/signature, and never
changes EFI or unloads an existing instance. A successful kmutil return does not
prove start() succeeded: use status and the logs to check. A firmware or power
failure now stops startup, so a missing service plus a failure log is useful
information, not a reason to bypass the startup checks.

If macOS requests kext approval, approve it in System Settings if you choose to
continue. If macOS requires a restart, restart, check the boot flag and status
again, and load only if no instance is loaded. If kmutil reports security or
policy errors, retain the exact error. Do not return to the old EFI installer or
try a live unload/reload loop.

First leave the GUI/status running for a minute without scanning. If stable,
click **Scan Now once** and record the result. Scanning and connection still
have known concurrency/protocol limitations: a passing scan does not establish
normal Wi-Fi reliability. Do not start connection testing if startup or scanning
has failed. If the machine freezes, restart normally: the EFI injection remains
disabled, so this procedure does not intentionally reload the experimental kext
at every boot. Use the recovery USB if needed. A hard freeze may prevent the
last log messages from reaching disk.

## 5. Finish the test

To remove the experimental flag while retaining the repaired boot configuration:

```sh
sudo diskutil mount disk0s1
python3 scripts/configure_test_boot.py --config /Volumes/EFI/EFI/OC/config.plist --disable --apply
```

Then reboot. Reboot between driver versions; hot unload is not validated.
Do not re-enable the historical RTL8723BE entry or copy the old v1.0.0 DMG back.

## Rebuild

```sh
make test
./scripts/build_dmg.sh
```

The app and DMG contain the same 1.0.1 development kext, CLI, manual loader and
this guide. Source tests and signatures are build checks, not hardware validation.

OpenCore's Enabled setting and boot-args configuration are described in the
[official reference](https://github.com/acidanthera/OpenCorePkg/blob/master/Docs/Configuration.tex).
