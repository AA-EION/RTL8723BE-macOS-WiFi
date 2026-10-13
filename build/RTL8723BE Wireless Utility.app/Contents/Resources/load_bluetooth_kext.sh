#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Manual USB firmware-loader test. No EFI, NVRAM, boot collection or USB-map edits.
set -euo pipefail
[[ $# == 1 && -d "$1" ]] || { echo "Usage: sudo $0 /absolute/path/RTL8723BluetoothFirmware.kext" >&2; exit 2; }
[[ $EUID == 0 ]] || { echo "Administrator privileges required." >&2; exit 2; }
IDENTIFIER="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$1/Contents/Info.plist")"
[[ "${IDENTIFIER}" == com.rtl8723be.macos.bluetooth ]] || { echo "Unexpected Bluetooth bundle." >&2; exit 2; }
/usr/bin/codesign --verify --strict "$1"
LOADED="$(/usr/bin/kmutil showloaded 2>&1)"
if [[ "${LOADED}" == *com.rtl8723be.macos.bluetooth* ]]; then
    echo "Bluetooth firmware driver is already loaded; use Bluetooth Settings."
    exit 0
fi
TEST_DIR="$(/usr/bin/mktemp -d /private/tmp/rtl8723be-bt-test.XXXXXX)"
/usr/bin/ditto "$1" "${TEST_DIR}/RTL8723BluetoothFirmware.kext"
# Enable only this manual, temporary copy. Never change the shipped bundle,
# OpenCore config, NVRAM or the boot collection to opt in to firmware upload.
PLIST="${TEST_DIR}/RTL8723BluetoothFirmware.kext/Contents/Info.plist"
for PERSONALITY in RTL8723B_0bda_8723 RTL8723B_0bda_b006 RTL8723B_0bda_b720 RTL8723B_0bda_b728; do
    /usr/libexec/PlistBuddy -c "Add :IOKitPersonalities:${PERSONALITY}:FirmwareUploadEnabled bool true" "${PLIST}"
done
/usr/bin/codesign --force --sign - "${TEST_DIR}/RTL8723BluetoothFirmware.kext"
/usr/bin/codesign --verify --strict "${TEST_DIR}/RTL8723BluetoothFirmware.kext"
/usr/sbin/chown -R root:wheel "${TEST_DIR}/RTL8723BluetoothFirmware.kext"
/bin/chmod -R go-w "${TEST_DIR}/RTL8723BluetoothFirmware.kext"
/usr/bin/kmutil load -p "${TEST_DIR}/RTL8723BluetoothFirmware.kext"
echo "Bluetooth loader requested. Confirm FirmwareLoaded in the app before pairing in macOS."
