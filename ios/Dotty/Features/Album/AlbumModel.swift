import Foundation
import Observation
import UIKit

/// The Album Viewer cartridge seen from the phone: the photos and albums on Dotty's SD card,
/// what Dotty shows, and photos waiting to be sent over Wi-Fi
/// (firmware: cartridges/album/, lib/dotty_core/src/transfer.h).
@Observable
final class AlbumModel {
    struct Photo: Identifiable, Hashable {
        var id: String { name }
        /// File name on the card, e.g. "20240714-193205.pbm" (the date taken).
        let name: String
        let added: Date?

        /// From the name; nil for names that aren't a date.
        var taken: Date? { AlbumModel.dateTaken(name) }
    }

    struct Album: Identifiable, Hashable {
        var id: String { name }
        let name: String
        let count: Int
        let cover: String?
    }

    /// What Dotty shows: the active album ("" = all photos), the photo on screen, and the
    /// lock screen (the active album, a photo a minute, or one photo).
    struct OnDotty: Equatable {
        var album = ""
        var index = 0
        var count = 0
        var photo = ""
        var lockPhoto: String?
    }

    private(set) var photos: [Photo] = []
    private(set) var albums: [Album] = []
    private(set) var onDotty = OnDotty()
    /// Bumped whenever the library or an album changes, so album screens reload.
    private(set) var libraryVersion = 0
    private(set) var loaded = false
    /// Thumbnails by photo name, filled as they're needed.
    private(set) var pictures: [String: UIImage] = [:]
    let outbox: PhotoOutbox
    var error: String?
    var notice: String?

    private let link: DottyLink
    @ObservationIgnored private var wanted: [String] = []
    @ObservationIgnored private var fetching = false

    /// Each photo's bitmap, kept on the phone so thumbnails cost Bluetooth only once.
    static let cache = URL.cachesDirectory.appending(path: "AlbumPhotos", directoryHint: .isDirectory)

    init(link: DottyLink) {
        self.link = link
        outbox = PhotoOutbox(link: link)
        outbox.onSynced = { [weak self] in
            guard let self else { return }
            try? await self.loadLibrary()
            try? await self.reloadState()
        }
        outbox.onAdded = { [weak self] name, bitmap in self?.store(bitmap, for: name) }
    }

    // MARK: - Loading and events

    func load() async {
        await outbox.endInterruptedSession()
        do {
            try await loadLibrary()
            try await reloadState()
            loaded = true
        } catch {
            report(error)
        }
    }

    private func loadLibrary() async throws {
        let reply = try await link.send("album.library")
        photos = (reply["photos"] as? [[String: Any]] ?? []).compactMap { item in
            guard let name = item["name"] as? String else { return nil }
            let added = (item["added"] as? Double).flatMap { $0 > 1_600_000_000 ? Date(timeIntervalSince1970: $0) : nil }
            return Photo(name: name, added: added)
        }
        albums = (reply["albums"] as? [[String: Any]] ?? []).compactMap { item in
            guard let name = item["name"] as? String else { return nil }
            return Album(name: name, count: item["count"] as? Int ?? 0, cover: item["cover"] as? String)
        }
        libraryVersion += 1
    }

    private func reloadState() async throws {
        apply(state: try await link.send("album.status"))
    }

    func handle(_ message: DottyMessage) {
        switch message.event {
        case "album.state": apply(state: message)
        case "album.library": Task { try? await loadLibrary() }
        default: break
        }
    }

    private func apply(state message: DottyMessage) {
        var state = OnDotty()
        state.album = message["album"] as? String ?? ""
        state.index = message["index"] as? Int ?? 0
        state.count = message["count"] as? Int ?? 0
        state.photo = message["photo"] as? String ?? ""
        if let saver = message["screensaver"] as? [String: Any], saver["mode"] as? String == "photo" {
            state.lockPhoto = saver["photo"] as? String
        }
        onDotty = state
    }

    /// Dotty came back (after a sync, sleep or going out of range): clear stale errors, reload.
    func reconnected() async {
        guard !outbox.syncing else { return }
        error = nil
        outbox.error = nil
        await load()
        resumeThumbnails()
    }

    // MARK: - Thumbnails

    /// The photo's picture if the phone has it; otherwise it's fetched from Dotty (one at a
    /// time, ~7 KB over Bluetooth each) and appears when it arrives.
    func picture(_ name: String) -> UIImage? {
        if let image = pictures[name] { return image }
        if let bits = try? Data(contentsOf: Self.cache.appending(path: name)), let image = PhotoDither.picture(bits) {
            Task { @MainActor in self.pictures[name] = image }
            return image
        }
        if !wanted.contains(name) {
            wanted.append(name)
            Task { @MainActor in self.resumeThumbnails() }
        }
        return nil
    }

