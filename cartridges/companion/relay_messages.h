#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// The relay through the iPhone (spec: docs/superpowers/specs/2026-10-23-companion-relay-design.md):
// the phone's JSON arrives on the Data characteristic in pieces (0x02, flags bit 0 = more follows,
// then bytes); Dotty's events go out as JSON on the Event characteristic. Plain C++ (host-tested).

constexpr size_t kRelayMaxMessage = 96 * 1024;  // a recall's 60 KB of chats plus room
constexpr size_t kRelayMaxVoiceSamples = 570;

// Samples per voice frame so the whole notification (0x1F 0x01 seq, the 3-byte ADPCM header, two
// samples a byte) fits MTU - 3: 570 made 291 bytes at the iPhone's MTU 293, and the stack silently cut
// the last byte of every frame.
size_t relayVoiceSamples(size_t mtu);

class PieceAssembler {
 public:
  // One 0x02 value. true when it completes a message (in `complete`); a message past the limit is
  // dropped and the next one starts clean.
  bool add(const uint8_t *data, size_t len, std::string &complete);
  void reset();
  size_t size() const { return buffer_.size(); }

 private:
  std::string buffer_;
  bool overflow_ = false;
};

std::string relayOpenEvent(const std::string &setupJson, const std::string &keyHint, const std::string &resume);
std::string relaySendEvent(const std::string &message);
std::string relayFetchEvent(uint32_t id, const std::string &model, const std::string &body);
std::string relayCloseEvent();

struct RelayIncoming {
  enum Kind { Gemini, Fetched, Closed, Bad } kind = Bad;
  std::string gemini;  // the message as it came (Gemini's, without its audio)
  uint32_t id = 0;     // fetched
  int status = 0;
  std::string body;
  int code = 0;        // closed
  std::string reason;
};
RelayIncoming parseRelayIncoming(const std::string &json);
