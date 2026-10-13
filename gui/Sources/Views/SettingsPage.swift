// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
// Adapted from TetherKitNext (Issen Software Group) / TetherKit (c) 2026 TetherKit contributors.

import AppKit
import ServiceManagement
import SwiftUI

/// Everything about the driver and the app itself.
struct SettingsPage: View {
    @Bindable var model: AppModel
    @State private var launchAtLogin = SMAppService.mainApp.status == .enabled
    @State private var showingLicenses = false
    @State private var copiedPayPal = false

    var body: some View {
        Form {
            driverSection
            performanceSection
            BluetoothCard(model: model)
            antennaSection
            commandLineSection
            savedNetworksSection
            generalSection
            donationSection
            aboutSection
        }
        .formStyle(.grouped)
        .frame(maxWidth: Design.Window.contentMaxWidth)
        .frame(maxWidth: .infinity)
        .sheet(isPresented: $showingLicenses) { LicensesSheet() }
    }

    // MARK: Driver

    private var driverSection: some View {
        Section("Driver") {
            LabeledContent {
                Text(model.driverVersion.map { "Loaded · v\($0)" } ?? "Loaded")
                    .foregroundStyle(.secondary)
            } label: {
                Label("RTL8723BEWiFi.kext", systemImage: "checkmark.shield")
            }
            if let d = model.diag {
                LabeledContent("Firmware", value: "v\(d.fw_version >> 16).\(d.fw_version & 0xFFFF)")
                LabeledContent("Hardware MAC", value: model.macAddress)
                LabeledContent("Received frames", value: Format.count(UInt64(d.rx_desc_done)))
            }
            HStack {
                Button(model.isBusy ? "Working…" : "Reload Driver…") {
                    Task { await model.loadDriver() }
                }
                .disabled(model.isBusy)
                .help("Unloads and loads the driver again (administrator password required)")
                Spacer()
                Button("Privacy & Security…") {
                    NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security")!)
                }
            }
        }
    }

