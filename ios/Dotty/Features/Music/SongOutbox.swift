import Foundation
import Observation
import UIKit

/// Songs picked on the phone, waiting to go to Dotty over Wi-Fi. The queue (and a copy of
/// each file) survives the app closing, and songs Dotty already has are dropped from it, so
/// an interrupted sync just carries on with what's missing
/// (firmware: lib/dotty_core/src/transfer.h).
@Observable
final class SongOutbox {
    struct Upload: Identifiable, Codable {
        enum State: Equatable { case waiting, sending, sent, failed(String) }

        let id: UUID
        /// The app's own copy in `SongOutbox.folder`.
        let fileName: String
        /// The song's file name, as picked.
        let name: String
        let size: Int64
        var sent: Int64 = 0
        var state = State.waiting

        var file: URL { SongOutbox.folder.appending(path: fileName) }

        enum CodingKeys: String, CodingKey { case id, fileName, name, size }
    }

    private(set) var uploads: [Upload] = []
    private(set) var syncing = false
    private(set) var syncStage: String?
    /// When the current sync started sending, for its speed.
    private(set) var syncStarted: Date?
    var error: String?
    var notice: String?
    /// Set when Dotty's Wi-Fi was weak during the last sync (uploads crawl; a hotspot next
    /// to Dotty measured 3x faster than a far router).
    private(set) var weakSignal: (ssid: String, rssi: Int)?

    /// Runs after a sync, once Dotty is reachable again (reload the library).
    var onSynced: (() async -> Void)?

    private let link: DottyLink

    static let folder = URL.applicationSupportDirectory.appending(path: "Outbox", directoryHint: .isDirectory)
    private static let manifest = folder.appending(path: "queue.json")
    /// The upload server of a sync that didn't finish (e.g. the app was closed), so it can
    /// be ended: until then Dotty keeps Bluetooth off.
    private static let sessionKey = "music.transferSession"

    init(link: DottyLink) {
        self.link = link
        if let data = try? Data(contentsOf: Self.manifest),
           let saved = try? JSONDecoder().decode([Upload].self, from: data) {
            uploads = saved.filter { FileManager.default.fileExists(atPath: $0.file.path) }
        }
    }

    // MARK: - The queue

    /// Copies picked files into the app, ready for the next sync.
    func add(_ urls: [URL]) {
        try? FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
        for url in urls {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let name = url.lastPathComponent
            guard !uploads.contains(where: { $0.name == name }) else { continue }
            let id = UUID()
            let fileName = id.uuidString + "." + url.pathExtension
            do {
                try FileManager.default.copyItem(at: url, to: Self.folder.appending(path: fileName))
                let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? 0
                uploads.append(Upload(id: id, fileName: fileName, name: name, size: size))
            } catch {
                self.error = "Couldn't read \(name)."
            }
        }
        save()
    }

    func remove(_ upload: Upload) {
        try? FileManager.default.removeItem(at: upload.file)
        uploads.removeAll { $0.id == upload.id }
        save()
    }

    /// Drops songs Dotty already has (same stored name and size). Returns how many.
    @discardableResult
    func dropSongsOnDotty(_ library: [MusicModel.Song]) -> Int {
        guard !syncing else { return 0 }
        // Compare composed forms: songs sent by older app versions kept iOS's decomposed names.
        let onDotty = Set(library.map { "\($0.name.precomposedStringWithCanonicalMapping)/\($0.size)" })
        let done = uploads.filter { onDotty.contains("\(Self.storedName($0.name))/\($0.size)") }
        done.forEach { try? FileManager.default.removeItem(at: $0.file) }
        uploads.removeAll { upload in done.contains { $0.id == upload.id } }
        if !done.isEmpty { save() }
        return done.count
    }

    private func save() {
        try? FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
        try? JSONEncoder().encode(uploads).write(to: Self.manifest, options: .atomic)
    }

    // MARK: - Progress

    var waitingCount: Int { uploads.filter { $0.state != .sent }.count }
    var totalBytes: Int64 { uploads.reduce(0) { $0 + $1.size } }
    var sentBytes: Int64 { uploads.reduce(0) { $0 + ($1.state == .sent ? $1.size : $1.sent) } }
    var progress: Double { totalBytes > 0 ? Double(sentBytes) / Double(totalBytes) : 0 }

