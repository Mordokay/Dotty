#pragma once

#include <Arduino.h>

#include <functional>
#include <string>

#include "live_protocol.h"
#include "turn_tracker.h"
#include "live_transport.h"

// One conversation with Gemini Live over the WebSocket: connect + setup, push-to-talk turns,
// the reply's audio and text as they arrive, set_mood, token usage (TurnTracker decides which
// messages belong to the question). When the server says goAway (~10 min) the session closes
// after the reply; its resumption handle (resumeHandle()) lets the next connect carry on the
// conversation. Knows nothing about the screen or the audio hardware.
class GeminiLive {
 public:
  struct ToolAnswer {
    std::string result;
    bool speak = false;
  };
  struct Callbacks {
    std::function<void()> ready;                                // setup done: a turn can start
    std::function<void(const int16_t *pcm, size_t frames)> audio;  // reply audio, 24 kHz mono
    std::function<void(const std::string &text)> heard;         // the user's words (pieces)
    std::function<void(const std::string &text)> said;          // the reply's words (pieces)
    std::function<void(const std::string &mood)> mood;          // set_mood
    // remember / update_memory / forget_memory; returns the memory's id (remember) or -1
    std::function<int(const ToolCall &call)> memory;
    // set_moment / cancel_moment / list_moments / start_timer / stop_timer: the answer, sent silently with
    // the others, or (speak) as a result the model talks about (an error to put right, the list).
    std::function<ToolAnswer(const ToolCall &call)> tool;
    std::function<void(int tokens)> turnDone;                   // the reply ended
    std::function<void(int tokens)> spent;                      // tokens outside a question
    // The connection failed or closed: why, a retry delay if the server gave one (-1), and
    // whether it was the free allowance running out or the key being refused.
    std::function<void(const std::string &error, int retryDelaySeconds, bool quota, bool badKey)> failed;
  };

  // `config.resumeHandle` (from resumeHandle() / latestHandle()) carries on an earlier session's
  // conversation. The transport: its own WebSocket (Wi-Fi), or the iPhone (relay_link.h).
  bool begin(const std::string &apiKey, const LiveConfig &config, Callbacks callbacks, LiveTransport &transport);
  bool begin(const std::string &apiKey, const LiveConfig &config, Callbacks callbacks) {
    return begin(apiKey, config, std::move(callbacks), ws_);
  }
  void startTurn();
  void sendAudio(const int16_t *pcm, size_t frames);
  void endTurn();
  // A typed turn. continueQuestion: a follow-up of the question still open (the writer's reading),
  // so its reply isn't taken for a new question's leftovers.
  void sendText(const std::string &text, bool continueQuestion = false);
  // A tool's late answer with content (recall): opens a question for the reply it brings.
  void answerTool(const std::string &id, const std::string &name, const std::string &result);  // a typed turn (the end-of-chat memory review)
  void sendRaw(const std::string &frame) { t_->sendText(frame); }  // dev: anything, even invalid
  void poll();
  void close();
  bool ready() const { return ready_ && t_->connected(); }
  bool connected() const { return t_->connected(); }
  bool overRelay() const { return t_ != &ws_; }
  bool inTurn() const { return turns_.turn; }
  uint32_t lastMessageAt() const { return lastMessageAt_; }  // millis(): silence = a dead link
  // The handle to resume this conversation with, once the server asked to move (goAway).
  const std::string &resumeHandle() const { return goAway_ ? config_.resumeHandle : kNone; }
  // The latest handle the server gave (the grace-window hand-over moves the chat with it).
  const std::string &latestHandle() const { return config_.resumeHandle; }
  // Keep this conversation for the next connect (a relay chat closed while idle: the next press resumes).
  void keepResume() {
    if (!config_.resumeHandle.empty()) goAway_ = true;
  }
  // Another companion from now on: never resume this conversation in it.
  void forgetResume() {
    goAway_ = false;
    config_.resumeHandle.clear();
  }

 private:
  bool open();
  void onMessage(const std::string &message);
  void onAudio(const int16_t *pcm, size_t frames);  // the relay's reply audio (decoded)

  WsTransport ws_;
  LiveTransport *t_ = &ws_;
  std::string key_;
  LiveConfig config_;
  Callbacks cb_;
  bool ready_ = false;
  TurnTracker turns_;
  bool goAway_ = false;  // close once the reply is over (the next connect resumes)
  uint32_t lastMessageAt_ = 0;
  static inline const std::string kNone;
};
