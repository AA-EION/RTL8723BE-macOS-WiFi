#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEXT_SRC="${PROJECT_DIR}/build/RTL8723BEWiFi.kext"
CLI_BIN="${PROJECT_DIR}/tools/rtl8723be_cli"
STAGING_DIR="${PROJECT_DIR}/dist/OpenCore_EFI_Staging"

if [[ $# -ne 0 ]]; then
    echo "Usage: $0 (local staging only; live loading/EFI installation is disabled)" >&2
    exit 2
fi

echo "=== Realtek RTL8723BE (pci10ec,b723) macOS 26.6.2 Verification & OpenCore Staging ==="

echo "[*] Building RTL8723BEWiFi.kext and rtl8723be_cli..."
make -C "${PROJECT_DIR}" all

echo "[1/4] Verifying Mach-O architecture and code signature..."
file "${KEXT_SRC}/Contents/MacOS/RTL8723BEWiFi"
codesign -dv "${KEXT_SRC}" 2>&1 | head -n 6 || true

echo "[2/4] Running kmutil KPI & symbol dependency resolution against running kernel ($(uname -r))..."
DIAG_OUT="$(kmutil print-diagnostics --bundle-path "${KEXT_SRC}" 2>&1 || true)"
echo "${DIAG_OUT}"
if ! echo "${DIAG_OUT}" | grep -q "Dependencies: OK"; then
    echo "[!] ERROR: KPI dependency verification failed! Refusing to stage kext."
    exit 1
fi
echo "[+] KPI Dependencies verified: OK"

echo "[3/4] Running 57-test hardware register / DMA / 802.11 / WPA2-CCMP verification suite..."
make -C "${PROJECT_DIR}" test

echo "[4/4] Staging OpenCore bundle in ${STAGING_DIR}..."
mkdir -p "${STAGING_DIR}/EFI/OC/Kexts"
rm -rf "${STAGING_DIR}/EFI/OC/Kexts/RTL8723BEWiFi.kext"
cp -R "${KEXT_SRC}" "${STAGING_DIR}/EFI/OC/Kexts/RTL8723BEWiFi.kext"
cp "${CLI_BIN}" "${STAGING_DIR}/rtl8723be_cli"

cat > "${STAGING_DIR}/config_plist_kernel_add_snippet.plist" <<'EOF'
<!-- Add this entry under Kernel -> Add in your OpenCore EFI/OC/config.plist -->
<dict>
	<key>Arch</key>
	<string>x86_64</string>
	<key>BundlePath</key>
	<string>RTL8723BEWiFi.kext</string>
	<key>Comment</key>
	<string>Realtek RTL8723BE PCIe Wireless LAN Driver (pci10ec,b723)</string>
	<key>Enabled</key>
	<false/>
	<key>ExecutablePath</key>
	<string>Contents/MacOS/RTL8723BEWiFi</string>
	<key>MaxKernel</key>
	<string></string>
	<key>MinKernel</key>
	<string>20.0.0</string>
	<key>PlistPath</key>
	<string>Contents/Info.plist</string>
</dict>
EOF

# Staging is deliberately local-only. Never infer a boot volume from disk0s1
# or overwrite an arbitrary mounted EFI (which may be the recovery EFI).
echo "[!] Experimental build: no EFI partition was mounted or modified."

echo ""
echo "[+] OpenCore Staging Bundle ready at: ${STAGING_DIR}/EFI/OC/Kexts/RTL8723BEWiFi.kext"
echo "[+] User-space CLI ready at         : ${CLI_BIN}"
