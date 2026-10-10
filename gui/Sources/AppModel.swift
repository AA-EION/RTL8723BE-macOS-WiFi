// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

import AppKit
import Darwin
import Observation

/// What the radio is doing, from the user's point of view.
enum LinkPhase: Equatable {
    case noDriver, disconnected, scanning, connecting, connected

    var label: String {
        switch self {
        case .noDriver: return "Driver not loaded"
        case .disconnected: return "Not connected"
        case .scanning: return "Scanning…"
        case .connecting: return "Connecting…"
        case .connected: return "Connected"
        }
    }
}

enum LogLevel: Int, CaseIterable, Comparable {
    case info, warning, error
    static func < (a: LogLevel, b: LogLevel) -> Bool { a.rawValue < b.rawValue }
    var label: String {
        switch self {
        case .info: return "INFO"
        case .warning: return "WARN"
        case .error: return "ERROR"
        }
    }
}

struct LogEntry: Identifiable {
    let id = UUID()
    let date: Date
    let level: LogLevel
    let message: String
}

struct WiFiNetwork: Identifiable, Hashable {
    var id: String { bssid }
    let ssid: String
    let bssid: String
    let bssidBytes: [UInt8]
    let channel: Int
    let rssi: Int
    let secured: Bool

    var displayName: String { ssid.isEmpty ? "Hidden network" : ssid }
    /// 0...1 for the variable-value Wi-Fi symbol.
    var signalLevel: Double {
        switch rssi {
        case (-55)...: return 1.0
        case (-67)...: return 0.7
        case (-78)...: return 0.4
        default: return 0.15
        }
    }
}

struct Throughput {
    var receiveBitsPerSecond: Double = 0
    var transmitBitsPerSecond: Double = 0
    var totalReceived: UInt64 = 0
    var totalSent: UInt64 = 0
}

/// Single source of truth for the UI. Talks to RTL8723BEWiFi.kext through its IOUserClient.
@Observable
@MainActor
final class AppModel {
    // Environment / onboarding
    var hardwarePresent = false
    var bootArgPresent = false
    var driverLoaded = false
    var driverVersion: String?

    // Link
    var phase: LinkPhase = .noDriver
    var macAddress = ""
    var ssid = ""
    var bssid = ""
    var channel = 0
    var rssi = -100
    var antenna = 2
    var interfaceName = ""
    var ipAddress = ""
    var connectedSince: Date?
    var throughput = Throughput()

    // Scan / join
    var networks: [WiFiNetwork] = []
    var isScanning = false
    var lastScan: Date?
    var joiningNetwork: WiFiNetwork?
    var isBusy = false
    var alertMessage: String?

    // Diagnostics
    var diag: RTL8723BEDiag?

    // Log
    var logs: [LogEntry] = []
    var logLevelFilter: LogLevel = .info
    var filteredLogs: [LogEntry] { logs.filter { $0.level >= logLevelFilter } }

    private var timer: Timer?
    private var lastCounters: (rx: UInt64, tx: UInt64, at: Date)?
    private var lastState: UInt32 = .max

