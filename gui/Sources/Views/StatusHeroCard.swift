// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
// Adapted from TetherKitNext (Issen Software Group) / TetherKit (c) 2026 TetherKit contributors.

import SwiftUI

/// The main status card: is it connected, to what, and with which address.
struct StatusHeroCard: View {
    @Bindable var model: AppModel

    private var accent: Color { Design.accent(for: model.phase) }

    var body: some View {
        HStack(alignment: .center, spacing: Design.Spacing.large) {
            StatusRing(phase: model.phase, accent: accent)

            VStack(alignment: .leading, spacing: Design.Spacing.small) {
                HStack(spacing: Design.Spacing.small) {
                    Text(model.phase == .connected ? model.ssid : model.phase.label)
                        .font(.system(.title2, design: .rounded).weight(.semibold))
                        .lineLimit(1)
                    if model.phase == .connected {
                        StatusBadge(text: "WPA2", color: .green)
                    }
                }
                Text(subline)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if model.phase == .connected {
                    HStack(spacing: Design.Spacing.large) {
                        inlineMetric("Interface", model.interfaceName.isEmpty ? "—" : model.interfaceName)
                        inlineMetric("IP address", model.ipAddress.isEmpty ? "Waiting for DHCP…" : model.ipAddress)
                        if let duration = model.connectedDuration {
                            inlineMetric("Connected", Format.duration(duration))
                        }
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(Design.Spacing.large)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface(cornerRadius: Design.Radius.hero,
                      tint: model.phase == .disconnected ? nil : accent)
    }

    private var subline: String {
        switch model.phase {
        case .connected:
            return "Channel \(model.channel) · \(model.rssi) dBm · \(model.antenna == 2 ? "AUX" : "MAIN") antenna"
        case .connecting:
            return "Authenticating and running the WPA2 handshake…"
        case .scanning:
            return "Listening on 2.4 GHz channels 1–13…"
        case .disconnected:
            return model.networks.isEmpty
                ? "Click Scan to look for nearby 2.4 GHz networks."
                : "\(model.networks.count) networks nearby. Pick one on the Networks page."
        case .noDriver:
            return "The RTL8723BE driver is not loaded."
        }
    }

    private func inlineMetric(_ caption: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(caption)
                .font(.caption2)
                .foregroundStyle(.tertiary)
            Text(value)
                .font(.system(.callout, design: .monospaced))
                .textSelection(.enabled)
        }
    }
}

/// While connected the inner circle breathes; transitional states spin an arc.
private struct StatusRing: View {
    let phase: LinkPhase
    let accent: Color

    @State private var rotation: Double = 0
    @State private var breathing = false

    private var isActive: Bool { phase == .connected }
    private var isWorking: Bool { phase == .scanning || phase == .connecting }

    var body: some View {
        ZStack {
            Circle()
                .fill(accent.opacity(0.12))
                .frame(width: 64, height: 64)
                .scaleEffect(breathing && isActive ? 1.08 : 1.0)
            Circle()
                .stroke(accent.opacity(0.25), lineWidth: 3)
                .frame(width: 64, height: 64)
            Circle()
                .trim(from: 0, to: isWorking ? 0.25 : (isActive ? 1.0 : 0.0))
                .stroke(accent, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                .frame(width: 64, height: 64)
                .rotationEffect(.degrees(isWorking ? rotation : -90))
            Image(systemName: Design.statusSymbol(for: phase))
                .font(.system(size: 22, weight: .medium))
                .foregroundStyle(accent)
                .symbolRenderingMode(.hierarchical)
        }
        .frame(width: 72, height: 72)
        .onAppear { restartAnimations() }
        .onChange(of: phase) { _, _ in restartAnimations() }
    }

    private func restartAnimations() {
        if isWorking {
            rotation = 0
            withAnimation(.linear(duration: 1.1).repeatForever(autoreverses: false)) { rotation = 360 }
        } else {
            rotation = 0
        }
        withAnimation(.easeInOut(duration: 1.8).repeatForever(autoreverses: true)) { breathing = isActive }
    }
}
