import Cocoa
import SwiftUI
import Combine

struct DiscoveredNetwork: Identifiable, Hashable {
    var id: String { "\(ssid)_\(bssid)" }
    let ssid: String
    let bssid: String
    let bssidBytes: [UInt8]
    let channel: UInt8
    let rssi: Int8
    let isWPA2: Bool

    var signalBars: Int {
        if rssi >= -55 { return 4 }
        if rssi >= -67 { return 3 }
        if rssi >= -78 { return 2 }
        return 1
    }
}

final class WiFiDriverModel: ObservableObject {
    @Published var isDriverLoaded: Bool = false
    @Published var pciDeviceDetected: Bool = false
    @Published var statusText: String = "Checking IORegistry..."
    @Published var stateCode: UInt32 = 0
    @Published var macAddress: String = "--:--:--:--:--:--"
    @Published var connectedSSID: String = "Not Connected"
    @Published var connectedBSSID: String = "--:--:--:--:--:--"
    @Published var currentChannel: UInt8 = 1
    @Published var currentRSSI: Int8 = -100
    @Published var activeAntenna: Int = 2 // Default AUX (#2) for HP 103c:804c
    @Published var txPackets: UInt64 = 0
    @Published var rxPackets: UInt64 = 0
    @Published var txErrors: UInt64 = 0
    @Published var rxErrors: UInt64 = 0
    @Published var networks: [DiscoveredNetwork] = []
    @Published var isScanning: Bool = false
    @Published var logMessages: [String] = []

    private var pollTimer: Timer?

