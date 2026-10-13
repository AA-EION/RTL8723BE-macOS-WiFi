# RTL8723B Bluetooth development driver

The card's Bluetooth function uses USB, independently of its PCIe Wi-Fi function.
`RTL8723BluetoothFirmware.kext` uploads the original Realtek firmware with the
Linux v6.12 `btrtl.c` protocol, then closes its USB interfaces so macOS can own
pairing, ACL, SCO, LE and power controls. It is not a new Bluetooth stack.

This host exposes `0bda:b006`. The kext also narrowly matches `0bda:8723`,
`0bda:b720` and `0bda:b728`, as found in the reference Realtek Bluetooth uploader.
USB matching alone is insufficient: the loader requires unpatched LMP 0x8723,
HCI revision 0x000b and HCI version 6 and selects the matching ROM patch.
Other chips are refused. These additional IDs are not hardware-tested here.

The current test host has an unresolved internal-speaker incident following an
approval restart. Bluetooth upload has not been verified. Restore its audio
before further loading/testing; see [AUDIO_INCIDENT.md](AUDIO_INCIDENT.md).

## Prepare and load

1. Follow the macOS Realtek Bluetooth reference project's
   [requirements](https://github.com/thegwchr/RealtekBluetoothFirmware/blob/main/docs/Installation.md).
   Third-party Realtek Bluetooth on macOS 12 and newer needs **Lilu and
   BlueToolFixup**. They are external dependencies; this installer does not install
   or change them. Do not run two Realtek firmware uploaders for the same device.
   BlueToolFixup comes from [Acidanthera BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM#bluetoolfixupkext);
   use BlueToolFixup, not the Broadcom firmware uploader or the older injector,
   for this Realtek device. The official requirement is Lilu 1.5.4 or newer.
2. Ensure the Bluetooth USB port is mapped correctly as an internal device.
   The installer does not modify EFI, NVRAM or the USB map.
3. After login, open this app, then Settings → Bluetooth → **Load Bluetooth Driver…**.
   Alternatively use:

   ```sh
   sudo ./scripts/load_bluetooth_kext.sh "$PWD/build/RTL8723BluetoothFirmware.kext"
   ```

4. Confirm **Firmware: Loaded** and an available macOS controller. Use
   **Bluetooth Settings…** to pair devices and control Bluetooth power.

The manual helper enables firmware upload only in its temporary, signed copy;
the shipped kext is inert without this opt-in. No Bluetooth boot-argument change
is needed for manual loading. The older `rtl8723be_bluetooth=1` opt-in remains
accepted, but is optional. macOS may require approval and restart for a new kext
or for installing the external native-stack dependencies.

The app checks a property set by this uploader on the matching USB device;
the macOS default-controller status is shown separately because another dongle
could be the default controller. A successful `kmutil load` alone proves neither
upload nor native attachment. A previously patched controller is not reflashed
using an assumed ROM identity; reboot for a cold upload test.

Enable **Share the single antenna with Bluetooth** in the Wi-Fi app while disconnected
when testing simultaneous traffic. The older separate boot flag also enables
this Linux single-antenna PTA baseline by default. This restores antenna arbitration instead of permanently
fixing the shared switch to Wi-Fi. The complete dynamic Linux SCO/HID/A2DP
policy is still pending. Test simultaneous Wi-Fi traffic and Bluetooth audio
before relying on this build. Two-antenna boards need their actual wiring and
coexistence policy validated.

## Verification

```sh
ioreg -r -c RTL8723BluetoothFirmware -l
log show --last 10m --predicate 'eventMessage CONTAINS "RTL8723Bluetooth"'
system_profiler SPBluetoothDataType
```

Host tests validate the actual firmware, malformed metadata/truncation and
Linux fragment wrap/final-index rules. Upload, native attachment, pairing,
LE, audio, simultaneous traffic and firmware retention after sleep/wake remain
hardware validation items. This build does not reset a USB controller already
owned by the native stack during wake.

The same installer bundles both kexts inside the app for manual post-login
loading. Uninstall unloads both and removes the app; firmware in the controller
remains until its next reset/power cycle. BlueToolFixup/Lilu are not removed.
