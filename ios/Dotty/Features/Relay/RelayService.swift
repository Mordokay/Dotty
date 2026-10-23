import Foundation
import Observation
import UIKit

/// A companion chat through this iPhone (spec: docs/superpowers/specs/2026-10-23-companion-relay-
/// design.md). Dotty keeps the logic; this is the pipe that holds the key:
///  - relay.open {setup, key}: a Gemini Live WebSocket with the Keychain key whose last 4 characters
///    match, Dotty's setup sent as it is;
///  - relay.send {message}: forwarded; Dotty's voice (raw 0x1F 0x01 seq + IMA ADPCM, 16 kHz) decoded and
///    sent as audio, in 100 ms messages;
///  - Gemini's messages go back without their audio (0x02 JSON pieces), the audio as ADPCM frames
///    (0x01 seq + frame, 24 kHz), all on one ordered queue so a turnComplete never overtakes the audio;
///  - relay.fetch {id, model, body}: a text-model call (writer, recall, macros) → {relay: fetched};
///  - a closed WebSocket → {relay: closed, code, reason}; relay.close → closed here.
/// App-wide (attached at launch), so it works in the background with the iPhone locked: Dotty's
/// notifications wake the app (bluetooth-central), and a background task covers each reply and fetch,
/// when Dotty sends nothing. The key never leaves the Keychain; only Google's endpoints are called.
@MainActor @Observable
final class RelayService {
    static let shared = RelayService()
    static let version = 1

    private(set) var lines: [String] = []

    @ObservationIgnored private weak var link: DottyLink?
    @ObservationIgnored private var socket: URLSessionWebSocketTask?
    @ObservationIgnored private var key: String?
    @ObservationIgnored private var voicePCM = Data()
    @ObservationIgnored private var encoder = Adpcm.State()
    @ObservationIgnored private var seq: UInt8 = 0
    @ObservationIgnored private var outbox: [Data] = []
    @ObservationIgnored private var pumping = false
    @ObservationIgnored private var background: UIBackgroundTaskIdentifier = .invalid
    @ObservationIgnored private var openedAt = Date()
    @ObservationIgnored private var endAt = Date()
    @ObservationIgnored private var firstAudio = true

    private static let host = "generativelanguage.googleapis.com"

    func attach(_ link: DottyLink) {
        self.link = link
        link.onRelayEvent = { [weak self] message in self?.event(message) }
        link.onRelayRaw = { [weak self] data in self?.voice(data) }
        link.onReady = { [weak self] in self?.hello() }
    }

    func log(_ s: String) {
        let stamp = Date().formatted(.dateTime.hour().minute().second())
        let where_ = UIApplication.shared.applicationState == .active ? "" : " [background]"
        lines.append("\(stamp) \(s)\(where_)")
        if lines.count > 300 { lines.removeFirst(lines.count - 300) }
    }

    /// Tells Dotty this iPhone can relay, and with which keys (their last 4 characters only).
    private func hello() {
        guard let link else { return }
        let hints = GeminiKeys.load().map(GeminiKeys.hint)
        Task {
            // Cartridges other than the Companion don't know the command: that's fine.
            _ = try? await link.send("relay.hello", ["version": Self.version, "keys": hints], timeout: 5)
        }
    }

    // MARK: - Dotty → Gemini

    private func event(_ message: DottyMessage) {
        switch message.event {
        case "relay.open":
            open(setup: message["setup"] as? String ?? "", hint: message["key"] as? String ?? "")
        case "relay.send":
            let text = message["message"] as? String ?? ""
            if text.contains("\"activityEnd\"") {
                flushVoice()  // the voice before the end of the turn
                endAt = Date()
                firstAudio = true
            }
            if text.contains("\"activityStart\"") || text.contains("\"clientContent\"") || text.contains("\"toolResponse\"") {
                beginBackground()  // a reply is coming
            }
            send(text)
        case "relay.fetch":
            fetch(id: message["id"] as? Int ?? 0, model: message["model"] as? String ?? "", body: message["body"] as? String ?? "")
        case "relay.close":
            log("Dotty closed the chat")
            closeSocket()
        default:
            break
        }
    }

    private func voice(_ frame: Data) {
        guard frame.count > 3, frame[frame.startIndex + 1] == 0x01 else { return }
        let pcm = Adpcm.decodeFrame(frame.dropFirst(3))
        pcm.withUnsafeBytes { voicePCM.append(contentsOf: $0) }
        if voicePCM.count >= 3200 { flushVoice() }  // 100 ms at 16 kHz
    }

