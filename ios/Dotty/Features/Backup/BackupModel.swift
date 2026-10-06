import CryptoKit
import Foundation
import Observation
import UIKit

/// Copies of Dotty's SD card kept on the iPhone, and putting one back. Both run over Wi-Fi
/// in the launcher's whole-card mode (firmware: lib/dotty_core/src/transfer.cpp,
/// transfer.start {scope: card}); tools/card_backup.py does the same from a Mac.
@Observable
final class BackupModel {
    struct Backup: Identifiable, Hashable {
        var id: URL { folder }
        let folder: URL
        let made: Date
        let files: Int
        let bytes: Int64
        let songs: Int
        let photos: Int
        let recordings: Int
        let withFirmware: Bool

        var summary: String {
            var parts: [String] = []
            if songs > 0 { parts.append(songs == 1 ? "1 song" : "\(songs) songs") }
            if photos > 0 { parts.append(photos == 1 ? "1 photo" : "\(photos) photos") }
            if recordings > 0 { parts.append(recordings == 1 ? "1 recording" : "\(recordings) recordings") }
            parts.append(ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file))
            return parts.joined(separator: " · ")
        }
    }

    /// A file on Dotty's card, from GET /card/list.
    struct CardFile: Codable {
        let path: String
        let size: Int64
        var sha256: String?
    }

    private(set) var backups: [Backup] = []
    private(set) var busy = false
    private(set) var stage: String?
    private(set) var progress: Double?
    private(set) var detail: String?
    var error: String?
    var notice: String?

    private let link: DottyLink

    /// In the app's Documents, so the Files app shows them (Info.plist: UIFileSharingEnabled).
    static let folder = URL.documentsDirectory.appending(path: "Backups", directoryHint: .isDirectory)
    nonisolated private static let manifestName = "dotty-backup.json"

    init(link: DottyLink) {
        self.link = link
        loadBackups()
    }

    // MARK: - The backups on this iPhone

    func loadBackups() {
        let folders = (try? FileManager.default.contentsOfDirectory(at: Self.folder, includingPropertiesForKeys: nil)) ?? []
        backups = folders.compactMap { folder in
            guard let data = try? Data(contentsOf: folder.appending(path: Self.manifestName)),
                  let manifest = try? JSONDecoder().decode(Manifest.self, from: data) else { return nil }
            let paths = manifest.files.map(\.path)
            return Backup(folder: folder, made: manifest.date, files: manifest.files.count,
                          bytes: manifest.files.reduce(0) { $0 + $1.size },
                          songs: paths.filter { $0.contains("/music/data/library/") }.count,
                          photos: paths.filter { $0.contains("/album/data/photos/") }.count,
                          recordings: paths.filter { $0.contains("/tape/data/recordings/") }.count,
                          withFirmware: manifest.scope == "all")
        }
        .sorted { $0.made > $1.made }
    }

    func delete(_ backup: Backup) {
        try? FileManager.default.removeItem(at: backup.folder)
        loadBackups()
    }

    private struct Manifest: Codable {
        let made: String
        let scope: String
        let files: [CardFile]

        var date: Date { ISO8601DateFormatter.local.date(from: made) ?? .distantPast }
    }

    // MARK: - Back up

    func backUp(withFirmware: Bool) async {
        await run("Backing up") { url, token in
            self.stage = "Reading Dotty's card"
            var files = try await self.list(url, token, hashes: false)
                .filter { !Self.hidden($0.path) && (withFirmware || !$0.path.contains("/firmware/")) }
            files.sort { $0.path < $1.path }
            let total = files.reduce(0) { $0 + $1.size }
            let stamp = Date()
            let folder = Self.folder.appending(path: Self.folderName(for: stamp), directoryHint: .isDirectory)
            let started = Date()
            var done: Int64 = 0
            for (i, file) in files.enumerated() {
                self.stage = "Copying \(i + 1) of \(files.count)"
                let target = folder.appending(path: String(file.path.dropFirst()))
                try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                let (temp, response) = try await URLSession.shared.download(for: self.request(url, token, "download", file.path))
                guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw DottyError.refused("Dotty couldn't send \(file.path)") }
                try? FileManager.default.removeItem(at: target)
                try FileManager.default.moveItem(at: temp, to: target)
                let size = (try? target.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? -1
                guard size == file.size else { throw DottyError.refused("\(file.path) arrived incomplete") }
                done += file.size
                self.progress = total > 0 ? Double(done) / Double(total) : nil
                self.detail = Self.rate(done: done, total: total, since: started)
            }
            let manifest = Manifest(made: ISO8601DateFormatter.local.string(from: stamp),
                                    scope: withFirmware ? "all" : "data", files: files)
            try JSONEncoder().encode(manifest).write(to: folder.appending(path: Self.manifestName))
            self.loadBackups()
            let size = ByteCountFormatter.string(fromByteCount: total, countStyle: .file)
            self.notice = "Backed up \(files.count) files (\(size))."
        }
    }

    // MARK: - Restore

    /// Makes Dotty's card match the backup, sending only what differs: Dotty fingerprints its
    /// files (SHA-256), unchanged ones stay, files the backup lacks are deleted (never the
    /// launcher's copies, hidden files, or cartridge copies when the backup has only data).
    func restore(_ backup: Backup) async {
        await run("Restoring") { url, token in
            let local = Self.localFiles(in: backup.folder)
            self.stage = "Dotty is checking its files"
            self.progress = nil
            let remote = Dictionary(try await self.list(url, token, hashes: true).map { ($0.path, $0) }, uniquingKeysWith: { a, _ in a })
            self.stage = "Comparing with the backup"
            // Fingerprinting this iPhone's copy (~160 MB) happens off the main thread.
            let remoteFingerprints = remote.mapValues { ($0.size, $0.sha256) }
            let send: [String] = await Task.detached {
                local.compactMap { path, file in
                    let size = (try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? -1
                    if let there = remoteFingerprints[path], there.0 == size, there.1 == Self.sha256(file) { return nil }
                    return path
                }
            }.value
            let keep: (String) -> Bool = { path in
                Self.hidden(path) || path.hasPrefix("/cartridges/launcher/") || (!backup.withFirmware && path.contains("/firmware/"))
            }
            let delete = remote.keys.filter { local[$0] == nil && !keep($0) }
            let unchanged = local.count - send.count
            let total = send.reduce(Int64(0)) { sum, path in
                sum + Int64((try? local[path]!.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
            }
            let started = Date()
            var done: Int64 = 0
            for (i, path) in send.sorted().enumerated() {
                self.stage = "Sending \(i + 1) of \(send.count)"
                var request = self.request(url, token, "upload", path)
                request.httpMethod = "POST"
                let (_, response) = try await URLSession.shared.upload(for: request, fromFile: local[path]!)
                guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw DottyError.refused("Dotty couldn't take \(path)") }
                done += Int64((try? local[path]!.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
                self.progress = total > 0 ? Double(done) / Double(total) : nil
                self.detail = Self.rate(done: done, total: total, since: started)
            }
            for (i, path) in delete.sorted().enumerated() {
                self.stage = "Removing \(i + 1) of \(delete.count)"
                var request = self.request(url, token, "card/delete", path)
                request.httpMethod = "POST"
                _ = try await URLSession.shared.data(for: request)
            }
            var parts = ["\(unchanged) unchanged"]
            if !send.isEmpty { parts.append("\(send.count) sent") }
            if !delete.isEmpty { parts.append("\(delete.count) removed") }
            self.notice = "Restored: " + parts.joined(separator: ", ") + "."
        }
    }

    // MARK: - The whole-card session

    /// Launcher → Wi-Fi → whole-card server; Bluetooth pauses meanwhile. Afterwards the
    /// session ends (POST /done), Bluetooth comes back and the cartridge that was running
    /// starts again.
    private func run(_ what: String, _ body: @escaping (URL, String) async throws -> Void) async {
        guard !busy, let info = link.info else { return }
        busy = true
        error = nil
        notice = nil
        progress = nil
        detail = nil
        let wasRunning = info.isLauncher ? nil : info.name
        UIApplication.shared.isIdleTimerDisabled = true
        let background = UIApplication.shared.beginBackgroundTask(withName: what)
        defer {
            busy = false
            stage = nil
            progress = nil
            detail = nil
            UIApplication.shared.isIdleTimerDisabled = false
            UIApplication.shared.endBackgroundTask(background)
        }
        var session: (URL, String)?
        do {
            stage = "Switching to the launcher"
            try await link.ensureLauncher()
            stage = "Dotty is joining Wi-Fi"
            let reply = try await link.send("transfer.start", ["scope": "card"], timeout: 120)
            guard let url = (reply["url"] as? String).flatMap(URL.init(string:)), let token = reply["token"] as? String else {
                throw DottyError.refused("Dotty didn't open its Wi-Fi connection.")
            }
            session = (url, token)
            try await body(url, token)
        } catch {
            self.error = "\(what) didn't finish: \(error.localizedDescription). Is this iPhone on the same Wi-Fi as Dotty?"
        }
        if let (url, token) = session {
            stage = "Reconnecting to Dotty"
            await SongOutbox.endSession(server: url, token: token)
            try? await link.waitForReconnect(timeout: 30)
        }
        if let wasRunning, link.info?.isLauncher == true {
            stage = "Starting \(wasRunning) again"
            _ = try? await link.send("launcher.start")
        }
    }

    private func list(_ url: URL, _ token: String, hashes: Bool) async throws -> [CardFile] {
        var request = URLRequest(url: url.appending(path: "card/list"))
        if hashes { request.url = URL(string: request.url!.absoluteString + "?hash=1") }
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        request.timeoutInterval = 120  // between pieces: Dotty streams the list as it fingerprints
        let (data, _) = try await URLSession.shared.data(for: request)
        return try JSONDecoder().decode([CardFile].self, from: data)
    }

    private func request(_ url: URL, _ token: String, _ endpoint: String, _ path: String) -> URLRequest {
        var components = URLComponents(url: url.appending(path: endpoint), resolvingAgainstBaseURL: false)!
        components.percentEncodedQuery = "path=" + (path.addingPercentEncoding(withAllowedCharacters: Self.unreserved) ?? path)
        var request = URLRequest(url: components.url!)
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        request.timeoutInterval = 60
        return request
    }

    // MARK: - Helpers

    /// The backup's files by card path ("/cartridges/…"), without the manifest or hidden files.
    nonisolated static func localFiles(in folder: URL) -> [String: URL] {
        var local: [String: URL] = [:]
        guard let walker = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: [.isRegularFileKey]) else {
            return local
        }
        for case let file as URL in walker {
            let relative = String(file.standardizedFileURL.path.dropFirst(folder.standardizedFileURL.path.count))
            guard (try? file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true,
                  relative != "/" + manifestName, !hidden(relative) else { continue }
            local[relative] = file
        }
        return local
    }

    /// Dotty reads "+" as a space: only unreserved ASCII goes as-is (like SongOutbox).
    private static let unreserved = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~/")

    /// Hidden files (e.g. a Mac's .Spotlight-V100 on the card) are neither copied nor removed.
    nonisolated static func hidden(_ path: String) -> Bool {
        path.split(separator: "/").contains { $0.hasPrefix(".") }
    }

    nonisolated static func sha256(_ file: URL) -> String {
        guard let handle = try? FileHandle(forReadingFrom: file) else { return "" }
        defer { try? handle.close() }
        var hasher = SHA256()
        while let chunk = try? handle.read(upToCount: 1 << 20), !chunk.isEmpty { hasher.update(data: chunk) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func folderName(for date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HHmm"
        return formatter.string(from: date)
    }

    static func rate(done: Int64, total: Int64, since start: Date) -> String {
        let seconds = max(1, Date().timeIntervalSince(start))
        let mb = { (b: Int64) in String(format: "%.1f", Double(b) / 1_048_576) }
        return "\(mb(done)) of \(mb(total)) MB · \(Int(Double(done) / 1024 / seconds)) KB/s"
    }
}

extension ISO8601DateFormatter {
    /// "2026-10-06T12:16:30" in local time, like tools/card_backup.py writes.
    nonisolated(unsafe) static let local: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withFullDate, .withTime, .withColonSeparatorInTime, .withDashSeparatorInDate]
        formatter.timeZone = .current
        return formatter
    }()
}