    init() {
        appendLog("RTL8723BE Wireless Utility initialized on macOS \(ProcessInfo.processInfo.operatingSystemVersionString)")
        checkPCIDevicePresence()
        refreshStatus()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 2.5, repeats: true) { [weak self] _ in
            self?.refreshStatus()
        }
    }

    func appendLog(_ msg: String) {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        let line = "[\(formatter.string(from: Date()))] \(msg)"
        DispatchQueue.main.async {
            self.logMessages.append(line)
            if self.logMessages.count > 80 {
                self.logMessages.removeFirst()
            }
        }
    }

    func checkPCIDevicePresence() {
        let task = Process()
        task.launchPath = "/usr/sbin/ioreg"
        task.arguments = ["-l"]
        let pipe = Pipe()
        task.standardOutput = pipe
        do {
            try task.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            if let str = String(data: data, encoding: .utf8), str.contains("pci10ec,b723") {
                DispatchQueue.main.async {
                    self.pciDeviceDetected = true
                }
                appendLog("Detected Realtek RTL8723BE PCIe hardware (pci10ec,b723, HP 103c:804c) in IORegistry.")
            }
        } catch {
            appendLog("IORegistry check error: \(error.localizedDescription)")
        }
    }

    func refreshStatus() {
        var conn: io_connect_t = 0
        let rc = rtl_open_client(&conn)
        if rc != 0 {
            DispatchQueue.main.async {
                self.isDriverLoaded = false
                self.statusText = self.pciDeviceDetected
                    ? "Hardware Detected (pci10ec,b723) — Kext Ready to Load"
                    : "Driver Not Loaded"
            }
            return
        }
        defer { rtl_close_client(conn) }

        var st = RTL8723BEStatus()
        if rtl_get_status(conn, &st) == 0 {
            let mac = String(format: "%02x:%02x:%02x:%02x:%02x:%02x",
                             st.mac.0, st.mac.1, st.mac.2, st.mac.3, st.mac.4, st.mac.5)
            let bssid = String(format: "%02x:%02x:%02x:%02x:%02x:%02x",
                               st.bssid.0, st.bssid.1, st.bssid.2, st.bssid.3, st.bssid.4, st.bssid.5)
            let ssid = withUnsafePointer(to: &st.ssid) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(RTL8723BE_MAX_SSID_LEN + 1)) {
                    String(cString: $0)
                }
            }
            DispatchQueue.main.async {
                self.isDriverLoaded = true
                self.stateCode = st.state
                self.statusText = Self.describeState(st.state)
                self.macAddress = mac
                self.connectedSSID = ssid.isEmpty ? "Not Connected" : ssid
                self.connectedBSSID = bssid
                self.currentChannel = st.channel
                self.currentRSSI = st.rssi
                self.activeAntenna = Int(st.antenna == 2 ? 2 : 1)
                self.txPackets = st.tx_packets
                self.rxPackets = st.rx_packets
                self.txErrors = st.tx_errors
                self.rxErrors = st.rx_errors
            }
        }
    }

    static func describeState(_ state: UInt32) -> String {
        switch state {
        case 0: return "Disconnected"
        case 1: return "Scanning 2.4 GHz Channels 1–13..."
        case 2: return "802.11 Authenticating..."
        case 3: return "802.11 Associating..."
        case 4: return "WPA2 4-Way Handshake..."
        case 5: return "Connected (WPA2-AES/CCMP)"
        default: return "Ready"
        }
    }

    func scanNetworks() {
        isScanning = true
        appendLog("Starting 2.4 GHz scan across channels 1..13...")
        DispatchQueue.global(qos: .userInitiated).async {
            var conn: io_connect_t = 0
            if rtl_open_client(&conn) == 0 {
                _ = rtl_trigger_scan(conn)
                Thread.sleep(forTimeInterval: 1.6)
                var raw = RTL8723BEScanResults()
                if rtl_get_scan_results(conn, &raw) == 0 {
                    var parsed: [DiscoveredNetwork] = []
                    let count = min(Int(raw.count), Int(RTL8723BE_MAX_SCAN_APS))
                    withUnsafePointer(to: &raw.aps) { ptr in
                        ptr.withMemoryRebound(to: RTL8723BEDiscoveredAP.self, capacity: Int(RTL8723BE_MAX_SCAN_APS)) { buf in
                            for i in 0..<count {
                                var ap = buf[i]
                                let ssid = withUnsafePointer(to: &ap.ssid) {
                                    $0.withMemoryRebound(to: CChar.self, capacity: Int(RTL8723BE_MAX_SSID_LEN + 1)) {
                                        String(cString: $0)
                                    }
                                }
                                let b = [ap.bssid.0, ap.bssid.1, ap.bssid.2, ap.bssid.3, ap.bssid.4, ap.bssid.5]
                                let bssidStr = String(format: "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5])
                                parsed.append(DiscoveredNetwork(
                                    ssid: ssid.isEmpty ? "<Hidden Network>" : ssid,
                                    bssid: bssidStr,
                                    bssidBytes: b,
                                    channel: ap.channel,
                                    rssi: ap.rssi,
                                    isWPA2: ap.is_wpa2 != 0
                                ))
                            }
                        }
                    }
                    rtl_close_client(conn)
                    DispatchQueue.main.async {
                        self.networks = parsed.sorted { $0.rssi > $1.rssi }
                        self.isScanning = false
                        self.appendLog("Scan completed: discovered \(parsed.count) access point(s).")
                    }
                    return
                }
                rtl_close_client(conn)
            }
            DispatchQueue.main.async {
                self.isScanning = false
                self.appendLog("Driver not yet loaded in kernel — click 'Load Kext Now' or 'Stage to OpenCore EFI' below.")
            }
        }
    }

    func connectToNetwork(_ net: DiscoveredNetwork, passphrase: String) {
        appendLog("Connecting to '\(net.ssid)' (\(net.bssid), Ch \(net.channel))...")
        DispatchQueue.global(qos: .userInitiated).async {
            var conn: io_connect_t = 0
            guard rtl_open_client(&conn) == 0 else {
                self.appendLog("Cannot connect: RTL8723BEWiFi.kext is not currently loaded in kernel.")
                return
            }
            defer { rtl_close_client(conn) }

            var params = RTL8723BEConnectParams()
            _ = withUnsafeMutablePointer(to: &params.ssid) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(RTL8723BE_MAX_SSID_LEN + 1)) { ptr in
                    strncpy(ptr, net.ssid, Int(RTL8723BE_MAX_SSID_LEN))
                }
            }
            _ = withUnsafeMutablePointer(to: &params.passphrase) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(RTL8723BE_MAX_PASSPHRASE_LEN + 1)) { ptr in
                    strncpy(ptr, passphrase, Int(RTL8723BE_MAX_PASSPHRASE_LEN))
                }
            }
            if net.bssidBytes.count == 6 {
                params.bssid = (net.bssidBytes[0], net.bssidBytes[1], net.bssidBytes[2],
                                net.bssidBytes[3], net.bssidBytes[4], net.bssidBytes[5])
            }
            let rc = rtl_connect_ap(conn, &params)
            if rc == 0 {
                self.appendLog("802.11 Auth/Assoc + WPA2 handshake initiated for '\(net.ssid)'.")
            } else {
                self.appendLog("Connect call returned IOReturn 0x\(String(rc, radix: 16)).")
            }
            Thread.sleep(forTimeInterval: 0.8)
            self.refreshStatus()
        }
    }

    func disconnectNetwork() {
        var conn: io_connect_t = 0
        guard rtl_open_client(&conn) == 0 else { return }
        defer { rtl_close_client(conn) }
        _ = rtl_disconnect_ap(conn)
        appendLog("Disconnected from active Wi-Fi session.")
        refreshStatus()
    }

    func setAntenna(_ ant: Int) {
        activeAntenna = ant
        var conn: io_connect_t = 0
        guard rtl_open_client(&conn) == 0 else {
            appendLog("Saved antenna preference: #\(ant) (\(ant == 2 ? "AUX" : "MAIN")). Will apply when kext loads.")
            return
        }
        defer { rtl_close_client(conn) }
        if rtl_set_antenna(conn, UInt8(ant)) == 0 {
            appendLog("Switched RTL8723BE RF diversity switch to Antenna #\(ant) (\(ant == 2 ? "AUX" : "MAIN")).")
        }
        refreshStatus()
    }

    func bundledKextPath() -> String {
        if let resPath = Bundle.main.resourcePath {
            let candidate = (resPath as NSString).appendingPathComponent("RTL8723BEWiFi.kext")
            if FileManager.default.fileExists(atPath: candidate) {
                return candidate
            }
        }
        return "/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/build/RTL8723BEWiFi.kext"
    }

    func loadKextWithAdminPrompt() {
        let kextPath = bundledKextPath()
        appendLog("Requesting administrator privileges to stage and load \(kextPath)...")
        let cmd = "rm -rf /tmp/RTL8723BEWiFi.kext && cp -R '\(kextPath)' /tmp/RTL8723BEWiFi.kext && chown -R root:wheel /tmp/RTL8723BEWiFi.kext && chmod -R 755 /tmp/RTL8723BEWiFi.kext && kmutil load -p /tmp/RTL8723BEWiFi.kext"
        let scriptSrc = "do shell script \"\(cmd)\" with administrator privileges"
        DispatchQueue.global(qos: .userInitiated).async {
            var errorDict: NSDictionary?
            if let script = NSAppleScript(source: scriptSrc) {
                let res = script.executeAndReturnError(&errorDict)
                if let err = errorDict {
                    self.appendLog("Kernel load message: \(err[NSAppleScript.errorMessage] ?? "Requires OpenCore reboot or SIP kext consent")")
                } else {
                    self.appendLog("kmutil load succeeded! \(res.stringValue ?? "")")
                }
                self.refreshStatus()
            }
        }
    }

    func installToOpenCoreEFI() {
        let kextPath = bundledKextPath()
        appendLog("Mounting EFI partition and installing RTL8723BEWiFi.kext to OpenCore EFI/OC/Kexts...")
        let cmd = """
        mkdir -p /Volumes/EFI_Mount && \
        (diskutil mount -mountPoint /Volumes/EFI_Mount disk0s1 || diskutil mount disk0s1 || true) && \
        for V in /Volumes/EFI_Mount /Volumes/EFI /Volumes/ESP; do \
          if [ -d "$V/EFI/OC/Kexts" ]; then \
            rm -rf "$V/EFI/OC/Kexts/RTL8723BEWiFi.kext" && \
            cp -R '\(kextPath)' "$V/EFI/OC/Kexts/RTL8723BEWiFi.kext" && \
            /usr/bin/python3 -c 'import plistlib,sys; p=sys.argv[1]; pl=plistlib.load(open(p,"rb")); k=pl.setdefault("Kernel",{}).setdefault("Add",[]); (k.append({"Arch":"x86_64","BundlePath":"RTL8723BEWiFi.kext","Comment":"Realtek RTL8723BE PCIe Wireless LAN Driver","Enabled":True,"ExecutablePath":"Contents/MacOS/RTL8723BEWiFi","MaxKernel":"","MinKernel":"20.0.0","PlistPath":"Contents/Info.plist"}) if not any(x.get("BundlePath")=="RTL8723BEWiFi.kext" for x in k) else None); plistlib.dump(pl,open(p,"wb"))' "$V/EFI/OC/config.plist" && \
            echo "Installed to $V/EFI/OC/Kexts/RTL8723BEWiFi.kext"; \
          fi; \
        done
        """
        let escaped = cmd.replacingOccurrences(of: "\"", with: "\\\"")
        let scriptSrc = "do shell script \"\(escaped)\" with administrator privileges"
        DispatchQueue.global(qos: .userInitiated).async {
            var errorDict: NSDictionary?
            if let script = NSAppleScript(source: scriptSrc) {
                let res = script.executeAndReturnError(&errorDict)
                if let err = errorDict {
                    self.appendLog("OpenCore install note: \(err[NSAppleScript.errorMessage] ?? "")")
                } else {
                    self.appendLog("OpenCore EFI updated! \(res.stringValue ?? "Reboot to activate kext in OpenCore.")")
                }
            }
        }
    }
}