    /// "Song 2 of 5 · 6.1 MB of 18 MB · 240 KB/s"
    var detail: String? {
        guard syncing, let started = syncStarted else { return nil }
        let current = (uploads.firstIndex { $0.state == .sending } ?? uploads.count - 1) + 1
        let sent = ByteCountFormatter.string(fromByteCount: sentBytes, countStyle: .file)
        let total = ByteCountFormatter.string(fromByteCount: totalBytes, countStyle: .file)
        var line = "Song \(current) of \(uploads.count) · \(sent) of \(total)"
        let seconds = Date().timeIntervalSince(started)
        if seconds > 1, sentBytes > 0 { line += " · \(Int(Double(sentBytes) / 1024 / seconds)) KB/s" }
        return line
    }

    // MARK: - Sync

    /// Dotty joins Wi-Fi and opens a one-time upload server; each song goes over HTTP.
    func sync() async {
        guard !syncing, waitingCount > 0 else { return }
        syncing = true
        error = nil
        notice = nil
        syncStage = "Dotty is joining Wi-Fi"
        // Keep the iPhone awake, and keep going for a while if the app goes to the background.
        UIApplication.shared.isIdleTimerDisabled = true
        let background = UIApplication.shared.beginBackgroundTask(withName: "Sending songs to Dotty")
        defer {
            syncing = false
            syncStage = nil
            syncStarted = nil
            UIApplication.shared.isIdleTimerDisabled = false
            UIApplication.shared.endBackgroundTask(background)
        }

        let server: URL, token: String, ssid: String, pausesBluetooth: Bool
        do {
            let reply = try await link.send("transfer.start", timeout: 45)
            guard let url = (reply["url"] as? String).flatMap(URL.init(string:)), let key = reply["token"] as? String else {
                throw DottyError.refused("Dotty didn't open its upload server.")
            }
            server = url
            token = key
            ssid = reply["ssid"] as? String ?? "Dotty's Wi-Fi"
            // Newer firmware turns Bluetooth off while it receives (uploads run ~2x faster)
            // and expects POST /done instead of transfer.stop.
            pausesBluetooth = reply["bluetooth"] as? String == "paused"
            let rssi = reply["rssi"] as? Int ?? 0
            weakSignal = rssi != 0 && rssi < Self.weakSignalDbm ? (ssid, rssi) : nil
        } catch {
            self.error = link.connection == .connected ? error.localizedDescription
                                                       : "Dotty isn't connected. Press PWR on Dotty to wake it."
            return
        }
        UserDefaults.standard.set([server.absoluteString, token], forKey: Self.sessionKey)

        var sentCount = 0, failedCount = 0, failuresInARow = 0
        syncStarted = Date()
        for index in uploads.indices where uploads[index].state != .sent {
            syncStage = "Sending \(uploads[index].name)"
            uploads[index].state = .sending
            uploads[index].sent = 0
            do {
                try await send(index: index, to: server, token: token)
                uploads[index].state = .sent
                try? FileManager.default.removeItem(at: uploads[index].file)
                sentCount += 1
                failuresInARow = 0
            } catch {
                uploads[index].state = .failed(error.localizedDescription)
                failedCount += 1
                failuresInARow += 1
                // One bad song shouldn't stop the rest; two in a row means Dotty is out of reach.
                if failuresInARow >= 2 { break }
            }
        }

        syncStage = "Finishing"
        if pausesBluetooth {
            await Self.endSession(server: server, token: token)
            syncStage = "Reconnecting to Dotty"
            try? await link.waitForReconnect(timeout: 30)
        } else {
            _ = try? await link.send("transfer.stop")
        }
        UserDefaults.standard.removeObject(forKey: Self.sessionKey)
        uploads.removeAll { $0.state == .sent }
        for i in uploads.indices { uploads[i].state = .waiting }
        save()

        if failedCount > 0 || waitingCount > 0 {
            error = sentCount == 0
                ? "Couldn't send songs to Dotty over Wi-Fi. Is this iPhone on \(ssid), and is Dotty close to the router? Allow Local Network for Dotty in Settings if iOS asked."
                : "\(sentCount) sent, \(waitingCount) still waiting. Tap Sync to send the rest."
        } else if sentCount > 0 {
            notice = sentCount == 1 ? "1 song is on Dotty." : "\(sentCount) songs are on Dotty."
        }
        await onSynced?()
    }

    /// A sync the app didn't finish (closed mid-way) leaves Dotty with Bluetooth off until
    /// its idle timeout: end it now. Harmless if Dotty already gave up.
    func endInterruptedSession() async {
        guard !syncing, let saved = UserDefaults.standard.stringArray(forKey: Self.sessionKey), saved.count == 2,
              let server = URL(string: saved[0]) else { return }
        UserDefaults.standard.removeObject(forKey: Self.sessionKey)
        await Self.endSession(server: server, token: saved[1])
    }

