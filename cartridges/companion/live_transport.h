#pragma once

#include <Arduino.h>

#include <functional>
#include <string>

#include "live_protocol.h"
#include "ws_client.h"

// How GeminiLive reaches Gemini: its own WebSocket over Wi-Fi (WsTransport), or the iPhone over
// Bluetooth (relay::Transport, relay_link.h). Both carry the same Live API JSON; the relay sends the
// voice as ADPCM and hands the reply audio over decoded, in arrival order with the JSON.
class LiveTransport {
 public:
  virtual ~LiveTransport() = default;
  // Opens the way to Gemini and sends the setup (the key: in the WebSocket's path; the relay names
  // it by its last 4 characters).
  virtual bool open(const std::string &apiKey, const std::string &setupJson, const std::string &resume, String &error) = 0;
  virtual bool sendText(const std::string &json) = 0;
  virtual bool sendVoice(const int16_t *pcm, size_t frames) = 0;
  virtual void flushVoice() {}  // the end of a turn's voice (the relay sends its last partial frame)
  // Messages in arrival order: Gemini's JSON (audio inline or not) and decoded reply audio. false =
  // the other end closed (closeCode/closeReason say why); true = still up, or closed by a handler.
  virtual bool poll(const std::function<void(const std::string &)> &onJson,
                    const std::function<void(const int16_t *, size_t)> &onAudio) = 0;
  virtual void close() = 0;
  virtual bool connected() const = 0;
  virtual uint16_t closeCode() const = 0;
  virtual const std::string &closeReason() const = 0;
};

class WsTransport : public LiveTransport {
 public:
  bool open(const std::string &apiKey, const std::string &setupJson, const std::string &, String &error) override {
    const std::string path =
        std::string("/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=") + apiKey;
    if (!ws_.connect("generativelanguage.googleapis.com", path.c_str(), error)) return false;
    return ws_.sendText(setupJson);
  }
  bool sendText(const std::string &json) override { return ws_.sendText(json); }
  bool sendVoice(const int16_t *pcm, size_t frames) override { return ws_.sendText(liveAudio(pcm, frames)); }
  bool poll(const std::function<void(const std::string &)> &onJson,
            const std::function<void(const int16_t *, size_t)> &) override {
    return ws_.poll(onJson);
  }
  void close() override { ws_.close(); }
  bool connected() const override { return ws_.connected(); }
  uint16_t closeCode() const override { return ws_.closeCode(); }
  const std::string &closeReason() const override { return ws_.closeReason(); }

 private:
  WsClient ws_;
};
