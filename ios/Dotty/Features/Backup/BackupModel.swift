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
        /// The cartridges it holds (a restore only touches these).
        let cartridges: [String]

        var summary: String {
            var parts = [cartridges.map { BackupModel.names[$0] ?? $0.capitalized }.joined(separator: ", ")]
            if songs > 0 { parts.append(songs == 1 ? "1 song" : "\(songs) songs") }
            if photos > 0 { parts.append(photos == 1 ? "1 photo" : "\(photos) photos") }
            if recordings > 0 { parts.append(recordings == 1 ? "1 recording" : "\(recordings) recordings") }
            parts.append(ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file))
            return parts.joined(separator: " · ")
        }
    }

    /// A cartridge with files on Dotty's card (storage.list): what a backup can include.
    struct CardCartridge: Identifiable, Hashable {
        let id: String
        let bytes: Int64

        var name: String { BackupModel.names[id] ?? id.capitalized }
    }

    static let names = ["music": "Music", "jokes": "Joke Factory", "weather": "Weather Station",
                        "album": "Album Viewer", "tape": "Tape Recorder", "launcher": "Dotty system"]

    /// "/cartridges/music/data/…" → "music".
    nonisolated static func cartridge(of path: String) -> String? {
        let parts = path.split(separator: "/")
        return parts.count > 2 && parts[0] == "cartridges" ? String(parts[1]) : nil
    }

    /// A file on Dotty's card, from GET /card/list.
    struct CardFile: Codable {
        let path: String
        let size: Int64
        var sha256: String?
    }

    private(set) var backups: [Backup] = []
    /// Cartridges with data on Dotty, for choosing what to back up.
    private(set) var onDotty: [CardCartridge] = []
    @ObservationIgnored private var job: Task<Void, Never>?
    private(set) var busy = false
    private(set) var cancelling = false
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
                          withFirmware: manifest.scope == "all",
                          cartridges: manifest.cartridges ?? Array(Set(paths.compactMap(Self.cartridge(of:)))).sorted())
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
        var cartridges: [String]? = nil  // older backups: every cartridge in `files`
        let files: [CardFile]

        var date: Date { ISO8601DateFormatter.local.date(from: made) ?? .distantPast }
    }

    /// The cartridges with files on Dotty (Bluetooth, no Wi-Fi needed).
    func loadOnDotty() async {
        guard let reply = try? await link.send("storage.list") else { return }
        onDotty = (reply["cartridges"] as? [[String: Any]] ?? []).compactMap { item in
            guard let id = item["id"] as? String, id != "launcher" else { return nil }
            let bytes = (item["data"] as? NSNumber)?.int64Value ?? 0
            let installed = !(item["versions"] as? [Any] ?? []).isEmpty
            return bytes > 0 || installed ? CardCartridge(id: id, bytes: bytes) : nil
        }
        .sorted { $0.name < $1.name }
    }

    // MARK: - Running and cancelling

    /// Starts a backup or restore that `cancel()` can stop.
    func start(_ work: @escaping () async -> Void) {
        guard job == nil else { return }
        job = Task {
            await work()
            job = nil
        }
    }

    func cancel() {
        guard job != nil else { return }
        cancelling = true
        stage = "Stopping"
        job?.cancel()
    }

    // MARK: - Back up

    /// Copies the chosen cartridges' files (and their firmware copies with `withFirmware`).
    func backUp(only cartridges: Set<String>, withFirmware: Bool) async {
        await run("Backup") { url, token in
            self.stage = "Reading Dotty's card"
            var files = try await self.list(url, token, hashes: false).filter { file in
                guard !Self.hidden(file.path), let id = Self.cartridge(of: file.path), cartridges.contains(id) else { return false }
                return withFirmware || !file.path.contains("/firmware/")
            }
            files.sort { $0.path < $1.path }
            let total = files.reduce(0) { $0 + $1.size }
            let stamp = Date()
            let folder = Self.folder.appending(path: Self.folderName(for: stamp), directoryHint: .isDirectory)
            let started = Date()
            var done: Int64 = 0
            do {
                for (i, file) in files.enumerated() {
                    try Task.checkCancellation()
                    self.stage = "Copying \(i + 1) of \(files.count)"
                    let target = folder.appending(path: String(file.path.dropFirst()))
                    try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                    let request = self.request(url, token, "download", file.path,
                                               progress: ("backup", i + 1, files.count, done, total))
                    let (temp, response) = try await URLSession.shared.download(for: request)
                    guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw DottyError.refused("Dotty couldn't send \(file.path)") }
                    try? FileManager.default.removeItem(at: target)
                    try FileManager.default.moveItem(at: temp, to: target)
                    let size = (try? target.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? -1
                    guard size == file.size else { throw DottyError.refused("\(file.path) arrived incomplete") }
                    done += file.size
                    self.progress = total > 0 ? Double(done) / Double(total) : nil
                    self.detail = Self.rate(done: done, total: total, since: started)
                }
            } catch {
                try? FileManager.default.removeItem(at: folder)  // no half backups
                throw error
            }
            let manifest = Manifest(made: ISO8601DateFormatter.local.string(from: stamp),
                                    scope: withFirmware ? "all" : "data", cartridges: cartridges.sorted(), files: files)
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
        await run("Restore") { url, token in
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
            // Only the backup's cartridges are made to match: others stay as they are.
            let scope = Set(backup.cartridges)
            let keep: (String) -> Bool = { path in
                Self.hidden(path) || path.hasPrefix("/cartridges/launcher/") || (!backup.withFirmware && path.contains("/firmware/"))
                    || !(Self.cartridge(of: path).map(scope.contains) ?? false)
            }
            let delete = remote.keys.filter { local[$0] == nil && !keep($0) }
            let unchanged = local.count - send.count
            let total = send.reduce(Int64(0)) { sum, path in
                sum + Int64((try? local[path]!.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
            }
            let started = Date()
            var done: Int64 = 0
            for (i, path) in send.sorted().enumerated() {
                try Task.checkCancellation()
                self.stage = "Sending \(i + 1) of \(send.count)"
                var request = self.request(url, token, "upload", path,
                                           progress: ("restore", i + 1, send.count, done, total))
                request.httpMethod = "POST"
                let (_, response) = try await URLSession.shared.upload(for: request, fromFile: local[path]!)
                guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw DottyError.refused("Dotty couldn't take \(path)") }
                done += Int64((try? local[path]!.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
                self.progress = total > 0 ? Double(done) / Double(total) : nil
                self.detail = Self.rate(done: done, total: total, since: started)
            }
            for (i, path) in delete.sorted().enumerated() {
                try Task.checkCancellation()
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
            cancelling = false
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
            let stoppedHere = error is CancellationError || (error as? URLError)?.code == .cancelled
            // Cleanup runs in its own task: a cancelled task's network calls fail at once.
            let stoppedOnDotty = await Task { () -> Bool in
                guard !stoppedHere, let (url, token) = session else { return false }
                return await Self.cancelledOnDotty(url, token)
            }.value
            if stoppedHere {
                self.notice = what == "Restore" ? "Restore stopped. Dotty has the files sent so far." : "Backup stopped."
            } else if stoppedOnDotty {
                self.notice = what == "Restore" ? "Restore stopped on Dotty. It has the files sent so far." : "Backup stopped on Dotty."
            } else {
                self.error = "\(what) didn't finish: \(error.localizedDescription). Is this iPhone on the same Wi-Fi as Dotty?"
            }
        }
        await Task { [link] in
            if let (url, token) = session {
                self.stage = "Reconnecting to Dotty"
                await SongOutbox.endSession(server: url, token: token)
                try? await link.waitForReconnect(timeout: 30)
            }
            if let wasRunning, link.info?.isLauncher == true {
                self.stage = "Starting \(wasRunning) again"
                _ = try? await link.send("launcher.start")
            }
        }.value
    }

    /// Asks Dotty whether the session was cancelled on its screen (GET /status).
    private static func cancelledOnDotty(_ url: URL, _ token: String) async -> Bool {
        var request = URLRequest(url: url.appending(path: "status"))
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        request.timeoutInterval = 5
        guard let (data, _) = try? await URLSession.shared.data(for: request),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return false }
        return json["cancelled"] as? Bool ?? false
    }

    private func list(_ url: URL, _ token: String, hashes: Bool) async throws -> [CardFile] {
        var request = URLRequest(url: url.appending(path: "card/list"))
        if hashes { request.url = URL(string: request.url!.absoluteString + "?hash=1") }
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        request.timeoutInterval = 120  // between pieces: Dotty streams the list as it fingerprints
        let (data, _) = try await URLSession.shared.data(for: request)
        return try JSONDecoder().decode([CardFile].self, from: data)
    }

    /// `progress` tells Dotty's screen where the job is: "20 of 50" and a bar by size
    /// (firmware: transfer.cpp readProgress).
    private func request(_ url: URL, _ token: String, _ endpoint: String, _ path: String,
                         progress: (job: String, step: Int, steps: Int, before: Int64, total: Int64)? = nil) -> URLRequest {
        var components = URLComponents(url: url.appending(path: endpoint), resolvingAgainstBaseURL: false)!
        components.percentEncodedQuery = "path=" + (path.addingPercentEncoding(withAllowedCharacters: Self.unreserved) ?? path)
        var request = URLRequest(url: components.url!)
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        if let progress {
            request.setValue(progress.job, forHTTPHeaderField: "X-Dotty-Job")
            request.setValue("\(progress.step)/\(progress.steps)", forHTTPHeaderField: "X-Dotty-Step")
            request.setValue("\(progress.before)/\(progress.total)", forHTTPHeaderField: "X-Dotty-Bytes")
        }
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
