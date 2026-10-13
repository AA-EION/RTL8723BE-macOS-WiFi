#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Issen Software Group
# Manual post-login loading only. Never mounts or writes an EFI partition.
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo "Usage: sudo $0 /absolute/path/RTL8723BEWiFi.kext" >&2
    exit 2
fi
KEXT_SOURCE="$1"
# Darwin 17 = macOS 10.13 High Sierra ... 19 = 10.15 Catalina, 20 = 11 Big Sur, 25 = 26 Tahoe.
# kmutil replaced kextload/kextunload/kextstat in macOS 11; older systems only have the old tools.
DARWIN_MAJOR="$(/usr/sbin/sysctl -n kern.osrelease | /usr/bin/cut -d. -f1)"
if (( DARWIN_MAJOR >= 20 )); then USE_KMUTIL=1; else USE_KMUTIL=0; fi
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
if (( USE_KMUTIL )); then
    LOADED="$(/usr/bin/kmutil showloaded 2>&1)"
else
    LOADED="$(/usr/sbin/kextstat -l 2>&1)"
fi
if [[ "${LOADED}" == *com.rtl8723be.macos.wifi* ]]; then
    echo "Closing UserClient apps and unloading previous com.rtl8723be.macos.wifi kext..."
    /usr/bin/killall RTL8723BEWirelessUtility RTL8723BEWirelessUtilityModern rtl8723be_cli 2>/dev/null || true
    sleep 1
    /sbin/kextunload -b com.rtl8723be.macos.wifi || {
        echo "Could not hot-unload active kext; please reboot once, then load v1.3.0." >&2
        exit 2
    }
fi
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "${KEXT_SOURCE}/Contents/Info.plist")"
if [[ "${VERSION}" != '1.3.0' ]]; then
    echo "Expected Linux-parity init build 1.3.0, got ${VERSION}. Refusing older build." >&2
    exit 2
fi
/usr/bin/codesign --verify --strict "${KEXT_SOURCE}"
# Always load a temporary copy: /private/tmp is cleared at every restart, so the driver
# can never start during boot (boot-time start right after the FileVault password freezes
# the machine). Remove any boot-time copy left behind by the withdrawn first 1.3.0 package.
if [[ -d /Library/Extensions/RTL8723BEWiFi.kext ]]; then
    /bin/rm -rf /Library/Extensions/RTL8723BEWiFi.kext
    echo "Removed /Library/Extensions/RTL8723BEWiFi.kext (boot-time copy)."
fi
TEST_DIR="$(/usr/bin/mktemp -d /private/tmp/rtl8723be-test.XXXXXX)"
/usr/bin/ditto "${KEXT_SOURCE}" "${TEST_DIR}/RTL8723BEWiFi.kext"
/usr/sbin/chown -R root:wheel "${TEST_DIR}/RTL8723BEWiFi.kext"
/bin/chmod -R go-w "${TEST_DIR}/RTL8723BEWiFi.kext"
echo "Manual test build: ${TEST_DIR}/RTL8723BEWiFi.kext"
echo "No EFI changes. If loading is denied, keep the error and follow Testing Guide; do not enable automatic injection."
if (( USE_KMUTIL )); then
    /usr/bin/kmutil load -p "${TEST_DIR}/RTL8723BEWiFi.kext"
else
    # macOS 10.13-10.15: kextload asks for user approval in Security & Privacy the first time
    # (unless SIP's unapproved-kext restriction is off, as on most Hackintoshes).
    if ! OUT="$(/sbin/kextload -v "${TEST_DIR}/RTL8723BEWiFi.kext" 2>&1)"; then
        echo "${OUT}" >&2
        if [[ "${OUT}" == *approv* || "${OUT}" == *"Security"* ]]; then
            echo "The driver is not approved yet. Open System Preferences > Security & Privacy, click Allow, then load it again." >&2
        fi
        exit 2
    fi
    echo "${OUT}"
fi
echo "Load request returned. Check driver status and kernel logs; this alone does not prove hardware startup succeeded."
