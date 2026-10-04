import Foundation
import Observation

/// The Music cartridge seen from the phone: what's playing, the library and playlists on
/// Dotty's SD card, and songs waiting to be sent over Wi-Fi
/// (firmware: cartridges/music/main.cpp, lib/dotty_core/src/transfer.h).
@Observable
final class MusicModel {
    struct Song: Identifiable, Hashable {
        var id: String { name }
        /// File name on the card, e.g. "NAPA-Deslocado.mp3".
        let name: String
        let title: String
        let size: Int
    }

    struct Playlist: Identifiable, Hashable {
        var id: String { name }
        let name: String
        let count: Int
    }

    struct NowPlaying: Equatable {
        /// "" plays the whole library.
        var queue = ""
        var index = 0
        var count = 0
        var song = ""
        var title = ""
        var playing = false
        var paused = false
        var position = 0
        var duration = 0
        var volume = 80
        /// When `position` was true, to move the progress bar between updates.
        var receivedAt = Date()

        func position(at date: Date) -> Double {
            guard playing else { return Double(position) }
            return min(Double(duration), Double(position) + date.timeIntervalSince(receivedAt))
        }
    }

    struct Upload: Identifiable {
        enum State: Equatable { case waiting, sending, sent, failed(String) }
        let id = UUID()
        /// The app's own copy, so the file stays readable after the picker closes.
        let file: URL
        let name: String
        let size: Int64
        var sent: Int64 = 0
        var state = State.waiting
    }

    private(set) var songs: [Song] = []
    private(set) var playlists: [Playlist] = []
    private(set) var now = NowPlaying()
    /// Bumped whenever the library or a playlist changes, so playlist screens reload.
    private(set) var libraryVersion = 0
    private(set) var loaded = false
    private(set) var uploads: [Upload] = []
    private(set) var syncing = false
    private(set) var syncStage: String?
    var error: String?
    var notice: String?

    private let link: DottyLink
    private static let outbox = FileManager.default.temporaryDirectory.appending(path: "Outbox", directoryHint: .isDirectory)

    init(link: DottyLink) {
        self.link = link
    }

    // MARK: - Loading and events

    func load() async {
        do {
            try await loadLibrary()
            apply(state: try await link.send("music.status"))
            loaded = true
        } catch {
            self.error = describe(error)
        }
    }

    private func loadLibrary() async throws {
        let reply = try await link.send("music.library")
        songs = (reply["songs"] as? [[String: Any]] ?? []).compactMap { item in
            guard let name = item["name"] as? String else { return nil }
            return Song(name: name, title: item["title"] as? String ?? name, size: item["size"] as? Int ?? 0)
        }
        playlists = (reply["playlists"] as? [[String: Any]] ?? []).compactMap { item in
            guard let name = item["name"] as? String else { return nil }
            return Playlist(name: name, count: item["count"] as? Int ?? 0)
        }
        libraryVersion += 1
    }

    func handle(_ message: DottyMessage) {
        switch message.event {
        case "music.state":
            apply(state: message)
        case "music.library":
            Task { try? await loadLibrary() }
        default:
            break
        }
    }

    private func apply(state message: DottyMessage) {
        var state = NowPlaying()
        state.queue = message["queue"] as? String ?? ""
        state.index = message["index"] as? Int ?? 0
        state.count = message["count"] as? Int ?? 0
        state.song = message["song"] as? String ?? ""
        state.title = message["title"] as? String ?? ""
        state.playing = message["playing"] as? Bool ?? false
        state.paused = message["paused"] as? Bool ?? false
        state.position = message["position"] as? Int ?? 0
        state.duration = message["duration"] as? Int ?? 0
        state.volume = message["volume"] as? Int ?? now.volume
        now = state
    }

    // MARK: - Playback

    func toggle() async { await control("music.toggle") }
    func next() async { await control("music.next") }
    func previous() async { await control("music.prev") }

    func setVolume(_ value: Int) async {
        await control("music.volume", ["value": value])
    }

    func play(playlist: String = "", song: String? = nil) async {
        var arguments: [String: Any] = ["playlist": playlist]
        if let song { arguments["song"] = song }
        await control("music.play", arguments)
    }

    private func control(_ command: String, _ arguments: [String: Any] = [:]) async {
        do {
            apply(state: try await link.send(command, arguments))
        } catch {
            self.error = describe(error)
        }
    }

    // MARK: - Library and playlists

    func playlistSongs(_ name: String) async -> [String] {
        (try? await link.send("music.playlist", ["name": name]))?["songs"] as? [String] ?? []
    }

    func createPlaylist(_ name: String) async {
        await edit("music.playlist.create", ["name": name])
    }

    func deletePlaylist(_ name: String) async {
        await edit("music.playlist.delete", ["name": name])
    }

    /// Commands are limited to 512 bytes, so long selections go in several batches.
    func add(_ songNames: [String], to playlist: String) async {
        var batch: [String] = []
        var batchBytes = 0
        for name in songNames {
            let bytes = name.utf8.count + 3
            if batchBytes + bytes > 380, !batch.isEmpty {
                await edit("music.playlist.add", ["name": playlist, "songs": batch])
                batch = []
                batchBytes = 0
            }
            batch.append(name)
            batchBytes += bytes
        }
        if !batch.isEmpty { await edit("music.playlist.add", ["name": playlist, "songs": batch]) }
    }

