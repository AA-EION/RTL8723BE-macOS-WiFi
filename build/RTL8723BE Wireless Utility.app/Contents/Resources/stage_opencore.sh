#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEXT_SRC="${PROJECT_DIR}/build/RTL8723BEWiFi.kext"
CLI_BIN="${PROJECT_DIR}/tools/rtl8723be_cli"
STAGING_DIR="${PROJECT_DIR}/dist/OpenCore_EFI_Staging"

echo "=== Realtek RTL8723BE (pci10ec,b723) macOS 26.6.2 Verification & OpenCore Staging ==="

if [[ ! -d "${KEXT_SRC}" ]]; then
    echo "[*] Building RTL8723BEWiFi.kext and rtl8723be_cli..."
    make -C "${PROJECT_DIR}" all
fi

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
"${PROJECT_DIR}/tests/test_runner" | tail -n 15

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
	<true/>
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

# If an OpenCore EFI partition is currently mounted under /Volumes/EFI or /Volumes/ESP, stage automatically if writable
for VOL in "/Volumes/EFI" "/Volumes/ESP"; do
    if [[ -d "${VOL}/EFI/OC/Kexts" && -w "${VOL}/EFI/OC/Kexts" ]]; then
        echo "[*] Detected mounted OpenCore EFI at ${VOL}/EFI/OC — copying RTL8723BEWiFi.kext..."
        rm -rf "${VOL}/EFI/OC/Kexts/RTL8723BEWiFi.kext"
        cp -R "${KEXT_SRC}" "${VOL}/EFI/OC/Kexts/RTL8723BEWiFi.kext"
        python3 - "${VOL}/EFI/OC/config.plist" <<'PYEOF'
import plistlib, sys, os
cfg_path = sys.argv[1]
if os.path.exists(cfg_path):
    with open(cfg_path, 'rb') as f:
        pl = plistlib.load(f)
    kadd = pl.setdefault('Kernel', {}).setdefault('Add', [])
    if not any(entry.get('BundlePath') == 'RTL8723BEWiFi.kext' for entry in kadd):
        kadd.append({
            'Arch': 'x86_64',
            'BundlePath': 'RTL8723BEWiFi.kext',
            'Comment': 'Realtek RTL8723BE PCIe Wireless LAN Driver (pci10ec,b723)',
            'Enabled': True,
            'ExecutablePath': 'Contents/MacOS/RTL8723BEWiFi',
            'MaxKernel': '',
            'MinKernel': '20.0.0',
            'PlistPath': 'Contents/Info.plist',
        })
        with open(cfg_path, 'wb') as f:
            plistlib.dump(pl, f)
        print(f"[+] Injected RTL8723BEWiFi.kext into {cfg_path}")
    else:
        print(f"[+] RTL8723BEWiFi.kext already enabled in {cfg_path}")
PYEOF
    fi
done

if [[ "${1:-}" == "--load" ]]; then
    echo "[*] Preparing root-owned kext in /tmp/RTL8723BEWiFi.kext and invoking kmutil load..."
    sudo rm -rf /tmp/RTL8723BEWiFi.kext
    sudo cp -R "${KEXT_SRC}" /tmp/RTL8723BEWiFi.kext
    sudo chown -R root:wheel /tmp/RTL8723BEWiFi.kext
    sudo chmod -R 755 /tmp/RTL8723BEWiFi.kext
    sudo kmutil load -p /tmp/RTL8723BEWiFi.kext
    echo "[+] Kernel extension load requested! Checking IORegistry..."
    ioreg -l | grep -A 15 "RTL8723BE" || true
fi

echo ""
echo "[+] OpenCore Staging Bundle ready at: ${STAGING_DIR}/EFI/OC/Kexts/RTL8723BEWiFi.kext"
echo "[+] User-space CLI ready at         : ${CLI_BIN}"
