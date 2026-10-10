// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

import SwiftUI

/// Live throughput of the Wi-Fi interface, measured from the interface byte counters.
struct ThroughputCard: View {
    var model: AppModel

    var body: some View {
        Card(title: "Throughput", systemImage: "chart.xyaxis.line") {
            Grid(alignment: .leading, horizontalSpacing: Design.Spacing.large,
                 verticalSpacing: Design.Spacing.medium) {
                GridRow {
                    MetricTile(caption: "Download", value: Format.bitrate(model.throughput.receiveBitsPerSecond),
                               systemImage: "arrow.down", tint: .blue)
                    MetricTile(caption: "Upload", value: Format.bitrate(model.throughput.transmitBitsPerSecond),
                               systemImage: "arrow.up", tint: .purple)
                }
                GridRow {
                    MetricTile(caption: "Total received", value: Format.bytes(model.throughput.totalReceived))
                    MetricTile(caption: "Total sent", value: Format.bytes(model.throughput.totalSent))
                }
            }
        }
    }
}

/// Read-only connection details, linking to the Networks page.
struct ConnectionCard: View {
    var model: AppModel
    let showNetworks: () -> Void

    var body: some View {
        Card(title: "Connection", systemImage: "network",
             accessory: AnyView(Button("Choose network", action: showNetworks).buttonStyle(.link))) {
            Grid(alignment: .leading, horizontalSpacing: Design.Spacing.large,
                 verticalSpacing: Design.Spacing.tight) {
                row("Network", model.phase == .connected ? model.ssid : "—")
                row("BSSID", model.phase == .connected ? model.bssid : "—")
                row("Channel", model.channel > 0 ? "\(model.channel) (2.4 GHz)" : "—")
                row("Signal", model.phase == .connected ? "\(model.rssi) dBm" : "—")
                row("Interface", model.interfaceName.isEmpty ? "—" : model.interfaceName)
                row("IP address", model.ipAddress.isEmpty ? "—" : model.ipAddress)
                row("MAC address", model.macAddress.isEmpty ? "—" : model.macAddress)
            }
        }
    }

    private func row(_ caption: String, _ value: String) -> some View {
        GridRow {
            Text(caption)
                .font(.callout)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.system(.callout, design: .monospaced))
                .textSelection(.enabled)
        }
    }
}
