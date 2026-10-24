import AVFoundation
import Foundation
import Speech
import UIKit

/// Voice notes from Dotty on this iPhone (spec: docs/superpowers/specs/2026-10-24-companion-recordings-
/// design.md). While the user records, Dotty streams the audio (rec.begin / rec.part / rec.undo / rec.end
/// and 0x1F 0x02 ADPCM frames); this service keeps it as a WAV, makes Apple's on-device draft live (a
/// recognition per part, so Undo drops its text too), and after Done:
///  1. sends the draft to Dotty at once (the companion reacts within seconds),
///  2. asks Gemini for the final transcript from the audio (the user's accent: Apple's draft as a hint),
///     and a summary with points (and a title when the given one is a placeholder),
///  3. sends those to Dotty (companion.recording.text; long text in companion.recording.piece).
/// Recordings the phone missed (away, joined late, a lost frame) are fetched whole (rec.fetch) when Dotty
/// asks (companion.recordings) or on each connection. App-wide, attached at launch, works in the
/// background like the relay.
@MainActor
final class RecordingService {
    static let shared = RecordingService()

    private weak var link: DottyLink?
    private var current: Live?
    private var syncing = false
    /// Recordings being finished, transcribed or fetched: nothing starts the same work twice (a sync
    /// arriving while one is still being handled used to fetch the whole file again).
    private var inFlight: Set<String> = []
    /// What an interrupted fetch had received (the next fetch asks Dotty to go on from there).
    private var partial: [String: [Int16]] = [:]
    private var lastFrameAt = Date.distantPast
    /// Up to this length the final transcript is made from the audio itself: the request (base64) must stay
    /// under Gemini's 20 MB; longer ones get the draft corrected without the audio.
    private static let audioLimit: Double = 6 * 60

    private struct Live {
        var id: String, companion: String, title: String
        var fetch = false  // a whole file Dotty resends (rec.fetch), not one being recorded
        var background: UIBackgroundTaskIdentifier = .invalid
        var parts = RecordingParts()
        var drafts: [DraftAccumulator] = []  // one per part
        var request: SFSpeechAudioBufferRecognitionRequest?
        var task: SFSpeechRecognitionTask?
    }

    func attach(_ link: DottyLink) {
        self.link = link
        link.onRecordingRaw = { [weak self] data in self?.frame(data) }
        link.onRecordingEvent = { [weak self] message in self?.event(message) }
        let previous = link.onReady
        link.onReady = { [weak self] in
            previous?()
            Task { await self?.sync() }
        }
        Task { _ = await SpeechAuth.request() }
    }

    private func log(_ s: String) { RelayService.shared.log("rec: " + s) }

    // MARK: - Streaming

    private func event(_ m: DottyMessage) {
        switch m.event {
        case "rec.begin":
            if let old = current { UIApplication.shared.endBackgroundTask(old.background) }
            stopRecognition()
            let id = m["id"] as? String ?? ""
            var live = Live(id: id, companion: m["companion"] as? String ?? "", title: m["title"] as? String ?? "Voice note")
            live.fetch = inFlight.contains(id)
            let from = m["from"] as? Int ?? 0
            if from > 0, let before = partial[id], before.count == from {
                live.parts.prefill(before)
                live.drafts = [DraftAccumulator()]
            }
            partial[id] = nil
            live.background = Self.beginBackground("voice-note")
            current = live
            lastFrameAt = Date()
            log("\(live.fetch ? "receiving" : "recording") \(live.title)\(from > 0 ? " from \(from / 16000) s" : "")")
        case _ where (m["id"] as? String) != current?.id && (m.event?.hasPrefix("rec.") ?? false):
            break  // about another recording (an old fetch's end after a new recording began)
        case "rec.part":
            finishPartRecognition()
            current?.parts.startPart()
            current?.drafts.append(DraftAccumulator())
            startPartRecognition()
        case "rec.undo":
            current?.task?.cancel()
            current?.task = nil
            current?.request = nil
            current?.parts.undo()
            if !(current?.drafts.isEmpty ?? true) { current?.drafts.removeLast() }
        case "rec.end":
            guard var live = current else { return }
            current = nil
            if m["how"] as? String != "done" {
                live.task?.cancel()
                if live.fetch && !live.parts.gap {  // the link went: the next fetch goes on from here
                    partial[live.id] = live.parts.samples
                } else {
                    inFlight.remove(live.id)
                }
                log(live.fetch ? "\(live.title): the transfer stopped, it goes on later" : "discarded on Dotty")
                UIApplication.shared.endBackgroundTask(live.background)
                return
            }
            live.request?.endAudio()
            Task { await self.finished(live) }
        case "companion.recordings":
            Task { await sync() }
        default:
            break
        }
    }

