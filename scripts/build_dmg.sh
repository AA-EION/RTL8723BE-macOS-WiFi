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
cp "${PROJECT_DIR}/LICENCE.rtlwifi_firmware.txt" "${APP_BUNDLE}/Contents/Resources/LICENCE.rtlwifi_firmware.txt"

# 6. Ad-hoc sign the .app bundle
codesign --force --deep --sign - "${APP_BUNDLE}"
codesign --verify --deep --strict "${APP_BUNDLE}"
echo "[+] Signed and verified ${APP_BUNDLE}"

# 7. Build the installer package (app -> /Applications, kext -> /Library/Extensions,
#    CLI -> /usr/local/bin). Nothing touches NVRAM or the EFI partition.
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "${PROJECT_DIR}/src/Info.plist")"
PKG_WORK="${PROJECT_DIR}/build/pkg"
PKG_ROOT="${PKG_WORK}/root"
rm -rf "${PKG_WORK}"
mkdir -p "${PKG_ROOT}/Applications" "${PKG_ROOT}/Library/Extensions" "${PKG_ROOT}/usr/local/bin" "${PKG_WORK}/resources"
ditto "${APP_BUNDLE}" "${PKG_ROOT}/Applications/${APP_NAME}"
ditto "${PROJECT_DIR}/build/RTL8723BEWiFi.kext" "${PKG_ROOT}/Library/Extensions/RTL8723BEWiFi.kext"
cp "${PROJECT_DIR}/tools/rtl8723be_cli" "${PKG_ROOT}/usr/local/bin/rtl8723be_cli"
pkgbuild --analyze --root "${PKG_ROOT}" "${PKG_WORK}/component.plist"
# Never let Installer "relocate" the app onto some other copy with the same bundle ID.
i=0
while /usr/libexec/PlistBuddy -c "Print :${i}" "${PKG_WORK}/component.plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Delete :${i}:BundleIsRelocatable" "${PKG_WORK}/component.plist" 2>/dev/null || true
    /usr/libexec/PlistBuddy -c "Add :${i}:BundleIsRelocatable bool false" "${PKG_WORK}/component.plist"
    i=$((i + 1))
done
pkgbuild --root "${PKG_ROOT}" --component-plist "${PKG_WORK}/component.plist" \
    --scripts "${PROJECT_DIR}/installer/scripts" \
    --identifier com.rtl8723be.macos.wifi.pkg --version "${VERSION}" --install-location / \
    "${PKG_WORK}/RTL8723BE-component.pkg"
cp "${PROJECT_DIR}/installer/resources/"*.txt "${PROJECT_DIR}/LICENCE.rtlwifi_firmware.txt" "${PKG_WORK}/resources/"
sed "s/__VERSION__/${VERSION}/" "${PROJECT_DIR}/installer/Distribution.xml" > "${PKG_WORK}/Distribution.xml"
PKG_OUT="${PKG_WORK}/RTL8723BE WiFi ${VERSION}.pkg"
productbuild --distribution "${PKG_WORK}/Distribution.xml" --resources "${PKG_WORK}/resources" \
    --package-path "${PKG_WORK}" "${PKG_OUT}"
pkgutil --check-signature "${PKG_OUT}" >/dev/null 2>&1 || true
echo "[+] Installer package: ${PKG_OUT}"

# 8. Assemble DMG: installer, uninstaller, docs and licence.
rm -rf "${DMG_ROOT}"
mkdir -p "${DMG_ROOT}"
cp "${PKG_OUT}" "${DMG_ROOT}/Install RTL8723BE WiFi.pkg"
cp "${PROJECT_DIR}/scripts/uninstall.sh" "${DMG_ROOT}/uninstall.sh"
cp "${PROJECT_DIR}/installer/resources/welcome.txt" "${DMG_ROOT}/Read Me.txt"
cp "${PROJECT_DIR}/LICENCE.rtlwifi_firmware.txt" "${DMG_ROOT}/LICENCE.rtlwifi_firmware.txt"

mkdir -p "${PROJECT_DIR}/dist"
rm -f "${DMG_OUT}"
hdiutil create -volname "RTL8723BE WiFi ${VERSION}" \
    -srcfolder "${DMG_ROOT}" \
    -ov -format UDZO \
    "${DMG_OUT}"

hdiutil verify "${DMG_OUT}"
echo "[+] DMG Installer successfully built and verified at: ${DMG_OUT}"
