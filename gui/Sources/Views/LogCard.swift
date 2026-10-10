// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
// Adapted from TetherKitNext (Issen Software Group) / TetherKit (c) 2026 TetherKit contributors.

import AppKit
import SwiftUI

/// The activity log: filterable, auto-scrolling, one-click copy for bug reports.
struct LogCard: View {
    @Bindable var model: AppModel
    @State private var autoScroll = true

    var body: some View {
        Card(title: "Log", systemImage: "text.alignleft") {
            VStack(alignment: .leading, spacing: Design.Spacing.small) {
                HStack(spacing: Design.Spacing.small) {
                    Picker("Level", selection: $model.logLevelFilter) {
                        Text("All").tag(LogLevel.info)
                        Text("Warning").tag(LogLevel.warning)
                        Text("Error").tag(LogLevel.error)
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .frame(maxWidth: 240)
                    Spacer()
                    Toggle("Auto-scroll", isOn: $autoScroll)
                        .toggleStyle(.checkbox)
                        .font(.callout)
                    Button {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(plainText, forType: .string)
                    } label: { Image(systemName: "doc.on.doc") }
                        .buttonStyle(.borderless)
                        .help("Copy all")
                    Button { model.clearLogs() } label: { Image(systemName: "trash") }
                        .buttonStyle(.borderless)
                        .help("Clear")
                }
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 2) {
                            ForEach(model.filteredLogs) { entry in
                                HStack(alignment: .top, spacing: Design.Spacing.small) {
                                    Text(Format.time(entry.date)).foregroundStyle(.tertiary)
                                    Text(entry.level.label)
                                        .foregroundStyle(Design.logColor(for: entry.level))
                                        .frame(width: 46, alignment: .leading)
                                    Text(entry.message)
                                        .foregroundStyle(Design.logColor(for: entry.level))
                                        .textSelection(.enabled)
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                }
                                .font(.system(size: 11, design: .monospaced))
                                .id(entry.id)
                            }
                        }
                        .padding(Design.Spacing.small)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(minHeight: 120, maxHeight: .infinity)
                    .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: Design.Radius.control))
                    .onChange(of: model.filteredLogs.last?.id) { _, newValue in
                        guard autoScroll, let newValue else { return }
                        withAnimation(.easeOut(duration: 0.15)) { proxy.scrollTo(newValue, anchor: .bottom) }
                    }
                }
                Text("Driver messages: /usr/bin/log show --last 10m --predicate 'eventMessage CONTAINS \"RTL8723BE\"'")
                    .font(.caption.monospaced())
                    .foregroundStyle(.tertiary)
                    .textSelection(.enabled)
            }
        }
    }

    private var plainText: String {
        model.filteredLogs
            .map { "\(Format.time($0.date)) [\($0.level.label)] \($0.message)" }
            .joined(separator: "\n")
    }
}
