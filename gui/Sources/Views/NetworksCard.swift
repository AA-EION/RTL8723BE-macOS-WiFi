// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

import SwiftUI

/// Nearby networks from the last scan; click one to join.
struct NetworksCard: View {
    @Bindable var model: AppModel
    @State private var joinTarget: WiFiNetwork?

    var body: some View {
        Card(title: "Nearby networks", systemImage: "wifi",
             accessory: AnyView(accessory)) {
            if model.networks.isEmpty {
                empty
            } else {
                VStack(spacing: 0) {
                    ForEach(model.networks) { network in
                        NetworkRow(network: network,
                                   isCurrent: model.phase == .connected && network.bssid == model.bssid,
                                   join: {
                                       if let password = model.savedPassword(for: network) {
                                           Task { await model.join(network, password: password) }
                                       } else { joinTarget = network }
                                   },
                                   disconnect: { Task { await model.disconnect() } })
                        .contextMenu {
                            Button("Join with another password…") { joinTarget = network }
                                .disabled(!network.secured || model.isBusy)
                            if model.savedNetworks.contains(network.ssid) {
                                Button("Forget this network") { model.forgetNetwork(network.ssid) }
                            }
                        }
                        if network.id != model.networks.last?.id {
                            Divider().padding(.leading, 40)
                        }
                    }
                }
            }
        }
        .sheet(item: $joinTarget) { network in
            JoinSheet(network: network) { password, remember in
                Task { await model.join(network, password: password, remember: remember) }
            }
        }
    }

    private var accessory: some View {
        HStack(spacing: Design.Spacing.small) {
            if let last = model.lastScan {
                Text("Updated \(last.formatted(date: .omitted, time: .shortened))")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
            Button {
                Task { await model.scan() }
            } label: {
                if model.isScanning {
                    ProgressView().controlSize(.small)
                } else {
                    Image(systemName: "arrow.clockwise")
                }
            }
            .buttonStyle(.borderless)
            .disabled(model.isScanning)
            .help("Scan again")
        }
    }

    private var empty: some View {
        VStack(spacing: Design.Spacing.small) {
            Image(systemName: model.isScanning ? "antenna.radiowaves.left.and.right" : "wifi.exclamationmark")
                .font(.system(size: 40, weight: .light))
                .foregroundStyle(.secondary)
                .symbolEffect(.variableColor.iterative, isActive: model.isScanning)
            Text(model.isScanning ? "Scanning…" : "No networks yet")
                .font(.title3.weight(.semibold))
            Text("The first scan also picks the antenna that hears best (about 3 seconds).")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            if !model.isScanning {
                Button("Scan") { Task { await model.scan() } }
                    .primaryActionButtonStyle()
                    .padding(.top, Design.Spacing.tight)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, Design.Spacing.large)
    }
}

private struct NetworkRow: View {
    let network: WiFiNetwork
    let isCurrent: Bool
    let join: () -> Void
    let disconnect: () -> Void
    @State private var hovering = false

    var body: some View {
        HStack(spacing: Design.Spacing.small) {
            Image(systemName: "wifi", variableValue: network.signalLevel)
                .font(.title3)
                .foregroundStyle(isCurrent ? Color.green : Color.accentColor)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    Text(network.displayName)
                        .font(.body.weight(isCurrent ? .semibold : .regular))
                    if network.secured {
                        Image(systemName: "lock.fill")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
                Text("Channel \(network.channel) · \(network.rssi) dBm · \(network.bssid)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if isCurrent {
                StatusBadge(text: "Connected", color: .green)
                Button("Disconnect", action: disconnect)
                    .secondaryActionButtonStyle()
                    .controlSize(.small)
            } else if hovering {
                Button("Join", action: join)
                    .secondaryActionButtonStyle()
                    .controlSize(.small)
                    .disabled(!network.secured)
                    .help(network.secured ? "" : "Only WPA2-PSK networks are supported")
            }
        }
        .padding(.vertical, Design.Spacing.tight)
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture(count: 2) { if !isCurrent && network.secured { join() } }
    }
}

private struct JoinSheet: View {
    let network: WiFiNetwork
    let join: (String, Bool) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var password = ""
    @State private var remember = true

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.medium) {
            HStack(spacing: Design.Spacing.small) {
                Image(systemName: "wifi", variableValue: network.signalLevel)
                    .font(.largeTitle)
                    .foregroundStyle(Color.accentColor)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Join “\(network.displayName)”")
                        .font(.headline)
                    Text("WPA2 Personal · channel \(network.channel)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            SecureField("Password", text: $password)
                .textFieldStyle(.roundedBorder)
                .onSubmit(submit)
            Toggle("Remember this network in Keychain", isOn: $remember)
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Join", action: submit)
                    .primaryActionButtonStyle()
                    .keyboardShortcut(.defaultAction)
                    .disabled(!NetworkCredentials.validPassword(password))
            }
        }
        .padding(Design.Spacing.large)
        .frame(width: 380)
    }

    private func submit() {
        guard NetworkCredentials.validPassword(password) else { return }
        join(password, remember)
        dismiss()
    }
}
