import Speech

/// Speech recognition's permission and results arrive on Apple's own queues. Under Swift 6 a closure
/// written inside a @MainActor type is main-actor isolated, and the runtime traps when Apple calls it
/// from another queue: the handlers live here, nonisolated and @Sendable, and hop to the main actor
/// themselves.
enum SpeechAuth {
    nonisolated static func request() async -> SFSpeechRecognizerAuthorizationStatus {
        await withCheckedContinuation { (c: CheckedContinuation<SFSpeechRecognizerAuthorizationStatus, Never>) in
            SFSpeechRecognizer.requestAuthorization { @Sendable status in c.resume(returning: status) }
        }
    }

    /// A recognition task whose results go to `onText` on the main actor (the text so far, and whether it's final).
    nonisolated static func recognize(_ recognizer: SFSpeechRecognizer, _ request: SFSpeechRecognitionRequest,
                                      onText: @escaping @MainActor @Sendable (String, Bool) -> Void,
                                      onError: @escaping @MainActor @Sendable (String) -> Void) -> SFSpeechRecognitionTask {
        recognizer.recognitionTask(with: request) { @Sendable result, error in
            let text = result?.bestTranscription.formattedString
            let final = result?.isFinal ?? false
            let failure = error?.localizedDescription
            Task { @MainActor in
                if let text { onText(text, final) }
                if let failure { onError(failure) }
            }
        }
    }
}
