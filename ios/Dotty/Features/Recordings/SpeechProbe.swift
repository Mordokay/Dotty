import AVFoundation
import Speech
import UIKit

/// A probe (recordings plan, Task 1): does Apple's on-device speech recognition keep working, fed live,
/// while the app is in the background and the iPhone is locked? Recordings stream from Dotty while the
/// iPhone sits in a pocket, so the live draft depends on it. Feeds a bundled 16 kHz WAV (speech made with
/// `say`) in 100 ms buffers, one every 100 ms, and logs the app's state with each partial result.
@MainActor
enum SpeechProbe {
    static func run(log: @escaping @MainActor (String) -> Void) async -> String {
        let status = await withCheckedContinuation { (c: CheckedContinuation<SFSpeechRecognizerAuthorizationStatus, Never>) in
            SFSpeechRecognizer.requestAuthorization { c.resume(returning: $0) }
        }
        guard status == .authorized else { return "speech probe: not authorized (\(status.rawValue))" }
        guard let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "en-US")) else { return "speech probe: no recognizer" }
        guard let url = Bundle.main.url(forResource: "speech_probe", withExtension: "wav"),
              let file = try? AVAudioFile(forReading: url) else { return "speech probe: no test audio" }
        log("speech probe: on-device \(recognizer.supportsOnDeviceRecognition), available \(recognizer.isAvailable); lock the iPhone now")
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.requiresOnDeviceRecognition = true
        request.shouldReportPartialResults = true
        let background = UIApplication.shared.beginBackgroundTask(withName: "speech-probe")
        defer { UIApplication.shared.endBackgroundTask(background) }
        var partials = 0, inBackground = 0
        var finalText = ""
        let started = Date()
        let done = AsyncStream<Void> { continuation in
            _ = recognizer.recognitionTask(with: request) { result, error in
                Task { @MainActor in
                    let state = UIApplication.shared.applicationState
                    if let result {
                        partials += 1
                        if state != .active { inBackground += 1 }
                        finalText = result.bestTranscription.formattedString
                        if partials % 5 == 1 || result.isFinal {
                            log(String(format: "speech probe %.1f s [%@]: %@", Date().timeIntervalSince(started),
                                       state == .active ? "front" : "background", String(finalText.suffix(60))))
                        }
                        if result.isFinal { continuation.finish() }
                    }
                    if let error {
                        log("speech probe error [\(state == .active ? "front" : "background")]: \(error.localizedDescription)")
                        continuation.finish()
                    }
                }
            }
        }
        let format = file.processingFormat
        let chunk = AVAudioFrameCount(format.sampleRate / 10)  // 100 ms
        while let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: chunk),
              (try? file.read(into: buffer, frameCount: chunk)) != nil, buffer.frameLength > 0 {
            request.append(buffer)
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        request.endAudio()
        for await _ in done {}
        let words = finalText.split(separator: " ").count
        return "speech probe done: \(words) words, \(partials) partial results, \(inBackground) of them in the background. Text: \(finalText)"
    }
}
