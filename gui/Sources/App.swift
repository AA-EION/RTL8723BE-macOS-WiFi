// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

import AppKit
import SwiftUI

@main
struct RTL8723BEWiFiApp: App {
    @State private var model = AppModel()

    var body: some Scene {
        Window("RTL8723BE Wi-Fi", id: "main") {
            ContentView(model: model)
                .frame(minWidth: Design.Window.minWidth, minHeight: Design.Window.minHeight)
        }
        .defaultSize(width: Design.Window.defaultWidth, height: Design.Window.defaultHeight)
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About RTL8723BE Wi-Fi") { Vendor.showAboutPanel() }
            }
            CommandGroup(after: .newItem) {
                Button("Scan for Networks") { Task { await model.scan() } }
                    .keyboardShortcut("r", modifiers: .command)
                    .disabled(!model.driverLoaded || model.isScanning)
            }
        }

        MenuBarExtra {
            MenuBarPanel(model: model)
        } label: {
            Image(systemName: Design.statusSymbol(for: model.phase))
        }
        .menuBarExtraStyle(.window)
    }
}

/// Brings the main window forward (also used from the menu bar panel).
@MainActor
func presentMainWindow(_ openWindow: OpenWindowAction) {
    NSApp.activate()
    openWindow(id: "main")
}
