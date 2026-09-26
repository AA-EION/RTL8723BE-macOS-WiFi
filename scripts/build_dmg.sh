#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_NAME="RTL8723BE Wireless Utility.app"
APP_BUNDLE="${PROJECT_DIR}/build/${APP_NAME}"
DMG_ROOT="${PROJECT_DIR}/build/dmg_staging"
DMG_OUT="${PROJECT_DIR}/dist/RTL8723BE_WiFi_Installer.dmg"

echo "=== Building Native macOS GUI Application & DMG Installer ==="

# 1. Ensure kext and CLI are built
make -C "${PROJECT_DIR}" all

# 2. Prepare .app bundle layout
rm -rf "${APP_BUNDLE}"
mkdir -p "${APP_BUNDLE}/Contents/MacOS"
mkdir -p "${APP_BUNDLE}/Contents/Resources"

# 3. Reuse the checked-in icon; packaging must not depend on a live graphics session.
cp "${PROJECT_DIR}/gui/AppIcon.icns" "${APP_BUNDLE}/Contents/Resources/AppIcon.icns"

# 4. Compile Swift GUI binary
echo "[*] Compiling Swift/AppKit GUI binary..."
swiftc -O -parse-as-library -target x86_64-apple-macosx13.0 \
    -import-objc-header "${PROJECT_DIR}/gui/RTL8723BEBridge.h" \
    -framework Cocoa -framework SwiftUI -framework IOKit \
    "${PROJECT_DIR}/gui/RTL8723BEApp.swift" \
    -o "${APP_BUNDLE}/Contents/MacOS/RTL8723BEWirelessUtility"

# 5. Populate Bundle Resources (Info.plist, RTL8723BEWiFi.kext, rtl8723be_cli, stage_opencore.sh)
cp "${PROJECT_DIR}/gui/Info.plist" "${APP_BUNDLE}/Contents/Info.plist"
cp -R "${PROJECT_DIR}/build/RTL8723BEWiFi.kext" "${APP_BUNDLE}/Contents/Resources/RTL8723BEWiFi.kext"
cp "${PROJECT_DIR}/tools/rtl8723be_cli" "${APP_BUNDLE}/Contents/Resources/rtl8723be_cli"
cp "${PROJECT_DIR}/scripts/load_test_kext.sh" "${APP_BUNDLE}/Contents/Resources/load_test_kext.sh"
cp "${PROJECT_DIR}/docs/TESTING.md" "${APP_BUNDLE}/Contents/Resources/Testing Guide.txt"

# 6. Ad-hoc sign the .app bundle
codesign --force --deep --sign - "${APP_BUNDLE}"
codesign --verify --deep --strict "${APP_BUNDLE}"
echo "[+] Signed and verified ${APP_BUNDLE}"

# 7. Assemble DMG Staging Folder
rm -rf "${DMG_ROOT}"
mkdir -p "${DMG_ROOT}"
cp -R "${APP_BUNDLE}" "${DMG_ROOT}/${APP_NAME}"
ln -s /Applications "${DMG_ROOT}/Applications"
cp -R "${PROJECT_DIR}/build/RTL8723BEWiFi.kext" "${DMG_ROOT}/RTL8723BEWiFi.kext"
cp "${PROJECT_DIR}/tools/rtl8723be_cli" "${DMG_ROOT}/rtl8723be_cli"

cp "${PROJECT_DIR}/docs/TESTING.md" "${DMG_ROOT}/Testing Guide.txt"

# 8. Create compressed UDZO DMG and verify
mkdir -p "${PROJECT_DIR}/dist"
rm -f "${DMG_OUT}"
hdiutil create -volname "RTL8723BE Wi-Fi Installer" \
    -srcfolder "${DMG_ROOT}" \
    -ov -format UDZO \
    "${DMG_OUT}"

hdiutil verify "${DMG_OUT}"
echo "[+] DMG Installer successfully built and verified at: ${DMG_OUT}"
