#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Issen Software Group
# Renders gui/Resources/Icon/AppIcon.svg into gui/AppIcon.icns and docs/assets/icon.png
# using only macOS tools (AppKit SVG support + iconutil).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/render.swift" <<'SWIFT'
import AppKit
let args = CommandLine.arguments
let svg = URL(fileURLWithPath: args[1]), outDir = URL(fileURLWithPath: args[2])
guard let image = NSImage(contentsOf: svg) else { fatalError("cannot load SVG") }
for (name, px) in [("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32), ("icon_32x32@2x", 64),
                   ("icon_128x128", 128), ("icon_128x128@2x", 256), ("icon_256x256", 256),
                   ("icon_256x256@2x", 512), ("icon_512x512", 512), ("icon_512x512@2x", 1024)] {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    NSGraphicsContext.current?.imageInterpolation = .high
    image.draw(in: NSRect(x: 0, y: 0, width: px, height: px))
    NSGraphicsContext.restoreGraphicsState()
    try! rep.representation(using: .png, properties: [:])!
        .write(to: outDir.appendingPathComponent("\(name).png"))
}
SWIFT
mkdir -p "$WORK/AppIcon.iconset"
swiftc -O "$WORK/render.swift" -o "$WORK/render" 2>/dev/null
"$WORK/render" "$ROOT/gui/Resources/Icon/AppIcon.svg" "$WORK/AppIcon.iconset"
iconutil -c icns "$WORK/AppIcon.iconset" -o "$ROOT/gui/AppIcon.icns"
cp "$WORK/AppIcon.iconset/icon_256x256@2x.png" "$ROOT/docs/assets/icon.png"
echo "Wrote gui/AppIcon.icns and docs/assets/icon.png"