    private func flushVoice() {
        guard !voicePCM.isEmpty else { return }
        send(#"{"realtimeInput":{"audio":{"mimeType":"audio/pcm;rate=16000","data":""# + voicePCM.base64EncodedString() + #""}}}"#)
        voicePCM = Data()
    }

    private func open(setup: String, hint: String) {
        closeSocket()
        guard let found = GeminiKeys.load().first(where: { GeminiKeys.hint($0) == hint }),
              let url = URL(string: "wss://\(Self.host)/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=\(found)") else {
            log("no key ..\(hint) on this iPhone")
            reply(["relay": "closed", "code": 4001, "reason": "no such key on this iPhone"])
            return
        }
        key = found
        encoder = Adpcm.State()
        seq = 0
        voicePCM = Data()
        openedAt = Date()
        beginBackground()
        let task = URLSession.shared.webSocketTask(with: url)
        task.maximumMessageSize = 16 << 20
        socket = task
        task.resume()
        task.send(.string(setup)) { _ in }
        log("chat opened (\(setup.utf8.count / 1024) KB setup)")
        receive(task)
    }

    private func send(_ text: String) {
        guard let socket else {
            reply(["relay": "closed", "code": 4002, "reason": "no chat open on the iPhone"])
            return
        }
        socket.send(.string(text)) { [weak self] error in
            if let error { Task { @MainActor in self?.log("send failed: \(error.localizedDescription)") } }
        }
    }

    private func closeSocket() {
        socket?.cancel(with: .normalClosure, reason: nil)
        socket = nil
        endBackground()
    }

    // MARK: - Gemini → Dotty

    private func receive(_ task: URLSessionWebSocketTask) {
        task.receive { [weak self] result in
            Task { @MainActor in
                guard let self, task === self.socket else { return }
                switch result {
                case .failure(let error):
                    let code = task.closeCode == .invalid ? 1006 : task.closeCode.rawValue
                    let reason = task.closeReason.flatMap { String(data: $0, encoding: .utf8) } ?? error.localizedDescription
                    self.log("Gemini closed \(code): \(reason)")
                    self.reply(["relay": "closed", "code": code, "reason": reason])
                    self.socket = nil
                    self.endAfterQueue()
                case .success(let message):
                    switch message {
                    case .data(let d): self.forward(d)
                    case .string(let s): self.forward(Data(s.utf8))
                    @unknown default: break
                    }
                    self.receive(task)
                }
            }
        }
    }

    /// One of Gemini's messages: its JSON without the audio, then the audio, in order.
    private func forward(_ data: Data) {
        let (json, audio) = RelayMessages.stripAudio(data)
        if json.count > 2 { enqueue(json: json) }
        for chunk in audio {
            if firstAudio {
                firstAudio = false
                log(String(format: "reply audio %.0f ms after the turn", Date().timeIntervalSince(endAt) * 1000))
            }
            var samples = [Int16](repeating: 0, count: chunk.count / 2)
            _ = samples.withUnsafeMutableBytes { chunk.copyBytes(to: $0) }
            let per = frameSamples
            var at = 0
            while at < samples.count {  // the whole chunk now: nothing waits behind a later message
                let end = min(at + per, samples.count)
                var frame = Data([0x01, seq])
                seq &+= 1
                frame.append(Adpcm.encodeFrame(samples[at..<end], &encoder))
                outbox.append(frame)
                at = end
            }
        }
        pump()
        if let root = (try? JSONSerialization.jsonObject(with: json)) as? [String: Any],
           (root["serverContent"] as? [String: Any])?["turnComplete"] as? Bool == true {
            endAfterQueue()  // the reply is out once the queue empties
        }
    }

    /// Samples per audio frame that fit one write (570 at the usual 290 bytes).
    private var frameSamples: Int { max(2, ((link?.maxWriteLength ?? 290) - 2 - Adpcm.header) * 2) }

    private func reply(_ object: [String: Any]) {
        guard let json = try? JSONSerialization.data(withJSONObject: object, options: [.withoutEscapingSlashes]) else { return }
        enqueue(json: json)
        pump()
    }

    private func enqueue(json: Data) {
        outbox.append(contentsOf: RelayMessages.pieces(json, max: link?.maxWriteLength ?? 290))
    }

    @ObservationIgnored private var endWhenEmpty = false

    private func endAfterQueue() {
        endWhenEmpty = true
        if outbox.isEmpty && !pumping { endBackground() }
    }

    private func pump() {
        guard !pumping, let link else { return }
        pumping = true
        Task {
            while !outbox.isEmpty {
                let next = outbox.removeFirst()
                if !(await link.writeStream(next)) {  // Dotty is gone: the chat with it too
                    log("Dotty went away")
                    outbox = []
                    socket?.cancel(with: .goingAway, reason: nil)
                    socket = nil
                    break
                }
            }
            pumping = false
            if endWhenEmpty { endBackground() }
        }
    }

    // MARK: - Text-model calls for Dotty

    private func fetch(id: Int, model: String, body: String) {
        guard let key = key ?? GeminiKeys.load().first,
              model.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "-" || $0 == "." }),
              let url = URL(string: "https://\(Self.host)/v1beta/models/\(model):generateContent?key=\(key)") else {
            reply(["relay": "fetched", "id": id, "status": 0, "body": "no key or a bad model name"])
            return
        }
        beginBackground()
        var request = URLRequest(url: url, timeoutInterval: 30)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = Data(body.utf8)
        let started = Date()
        Task {
            var status = 0
            var text = ""
            do {
                let (data, response) = try await URLSession.shared.data(for: request)
                status = (response as? HTTPURLResponse)?.statusCode ?? 0
                text = String(data: data, encoding: .utf8) ?? ""
            } catch {
                text = error.localizedDescription
            }
            log(String(format: "fetch %d: %d in %.1f s", id, status, Date().timeIntervalSince(started)))
            reply(["relay": "fetched", "id": id, "status": status, "body": text])
            endAfterQueue()
        }
    }

    // MARK: - Background

    private func beginBackground() {
        endWhenEmpty = false
        guard background == .invalid else { return }
        background = UIApplication.shared.beginBackgroundTask(withName: "relay") { [weak self] in
            Task { @MainActor in
                self?.log("background time ran out")
                self?.endBackground()
            }
        }
    }

    private func endBackground() {
        endWhenEmpty = false
        guard background != .invalid else { return }
        UIApplication.shared.endBackgroundTask(background)
        background = .invalid
    }
}
