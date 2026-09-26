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

# 3. Generate macOS AppIcon.icns via CoreGraphics Swift snippet
ICONSET_DIR="${PROJECT_DIR}/build/AppIcon.iconset"
rm -rf "${ICONSET_DIR}"
mkdir -p "${ICONSET_DIR}"

swift - "${ICONSET_DIR}" <<'SWIFTEOF'
import Cocoa

let outDir = CommandLine.arguments[1]
let sizes = [16, 32, 64, 128, 256, 512, 1024]

for sz in sizes {
    let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: sz,
        pixelsHigh: sz,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: 0,
        bitsPerPixel: 0
    )!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

    let rect = NSRect(x: 0, y: 0, width: CGFloat(sz), height: CGFloat(sz))
    let path = NSBezierPath(roundedRect: rect.insetBy(dx: CGFloat(sz)*0.06, dy: CGFloat(sz)*0.06),
                            xRadius: CGFloat(sz)*0.22, yRadius: CGFloat(sz)*0.22)
    let grad = NSGradient(starting: NSColor(calibratedRed: 0.05, green: 0.38, blue: 0.92, alpha: 1.0),
                          ending: NSColor(calibratedRed: 0.0, green: 0.78, blue: 0.95, alpha: 1.0))!
    grad.draw(in: path, angle: -45)

    // Draw Wi-Fi arcs
    NSColor.white.setStroke()
    let center = NSPoint(x: CGFloat(sz)*0.5, y: CGFloat(sz)*0.30)
    for rFactor in [0.16, 0.28, 0.40] {
        let arc = NSBezierPath()
        arc.lineWidth = CGFloat(sz) * 0.055
        arc.lineCapStyle = .round
        arc.appendArc(withCenter: center, radius: CGFloat(sz) * CGFloat(rFactor), startAngle: 42, endAngle: 138)
        arc.stroke()
    }
    let dotRadius = CGFloat(sz) * 0.045
    let dotRect = NSRect(x: center.x - dotRadius, y: center.y - dotRadius, width: dotRadius*2, height: dotRadius*2)
    NSColor.white.setFill()
    NSBezierPath(ovalIn: dotRect).fill()

    NSGraphicsContext.restoreGraphicsState()
    if let png = rep.representation(using: .png, properties: [:]) {
        let file = "\(outDir)/icon_\(sz)x\(sz).png"
        try? png.write(to: URL(fileURLWithPath: file))
        if sz >= 32 {
            let half = sz / 2
            let file2x = "\(outDir)/icon_\(half)x\(half)@2x.png"
            try? png.write(to: URL(fileURLWithPath: file2x))
        }
    }
}
SWIFTEOF

iconutil -c icns "${ICONSET_DIR}" -o "${APP_BUNDLE}/Contents/Resources/AppIcon.icns" || true

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
cp "${PROJECT_DIR}/scripts/stage_opencore.sh" "${APP_BUNDLE}/Contents/Resources/stage_opencore.sh"

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

cat > "${DMG_ROOT}/README - Quick Install.txt" <<'EOF'
Realtek RTL8723BE PCIe (pci10ec,b723) macOS Wi-Fi Driver & Wireless Utility
===========================================================================

1. Drag "RTL8723BE Wireless Utility.app" to the "Applications" folder.
2. Open "RTL8723BE Wireless Utility.app".
3. In the right-hand panel, click:
   - "Install Kext to OpenCore EFI" (automatically mounts your EFI partition,
     copies RTL8723BEWiFi.kext to EFI/OC/Kexts/, and injects it into config.plist), OR
   - "Load Kext Now (kmutil load)" to test loading immediately.
4. For HP laptops (subsystem 103c:804c), keep the top-right Antenna switch set to
   "Ant #2 (Aux - HP)" for optimal signal reception.
5. Click "Scan Now" to discover 2.4 GHz Wi-Fi networks and click "Connect" to join.
EOF

# 8. Create compressed UDZO DMG and verify
mkdir -p "${PROJECT_DIR}/dist"
rm -f "${DMG_OUT}"
hdiutil create -volname "RTL8723BE Wi-Fi Installer" \
    -srcfolder "${DMG_ROOT}" \
    -ov -format UDZO \
    "${DMG_OUT}"

hdiutil verify "${DMG_OUT}"
echo "[+] DMG Installer successfully built and verified at: ${DMG_OUT}"