    private func resumeThumbnails() {
        guard !fetching, !wanted.isEmpty, link.connection == .connected, !outbox.syncing else { return }
        fetching = true
        Task {
            defer { fetching = false }
            while let name = wanted.first, link.connection == .connected, !outbox.syncing {
                if let reply = try? await link.send("album.photo", ["name": name]),
                   let text = reply["data"] as? String, let bits = Data(base64Encoded: text) {
                    store(bits, for: name)
                }
                wanted.removeAll { $0 == name }
            }
        }
    }

    private func store(_ bitmap: Data, for name: String) {
        try? FileManager.default.createDirectory(at: Self.cache, withIntermediateDirectories: true)
        try? bitmap.write(to: Self.cache.appending(path: name))
        pictures[name] = PhotoDither.picture(bitmap)
    }

    // MARK: - Showing

    /// Shows a photo (in an album, "" = all photos) on Dotty's screen.
    func show(album: String, photo: String? = nil) async {
        var arguments: [String: Any] = ["album": album]
        if let photo { arguments["photo"] = photo }
        await control("album.show", arguments)
    }

    /// The lock screen shows this album, a new photo every minute (it becomes Dotty's active album).
    func lockScreen(album: String) async {
        await control("album.screensaver", ["mode": "album", "album": album])
    }

    /// The lock screen always shows this photo.
    func lockScreen(photo: String) async {
        await control("album.screensaver", ["mode": "photo", "photo": photo])
    }

    private func control(_ command: String, _ arguments: [String: Any]) async {
        do {
            apply(state: try await link.send(command, arguments))
        } catch {
            report(error)
        }
    }

    // MARK: - Albums

    func albumPhotos(_ name: String) async -> [String] {
        (try? await link.send("album.album", ["name": name]))?["photos"] as? [String] ?? []
    }

    func createAlbum(_ name: String, with photoNames: [String] = []) async {
        await edit("album.create", ["name": name])
        if !photoNames.isEmpty { await add(photoNames, to: name) }
    }

    /// True when Dotty took the new name.
    func renameAlbum(_ name: String, to newName: String) async -> Bool {
        do {
            try await link.send("album.rename", ["name": name, "to": newName])
            try await loadLibrary()
            return true
        } catch {
            report(error)
            return false
        }
    }

    /// The album goes; its photos stay on Dotty.
    func deleteAlbum(_ name: String) async {
        await edit("album.delete", ["name": name])
    }

    func add(_ photoNames: [String], to album: String) async {
        for batch in Self.batches(photoNames) { await edit("album.add", ["name": album, "photos": batch]) }
    }

    func remove(_ photo: String, from album: String) async {
        await edit("album.remove", ["name": album, "photo": photo])
    }

    /// Positions as in `albumPhotos`.
    func move(in album: String, from: Int, to: Int) async {
        await edit("album.move", ["name": album, "from": from, "to": to])
    }

    /// Deletes photos from Dotty's card (and every album).
    func deletePhotos(_ names: [String]) async {
        for batch in Self.batches(names) { await edit("album.photo.delete", ["names": batch]) }
        for name in names {
            pictures[name] = nil
            try? FileManager.default.removeItem(at: Self.cache.appending(path: name))
        }
    }

    private func edit(_ command: String, _ arguments: [String: Any]) async {
        do {
            try await link.send(command, arguments)
            try await loadLibrary()
        } catch {
            report(error)
        }
    }

    /// Commands are limited to 512 bytes, so long lists go in several.
    static func batches(_ names: [String]) -> [[String]] {
        var batches: [[String]] = []
        var batch: [String] = []
        var bytes = 0
        for name in names {
            let size = name.utf8.count + 3
            if bytes + size > 380, !batch.isEmpty {
                batches.append(batch)
                batch = []
                bytes = 0
            }
            batch.append(name)
            bytes += size
        }
        if !batch.isEmpty { batches.append(batch) }
        return batches
    }

    /// "20240714-193205.pbm" → that date.
    static func dateTaken(_ name: String) -> Date? {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter.date(from: String(name.prefix(15)))
    }

    /// Keeps real failures; losing the connection is shown live by NotConnectedNotice.
    private func report(_ error: Error) {
        if !link.lostConnection(error) { self.error = error.localizedDescription }
    }
}

/// Photos edited on the phone, waiting to go to Dotty over Wi-Fi (5 KB each). The queue
/// survives the app closing; sending starts as soon as photos are added.
@Observable
final class PhotoOutbox {
    struct Upload: Identifiable, Codable {
        let id: UUID
        /// The name on Dotty ("20240714-193205.pbm").
        let name: String
        /// The album it goes into once on Dotty, if any.
        let album: String?

        var file: URL { PhotoOutbox.folder.appending(path: id.uuidString + ".pbm") }
    }

    private(set) var uploads: [Upload] = []
    private(set) var syncing = false
    private(set) var stage: String?
    private(set) var sent = 0
    var error: String?
    var notice: String?

    /// After a sync, once Dotty is reachable again (reload the library).
    var onSynced: (() async -> Void)?
    /// A photo was queued: its bitmap, to show straight away.
    var onAdded: ((String, Data) -> Void)?