struct MainDashboardView: View {
    @ObservedObject var model: WiFiDriverModel
    @State private var selectedNetwork: DiscoveredNetwork?
    @State private var passphraseInput: String = ""
    @State private var showingPasswordSheet: Bool = false

    var body: some View {
        VStack(spacing: 0) {
            // Top Header Banner
            HStack(spacing: 14) {
                ZStack {
                    RoundedRectangle(cornerRadius: 12)
                        .fill(LinearGradient(
                            colors: [Color.blue.opacity(0.85), Color.cyan.opacity(0.75)],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        ))
                        .frame(width: 48, height: 48)
                    Image(systemName: model.stateCode == 5 ? "wifi" : "antenna.radiowaves.left.and.right")
                        .font(.system(size: 23, weight: .semibold))
                        .foregroundColor(.white)
                }

                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 8) {
                        Text("Realtek RTL8723BE Wireless Utility")
                            .font(.system(size: 17, weight: .bold))
                        Text(model.isDriverLoaded ? "KEXT ACTIVE" : (model.pciDeviceDetected ? "HW DETECTED (10EC:B723)" : "STANDBY"))
                            .font(.system(size: 10, weight: .bold))
                            .padding(.horizontal, 7)
                            .padding(.vertical, 2)
                            .background(model.isDriverLoaded ? Color.green.opacity(0.2) : Color.orange.opacity(0.2))
                            .foregroundColor(model.isDriverLoaded ? .green : .orange)
                            .cornerRadius(5)
                    }
                    Text(model.statusText)
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                }

                Spacer()

                Picker("Antenna", selection: Binding(
                    get: { model.activeAntenna },
                    set: { model.setAntenna($0) }
                )) {
                    Text("Ant #1 (Main)").tag(1)
                    Text("Ant #2 (Aux - HP)").tag(2)
                }
                .pickerStyle(.segmented)
                .frame(width: 210)
            }
            .padding(16)
            .background(Color(NSColor.windowBackgroundColor))

