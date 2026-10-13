// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

import AppKit
import SwiftUI

/// Shown until the driver is running: walks through the three things it needs.
struct OnboardingView: View {
    @Bindable var model: AppModel

    var body: some View {
        ScrollView {
            VStack(spacing: Design.Spacing.large) {
                VStack(spacing: Design.Spacing.small) {
                    if let icon = NSApp.applicationIconImage {
                        Image(nsImage: icon)
                            .resizable()
                            .frame(width: 96, height: 96)
                    }
                    Text("RTL8723BE Wi-Fi")
                        .font(.system(.largeTitle, design: .rounded).weight(.semibold))
                    Text("Wi-Fi for the Realtek RTL8723BE card on macOS.")
                        .font(.title3)
                        .foregroundStyle(.secondary)
                    Text("By Issen Software Group")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
                .padding(.top, Design.Spacing.large)

                VStack(spacing: Design.Spacing.gutter) {
                    step(1, done: model.hardwarePresent, title: "Wi-Fi card detected",
                         detail: model.hardwarePresent
                            ? "Realtek RTL8723BE (pci10ec,b723) found."
                            : "No RTL8723BE was found on the PCI bus. This app only supports that card.")
                    step(2, done: model.bootArgPresent, title: "Boot flag set",
                         detail: model.bootArgPresent
                            ? "rtl8723be_experimental=1 is present."
                            : "Add rtl8723be_experimental=1 to boot-args in your OpenCore config.plist, then restart. "
                              + "This app never changes NVRAM or your EFI.")
                    step(3, done: model.driverLoaded, title: "Driver running",
                         detail: "Load the driver after each login. The first time (and after updates) macOS asks you "
                               + "to allow it in System Settings › Privacy & Security; allow it, restart, and load it again.")
                }
                .frame(maxWidth: 560)

                HStack(spacing: Design.Spacing.small) {
                    Button {
                        Task { await model.loadDriver() }
                    } label: {
                        HStack(spacing: Design.Spacing.tight) {
                            if model.isBusy { ProgressView().controlSize(.small) }
                            Text("Load Driver…")
                        }
                        .padding(.horizontal, 6)
                    }
                    .primaryActionButtonStyle()
                    .controlSize(.large)
                    .disabled(model.isBusy || !model.hardwarePresent || !model.bootArgPresent)

                    Button("Privacy & Security…") {
                        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security")!)
                    }
                    .secondaryActionButtonStyle()
                    .controlSize(.large)
                }

                BluetoothCard(model: model).frame(maxWidth: 560)

                if let reason = blockedReason {
                    Label(reason, systemImage: "info.circle")
                        .font(.callout)
                        .foregroundStyle(.orange)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 560)
                } else if let last = model.logs.last, last.level != .info {
                    Text(last.message)
                        .font(.callout)
                        .foregroundStyle(.orange)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 560)
                }
            }
            .padding(Design.Spacing.large)
            .frame(maxWidth: .infinity)
        }
    }

    /// Why "Load Driver…" is disabled, so the button never greys out silently.
    private var blockedReason: String? {
        if !model.hardwarePresent {
            return "No RTL8723BE card was found, so there is nothing to load the driver for."
        }
        if !model.bootArgPresent {
            return "This Mac was started without rtl8723be_experimental=1 in boot-args, so the driver "
                + "would refuse to start. Restart using an OpenCore EFI whose boot-args include it."
        }
        return nil
    }

    private func step(_ n: Int, done: Bool, title: String, detail: String) -> some View {
        HStack(alignment: .top, spacing: Design.Spacing.medium) {
            ZStack {
                Circle().fill(done ? Color.green.opacity(0.18) : Color.secondary.opacity(0.12))
                if done {
                    Image(systemName: "checkmark").font(.callout.weight(.bold)).foregroundStyle(.green)
                } else {
                    Text("\(n)").font(.callout.weight(.semibold)).foregroundStyle(.secondary)
                }
            }
            .frame(width: 30, height: 30)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.headline)
                Text(detail)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
            }
            Spacer(minLength: 0)
        }
        .padding(Design.Spacing.section)
        .contentSurface()
    }
}
