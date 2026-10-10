// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
// Adapted from TetherKitNext (Issen Software Group) / TetherKit (c) 2026 TetherKit contributors.

import AppKit
import SwiftUI

/// Sidebar destinations.
enum Pane: String, CaseIterable, Identifiable, Hashable {
    case overview, networks, activity, settings

    var id: Self { self }

    var title: String {
        switch self {
        case .overview: return "Overview"
        case .networks: return "Networks"
        case .activity: return "Activity"
        case .settings: return "Settings"
        }
    }

    var symbol: String {
        switch self {
        case .overview: return "gauge.with.dots.needle.67percent"
        case .networks: return "wifi"
        case .activity: return "list.bullet.rectangle"
        case .settings: return "gearshape"
        }
    }
}

struct ContentView: View {
    @Bindable var model: AppModel
    @SceneStorage("selectedPane") private var pane: Pane = .overview

    var body: some View {
        Group {
            if model.driverLoaded {
                mainSplit
            } else {
                OnboardingView(model: model)
            }
        }
        .animation(.smooth(duration: 0.25), value: model.driverLoaded)
        .alert("RTL8723BE Wi-Fi",
               isPresented: Binding(get: { model.alertMessage != nil },
                                    set: { if !$0 { model.alertMessage = nil } })) {
            Button("OK") { model.alertMessage = nil }
        } message: {
            Text(model.alertMessage ?? "")
        }
    }

    private var mainSplit: some View {
        NavigationSplitView {
            List(selection: Binding<Pane?>(get: { pane }, set: { pane = $0 ?? .overview })) {
                Section {
                    ForEach([Pane.overview, .networks, .activity]) { sidebarRow($0) }
                }
                Section {
                    sidebarRow(.settings)
                }
            }
            .navigationSplitViewColumnWidth(min: 190, ideal: 210, max: 260)
            .safeAreaInset(edge: .bottom) {
                SidebarStatus(model: model)
                    .padding(Design.Spacing.small)
            }
        } detail: {
            detail
                .navigationTitle(pane.title)
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        PrimaryActionButton(model: model) { pane = .networks }
                    }
                }
        }
    }

    private func sidebarRow(_ item: Pane) -> some View {
        Label(item.title, systemImage: item.symbol)
            .badge(item == .networks && !model.networks.isEmpty && model.phase != .connected
                   ? Text(verbatim: "\(model.networks.count)") : nil)
            .tag(item)
    }

    @ViewBuilder
    private var detail: some View {
        switch pane {
        case .overview:
            Page {
                StatusHeroCard(model: model)
                ThroughputCard(model: model)
                ConnectionCard(model: model) { pane = .networks }
            }
        case .networks:
            Page { NetworksCard(model: model) }
        case .activity:
            LogCard(model: model)
                .padding(Design.Spacing.medium)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        case .settings:
            SettingsPage(model: model)
        }
    }
}

/// Scrollable, width-capped page column.
struct Page<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        ScrollView {
            VStack(spacing: Design.Spacing.gutter) {
                content()
            }
            .frame(maxWidth: Design.Window.contentMaxWidth)
            .padding(Design.Spacing.medium)
            .frame(maxWidth: .infinity)
        }
        .scrollBounceBehavior(.basedOnSize)
    }
}

/// The one primary action, always in the toolbar: Scan, or Disconnect while connected.
struct PrimaryActionButton: View {
    @Bindable var model: AppModel
    var showNetworks: () -> Void = {}

    var body: some View {
        let connected = model.phase == .connected
        Button {
            Task {
                if connected {
                    await model.disconnect()
                } else {
                    showNetworks()
                    await model.scan()
                }
            }
        } label: {
            HStack(spacing: Design.Spacing.tight) {
                if model.isScanning || model.isBusy || model.phase == .connecting {
                    ProgressView().controlSize(.small)
                } else {
                    Image(systemName: connected ? "stop.fill" : "antenna.radiowaves.left.and.right")
                }
                Text(connected ? "Disconnect" : "Scan")
            }
            .padding(.horizontal, 4)
        }
        .primaryActionButtonStyle()
        .tint(connected ? .red : .accentColor)
        .disabled(!model.driverLoaded || model.isScanning || model.isBusy || model.phase == .connecting)
        .keyboardShortcut(.return, modifiers: .command)
    }
}

/// Compact live status at the bottom of the sidebar.
private struct SidebarStatus: View {
    var model: AppModel

    var body: some View {
        HStack(spacing: Design.Spacing.small) {
            Circle()
                .fill(Design.accent(for: model.phase))
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 1) {
                Text(model.phase == .connected ? model.ssid : model.phase.label)
                    .font(.callout.weight(.medium))
                    .lineLimit(1)
                if model.phase == .connected {
                    Text("↓ \(Format.bitrate(model.throughput.receiveBitsPerSecond))  "
                         + "↑ \(Format.bitrate(model.throughput.transmitBitsPerSecond))")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, Design.Spacing.small)
        .padding(.vertical, Design.Spacing.tight)
        .glassSurface(cornerRadius: Design.Radius.control,
                      tint: model.phase == .connected ? .green : nil)
    }
}