    func remove(_ song: String, from playlist: String) async {
        await edit("music.playlist.remove", ["name": playlist, "song": song])
    }

    func deleteSong(_ name: String) async {
        await edit("music.song.delete", ["name": name])
    }

    private func edit(_ command: String, _ arguments: [String: Any]) async {
        do {
            try await link.send(command, arguments)
            try await loadLibrary()
        } catch {
            self.error = describe(error)
        }
    }

    // MARK: - Sending songs over Wi-Fi

    /// Copies picked files into the app, ready for the next sync.
    func queue(_ urls: [URL]) {
        try? FileManager.default.createDirectory(at: Self.outbox, withIntermediateDirectories: true)
        for url in urls {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let name = url.lastPathComponent
            guard !uploads.contains(where: { $0.name == name && $0.state != .sent }) else { continue }
            let copy = Self.outbox.appending(path: UUID().uuidString + "-" + name)
            do {
                try FileManager.default.copyItem(at: url, to: copy)
                let size = (try? copy.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? 0
                uploads.append(Upload(file: copy, name: name, size: size))
            } catch {
                self.error = "Couldn't read \(name)."
            }
        }
    }

    func unqueue(_ upload: Upload) {
        try? FileManager.default.removeItem(at: upload.file)
        uploads.removeAll { $0.id == upload.id }
    }

    var waitingCount: Int { uploads.filter { $0.state != .sent }.count }

    var syncProgress: Double {
        let total = uploads.reduce(Int64(0)) { $0 + $1.size }
        guard total > 0 else { return 0 }
        return Double(uploads.reduce(Int64(0)) { $0 + ($1.state == .sent ? $1.size : $1.sent) }) / Double(total)
    }

    /// Dotty joins Wi-Fi and opens a one-time upload server; each song goes over HTTP.
    func sync() async {
        guard !syncing, waitingCount > 0 else { return }
        syncing = true
        error = nil
        notice = nil
        syncStage = "Dotty is joining Wi-Fi"
        defer {
            syncing = false
            syncStage = nil
        }

        let server: URL, token: String, ssid: String
        do {
            let reply = try await link.send("transfer.start", timeout: 45)
            guard let url = (reply["url"] as? String).flatMap(URL.init(string:)), let key = reply["token"] as? String else {
                throw DottyError.refused("Dotty didn't open its upload server.")
            }
            server = url
            token = key
            ssid = reply["ssid"] as? String ?? "Dotty's Wi-Fi"
        } catch {
            self.error = describe(error)
            return
        }

        var sentCount = 0
        for index in uploads.indices where uploads[index].state != .sent {
            syncStage = "Sending \(uploads[index].name)"
            uploads[index].state = .sending
            uploads[index].sent = 0
            do {
                try await send(index: index, to: server, token: token)
                uploads[index].state = .sent
                try? FileManager.default.removeItem(at: uploads[index].file)
                sentCount += 1
            } catch {
                uploads[index].state = .failed(error.localizedDescription)
                self.error = "Couldn't reach Dotty over Wi-Fi. Is this iPhone on \(ssid)? Allow Local Network for Dotty in Settings if iOS asked."
                break
            }
        }

        syncStage = "Finishing"
        _ = try? await link.send("transfer.stop")
        uploads.removeAll { $0.state == .sent }
        try? await loadLibrary()
        if sentCount > 0 {
            notice = sentCount == 1 ? "1 song is on Dotty." : "\(sentCount) songs are on Dotty."
        }
    }

    /// The first request can fail while iOS asks for Local Network access, so retry a little.
    private func send(index: Int, to server: URL, token: String) async throws {
        let upload = uploads[index]
        var components = URLComponents(url: server.appending(path: "upload"), resolvingAgainstBaseURL: false)!
        components.percentEncodedQuery = "dir=library&name=" + Self.queryEscape(upload.name)
        var request = URLRequest(url: components.url!)
        request.httpMethod = "PUT"
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.timeoutInterval = 30

        let id = upload.id
        let delegate = UploadProgress { [weak self] sent in
            Task { @MainActor in
                guard let self, let i = self.uploads.firstIndex(where: { $0.id == id }) else { return }
                self.uploads[i].sent = sent
            }
        }
        var lastError: Error = DottyError.timeout
        for attempt in 0..<4 {
            if attempt > 0 { try await Task.sleep(for: .seconds(2)) }
            do {
                let (data, response) = try await URLSession.shared.upload(for: request, fromFile: upload.file, delegate: delegate)
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                guard status == 200 else {
                    let message = (try? JSONSerialization.jsonObject(with: data) as? [String: Any])?["error"] as? String
                    throw DottyError.refused(message ?? "Dotty answered \(status).")
                }
                return
            } catch let error as DottyError {
                throw error
            } catch {
                lastError = error
            }
        }
        throw lastError
    }

    /// Strict escaping: Dotty reads "+" as a space, so only unreserved ASCII goes as-is.
    private static let unreserved = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~")

    private static func queryEscape(_ text: String) -> String {
        text.addingPercentEncoding(withAllowedCharacters: unreserved) ?? text
    }

    private func describe(_ error: Error) -> String {
        link.connection == .connected ? error.localizedDescription : "Dotty isn't connected. Press PWR on Dotty to wake it."
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
