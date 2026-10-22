import Foundation
import Security

/// The Gemini API keys on this iPhone (up to 3; Dotty takes them in turn, one per session). Kept
/// in the Keychain as a JSON array; the app's Gemini helper uses the first. Dotty has its own copy
/// and only ever sends back the last 4 characters of each.
enum GeminiKeys {
    private static let service = "com.greenspherestudios.dotty"
    private static let account = "gemini-keys"

    static func load() -> [String] {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                    kSecAttrAccount as String: account, kSecReturnData as String: true,
                                    kSecMatchLimit as String: kSecMatchLimitOne]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess, let data = item as? Data,
              let keys = try? JSONDecoder().decode([String].self, from: data) else { return [] }
        return keys
    }

    static func save(_ keys: [String]) {
        let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                   kSecAttrAccount as String: account]
        SecItemDelete(base as CFDictionary)
        guard !keys.isEmpty, let data = try? JSONEncoder().encode(keys) else { return }
        var add = base
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(add as CFDictionary, nil)
    }

    /// The last 4 characters, as Dotty shows them.
    static func hint(_ key: String) -> String { String(key.suffix(4)) }

    enum Check: Equatable { case ok, refused(String), offline }

    /// Asks Google whether the key works: listing the models costs nothing (no tokens).
    static func validate(_ key: String) async -> Check {
        let trimmed = key.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty,
              let url = URL(string: "https://generativelanguage.googleapis.com/v1beta/models?pageSize=1&key=\(trimmed)") else {
            return .refused("That doesn't look like a key.")
        }
        do {
            let (data, response) = try await URLSession.shared.data(from: url)
            let status = (response as? HTTPURLResponse)?.statusCode ?? 0
            if status == 200 { return .ok }
            let message = (try? JSONSerialization.jsonObject(with: data) as? [String: Any])
                .flatMap { $0["error"] as? [String: Any] }?["message"] as? String
            return .refused(message ?? "Google said no (HTTP \(status)).")
        } catch {
            return .offline
        }
    }
}