    private let link: DottyLink
    static let folder = URL.applicationSupportDirectory.appending(path: "PhotoOutbox", directoryHint: .isDirectory)
    private static let manifest = folder.appending(path: "queue.json")
    private static let sessionKey = "album.transferSession"

    init(link: DottyLink) {
        self.link = link
        if let data = try? Data(contentsOf: Self.manifest),
           let saved = try? JSONDecoder().decode([Upload].self, from: data) {
            uploads = saved.filter { FileManager.default.fileExists(atPath: $0.file.path) }
        }
    }

    /// Names already used (queued), so new photos get their own.
    var queuedNames: Set<String> { Set(uploads.map(\.name)) }

    func add(_ bitmap: Data, name: String, album: String?) {
        try? FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
        let upload = Upload(id: UUID(), name: name, album: album)
        do {
            try PhotoDither.pbm(bitmap).write(to: upload.file)
            uploads.append(upload)
            save()
            onAdded?(name, bitmap)
        } catch {
            self.error = "Couldn't save the photo on this iPhone."
        }
    }

    func discardAll() {
        uploads.forEach { try? FileManager.default.removeItem(at: $0.file) }
        uploads = []
        save()
    }

    private func save() {
        try? FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
        try? JSONEncoder().encode(uploads).write(to: Self.manifest, options: .atomic)
    }

    /// Dotty joins Wi-Fi and opens a one-time upload server; each photo goes over HTTP, then
    /// the ones meant for an album are added to it.
    func sync() async {
        guard !syncing, !uploads.isEmpty else { return }
        syncing = true
        error = nil
        notice = nil
        sent = 0
        stage = "Dotty is joining Wi-Fi"
        UIApplication.shared.isIdleTimerDisabled = true
        let background = UIApplication.shared.beginBackgroundTask(withName: "Sending photos to Dotty")
        defer {
            syncing = false
            stage = nil
            UIApplication.shared.isIdleTimerDisabled = false
            UIApplication.shared.endBackgroundTask(background)
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
            if !link.lostConnection(error) { self.error = error.localizedDescription }
            return
        }
        UserDefaults.standard.set([server.absoluteString, token], forKey: Self.sessionKey)

        var done: [Upload] = []
        var failures = 0
        for upload in uploads {
            stage = "Sending photo \(done.count + 1) of \(uploads.count)"
            if await send(upload, to: server, token: token) {
                done.append(upload)
                sent = done.count
            } else {
                failures += 1
                if failures >= 2 { break }  // Dotty is out of reach
            }
        }

        stage = "Reconnecting to Dotty"
        await SongOutbox.endSession(server: server, token: token)
        UserDefaults.standard.removeObject(forKey: Self.sessionKey)
        try? await link.waitForReconnect(timeout: 30)

        // Into their albums (Bluetooth is back).
        let byAlbum = Dictionary(grouping: done.filter { $0.album != nil }, by: { $0.album! })
        for (album, items) in byAlbum {
            for batch in AlbumModel.batches(items.map(\.name)) {
                _ = try? await link.send("album.add", ["name": album, "photos": batch])
            }
        }

        done.forEach { try? FileManager.default.removeItem(at: $0.file) }
        uploads.removeAll { upload in done.contains { $0.id == upload.id } }
        save()
        if !uploads.isEmpty {
            error = done.isEmpty
                ? "Couldn't send photos to Dotty over Wi-Fi. Is this iPhone on \(ssid)? Allow Local Network for Dotty in Settings if iOS asked."
                : "\(done.count) sent, \(uploads.count) still waiting."
        } else if !done.isEmpty {
            notice = done.count == 1 ? "1 photo is on Dotty." : "\(done.count) photos are on Dotty."
        }
        await onSynced?()
    }

    /// The first request can fail while iOS asks for Local Network access: up to 3 tries.
    private func send(_ upload: Upload, to server: URL, token: String) async -> Bool {
        var components = URLComponents(url: server.appending(path: "upload"), resolvingAgainstBaseURL: false)!
        components.queryItems = [URLQueryItem(name: "dir", value: "photos"), URLQueryItem(name: "name", value: upload.name)]
        var request = URLRequest(url: components.url!)
        request.httpMethod = "POST"
        request.setValue(token, forHTTPHeaderField: "X-Dotty-Token")
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.timeoutInterval = 20
        for attempt in 0..<3 {
            if attempt > 0 { try? await Task.sleep(for: .seconds(2)) }
            if let (_, response) = try? await URLSession.shared.upload(for: request, fromFile: upload.file),
               (response as? HTTPURLResponse)?.statusCode == 200 {
                return true
            }
        }
        return false
    }

    /// A sync the app didn't finish leaves Dotty with Bluetooth off until its idle timeout.
    func endInterruptedSession() async {
        guard !syncing, let saved = UserDefaults.standard.stringArray(forKey: Self.sessionKey), saved.count == 2,
              let server = URL(string: saved[0]) else { return }
        UserDefaults.standard.removeObject(forKey: Self.sessionKey)
        await SongOutbox.endSession(server: server, token: saved[1])
    }
}
