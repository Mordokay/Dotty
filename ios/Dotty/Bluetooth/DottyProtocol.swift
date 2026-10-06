import CoreBluetooth
import Foundation

/// The "Dotty Core" BLE service, the same in the launcher and every cartridge
/// (firmware: lib/dotty_core/src/core_ble.h).
enum DottyUUID {
    static let service = CBUUID(string: "b9c10000-fbaa-4525-8400-055f7a543231")
    /// Read, open: identifies a Dotty before pairing.
    static let info = CBUUID(string: "b9c10001-fbaa-4525-8400-055f7a543231")
    /// Write, encrypted: JSON commands. The first write makes iOS pair (code on Dotty's screen).
    static let command = CBUUID(string: "b9c10002-fbaa-4525-8400-055f7a543231")
    /// Notify: JSON replies and events.
    static let event = CBUUID(string: "b9c10003-fbaa-4525-8400-055f7a543231")
    /// Write without response, encrypted: bulk bytes (cartridge uploads).
    static let data = CBUUID(string: "b9c10004-fbaa-4525-8400-055f7a543231")
}

/// What a Dotty says about itself (the Info characteristic).
struct DottyInfo: Decodable, Equatable, Sendable {
    struct Installed: Decodable, Equatable, Sendable {
        let id: String
        let name: String
        let version: String
    }

    /// "launcher" or "cartridge".
    let role: String
    let id: String
    let name: String
    let version: String
    let battery: Int
    /// On a charger and not full yet (newer firmware; nil before).
    let charging: Bool?
    /// A charger or computer is powering Dotty.
    let power: Bool?
    let serial: String?
    /// Command namespaces ("wifi", "music", …). The Info value is capped at 512 bytes, so
    /// newer firmware lists these instead of every command.
    let features: [String]?
    /// Every command (older firmware only; newer firmware returns it from core.info).
    let commands: [String]?
    /// Launcher only: the cartridge in the slot.
    let installed: Installed?
    /// Launcher only: an SD card is in.
    let card: Bool?
    /// Cartridges only: the launcher's version (firmware from the launcher-updates era on).
    let launcher: String?

    var isLauncher: Bool { role == "launcher" }

    /// Dotty's system (launcher) version, whether it or a cartridge is running.
    var launcherVersion: String? { isLauncher ? version : launcher }

    /// True if the firmware has commands in this namespace, e.g. "wifi".
    func supports(_ feature: String) -> Bool {
        features?.contains(feature) ?? commands?.contains { $0.hasPrefix(feature + ".") } ?? false
    }

    /// True if this firmware's version is at least `minimum` ("0.7.0").
    func isAtLeast(_ minimum: String) -> Bool {
        version.compare(minimum, options: .numeric) != .orderedAscending
    }

    /// The cartridge Dotty runs (or has installed, when the launcher is up).
    var cartridgeName: String? { isLauncher ? installed.map { "\($0.name) \($0.version)" } : "\(name) \(version)" }
}

/// A JSON message from Dotty: a reply to a command ({"cmd": …, "ok": …}) or an event.
struct DottyMessage: Sendable {
    let raw: Data

    private var object: [String: Any] {
        (try? JSONSerialization.jsonObject(with: raw)) as? [String: Any] ?? [:]
    }

    var command: String? { object["cmd"] as? String }
    var event: String? { object["event"] as? String }
    var ok: Bool { object["ok"] as? Bool ?? true }
    var error: String? { object["error"] as? String }

    subscript(key: String) -> Any? { object[key] }

    func decode<T: Decodable>(_ type: T.Type) throws -> T {
        try JSONDecoder().decode(type, from: raw)
    }
}

enum DottyError: LocalizedError {
    case notConnected
    case timeout
    case refused(String)
    case pairingFailed

    var errorDescription: String? {
        switch self {
        case .notConnected: "Dotty isn't connected."
        case .timeout: "Dotty didn't answer in time."
        case .refused(let message): message.prefix(1).uppercased() + message.dropFirst()
        case .pairingFailed: "Pairing didn't finish. Check the code on Dotty's screen and try again."
        }
    }
}

/// The Dotty this phone is paired with, remembered across launches.
struct PairedDotty: Codable, Equatable, Sendable {
    let identifier: UUID
    var name: String
    var serial: String?

    private static let key = "pairedDotty"

    static func load() -> PairedDotty? {
        guard let data = UserDefaults.standard.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(PairedDotty.self, from: data)
    }

    func save() {
        UserDefaults.standard.set(try? JSONEncoder().encode(self), forKey: Self.key)
    }

    static func clear() {
        UserDefaults.standard.removeObject(forKey: key)
    }
}
