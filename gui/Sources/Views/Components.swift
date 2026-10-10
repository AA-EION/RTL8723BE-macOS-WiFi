// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
// Adapted from TetherKitNext (Issen Software Group) / TetherKit (c) 2026 TetherKit contributors.

import AppKit
import SwiftUI

struct CopyableCommand: View {
    let command: String
    @State private var copied = false

    var body: some View {
        HStack(spacing: Design.Spacing.small) {
            Text(command)
                .font(.system(.callout, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
            Button {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(command, forType: .string)
                copied = true
                Task {
                    try? await Task.sleep(for: .seconds(2))
                    copied = false
                }
            } label: {
                Label(copied ? "Copied" : "Copy", systemImage: copied ? "checkmark" : "doc.on.doc")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(copied ? "Copied to the clipboard" : "Copy command")
        }
        .padding(Design.Spacing.small)
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: Design.Radius.control, style: .continuous))
    }
}

/// Shows the licences that ship inside the app (Resources/Licenses).
struct LicensesSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State private var selection = 0

    private static let documents: [(title: String, file: String)] = [
        ("Overview", "LICENSE"),
        ("Notices", "NOTICE.md"),
        ("MIT", "MIT.txt"),
        ("GPL-2.0", "GPL-2.0-only.txt"),
        ("Firmware", "LicenseRef-rtlwifi-firmware.txt"),
    ]

    private func text(_ file: String) -> String {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("Licenses"),
              let body = try? String(contentsOf: dir.appendingPathComponent(file), encoding: .utf8)
        else { return "Licence file not found." }
        return body
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.medium) {
            Text("Licenses").font(.title3.weight(.semibold))
            Text("App and tools: MIT. Kernel driver: GPL-2.0-only (ported from Linux). Firmware: Realtek.")
                .font(.callout)
                .foregroundStyle(.secondary)
            Picker("Document", selection: $selection) {
                ForEach(Self.documents.indices, id: \.self) { Text(Self.documents[$0].title).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            ScrollView {
                Text(text(Self.documents[selection].file))
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(Design.Spacing.small)
            }
            .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: Design.Radius.control))
            HStack {
                Link("Source code", destination: Vendor.projectURL)
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(Design.Spacing.large)
        .frame(width: 600, height: 520)
    }
}
