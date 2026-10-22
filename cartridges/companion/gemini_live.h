#pragma once

#include <Arduino.h>

#include <functional>
#include <string>

#include "live_protocol.h"
#include "ws_client.h"

// One conversation with Gemini Live over the WebSocket: connect + setup, push-to-talk turns,
// the reply's audio and text as they arrive, set_mood calls answered at once, token usage, and
// reconnecting with the session-resumption handle when the server says goAway (~10 min) or the
// connection drops between turns. Knows nothing about the screen or the audio hardware.
class GeminiLive {
 public:
  struct Callbacks {
    std::function<void()> ready;                                // setup done: a turn can start
    std::function<void(const int16_t *pcm, size_t frames)> audio;  // reply audio, 24 kHz mono
    std::function<void(const std::string &text)> heard;         // the user's words (pieces)
    std::function<void(const std::string &text)> said;          // the reply's words (pieces)
    std::function<void(const std::string &mood)> mood;          // set_mood
    std::function<void(int tokens)> turnDone;                   // the reply ended
    // The connection failed or closed: why, a retry delay if the server gave one (-1), and
    // whether it was the free allowance running out or the key being refused.
    std::function<void(const std::string &error, int retryDelaySeconds, bool quota, bool badKey)> failed;
  };

  bool begin(const std::string &apiKey, const LiveConfig &config, Callbacks callbacks);
  void startTurn();
  void sendAudio(const int16_t *pcm, size_t frames);
  void endTurn();
  void sendText(const std::string &text);  // a typed turn (developer aid)
  void poll();
  void close();
  bool ready() const { return ready_ && ws_.connected(); }
  bool connected() const { return ws_.connected(); }
  bool inTurn() const { return turn_; }

 private:
  bool open();
  void onMessage(const std::string &message);

  WsClient ws_;
  std::string key_;
  LiveConfig config_;
  Callbacks cb_;
  bool ready_ = false;
  bool turn_ = false;       // from startTurn (or sendText) until the reply has ended
  bool speech_ = false;     // the current reply has brought audio or text
  bool goAway_ = false;     // reconnect once the turn is over
  int tokens_ = 0;          // this turn's tokens (the tool-only turn and the speech turn)
};
