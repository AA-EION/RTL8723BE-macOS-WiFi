// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
import Foundation
import Security

/// Passwords live in the user's login Keychain, never preferences or activity logs.
struct NetworkCredentials {
    private let service: String
    init(service: String = "com.rtl8723be.macos.utility.wpa2") { self.service = service }

    static func validPassword(_ password: String) -> Bool {
        let bytes = Array(password.utf8)
        if bytes.count == 64 {
            return bytes.allSatisfy { (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0) }
        }
        return (8...63).contains(bytes.count) && bytes.allSatisfy { (32...126).contains($0) }
    }

    private func query(_ ssid: String) -> [String: Any] {
        // SSID, not BSSID: several radios in one network can share a password.
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: Data(ssid.utf8).base64EncodedString(),
         kSecAttrSynchronizable as String: false]
    }

    func password(for ssid: String) throws -> String? {
        var q = query(ssid)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data,
              let password = String(data: data, encoding: .utf8) else { throw KeychainError(status: status) }
        return password
    }

    func save(_ password: String, for ssid: String) throws {
        guard Self.validPassword(password), !ssid.isEmpty, ssid.utf8.count <= 32 else {
            throw KeychainError(status: errSecParam)
        }
        let q = query(ssid)
        let attributes: [String: Any] = [kSecValueData as String: Data(password.utf8)]
        var status = SecItemUpdate(q as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            var added = q.merging(attributes) { _, new in new }
            added[kSecAttrLabel as String] = "RTL8723BE Wi-Fi: \(ssid)"
            added[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            status = SecItemAdd(added as CFDictionary, nil)
        }
        guard status == errSecSuccess else { throw KeychainError(status: status) }
    }

    func forget(_ ssid: String) throws {
        let status = SecItemDelete(query(ssid) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw KeychainError(status: status) }
    }

    struct KeychainError: LocalizedError {
        let status: OSStatus
        var errorDescription: String? {
            SecCopyErrorMessageString(status, nil) as String? ?? "Keychain error \(status)."
        }
    }
}
