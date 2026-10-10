// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
// Adapted from TetherKitNext (Issen Software Group) / TetherKit (c) 2026 TetherKit contributors.

import AppKit

/// Who makes the app, and where to find them.
enum Vendor {
    static let name = "Issen Software Group"
    static let website = URL(string: "https://issen.kurokamicorp.com/")!
    static let projectURL = URL(string: "https://github.com/AA-EION/RTL8723BE-macOS-WiFi")!
    static let releasesURL = URL(string: "https://github.com/AA-EION/RTL8723BE-macOS-WiFi/releases")!

    static let donationEmail = "juanesgtgt2@gmail.com"
    static let donationURL = URL(string:
        "https://www.paypal.com/donate/?business=juanesgtgt2%40gmail.com&no_recurring=0&item_name=Issen+Software+Group")!

    static var appVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"
    }

    /// Standard About panel with the vendor credit and a link to its website.
    @MainActor
    static func showAboutPanel() {
        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = .center
        let base: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize),
            .foregroundColor: NSColor.secondaryLabelColor,
            .paragraphStyle: paragraph,
        ]
        let credits = NSMutableAttributedString(
            string: "Made by Issen Software Group. Hardware support ported from the Linux rtlwifi driver.\n",
            attributes: base)
        var link = base
        link[.link] = website
        credits.append(NSAttributedString(string: website.host() ?? website.absoluteString, attributes: link))
        NSApp.activate()
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
    }
}