    private static func endSession(server: URL, token: String) async {
        var request = URLRequest(url: server.appending(path: "done"))
        request.httpMethod = "POST"
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        request.timeoutInterval = 5
        _ = try? await URLSession.shared.data(for: request)
    }

    /// POST, not PUT: iOS silently re-sends a PUT whose connection drops, which hid failures
    /// (one song went four times). The first request can fail while iOS asks for Local
    /// Network access, so a failed song is tried up to three times.
    private func send(index: Int, to server: URL, token: String) async throws {
        let upload = uploads[index]
        var components = URLComponents(url: server.appending(path: "upload"), resolvingAgainstBaseURL: false)!
        // The name is sent as Dotty will store it: composed (iOS hands out decomposed Korean,
        // ~1.7x longer) and at most 120 bytes. Dotty's server refuses URLs over 512
        // characters before the upload starts, which failed 3 long-named songs.
        components.percentEncodedQuery = "dir=library&name=" + Self.queryEscape(Self.storedName(upload.name))
        var request = URLRequest(url: components.url!)
        request.httpMethod = "POST"
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.timeoutInterval = 45

        let id = upload.id
        let delegate = UploadProgress { [weak self] sent in
            Task { @MainActor in
                guard let self, let i = self.uploads.firstIndex(where: { $0.id == id }) else { return }
                self.uploads[i].sent = sent
            }
        }
        var lastError: Error = DottyError.timeout
        for attempt in 0..<3 {
            if attempt > 0 {
                try await Task.sleep(for: .seconds(2))
                uploads[index].sent = 0
            }
            do {
                let (data, response) = try await URLSession.shared.upload(for: request, fromFile: upload.file, delegate: delegate)
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                if status == 200 { return }
                let message = (try? JSONSerialization.jsonObject(with: data) as? [String: Any])?["error"] as? String
                lastError = DottyError.refused(message ?? "Dotty answered \(status).")
                if status == 403 { break }  // the session is gone; retrying won't help
            } catch {
                lastError = error
            }
        }
        throw lastError
    }

    /// Below this, uploads ran at ~100-160 KB/s (−85 dBm); a hotspot close by did > 500 KB/s.
    static let weakSignalDbm = -75

    /// Strict escaping: Dotty reads "+" as a space, so only unreserved ASCII goes as-is.
    private static let unreserved = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~")

    private static func queryEscape(_ text: String) -> String {
        text.addingPercentEncoding(withAllowedCharacters: unreserved) ?? text
    }

    /// The name Dotty stores a file under: composed Unicode, then storage::safeName in the
    /// firmware (no / \ : or control characters, no leading dots, at most 120 bytes keeping
    /// the extension).
    static func storedName(_ name: String) -> String {
        let blocked: Set<UInt8> = [UInt8(ascii: "/"), UInt8(ascii: "\\"), UInt8(ascii: ":")]
        var bytes = Array(name.precomposedStringWithCanonicalMapping.utf8.filter { $0 >= 32 && !blocked.contains($0) })
        func trim() {
            let space: (UInt8) -> Bool = { $0 == 32 || (9...13).contains($0) }
            while let first = bytes.first, space(first) { bytes.removeFirst() }
            while let last = bytes.last, space(last) { bytes.removeLast() }
        }
        trim()
        while bytes.first == UInt8(ascii: ".") { bytes.removeFirst() }
        let limit = 120
        if bytes.count > limit {
            var ext: [UInt8] = []
            if let dot = bytes.lastIndex(of: UInt8(ascii: ".")), dot > 0, bytes.count - dot <= 8 { ext = Array(bytes[dot...]) }
            var keep = limit - ext.count
            while keep > 0, bytes[keep] & 0xC0 == 0x80 { keep -= 1 }
            bytes = Array(bytes[..<keep]) + ext
            trim()
        }
        return String(decoding: bytes, as: UTF8.self)
    }
}

/// Bytes sent so far for one upload (URLSession calls this off the main actor).
nonisolated final class UploadProgress: NSObject, URLSessionTaskDelegate, Sendable {
    private let onSent: @Sendable (Int64) -> Void

    init(onSent: @escaping @Sendable (Int64) -> Void) {
        self.onSent = onSent
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didSendBodyData bytesSent: Int64,
                    totalBytesSent: Int64, totalBytesExpectedToSend: Int64) {
        onSent(totalBytesSent)
    }
}