    init() {
        log("RTL8723BE Wi-Fi \(Vendor.appVersion) started on macOS "
            + ProcessInfo.processInfo.operatingSystemVersionString)
        refreshEnvironment()
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
        }
    }

    var connectedDuration: TimeInterval? {
        connectedSince.map { Date().timeIntervalSince($0) }
    }

    // MARK: - Logging

    func log(_ message: String, _ level: LogLevel = .info) {
        logs.append(LogEntry(date: Date(), level: level, message: message))
        if logs.count > 500 { logs.removeFirst(logs.count - 500) }
    }

    func clearLogs() { logs.removeAll() }

    // MARK: - Polling

    func refreshEnvironment() {
        hardwarePresent = rtl_hardware_present() == 1
        var size = 0
        sysctlbyname("kern.bootargs", nil, &size, nil, 0)
        var buffer = [CChar](repeating: 0, count: max(size, 1))
        sysctlbyname("kern.bootargs", &buffer, &size, nil, 0)
        let bootArgs = String(cString: buffer)
        bootArgPresent = bootArgs.split(separator: " ").contains("rtl8723be_experimental=1")
    }

    func refresh() {
        var conn: io_connect_t = 0
        guard rtl_open_client(&conn) == 0 else {
            if driverLoaded { log("Driver is no longer loaded.", .warning) }
            driverLoaded = false
            phase = .noDriver
            interfaceName = ""
            ipAddress = ""
            connectedSince = nil
            return
        }
        defer { rtl_close_client(conn) }
        if !driverLoaded {
            driverLoaded = true
            log("Connected to RTL8723BEWiFi.kext.")
            Task { await loadDriverVersion() }
        }

        var st = RTL8723BEStatus()
        guard rtl_get_status(conn, &st) == 0 else { return }
        macAddress = Self.mac(st.mac)
        bssid = Self.mac(st.bssid)
        ssid = withUnsafeBytes(of: st.ssid) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        channel = Int(st.channel)
        rssi = Int(st.rssi)
        antenna = st.antenna == 2 ? 2 : 1

        if st.state != lastState {
            if st.state == 5 {
                connectedSince = Date()
                log("Connected to \(ssid) (channel \(channel)).")
            } else if lastState == 5 {
                connectedSince = nil
                log("Disconnected from the network.", .warning)
            }
            lastState = st.state
        }
        switch st.state {
        case 5: phase = .connected
        case 2, 3, 4: phase = .connecting
        case 1: phase = .scanning
        default: phase = isScanning ? .scanning : .disconnected
        }

        var diagnostics = RTL8723BEDiag()
        if rtl_get_diag(conn, &diagnostics) == 0 { diag = diagnostics }

        updateInterface()
    }

    private func updateInterface() {
        var name = [CChar](repeating: 0, count: 32)
        interfaceName = rtl_copy_bsd_name(&name, name.count) == 0 ? String(cString: name) : ""
        guard !interfaceName.isEmpty else { return }

        var ifap: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifap) == 0, let first = ifap else { return }
        defer { freeifaddrs(ifap) }
        var address = ""
        var rx: UInt64 = 0, tx: UInt64 = 0
        for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let ifa = ptr.pointee
            guard String(cString: ifa.ifa_name) == interfaceName, let sa = ifa.ifa_addr else { continue }
            if sa.pointee.sa_family == UInt8(AF_INET) {
                var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
                address = String(cString: host)
            } else if sa.pointee.sa_family == UInt8(AF_LINK), let data = ifa.ifa_data {
                let stats = data.assumingMemoryBound(to: if_data.self).pointee
                rx = UInt64(stats.ifi_ibytes)
                tx = UInt64(stats.ifi_obytes)
            }
        }
        ipAddress = address
        // macOS 26 leaves if_data input counters at zero for IOKit Ethernet drivers;
        // the driver's own counters (diagnostics v3+) are authoritative when present.
        if let d = diag, d.version >= 3 {
            rx = d.rx_bytes
            tx = d.tx_bytes
        }
        let now = Date()
        if let last = lastCounters, rx >= last.rx, tx >= last.tx {
            let dt = max(now.timeIntervalSince(last.at), 0.1)
            throughput.receiveBitsPerSecond = Double(rx - last.rx) * 8 / dt
            throughput.transmitBitsPerSecond = Double(tx - last.tx) * 8 / dt
        }
        throughput.totalReceived = rx
        throughput.totalSent = tx
        lastCounters = (rx, tx, now)
    }

    private func loadDriverVersion() async {
        let output = await Self.run("/usr/bin/kmutil", ["showloaded", "--list-only", "--bundle-identifier",
                                                         "com.rtl8723be.macos.wifi"])
        if let range = output.range(of: #"\(([0-9.]+)\)"#, options: .regularExpression) {
            driverVersion = String(output[range].dropFirst().dropLast())
        }
    }

    // MARK: - Actions

    func scan() async {
        guard driverLoaded, !isScanning else { return }
        isScanning = true
        phase = .scanning
        log("Scanning 2.4 GHz channels 1–13…")
        let result: (Int32, [WiFiNetwork]) = await Task.detached {
            var conn: io_connect_t = 0
            guard rtl_open_client(&conn) == 0 else { return (-1, []) }
            defer { rtl_close_client(conn) }
            let rc = rtl_trigger_scan(conn)
            var raw = RTL8723BEScanResults()
            guard rc == 0, rtl_get_scan_results(conn, &raw) == 0 else { return (rc, []) }
            return (0, Self.parse(raw))
        }.value
        isScanning = false
        if result.0 == 0 {
            networks = result.1
            lastScan = Date()
            log("Found \(networks.count) network\(networks.count == 1 ? "" : "s").")
        } else {
            log("Scan failed (IOReturn 0x\(String(UInt32(bitPattern: result.0), radix: 16))).", .error)
        }
        refresh()
    }

    func join(_ network: WiFiNetwork, password: String) async {
        isBusy = true
        defer { isBusy = false }
        log("Joining \(network.displayName) (\(network.bssid), channel \(network.channel))…")
        var params = RTL8723BEConnectParams()
        withUnsafeMutableBytes(of: &params.ssid) { buf in
            let bytes = Array(network.ssid.utf8.prefix(Int(RTL8723BE_MAX_SSID_LEN)))
            buf.copyBytes(from: bytes)
        }
        withUnsafeMutableBytes(of: &params.passphrase) { buf in
            let bytes = Array(password.utf8.prefix(Int(RTL8723BE_MAX_PASSPHRASE_LEN)))
            buf.copyBytes(from: bytes)
        }
        let b = network.bssidBytes
        if b.count == 6 { params.bssid = (b[0], b[1], b[2], b[3], b[4], b[5]) }
        let paramsCopy = params
        let rc: Int32 = await Task.detached {
            var conn: io_connect_t = 0
            guard rtl_open_client(&conn) == 0 else { return -1 }
            defer { rtl_close_client(conn) }
            var p = paramsCopy
            return rtl_connect_ap(conn, &p)
        }.value
        guard rc == 0 else {
            alertMessage = rc == Int32(bitPattern: 0xE00002C2)
                ? "The password must be 8–63 characters."
                : "The driver refused the request (IOReturn 0x\(String(UInt32(bitPattern: rc), radix: 16)))."
            log("Join failed: \(alertMessage ?? "")", .error)
            return
        }
        // The 802.11 exchange and WPA2 handshake complete asynchronously in the driver.
        for _ in 0..<30 {
            try? await Task.sleep(for: .milliseconds(500))
            refresh()
            if phase == .connected { return }
            if phase == .disconnected { break }
        }
        if phase != .connected {
            alertMessage = "Could not join \(network.displayName). Check the password and that the network is in range."
            log("Join timed out or was rejected.", .error)
        }
    }

    func disconnect() async {
        await Task.detached {
            var conn: io_connect_t = 0
            guard rtl_open_client(&conn) == 0 else { return }
            defer { rtl_close_client(conn) }
            _ = rtl_disconnect_ap(conn)
        }.value
        refresh()
    }

    func setAntenna(_ value: Int) {
        var conn: io_connect_t = 0
        guard rtl_open_client(&conn) == 0 else { return }
        defer { rtl_close_client(conn) }
        if rtl_set_antenna(conn, UInt8(value)) == 0 {
            antenna = value
            log("Wi-Fi antenna set to \(value == 2 ? "AUX (S0)" : "MAIN (S1)").")
        }
    }

    /// Asks for an administrator password and loads the driver (installed copy first).
    func loadDriver() async {
        guard let resources = Bundle.main.resourcePath else { return }
        isBusy = true
        defer { isBusy = false }
        let helper = (resources as NSString).appendingPathComponent("load_test_kext.sh")
        let kext = (resources as NSString).appendingPathComponent("RTL8723BEWiFi.kext")
        func q(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\"'\"'") + "'" }
        let command = "/bin/bash \(q(helper)) \(q(kext))"
        let source = "do shell script \"\(command.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
        log("Loading the driver (administrator password required)…")
        let message: String? = await Task.detached {
            var error: NSDictionary?
            let result = NSAppleScript(source: source)?.executeAndReturnError(&error)
            if let error { return error[NSAppleScript.errorMessage] as? String ?? "Load failed." }
            return result?.stringValue
        }.value
        if let message, !message.isEmpty {
            let failed = message.contains("not approved") || message.contains("disabled") || message.contains("Could not")
            log(message, failed ? .warning : .info)
            if message.contains("not approved") {
                alertMessage = "macOS needs your approval: open System Settings › Privacy & Security, allow the RTL8723BE driver, then restart."
            }
        }
        try? await Task.sleep(for: .seconds(2))
        refreshEnvironment()
        refresh()
    }

    // MARK: - Helpers

    nonisolated private static func parse(_ raw: RTL8723BEScanResults) -> [WiFiNetwork] {
        var copy = raw
        let count = min(Int(copy.count), Int(RTL8723BE_MAX_SCAN_APS))
        return withUnsafeBytes(of: &copy.aps) { buf in
            let aps = buf.bindMemory(to: RTL8723BEDiscoveredAP.self)
            return (0..<count).map { i -> WiFiNetwork in
                var ap = aps[i]
                let ssid = withUnsafeBytes(of: &ap.ssid) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
                let b = [ap.bssid.0, ap.bssid.1, ap.bssid.2, ap.bssid.3, ap.bssid.4, ap.bssid.5]
                return WiFiNetwork(ssid: ssid, bssid: Self.mac(ap.bssid), bssidBytes: b,
                                   channel: Int(ap.channel), rssi: Int(ap.rssi), secured: ap.is_wpa2 != 0)
            }
            .sorted { $0.rssi > $1.rssi }
        }
    }

    nonisolated static func mac(_ m: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8)) -> String {
        String(format: "%02x:%02x:%02x:%02x:%02x:%02x", m.0, m.1, m.2, m.3, m.4, m.5)
    }

    nonisolated private static func run(_ path: String, _ args: [String]) async -> String {
        await Task.detached {
            let task = Process()
            task.executableURL = URL(fileURLWithPath: path)
            task.arguments = args
            let pipe = Pipe()
            task.standardOutput = pipe
            task.standardError = Pipe()
            guard (try? task.run()) != nil else { return "" }
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            task.waitUntilExit()
            return String(decoding: data, as: UTF8.self)
        }.value
    }
}
