import AVFoundation
import Speech
import UIKit

/// A probe (recordings plan, Task 1): does Apple's on-device speech recognition keep working, fed live,
/// while the app is in the background and the iPhone is locked? Recordings stream from Dotty while the
/// iPhone sits in a pocket, so the live draft depends on it. Feeds a bundled 16 kHz WAV (speech made with
/// `say`) in 100 ms buffers, one every 100 ms, and logs the app's state with each partial result.
@MainActor
enum SpeechProbe {
    @MainActor private final class Tally {
        var partials = 0, inBackground = 0
        var text = ""
        var done = false
    }

    static func run(log: @escaping @MainActor (String) -> Void) async -> String {
        let status = await SpeechAuth.request()
        guard status == .authorized else { return "speech probe: not authorized (\(status.rawValue))" }
        guard let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "en-US")) else { return "speech probe: no recognizer" }
        guard let url = Bundle.main.url(forResource: "speech_probe", withExtension: "wav"),
              let file = try? AVAudioFile(forReading: url) else { return "speech probe: no test audio" }
        log("speech probe: on-device \(recognizer.supportsOnDeviceRecognition), available \(recognizer.isAvailable); lock the iPhone now")
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.requiresOnDeviceRecognition = recognizer.supportsOnDeviceRecognition
        request.shouldReportPartialResults = true
        let background = UIApplication.shared.beginBackgroundTask(withName: "speech-probe")
        defer { UIApplication.shared.endBackgroundTask(background) }
        let tally = Tally()
        let started = Date()
        let task = SpeechAuth.recognize(recognizer, request, onText: { text, final in
            let state = UIApplication.shared.applicationState
            tally.partials += 1
            if state != .active { tally.inBackground += 1 }
            tally.text = text
            if tally.partials % 5 == 1 || final {
                log(String(format: "speech probe %.1f s [%@]: %@", Date().timeIntervalSince(started),
                           state == .active ? "front" : "background", String(text.suffix(60))))
            }
            if final { tally.done = true }
        }, onError: { error in
            log("speech probe error [\(UIApplication.shared.applicationState == .active ? "front" : "background")]: \(error)")
            tally.done = true
        })
        let format = file.processingFormat
        let chunk = AVAudioFrameCount(format.sampleRate / 10)  // 100 ms
        while let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: chunk),
              (try? file.read(into: buffer, frameCount: chunk)) != nil, buffer.frameLength > 0 {
            request.append(buffer)
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        request.endAudio()
        for _ in 0..<150 where !tally.done { try? await Task.sleep(nanoseconds: 100_000_000) }
        task.finish()
        let words = tally.text.split(separator: " ").count
        return "speech probe done: \(words) words, \(tally.partials) partial results, \(tally.inBackground) of them in the background. Text: \(tally.text)"
    }
}
