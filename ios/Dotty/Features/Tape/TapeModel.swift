import AVFoundation
import Foundation
import Observation

/// The Tape Recorder cartridge seen from the phone: recordings on Dotty's SD card, the deck's
/// state, and copies fetched over Wi-Fi to listen to and share
/// (firmware: cartridges/tape/, lib/dotty_core/src/transfer.h: GET /download).
@Observable
final class TapeModel {
    struct Recording: Identifiable, Hashable {
        var id: String { name }
        /// File name on the card, e.g. "20261006-091412.wav".
        let name: String
        /// "Mon 6 Oct 09:14", or the name it was given.
        let title: String
        let size: Int
        let duration: Double

        /// The phone's copy, once fetched.
        var local: URL { TapeModel.folder.appending(path: name) }
        var isOnPhone: Bool { FileManager.default.fileExists(atPath: local.path) }
    }

    /// The deck on Dotty.
    struct Deck: Equatable {
        var state = "idle"  // recording, paused
        var elapsed = 0
        var playing: String?
        var paused = false
        /// Paused tape: its parts (BOOT holds) and the last one's length (what Undo removes).
        var parts = 0
        var lastPart = 0.0
    }

    private(set) var recordings: [Recording] = []
    private(set) var deck = Deck()
    private(set) var loaded = false
    /// Recordings being fetched over Wi-Fi.
    private(set) var fetching: Set<String> = []
    private(set) var fetchStage: String?
    /// The recording playing on the phone.
    private(set) var listening: String?
    var error: String?
    var notice: String?

    private let link: DottyLink
    @ObservationIgnored private var player: AVAudioPlayer?
    @ObservationIgnored private var playerDelegate: Finished?

    static let folder = URL.cachesDirectory.appending(path: "Recordings", directoryHint: .isDirectory)
    private static let sessionKey = "tape.transferSession"

    init(link: DottyLink) {
        self.link = link
    }

    // MARK: - Loading and events

    func load() async {
        await endInterruptedSession()
        do {
            try await loadList()
            apply(try await link.send("tape.status"))
            loaded = true
        } catch {
            report(error)
        }
    }

    private func loadList() async throws {
        let reply = try await link.send("tape.list")
        recordings = (reply["recordings"] as? [[String: Any]] ?? []).compactMap { item in
            guard let name = item["name"] as? String else { return nil }
            return Recording(name: name, title: item["title"] as? String ?? name, size: item["size"] as? Int ?? 0,
                             duration: item["duration"] as? Double ?? 0)
        }
    }

    func handle(_ message: DottyMessage) {
        switch message.event {
        case "tape.state": apply(message)
        case "tape.list": Task { try? await loadList() }
        default: break
        }
    }

    private func apply(_ message: DottyMessage) {
        let wasTaping = self.deck.state != "idle"
        var deck = Deck()
        deck.state = message["state"] as? String ?? "idle"
        deck.elapsed = message["elapsed"] as? Int ?? 0
        deck.playing = message["playing"] as? String
        deck.paused = message["paused"] as? Bool ?? false
        deck.parts = message["parts"] as? Int ?? 0
        deck.lastPart = message["lastPart"] as? Double ?? 0
        self.deck = deck
        // A tape was just saved (0.1.0 firmware didn't send tape.list for it).
        if wasTaping && deck.state == "idle" { Task { try? await loadList() } }
    }

    func reconnected() async {
        guard fetching.isEmpty else { return }
        error = nil
        await load()
    }

    // MARK: - On Dotty

    func playOnDotty(_ recording: Recording) async {
        do { apply(try await link.send("tape.play", ["name": recording.name])) } catch { report(error) }
    }

    /// The paused tape: drop its last part, save it, or throw it away.
    func undoPart() async {
        do { apply(try await link.send("tape.undo")) } catch { report(error) }
    }

    func saveTape() async {
        do {
            apply(try await link.send("tape.save"))
            try await loadList()
        } catch { report(error) }
    }

    func discardTape() async {
        do { apply(try await link.send("tape.discard")) } catch { report(error) }
    }

    func stopOnDotty() async {
        do { apply(try await link.send("tape.stop")) } catch { report(error) }
    }

    /// True when Dotty took the new name.
    func rename(_ recording: Recording, to title: String) async -> Bool {
        do {
            try await link.send("tape.rename", ["name": recording.name, "to": title])
            try? FileManager.default.removeItem(at: recording.local)
            try await loadList()
            return true
        } catch {
            report(error)
            return false
        }
    }