            Divider()

            // Telemetry Strip
            HStack(spacing: 16) {
                telemetryCard(title: "CONNECTED SSID", value: model.connectedSSID, icon: "wifi.circle.fill")
                telemetryCard(title: "HARDWARE MAC", value: model.macAddress, icon: "cpu")
                telemetryCard(title: "CHANNEL / RSSI", value: "Ch \(model.currentChannel) (\(model.currentRSSI) dBm)", icon: "waveform.path.ecg")
                telemetryCard(title: "TX / RX FRAMES", value: "\(model.txPackets) / \(model.rxPackets)", icon: "arrow.up.arrow.down.circle")
            }
            .padding(14)

            Divider()

            // Main Split Area: Scanner Table + Actions
            HSplitView {
                // Left: Wi-Fi Network List
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Text("2.4 GHz 802.11b/g/n Networks")
                            .font(.system(size: 13, weight: .semibold))
                        Spacer()
                        if model.isScanning {
                            ProgressView()
                                .scaleEffect(0.6)
                        }
                        Button(action: { model.scanNetworks() }) {
                            Label("Scan Now", systemImage: "arrow.clockwise")
                        }
                        .disabled(model.isScanning)
                    }

                    List(model.networks, selection: $selectedNetwork) { net in
                        HStack {
                            Image(systemName: "wifi")
                                .foregroundColor(.blue)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(net.ssid)
                                    .font(.system(size: 13, weight: .medium))
                                Text("BSSID: \(net.bssid) • Channel \(net.channel)")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            if net.isWPA2 {
                                Image(systemName: "lock.fill")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Text("\(net.rssi) dBm")
                                .font(.system(size: 11, design: .monospaced))
                                .foregroundColor(.secondary)
                            Button("Connect") {
                                selectedNetwork = net
                                passphraseInput = ""
                                showingPasswordSheet = true
                            }
                            .buttonStyle(.borderedProminent)
                            .controlSize(.small)
                        }
                        .padding(.vertical, 4)
                    }
                    .overlay(
                        Group {
                            if model.networks.isEmpty {
                                VStack(spacing: 8) {
                                    Image(systemName: "antenna.radiowaves.left.and.right.slash")
                                        .font(.system(size: 28))
                                        .foregroundColor(.secondary)
                                    Text("Click 'Scan Now' to sweep 2.4 GHz channels 1–13")
                                        .font(.system(size: 12))
                                        .foregroundColor(.secondary)
                                }
                            }
                        }
                    )
                }
                .padding(14)
                .frame(minWidth: 410)

