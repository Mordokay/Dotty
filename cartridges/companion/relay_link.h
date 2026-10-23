#pragma once

#include <Arduino.h>

#include <string>

#include "live_transport.h"

// The relay through the iPhone on Dotty's side (spec: docs/superpowers/specs/2026-10-23-companion-
// relay-design.md). The phone sends relay.hello {version, keys} when it connects; then a chat can go
// through it: Dotty's Live API messages as relay.send events, the voice as ADPCM frames (0x1F 0x01
// seq + frame), and back on the Data characteristic Gemini's messages without audio (0x02 pieces) and
// the reply audio (0x01 seq + frame), in order. Text-model calls for the writer / recall / macros go
// as relay.fetch. Data arrives on the BLE task into a PSRAM message buffer; everything else runs on
// the loop.
namespace relay {

constexpr int kVersion = 1;
// The text model was slow on 2026-10-23: 22-24 s through the phone and 5-16 s from a Mac (2-3 s
// that morning); 30 s cut a bedtime story off (the fallback is a short improvised reply).
constexpr uint32_t kFetchTimeoutMs = 60000;

void begin();  // the buffer, the Data handler, the relay.hello command
// Drains what the phone sent while no relay session is reading it (fetch results kept, the rest
// dropped); notices the phone going away. Call every loop pass.
void poll();
bool phoneReady();  // connected, and it said hello with our version
bool phoneHasKey(const String &keyHint);

// A text-model call through the phone (generateContent for `model`); the id to collect it with.
uint32_t fetch(const std::string &model, const std::string &body);
// true once the answer is in (or kFetchTimeoutMs passed: status 0, body "timed out").
bool takeFetched(uint32_t id, int &status, std::string &body);

class Transport : public LiveTransport {
 public:
  bool open(const std::string &apiKey, const std::string &setupJson, const std::string &resume, String &error) override;
  bool sendText(const std::string &json) override;
  bool sendVoice(const int16_t *pcm, size_t frames) override;
  void flushVoice() override;
  bool poll(const std::function<void(const std::string &)> &onJson,
            const std::function<void(const int16_t *, size_t)> &onAudio) override;
  void close() override;
  bool connected() const override;
  uint16_t closeCode() const override { return closeCode_; }
  const std::string &closeReason() const override { return closeReason_; }

 private:
  bool open_ = false;
  uint16_t closeCode_ = 0;
  std::string closeReason_;
};

}  // namespace relay
