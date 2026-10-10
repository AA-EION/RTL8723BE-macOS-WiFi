// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
// Adapted from TetherKitNext (Issen Software Group) / TetherKit (c) 2026 TetherKit contributors.

import AppKit
import SwiftUI

/// The small panel behind the menu bar icon: status at a glance plus quick actions.
struct MenuBarPanel: View {
    var model: AppModel
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.small) {
            HStack(spacing: Design.Spacing.tight) {
                Circle().fill(Design.accent(for: model.phase)).frame(width: 8, height: 8)
                Text(model.phase == .connected ? model.ssid : model.phase.label)
                    .font(.headline)
                    .lineLimit(1)
                Spacer()
                if let duration = model.connectedDuration {
                    Text(Format.duration(duration))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }

            if model.phase == .connected {
                speedRow("arrow.down", "Down", model.throughput.receiveBitsPerSecond, .blue)
                speedRow("arrow.up", "Up", model.throughput.transmitBitsPerSecond, .purple)
                Text("\(model.interfaceName) · \(model.ipAddress.isEmpty ? "no IP yet" : model.ipAddress)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
            } else if !model.networks.isEmpty {
                Text("\(model.networks.count) networks nearby")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Divider()
            PrimaryActionButton(model: model)
                .controlSize(.large)
                .frame(maxWidth: .infinity)
            HStack {
                Button("Open RTL8723BE Wi-Fi") {
                    dismiss()
                    presentMainWindow(openWindow)
                }
                .secondaryActionButtonStyle()
                .controlSize(.small)
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
                    .controlSize(.small)
            }
        }
        .padding(Design.Spacing.medium)
        .frame(width: 280)
    }

    private func speedRow(_ symbol: String, _ caption: String, _ bps: Double, _ tint: Color) -> some View {
        HStack(spacing: Design.Spacing.small) {
            Image(systemName: symbol).font(.caption).foregroundStyle(tint).frame(width: 14)
            Text(caption).font(.callout).foregroundStyle(.secondary)
            Spacer()
            Text(Format.bitrate(bps)).font(.callout.monospacedDigit()).foregroundStyle(tint)
        }
    }
}