    private func frame(_ data: Data) {
        guard current != nil else { return }
        lastFrameAt = Date()
        if current?.parts.partCount == 0 {  // a whole file resent (rec.fetch): one part, no rec.part
            current?.parts.startPart()
            current?.drafts.append(DraftAccumulator())
            startPartRecognition()
        }
        current?.parts.add(data)
        let pcm = Adpcm.decodeFrame(data.dropFirst(3))
        guard let request = current?.request, !pcm.isEmpty,
              let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 16000, channels: 1, interleaved: false),
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(pcm.count)) else { return }
        buffer.frameLength = AVAudioFrameCount(pcm.count)
        let out = buffer.floatChannelData![0]
        for (i, s) in pcm.enumerated() { out[i] = Float(s) / 32768 }
        request.append(buffer)
    }

    // MARK: - Apple's draft, live

    private func startPartRecognition() {
        guard SFSpeechRecognizer.authorizationStatus() == .authorized,
              let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "en-US")), recognizer.isAvailable else { return }
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.requiresOnDeviceRecognition = recognizer.supportsOnDeviceRecognition
        request.shouldReportPartialResults = true
        let index = max(0, (current?.drafts.count ?? 1) - 1)
        let id = current?.id
        current?.request = request
        current?.task = SpeechAuth.recognize(recognizer, request, onText: { [weak self] text, _ in
            self?.setDraft(id: id, index: index, text: text)
        }, onError: { _ in })
    }

    private var finishedDrafts: [String: [DraftAccumulator]] = [:]  // a recording's drafts after rec.end (late results)

    private func setDraft(id: String?, index: Int, text: String) {
        if current?.id == id, var drafts = current?.drafts, index < drafts.count {
            drafts[index].update(text)
            current?.drafts = drafts
        } else if let id, var drafts = finishedDrafts[id], index < drafts.count {
            drafts[index].update(text)
            finishedDrafts[id] = drafts
        }
    }

    private func finishPartRecognition() {
        current?.request?.endAudio()
        current?.request = nil
    }

    private func stopRecognition() {
        current?.task?.cancel()
        current = nil
    }

    // MARK: - After Done

    private func finished(_ live: Live) async {
        inFlight.insert(live.id)
        defer {
            inFlight.remove(live.id)
            UIApplication.shared.endBackgroundTask(live.background)
        }
        if live.parts.gap {
            log("\(live.title): frames were lost, asking Dotty for the whole recording")
            await fetch(companion: live.companion, id: live.id)
            return
        }
        // Saved at once: a sync arriving meanwhile finds it (and doesn't fetch it again).
        let samples = live.parts.samples
        var r = RecordingStore.find(companion: live.companion, id: live.id)
            ?? LocalRecording(id: live.id, companion: live.companion, title: live.title,
                              created: RecordingStore.date(fromName: live.id), duration: 0)
        r.duration = Double(samples.count) / 16000
        RecordingStore.saveAudio(r, wav: RecordingParts.wav(samples))
        RecordingStore.save(r)
        finishedDrafts[live.id] = live.drafts
        async let hints = memoryHints(live.companion)       // read from Dotty meanwhile (Gemini's hints)
        try? await Task.sleep(nanoseconds: 1_500_000_000)  // the last part's final words
        var draft = (finishedDrafts.removeValue(forKey: live.id) ?? live.drafts).map(\.text).filter { !$0.isEmpty }
            .joined(separator: " ")
        if draft.isEmpty { draft = await fileDraft(RecordingStore.wavURL(r)) }  // no live words (the background, a fetch)
        r.draft = draft
        RecordingStore.save(r)
        log("\(r.title): \(Int(r.duration)) s, Apple's draft \(draft.split(separator: " ").count) words")
        let memories = await hints
        if !draft.isEmpty { await sendText(r, transcript: draft, isDraft: true) }  // the reaction needs it now
        await makeFinal(r, memories: memories)
    }

    /// Apple's draft from the saved file (when nothing came live).
    private func fileDraft(_ url: URL) async -> String {
        guard SFSpeechRecognizer.authorizationStatus() == .authorized,
              let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "en-US")), recognizer.isAvailable else { return "" }
        let request = SFSpeechURLRecognitionRequest(url: url)
        request.requiresOnDeviceRecognition = recognizer.supportsOnDeviceRecognition
        var text = DraftAccumulator()
        var done = false
        let task = SpeechAuth.recognize(recognizer, request, onText: { t, final in
            text.update(t)
            if final { done = true }
        }, onError: { _ in done = true })
        for _ in 0..<600 where !done { try? await Task.sleep(nanoseconds: 100_000_000) }
        task.cancel()
        return text.text
    }

    /// Asks Dotty for the whole recording (or the rest of an interrupted transfer) and waits while it comes.
    private func fetch(companion: String, id: String) async {
        let from = partial[id]?.count ?? 0
        inFlight.insert(id)
        log("fetching \(id) from Dotty\(from > 0 ? " (from \(from / 16000) s)" : "")")
        guard (try? await link?.send("rec.fetch", ["companion": companion, "id": id, "from": from], timeout: 10)) != nil else {
            inFlight.remove(id)
            return
        }
        lastFrameAt = Date()
        // As long as frames keep coming (a 30-minute note takes ~10 minutes), not a fixed time.
        while current?.id == id || (inFlight.contains(id) && Date().timeIntervalSince(lastFrameAt) < 15) {
            if current?.id != id && partial[id] != nil { break }  // stopped: it goes on next time
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }

    /// Gemini's transcript (from the audio when short enough) and the summary, then to Dotty.
    private func makeFinal(_ start: LocalRecording, memories known: String? = nil) async {
        var r = start
        guard !r.nothingSaid, RecordingRetry.shouldTry(attempts: r.finalAttempts, last: r.lastFinalAttempt, now: Date()) else { return }
        let task = Self.beginBackground("voice-note-final")
        defer { UIApplication.shared.endBackgroundTask(task) }
        r.finalAttempts += 1
        r.lastFinalAttempt = Date()
        RecordingStore.save(r)
        let started = Date()
        let memories: String
        if let known { memories = known } else { memories = await memoryHints(r.companion) }
        let memoriesAt = Date()
        do {
            // One request: the transcript, the summary and points, a title (two calls cost a round trip each).
            let schema: [String: Any] = ["type": "OBJECT", "properties": [
                "transcript": ["type": "STRING"], "summary": ["type": "STRING"],
                "points": ["type": "ARRAY", "items": ["type": "STRING"]], "title": ["type": "STRING"],
            ], "required": ["transcript", "summary", "points", "title"]]
            let placeholder = r.title.lowercased() == "voice note" || r.title.lowercased() == "test note"
            let hints = "Hints: the note's title is \"\(r.title)\"; names and places the speaker knows: \(memories.isEmpty ? "none" : memories)."
            // The user (2026-10-24): not literal; a light polish is fine as long as the message doesn't change.
            let polish = "Write it as clean, readable text: drop filler words (uh, um, filler \"like\" and \"you know\"), false " +
                "starts and accidental repeats, and fix grammar slips only where needed; keep the speaker's own wording, ideas and " +
                "order, never add, remove or change what they meant. If no words are spoken, transcript is empty."
            let rest = " Also: summary = one or two sentences on what it's about; points = 3 to 6 short key points (the facts, plans " +
                "and ideas); title = " + (placeholder ? "a short title (2 to 5 words)" : "\"\(r.title)\" unchanged") + "."
            var answer: [String: Any] = [:]
            var audioKB = 0
            if !r.final.isEmpty {  // the transcript is there (an earlier try's summary failed): the summary alone
                answer = try await GeminiText.generate("Here is a voice note's text." + rest + " transcript = the text unchanged." +
                                                       "\n\nText: \(r.final)", schema: schema)
            } else if r.duration <= Self.audioLimit, let wav = try? Data(contentsOf: RecordingStore.wavURL(r)) {
                audioKB = wav.count / 1024
                answer = try await GeminiText.generateWithAudio(
                    "Transcribe this voice note, in English. The speaker is fluent but not a native speaker and has an accent: use the " +
                    "hints to resolve words that are hard to hear. \(hints) An automatic draft (it may contain mishearings): \"\(r.draft)\". " +
                    polish + rest, wav: wav, schema: schema)
            } else {
                answer = try await GeminiText.generate(
                    "Here is an automatic transcript of a long voice note by a fluent non-native English speaker with an accent. Fix " +
                    "words that were clearly misheard, using the hints and the context. \(hints) " + polish + rest +
                    "\n\nTranscript: \(r.draft)", schema: schema)
            }
            let geminiAt = Date()
            if r.final.isEmpty { r.final = (answer["transcript"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines) }
            if r.final.isEmpty {  // nothing was said: no summary, no more tries
                r.nothingSaid = true
                r.needsFinal = false
                RecordingStore.save(r)
                log("\(r.title): no words in it")
                return
            }
            r.summary = answer["summary"] as? String ?? ""
            r.points = (answer["points"] as? [String] ?? []).prefix(6).map { String($0.prefix(80)) }
            if placeholder, let t = answer["title"] as? String, !t.isEmpty { r.title = String(t.prefix(60)) }
            r.needsFinal = r.summary.isEmpty
            RecordingStore.save(r)
            log(String(format: "%@: memories %.1f s, Gemini %.1f s (%d KB of audio)", r.title, memoriesAt.timeIntervalSince(started),
                       geminiAt.timeIntervalSince(memoriesAt), audioKB))
        } catch {
            r.needsFinal = true
            log("\(r.title): Gemini couldn't make the final transcript yet (\(error.localizedDescription))")
        }
        RecordingStore.save(r)
        let sendStarted = Date()
        defer { log(String(format: "%@: sending to Dotty %.1f s", r.title, Date().timeIntervalSince(sendStarted))) }
        if !r.final.isEmpty {
            await sendText(r, transcript: r.final, isDraft: false)
        } else if !r.draft.isEmpty {
            await sendText(r, transcript: r.draft, isDraft: true)
        }
    }

    private func memoryHints(_ companion: String) async -> String {
        guard let r = try? await link?.send("companion.memories", ["companion": companion, "offset": 0, "limit": 50]) else { return "" }
        let texts = (r["memories"] as? [[String: Any]] ?? []).compactMap { $0["text"] as? String }
        return String(texts.joined(separator: "; ").prefix(3000))
    }

    /// companion.recording.text: inline when it fits a command (512 bytes), else the long fields in pieces.
    private func sendText(_ r: LocalRecording, transcript: String, isDraft: Bool) async {
        guard let link else { return }
        var head: [String: Any] = ["id": r.id, "companion": r.companion, "isDraft": isDraft, "summary": String(r.summary.prefix(300)),
                                   "points": r.points]
        if !r.title.isEmpty { head["title"] = r.title }
        var inline = head
        inline["transcript"] = transcript
        do {
            if let size = try? JSONSerialization.data(withJSONObject: inline).count, size < 460 {
                _ = try await link.send("companion.recording.text", inline)
            } else {
                head.removeValue(forKey: "draft")
                head["pieces"] = true
                while (try? JSONSerialization.data(withJSONObject: head).count) ?? 0 > 460, !(head["points"] as? [String] ?? []).isEmpty {
                    head["points"] = Array((head["points"] as? [String] ?? []).dropLast())
                }
                _ = try await link.send("companion.recording.text", head)
                try await sendPieces(link, id: r.id, field: "transcript", text: transcript)
                _ = try await link.send("companion.recording.text", ["id": r.id, "done": true])
            }
            if !isDraft {
                var sent = r
                sent.textSent = true
                RecordingStore.save(sent)
            }
            log("\(r.title): \(isDraft ? "draft" : "final transcript") sent to Dotty")
        } catch {
            log("\(r.title): couldn't send the text to Dotty (\(error.localizedDescription))")
        }
    }

    private func sendPieces(_ link: DottyLink, id: String, field: String, text: String) async throws {
        let bytes = Array(text.utf8)
        var at = 0
        while at < bytes.count {
            var end = min(at + 240, bytes.count)
            while end < bytes.count && (bytes[end] & 0xC0) == 0x80 { end -= 1 }  // whole UTF-8 characters
            let piece = String(decoding: bytes[at..<end], as: UTF8.self)
            _ = try await link.send("companion.recording.piece", ["id": id, "field": field, "offset": at, "total": bytes.count, "text": piece])
            at = end
        }
    }

    // MARK: - Catching up

    /// Recordings Dotty holds that this iPhone lacks, or whose text Dotty lacks.
    func sync() async {
        guard !syncing, let link else { return }
        syncing = true
        let task = Self.beginBackground("voice-note-sync")
        defer {
            syncing = false
            UIApplication.shared.endBackgroundTask(task)
        }
        guard let list = try? await link.send("companion.list") else { return }
        for c in list["companions"] as? [[String: Any]] ?? [] {
            guard let companion = c["id"] as? String,
                  let r = try? await link.send("companion.recordings", ["companion": companion]) else { continue }
            for item in r["recordings"] as? [[String: Any]] ?? [] {
                guard let id = item["name"] as? String, !inFlight.contains(id), current?.id != id else { continue }
                let synced = item["synced"] as? Bool ?? false, hasText = item["text"] as? Bool ?? false
                let isDraft = item["isDraft"] as? Bool ?? false
                if let local = RecordingStore.find(companion: companion, id: id),
                   FileManager.default.fileExists(atPath: RecordingStore.wavURL(local).path) {
                    if local.nothingSaid { continue }
                    if local.needsFinal || local.final.isEmpty || local.summary.isEmpty { await makeFinal(local) }
                    else if !hasText || isDraft || !local.textSent { await sendText(local, transcript: local.final, isDraft: false) }
                } else if !synced || !hasText {
                    await fetch(companion: companion, id: id)
                }
            }
        }
    }

    /// A background task that ends itself if iOS runs out of patience (the expiry handler runs on main).
    private static func beginBackground(_ name: String) -> UIBackgroundTaskIdentifier {
        final class Box: @unchecked Sendable { var id: UIBackgroundTaskIdentifier = .invalid }
        let box = Box()
        box.id = UIApplication.shared.beginBackgroundTask(withName: name) {
            UIApplication.shared.endBackgroundTask(box.id)
        }
        return box.id
    }
}
