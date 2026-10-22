#pragma once

#include <Arduino.h>

#include <functional>
#include <string>

#include "live_protocol.h"
#include "turn_tracker.h"
#include "ws_client.h"

// One conversation with Gemini Live over the WebSocket: connect + setup, push-to-talk turns,
// the reply's audio and text as they arrive, set_mood, token usage (TurnTracker decides which
// messages belong to the question). When the server says goAway (~10 min) the session closes
// after the reply; its resumption handle (resumeHandle()) lets the next connect carry on the
// conversation. Knows nothing about the screen or the audio hardware.
class GeminiLive {
 public:
  struct Callbacks {
    std::function<void()> ready;                                // setup done: a turn can start
    std::function<void(const int16_t *pcm, size_t frames)> audio;  // reply audio, 24 kHz mono
    std::function<void(const std::string &text)> heard;         // the user's words (pieces)
    std::function<void(const std::string &text)> said;          // the reply's words (pieces)
    std::function<void(const std::string &mood)> mood;          // set_mood
    std::function<void(int tokens)> turnDone;                   // the reply ended
    std::function<void(int tokens)> spent;                      // tokens outside a question
    // The connection failed or closed: why, a retry delay if the server gave one (-1), and
    // whether it was the free allowance running out or the key being refused.
    std::function<void(const std::string &error, int retryDelaySeconds, bool quota, bool badKey)> failed;
  };

  // `config.resumeHandle` (from resumeHandle()) carries on an earlier session's conversation.
  bool begin(const std::string &apiKey, const LiveConfig &config, Callbacks callbacks);
  void startTurn();
  void sendAudio(const int16_t *pcm, size_t frames);
  void endTurn();
  void sendText(const std::string &text);  // a typed turn (developer aid)
  void poll();
  void close();
  bool ready() const { return ready_ && ws_.connected(); }
  bool connected() const { return ws_.connected(); }
  bool inTurn() const { return turns_.turn; }
  uint32_t lastMessageAt() const { return lastMessageAt_; }  // millis(): silence = a dead link
  // The handle to resume this conversation with, once the server asked to move (goAway).
  const std::string &resumeHandle() const { return goAway_ ? config_.resumeHandle : kNone; }

 private:
  bool open();
  void onMessage(const std::string &message);

  WsClient ws_;
  std::string key_;
  LiveConfig config_;
  Callbacks cb_;
  bool ready_ = false;
  TurnTracker turns_;
  bool goAway_ = false;  // close once the reply is over (the next connect resumes)
  uint32_t lastMessageAt_ = 0;
  static inline const std::string kNone;
};