    private var performanceSection: some View {
        Section("Wi-Fi performance") {
            if let info = model.linkInfo {
                Toggle("802.11n (HT20)", isOn: Binding(
                    get: { info.options & UInt32(kRTLOptionHT.rawValue) != 0 },
                    set: { model.setDriverOption(UInt32(kRTLOptionHT.rawValue), enabled: $0) }))
                Toggle("Hardware transmit encryption", isOn: Binding(
                    get: { info.options & UInt32(kRTLOptionTxCrypto.rawValue) != 0 },
                    set: { model.setDriverOption(UInt32(kRTLOptionTxCrypto.rawValue), enabled: $0) }))
                Toggle("Share the single antenna with Bluetooth", isOn: Binding(
                    get: { info.options & UInt32(kRTLOptionBluetoothCoexist.rawValue) != 0 },
                    set: { model.setDriverOption(UInt32(kRTLOptionBluetoothCoexist.rawValue), enabled: $0) }))
                LabeledContent("Radio mode", value: info.ht20 != 0 ? "802.11n · HT20" : "802.11b/g")
                LabeledContent("Rate control", value: info.firmware_rate_control != 0 ? "Firmware" : "Driver")
                LabeledContent("Transmit encryption", value: info.tx_crypto != 0 ? "Hardware CCMP" : "Software CCMP")
            }
            Text("Change options while disconnected. Hardware encryption and single-antenna Bluetooth sharing are experimental; receive authentication and replay protection remain in software.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .disabled(model.phase != .disconnected || model.isBusy)
    }

    // MARK: Antenna

    private var antennaSection: some View {
        Section {
            Picker("Wi-Fi antenna", selection: Binding(get: { model.antenna },
                                                        set: { model.setAntenna($0) })) {
                Text("Main (S1)").tag(1)
                Text("Aux (S0)").tag(2)
            }
            .pickerStyle(.segmented)
        } header: {
            Text("Antenna")
        } footer: {
            Text("The first scan after the driver loads measures both RF paths and keeps the better one. "
                 + "Many HP boards have their single antenna on Aux.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Command-line tool

    private var commandLineSection: some View {
        Section {
            CopyableCommand(command: "rtl8723be_cli scan")
            CopyableCommand(command: "rtl8723be_cli connect \"Network\" 'password'")
            CopyableCommand(command: "rtl8723be_cli diag")
        } header: {
            Text("Command-line tool")
        } footer: {
            Text("/usr/local/bin/rtl8723be_cli")
                .font(.caption.monospaced())
                .foregroundStyle(.tertiary)
        }
    }

    private var savedNetworksSection: some View {
        Section("Saved networks") {
            Toggle("Automatically join remembered networks", isOn: $model.autoJoin)
            ForEach(model.savedNetworks, id: \.self) { ssid in
                HStack {
                    Text(ssid)
                    Spacer()
                    Button("Forget") { model.forgetNetwork(ssid) }
                }
            }
            Text("Passwords are stored in your login Keychain after a successful connection. Disconnect pauses automatic joining until you join manually or reopen the app.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    // MARK: General

    private var generalSection: some View {
        Section("General") {
            Toggle("Open at login", isOn: $launchAtLogin)
                .onChange(of: launchAtLogin) { _, enabled in setLaunchAtLogin(enabled) }
            LabeledContent {
                Button("Check for Updates…") { NSWorkspace.shared.open(Vendor.releasesURL) }
            } label: {
                Text("Version \(Vendor.appVersion)")
            }
        }
    }

    private func setLaunchAtLogin(_ enabled: Bool) {
        let service = SMAppService.mainApp
        do {
            if enabled { try service.register() } else { try service.unregister() }
        } catch {
            model.alertMessage = "Could not change the login item: \(error.localizedDescription)"
        }
        let actual = service.status == .enabled
        if actual != launchAtLogin { launchAtLogin = actual }
    }

    // MARK: Support & Donations

    private var donationSection: some View {
        Section("Support & Donations") {
            VStack(alignment: .leading, spacing: Design.Spacing.small) {
                Label("Support RTL8723BE Wi-Fi Development", systemImage: "heart.fill")
                    .foregroundStyle(.pink)
                    .font(.headline)

                Text("Issen Software Group is managed solely by me, and for now, software development depends on "
                     + "donations to stay alive. After Issen Software Group reaches certain sustainability and income "
                     + "goals, the free apps will stop asking for donations.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                HStack(spacing: Design.Spacing.small) {
                    Button {
                        NSWorkspace.shared.open(Vendor.donationURL)
                    } label: {
                        Label("Donate via PayPal", systemImage: "heart.fill")
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.pink)

                    Button(copiedPayPal ? "Copied" : "Copy PayPal Email (\(Vendor.donationEmail))") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(Vendor.donationEmail, forType: .string)
                        copiedPayPal = true
                        Task {
                            try? await Task.sleep(for: .seconds(2))
                            copiedPayPal = false
                        }
                    }
                    .buttonStyle(.bordered)
                }
                .padding(.top, Design.Spacing.tight)
            }
            .padding(.vertical, 4)
        }
    }

    // MARK: About

    private var aboutSection: some View {
        Section("About") {
            Text("Made by Issen Software Group. The app is MIT-licensed; the driver is GPL-2.0-only, "
                 + "ported from the Linux rtlwifi rtl8723be driver. Firmware © Realtek Semiconductor Corp.")
                .font(.callout)
                .foregroundStyle(.secondary)
            LabeledContent("Made by Issen Software Group") {
                Link("Issen Software Group Website", destination: Vendor.website)
            }
            HStack {
                Link("Project Website", destination: Vendor.projectURL)
                Spacer()
                Button("Show Licenses") { showingLicenses = true }
            }
        }
    }
}