    func delete(_ recording: Recording) async {
        if listening == recording.name { stopListening() }
        do {
            try await link.send("tape.delete", ["name": recording.name])
            try? FileManager.default.removeItem(at: recording.local)
            try await loadList()
        } catch {
            report(error)
        }
    }

    // MARK: - On the phone

    /// Plays the phone's copy, fetching it first if needed. Again = pause/resume.
    func listen(to recording: Recording) async {
        if listening == recording.name, let player {
            if player.isPlaying { player.pause() } else { player.play() }
            listening = recording.name  // refresh observers
            return
        }
        guard await fetch([recording]) else { return }
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback)
            try AVAudioSession.sharedInstance().setActive(true)
            let player = try AVAudioPlayer(contentsOf: recording.local)
            let delegate = Finished { [weak self] in self?.listening = nil }
            player.delegate = delegate
            player.play()
            self.player = player
            playerDelegate = delegate
            listening = recording.name
        } catch {
            self.error = "Couldn't play this recording on the iPhone."
        }
    }

    var isListening: Bool { player?.isPlaying == true }

    func stopListening() {
        player?.stop()
        player = nil
        listening = nil
    }

    /// Fetches the recordings the phone doesn't have yet over Wi-Fi (Bluetooth pauses
    /// meanwhile). True when all of them are on the phone.
    @discardableResult
    func fetch(_ wanted: [Recording]) async -> Bool {
        let missing = wanted.filter { !$0.isOnPhone }
        if missing.isEmpty { return true }
        guard fetching.isEmpty else { return false }
        fetching = Set(missing.map(\.name))
        error = nil
        fetchStage = "Dotty is joining Wi-Fi"
        defer {
            fetching = []
            fetchStage = nil
        }
        let server: URL, token: String
        do {
            let reply = try await link.send("transfer.start", timeout: 45)
            guard let url = (reply["url"] as? String).flatMap(URL.init(string:)), let key = reply["token"] as? String else {
                throw DottyError.refused("Dotty didn't open its Wi-Fi connection.")
            }
            server = url
            token = key
        } catch {
            report(error)
            return false
        }
        UserDefaults.standard.set([server.absoluteString, token], forKey: Self.sessionKey)
        try? FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)

        var failed = false
        for (i, recording) in missing.enumerated() {
            let size = ByteCountFormatter.string(fromByteCount: Int64(recording.size), countStyle: .file)
            fetchStage = missing.count == 1 ? "Getting it from Dotty (\(size))" : "Getting \(i + 1) of \(missing.count) (\(size))"
            var components = URLComponents(url: server.appending(path: "download"), resolvingAgainstBaseURL: false)!
            components.queryItems = [URLQueryItem(name: "dir", value: "recordings"), URLQueryItem(name: "name", value: recording.name)]
            var request = URLRequest(url: components.url!)
            request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
            request.timeoutInterval = 30
            do {
                let (file, response) = try await URLSession.shared.download(for: request)
                guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw DottyError.refused("Dotty couldn't send it.") }
                try? FileManager.default.removeItem(at: recording.local)
                try FileManager.default.moveItem(at: file, to: recording.local)
            } catch {
                failed = true
                break
            }
        }

        fetchStage = "Reconnecting to Dotty"
        await SongOutbox.endSession(server: server, token: token)
        UserDefaults.standard.removeObject(forKey: Self.sessionKey)
        try? await link.waitForReconnect(timeout: 30)
        if failed {
            error = "Couldn't get the recording over Wi-Fi. Is this iPhone on the same network as Dotty? Allow Local Network for Dotty in Settings if iOS asked."
        }
        return !failed
    }

    private func endInterruptedSession() async {
        guard fetching.isEmpty, let saved = UserDefaults.standard.stringArray(forKey: Self.sessionKey), saved.count == 2,
              let server = URL(string: saved[0]) else { return }
        UserDefaults.standard.removeObject(forKey: Self.sessionKey)
        await SongOutbox.endSession(server: server, token: saved[1])
    }

    private func report(_ error: Error) {
        if !link.lostConnection(error) { self.error = error.localizedDescription }
    }
}

/// Tells the model when a recording ends on the phone.
private final class Finished: NSObject, AVAudioPlayerDelegate {
    let done: @MainActor () -> Void
    init(_ done: @escaping @MainActor () -> Void) { self.done = done }

    nonisolated func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        Task { @MainActor in done() }
    }
}
