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
        var shuffle = false
        /// When `position` was true, to move the progress bar between updates.
        var receivedAt = Date()

        func position(at date: Date) -> Double {
            guard playing else { return Double(position) }
            return min(Double(duration), Double(position) + date.timeIntervalSince(receivedAt))
        }
    }

    private(set) var songs: [Song] = []
    private(set) var playlists: [Playlist] = []
    private(set) var now = NowPlaying()
    /// Bumped whenever the library or a playlist changes, so playlist screens reload.
    private(set) var libraryVersion = 0
    private(set) var loaded = false
    /// Songs waiting to go to Dotty over Wi-Fi.
    let outbox: SongOutbox
    var error: String?
    var notice: String?

    private let link: DottyLink

    init(link: DottyLink) {
        self.link = link
        outbox = SongOutbox(link: link)
        outbox.onSynced = { [weak self] in
            try? await self?.loadLibrary()
            try? await self?.reloadState()
        }
    }

    // MARK: - Loading and events

    func load() async {
        await outbox.endInterruptedSession()
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
        let skipped = outbox.dropSongsOnDotty(songs)
        if skipped > 0 { notice = skipped == 1 ? "1 waiting song was already on Dotty." : "\(skipped) waiting songs were already on Dotty." }
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
        state.shuffle = message["shuffle"] as? Bool ?? false
        now = state
    }

    // MARK: - Playback

    func toggle() async { await control("music.toggle") }
    func next() async { await control("music.next") }
    func previous() async { await control("music.prev") }

    func setShuffle(_ on: Bool) async {
        await control("music.shuffle", ["on": on])
    }

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

    /// True when Dotty took the new name.
    func renamePlaylist(_ name: String, to newName: String) async -> Bool {
        do {
            try await link.send("music.playlist.rename", ["name": name, "to": newName])
            try await loadLibrary()
            return true
        } catch {
            self.error = describe(error)
            return false
        }
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

    private func reloadState() async throws {
        apply(state: try await link.send("music.status"))
    }

    /// Dotty came back (after a sync, sleep or going out of range): clear stale errors, reload.
    func reconnected() async {
        guard !outbox.syncing else { return }
        error = nil
        await load()
    }

    private func describe(_ error: Error) -> String {
        link.connection == .connected ? error.localizedDescription : "Dotty isn't connected. Press PWR on Dotty to wake it."
    }
}

