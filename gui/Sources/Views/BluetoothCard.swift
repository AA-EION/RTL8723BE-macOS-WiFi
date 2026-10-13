// SPDX-License-Identifier: MIT
import SwiftUI

struct BluetoothCard: View {
    var model: AppModel
    var body: some View {
        Section("Bluetooth") {
            LabeledContent("RTL8723B USB radio", value: model.bluetoothPresent ? "Detected" : "Not detected")
            LabeledContent("Firmware", value: model.bluetoothFirmwareLoaded ? "Loaded" : "Upload not confirmed")
            LabeledContent("macOS controller", value: model.bluetoothControllerAvailable ? (model.bluetoothPowered ? "On" : "Off") : "Unavailable")
            HStack {
                Button("Load Bluetooth Driver…") { Task { await model.loadBluetoothDriver() } }
                    .disabled(model.isBusy || !model.bluetoothPresent || model.bluetoothFirmwareLoaded)
                Button("Bluetooth Settings…") { model.openBluetoothSettings() }
                Button("Bluetooth Guide…") {
                    if let path = Bundle.main.path(forResource: "Bluetooth Guide", ofType: "txt") {
                        NSWorkspace.shared.open(URL(fileURLWithPath: path))
                    }
                }
            }
            Text("Load firmware after login with the administrator prompt. Pairing and power use macOS; native Realtek Bluetooth requires Lilu and BlueToolFixup on macOS 12 or later. See the Bluetooth guide.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }
}
