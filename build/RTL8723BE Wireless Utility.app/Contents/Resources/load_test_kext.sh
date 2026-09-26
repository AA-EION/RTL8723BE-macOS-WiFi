#!/usr/bin/env bash
# Manual post-login loading only. Never mounts or writes an EFI partition.
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo "Usage: sudo $0 /absolute/path/RTL8723BEWiFi.kext" >&2
    exit 2
fi
KEXT_SOURCE="$1"
BOOT_ARGS="$(/usr/sbin/sysctl -n kern.bootargs)"
case " ${BOOT_ARGS} " in
    *" rtl8723be_experimental=1 "*) ;;
    *) echo "Hardware test is disabled. Follow Testing Guide to prepare the boot flag, then reboot. No kext loaded." >&2; exit 2 ;;
esac
if [[ "${EUID}" -ne 0 ]]; then
    echo "Run with sudo (or the GUI's administrator prompt)." >&2
    exit 2
fi
if [[ ! -d "${KEXT_SOURCE}" ]]; then
    echo "Kext not found: ${KEXT_SOURCE}" >&2
    exit 2
fi
LOADED="$(/usr/bin/kmutil showloaded 2>&1)"
if [[ "${LOADED}" == *com.rtl8723be.macos.wifi* ]]; then
    if /usr/sbin/ioreg -c RTL8723BE | /usr/bin/grep -q "RTL8723BE"; then
        echo "An RTL8723BE kext instance is actively attached. Reboot before testing another build; hot unload of active hardware is disabled." >&2
        exit 2
    else
        echo "Previous RTL8723BE kext did not attach (0 active instances). Unloading unreferenced kext bundle..."
        /sbin/kextunload -b com.rtl8723be.macos.wifi || true
    fi
fi
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "${KEXT_SOURCE}/Contents/Info.plist")"
if [[ "${VERSION}" != '1.0.2' ]]; then
    echo "Expected freeze-fix build 1.0.2, got ${VERSION}. Refusing older build." >&2
    exit 2
fi
/usr/bin/codesign --verify --strict "${KEXT_SOURCE}"
TEST_DIR="$(/usr/bin/mktemp -d /private/tmp/rtl8723be-test.XXXXXX)"
/usr/bin/ditto "${KEXT_SOURCE}" "${TEST_DIR}/RTL8723BEWiFi.kext"
/usr/sbin/chown -R root:wheel "${TEST_DIR}/RTL8723BEWiFi.kext"
/bin/chmod -R go-w "${TEST_DIR}/RTL8723BEWiFi.kext"
echo "Manual test build: ${TEST_DIR}/RTL8723BEWiFi.kext"
echo "No EFI changes. If loading is denied, keep the error and follow Testing Guide; do not enable automatic injection."
/usr/bin/kmutil load -p "${TEST_DIR}/RTL8723BEWiFi.kext"
echo "Load request returned. Check driver status and kernel logs; this alone does not prove hardware startup succeeded."
