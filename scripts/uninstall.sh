#!/bin/bash
# Removes the RTL8723BE driver and app. Run: sudo ./uninstall.sh
set -u
if [[ "${EUID}" -ne 0 ]]; then echo "Run with sudo." >&2; exit 2; fi
/usr/bin/killall RTL8723BEWirelessUtility rtl8723be_cli 2>/dev/null || true
/usr/bin/kmutil unload -b com.rtl8723be.macos.wifi 2>/dev/null || true
/bin/rm -rf /Library/Extensions/RTL8723BEWiFi.kext "/Applications/RTL8723BE Wireless Utility.app" /usr/local/bin/rtl8723be_cli
/usr/sbin/pkgutil --forget com.rtl8723be.macos.wifi.pkg 2>/dev/null || true
/usr/bin/kmutil clear-staging 2>/dev/null || true
echo "Removed. Restart to rebuild the kernel extension cache without the driver."