                // Right: One-Click Driver & OpenCore Manager + Live Logs
                VStack(alignment: .leading, spacing: 12) {
                    Text("Driver & OpenCore Installer")
                        .font(.system(size: 13, weight: .semibold))

                    VStack(alignment: .leading, spacing: 8) {
                        Button(action: { model.installToOpenCoreEFI() }) {
                            HStack {
                                Image(systemName: "externaldrive.fill.badge.checkmark")
                                Text("Install Kext to OpenCore EFI")
                                Spacer()
                            }
                            .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)

                        Button(action: { model.loadKextWithAdminPrompt() }) {
                            HStack {
                                Image(systemName: "bolt.shield.fill")
                                Text("Load Kext Now (kmutil load)")
                                Spacer()
                            }
                            .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)

                        if model.stateCode == 5 {
                            Button(role: .destructive, action: { model.disconnectNetwork() }) {
                                HStack {
                                    Image(systemName: "xmark.circle.fill")
                                    Text("Disconnect Wi-Fi")
                                    Spacer()
                                }
                                .frame(maxWidth: .infinity)
                            }
                            .buttonStyle(.bordered)
                        }
                    }

                    Divider()

                    Text("Kernel & Utility Diagnostic Log")
                        .font(.system(size: 12, weight: .semibold))

                    ScrollView {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(Array(model.logMessages.enumerated()), id: \.offset) { _, line in
                                Text(line)
                                    .font(.system(size: 10.5, design: .monospaced))
                                    .foregroundColor(.primary)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                            }
                        }
                        .padding(8)
                    }
                    .background(Color(NSColor.textBackgroundColor))
                    .cornerRadius(6)
                }
                .padding(14)
                .frame(minWidth: 290)
            }
        }
        .frame(minWidth: 760, minHeight: 480)
        .sheet(isPresented: $showingPasswordSheet) {
            VStack(alignment: .leading, spacing: 14) {
                Text("Join Wi-Fi Network '\(selectedNetwork?.ssid ?? "")'")
                    .font(.headline)
                Text("Enter the WPA2-PSK passphrase for \(selectedNetwork?.ssid ?? "") (\(selectedNetwork?.bssid ?? "")):")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                SecureField("WPA2 Passphrase", text: $passphraseInput)
                    .textFieldStyle(.roundedBorder)
                HStack {
                    Spacer()
                    Button("Cancel") {
                        showingPasswordSheet = false
                    }
                    Button("Connect") {
                        if let net = selectedNetwork {
                            model.connectToNetwork(net, passphrase: passphraseInput)
                        }
                        showingPasswordSheet = false
                    }
                    .buttonStyle(.borderedProminent)
                }
            }
            .padding(20)
            .frame(width: 380)
        }
    }

    private func telemetryCard(title: String, value: String, icon: String) -> some View {
        HStack(spacing: 10) {
            Image(systemName: icon)
                .font(.system(size: 18))
                .foregroundColor(.blue)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 9.5, weight: .bold))
                    .foregroundColor(.secondary)
                Text(value)
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .lineLimit(1)
            }
            Spacer()
        }
        .padding(10)
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(8)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    var statusItem: NSStatusItem!
    let model = WiFiDriverModel()

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)

        // Create Main Dashboard Window
        let contentView = MainDashboardView(model: model)
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 780, height: 510),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.center()
        window.title = "RTL8723BE Wireless Utility (pci10ec,b723)"
        window.contentView = NSHostingView(rootView: contentView)
        window.makeKeyAndOrderFront(nil)

        // Create macOS Menu Bar Extra Icon
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "wifi", accessibilityDescription: "RTL8723BE Wi-Fi")
        }
        let menu = NSMenu()
        menu.addItem(NSMenuItem(title: "Open RTL8723BE Wireless Utility", action: #selector(showMainWindow), keyEquivalent: "o"))
        menu.addItem(NSMenuItem(title: "Scan 2.4 GHz Networks", action: #selector(triggerMenuScan), keyEquivalent: "s"))
        menu.addItem(NSMenuItem.separator())
        menu.addItem(NSMenuItem(title: "Install Kext to OpenCore EFI...", action: #selector(triggerOpenCoreInstall), keyEquivalent: "i"))
        menu.addItem(NSMenuItem(title: "Load Kext Now (Administrator)...", action: #selector(triggerLoadKext), keyEquivalent: "l"))
        menu.addItem(NSMenuItem.separator())
        menu.addItem(NSMenuItem(title: "Quit RTL8723BE Utility", action: #selector(quitApp), keyEquivalent: "q"))
        statusItem.menu = menu

        NSApp.activate(ignoringOtherApps: true)
    }

    @objc func showMainWindow() {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc func triggerMenuScan() {
        showMainWindow()
        model.scanNetworks()
    }

    @objc func triggerOpenCoreInstall() {
        model.installToOpenCoreEFI()
    }

    @objc func triggerLoadKext() {
        model.loadKextWithAdminPrompt()
    }

    @objc func quitApp() {
        NSApp.terminate(nil)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return false
    }
}

@main
struct RTL8723BEUtilityMain {
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }
}
