// SPDX-License-Identifier: MIT
// Uses a unique service; never queries or modifies the app's saved network items.
import Foundation

@main
struct CredentialTests {
    static func main() throws {
        precondition(!NetworkCredentials.validPassword("short"))
        precondition(NetworkCredentials.validPassword("password"))
        precondition(NetworkCredentials.validPassword(String(repeating: "a", count: 63)))
        precondition(NetworkCredentials.validPassword(String(repeating: "0123456789abcdef", count: 4)))
        precondition(!NetworkCredentials.validPassword(String(repeating: "z", count: 64)))
        precondition(!NetworkCredentials.validPassword("password\u{0}"))
        precondition(!NetworkCredentials.validPassword("contraseña"))
        if CommandLine.arguments.contains("--keychain") {
            let store = NetworkCredentials(service: "com.rtl8723be.tests." + UUID().uuidString)
            let ssid = "Test network"
            defer { try? store.forget(ssid); try? store.forget("Other network") }
            func check(_ actual: String?, _ expected: String?) { precondition(actual == expected) }
            check(try store.password(for: ssid), nil)
            try store.save("first-password", for: ssid)
            check(try store.password(for: ssid), "first-password")
            check(try store.password(for: "Other network"), nil)
            try store.save("replacement-password", for: ssid)
            check(try store.password(for: ssid), "replacement-password")
            try store.forget(ssid)
            check(try store.password(for: ssid), nil)
        }
        print("Credential validation passed" + (CommandLine.arguments.contains("--keychain") ? "; Keychain save/read/update/forget passed." : "."))
    }
}
